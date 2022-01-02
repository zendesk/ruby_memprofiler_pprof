#include <functional>
#include <iostream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <zlib.h>

#include "pprof.pb.h"
#include "ruby_memprofiler_pprof.h"

template<typename T>
struct intern_table {

    struct map_el_t {
        intern_table &tab;
        uint64_t ix;
        const T *held_obj;

        map_el_t(intern_table &tab, uint64_t ix) : tab(tab), ix(ix), held_obj(nullptr) {}
        map_el_t(intern_table &tab, const T *held_obj) : tab(tab), ix(0), held_obj(held_obj) {}

        const T &get_obj() const noexcept {
            if (held_obj) {
                return held_obj;
            } else {
                return tab.data[ix];
            }
        }

        bool operator<(const map_el_t &other) {
            return get_obj() < other.get_obj();
        }

        bool operator==(const map_el_t &other) {
            return get_obj() == other.get_obj();
        }
    };

    struct map_el_hash_t {
        std::size_t operator()(map_el_t const &el) const noexcept {
            return std::hash(el.get_obj());
        }
    };

    std::unordered_map<map_el_t, uint64_t, map_el_hash_t> obj_tab;
    std::vector<T> data;

    uint64_t intern(const T &obj) {
        map_el_t key_to_find(*this, obj);
        auto it = obj_tab.find(key_to_find);
        if (it != obj_tab.end()) {
            return it->second;
        }

        uint64_t new_ix = data.size();
        data.insert(obj);
        map_el_t key_to_insert(*this, new_ix);
        obj_tab.insert(key_to_insert);
        return new_ix;
    }
};

struct pprof_serialize_state {
    perftools::profiles::Profile prof_msg;
    std::string serialized_msg_no_compress;
    std::vector<char> serialized_msg_gzip;
    struct str_intern_tab_index *strtab_ix;
};

static struct pprof_serialize_state *rmmp_pprof_serialize_init_impl() {
    auto *st = new pprof_serialize_state();
    return st;
}

struct pprof_serialize_state *rmmp_pprof_serialize_init() {
    try {
        return rmmp_pprof_serialize_init_impl();
    } catch (...) {
        return nullptr;
    }
}

static void rmmp_pprof_serialize_add_strtab_impl(struct pprof_serialize_state *state, struct str_intern_tab_index *strtab_ix) {
    state->prof_msg.mutable_string_table()->Reserve(static_cast<int>(strtab_ix->str_list_len));
    for (size_t i = 0; i < strtab_ix->str_list_len; i++) {
        auto el = strtab_ix->str_list[i];
        state->prof_msg.mutable_string_table()->Add(
            std::move(std::string(el->str, el->str_len))
        );
    }
    state->strtab_ix = strtab_ix;
}


void rmmp_pprof_serialize_add_strtab(struct pprof_serialize_state *state, struct str_intern_tab_index *strtab_ix) {
    try {
        rmmp_pprof_serialize_add_strtab_impl(state, strtab_ix);
    } catch (...) {

    }
}

static void rmmp_pprof_serialize_add_alloc_samples_impl(struct pprof_serialize_state *state, struct mpp_sample *sample_list) {
    struct mpp_sample *s = sample_list;
    std::unordered_map<uint64_t, perftools::profiles::Function> function_tab;
    std::unordered_map<uint64_t, perftools::profiles::Location> location_tab;

    while (s) {
        perftools::profiles::Sample sample_pb;
        sample_pb.mutable_location_id()->Reserve(static_cast<int>(s->bt.frames_count));

        // Using an ordinary for loop here can't work because we have to go backwards, and size_t is unsigned.
        if (s->bt.frames_count) {
            size_t i = s->bt.frames_count - 1;
            while (true) {
                struct mpp_rb_backtrace_frame frame = s->bt.frames[i];

                auto fnit = function_tab.find(frame.function_id);
                perftools::profiles::Function fn;
                if (fnit != function_tab.end()) {
                    fn = fnit->second;
                } else {
                    fn.set_id(frame.function_id);
                    fn.set_filename(mpp_strtab_index_of(state->strtab_ix, s->bt.frames[i].filename));
                    fn.set_name(mpp_strtab_index_of(state->strtab_ix, s->bt.frames[i].function_name));
                    fn.set_system_name(mpp_strtab_index_of(state->strtab_ix, s->bt.frames[i].function_name));
                    function_tab.insert({frame.function_id, fn});
                }

                auto locit = location_tab.find(frame.location_id);
                perftools::profiles::Location loc;
                if (locit != location_tab.end()) {
                    loc = locit->second;
                } else {
                    loc.set_id(frame.location_id);
                    perftools::profiles::Line ln;
                    ln.set_function_id(frame.function_id);
                    ln.set_line(static_cast<int64_t>(frame.line_number));
                    loc.mutable_line()->Add()->CopyFrom(ln);
                    location_tab.insert({frame.location_id, loc});
                }

                sample_pb.mutable_location_id()->Add(frame.location_id);

                if (i == 0) {
                    break;
                } else {
                    i--;
                }
            }
        }
        sample_pb.mutable_value()->Add(1);
        state->prof_msg.mutable_sample()->Add()->CopyFrom(sample_pb);
        s = s->next_alloc;
    }

    for (auto &it : location_tab) {
        state->prof_msg.mutable_location()->Add()->CopyFrom(it.second);
    }
    for (auto &it : function_tab) {
        state->prof_msg.mutable_function()->Add()->CopyFrom(it.second);
    }

    // TODO: This is completely wrong.
    int64_t str_allocations_ix = state->prof_msg.mutable_string_table()->size();
    state->prof_msg.mutable_string_table()->Add("allocations");
    int64_t str_count_ix = state->prof_msg.mutable_string_table()->size();
    state->prof_msg.mutable_string_table()->Add("count");
    perftools::profiles::ValueType allocations_vt;
    allocations_vt.set_type(str_allocations_ix);
    allocations_vt.set_unit(str_count_ix);
    state->prof_msg.mutable_sample_type()->Add()->CopyFrom(allocations_vt);
}

void rmmp_pprof_serialize_add_alloc_samples(struct pprof_serialize_state *state, struct mpp_sample *sample_list) {
    try {
        rmmp_pprof_serialize_add_alloc_samples_impl(state, sample_list);
    } catch (...) {

    }
}

static void rmmp_pprof_serialize_to_memory_impl(struct pprof_serialize_state *state, char **outbuf, size_t *outlen, int *abort_flag) {
    state->serialized_msg_no_compress = state->prof_msg.SerializeAsString();

    // Gzip it as per standard.
    z_stream strm;
    int r;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int windowBits = 15;
    int GZIP_ENCODING = 16;
    r = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits | GZIP_ENCODING, 8, Z_DEFAULT_STRATEGY);
    if (r != Z_OK) {
        // do.... something?
        return;
    }
    strm.avail_in = state->serialized_msg_no_compress.size();
    strm.next_in = reinterpret_cast<unsigned char *>(state->serialized_msg_no_compress.data());
    state->serialized_msg_gzip.resize(1024);
    strm.avail_out = state->serialized_msg_gzip.size();
    strm.next_out = reinterpret_cast<unsigned char *>(state->serialized_msg_gzip.data());
    while(true) {
        int flush = strm.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH;
        r = deflate(&strm, flush);
        if (r == Z_STREAM_END) {
            break;
        } else if (r != Z_OK) {
            // uh wat?
            std::cerr << "deflate error " << r << "; in " << strm.avail_in << ", out " << strm.avail_out << ";\n";
        }

        if (strm.avail_out == 0) {
            size_t orig_size = state->serialized_msg_gzip.size();
            state->serialized_msg_gzip.resize( orig_size + 1024);
            strm.avail_out = 1024;
            strm.next_out = reinterpret_cast<unsigned char *>(state->serialized_msg_gzip.data() + orig_size);
        }
    }
    // Trim out the remainder of the buffer.
    state->serialized_msg_gzip.resize(state->serialized_msg_gzip.size() - strm.avail_out);
    deflateEnd(&strm);

    *outbuf = state->serialized_msg_gzip.data();
    *outlen = state->serialized_msg_gzip.size();
}

void rmmp_pprof_serialize_to_memory(struct pprof_serialize_state *state, char **outbuf, size_t *outlen, int *abort_flag) {
    try {
        rmmp_pprof_serialize_to_memory_impl(state, outbuf, outlen, abort_flag);
    } catch (std::exception &ex) {
        std::cerr << "EXCEPTION: " << ex.what() << "\n";
    } catch (...) {
        std::cerr << "SOME OTHER EXCEPTION \n";
    }
}

static void rmmp_pprof_serialize_destroy_impl(struct pprof_serialize_state *state) {
    delete state;
}

void rmmp_pprof_serialize_destroy(struct pprof_serialize_state *state) {
    try {
        rmmp_pprof_serialize_destroy_impl(state);
    } catch (...) {

    }
}
