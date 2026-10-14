#pragma once

#include <algorithm>
#include <cmath>

#include "gpu/draw_list.hpp"

namespace ps::ui {

// The easing curves the interface uses. Adding more is easy.
enum class Easing { kLinear, kInQuad, kOutQuad, kInOutQuad, kOutCubic, kInOutCubic };

inline float ease(Easing curve, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (curve) {
        case Easing::kInQuad:  return t * t;
        case Easing::kOutQuad: return 1.0f - (1.0f - t) * (1.0f - t);
        case Easing::kInOutQuad:
            return t < 0.5f ? 2.0f * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
        case Easing::kOutCubic: return 1.0f - std::pow(1.0f - t, 3.0f);
        case Easing::kInOutCubic:
            return t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Easing::kLinear:
        default: return t;
    }
}

inline float lerp_value(float a, float b, float t) { return a + (b - a) * t; }

inline gpu::Color lerp_value(const gpu::Color& a, const gpu::Color& b, float t) {
    return gpu::Color{lerp_value(a.r, b.r, t), lerp_value(a.g, b.g, t),
                      lerp_value(a.b, b.b, t), lerp_value(a.a, b.a, t)};
}

// A property that eases toward whatever it is assigned.
//
// Assignment is the trigger:
//
//     Animated<Color> border;
//     border.set_behavior(0.08f, Easing::kInOutQuad);
//     border = hovered ? lit : dim;   // eases; no explicit animation object
//
// Without a behavior set, assignment snaps.
template <typename T>
class Animated {
public:
    Animated() = default;
    explicit Animated(const T& initial) : current_(initial), target_(initial), start_(initial) {}

    void set_behavior(float duration_seconds, Easing curve) {
        duration_ = std::max(duration_seconds, 0.0f);
        curve_ = curve;
    }

    // Assign through the behavior. Re-targeting mid-flight restarts from where
    // the value actually is, not from the previous origin, so an interrupted
    // transition reverses smoothly instead of jumping.
    Animated& operator=(const T& target) {
        if (duration_ <= 0.0f) {
            // Zero duration completes immediately and in-line: assignment is a
            // plain assignment, with nothing left pending to run later.
            current_ = target_ = start_ = target;
            elapsed_ = 0.0f;
            animating_ = false;
            return *this;
        }
        if (animating_ && target_ == target) return *this;
        start_ = current_;
        target_ = target;
        elapsed_ = 0.0f;
        animating_ = true;
        return *this;
    }

    // Sets with no easing regardless of behavior -- for initialisation.
    void reset(const T& value) {
        current_ = target_ = start_ = value;
        elapsed_ = 0.0f;
        animating_ = false;
    }

    // Returns true while still in motion, so a caller can tell whether the
    // frame needs to be followed by another one.
    bool tick(float dt) {
        if (!animating_) return false;
        elapsed_ += dt;
        if (elapsed_ >= duration_) {
            current_ = target_;
            animating_ = false;
            return false;
        }
        current_ = lerp_value(start_, target_, ease(curve_, elapsed_ / duration_));
        return true;
    }

    const T& value() const { return current_; }
    const T& target() const { return target_; }
    bool animating() const { return animating_; }

    operator const T&() const { return current_; }

private:
    T current_{};
    T target_{};
    T start_{};
    float elapsed_ = 0.0f;
    float duration_ = 0.0f;
    Easing curve_ = Easing::kLinear;
    bool animating_ = false;
};

}  // namespace ps::ui
