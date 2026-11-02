#pragma once

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/mask_texture.hpp"
#include "fixes/fixes.hpp"
#include "fixes/rematte.hpp"
#include "light/light_map.hpp"
#include "matting/plate.hpp"

namespace ps::app {

// The selected light-map crop as it will export, shown in the card while the
// light tool is in hand: the same rendering as the export (light/light_map),
// at a size for the card, on a thread of its own.
//
// Two layers of work. The frame -- its light, its matte with the patches and
// fixes, the plate's light on it -- is made once per frame and kept; a crop
// moved, or its colour changed, is rendered again from that alone, which is
// quick enough to follow a slider.
//
// Linear light is shown at this frame's own exposure -- the export's spans
// the whole clip, which only an export can measure.
class LightPreview {
public:
    struct Frame {
        std::int64_t frame = -1;
        std::uint64_t version = 0;  // of the matte, fixes and plate it is made from
        bool fill_shadows = false;
        int width = 0, height = 0;
        std::vector<std::uint16_t> working;  // RGBA half floats, linear Rec.2020
        std::vector<std::uint8_t> coverage;  // the stack's matte, a byte a pixel
        std::vector<std::uint8_t> patches;   // RemattePatches' fold, or empty
        fixes::PixelRect patch_rect;
        fixes::Fixes fixes;
        std::string plate_path;
    };
    struct Crop {
        fixes::LightCrop crop;  // its corners on this frame as its only key
        bool linear = true;
    };

    LightPreview() = default;
    ~LightPreview();
    LightPreview(const LightPreview&) = delete;
    LightPreview& operator=(const LightPreview&) = delete;

    void create(SDL_GPUDevice* device, std::function<void()> notify);
    void destroy();

    // Whether the frame this preview would be made from is already in hand.
    bool has_frame(std::int64_t frame, std::uint64_t version, bool fill_shadows) const;
    // A preview of `crop`, from `frame` if given (a new frame), else from the
    // frame in hand. Replaces whatever was waiting.
    void request(std::optional<Frame> frame, Crop crop);
    // Uploads a finished preview. True if there was one.
    bool take(SDL_GPUCommandBuffer* cmd);
    // Blocks until the last request is done. For screenshots.
    void wait();

    SDL_GPUTexture* texture() const { return shown_ ? texture_.texture() : nullptr; }
    float aspect() const { return shown_h_ > 0 ? static_cast<float>(shown_w_) / shown_h_ : 1.0f; }
    // Forgets what is shown -- the crop was deselected or the clip changed.
    void clear() { shown_ = false; }

private:
    void loop();
    void prepare(Frame in);
    bool render(const Crop& crop, std::vector<std::uint8_t>* rgba, int* width, int* height);

    std::function<void()> notify_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool busy_ = false;
    std::optional<Frame> pending_frame_;
    std::optional<Crop> pending_crop_;

    // The frame in hand, on the worker; its key readable from the main thread.
    std::int64_t frame_ = -1;
    std::uint64_t version_ = 0;
    bool fill_shadows_ = false;
    int width_ = 0, height_ = 0;
    std::vector<float> linear_, alpha_;
    std::string plate_path_;
    matting::PlateImage plate_;
    light::FrameLight light_;

    // Made, waiting to be uploaded.
    bool ready_ = false;
    std::vector<std::uint8_t> made_;
    int made_w_ = 0, made_h_ = 0;

    MaskTexture texture_;
    bool shown_ = false;
    int shown_w_ = 0, shown_h_ = 0;
};

}  // namespace ps::app
