#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string>

#include "app/clip_frames.hpp"
#include "app/lockstep_stream.hpp"
#include "app/mask_texture.hpp"
#include "app/matte_frames.hpp"
#include "app/matte_job.hpp"
#include "color/cube_lut.hpp"
#include "editor/editor_window.hpp"
#include "editor/inspector.hpp"
#include "fixes/fixes.hpp"
#include "gpu/display_pass.hpp"
#include "gpu/draw_list.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/icon_atlas.hpp"
#include "gpu/ingest_pass.hpp"
#include "gpu/nebula_pass.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"

namespace ps::app {

// The interactive window: the editor's interface around the footage, and the
// loop that plays it.
//
// Every frame composites in one order into the context's linear target -- the
// footage into the viewer's area, the nebula into the inspector's, then the
// interface over both -- and is encoded once for the swapchain. Footage and
// mattes are decoded on worker threads (see FrameSource), so nothing the
// main thread waits on is slower than an upload.
class EditorApp {
public:
    struct Options {
        // Empty for a window with nothing open yet.
        std::string clip;
        std::int64_t frame = 0;
        bool software_decode = false;
        // Already loaded by the caller, which checked it; the path is for the
        // matting tool, which opens it itself.
        std::optional<color::CubeLut> lut;
        std::string lut_path;
        // Where tools/matte_clip.py writes each clip's renders and reads them
        // back from.
        std::string render_dir;
    };

    EditorApp() = default;
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // `gpu` and `ingest` are the caller's and must outlive this.
    bool create(gpu::GpuContext& gpu, gpu::IngestPass& ingest, Options options);
    void destroy();

    // Opens a clip in place of the current one, at `frame`. A matte run for
    // the old one is stopped; its finished frames stay for a resume.
    bool open_clip(const std::string& path, std::int64_t frame = 0);
    // Loads a LUT and puts the clip through it.
    bool open_lut(const std::string& path);

    // Switches the viewer by the view's name. False for a name that is not a
    // view, or a view with nothing to show.
    bool set_view(const std::string& name);

    // Runs until the window closes.
    void run();

    // Renders one settled frame -- animations finished, the frame asked for
    // decoded -- at the window's current size and writes it as a PNG. Works
    // with a hidden window.
    bool screenshot(const char* path);

private:
    void handle(const SDL_Event& event);
    // A file dropped on the window or chosen in File > Open: a LUT by its
    // extension, a clip otherwise.
    void open_path(const std::string& path);
    void show_open_dialog();
    // Adds the File menu once there is a menu bar to add it to.
    void install_menu();
    void set_playing(bool playing);
    void seek(std::int64_t frame);
    // Asks every source the view needs for the frame wanted, and shows it
    // once they all have it. Never waits.
    void advance(SDL_GPUCommandBuffer* cmd);
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
    // One frame's worth of the main loop's work, for when AppKit is running
    // its own loop instead (see app/tracking_ticker.hpp).
    static void tracking_tick(void* self);

    // Where the matte on screen comes from. The finished videos play
    // smoothly and carry the subject; the per-frame PNGs are there while a
    // run is going or after one stopped short, and are whichever is newer.
    enum class MatteSource { kNone, kVideos, kFrames };

    std::string render_path(const char* suffix) const;
    void open_videos();
    void choose_matte_source();
    // Brings the card, the pill, the ruler and the views into line with the
    // matte and the job.
    void refresh_matte_ui();
    void toggle_job();
    void poll_job();
    // After any change to the fixes: the mask, the ruler's keys, and the file.
    void fixes_changed(bool save);
    void undo(bool redo);

    gpu::GpuContext* gpu_ = nullptr;
    gpu::IngestPass* ingest_ = nullptr;
    Options options_;
    std::string clip_name_;

    ClipFrames clip_;
    LockstepStream matte_;
    LockstepStream subject_;
    MatteFrames frames_;
    MatteJob job_;
    MatteSource source_ = MatteSource::kNone;

    // The hand-made fixes for the open clip, kept beside its renders.
    fixes::Fixes fixes_;
    fixes::History history_;
    // Counts changes, so the mask is rebuilt only when the fixes or the
    // frame have moved since it was last made.
    std::uint64_t fixes_version_ = 0;
    std::uint64_t mask_version_ = ~0ull;
    std::int64_t mask_frame_ = -1;
    MaskTexture mask_;
    bool mask_live_ = false;
    // Whether the clip's fixes file read cleanly (or was not there); if it
    // did not, it is never saved over.
    bool fixes_readable_ = true;
    Uint64 job_started_ns_ = 0;
    std::int64_t done_at_start_ = 0;
    Uint64 last_poll_ns_ = 0;
    editor::View view_ = editor::View::kFootage;

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
    bool playing_ = false;
    // The frame wanted, and the frame on screen. They differ while the
    // workers catch up.
    std::int64_t target_ = 0;
    std::int64_t shown_ = -1;
    Uint64 next_due_ = 0;
    Uint64 last_tick_ = 0;
    // Seconds the nebula has churned. Advanced only while it is churning, so
    // it pauses where it was rather than leaping on when it comes back.
    float nebula_time_ = 0.0f;
    // The SDL event a path chosen in the open dialog arrives as: the dialog
    // answers on a thread of its own.
    Uint32 open_event_ = 0;
    bool file_menu_ = false;
};

}  // namespace ps::app
