#pragma once

#include "gpu/draw_list.hpp"

// The colours the interface shares. Understory's editor is the reference --
// white outlines on dark, filled white for whatever is active -- with its teal
// traded for the purple of the inspector's nebula.
namespace ps::editor::theme {

using gpu::Color;

inline const Color kWhite = Color::hex(0xFFFFFF);
inline const Color kClear = Color{0.0f, 0.0f, 0.0f, 0.0f};
// What an outline turns while the pointer is over it.
inline const Color kHover = Color::hex(0xD9B8FF);
// The icon on an active, white-filled button: the nebula's deepest plum.
inline const Color kActiveIcon = Color::hex(0x4B1A5E);
// A button that exists but does nothing yet.
inline const Color kDormant = Color::hex(0x555555);

// The strip along the bottom.
inline const Color kBar = Color::hex(0x141417);
inline const Color kBarRule = Color::hex(0x333333);
// The ruler under the clip, a step lighter than the bar past its end.
inline const Color kClipSpan = Color::hex(0x1C1922);
inline const Color kPlayhead = Color::hex(0xFF4444);
inline const Color kPlayheadLit = Color::hex(0xFF6666);

// Around the footage, where the letterbox falls.
inline const Color kViewer = Color::hex(0x0B0A0D);

}  // namespace ps::editor::theme
