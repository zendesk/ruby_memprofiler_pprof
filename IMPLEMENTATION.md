# Implementation notes for ruby_memprofiler_pprof

This document contains some notes about how the inside of the `ruby_memprofiler_pprof` (RMP) gem is implemented.

## Concepts

The core idea of RMP is to trap every object allocation and free. When an object is allocated, RMP collects details of what was allocated as well as a full Ruby backtrace of what code led to the allocation. This is saved in two places; in a singly linked list of allocation samples, and in a hashtable of heap samples. When the object is freed, the sample is removed from the heap sample map (but NOT from the allocation sample list).

When `#flush` is called on the collector, that creates the pprof data, containing one set of measurements for what was in the allocation sample list (that's `allocations`/`allocations_size`), and another set of measurements for what's in the heap sample map (that's `retained_objects`/`retained_size`). Once the pprof data is generated, the allocation sample list is cleared.

Thus, in each profile, you see what codepaths allocated memory since the last profile flush, and what codepaths allocated memory (since profiling started) that's still alive at the time the profile was generated.

## C extension implementation

RMP is mostly implemented as a C extension (under `ext/ruby_memprofiler_pprof`), because what it does involves calling undocumented APIs not exposed to Ruby code. When installing the gem, the C extension is compiled, and when executing `require "ruby_memprofiler_pprof"`, the compiled shared object is loaded into your program.

Ruby has a [documented(-ish)](https://ruby-doc.org/core-3.1.0/doc/extension_rdoc.html) [C API](https://silverhammermba.github.io/emberb/c/), which exposes some safe-ish operations to C extensions that remains _reasonably_ stable between Ruby versions. These are the functions that are exposed in the [ruby.h](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby.h) C header file. However, some things this gem needs to do are not possible within the exposed API.

To cheat, and get access to internal Ruby functions and structure definitions, RMP uses the [debase-ruby_core_source](https://github.com/os97673/debase-ruby_core_source) gem to pull in a copy of the private, internal C header files. This gives us access to function prototypes and structure definitions that would otherwise be inaccessible.

It is the intention of this project, once it has gotten some real world usage and stabilised, to propose new APIs to Ruby upstream that could remove the need for this kind of hackery. For now, though, doing these tricks lets us get real world experience in real world apps using current mainline Rubies.

## Tracepoint API usage

In order to trap and collect samples on object creation/deletion, we use Ruby's tracepoint API. The tracepoint API organises for the Ruby interpreter to call back our code when certain events happen, like function calls and returns, for example. This is available to [Ruby code](https://ruby-doc.org/core-3.1.0/TracePoint.html) and also [C code](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby/debug.h#L380) through the `rb_tracepoint_new` & `rb_tracepoint_enable` functions. However, the documented list of events does _not_ include a notification when objects are created & destroyed.

There is, however, an undocumented pair of events for this: `RUBY_INTERNAL_EVENT_NEWOBJ` and `RUBY_INTERNAL_EVENT_FREEOBJ`. These events can be trapped from a C extension when using `rb_tracepoint_new` (but not from Ruby code). In fact, using these two tracepoints is exactly how the `objspace` [extension](https://github.com/ruby/ruby/blob/v3_1_2/ext/objspace/object_tracing.c) (which is built in to Ruby, and exposed to Ruby code [as a library](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html)) works.

So, in order to have Ruby call our extension C code when Ruby objects are created or destroyed, we use `rb_tracepoint_new(RUBY_INTERNAL_EVENT_NEWOBJ, ...)` and `rb_tracepoint_new(RUBY_INTERNAL_EVENT_FREEOBJ, ...)`.

## Ruby backtrace collection

Inside our newobj tracepoint handler, we want to collect a backtrace of what current Ruby code caused this object to be allocated. The `objspace` gem from the previous extension does something _like_ this; the [`#allocation_sourcefile`](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html#method-c-allocation_sourcefile) & [`#allocation_sourceline`](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html#method-c-allocation_sourceline) methods it provides gives the file and line number that caused an allocation to happen. This is implemented by calling the `rb_tracearg_path` & `rb_tracearg_lineno` methods [inside the tracepoint](https://github.com/ruby/ruby/blob/v3_1_2/ext/objspace/object_tracing.c#L79).

However, we want to do better than this; the single Ruby file & line number might usefully identify some codepaths, but very often that will simply point to some piece of shared code called from many different places; that's not enough to tell you what your application was doing to cause the allocation. This is why we want a full backtrace instead; the complete sequence of calls that lead to a particular allocation happening.

To get a backtrace of the Ruby code from our tracepoint handler, we have a few options.

### `Thread#backtrace_locations`

In Ruby code, it's possible to get a backtrace by calling [`Thread.current.backtrace_locations`](https://ruby-doc.org/core-3.1.0/Thread.html#method-i-backtrace_locations). This returns an array of [`Thread::Backtrace::Location`](https://ruby-doc.org/core-3.1.0/Thread/Backtrace/Location.html) structures, which gives us goodies like the filename, line number, and method name. This is exactly the information we're after to put in our profile!

Unfortunately, it's tremendously, tremendously slow. This allocates a whole bunch of Ruby objects to hold the various parts of the backtrace, and each of those must call _back into_ our newobj tracepoint. At a 100% sampling rate, this would never terminate; at even very low sampling rates (e.g. 1% or so) it still seems to add a lot of overhead. There's a benchmark script in the repo to demonstrate how slow this is - see the end of this section for some benchmark results!

### `rb_make_backtrace()`

Ruby has a function [`rb_make_backtrace`](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby/internal/intern/vm.h#L431) exported in the C API which returns a Ruby array of Ruby strings of the form `"file:lineno:in 'method'"`. This is not ideal for our purposes, because it means we have to do string manipulation/integer parsing to split that up. More importantly, however, its implemented internally in an identical way to `Thread#backtrace_locations`, so it's just as slow. 

### `rb_profile_frames()`

I didn't actually find [`rb_profile_frames`](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby/debug.h#L51) when doing the initial implementation of this gem (I believe the doc comment there is actually new, although the function goes back to at least 2.6). It actually looks promising, and I'm going to try it out at some point.

### Manually walking CFP frames

If we want to avoid allocating Ruby objects, it seemed the best way to parse its data structures in C code rather than using any of the provided APIs which deal in VALUE's (the C type representing Ruby objects). I implemented this by more-or-less copying wholesale the implementation of [`backtrace_each()`](https://github.com/ruby/ruby/blob/v3_1_2/vm_backtrace.c#L855) out of the Ruby source code. In order to make this actually compile, we need definitions from some internal Ruby headers (specifically, the layout of the `rb_control_frame_t` structure, and the implementation of the `RUBY_VM_*_CONTROL_FRAME` family of macros, plus a few others). These definitions are of course normally inaccessible to C extensions, but in our case are provided by the `debase-ruby_core_source` gem.

The resulting code (`mpp_rb_backtrace_capture` in [`backtrace.c`](ext/ruby_memprofiler_pprof/backtrace.c)) walks Ruby's internal control frame structure directly from C, producing a structure used inside the extension as an internal representation of a backtrace.

### Benchmarks

To measure the performance of these backtrace capturing methods, there's a benchmark in [`scripts/benchmark.rb`](scripts/benchmark.rb). I also added a method to the profiler, `RubyMemprofilerPprof::Collector#bt_method=`, which can be used to select a backtrace generation implementation. On my laptop, the results of the benchmark are:

```
$ bundle exec ruby script/benchmark.rb
                                                         user     system      total        real
no profiling (1)                                    19.554166   0.455122  20.009288 ( 20.131883)
no profiling (2)                                    18.334558   0.433204  18.767762 ( 18.834532)
with profiling (10%, CFP walking)                   20.376882   0.537873  20.914755 ( 21.503034)
with reporting (10%, CFP walking)                   19.846640   0.498443  20.345083 ( 20.430364)
with profiling (10%, Thread#backtrace_location)     35.042741   0.498857  35.541598 ( 35.817300)
```

The CFP walking method adds minimal overhead to the benchmark execution (around 5% tops, depending on what you compare), whereas the `Thread#backtrace_location`-based method adds almost 75% to the execution time.

Thus, the CFP-walking with `debase-ruby_core_source` headers is the default backtrace generation method in use.

## Keeping track of object liveness

In theory, what we need to do to generate profiles of retained, unfreed objects is fairly simple; when our newobj tracepoint hook is called, add the object into a hashmap (keyed by its VALUE), and when the freeobj hook is called, remove that VALUE from the hashmap. Unfortunately, it's not _quite_ that simple.

Firstly, there's the problem of sampling. As mentioned in the main documentation, RMP is designed to work by sampling N% of allocations, and building up a holistic picture of memory usage by combining profiles from multiple instances of your application. Ideally, we would simply skip over the newobj or freeobj tracepoint (100 - N)% of the time without doing any work at all; however, we always need to _look_ in the live object hashmap for the object in our freeobj hook, because we don't magically know _which_ N% of allocations made their way into that map.

More concerningly, there are circumstances where Ruby calls newobj for an object, without ever calling freeobj on it at all! This seems to happen most often, that I've seen, for objects of time `T_IMEMO` (which represent pieces of Ruby code, as far as I can tell). There are a couple of tricky parts of handling this.

Firstly, our newobj hook could be called twice for the same VALUE, without a freeobj for it in the interim. We can detect this by checking to see in our newobj hook whether or not the VALUE is already in the live object map. If so, treat that as a free of that VALUE first, and _then_ follow the new-object path.

Secondly, at any given point, we have to treat any VALUE as if it might have become invalid while we weren't looking. We can't assume that if freeobj hasn't removed an object from the map, that it's actually still valid. In practice, this turns into a bunch of checks to see if VALUEs are T_NONE before touching them (it seems, when Ruby does internally "free" an object without calling our freeobj hook, it zeros the objects type out to T_NONE).

Finally, it means we also have to check that VALUES are not T_NONE whilst generating the pprof report; otherwise, we would be counting that object as still live when in fact it has been freed.

## Deferred size measurement

As well as recording the fact that an object was allocated, we'd like to record the size of the allocation too. Ruby provides a method for this, `rb_obj_memsize_of()`, which will tell you the total size of the memory backing a given VALUE - both the size it occupies in the [Ruby heap](https://jemma.dev/blog/gc-compaction), as well as any other malloc'd memory reported for the object, e.g. as reported by a C extension thorugh it's `.dsize` callback.

Unfortunately, we can't call `rb_obj_memsize_of` on an object in the newobj tracepoint, for two reasons:

* That would only return the size of the object on the Ruby heap (i.e. for most Ruby versions, 40 bytes). The off-heap storage for e.g. a large array or string won't have actually been allocated yet at this point of constructing the Array or String.
* On new Ruby versions >= 3.1 with variable-width allocations, `rb_obj_memsize_of` actually crashes when calling it on an object at this point of its lifecycle. I've observed this happening with T_CLASS objects, because their ivar tables (stored inside the variable-width part of the RVALUE) are not set up at this point. This is a similar problem to [this bug](https://bugs.ruby-lang.org/issues/18795).

The solution for both of these problems is the same. In the newobj tracepoint, instead of measuring the objects size, add it to a list of "pending size measurement" objects. Then, at some later point, go through the list and call `rb_obj_memsize_of` on them.

What should the "later point" be? For now, I settled on defining a creturn tracepoint hook to do this. Every time a ruby function implemented in C transferrs control back to Ruby, the creturn hook should fire. Since all object allocations need to happen in C (or, if I'm wrong about this, enough do that this tracepoint gets fired very often anyway), this should fire our creturn tracepoint "pretty soon" after the object has been created and hopefully before its size has changed too much from then!

It ultimately doesn't matter too much, I believe, because we _also_ call `rb_obj_memsize_of` on live objects again when constructing the retained object profile on flush, and it's the size of the objects that are not immediately freed that is probably of most use when profiling the memory usage of an application.

## UPB: C protobuf library

Constructing the pprof-formatted profile from the internal representation (i.e. what the `MemprofilerPprof::Collector#flush` method does) could take a decently long time (depending on how much data there is; I haven't really benchmarked this yet). It would be nice if we could release the Ruby GVL (global value lock) during this time, so that other threads could execute Ruby code while this is happening. In order to make this possible, that flush process can't call any Ruby methods (it's illegal to do so without holding the GVL).

(N.b. - I haven't actually _implemented_ releasing the GVL in `#flush`, but I wanted to make sure it was possible)

There are two things that the flush method does for which one would normally reach for a gem - serializing the profile data to protobuf, and compressing the profile with gzip (it's mandatory to do so according to the pprof file spec). Instead, we need to use C libraries to do these things, without calling back into Ruby.

To achieve the protobuf serialization entirely in C, RMP embeds a copy of the [UPB protobuf library](https://github.com/protocolbuffers/upb). This is supposed to be a simple-to-embed implementation of protobuf which forms the core of other language protobuf implementations. In any case, it's simple enough to embed it into our gem (see [`extconf.rb`](ext/ruby_memprofiler_pprof/extconf.rb) for details) and use it to serialise the pprof data.

The gzip serialisation is achieved by linking against zlib directly, which should be available on any system which has Ruby.

