#include "editor/viewer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/theme.hpp"

namespace ps::editor {

namespace {

// Out to a quarter of fit, to see the frame small against the area; in to 32
// device pixels an image pixel, past any hair worth looking at.
constexpr float kMinOfFit = 0.25f;
constexpr float kMaxDevicePerPixel = 32.0f;
constexpr float kNearestFrom = 2.0f;

SDL_Cursor* move_cursor() {
    static SDL_Cursor* cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    return cursor;
}

}  // namespace

void Viewer::build(ui::FontCache& fonts) {
    readout_ = add<ui::TextNode>();
    readout_->set_font_cache(&fonts);
    readout_->pixel_size = 13.0f;
    readout_->color = gpu::Color::hex(0xFFFFFF, 0.7f);
    readout_->visible = false;

    on_hover_changed = [this](bool inside) {
        if (inside && (navigating_ || space_held_)) SDL_SetCursor(move_cursor());
        else if (!panning_) SDL_SetCursor(SDL_GetDefaultCursor());
    };
    on_press = [this](float x, float y) {
        last_x_ = x;
        last_y_ = y;
        if (navigating_ || space_held_) begin_pan(x, y);
    };
    on_drag = [this](float x, float y) { pan_to(x, y); };
    on_release = [this] { end_pan(); };
    on_middle_press = [this](float x, float y) { begin_pan(x, y); };
    on_middle_release = [this] { end_pan(); };
    on_double_click = [this] {
        if (!navigating_) return;
        // Between fit and one image pixel to one device pixel, about the
        // point clicked.
        if (fitted_) {
            zoom_at((1.0f / device_scale_) / scale_, last_x_, last_y_);
        } else {
            fit();
        }
    };
    // Two fingers pan, as they do across every Mac canvas; a modifier makes
    // the wheel zoom, for a mouse, which has no pinch.
    on_wheel = [this](float dx, float dy) {
        float px = 0.0f, py = 0.0f;
        SDL_GetMouseState(&px, &py);
        if ((SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)) != 0) {
            zoom_at(std::exp(dy / 240.0f), px, py);
        } else {
            centre_x_ -= dx / scale_;
            centre_y_ -= dy / scale_;
            fitted_ = false;
            clamp_centre();
            refresh_readout();
        }
        return true;
    };
    keeps_drag = true;
}

void Viewer::begin_pan(float x, float y) {
    panning_ = true;
    last_x_ = x;
    last_y_ = y;
}

void Viewer::pan_to(float x, float y) {
    if (!panning_) return;
    centre_x_ -= (x - last_x_) / scale_;
    centre_y_ -= (y - last_y_) / scale_;
    last_x_ = x;
    last_y_ = y;
    fitted_ = false;
    if (space_held_) space_panned_ = true;
    clamp_centre();
    refresh_readout();
}

void Viewer::end_pan() { panning_ = false; }

void Viewer::set_image_size(int width, int height) {
    if (width == image_w_ && height == image_h_) return;
    image_w_ = width;
    image_h_ = height;
    fit();
}

void Viewer::set_navigating(bool navigating) { navigating_ = navigating; }

void Viewer::set_space_held(bool held) {
    if (held && !space_held_) space_panned_ = false;
    space_held_ = held;
    if (hovered()) SDL_SetCursor(held || navigating_ ? move_cursor() : SDL_GetDefaultCursor());
}

float Viewer::fit_scale() const {
    if (image_w_ <= 0 || image_h_ <= 0 || width <= 0.0f || height <= 0.0f) return 1.0f;
    return std::min(width / image_w_, height / image_h_);
}

void Viewer::fit() {
    fitted_ = true;
    scale_ = fit_scale();
    centre_x_ = image_w_ * 0.5f;
    centre_y_ = image_h_ * 0.5f;
    refresh_readout();
}

void Viewer::actual_pixels() { zoom_by((1.0f / device_scale_) / scale_); }

void Viewer::zoom_by(float factor) {
    zoom_at(factor, abs_x() + width * 0.5f, abs_y() + height * 0.5f);
}

void Viewer::zoom_at(float factor, float x, float y) {
    if (image_w_ <= 0) return;
    const float next = std::clamp(scale_ * factor, fit_scale() * kMinOfFit,
                                  kMaxDevicePerPixel / device_scale_);
    // The image point under (x, y) stays under it.
    float ix = 0.0f, iy = 0.0f;
    to_image(x, y, &ix, &iy);
    scale_ = next;
    const float cx = abs_x() + width * 0.5f, cy = abs_y() + height * 0.5f;
    centre_x_ = ix - (x - cx) / scale_;
    centre_y_ = iy - (y - cy) / scale_;
    fitted_ = false;
    clamp_centre();
    refresh_readout();
}

void Viewer::clamp_centre() {
    // The centre stays on the image, so some of it is always in view.
    centre_x_ = std::clamp(centre_x_, 0.0f, static_cast<float>(image_w_));
    centre_y_ = std::clamp(centre_y_, 0.0f, static_cast<float>(image_h_));
}

void Viewer::layout_self() {
    if (fitted_) {
        scale_ = fit_scale();
        centre_x_ = image_w_ * 0.5f;
        centre_y_ = image_h_ * 0.5f;
    }
    readout_->x = 12.0f;
    readout_->y = height - 26.0f;
    readout_->width = 120.0f;
    readout_->height = 18.0f;
}

void Viewer::refresh_readout() {
    readout_->visible = !fitted_ && image_w_ > 0;
    // As a percentage of actual pixels: one image pixel to one device pixel.
    readout_->set_text(
        std::to_string(static_cast<int>(std::lround(scale_ * device_scale_ * 100.0f))) + "%");
}

gpu::Rect Viewer::image_rect() const {
    const float cx = abs_x() + width * 0.5f, cy = abs_y() + height * 0.5f;
    return gpu::Rect{cx - centre_x_ * scale_, cy - centre_y_ * scale_, image_w_ * scale_,
                     image_h_ * scale_};
}

bool Viewer::magnified() const { return scale_ * device_scale_ >= kNearestFrom; }

void Viewer::from_image(float ix, float iy, float* x, float* y) const {
    const gpu::Rect r = image_rect();
    *x = r.x + ix * scale_;
    *y = r.y + iy * scale_;
}

void Viewer::to_image(float x, float y, float* ix, float* iy) const {
    const gpu::Rect r = image_rect();
    *ix = (x - r.x) / scale_;
    *iy = (y - r.y) / scale_;
}

}  // namespace ps::editor
