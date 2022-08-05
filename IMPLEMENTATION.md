# Implementation notes for ruby_memprofiler_pprof

This document contains some notes about how the inside of the `ruby_memprofiler_pprof` (RMP) gem is implemented.

## Concepts

The core idea of RMP is to trap every object allocation and free. When an object is allocated, RMP collects details of what was allocated as well as a full Ruby backtrace of what code led to the allocation. This is saved in a hashtable of heap samples. When the object is freed, the sample is removed from the heap sample map.

When `#flush` is called on the collector, that creates the pprof data, the collector walks all the (still-live) objects in the heap sample map and measures their size with `rb_obj_memsize_of`; it records both the number of live allocations (`retained_objects`) and their size (`retained_size`) and emits these into the pprof protobuf data.

Thus, in each profile, you see what codepaths allocated memory (since profiling started) that's still alive at the time the profile was generated.

## C extension implementation

RMP is mostly implemented as a C extension (under `ext/ruby_memprofiler_pprof_ext`), because what it does involves calling undocumented APIs not exposed to Ruby code. When installing the gem, the C extension is compiled, and when executing `require "ruby_memprofiler_pprof"`, the compiled shared object is loaded into your program.

Ruby has a [documented(-ish)](https://ruby-doc.org/core-3.1.0/doc/extension_rdoc.html) [C API](https://silverhammermba.github.io/emberb/c/), which exposes some safe-ish operations to C extensions that remains _reasonably_ stable between Ruby versions. These are the functions that are exposed in the [ruby.h](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby.h) C header file. However, some things this gem needs to do are not possible within the exposed API.

To cheat, and get access to internal Ruby functions and structure definitions, RMP uses two tricks. First, it uses the internal MJIT header; this is an internal header file that Ruby ships with in order to allow code that is compiled with the MJIT just-in-time compiler to call back into the VM to do certain things. This is incoluded by requiring `ruby_mjit_min_header-#{RUBY_VERSION}.h"`.

However, there are still some internal functions that are not exported in the MJIT header that we need access to - in particular, an implementation of the following two functions:

  * `is_pointer_to_heap`, from `gc.c`, which says whether or not a given pointer points into the Ruby heap and so can be considered a `VALUE`.
  * `rb_gc_disable_no_rest`, also from `gc.c`, which disables the garbage collector temporarily _without finishing the current collection_.

To get access to these, we copy-paste rather large swathes of Ruby's internal structures in the [`ruby_private/`](ext/ruby_memprofiler_pprof_ext/ruby_private/) directory for each version of Ruby we support; then, we implement copies of the above two methods which use those internal structures in [`ruby_hacks.c`](ext/ruby_memprofiler_pprof_ext/ruby_hacks.c)

It is the intention of this project, once it has gotten some real world usage and stabilised, to propose new APIs to Ruby upstream that could remove the need for this kind of hackery. For now, though, doing these tricks lets us get real world experience in real world apps using current mainline Rubies.

## Tracepoint API usage

In order to trap and collect samples on object creation/deletion, we use Ruby's tracepoint API. The tracepoint API organises for the Ruby interpreter to call back our code when certain events happen, like function calls and returns, for example. This is available to [Ruby code](https://ruby-doc.org/core-3.1.0/TracePoint.html) and also [C code](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby/debug.h#L380) through the `rb_tracepoint_new` & `rb_tracepoint_enable` functions. However, the documented list of events does _not_ include a notification when objects are created & destroyed.

There is, however, an undocumented pair of events for this: `RUBY_INTERNAL_EVENT_NEWOBJ` and `RUBY_INTERNAL_EVENT_FREEOBJ`. These events can be trapped from a C extension when using `rb_tracepoint_new` (but not from Ruby code). In fact, using these two tracepoints is exactly how the `objspace` [extension](https://github.com/ruby/ruby/blob/v3_1_2/ext/objspace/object_tracing.c) (which is built in to Ruby, and exposed to Ruby code [as a library](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html)) works.

So, in order to have Ruby call our extension C code when Ruby objects are created or destroyed, we use `rb_tracepoint_new(RUBY_INTERNAL_EVENT_NEWOBJ, ...)` and `rb_tracepoint_new(RUBY_INTERNAL_EVENT_FREEOBJ, ...)`.

## Ruby backtrace collection (backtracie)

Inside our newobj tracepoint handler, we want to collect a backtrace of what current Ruby code caused this object to be allocated. The `objspace` gem from the previous extension does something _like_ this; the [`#allocation_sourcefile`](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html#method-c-allocation_sourcefile) & [`#allocation_sourceline`](https://ruby-doc.org/stdlib-3.1.0/libdoc/objspace/rdoc/ObjectSpace.html#method-c-allocation_sourceline) methods it provides gives the file and line number that caused an allocation to happen. This is implemented by calling the `rb_tracearg_path` & `rb_tracearg_lineno` methods [inside the tracepoint](https://github.com/ruby/ruby/blob/v3_1_2/ext/objspace/object_tracing.c#L79).

However, we want to do better than this; the single Ruby file & line number might usefully identify some codepaths, but very often that will simply point to some piece of shared code called from many different places; that's not enough to tell you what your application was doing to cause the allocation. This is why we want a full backtrace instead; the complete sequence of calls that lead to a particular allocation happening.

In Ruby, it's possible to get a backtrace by calling [`Thread.current.backtrace_locations`](https://ruby-doc.org/core-3.1.0/Thread.html#method-i-backtrace_locations). This returns an array of [`Thread::Backtrace::Location`](https://ruby-doc.org/core-3.1.0/Thread/Backtrace/Location.html) structures, which gives us goodies like the filename, line number, and method name. This is exactly the information we're after to put in our profile! 

Unfortunately, it's tremendously, tremendously slow - mostly because it must allocate a whole bunch of Ruby strings to hold various parts of the backtrace. Furthermore, most allocations in Ruby are freed soon after, and most likely before `#flush` is called to emit a pprof profile. So, we go to all this effort to stringify a backtrace, only to throw it out without it ever appearing in a profile.

To solve these problems, we instead generate backtraces using the [Backtracie gem](https://github.com/ivoanjo/backtracie/).

Backtracie is a rather special gem - as well as exposing a Ruby API that can be called from Ruby code, it _also_ exposes a low-level C API which allows other C extensions to call directly into its code. In this way, we can avoid the overhead of constantly calling into and out of Ruby to call methods from Backtracie. This C API is defined in the header [`public/backtracie.h`](https://github.com/ivoanjo/backtracie/blob/main/ext/backtracie_native_extension/public/backtracie.h), which we vendored in this project to avoid tricky build-time dependencies.

Backtracie collects backtraces in two stages. Firstly, RMP calls [`backtracie_capture_frame_for_thread`](https://github.com/ivoanjo/backtracie/blob/main/ext/backtracie_native_extension/public/backtracie.h#L105), for each frame on the callstack, to capture a lightweight `raw_location` struct by directly reaching into VM private data structures. Then, for each frame, we stringify it into a stack-allocated C string by calling methods like `backtracie_frame_name_cstr` and `backtracie_frame_filename_cstr`. Importantly, these methods do _not_ do any Ruby string manipulation, nor do they do any memory allocation at all; this is essential for getting adequate performance, because calling back into the Ruby VM from within the tracepoint handler would be far too slow.

Additionally, as a nice bonus, Backtracie is capable of producing much _nicer_ backtraces than the default Ruby backtrace generation; where Ruby often just prints a method name, Backtracie can produce a fully-qualified method name including the class i.e. `Foo::Thing#the_method` instead of just `the_method`.

## Keeping track of object liveness

In theory, what we need to do to generate profiles of retained, unfreed objects is fairly simple; when our newobj tracepoint hook is called, add the object into a hashmap (keyed by its VALUE), and when the freeobj hook is called, remove that VALUE from the hashmap. Unfortunately, it's not _quite_ that simple.

### Sampling

Firstly, There's the problem of sampling. As mentioned in the main documentation, RMP is designed to work by sampling N% of allocations, and building up a holistic picture of memory usage by combining profiles from multiple instances of your application. Ideally, we would simply skip over the newobj or freeobj tracepoint (100 - N)% of the time without doing any work at all; however, we always need to _look_ in the live object hashmap for the object in our freeobj hook, because we don't magically know _which_ N% of allocations made their way into that map.

### Recursive hook non-execution

Secondly, Ruby refuses to run newobj/freeobj hooks re-entrantly. If an object is allocated inside a newobj hook, the newobj hook [will NOT be called recursively](https://github.com/ruby/ruby/blob/55c771c302f94f1d1d95bf41b42459b4d2d1c337/vm_trace.c#L401) on that object. If the newobj hook triggers a GC, and an object is therefore freed, the freeobj hook will NOT be called either.

This means that any object we allocate inside the newobj hook won't be included in heap samples (slightly annoying, but not a dealbreaker), but also that we might miss the fact that an object is freed if GC is triggered in the newobj hook (huge problem; trashes the accuracy of our data). GC can be triggered even in the absence of allocating any Ruby objects, too (`ruby_xmalloc` can trigger it).

So, to work around this, we disable GC during our newobj hook. The catch is, we can't quite do that by simply calling `rb_gc_disable`. That method actually [_completes_ any in-progress GC sweep](https://github.com/ruby/ruby/blob/87d8d25796df3865b5a0c9069c604e475a28027f/gc.c#L11461) before returning, which does exactly what we _don't_ want (it would cause objects to be freed whilst we're in a newobj tracepoint hook, thus causing their freeobj hooks to not fire. Ruby actually has a method `rb_gc_disable_no_rest` which does _not_ complete an in-progress GC sweep, however that method is not exposed to C extensions.

We hacked around this by copying the private `rb_objspace` structure definition (for each Ruby version we support) into `ruby_private/`, and re-implementing `rb_gc_disable_no_rest` ourselves. The method itself is quite trivial; it just needs to flip the [`dont_gc` bit](https://github.com/ruby/ruby/blob/87d8d25796df3865b5a0c9069c604e475a28027f/gc.c#L731) on the `rb_objspace` struct.


### rb_gc_force_recycle

In Ruby versions < 3.1, there's an API `rb_gc_force_recycle`, which directly marks an object as freed without going through machinery like running freeobj tracepoint hooks. This is used in the standard library in a few places, and results in a situation where our newobj hook could be called twice for the same VALUE, without a freeobj for it in the interim.

We can detect this by checking to see in our newobj hook whether or not the VALUE is already in the live object map. If so, treat that as a free of that VALUE first, and _then_ follow the new-object path. This is probably good enough, since most usages of `rb_gc_force_recycle` are to "free" otherwise very short lived objects, so it's very likely the VALUE will be re-used for another object pretty soon. So hopefully it won't distort the heap profiles too much.

## Deferred size measurement

As well as recording the fact that an object was allocated, we'd like to record the size of the allocation too. Ruby provides a method for this, `rb_obj_memsize_of()`, which will tell you the total size of the memory backing a given VALUE - both the size it occupies in the [Ruby heap](https://jemma.dev/blog/gc-compaction), as well as any other malloc'd memory reported for the object, e.g. as reported by a C extension thorugh it's `.dsize` callback.

Unfortunately, we can't call `rb_obj_memsize_of` on an object in the newobj tracepoint, for two reasons:

* That would only return the size of the object on the Ruby heap (i.e. for most Ruby versions, 40 bytes). The off-heap storage for e.g. a large array or string won't have actually been allocated yet at this point of constructing the Array or String.
* On new Ruby versions >= 3.1 with variable-width allocations, `rb_obj_memsize_of` actually crashes when calling it on an object at this point of its lifecycle. I've observed this happening with `T_CLASS` objects, because their ivar tables (stored inside the variable-width part of the RVALUE) are not set up at this point. This is a similar problem to [this bug](https://bugs.ruby-lang.org/issues/18795).

Thankfully, the "solution" here is pretty simple. We only need to record the size of the object whilst creating our profile pprof file anyway, so only measure it then. It actually turns into a bit of a non-issue, but I'm keeping the section here discussing the fact that `rb_obj_memsize_of` can't be used in a newobj tracepoint for documentation purposes.

## UPB: C protobuf library

The pprof format is a (gzipped) protocol buffers structured file; in order to produce one, RMP needs a library to do so. Whilst we _could_ use the Ruby protobuf library published by Google for this, that's not especially convenient to call from a C extension (plus, it would get in the way of releasing the GVL - see the next section).

Instead, RMP embeds a copy of the [UPB protobuf library](https://github.com/protocolbuffers/upb). This is supposed to be a simple-to-embed implementation of protobuf which forms the core of other language protobuf implementations. We embed it into our gem (see [`extconf.rb`](ext/ruby_memprofiler_pprof_ext/extconf.rb) for details) and use it to serialise the pprof data.

The gzip serialisation is achieved by linking against zlib directly, which should be available on any system which has Ruby.

## Releasing the GVL during flush

Periodically, in order to actually get any useful data _out_ of the profiler, the user needs to call `MemprofilerPprof::Collector#flush` to construct a pprof-formatted file containing details about all currently-live memory allocations. This operation is reasonably heavyweight; it needs to traverse the live-object map, measure the size of all the Ruby objects in it with `rb_obj_memsize_of`, construct a protobuf representation of all of this, serialise it, and compress it with gzip (that's actually a requirement of the pprof specification). If this was done whilst the RMP extension was still holding the GVL, that would translate to a long pause for the application, which is obviously undesirable.

To avoid this, RMP needs to do as much of the flush work as possible whilst not holding the GVL; whilst it _is_ holding the GVL, it needs to be very concious to not hold it for a long uninterrupted time, to avoid long application pauses. There are two kwargs parameters for `#flush` that control these two behaviours; `yield_gvl` and `proactively_yield_gvl`.

When `yield_gvl` is specified, RMP will perform the work of serialising the protobuf representation and gzipping it without holding the GVL, by simply calling `rb_thread_call_without_gvl`. Whilst in this state, RMP must be careful not to call _any_ Ruby APIs - this restriction is OK for this phase of the flushing, because the UPB protobuf library itself obviously does not deal with any Ruby APIs.

However, on its own, that's not enough. The process of iterating through the currently-live objects, measuring their size, and constructing the protobuf data that is to be serialised also takes quite a long time. This also, however, requires the GVL; otherwise, we might be iterating the live-objects hash whilst another thread is creating a new object and trying to append to the same hash (and, of course, `rb_obj_memsize_of` _also_ requires the GVL). We want to break up this work into chunks, and yield the GVL in between, so that this manifests as many shorter pauses rather than one very long pause. That is the purpose of the `proactively_yield_gvl` flag.

When this flag is set, RMP will, whilst traversing the live-object hash, periodically check to see if any _other_ thread is waiting for the GVL. There's no Ruby API for this, but we implemented a `mpp_is_someone_else_waiting_for_gvl` method which works by, again, peeking into the internal GVL data structure to find out. If that turns out to be the case, we call `rb_thread_schedule` to give up the GVL and run a different thread.

(You might ask, why not simply just call `rb_thread_schedule` unconditionally? We don't want to give up the CPU time of the application to any _other_ process if it turns out no other application threads want to run).

## String interning

When dealing with strings, like the names of methods or files, in the backtrace samples, RMP interns them into an internal string table. This is done for two reasons:

* Firstly, the pprof format requires that strings are interned; all of the string fields in [pprof.proto](proto/pprof.proto) like `name`, `file_name`, etc are actually of type `int64`, which should be an index into the array `string_table`. So no matter what we did internally, we would need to produce such a table when emitting the pprof file.
* Secondly, a lot of the strings in the profile are highly likely to be repeated a lot. There are only so many different filenames, method names, etc in a program; if we kept an independent copy of them for each sample in our live allocations map, we would quickly wind up using more memory than we're trying to trace!

For this reason, we intern strings for samples when we get them out of backtracie. The `collector_cdata` struct has a single string interning table, `struct mpp_strtab *string_table`. The string table is implemented in [strtab.c](ext/ruby_memprofiler_pprof_ext/strtab.c) and provides two main families of methods - methods to add a string to the table, like `mpp_strtab_intern`, and ones to remove them, like `mpp_strtab_release`.

The `mpp_strtab_intern` methods take a string, and if the string is not in the table, add it to the table. If the string is already in the table, its refcount is incremented. In both cases, a pointer to the internal string in the intern table is returned; this pointer can be compared to any other pointer returned from `mpp_strtab_intern` to know if it's the same string or not. The `mpp_strtab_release` methods do the opposite; they decrement the refcount of a string in the intern table, and if it was the last reference, free it.

## Benchmarks

There's still a pretty significance performance overhead, but I'm working on it!

```
$ bundle exec ruby script/benchmark.rb
                                           user     system      total        real
no profiling (1)                      15.844936   0.469273  16.314209 ( 16.337021)
no profiling (2)                      14.966771   0.034934  15.001705 ( 15.020942)
with profiling (1%, no flush)         15.971306   0.477258  16.448564 ( 16.471195)
with reporting (1%, with flush)       17.402173   0.029948  17.432121 ( 17.454925)
with profiling (10%, no flush)        23.168302   0.289576  23.457878 ( 23.487329)
with reporting (10%, with flush)      48.194583   0.042943  48.237526 ( 48.290087)
with profiling (100%, no flush)       24.493680   0.329478  24.823158 ( 24.856144)
with reporting (100%, with flush)     95.083065   0.099887  95.182952 ( 95.286911)
```


## Potential future improvements

* Use a flag on the VALUE to avoid a lookup in freeobj. G
