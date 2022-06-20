# Implementation notes for ruby_memprofiler_pprof

This document contains some notes about how the inside of the `ruby_memprofiler_pprof` (RMP) gem is implemented.

## Concepts

The core idea of RMP is to trap every object allocation and free. When an object is allocated, RMP collects details of what was allocated as well as a full Ruby backtrace of what code led to the allocation. This is saved in a hashtable of heap samples. When the object is freed, the sample is removed from the heap sample map.

When `#flush` is called on the collector, that creates the pprof data, the collector walks all the (still-live) objects in the heap sample map and measures their size with `rb_obj_memsize_of`; it records both the number of live allocations (`retained_objects`) and their size (`retained_size`) and emits these into the pprof protobuf data.

Thus, in each profile, you see what codepaths allocated memory (since profiling started) that's still alive at the time the profile was generated.

## C extension implementation

RMP is mostly implemented as a C extension (under `ext/ruby_memprofiler_pprof`), because what it does involves calling undocumented APIs not exposed to Ruby code. When installing the gem, the C extension is compiled, and when executing `require "ruby_memprofiler_pprof"`, the compiled shared object is loaded into your program.

Ruby has a [documented(-ish)](https://ruby-doc.org/core-3.1.0/doc/extension_rdoc.html) [C API](https://silverhammermba.github.io/emberb/c/), which exposes some safe-ish operations to C extensions that remains _reasonably_ stable between Ruby versions. These are the functions that are exposed in the [ruby.h](https://github.com/ruby/ruby/blob/v3_1_2/include/ruby.h) C header file. However, some things this gem needs to do are not possible within the exposed API.

To cheat, and get access to internal Ruby functions and structure definitions, RMP uses the [debase-ruby_core_source](https://github.com/os97673/debase-ruby_core_source) gem to pull in a copy of the private, internal C header files. This gives us access to function prototypes and structure definitions that would otherwise be inaccessible.

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

Backtracie collects backtraces in two stages. First, it constructs a [`raw_location`](https://github.com/ivoanjo/backtracie/blob/5cb1db0f09829166460e5a2851f000c87d158ffa/ext/backtracie_native_extension/ruby_shards.h#L117) struct capturing the nescessary data (e.g. iseq (instruction sequence) or cme (callable method entry) pointers, and self objects). This is done by directly reaching into VM private data structures - either also using `debase-ruby_core_source`, or with the MJIT private header, depending on the Ruby version. Then, it converts this raw data into a string backtrace representation in a seprate step.

One of the main motivations for the Backtracie project is to produce backtraces with much richer data than using Ruby's standard backtrace APIs (for example, including class names). This is good for ruby_memprofiler_pprof, but on its own, this wouldn't solve our performance problems. 

We have (temporarily!) [forked Backtracie](https://github.com/KJTsanaktsidis/backtracie/tree/ktsanaktsidis/c_public_v2) to give it a C API which allows separating those two stages. In the `newobj` tracepoint handler, we do the "capturing" step, and keep only the `raw_location` structures around in the live sample map. Then, when producing the pprof profile during `#flush`, we do the second "stringifying" step. In this way, we only have to pay this cost for allocations which _actually_ live long enough to make it into a profile.

Our intention is to get that kind of API separation between collection/stringifying merged into Backtracie.

### Benchmarks

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

## Keeping track of object liveness

In theory, what we need to do to generate profiles of retained, unfreed objects is fairly simple; when our newobj tracepoint hook is called, add the object into a hashmap (keyed by its VALUE), and when the freeobj hook is called, remove that VALUE from the hashmap. Unfortunately, it's not _quite_ that simple.

### Sampling

Firstly, There's the problem of sampling. As mentioned in the main documentation, RMP is designed to work by sampling N% of allocations, and building up a holistic picture of memory usage by combining profiles from multiple instances of your application. Ideally, we would simply skip over the newobj or freeobj tracepoint (100 - N)% of the time without doing any work at all; however, we always need to _look_ in the live object hashmap for the object in our freeobj hook, because we don't magically know _which_ N% of allocations made their way into that map.

### Recursive hook non-execution

Secondly, Ruby refuses to run newobj/freeobj hooks re-entrantly. If an object is allocated inside a newobj hook, the newobj hook will NOT be called recursively on that object. If the newobj hook triggers a GC, and an object is therefore freed, the freeobj hook will NOT be called either.

This means that any object we allocate inside the newobj hook won't be included in heap samples (slightly annoying, but not a dealbreaker), but also that we might miss the fact that an object is freed if GC is triggered in the newobj hook (huge problem; trashes the accuracy of our data). GC can be triggered even in the absence of allocating any Ruby objects, too (`ruby_xmalloc` can trigger it).

So, to work around this, we disable GC during our newobj hook. The catch is, we can't quite do that by simply calling `rb_gc_disable`; that actually _completes_ any in-progress GC sweep before returning, which does exactly what we _don't_ want (causes objects to be freed whilst we're in a newobj tracepoint hook, thus causing their freeobj hooks to not fire). We hacked around this, for now, by directly flipping the `dont_gc` bit on the `rb_objspace` struct to temporarily disable (and then re-enable later) GC.

### rb_gc_force_recycle

In Ruby versions < 3.1, there's an API `rb_gc_force_recycle`, which directly marks an object as freed without going through machinery like running freeobj tracepoint hooks. This is used in the standard library in a few places, and results in a situation where our newobj hook could be called twice for the same VALUE, without a freeobj for it in the interim.

We can detect this by checking to see in our newobj hook whether or not the VALUE is already in the live object map. If so, treat that as a free of that VALUE first, and _then_ follow the new-object path. This is probably good enough, since most usages of `rb_gc_force_recycle` are to "free" otherwise very short lived objects, so it's very likely the VALUE will be re-used for another object pretty soon. So hopefully it won't distort the heap profiles too much.

## Deferred size measurement

As well as recording the fact that an object was allocated, we'd like to record the size of the allocation too. Ruby provides a method for this, `rb_obj_memsize_of()`, which will tell you the total size of the memory backing a given VALUE - both the size it occupies in the [Ruby heap](https://jemma.dev/blog/gc-compaction), as well as any other malloc'd memory reported for the object, e.g. as reported by a C extension thorugh it's `.dsize` callback.

Unfortunately, we can't call `rb_obj_memsize_of` on an object in the newobj tracepoint, for two reasons:

* That would only return the size of the object on the Ruby heap (i.e. for most Ruby versions, 40 bytes). The off-heap storage for e.g. a large array or string won't have actually been allocated yet at this point of constructing the Array or String.
* On new Ruby versions >= 3.1 with variable-width allocations, `rb_obj_memsize_of` actually crashes when calling it on an object at this point of its lifecycle. I've observed this happening with T_CLASS objects, because their ivar tables (stored inside the variable-width part of the RVALUE) are not set up at this point. This is a similar problem to [this bug](https://bugs.ruby-lang.org/issues/18795).

Thankfully, the "solution" here is pretty simple. We only need to record the size of the object whilst creating our profile pprof file anyway, so only measure it then. It actually turns into a bit of a non-issue, but I'm keeping the section here discussing the fact that `rb_obj_memsize_of` can't be used in a newobj tracepoint for documentation purposes.

## UPB: C protobuf library

Constructing the pprof-formatted profile from the internal representation (i.e. what the `MemprofilerPprof::Collector#flush` method does) could take a decently long time (depending on how much data there is; I haven't really benchmarked this yet). It would be nice if we could release the Ruby GVL (global value lock) during this time, so that other threads could execute Ruby code while this is happening. In order to make this possible, that flush process can't call any Ruby methods (it's illegal to do so without holding the GVL).

(N.b. - I haven't actually _implemented_ releasing the GVL in `#flush`, but I wanted to make sure it was possible)

There are two things that the flush method does for which one would normally reach for a gem - serializing the profile data to protobuf, and compressing the profile with gzip (it's mandatory to do so according to the pprof file spec). Instead, we need to use C libraries to do these things, without calling back into Ruby.

To achieve the protobuf serialization entirely in C, RMP embeds a copy of the [UPB protobuf library](https://github.com/protocolbuffers/upb). This is supposed to be a simple-to-embed implementation of protobuf which forms the core of other language protobuf implementations. In any case, it's simple enough to embed it into our gem (see [`extconf.rb`](ext/ruby_memprofiler_pprof/extconf.rb) for details) and use it to serialise the pprof data.

The gzip serialisation is achieved by linking against zlib directly, which should be available on any system which has Ruby.

