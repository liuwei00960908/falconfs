/* Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * SPDX-License-Identifier: MulanPSL-2.0
 */

#include "block_meta.h"
#include "falcon_meta.h"
#include "remote_connection_utils/error_code_def.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 55510;
    std::string op = "put";
    size_t size = 2 * 1024 * 1024;
    size_t keys = 100;
    int threads = 1;
    uint64_t capacity = 100ULL * 1024 * 1024 * 1024;
    std::string blockDataDir = "/tmp/opencode/falcon-block-data";
    std::string configPath = "tests/block_api/block_api_bench.conf";
    std::string prefix;
    std::string phase;
    bool prepare = true;
    bool verify = false;
    bool csv = false;
    bool csvHeader = false;
};

struct ThreadResult {
    uint64_t attemptOps = 0;
    uint64_t successOps = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    std::vector<uint64_t> successLatenciesUs;
    std::vector<int> errorCodes;
};

struct Summary {
    uint64_t attemptOps = 0;
    uint64_t successOps = 0;
    uint64_t totalBytes = 0;
    uint64_t errors = 0;
    double seconds = 0.0;
    double avgUs = 0.0;
    uint64_t p50Us = 0;
    uint64_t p95Us = 0;
    uint64_t p99Us = 0;
    std::map<int, uint64_t> errorCodes;
};

struct ThreadContext {
    std::vector<char> writeBuffer;
    std::vector<char> readBuffer;
    std::vector<char> expectedBuffer;
};

void Usage(const char *prog)
{
    std::cerr << "Usage: " << prog
              << " --op put|get|del|reclaim --size BYTES --keys N --threads N "
                 "[--config PATH] [--host HOST] [--port PORT] [--capacity BYTES] [--block-data-dir DIR] "
                 "[--prefix PREFIX] [--phase PHASE] [--no-prepare] [--verify] [--csv] [--csv-header]\n";
}

std::string Trim(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool ParseSizeT(const char *text, size_t *value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    try {
        *value = static_cast<size_t>(std::stoull(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseUInt64(const char *text, uint64_t *value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    try {
        *value = static_cast<uint64_t>(std::stoull(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseInt(const char *text, int *value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    try {
        *value = std::stoi(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseBool(const std::string &text, bool *value)
{
    std::string normalized = text;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        *value = true;
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        *value = false;
        return true;
    }
    return false;
}

std::string ConfigPathFromArgs(int argc, char **argv, const std::string &defaultPath)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config") {
            return argv[i + 1];
        }
    }
    return defaultPath;
}

bool ApplyConfigValue(Options *options, const std::string &key, const std::string &value)
{
    if (key == "host") {
        options->host = value;
    } else if (key == "port") {
        return ParseInt(value.c_str(), &options->port);
    } else if (key == "op") {
        options->op = value;
    } else if (key == "size") {
        return ParseSizeT(value.c_str(), &options->size);
    } else if (key == "keys") {
        return ParseSizeT(value.c_str(), &options->keys);
    } else if (key == "threads") {
        return ParseInt(value.c_str(), &options->threads);
    } else if (key == "capacity") {
        return ParseUInt64(value.c_str(), &options->capacity);
    } else if (key == "block_data_dir") {
        options->blockDataDir = value;
    } else if (key == "prefix") {
        options->prefix = value;
    } else if (key == "phase") {
        options->phase = value;
    } else if (key == "prepare") {
        return ParseBool(value, &options->prepare);
    } else if (key == "verify") {
        return ParseBool(value, &options->verify);
    } else if (key == "csv") {
        return ParseBool(value, &options->csv);
    } else if (key == "csv_header") {
        bool enabled = false;
        if (!ParseBool(value, &enabled)) {
            return false;
        }
        options->csvHeader = enabled;
        if (enabled) {
            options->csv = true;
        }
    }
    return true;
}

bool LoadConfig(Options *options)
{
    std::ifstream input(options->configPath);
    if (!input.is_open()) {
        return true;
    }

    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        size_t sep = line.find('=');
        if (sep == std::string::npos) {
            std::cerr << "invalid config line " << lineNumber << " in " << options->configPath << std::endl;
            return false;
        }

        std::string key = Trim(line.substr(0, sep));
        std::string value = Trim(line.substr(sep + 1));
        if (!ApplyConfigValue(options, key, value)) {
            std::cerr << "invalid config value at line " << lineNumber << " in " << options->configPath << std::endl;
            return false;
        }
    }
    return true;
}

bool ParseArgs(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << std::endl;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--host") {
            const char *value = needValue("--host");
            if (value == nullptr) {
                return false;
            }
            options->host = value;
        } else if (arg == "--port") {
            const char *value = needValue("--port");
            if (value == nullptr || !ParseInt(value, &options->port)) {
                return false;
            }
        } else if (arg == "--op") {
            const char *value = needValue("--op");
            if (value == nullptr) {
                return false;
            }
            options->op = value;
        } else if (arg == "--size") {
            const char *value = needValue("--size");
            if (value == nullptr || !ParseSizeT(value, &options->size)) {
                return false;
            }
        } else if (arg == "--keys") {
            const char *value = needValue("--keys");
            if (value == nullptr || !ParseSizeT(value, &options->keys)) {
                return false;
            }
        } else if (arg == "--threads") {
            const char *value = needValue("--threads");
            if (value == nullptr || !ParseInt(value, &options->threads)) {
                return false;
            }
        } else if (arg == "--capacity") {
            const char *value = needValue("--capacity");
            if (value == nullptr || !ParseUInt64(value, &options->capacity)) {
                return false;
            }
        } else if (arg == "--block-data-dir") {
            const char *value = needValue("--block-data-dir");
            if (value == nullptr) {
                return false;
            }
            options->blockDataDir = value;
        } else if (arg == "--config") {
            const char *value = needValue("--config");
            if (value == nullptr) {
                return false;
            }
            options->configPath = value;
        } else if (arg == "--prefix") {
            const char *value = needValue("--prefix");
            if (value == nullptr) {
                return false;
            }
            options->prefix = value;
        } else if (arg == "--phase") {
            const char *value = needValue("--phase");
            if (value == nullptr) {
                return false;
            }
            options->phase = value;
        } else if (arg == "--no-prepare") {
            options->prepare = false;
        } else if (arg == "--verify") {
            options->verify = true;
        } else if (arg == "--csv") {
            options->csv = true;
        } else if (arg == "--csv-header") {
            options->csv = true;
            options->csvHeader = true;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::cerr << "unknown argument: " << arg << std::endl;
            return false;
        }
    }

    if (options->size == 0 || options->keys == 0 || options->threads <= 0) {
        return false;
    }
    if (options->capacity < options->size) {
        return false;
    }
    if (options->op != "put" && options->op != "get" && options->op != "del" && options->op != "reclaim") {
        return false;
    }
    if (options->prefix.empty()) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        options->prefix = "block_bench_" + std::to_string(now);
    }
    return true;
}

void ApplyEnvironment(const Options &options)
{
    if (!options.blockDataDir.empty()) {
        setenv("FALCON_BLOCK_DATA_DIR", options.blockDataDir.c_str(), 1);
    }
    setenv("FALCON_BLOCK_FILE_CAPACITY", std::to_string(options.capacity).c_str(), 1);
}

std::string KeyFor(const Options &options, const std::string &phase, size_t index)
{
    return options.prefix + "_" + phase + "_" + std::to_string(index);
}

std::string EffectivePhase(const Options &options, const std::string &fallback)
{
    return options.phase.empty() ? fallback : options.phase;
}

std::vector<char> DataFor(size_t size)
{
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>('a' + (i % 26));
    }
    return data;
}

int PutOne(const std::string &key, const std::vector<char> &data)
{
    return FalconBlockPut(key, data.data(), data.size());
}

int PrepareKeys(const Options &options, const std::string &phase)
{
    std::vector<char> data = DataFor(options.size);
    for (size_t i = 0; i < options.keys; ++i) {
        int ret = PutOne(KeyFor(options, phase, i), data);
        if (ret != SUCCESS) {
            std::cerr << "prepare put failed: key=" << KeyFor(options, phase, i) << " ret=" << ret << std::endl;
            return ret;
        }
    }
    return SUCCESS;
}

int DeleteKeys(const Options &options, const std::string &phase)
{
    for (size_t i = 0; i < options.keys; ++i) {
        int ret = FalconBlockDel(KeyFor(options, phase, i));
        if (ret != SUCCESS && ret != FILE_NOT_EXISTS) {
            std::cerr << "prepare delete failed: key=" << KeyFor(options, phase, i) << " ret=" << ret << std::endl;
            return ret;
        }
    }
    return SUCCESS;
}

uint64_t Percentile(std::vector<uint64_t> *values, double percentile)
{
    if (values->empty()) {
        return 0;
    }
    size_t index = static_cast<size_t>((percentile / 100.0) * static_cast<double>(values->size() - 1));
    std::nth_element(values->begin(), values->begin() + index, values->end());
    return (*values)[index];
}

Summary Summarize(const Options &options,
                  const std::vector<ThreadResult> &threadResults,
                  std::chrono::steady_clock::duration elapsed)
{
    Summary summary;
    std::vector<uint64_t> latencies;
    for (const auto &result : threadResults) {
        summary.attemptOps += result.attemptOps;
        summary.successOps += result.successOps;
        summary.totalBytes += result.bytes;
        summary.errors += result.errors;
        latencies.insert(latencies.end(), result.successLatenciesUs.begin(), result.successLatenciesUs.end());
        for (int code : result.errorCodes) {
            ++summary.errorCodes[code];
        }
    }

    summary.seconds = std::chrono::duration<double>(elapsed).count();
    if (!latencies.empty()) {
        uint64_t latencySum = std::accumulate(latencies.begin(), latencies.end(), uint64_t{0});
        summary.avgUs = static_cast<double>(latencySum) / static_cast<double>(latencies.size());
        std::vector<uint64_t> p50 = latencies;
        std::vector<uint64_t> p95 = latencies;
        std::vector<uint64_t> p99 = latencies;
        summary.p50Us = Percentile(&p50, 50.0);
        summary.p95Us = Percentile(&p95, 95.0);
        summary.p99Us = Percentile(&p99, 99.0);
    }
    (void)options;
    return summary;
}

void PrintSummary(const Options &options, const Summary &summary)
{
    double opsSec = summary.seconds > 0.0 ? static_cast<double>(summary.successOps) / summary.seconds : 0.0;
    double mbSec = summary.seconds > 0.0 ? static_cast<double>(summary.totalBytes) / 1024.0 / 1024.0 / summary.seconds : 0.0;

    if (options.csvHeader) {
        std::cout << "op,size,threads,keys,attempt_ops,success_ops,total_bytes,seconds,success_ops_sec,mb_sec,avg_us,p50_us,p95_us,p99_us,errors,error_codes,prefix\n";
    }
    if (options.csv) {
        std::cout << options.op << ',' << options.size << ',' << options.threads << ',' << options.keys << ','
                  << summary.attemptOps << ',' << summary.successOps << ',' << summary.totalBytes << ','
                  << summary.seconds << ',' << opsSec << ',' << mbSec << ',' << summary.avgUs << ','
                  << summary.p50Us << ',' << summary.p95Us << ',' << summary.p99Us << ',' << summary.errors << ',';
        bool first = true;
        for (const auto &[code, count] : summary.errorCodes) {
            if (!first) {
                std::cout << '|';
            }
            first = false;
            std::cout << code << ':' << count;
        }
        std::cout << ',' << options.prefix << std::endl;
        return;
    }

    double avgNs = summary.avgUs * 1000.0;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "[FINISH] BlockOp " << options.op << ", Time " << summary.seconds << ", OPs "
              << summary.successOps << ", Throughput " << opsSec << ", Average Latency " << avgNs << std::endl;
    std::cout << "BlockApiBench Summary" << std::endl;
    std::cout << "  op: " << options.op << std::endl;
    std::cout << "  data_size_bytes: " << options.size << std::endl;
    std::cout << "  threads: " << options.threads << std::endl;
    std::cout << "  keys: " << options.keys << std::endl;
    std::cout << "  attempt_ops: " << summary.attemptOps << std::endl;
    std::cout << "  success_ops: " << summary.successOps << std::endl;
    std::cout << "  total_bytes: " << summary.totalBytes << std::endl;
    std::cout << "  elapsed_seconds: " << summary.seconds << std::endl;
    std::cout << "  throughput_success_ops_sec: " << opsSec << std::endl;
    std::cout << "  throughput_mib_sec: " << mbSec << std::endl;
    std::cout << "  latency_avg_us: " << summary.avgUs << std::endl;
    std::cout << "  latency_p50_us: " << summary.p50Us << std::endl;
    std::cout << "  latency_p95_us: " << summary.p95Us << std::endl;
    std::cout << "  latency_p99_us: " << summary.p99Us << std::endl;
    std::cout << "  errors: " << summary.errors << std::endl;
    if (!summary.errorCodes.empty()) {
        std::cout << "  error_codes: ";
        bool first = true;
        for (const auto &[code, count] : summary.errorCodes) {
            if (!first) {
                std::cout << '|';
            }
            first = false;
            std::cout << code << ':' << count;
        }
        std::cout << std::endl;
    }
    std::cout << "  prefix: " << options.prefix << std::endl;
}

template <typename Fn>
Summary RunTimed(const Options &options, Fn op)
{
    std::vector<ThreadResult> results(static_cast<size_t>(options.threads));
    std::atomic<size_t> nextIndex{0};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;

    for (int t = 0; t < options.threads; ++t) {
        workers.emplace_back([&, t]() {
            ThreadResult &result = results[static_cast<size_t>(t)];
            ThreadContext ctx;
            ctx.writeBuffer = DataFor(options.size);
            ctx.readBuffer.resize(options.size);
            if (options.verify) {
                ctx.expectedBuffer = ctx.writeBuffer;
            }

            result.successLatenciesUs.reserve(options.keys / static_cast<size_t>(options.threads) + 1);
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (true) {
                size_t index = nextIndex.fetch_add(1);
                if (index >= options.keys) {
                    break;
                }

                auto begin = std::chrono::steady_clock::now();
                int ret = op(index, result, ctx);
                auto end = std::chrono::steady_clock::now();
                uint64_t latencyUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());

                ++result.attemptOps;
                if (ret == SUCCESS) {
                    ++result.successOps;
                    result.successLatenciesUs.push_back(latencyUs);
                } else {
                    ++result.errors;
                    result.errorCodes.push_back(ret);
                }
            }
        });
    }

    while (ready.load() < options.threads) {
        std::this_thread::yield();
    }
    auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    auto end = std::chrono::steady_clock::now();
    return Summarize(options, results, end - begin);
}

Summary RunPut(const Options &options, const std::string &phase)
{
    return RunTimed(options, [&](size_t index, ThreadResult &result, ThreadContext &ctx) {
        int ret = FalconBlockPut(KeyFor(options, phase, index), ctx.writeBuffer.data(), ctx.writeBuffer.size());
        if (ret == SUCCESS) {
            result.bytes += options.size;
        }
        return ret;
    });
}

Summary RunGet(const Options &options, const std::string &phase)
{
    return RunTimed(options, [&](size_t index, ThreadResult &result, ThreadContext &ctx) {
        int ret = FalconBlockGet(KeyFor(options, phase, index), ctx.readBuffer.data(), ctx.readBuffer.size());
        if (ret == SUCCESS) {
            if (options.verify) {
                if (std::memcmp(ctx.readBuffer.data(), ctx.expectedBuffer.data(), options.size) != 0) {
                    return static_cast<int>(IO_ERROR);
                }
            }
            result.bytes += options.size;
        }
        return ret;
    });
}

Summary RunDel(const Options &options, const std::string &phase)
{
    return RunTimed(options, [&](size_t index, ThreadResult &result, ThreadContext &ctx) {
        (void)result;
        (void)ctx;
        return FalconBlockDel(KeyFor(options, phase, index));
    });
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    options.configPath = ConfigPathFromArgs(argc, argv, options.configPath);
    if (!LoadConfig(&options)) {
        return 1;
    }
    if (!ParseArgs(argc, argv, &options)) {
        Usage(argv[0]);
        return 1;
    }

    ApplyEnvironment(options);

    try {
        router = std::make_shared<Router>(ServerIdentifier(options.host, options.port));
    } catch (const std::exception &ex) {
        std::cerr << "failed to initialize router for " << options.host << ':' << options.port << ": " << ex.what()
                  << std::endl;
        return 1;
    }

    Summary summary;
    if (options.op == "put") {
        DeleteKeys(options, "put");
        summary = RunPut(options, "put");
    } else if (options.op == "get") {
        std::string phase = EffectivePhase(options, "get");
        if (options.prepare) {
            int ret = PrepareKeys(options, phase);
            if (ret != SUCCESS) {
                return 1;
            }
        }
        summary = RunGet(options, phase);
    } else if (options.op == "del") {
        std::string phase = EffectivePhase(options, "del");
        if (options.prepare) {
            int ret = PrepareKeys(options, phase);
            if (ret != SUCCESS) {
                return 1;
            }
        }
        summary = RunDel(options, phase);
    } else if (options.op == "reclaim") {
        int ret = PrepareKeys(options, "reclaim_old");
        if (ret != SUCCESS) {
            return 1;
        }
        ret = DeleteKeys(options, "reclaim_old");
        if (ret != SUCCESS) {
            return 1;
        }
        summary = RunPut(options, "reclaim_new");
    }

    PrintSummary(options, summary);
    return summary.errors == 0 ? 0 : 1;
}
