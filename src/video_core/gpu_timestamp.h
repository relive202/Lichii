// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

/// How much faster Boost and Overclock make Switch 2 GPU work look.
[[nodiscard]] constexpr u64 AmpereClockDivisor(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 4;
    case Settings::GpuClock::Overclock:
        return 16;
    default:
        return 1;
    }
}

/// The GPU Clocks setting as a title's timestamps use it. Ocarina of Time (Switch 2) sizes its
/// dynamic resolution from GPU time; Boost already takes it to its 1920x1080 maximum, and players
/// on Overclock saw textures flicker and turn white (with Force maximum clocks on, too), so there
/// Overclock counts as Boost.
[[nodiscard]] constexpr Settings::GpuClock EffectiveGpuClock(Settings::GpuClock clock,
                                                             u64 program_id) {
    constexpr u64 OcarinaOfTime = 0x0400486024DEC000ULL;
    if (program_id == OcarinaOfTime && clock == Settings::GpuClock::Overclock) {
        return Settings::GpuClock::Boost;
    }
    return clock;
}

/// Switch 2 titles read report timestamps as nanoseconds and calibrate them against CPU time, so
/// Boost and Overclock shrink only the time since the last present (present_ns): GPU durations
/// within a frame look shorter, while a timestamp never runs ahead of the CPU clock and trails it
/// by less than the time since that present.
template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock,
                                   u64 present_ns) {
    if (ampere) {
        const u64 now = static_cast<u64>(timing.GetGlobalTimeNs().count());
        if (now <= present_ns) {
            return now;
        }
        return present_ns + (now - present_ns) / AmpereClockDivisor(clock);
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
