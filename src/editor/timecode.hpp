#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ps::editor {

// HH:MM:SS:FF for a frame index, non-drop-frame.
//
// Counted at the nominal rate -- 24 for 23.976, 30 for 29.97 -- which is how
// an NLE labels non-drop timecode, so the reading here matches the one in the
// edit. The clock drifts from wall time at the fractional rates by 0.1%, which
// is what drop-frame exists to correct and what nobody counting frames wants.
inline std::string format_timecode(std::int64_t frame, double fps) {
    const std::int64_t base = std::max<std::int64_t>(1, std::llround(fps > 0.0 ? fps : 24.0));
    const std::int64_t f = std::max<std::int64_t>(0, frame);
    const std::int64_t seconds = f / base;
    char out[32];
    std::snprintf(out, sizeof out, "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(seconds / 3600 % 100),
                  static_cast<long long>(seconds / 60 % 60),
                  static_cast<long long>(seconds % 60), static_cast<long long>(f % base));
    return out;
}

}  // namespace ps::editor
