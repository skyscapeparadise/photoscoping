#include "app/editor_app.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "editor/theme.hpp"

namespace ps::app {

namespace {

// Largest a tool button's hand is drawn (ToolStrip: 84px buttons, 56/72 of
// that the hand), and the transport's glyphs and the playhead's tip.
constexpr float kLargeIcon = 66.0f;
constexpr float kSmallIcon = 24.0f;

// How often an otherwise idle window redraws to move the nebula. It drifts
// slowly enough that this reads as smooth, at a quarter of the frames a
// 120Hz display would otherwise draw.
constexpr Sint32 kChurnIntervalMs = 33;

constexpr int kMinWindowWidth = 640;
constexpr int kMinWindowHeight = 400;

// Where a bundled resource is. Next to the working directory when run from the
// checkout; beside the executable's parent when run as build/photoscoping from
// anywhere else; in Resources inside the .app. Resolved to a path rather than
// changing directory, because clip and LUT paths on the command line are
// relative to where the user ran it.
std::string resource_path(const std::string& relative) {
    std::error_code ec;
    if (std::filesystem::exists(relative, ec)) return relative;
    if (const char* base = SDL_GetBasePath()) {
        const std::filesystem::path root(base);
        for (const std::filesystem::path& candidate :
             {root, root / "..", root / ".." / "Resources"}) {
            if (std::filesystem::exists(candidate / relative, ec))
                return (candidate / relative).lexically_normal().string();
        }
    }
    return relative;
}

// Wheel deltas, in pixels, whatever produced them.
//
// A trackpad sends precise scrolls, which the Cocoa backend passes on as
// pixels scaled down by ten; a wheel mouse sends line counts, one per notch.
// Which kind a device sends is remembered per device -- one that has ever sent
// a fractional delta is precise -- so a mouse does not inherit a trackpad's
// scale on a machine with both.
//
// SDL's Cocoa backend negates the horizontal axis and not the vertical, so the
// horizontal is flipped back where this is used, and both mean "the content
// moves this way" by the time anything consumes them.
constexpr float kPixelsPerPreciseUnit = 10.0f;
constexpr float kPixelsPerNotch = 48.0f;

float wheel_scale(const SDL_MouseWheelEvent& e) {
    static std::unordered_map<SDL_MouseID, bool> precise;
    bool& is_precise = precise[e.which];
    if (!is_precise)
        is_precise = (e.x != 0.0f && e.x != std::round(e.x)) ||
                     (e.y != 0.0f && e.y != std::round(e.y));
    return is_precise ? kPixelsPerPreciseUnit : kPixelsPerNotch;
}

// The viewer's background, cleared into the linear target, which holds
// premultiplied nits against the interface's reference white.
SDL_FColor viewer_background() {
    auto linear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const gpu::Color& c = editor::theme::kViewer;
    const float nits = gpu::DrawList::kSdrRefNits;
    return {linear(c.r) * nits, linear(c.g) * nits, linear(c.b) * nits, 1.0f};
}

SDL_FRect to_device(const gpu::Rect& r, float scale) {
    // Whole pixels, so the footage and the nebula meet the interface's edges
    // exactly rather than on a blended half pixel.
    const float x0 = std::round(r.x * scale), y0 = std::round(r.y * scale);
    const float x1 = std::round((r.x + r.w) * scale), y1 = std::round((r.y + r.h) * scale);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

}  // namespace

EditorApp::~EditorApp() { destroy(); }

bool EditorApp::create(gpu::GpuContext& gpu, gpu::IngestPass& ingest, Clip clip) {
    gpu_ = &gpu;
    ingest_ = &ingest;
    clip_ = std::move(clip);

    SDL_SetWindowMinimumSize(gpu.window(), kMinWindowWidth, kMinWindowHeight);
    SDL_SetWindowTitle(gpu.window(), clip_.decoder
                                         ? ("photoscoping \xE2\x80\x94 " + clip_.name).c_str()
                                         : "photoscoping");

    if (!display_.create(gpu) || !nebula_.create(gpu) || !draw_.create(gpu)) return false;
    if (!fonts_.init(gpu.device(), resource_path("fonts/MonaSans-Regular.ttf").c_str(),
                     resource_path("fonts/MonaSans-Bold.ttf").c_str()))
        return false;
    if (!build_atlases()) return false;

    root_.build(fonts_, large_icons_, small_icons_);
    root_.inspector()->set_clip(clip_.info);

    root_.inspector()->views()->on_view_changed = [this](editor::View view) { view_ = view; };
    root_.inspector()->on_action = [this] { toggle_job(); };
    job_.locate();
    if (clip_.decoder) {
        frames_.open(gpu.device(), render_path("_mattes"), clip_.decoder->frame_count());
        choose_matte_source();
    }
    refresh_matte_ui();

    editor::Timeline* timeline = root_.timeline();
    if (clip_.decoder) {
        timeline->set_clip(clip_.decoder->frame_rate(), clip_.decoder->frame_count());
        timeline->set_frame(clip_.decoder->frame_index());
        target_ = clip_.decoder->frame_index();
        fresh_frame_ = true;
    }
    timeline->on_seek = [this](std::int64_t frame) { seek(frame); };
    timeline->on_play = [this](bool play) { set_playing(play); };
    timeline->on_stop = [this] {
        set_playing(false);
        target_ = 0;
    };
    timeline->on_step = [this](int delta) {
        if (!clip_.decoder) return;
        set_playing(false);
        target_ = std::max<std::int64_t>(0, clip_.decoder->frame_index() + delta);
    };

    SDL_AddEventWatch(&EditorApp::watch_events, this);
    last_tick_ = SDL_GetTicksNS();
    return true;
}

void EditorApp::destroy() {
    if (!gpu_) return;
    SDL_RemoveEventWatch(&EditorApp::watch_events, this);
    // The interface holds text that belongs to the font cache's engine, so
    // the tree goes first.
    root_.clear_children();
    job_.stop();
    frames_.destroy();
    matte_.destroy();
    subject_.destroy();
    large_icons_.destroy();
    small_icons_.destroy();
    fonts_.destroy();
    draw_.destroy();
    nebula_.destroy();
    display_.destroy();
    gpu_ = nullptr;
}

bool EditorApp::build_atlases() {
    const float scale = SDL_GetWindowDisplayScale(gpu_->window());
    atlas_scale_ = scale > 0.0f ? scale : 1.0f;
    const std::string dir = resource_path("assets/icons");
    return large_icons_.build(gpu_->device(), dir.c_str(),
                              static_cast<int>(std::ceil(kLargeIcon * atlas_scale_))) &&
           small_icons_.build(gpu_->device(), dir.c_str(),
                              static_cast<int>(std::ceil(kSmallIcon * atlas_scale_)));
}

bool EditorApp::set_view(const std::string& name) {
    editor::ViewSwitch* views = root_.inspector()->views();
    for (editor::View view : {editor::View::kFootage, editor::View::kMatte,
                              editor::View::kSubject, editor::View::kOverlay}) {
        if (name != editor::view_label(view)) continue;
        if (!views->available(view)) return false;
        views->set_view(view);
        return true;
    }
    return false;
}

std::string EditorApp::render_path(const char* suffix) const {
    return (std::filesystem::path(clip_.render_dir) / (clip_.name + suffix)).string();
}

void EditorApp::open_videos() {
    // A render that will not open is left out with a word in the log, not
    // treated as fatal: the footage is still worth looking at without it.
    std::error_code ec;
    const std::string matte = render_path("_matte.mp4");
    const std::string subject = render_path("_comp.mp4");
    if (std::filesystem::exists(matte, ec) &&
        !matte_.open(gpu_->device(), matte, LockstepStream::Kind::kCoverage))
        SDL_Log("cannot open the matte %s", matte.c_str());
    if (std::filesystem::exists(subject, ec) &&
        !subject_.open(gpu_->device(), subject, LockstepStream::Kind::kPicture))
        SDL_Log("cannot open the subject %s", subject.c_str());
}

void EditorApp::choose_matte_source() {
    std::error_code ec;
    const std::string video = render_path("_matte.mp4");
    const bool have_video = std::filesystem::exists(video, ec);
    const bool frames_newer =
        frames_.done_count() > 0 &&
        (!have_video || frames_.newest() > std::filesystem::last_write_time(video, ec));
    if (job_.running() || frames_newer) {
        source_ = MatteSource::kFrames;
    } else if (have_video) {
        if (!matte_.is_open()) open_videos();
        source_ = matte_.is_open() ? MatteSource::kVideos : MatteSource::kNone;
    } else {
        source_ = MatteSource::kNone;
    }
}

namespace {

std::string time_left(double seconds) {
    if (seconds < 60.0) return "under a minute left";
    const int minutes = static_cast<int>(std::ceil(seconds / 60.0));
    if (minutes < 60) return std::to_string(minutes) + " min left";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min left";
}

}  // namespace

void EditorApp::refresh_matte_ui() {
    editor::Inspector* inspector = root_.inspector();
    const std::int64_t total = clip_.decoder ? clip_.decoder->frame_count() : 0;
    const std::int64_t done = frames_.done_count();
    const bool complete = total > 0 && done >= total;

    switch (source_) {
        case MatteSource::kNone:
            root_.timeline()->set_matted({});
            break;
        case MatteSource::kFrames:
            root_.timeline()->set_matted(frames_.done());
            break;
        case MatteSource::kVideos:
            root_.timeline()->set_matted(std::vector<bool>(
                static_cast<std::size_t>(std::min(total, matte_.frame_count())), true));
            break;
    }
    inspector->views()->set_available(
        source_ != MatteSource::kNone,
        source_ == MatteSource::kVideos && subject_.is_open());

    std::string row = "none";
    if (job_.running()) {
        row = std::to_string(done) + " of " + std::to_string(total);
        const double elapsed = (SDL_GetTicksNS() - job_started_ns_) / 1e9;
        const std::int64_t made = done - done_at_start_;
        if (made > 0) row += " \xC2\xB7 " + time_left((total - done) * elapsed / made);
    } else if (source_ == MatteSource::kVideos) {
        row = "rendered";
    } else if (source_ == MatteSource::kFrames) {
        row = complete ? std::to_string(done) + " frames" : std::to_string(done) + " of " +
                                                              std::to_string(total);
    }
    inspector->set_row("matte", row);

    if (!clip_.decoder || !job_.available())
        inspector->set_action("matte clip", false);
    else if (job_.running())
        inspector->set_action("stop matting", true);
    else if (done > 0 && !complete)
        inspector->set_action("resume matting", true);
    else if (source_ != MatteSource::kNone)
        inspector->set_action("matted", false);
    else
        inspector->set_action("matte clip", true);
}

void EditorApp::toggle_job() {
    if (job_.running()) {
        job_.stop();
    } else if (clip_.decoder) {
        // The videos are about to be replaced, and the PNGs are what will
        // be current, so stop reading the old ones now.
        matte_.destroy();
        subject_.destroy();
        if (job_.start(clip_.path, clip_.lut_path, clip_.render_dir, clip_.name)) {
            job_started_ns_ = SDL_GetTicksNS();
            done_at_start_ = frames_.done_count();
        }
    }
    frames_.rescan();
    choose_matte_source();
    refresh_matte_ui();
}

void EditorApp::poll_job() {
    if (!job_.running()) return;
    const Uint64 now = SDL_GetTicksNS();
    if (now - last_poll_ns_ < 1000000000ull) return;
    last_poll_ns_ = now;
    frames_.rescan();
    bool succeeded = false;
    if (job_.poll_finished(&succeeded)) {
        if (!succeeded)
            SDL_Log("the matting stopped short; see %s", render_path("_matte.log").c_str());
        // Finished, the new videos are in place and newer than the frames.
        choose_matte_source();
    }
    refresh_matte_ui();
}

void EditorApp::set_playing(bool playing) {
    if (!clip_.decoder) return;
    playing_ = playing;
    next_due_ = SDL_GetTicksNS();
}

void EditorApp::seek(std::int64_t frame) {
    if (!clip_.decoder) return;
    target_ = frame;
    // Scrubbing while playing carries on from wherever the head was put.
    next_due_ = SDL_GetTicksNS() +
                static_cast<Uint64>(1e9 / std::max(1.0, clip_.decoder->frame_rate()));
}

void EditorApp::handle(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            running_ = false;
            break;
        case SDL_EVENT_KEY_DOWN: {
            media::VideoDecoder* d = clip_.decoder;
            switch (event.key.key) {
                case SDLK_SPACE:
                    if (!event.key.repeat) set_playing(!playing_);
                    break;
                case SDLK_RIGHT:
                    if (d) { set_playing(false); target_ = d->frame_index() + 1; }
                    break;
                case SDLK_LEFT:
                    if (d) {
                        set_playing(false);
                        target_ = std::max<std::int64_t>(0, d->frame_index() - 1);
                    }
                    break;
                case SDLK_HOME:
                    set_playing(false);
                    target_ = 0;
                    break;
                case SDLK_TAB:
                    if (!event.key.repeat) root_.toggle_panel();
                    break;
                case SDLK_1: root_.inspector()->views()->set_view(editor::View::kFootage); break;
                case SDLK_2: root_.inspector()->views()->set_view(editor::View::kMatte); break;
                case SDLK_3: root_.inspector()->views()->set_view(editor::View::kSubject); break;
                case SDLK_4: root_.inspector()->views()->set_view(editor::View::kOverlay); break;
                default: break;
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            input_.set_pointer(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            input_.pointer_left();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) input_.press();
            else if (event.button.button == SDL_BUTTON_RIGHT) input_.press_right();
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) input_.release();
            else if (event.button.button == SDL_BUTTON_RIGHT) input_.release_right();
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            const float s = wheel_scale(event.wheel);
            input_.wheel(-event.wheel.x * s, event.wheel.y * s, root_);
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            // Dragged onto a display of another density: the art is
            // rasterized for the old one. Rebuilt in place, so every icon node
            // pointing at an atlas keeps pointing at the right one.
            if (SDL_GetWindowDisplayScale(gpu_->window()) != atlas_scale_) build_atlases();
            break;
        default: break;
    }
}

void EditorApp::decode_pending() {
    media::VideoDecoder* d = clip_.decoder;
    if (!d) return;
    if (playing_ && SDL_GetTicksNS() >= next_due_) {
        target_ = d->frame_index() + 1;
        next_due_ += static_cast<Uint64>(1e9 / (d->frame_rate() > 0 ? d->frame_rate() : 24.0));
    }
    if (target_ != d->frame_index()) {
        const bool ok = target_ == d->frame_index() + 1 ? d->read_next() : d->seek_frame(target_);
        if (ok) fresh_frame_ = true;
        else playing_ = false;  // the end of the clip
        target_ = d->frame_index();
    }
    root_.timeline()->set_frame(d->frame_index());
    root_.timeline()->set_playing(playing_);
}

void EditorApp::prepare(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height, float dt) {
    if (fresh_frame_ && clip_.decoder) {
        ingest_->process(cmd, *clip_.decoder, clip_.transform());
        fresh_frame_ = false;
    }
    gpu_->resize_if_needed(width, height);

    int lw = 0, lh = 0;
    SDL_GetWindowSize(gpu_->window(), &lw, &lh);
    const float scale = lw > 0 ? static_cast<float>(width) / lw : 1.0f;

    root_.layout(static_cast<float>(lw), static_cast<float>(lh));
    input_.update(root_);
    animating_ = root_.tick(dt);
    if (churning()) nebula_time_ += dt;
    // The tick can move the panel; lay out again so what is drawn is where
    // the animation has got to, not where it was a frame ago.
    if (animating_) root_.layout(static_cast<float>(lw), static_cast<float>(lh));

    composite_backdrop(cmd, scale);
    draw_.begin_frame(scale);
    root_.paint(draw_, ui::PaintContext{});
}

void EditorApp::composite_backdrop(SDL_GPUCommandBuffer* cmd, float scale) {
    gpu::DisplayPass::Frame frame;
    gpu::DisplayView view = gpu::DisplayView::kFootage;
    if (clip_.decoder && ingest_->working()) {
        frame.working = ingest_->working();
        frame.width = ingest_->width();
        frame.height = ingest_->height();
        // Only what the view reads is decoded, so the footage view plays
        // as fast as it did before there were renders to keep up with.
        const std::int64_t at = clip_.decoder->frame_index();
        const bool wants_matte = view_ != editor::View::kFootage;
        if (wants_matte && source_ == MatteSource::kFrames) {
            frame.matte = frames_.texture(at);
        } else if (wants_matte && source_ == MatteSource::kVideos) {
            if (matte_.sync(cmd, at)) frame.matte = matte_.working();
            if (view_ == editor::View::kSubject && subject_.sync(cmd, at))
                frame.subject = subject_.working();
        }
        switch (view_) {
            case editor::View::kFootage: view = gpu::DisplayView::kFootage; break;
            case editor::View::kMatte: view = gpu::DisplayView::kMatte; break;
            case editor::View::kSubject: view = gpu::DisplayView::kSubject; break;
            case editor::View::kOverlay: view = gpu::DisplayView::kOverlay; break;
        }
    }
    display_.draw(cmd, gpu_->linear_target(), to_device(root_.viewer_rect(), scale), frame, view,
                  scale, viewer_background());
    const gpu::Rect panel = root_.panel_rect();
    nebula_.draw(cmd, gpu_->linear_target(), to_device(panel, scale), panel.w, panel.h,
                 root_.panel_width(), nebula_time_);
}

bool EditorApp::churning() const {
    const SDL_WindowFlags hidden = SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED;
    return !job_.running() && root_.panel_rect().w > 0.0f &&
           (SDL_GetWindowFlags(gpu_->window()) & hidden) == 0;
}

bool EditorApp::render() {
    if (rendering_) return true;
    rendering_ = true;

    const Uint64 now = SDL_GetTicksNS();
    // Clamped, so a long block on input does not land as one enormous step
    // that finishes every animation at once.
    const float dt = std::min(0.1f, static_cast<float>(now - last_tick_) / 1e9f);
    last_tick_ = now;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device());
    if (!cmd) {
        SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        rendering_ = false;
        return false;
    }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu_->window(), &swapchain, &sw, &sh)) {
        SDL_Log("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        rendering_ = false;
        return false;
    }
    // Null without an error when the window is minimized; nothing to draw.
    if (swapchain) {
        prepare(cmd, sw, sh, dt);
        draw_.upload(cmd);
        draw_.render_ui(cmd, sw, sh, /*clear=*/false);
        draw_.encode(cmd, swapchain);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    rendering_ = false;
    return true;
}

bool SDLCALL EditorApp::watch_events(void* self, SDL_Event* event) {
    auto* app = static_cast<EditorApp*>(self);
    if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1 && app->running_)
        app->render();
    return true;
}

void EditorApp::run() {
    running_ = true;
    while (running_) {
        SDL_Event event;
        // Block for input when nothing is moving; while playing, wake for the
        // next frame; while the nebula churns, wake often enough to move it;
        // while something animates, keep going.
        bool got = false;
        if (playing_) {
            const Uint64 now = SDL_GetTicksNS();
            Sint32 wait = next_due_ > now ? static_cast<Sint32>((next_due_ - now) / 1000000) : 0;
            if (churning()) wait = std::min(wait, kChurnIntervalMs);
            got = SDL_WaitEventTimeout(&event, wait);
        } else if (animating_) {
            got = SDL_PollEvent(&event);
        } else if (churning()) {
            got = SDL_WaitEventTimeout(&event, kChurnIntervalMs);
        } else if (job_.running()) {
            // Often enough to see each frame arrive.
            got = SDL_WaitEventTimeout(&event, 1000);
        } else {
            got = SDL_WaitEvent(&event);
        }
        if (got) {
            do handle(event);
            while (SDL_PollEvent(&event));
        }
        poll_job();
        decode_pending();
        if (!running_ || !render()) break;
    }
    running_ = false;
}

bool EditorApp::screenshot(const char* path) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(gpu_->window(), &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_->device());
    if (!cmd) return false;
    // Settle every animation first: what is wanted is the resting state.
    prepare(cmd, static_cast<Uint32>(pw), static_cast<Uint32>(ph), 0.0f);
    for (int i = 0; i < 100 && root_.tick(0.05f); ++i) {}
    // The first pass laid out against unsettled animations; this one draws
    // the settled state over the same target.
    int lw = 0, lh = 0;
    SDL_GetWindowSize(gpu_->window(), &lw, &lh);
    const float scale = static_cast<float>(pw) / std::max(1, lw);
    root_.layout(static_cast<float>(lw), static_cast<float>(lh));
    composite_backdrop(cmd, scale);
    SDL_SubmitGPUCommandBuffer(cmd);

    draw_.begin_frame(scale);
    root_.paint(draw_, ui::PaintContext{});
    return draw_.save_png(path, static_cast<Uint32>(pw), static_cast<Uint32>(ph),
                          /*clear_target=*/false);
}

}  // namespace ps::app
