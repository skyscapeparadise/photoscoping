#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>

#include "editor/editor_window.hpp"
#include "editor/inspector.hpp"
#include "gpu/display_pass.hpp"
#include "gpu/draw_list.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/icon_atlas.hpp"
#include "gpu/ingest_pass.hpp"
#include "gpu/nebula_pass.hpp"
#include "media/video_decoder.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"

namespace ps::app {

// The interactive window: the editor's interface around the footage, and the
// loop that plays it.
//
// Every frame composites in one order into the context's linear target -- the
// footage into the viewer's area, the nebula into the inspector's, then the
// interface over both -- and is encoded once for the swapchain.
class EditorApp {
public:
    struct Clip {
        // Null for a window with nothing open.
        media::VideoDecoder* decoder = nullptr;
        std::string name;
        // How the decoder's current frame becomes working space. Asked again
        // for every frame, since the signal is described per frame.
        std::function<gpu::InputTransform()> transform;
        editor::ClipInfo info;
    };

    EditorApp() = default;
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // `gpu` and `ingest` are the caller's and must outlive this.
    bool create(gpu::GpuContext& gpu, gpu::IngestPass& ingest, Clip clip);
    void destroy();

    // Runs until the window closes.
    void run();

    // Renders one settled frame -- animations finished, the first frame of
    // the clip decoded -- at the window's current size and writes it as a
    // PNG. Works with a hidden window.
    bool screenshot(const char* path);

private:
    void handle(const SDL_Event& event);
    void set_playing(bool playing);
    void seek(std::int64_t frame);
    // Moves the decoder toward the frame asked for, if it is not there.
    void decode_pending();
    // Lays out, paints and presents one frame. False when the GPU gave up.
    bool render();
    // Everything up to the interface: footage and backdrop into the linear
    // target. Shared by render() and screenshot(), which differ only in where
    // the result is encoded to.
    void prepare(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height, float dt);
    void composite_backdrop(SDL_GPUCommandBuffer* cmd, float scale);
    // Whether the nebula is on screen to be seen churning. Not while the
    // panel is away, nor while the window is minimized or covered: a sky
    // nobody can see is a GPU kept awake for nothing.
    bool churning() const;
    bool build_atlases();

    // Live resizing on macOS runs inside the system's own event loop, and the
    // main loop does not get a turn until the mouse is let go. SDL sends a
    // live-resize expose from in there instead, and this redraws on it, so the
    // layout follows the window edge rather than stretching until release.
    static bool SDLCALL watch_events(void* self, SDL_Event* event);

    gpu::GpuContext* gpu_ = nullptr;
    gpu::IngestPass* ingest_ = nullptr;
    Clip clip_;

    gpu::DisplayPass display_;
    gpu::NebulaPass nebula_;
    gpu::DrawList draw_;
    ui::FontCache fonts_;
    // Two rasterizations of the same art: the hands are drawn three times the
    // size of the transport's glyphs, and one atlas at either size would
    // either blur the large ones or alias the small ones.
    gpu::IconAtlas large_icons_;
    gpu::IconAtlas small_icons_;
    float atlas_scale_ = 0.0f;

    editor::EditorWindow root_;
    ui::InputRouter input_;

    bool running_ = false;
    bool rendering_ = false;
    bool animating_ = false;
    bool fresh_frame_ = false;
    bool playing_ = false;
    std::int64_t target_ = 0;
    Uint64 next_due_ = 0;
    Uint64 last_tick_ = 0;
    // Seconds the nebula has churned. Advanced only while it is churning, so
    // it pauses where it was rather than leaping on when it comes back.
    float nebula_time_ = 0.0f;
};

}  // namespace ps::app
