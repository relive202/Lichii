// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only instrumentation: EDEN_NX2_COMPILE_TRACE=1 logs how long the driver takes to compile
// each pipeline and how many other compiles were running at the same time, plus how long draws
// wait for a pipeline. EDEN_NX2_COMPILE_SALT=<n> adds an unused specialization constant so the
// driver's own disk cache misses and every pipeline is really compiled (measurement only).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::CompileTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_COMPILE_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// Returns a specialization info carrying the salt, or nullptr when no salt is set.
inline const VkSpecializationInfo* Salt() {
    static const u32 salt_value = [] {
        const char* value = std::getenv("EDEN_NX2_COMPILE_SALT");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    static const VkSpecializationMapEntry entry{.constantID = 0x7FFF0000u, .offset = 0,
                                               .size = sizeof(u32)};
    static const VkSpecializationInfo info{.mapEntryCount = 1, .pMapEntries = &entry,
                                           .dataSize = sizeof(u32), .pData = &salt_value};
    return salt_value != 0 ? &info : nullptr;
}

/// Saves the SPIR-V of a shader once (EDEN_NX2_COMPILE_TRACE_DIR) so slow ones can be studied.
inline void DumpSpirv(u64 hash, const char* stage, std::span<const u32> code) {
    if (!Enabled()) {
        return;
    }
    static const char* directory = std::getenv("EDEN_NX2_COMPILE_TRACE_DIR");
    if (directory == nullptr || directory[0] == '\0') {
        return;
    }
    const auto path = std::filesystem::path{directory} / fmt::format("{:016x}_{}.spv", hash, stage);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size_bytes()));
}

struct Stats {
    std::atomic<int> in_flight{0};
    std::atomic<int> draws_waiting{0};
    std::mutex mutex;
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    int compiles = 0;
    double compile_ms = 0.0;
    double compile_max_ms = 0.0;
    double overlap_sum = 0.0;
    int overlap_max = 0;
    int waits = 0;
    double wait_ms = 0.0;
    double wait_max_ms = 0.0;
};

inline Stats& Get() {
    static Stats stats;
    return stats;
}

inline void FlushLocked(Stats& s, std::chrono::steady_clock::time_point now) {
    if (now - s.window_start < std::chrono::seconds(10)) {
        return;
    }
    if (s.compiles > 0 || s.waits > 0) {
        LOG_WARNING(Render_Vulkan,
                    "CompileTrace 10s: {} driver compiles, {:.0f} ms total, max {:.0f} ms, "
                    "avg {:.2f} others running, max {} running | {} draw waits, {:.0f} ms total, "
                    "max {:.0f} ms",
                    s.compiles, s.compile_ms, s.compile_max_ms,
                    s.compiles ? s.overlap_sum / s.compiles : 0.0, s.overlap_max, s.waits,
                    s.wait_ms, s.wait_max_ms);
    }
    s.window_start = now;
    s.compiles = 0;
    s.compile_ms = s.compile_max_ms = s.overlap_sum = 0.0;
    s.overlap_max = 0;
    s.waits = 0;
    s.wait_ms = s.wait_max_ms = 0.0;
}

/// Wraps one driver vkCreate*Pipelines call.
class CompileScope {
public:
    explicit CompileScope(const char* kind_, std::string label_ = {})
        : kind{kind_}, label{std::move(label_)}, active{Enabled()} {
        if (active) {
            others_at_start = Get().in_flight.fetch_add(1, std::memory_order_acq_rel);
            start = std::chrono::steady_clock::now();
        }
    }
    ~CompileScope() {
        if (!active) {
            return;
        }
        auto& s = Get();
        const int others_at_end = s.in_flight.fetch_sub(1, std::memory_order_acq_rel) - 1;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - start).count();
        const int others = std::max(others_at_start, others_at_end);
        const int waiting = s.draws_waiting.load(std::memory_order_relaxed);
        if (ms >= 100.0) {
            LOG_WARNING(Render_Vulkan,
                        "CompileTrace {} compile {:.0f} ms, others running at start {} / end {}, "
                        "draws waiting {}, shader {}",
                        kind, ms, others_at_start, others_at_end, waiting, label);
        }
        std::scoped_lock lock{s.mutex};
        ++s.compiles;
        s.compile_ms += ms;
        s.compile_max_ms = std::max(s.compile_max_ms, ms);
        s.overlap_sum += others;
        s.overlap_max = std::max(s.overlap_max, others);
        FlushLocked(s, now);
    }

private:
    const char* kind;
    std::string label;
    bool active;
    int others_at_start = 0;
    std::chrono::steady_clock::time_point start;
};

/// Wraps the GPU thread waiting for a pipeline a draw needs.
class WaitScope {
public:
    explicit WaitScope(const char* kind_) : kind{kind_}, active{Enabled()} {
        if (active) {
            Get().draws_waiting.fetch_add(1, std::memory_order_acq_rel);
            start = std::chrono::steady_clock::now();
        }
    }
    ~WaitScope() {
        if (!active) {
            return;
        }
        auto& s = Get();
        s.draws_waiting.fetch_sub(1, std::memory_order_acq_rel);
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - start).count();
        if (ms >= 100.0) {
            LOG_WARNING(Render_Vulkan, "CompileTrace {} draw waited {:.0f} ms, compiles running {}",
                        kind, ms, s.in_flight.load(std::memory_order_relaxed));
        }
        std::scoped_lock lock{s.mutex};
        ++s.waits;
        s.wait_ms += ms;
        s.wait_max_ms = std::max(s.wait_max_ms, ms);
        FlushLocked(s, now);
    }

private:
    const char* kind;
    bool active;
    std::chrono::steady_clock::time_point start;
};

} // namespace Vulkan::CompileTrace
