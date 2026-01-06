// Replace file with original simple implementation from 'original/UnrealOodleWrapper.cpp'
#define OODLE_ALLOW_DEPRECATED_COMPRESSORS

#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <cstdarg>
#include "include\oodle2.h"

// Environment-gated minimal job system for Oodle plugin instrumentation.
// Enable by setting environment variable UNREALOODLE_ENABLE_JOBSYS=1
// Optionally set UNREALOODLE_JOB_THREADS to an integer to control worker count.
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <future>

static unsigned long long parse_human_size(const char * s) {
    if (!s) return 0ULL;
    unsigned long long val = 0ULL;
    char * end = NULL;
    val = strtoull(s, &end, 10);
    if (end && *end) {
        char c = *end;
        if (c == 'k' || c == 'K') val *= 1024ULL;
        else if (c == 'm' || c == 'M') val *= 1024ULL * 1024ULL;
        else if (c == 'g' || c == 'G') val *= 1024ULL * 1024ULL * 1024ULL;
    }
    return val;
}

namespace {
    // Global flag to enable per-task instrumentation logging (env-gated)
    std::atomic<bool> g_jobInstrEnabled{false};
    // Gate ad-hoc debug prints (set env UNREALOODLE_DEBUG=1 to enable)
    std::atomic<bool> g_debugEnabled{false};
    static void debug_printf(const char * fmt, ...) {
        if (!g_debugEnabled.load()) return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    struct JobSystem {
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> stopping{false};

        std::atomic<size_t> tasks_executed{0};
        std::atomic<size_t> peak_queue{0};
        std::atomic<uint64_t> total_busy_ns{0};

        std::atomic<OO_U64> nextHandle{1};
        std::unordered_map<OO_U64, std::shared_future<void>> futures;

        void init(int threads) {
            stopping = false;
            tasks_executed = 0;
            peak_queue = 0;
            total_busy_ns = 0;
            workers.reserve(threads);
            for (int i = 0; i < threads; ++i) {
                try { workers.emplace_back([this]{ this->workerLoop(); }); }
                catch (...) { break; }
            }
        }

        void shutdown() {
            {
                std::unique_lock<std::mutex> lk(mu);
                stopping = true;
            }
            cv.notify_all();
            for (auto &t : workers) if (t.joinable()) t.join();
            workers.clear();
        }

        void workerLoop() {
            while (true) {
                std::function<void()> fn;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [this]{ return stopping || !tasks.empty(); });
                    if (stopping && tasks.empty()) return;
                    fn = std::move(tasks.front()); tasks.pop();
                }
                auto t0 = std::chrono::steady_clock::now();
                try { fn(); } catch (...) {}
                auto t1 = std::chrono::steady_clock::now();
                auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                total_busy_ns.fetch_add((uint64_t)d);
                tasks_executed.fetch_add(1);
            }
        }

        OO_U64 runJob(t_fp_Oodle_Job * fp_job, void * job_data, OO_U64 * dependencies, int num_dependencies, void * user_ptr) {
            auto taskPtr = std::make_shared<std::packaged_task<void()>>([fp_job, job_data]{ (*fp_job)(job_data); });
            auto fut = taskPtr->get_future().share();
            OO_U64 handle = nextHandle.fetch_add(1);
            {
                std::unique_lock<std::mutex> lk(mu);
                futures.emplace(handle, fut);
            }
            // copy dependency handles since the pointer passed in by Oodle may be short-lived
            std::vector<OO_U64> depsVec;
            if (dependencies && num_dependencies > 0) {
                depsVec.reserve(num_dependencies);
                for (int i = 0; i < num_dependencies; ++i) depsVec.push_back(dependencies[i]);
            }
            // capture handle and depsVec so we can log per-task timings when instrumentation is enabled
            auto wrapper = [this, taskPtr, deps = std::move(depsVec), handle]() {
                if (!deps.empty()) {
                    for (OO_U64 dep : deps) {
                        std::shared_future<void> df;
                        {
                            std::unique_lock<std::mutex> lk(mu);
                            auto it = futures.find(dep);
                            if (it != futures.end()) df = it->second;
                        }
                        if (df.valid()) { try { df.wait(); } catch(...) {} }
                    }
                }
                // optional per-task timing/logging
                bool instr = g_jobInstrEnabled.load();
                if (instr) {
                    auto t0 = std::chrono::steady_clock::now();
                    try { (*taskPtr)(); } catch(...) {}
                    auto t1 = std::chrono::steady_clock::now();
                    double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
                    fprintf(stderr, "[UnrealOodleWrapper] task handle=%llu duration_ms=%.3f\n", (unsigned long long)handle, ms);
                } else {
                    try { (*taskPtr)(); } catch(...) {}
                }
            };
            {
                std::unique_lock<std::mutex> lk(mu);
                tasks.push(wrapper);
                size_t q = tasks.size();
                size_t prev = peak_queue.load();
                while (q > prev && !peak_queue.compare_exchange_weak(prev, q)) {}
            }
            cv.notify_one();
            return handle;
        }

        void waitJob(OO_U64 handle, void * user_ptr) {
            if (handle == 0) return;
            std::shared_future<void> f;
            {
                std::unique_lock<std::mutex> lk(mu);
                auto it = futures.find(handle);
                if (it != futures.end()) f = it->second;
            }
            if (f.valid()) f.wait();
            {
                std::unique_lock<std::mutex> lk(mu);
                futures.erase(handle);
            }
        }

        int getWorkerCount() const { return (int)workers.size(); }
        size_t getTasksExecuted() const { return tasks_executed.load(); }
        size_t getPeakQueue() const { return peak_queue.load(); }
        uint64_t getTotalBusyNs() const { return total_busy_ns.load(); }
    } g_jobSystem;

    OO_U64 OODLE_CALLBACK job_run_wrapper(t_fp_Oodle_Job * fp_job, void * job_data, OO_U64 * dependencies, int num_dependencies, void * user_ptr) {
        return g_jobSystem.runJob(fp_job, job_data, dependencies, num_dependencies, user_ptr);
    }

    void OODLE_CALLBACK job_wait_wrapper(OO_U64 job_handle, void * user_ptr) {
        g_jobSystem.waitJob(job_handle, user_ptr);
    }

}

int printUsage() {
            std::cerr << ".exe [option] <input> <output>    (use '-' as shorthand for stdin/stdout)\n";
            std::cerr << "Option:\n";
            std::cerr << " -c <level> <method>    Compress. <level> = -4..9, <method> = -1..5\n";
            std::cerr << "     Optional flags:\n";
            std::cerr << "         --legacy : emit raw compressed bytes with no OOZ header (requires explicit decompressed size when decoding)\n";
            std::cerr << "         --ooz    : emit simple 8-byte original-size prefix (legacy compatibility)\n";
            std::cerr << "         --jobify <mode> : control internal parallel jobs (modes: default, disable, normal, aggressive)\n";
            std::cerr << "         --jobify-count <N> : target parallelism (0 = use all logical cores)\n";
            std::cerr << " -d <size>             Decompress. <size> may be omitted if compressed data contains an OOZ header.\n\n";
            std::cerr << "Notes on IO:\n";
            std::cerr << " Use '-' as shorthand to read from stdin or write to stdout. The previous legacy form 'stdin=N' is no longer required.\n\n";
            std::cerr << "Compression Levels:\n";
            std::cerr << " <0 = bias towards speed\n";
            std::cerr << "  0 = no compression (copy)\n";
            std::cerr << " >0 = bias towards compression ratio\n\n";
            std::cerr << "Compression Methods:\n";
            std::cerr << " -1 = LZB16 (DEPRECATED)\n";
            std::cerr << "  0 = None (pass-through)\n";
            std::cerr << "  1 = Kraken\n";
            std::cerr << "  2 = Leviathan\n";
            std::cerr << "  3 = Mermaid\n";
            std::cerr << "  4 = Selkie\n";
            std::cerr << "  5 = Hydra\n\n";
    return 1;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
        return printUsage();
    // Read env var to enable debug prints early (so stdin dump uses it)
    char* env_dbg = NULL; size_t dbg_len = 0;
    if (_dupenv_s(&env_dbg, &dbg_len, "UNREALOODLE_DEBUG") == 0 && env_dbg) {
        if (env_dbg[0] == '1' || env_dbg[0] == 't' || env_dbg[0] == 'T') g_debugEnabled.store(true);
        free(env_dbg);
    }
    long compression_level = 9;
    bool compress = false;
    char* p = 0;
    long decompress_size = 0;
    long int compression_method = 3;
    int addarg = 0;
    int inputIndex = 0;
    int outputIndex = 0;

    if (!strncmp(argv[1], "-c", 2)) {
        errno = 0;
        compress = true;
        compression_level = strtol(argv[2], &p, 10);
        if (errno != 0 || *p != '\0' || compression_level > 9 || compression_level < -4) {
            std::cerr << "Wrong compression level input!\n";
            return printUsage();
        }
        compression_method = strtol(argv[3], &p, 10);
        if (errno != 0 || *p != '\0' || compression_method > 5 || compression_method < -1) {
            std::cerr << "Wrong compression method input!\n";
            return printUsage();
        }
        switch (compression_method) {
            case -1:
                compression_method = 4;
                break;
            case 0:
                compression_method = 3;
                break;
            case 1:
                compression_method = 8;
                break;
            case 2:
                compression_method = 13;
                break;
            case 3:
                compression_method = 9;
                break;
            case 4:
                compression_method = 11;
                break;
            case 5:
                compression_method = 12;
        }
        addarg = 1;
    }
    else if (strncmp(argv[1], "-d", 2)) {
        std::cerr << "Wrong option!\n";
        return printUsage();
    }
    else {
        // support both forms: -d <size> <input> <output>  OR  -d <input> <output> (size omitted)
        if (argc != 5 && argc != 4) return printUsage();
        if (argc == 5) {
            errno = 0;
            decompress_size = strtol(argv[2], &p, 10);
            if (errno != 0 || *p != '\0' || decompress_size > INT_MAX || decompress_size < 1) {
                std::cerr << "Wrong decompress size input!\n";
                return printUsage();
            }
            inputIndex = 3; outputIndex = 4;
        } else {
            // size omitted
            decompress_size = 0;
            inputIndex = 2; outputIndex = 3;
        }
    }

    FILE* file = 0;
    size_t filesize = 0;
    void* buffer = 0;

    

    // support '-' shorthand or "stdin" (optionally "stdin=N" for fixed-size legacy)
    // determine input/output indices for compress mode
    if (compress) {
        inputIndex = 3 + addarg;
        outputIndex = 4 + addarg;
    }

    if ((argv[inputIndex][0] == '-' && argv[inputIndex][1] == 0) || !strncmp(argv[inputIndex], "stdin", 5)) {
        // If provided as "stdin=N" use the given size, otherwise stream until EOF
        if (argv[inputIndex][5] == '=') {
            filesize = strtol(argv[inputIndex] + 6, &p, 10);
            buffer = malloc(filesize);
            _setmode(_fileno(stdin), _O_BINARY);
            fread(buffer, filesize, 1, stdin);
        } else {
            // read all stdin into memory
            const size_t CHUNK = 1 << 16;
            unsigned char *buf = NULL;
            size_t cap = 0, len = 0;
            _setmode(_fileno(stdin), _O_BINARY);
            for (;;) {
                if (cap - len < CHUNK) {
                    size_t newcap = cap + CHUNK + (cap >> 1);
                    unsigned char *n = (unsigned char*)realloc(buf, newcap);
                    if (!n) { free(buf); std::cerr << "memory error\n"; return printUsage(); }
                    buf = n; cap = newcap;
                }
                size_t got = fread(buf + len, 1, CHUNK, stdin);
                if (got == 0) break;
                len += got;
            }
            filesize = (long)len;
            buffer = buf;
                // debug: dump first bytes read from stdin
                {
                    size_t dump_len = (filesize < 32) ? filesize : 32;
                    debug_printf("[DEBUG] stdin read %zu bytes, first %zu bytes: ", (size_t)filesize, dump_len);
                    for (size_t di = 0; di < dump_len; ++di) debug_printf("%02x", buf[di]);
                    debug_printf("\n");
                }
        }
    } else {
        fopen_s(&file, argv[inputIndex], "rb");
        fseek(file, 0, 2);
        filesize = ftell(file);
        fseek(file, 0, 0);
        buffer = malloc(filesize);
        fread(buffer, filesize, 1, file);
        fclose(file);
    }

    if (!compress) {
        // Support decompressing either raw Oodle payloads or OOZ container files produced by this tool.
        unsigned char * ubuf = (unsigned char *)buffer;
        size_t payload_offset = 0;
        size_t payload_size = filesize;
        // Detect OOZ header (SDK layout: magic(4), ver_flags(4), orig64(8), comp64(8))
        const uint32_t OOZ_MAGIC = 0x8C0A0000u;
        if (filesize >= 24) {
            uint32_t file_magic = 0;
            memcpy(&file_magic, ubuf + 0, sizeof(file_magic));
            if (file_magic == OOZ_MAGIC) {
                uint32_t ver_flags = 0;
                memcpy(&ver_flags, ubuf + 4, sizeof(ver_flags));
                unsigned long long orig64 = 0ULL;
                unsigned long long comp64 = 0ULL;
                memcpy(&orig64, ubuf + 8, sizeof(orig64));
                memcpy(&comp64, ubuf + 16, sizeof(comp64));

                size_t seek_mem_size = 0;
                // Only attempt to interpret an on-disk seek-table if the header indicates it AND
                // there are at least enough bytes present to hold the seek-table struct. This
                // avoids reading past the buffer when streaming from stdin where the seek-table
                // may not be present even if version/flags field has low bits set.
                if ((ver_flags & 1u) && filesize >= 24 + (size_t)sizeof(OodleLZ_SeekTable)) {
                    OodleLZ_SeekTable * pOnDiskSeek = (OodleLZ_SeekTable *)(ubuf + 24);
                    OO_S32 numSeek = pOnDiskSeek->numSeekChunks;
                    OodleLZSeekTable_Flags st_flags = OodleLZSeekTable_Flags_None;
                    OO_SINTa mem_needed = OodleLZ_GetSeekTableMemorySizeNeeded(numSeek, st_flags);
                    if (mem_needed > 0 && (size_t)mem_needed + 24 <= filesize) seek_mem_size = (size_t)mem_needed;
                }

                payload_offset = 24 + seek_mem_size;
                if (comp64 >= seek_mem_size) payload_size = (size_t)(comp64 - (unsigned long long)seek_mem_size);
                else payload_size = 0;
            }
        }

        // If not the full SDK OOZ header, also detect the simple compatibility prefix used by --ooz:
        // an 8-byte original-size prefix followed by compressed payload. Detect this even if the
        // decompress size was omitted by using a safe heuristic: the orig64 must be >0, <= a sane
        // maximum, and the orig64 should be >= (filesize - 8) since compressed size is usually <= orig.
        if (payload_offset == 0 && filesize >= 8) {
            unsigned long long maybe_orig = 0ULL;
            memcpy(&maybe_orig, ubuf + 0, sizeof(maybe_orig));
            const unsigned long long MAX_REASONABLE_ORIG = (1ULL << 34); // ~16GiB
            // Accept when orig64 seems reasonable. Allow the compressed payload to be slightly
            // larger than orig (e.g. incompressible data) by up to 64KiB to avoid false negatives.
            size_t comp_payload = (filesize > 8) ? (filesize - 8) : 0;
            bool comp_not_much_larger = (comp_payload <= maybe_orig) || (comp_payload <= maybe_orig + 65536ULL);
            if (maybe_orig > 0 && maybe_orig <= MAX_REASONABLE_ORIG && comp_not_much_larger) {
                // accept as compat prefix
                payload_offset = 8;
                payload_size = filesize - 8;
                // if user omitted decompress size, fill it in from header so later code can use it
                if (decompress_size == 0) {
                    if (maybe_orig <= (unsigned long long)INT_MAX) decompress_size = (long)maybe_orig;
                }
                debug_printf("[DEBUG] heuristically detected compat ooz prefix orig64=%llu -> payload_offset=%zu payload_size=%zu (decompress_size=%ld)\n",
                             maybe_orig, payload_offset, payload_size, decompress_size);
            }
        }

        void* new_buffer = malloc(decompress_size);
        debug_printf("[DEBUG] payload_offset=%zu payload_size=%zu filesize=%zu\n", payload_offset, payload_size, filesize);
        size_t dec_size = OodleLZ_Decompress(((unsigned char*)buffer) + payload_offset, payload_size, new_buffer, decompress_size, OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_Lots);
        debug_printf("[DEBUG] OodleLZ_Decompress returned %zu\n", dec_size);
        if (!dec_size) {
            std::cerr << "Error while decompressing. Invalid data or provided wrong decompression size!\n";
            return printUsage();
        }
        // support '-' shorthand for stdout as well
        if (((argv[outputIndex][0] == '-' && argv[outputIndex][1] == 0) || !strncmp(argv[outputIndex], "stdout", 6)) && new_buffer) {
            fflush(stdout);
            _setmode(_fileno(stdout), _O_BINARY);
            fwrite(new_buffer, decompress_size, 1, stdout);
        }
        else {
            FILE* newfile;
            fopen_s(&newfile, argv[outputIndex], "wb");
            if (newfile && new_buffer) {
                fwrite(new_buffer, decompress_size, 1, newfile);
                fclose(newfile);
            }
            free(new_buffer);
            fprintf(stderr, "Finished decompression. Compression ratio: %.2f%%\nDecompressed file size: %zd B\n", (float(filesize) / decompress_size) * 100, dec_size);
        }
        return 0;
    }
    else {
        size_t comp_buffer_size = OodleLZ_GetCompressedBufferSizeNeeded(OodleLZ_Compressor(compression_method), filesize);
        void* comp_buffer = malloc(comp_buffer_size);

        // CLI-controlled jobify and seek/chunk tuning
        // Defaults: jobify=aggressive, jobify-count=0 (auto), seek-points=16, seek-chunk-len=0 (auto)
        OodleLZ_Jobify jobify_mode = OodleLZ_Jobify_Aggressive;
        int jobify_count = 0;
        int seek_points = 16;
        OO_S32 seek_chunk_len_cli = 0;
        bool jobify_cli_provided = false;
        bool seek_points_cli_provided = false;
        bool seek_chunk_cli_provided = false;

        // Parse optional flags after required args (argv[6..])
        bool emit_compat_ooz = false;
        for (int ai = 6; ai < argc; ++ai) {
            if (strcmp(argv[ai], "--jobify") == 0 && ai + 1 < argc) {
                const char * m = argv[++ai];
                jobify_cli_provided = true;
                if (strcmp(m, "default") == 0) jobify_mode = OodleLZ_Jobify_Default;
                else if (strcmp(m, "disable") == 0) jobify_mode = OodleLZ_Jobify_Disable;
                else if (strcmp(m, "normal") == 0) jobify_mode = OodleLZ_Jobify_Normal;
                else if (strcmp(m, "aggressive") == 0) jobify_mode = OodleLZ_Jobify_Aggressive;
                else {
                    // numeric
                    long v = strtol(m, NULL, 10);
                    if (v >= 0 && v < (int)OodleLZ_Jobify_Count) jobify_mode = (OodleLZ_Jobify)v;
                }
            }
            else if (strcmp(argv[ai], "--jobify-count") == 0 && ai + 1 < argc) {
                jobify_count = (int)strtol(argv[++ai], NULL, 10);
            }
            else if (strcmp(argv[ai], "--seek-points") == 0 && ai + 1 < argc) {
                seek_points = (int)strtol(argv[++ai], NULL, 10);
                seek_points_cli_provided = true;
            }
            else if (strcmp(argv[ai], "--seek-chunk-len") == 0 && ai + 1 < argc) {
                unsigned long long v = parse_human_size(argv[++ai]);
                if (v > 0) {
                    seek_chunk_len_cli = (OO_S32)v;
                    seek_chunk_cli_provided = true;
                }
            }
            else if (strcmp(argv[ai], "--ooz") == 0) {
                // mark compatibility (simple 8-byte orig64 prefix) output
                emit_compat_ooz = true;
            }
            else {
                // unknown arg - ignore for now
            }
        }

        bool jobsys_enabled = false;
        int job_threads = 0;

        // If jobify mode is not disable, register local job system
        if (jobify_mode != OodleLZ_Jobify_Disable) {
            jobsys_enabled = true;
            // Determine job thread count.
            // If CLI explicitly provided --jobify-count, honor it:
            //   - if provided value == 0 => use full hardware concurrency (all logical cores)
            //   - if provided value > 0 => use that many threads
            // If CLI did not provide it, fall back to env UNREALOODLE_JOB_THREADS (if set),
            // otherwise default to (cores > 1 ? cores - 1 : 1) which preserves previous behavior.
            if (jobify_cli_provided) {
                if (jobify_count == 0) {
                    unsigned int cores = std::thread::hardware_concurrency();
                    job_threads = (int)(cores > 0 ? cores : 1);
                } else {
                    job_threads = jobify_count;
                }
            } else {
                char* env_threads = NULL;
                size_t thr_len = 0;
                if (_dupenv_s(&env_threads, &thr_len, "UNREALOODLE_JOB_THREADS") == 0 && env_threads) {
                    job_threads = (int)strtol(env_threads, NULL, 10);
                    free(env_threads);
                }
                if (job_threads <= 0) {
                    unsigned int cores = std::thread::hardware_concurrency();
                    job_threads = (int)((cores > 1) ? cores - 1 : 1);
                }
            }

            // optional per-task instrumentation flag via env
            char* env_instr = NULL;
            size_t instr_len = 0;
            if (_dupenv_s(&env_instr, &instr_len, "UNREALOODLE_JOB_INSTR") == 0 && env_instr) {
                if (env_instr[0] == '1' || env_instr[0] == 't' || env_instr[0] == 'T') {
                    g_jobInstrEnabled.store(true);
                }
                free(env_instr);
            }

            std::cerr << "[UnrealOodleWrapper] registering job system with target_parallelism=" << job_threads << "\n";
            g_jobSystem.init(job_threads);
            OodleCore_Plugins_SetJobSystemAndCount(job_run_wrapper, job_wait_wrapper, job_threads);
        }

        // Use compress options
        OodleLZ_CompressOptions opts = *OodleLZ_CompressOptions_GetDefault(OodleLZ_Compressor(compression_method), OodleLZ_CompressionLevel(compression_level));
        if (jobsys_enabled) {
            opts.jobify = jobify_mode;
            opts.seekChunkReset = 1;
            if (seek_chunk_cli_provided) {
                opts.seekChunkLen = seek_chunk_len_cli;
            } else {
                opts.seekChunkLen = OodleLZ_MakeSeekChunkLen((OO_S64)filesize, seek_points);
            }
        }

        auto comp_start = std::chrono::steady_clock::now();
        size_t com_size = OodleLZ_Compress(OodleLZ_Compressor(compression_method), buffer, filesize, comp_buffer, OodleLZ_CompressionLevel(compression_level), &opts, NULL, NULL, NULL, 0);
        if (!com_size) {
            std::cerr << "Error while compressing!\n";
            if (jobsys_enabled) g_jobSystem.shutdown();
            return 1;
        }
        /* emit_compat_ooz is set during optional-arg parsing above */

        // Prepare seek table buffer via Oodle SDK (do this before choosing stdout vs file output)
        OO_S32 seekChunkLen = opts.seekChunkLen;
        if (seekChunkLen == 0) {
            seekChunkLen = OodleLZ_MakeSeekChunkLen((OO_S64)filesize, 16);
        }
        OO_S32 numSeek = (OO_S32)((filesize + (size_t)seekChunkLen - 1) / (size_t)seekChunkLen);
        OodleLZSeekTable_Flags st_flags = OodleLZSeekTable_Flags_None;
        OO_SINTa seek_mem_needed = OodleLZ_GetSeekTableMemorySizeNeeded(numSeek, st_flags);
        void * seek_mem = NULL;
        if (seek_mem_needed > 0) {
            seek_mem = malloc((size_t)seek_mem_needed);
        }

        bool have_seek_table = false;
        if (seek_mem) {
            OodleLZ_SeekTable * pSeek = (OodleLZ_SeekTable *)seek_mem;
            OO_BOOL ok = OodleLZ_FillSeekTable(pSeek, st_flags, seekChunkLen, buffer, (OO_SINTa)filesize, comp_buffer, (OO_SINTa)com_size);
            if (ok) {
                have_seek_table = true;
            }
        }

        if ((argv[outputIndex][0] == '-' && argv[outputIndex][1] == 0) || !strncmp(argv[outputIndex], "stdout", 6)) {
            fflush(stdout);
            _setmode(_fileno(stdout), _O_BINARY);
            debug_printf("[DEBUG] writing %zu bytes to stdout (emit_compat_ooz=%d)\n", (size_t)com_size, emit_compat_ooz);
            if (emit_compat_ooz) {
                // Write simple 8-byte original-size prefix for compatibility
                unsigned long long orig64 = (unsigned long long)filesize;
                size_t w1 = fwrite(&orig64, sizeof(orig64), 1, stdout);
                debug_printf("[DEBUG] fwrite(orig64) returned %zu errno=%d\n", w1, errno);
                size_t w2 = fwrite(comp_buffer, com_size, 1, stdout);
                debug_printf("[DEBUG] fwrite(payload) returned %zu errno=%d\n", w2, errno);
                int ffl = fflush(stdout);
                debug_printf("[DEBUG] fflush(stdout)=%d errno=%d\n", ffl, errno);
            } else {
                // SDK-style header written to stdout
                unsigned long long orig64 = (unsigned long long)filesize;
                uint32_t magic = 0x8C0A0000u;
                uint32_t header_ver = 1u;
                uint32_t ver_flags = header_ver;
                unsigned long long comp64 = (unsigned long long)com_size;
                size_t wm;
                wm = fwrite(&magic, sizeof(magic), 1, stdout); debug_printf("[DEBUG] fwrite(magic)=%zu errno=%d\n", wm, errno);
                wm = fwrite(&ver_flags, sizeof(ver_flags), 1, stdout); debug_printf("[DEBUG] fwrite(ver_flags)=%zu errno=%d\n", wm, errno);
                wm = fwrite(&orig64, sizeof(orig64), 1, stdout); debug_printf("[DEBUG] fwrite(orig64)=%zu errno=%d\n", wm, errno);
                wm = fwrite(&comp64, sizeof(comp64), 1, stdout); debug_printf("[DEBUG] fwrite(comp64)=%zu errno=%d\n", wm, errno);
                wm = fwrite(comp_buffer, com_size, 1, stdout); debug_printf("[DEBUG] fwrite(payload)=%zu errno=%d\n", wm, errno);
                int ffl = fflush(stdout); debug_printf("[DEBUG] fflush(stdout)=%d errno=%d\n", ffl, errno);
            }
        }
        else {
            FILE* newfile;
            fopen_s(&newfile, argv[outputIndex], "wb");
            // Write either compatibility header (--ooz) or SDK OOZ header by default
            uint32_t magic = 0x8C0A0000u;
            uint32_t header_ver = 1u;
            // header flags: bit0 = seek table present
            uint32_t header_flags = 0u;
            unsigned long long orig64 = (unsigned long long)filesize;
            unsigned long long comp64 = (unsigned long long)com_size; // may be adjusted below to include seek-table bytes

            if (emit_compat_ooz) {
                // simple 8-byte little-endian original size prefix
                fwrite(&orig64, sizeof(orig64), 1, newfile);
                fwrite(comp_buffer, com_size, 1, newfile);
                fclose(newfile);
                free(comp_buffer);
                if (jobsys_enabled) g_jobSystem.shutdown();
                fprintf(stderr, "Finished compression. Compression ratio: %.2f%%\n", (float(com_size) / filesize) * 100);
                return 0;
            }

            // Use seek_mem prepared earlier (if any)
            size_t portable_seek_table_bytes = 0;
            if (have_seek_table) {
                OodleLZ_SeekTable * pSeekTmp = (OodleLZ_SeekTable *)seek_mem;
                portable_seek_table_bytes = sizeof(OO_U32) + sizeof(OO_U32) + (size_t)sizeof(unsigned long long) * (size_t)pSeekTmp->numSeekChunks;
                header_flags |= 1u; // indicate seek table present
            }

            // Write standard OOZ header (24 bytes): magic (4), version/flags (4), original size (8), compressed size (8)
            // Compose version/flags into a single 32-bit field as the SDK expects.
            uint32_t ver_flags = 0u;
            ver_flags = header_ver;
            if (have_seek_table) ver_flags |= 1u; // set seek-table-present bit (SDK expects flags in this field)

            // Adjust compressed-size to include the seek-table bytes we will write
            if (have_seek_table) {
                comp64 = (unsigned long long)com_size + (unsigned long long)seek_mem_needed;
            } else {
                comp64 = (unsigned long long)com_size;
            }

            // Write standard OOZ header (SDK order): magic (4), version/flags (4), original size (8), compressed size (8)
            // This matches the SDK sample's WriteOOZHeader layout.
            fwrite(&magic, sizeof(magic), 1, newfile);
            fwrite(&ver_flags, sizeof(ver_flags), 1, newfile);
            fwrite(&orig64, sizeof(orig64), 1, newfile);
            fwrite(&comp64, sizeof(comp64), 1, newfile);

            // If seek table present, write the SDK-filled seek-table memory blob immediately after the header.
            // The SDK's WriteOOZHeader serializes the seek table by writing the raw memory returned by
            // OodleLZ_GetSeekTableMemorySizeNeeded / OodleLZ_FillSeekTable. Emit that blob verbatim so
            // the on-disk layout exactly matches the SDK's expectations.
            if (have_seek_table) {
                // seek_mem_needed holds the byte-size we allocated earlier
                fwrite(seek_mem, (size_t)seek_mem_needed, 1, newfile);
            }

            // then the raw compressed payload
            fwrite(comp_buffer, com_size, 1, newfile);

            if (seek_mem) free(seek_mem);
            fclose(newfile);
            free(comp_buffer);
            if (jobsys_enabled) {
                auto comp_end = std::chrono::steady_clock::now();
                int workers = g_jobSystem.getWorkerCount();
                uint64_t busy_ns = g_jobSystem.getTotalBusyNs();
                size_t exec = g_jobSystem.getTasksExecuted();
                size_t peak = g_jobSystem.getPeakQueue();
                uint64_t wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(comp_end - comp_start).count();
                double busy_pct = 0.0;
                if (workers > 0 && wall_ns > 0) {
                    busy_pct = (double)busy_ns / ((double)workers * (double)wall_ns) * 100.0;
                }
                std::cerr << "[UnrealOodleWrapper] job system workers=" << workers
                          << " tasks_executed=" << exec
                          << " peak_queue=" << peak
                          << " total_busy_ms=" << (busy_ns / 1000000.0)
                          << " wall_ms=" << (wall_ns / 1000000.0)
                          << " busy%=" << busy_pct << "\n";
                g_jobSystem.shutdown();
            }
            fprintf(stderr, "Finished compression. Compression ratio: %.2f%%\n", (float(com_size) / filesize) * 100);
        }
        return 0;
    }
}
