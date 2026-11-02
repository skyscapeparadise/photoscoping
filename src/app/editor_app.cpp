#include "app/editor_app.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>

#include "app/file_menu.hpp"
#include "app/tracking_ticker.hpp"
#include "editor/theme.hpp"
#include "fixes/mask.hpp"
#include "media/export_format.hpp"
#include "matting/plate.hpp"
#include "light/light_map.hpp"

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

// A file's name without its directory or extension, which is what the
// inspector and the title call it.
std::string stem_of(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

bool is_lut(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".cube";
}

editor::ClipInfo clip_info(const std::string& name, const media::VideoDecoder& d,
                           const std::string& lut_path) {
    editor::ClipInfo info;
    info.name = name;
    char text[64];
    std::snprintf(text, sizeof text, "%d \xC3\x97 %d", d.width(), d.height());
    info.rows.emplace_back("size", text);
    std::snprintf(text, sizeof text, "%.3f", d.frame_rate());
    // 24.000 reads as a measurement; 24 reads as a frame rate.
    std::string rate = text;
    rate.erase(rate.find_last_not_of('0') + 1);
    if (rate.back() == '.') rate.pop_back();
    info.rows.emplace_back("rate", rate + " fps");
    info.rows.emplace_back("length", std::to_string(d.frame_count()) + " frames");
    info.rows.emplace_back("codec", d.codec_name());
    info.rows.emplace_back("decode", d.hardware_decoding() ? "videotoolbox" : "software");
    info.rows.emplace_back("input", lut_path.empty() ? "file tags" : stem_of(lut_path));
    // Filled in from what has been matted.
    info.rows.emplace_back("matte", "none");
    info.rows.emplace_back("re-matte", "none");
    info.rows.emplace_back("plate", "none");
    info.rows.emplace_back("export", "none");
    return info;
}

std::string time_left(double seconds) {
    if (seconds < 60.0) return "under a minute left";
    const int minutes = static_cast<int>(std::ceil(seconds / 60.0));
    if (minutes < 60) return std::to_string(minutes) + " min left";
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min left";
}

}  // namespace

EditorApp::~EditorApp() { destroy(); }

bool EditorApp::create(gpu::GpuContext& gpu, gpu::IngestPass& ingest, Options options) {
    gpu_ = &gpu;
    ingest_ = &ingest;
    options_ = std::move(options);

    SDL_SetWindowMinimumSize(gpu.window(), kMinWindowWidth, kMinWindowHeight);
    SDL_SetWindowTitle(gpu.window(), "photoscoping");

    if (!display_.create(gpu) || !nebula_.create(gpu) || !draw_.create(gpu)) return false;
    if (!fonts_.init(gpu.device(), resource_path("fonts/MonaSans-Regular.ttf").c_str(),
                     resource_path("fonts/MonaSans-Bold.ttf").c_str()))
        return false;
    if (!build_atlases()) return false;

    root_.build(fonts_, large_icons_, small_icons_, &fixes_);
    mask_.create(gpu.device());
    editor::GarbageTool* garbage = root_.garbage_tool();
    garbage->on_edit_begin = [this] { history_.record(fixes_); };
    garbage->on_live_change = [this] { fixes_changed(/*save=*/false); };
    garbage->on_changed = [this] { fixes_changed(/*save=*/true); };
    editor::PaintTool* paint = root_.paint_tool();
    paint->on_edit_begin = [this] { history_.record(fixes_); };
    paint->on_live_change = [this] { fixes_changed(/*save=*/false); };
    paint->on_changed = [this] { fixes_changed(/*save=*/true); };
    editor::RematteTool* rematte = root_.rematte_tool();
    rematte->on_edit_begin = [this] { history_.record(fixes_); };
    rematte->on_live_change = [this] { fixes_changed(/*save=*/false); };
    rematte->on_changed = [this] { fixes_changed(/*save=*/true); };
    rematte->is_made = [this](std::int64_t frame, const fixes::PixelRect& box) {
        return patches_.made(frame, box);
    };
    editor::LightTool* light = root_.light_tool();
    light->on_edit_begin = [this] { history_.record(fixes_); };
    light->on_live_change = [this] { fixes_changed(/*save=*/false); };
    light->on_changed = [this] { fixes_changed(/*save=*/true); };
    light->on_selection_changed = [this] { refresh_light_controls(); };
    editor::LightControls* controls = root_.inspector()->light_controls();
    controls->on_edit_begin = [this] { history_.record(fixes_); };
    controls->on_adjust = [this](const fixes::LightCrop& adjusted, bool done) {
        fixes::LightCrop* crop = fixes_.find_light(root_.light_tool()->selected());
        if (!crop) return;
        crop->hue = adjusted.hue;
        crop->saturation = adjusted.saturation;
        crop->lightness = adjusted.lightness;
        crop->colorize = adjusted.colorize;
        fixes_changed(done);
    };
    // The pill under the inspector is the matting run's, or the re-matte's
    // with that tool in hand.
    root_.on_tool_changed = [this](editor::Tool) {
        refresh_light_controls();
        refresh_matte_ui();
    };
    root_.inspector()->views()->on_view_changed = [this](editor::View view) {
        view_ = view;
        frames_.set_keep_coverage(wants_coverage());
    };
    root_.inspector()->on_action = [this] { toggle_job(); };
    job_.locate();

    editor::Timeline* timeline = root_.timeline();
    timeline->on_seek = [this](std::int64_t frame) { seek(frame); };
    timeline->on_play = [this](bool play) { set_playing(play); };
    timeline->on_stop = [this] {
        set_playing(false);
        target_ = 0;
    };
    timeline->on_step = [this](int delta) {
        if (!clip_.is_open()) return;
        set_playing(false);
        target_ = std::max<std::int64_t>(0, std::max<std::int64_t>(shown_, 0) + delta);
    };

    // A worker that has made a frame wakes the main loop, which may be
    // asleep on its events, to come and get it.
    open_event_ = SDL_RegisterEvents(3);
    const Uint32 frame_event = open_event_ + 1;
    export_event_ = open_event_ + 2;
    for (FrameSource* source : {static_cast<FrameSource*>(&clip_),
                                static_cast<FrameSource*>(&matte_),
                                static_cast<FrameSource*>(&subject_),
                                static_cast<FrameSource*>(&frames_),
                                static_cast<FrameSource*>(&patches_)}) {
        source->set_notify([frame_event] {
            SDL_Event event = {};
            event.type = frame_event;
            SDL_PushEvent(&event);
        });
    }

    subject_est_.create(gpu.device(), [frame_event] {
        SDL_Event event = {};
        event.type = frame_event;
        SDL_PushEvent(&event);
    });
    light_preview_.create(gpu.device(), [frame_event] {
        SDL_Event event = {};
        event.type = frame_event;
        SDL_PushEvent(&event);
    });

    if (options_.lut) {
        ingest_->set_lut(*options_.lut);
        clip_.set_lut(&*options_.lut);
    }
    if (!options_.clip.empty() && !open_clip(options_.clip, options_.frame)) return false;
    refresh_matte_ui();

    install_menu();
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
    server_.stop();
    clip_.close();
    frames_.destroy();
    patches_.destroy();
    subject_est_.destroy();
    light_preview_.destroy();
    matte_.destroy();
    subject_.destroy();
    mask_.destroy();
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

bool EditorApp::open_clip(const std::string& path, std::int64_t frame) {
    // A run belongs to the clip it was started for; its finished frames stay
    // on disk, and resume when that clip is open again.
    job_.stop();
    server_.stop();
    matte_.destroy();
    subject_.destroy();
    frames_.destroy();
    patches_.destroy();
    if (!clip_.open(path, !options_.software_decode, ingest_)) {
        SDL_Log("cannot open %s", path.c_str());
        refresh_matte_ui();
        return false;
    }
    clip_.set_lut(options_.lut ? &*options_.lut : nullptr);
    clip_name_ = stem_of(path);
    SDL_SetWindowTitle(gpu_->window(), ("photoscoping \xE2\x80\x94 " + clip_name_).c_str());
    root_.inspector()->set_clip(clip_info(clip_name_, clip_.decoder(), options_.lut_path));

    playing_ = false;
    shown_ = -1;
    target_ = std::clamp<std::int64_t>(frame, 0, std::max<std::int64_t>(0, clip_.frame_count() - 1));
    root_.timeline()->set_clip(clip_.frame_rate(), clip_.frame_count());
    root_.viewer()->set_image_size(clip_.decoder().width(), clip_.decoder().height());

    frames_.open(gpu_->device(), render_path("_mattes"), clip_.frame_count());
    patches_.open(gpu_->device(), render_path("_rematte"), clip_.decoder().width(),
                  clip_.decoder().height(), clip_.frame_count());
    ++subject_version_;
    choose_matte_source();

    std::string error;
    if (!fixes_.load(render_path("_fixes.json"), &error))
        SDL_Log("cannot read %s (%s); it is left as it is, and not written over",
                render_path("_fixes.json").c_str(), error.c_str());
    fixes_readable_ = error.empty();
    history_.clear();
    patches_.set_fixes(fixes_);
    fixes_changed(/*save=*/false);
    update_plate();
    refresh_matte_ui();
    SDL_Log("opened %s", path.c_str());
    return true;
}

bool EditorApp::open_lut(const std::string& path) {
    std::string error;
    std::optional<color::CubeLut> lut = color::load_cube(path, &error);
    if (!lut) {
        SDL_Log("cannot load %s: %s", path.c_str(), error.c_str());
        return false;
    }
    if (lut->full_range_in && !*lut->full_range_in) {
        SDL_Log("%s expects narrow-range input, which is not supported yet", path.c_str());
        return false;
    }
    if (!ingest_->set_lut(*lut)) return false;
    options_.lut = std::move(lut);
    options_.lut_path = path;
    // What the preview decodes as scene light may have changed with it.
    ++subject_version_;
    clip_.set_lut(&*options_.lut);
    root_.inspector()->set_row("input", stem_of(path));
    SDL_Log("LUT %s", path.c_str());
    return true;
}

void EditorApp::open_path(const std::string& path) {
    if (is_lut(path)) open_lut(path);
    else open_clip(path);
}

void EditorApp::install_menu() {
#if defined(SDL_PLATFORM_APPLE)
    if (!file_menu_)
        file_menu_ = install_file_menu(FileMenuActions{
            this,
            [](void* self) { static_cast<EditorApp*>(self)->show_open_dialog(); },
            [](void* self, int format) { static_cast<EditorApp*>(self)->show_export_dialog(format); },
            [](void* self) { return static_cast<EditorApp*>(self)->can_export(); },
            [](void* self, int action) { static_cast<EditorApp*>(self)->plate_action(action); },
            [](void* self) { return static_cast<EditorApp*>(self)->plate_state(); },
            [](void* self) { return static_cast<EditorApp*>(self)->can_export_lights(); },
            [](void* self, int option, int value) {
                static_cast<EditorApp*>(self)->light_option(option, value);
            },
            [](void* self, int option) { return static_cast<EditorApp*>(self)->light_value(option); },
            [](void* self) { static_cast<EditorApp*>(self)->matte_frame(); },
            [](void* self) { return static_cast<EditorApp*>(self)->can_matte_frame(); },
        });
#endif
}

void EditorApp::show_open_dialog() {
    static const SDL_DialogFileFilter kFilters[] = {
        {"Clips and LUTs", "mov;mp4;m4v;mxf;avi;mkv;crm;cube"},
        {"All files", "*"},
    };
    // The answer comes on whatever thread the dialog likes, so it is handed
    // to the main loop as an event rather than acted on here.
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
            if (!files || !files[0]) return;  // an error, or cancelled
            SDL_Event event = {};
            event.type = static_cast<EditorApp*>(self)->open_event_;
            event.user.data1 = SDL_strdup(files[0]);
            SDL_PushEvent(&event);
        },
        this, gpu_->window(), kFilters, 2, nullptr, /*allow_many=*/false);
}

bool EditorApp::can_matte_frame() const {
    if (!clip_.is_open() || !job_.available() || job_.running() || shown_ < 0) return false;
    if (server_.asked(shown_)) return false;
    // Nothing to do on a frame already matted -- and refined, with a plate.
    const bool refined = !fixes_.plate.set() ||
                         (shown_ < static_cast<std::int64_t>(frames_.refined().size()) &&
                          frames_.refined()[static_cast<std::size_t>(shown_)]);
    return !(frames_.matted(shown_) && refined);
}

void EditorApp::matte_frame() {
    if (!can_matte_frame()) return;
    // The process is started on the first frame asked for, and kept: the
    // models' warm-up is paid once, not for every frame.
    if (!server_.running() &&
        !server_.start(job_.app_path(), job_.models_path(), clip_.path(), options_.lut_path,
                       options_.render_dir, clip_name_))
        return;
    server_.request(shown_);
    set_playing(false);
    refresh_matte_ui();
}

void EditorApp::make_way() { server_.stop(); }

void EditorApp::poll_server() {
    if (!server_.running()) return;
    bool died = false;
    const std::vector<std::int64_t> finished = server_.poll(&died);
    if (!finished.empty()) {
        frames_.rescan();
        choose_matte_source();
        ++subject_version_;
        for (const std::int64_t f : finished) {
            if (f != shown_ || !frames_.matted(f)) continue;
            // The frame on show, matted: fetched now, in a view that shows
            // it, so the feedback asked for is there to see.
            frames_.refresh();
            if (view_ == editor::View::kFootage) {
                refresh_matte_ui();  // the views become available first
                root_.inspector()->views()->set_view(editor::View::kOverlay);
            }
        }
    }
    // Ten minutes with nothing asked of it, and its memory is worth more
    // than its warmth.
    if (server_.running() && !server_.busy() && server_.idle_seconds() > 600.0) {
        SDL_Log("the matting process has been idle ten minutes; stopping it");
        server_.stop();
    }
    if (!finished.empty() || died) refresh_matte_ui();
}

bool EditorApp::can_export_lights() const {
    // The matte says where the performer hides the backdrop, so the clip is
    // matted first, as for any export.
    return can_export() && !fixes_.lights.empty();
}

void EditorApp::light_option(int option, int value) {
    fixes::LightExport e = fixes_.light_export;
    switch (option) {
        case kLightLinear: e.linear = value != 0; break;
        case kLightFillShadows: e.fill_shadows = !e.fill_shadows; break;
        case kLightSize: e.size = std::max(0, value); break;
        default: return;
    }
    if (e == fixes_.light_export) return;
    history_.record(fixes_);
    fixes_.light_export = e;
    fixes_changed(/*save=*/true);
}

int EditorApp::light_value(int option) const {
    switch (option) {
        case kLightLinear: return fixes_.light_export.linear ? 1 : 0;
        case kLightFillShadows: return fixes_.light_export.fill_shadows ? 1 : 0;
        case kLightSize: return fixes_.light_export.size;
        default: return 0;
    }
}

void EditorApp::refresh_light_controls() {
    editor::LightTool* tool = root_.light_tool();
    tool->forget_missing();
    const fixes::LightCrop* crop = fixes_.find_light(tool->selected());
    std::string size;
    if (crop) {
        int w = 0, h = 0;
        light::output_size(crop->at(std::max<std::int64_t>(shown_, 0)), fixes_.light_export.size, &w, &h);
        size = std::to_string(w) + " \xC3\x97 " + std::to_string(h) +
               (fixes_.light_export.size > 0 ? " \xC2\xB7 long side set" : " \xC2\xB7 its own size");
    }
    root_.inspector()->light_controls()->bind(crop, size);
    if (root_.tool() == editor::Tool::kLight) root_.inspector()->set_heading(tool->heading());
}

bool EditorApp::can_export() const {
    // Every frame matted -- and refined by the plate, if there is one: an
    // export is of the matte as the viewer shows it.
    return clip_.is_open() && job_.available() && !job_.running() && clip_.frame_count() > 0 &&
           frames_.done_count() >= clip_.frame_count() &&
           (!fixes_.plate.set() || frames_.refined_count() >= clip_.frame_count());
}

void EditorApp::plate_action(int action) {
    if (!clip_.is_open()) return;
    const std::int64_t at = std::max<std::int64_t>(shown_, 0);
    fixes::PlateRange plate = fixes_.plate;
    switch (action) {
        case kPlateStart:
            if (!plate.set()) plate = fixes::PlateRange{at, at, false};
            plate.first = at;
            plate.last = std::max(plate.last, at);
            break;
        case kPlateEnd:
            if (!plate.set()) plate = fixes::PlateRange{at, at, false};
            plate.last = at;
            plate.first = std::min(plate.first, at);
            break;
        case kPlateClear: plate = fixes::PlateRange{}; break;
        case kPlateCorrect:
            if (!plate.set()) return;
            plate.correct = !plate.correct;
            break;
        default: return;
    }
    if (plate == fixes_.plate) return;
    history_.record(fixes_);
    fixes_.plate = plate;
    fixes_changed(/*save=*/true);
}

int EditorApp::plate_state() const {
    int state = 0;
    if (clip_.is_open()) state |= kPlateClipOpen;
    if (fixes_.plate.set()) state |= kPlateIsSet;
    if (fixes_.plate.correct) state |= kPlateCorrects;
    return state;
}

std::string EditorApp::plate_image_path() const {
    return (std::filesystem::path(options_.render_dir) / fixes::plate_image_name(clip_name_, fixes_.plate))
        .string();
}

bool EditorApp::plate_image_ready() const {
    std::error_code ec;
    return fixes_.plate.set() && std::filesystem::exists(plate_image_path(), ec);
}

void EditorApp::start_plate_job() {
    if (!clip_.is_open() || !fixes_.plate.set() || plate_image_ready()) {
        plate_wanted_ = false;
        return;
    }
    if (job_.running() || !job_.available()) {
        plate_wanted_ = job_.available();
        return;
    }
    plate_wanted_ = false;
    make_way();
    if (job_.start(MatteJob::Kind::kPlate, clip_.path(), options_.lut_path, options_.render_dir,
                   clip_name_))
        job_started_ns_ = SDL_GetTicksNS();
}

void EditorApp::update_plate() {
    const fixes::PlateRange& plate = fixes_.plate;
    // The refined mattes, where there are any; the stack's own elsewhere.
    frames_.set_preferred(plate.set() ? (std::filesystem::path(options_.render_dir) /
                                         fixes::plate_mattes_name(clip_name_, plate))
                                            .string()
                                      : std::string());
    root_.timeline()->set_plate(plate.set() ? plate.first : -1, plate.set() ? plate.last : -1);
    ++subject_version_;
    start_plate_job();
}

void EditorApp::show_export_dialog(int format_index) {
    if (!can_export() || format_index < 0 ||
        format_index >= static_cast<int>(std::size(media::kExportFormats)))
        return;
    const media::ExportFormatInfo& info = media::kExportFormats[format_index];
    if (info.kind == media::ExportKind::kLight) {
        // Light maps are a file (or folder) for each crop: asked for is the
        // folder they go in, starting beside the clip.
        if (!can_export_lights()) return;
        pending_export_format_ = format_index;
        static std::string start;
        start = std::filesystem::absolute(clip_.path()).parent_path().string() + "/";
        SDL_ShowOpenFolderDialog(
            [](void* self, const char* const* files, int) {
                if (!files || !files[0]) return;
                auto* app = static_cast<EditorApp*>(self);
                SDL_Event event = {};
                event.type = app->export_event_;
                event.user.data1 = SDL_strdup(files[0]);
                event.user.code = app->pending_export_format_;
                SDL_PushEvent(&event);
            },
            this, gpu_->window(), start.c_str(), /*allow_many=*/false);
        return;
    }
    if (!can_export()) return;
    // Suggested beside the clip, named for it and what it holds -- with a
    // space, not the underscore the app's own renders use, so the suggestion
    // is never one of them.
    const std::string name = clip_name_ + (info.subject() ? " subject" : " matte") + info.extension;
    const std::string where =
        (std::filesystem::absolute(clip_.path()).parent_path() / name).string();
    // One filter, the format's own; a PNG sequence is a folder, and has none.
    static SDL_DialogFileFilter filter;
    static std::string pattern;
    pattern = info.extension[0] ? info.extension + 1 : "";
    filter = {info.label, pattern.c_str()};
    pending_export_format_ = format_index;
    SDL_ShowSaveFileDialog(
        [](void* self, const char* const* files, int) {
            if (!files || !files[0]) return;  // an error, or cancelled
            auto* app = static_cast<EditorApp*>(self);
            SDL_Event event = {};
            event.type = app->export_event_;
            event.user.data1 = SDL_strdup(files[0]);
            event.user.code = app->pending_export_format_;
            SDL_PushEvent(&event);
        },
        this, gpu_->window(), pattern.empty() ? nullptr : &filter, pattern.empty() ? 0 : 1,
        where.c_str());
}

void EditorApp::start_export(const std::string& chosen, int format_index) {
    const media::ExportFormatInfo& info = media::kExportFormats[format_index];
    if (info.kind == media::ExportKind::kLight) {
        if (!can_export_lights()) return;
        make_way();
        if (job_.start(MatteJob::Kind::kLights, clip_.path(), options_.lut_path, options_.render_dir,
                       clip_name_, chosen, info.id)) {
            job_started_ns_ = SDL_GetTicksNS();
            export_to_ = std::to_string(fixes_.lights.size()) +
                         (fixes_.lights.size() == 1 ? " light map" : " light maps");
            export_status_ = "starting";
        }
        refresh_matte_ui();
        return;
    }
    if (!can_export()) return;
    // The format's extension, whatever was typed.
    std::filesystem::path path(chosen);
    if (info.extension[0]) path.replace_extension(info.extension);
    // Never over the app's own renders, or the clip itself: the save dialog
    // would ask before replacing them, but the answer would still lose them.
    std::error_code ec;
    for (const std::string& own : {render_path("_matte.mp4"), render_path("_comp.mp4"),
                                   render_path("_mattes"), render_path("_rematte"),
                                   render_path("_fixes.json"), clip_.path()}) {
        if (std::filesystem::equivalent(path, own, ec)) {
            SDL_Log("not exporting over %s, which the app reads", own.c_str());
            export_status_ = "not over the app's own files";
            refresh_matte_ui();
            return;
        }
    }
    make_way();
    if (job_.start(MatteJob::Kind::kExport, clip_.path(), options_.lut_path, options_.render_dir,
                   clip_name_, path.string(), info.id)) {
        job_started_ns_ = SDL_GetTicksNS();
        export_to_ = path.filename().string();
        export_status_ = "starting";
    }
    refresh_matte_ui();
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
    return (std::filesystem::path(options_.render_dir) / (clip_name_ + suffix)).string();
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
    if (!clip_.is_open()) {
        source_ = MatteSource::kNone;
        return;
    }
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

void EditorApp::refresh_matte_ui() {
    editor::Inspector* inspector = root_.inspector();
    const std::int64_t total = clip_.frame_count();
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
        (source_ == MatteSource::kVideos && subject_.is_open()) || source_ == MatteSource::kFrames);
    frames_.set_keep_coverage(wants_coverage());

    std::string row = "none";
    if (server_.busy()) {
        row = (server_.ready() ? "matting frame " : "warming up \xC2\xB7 frame ") +
              std::to_string(server_.current());
        if (server_.waiting() > 0) row += " \xC2\xB7 " + std::to_string(server_.waiting()) + " waiting";
    } else if (job_.running() && job_.kind() == MatteJob::Kind::kMatte && !complete) {
        row = std::to_string(done) + " of " + std::to_string(total);
        const double elapsed = (SDL_GetTicksNS() - job_started_ns_) / 1e9;
        const std::int64_t made = done - done_at_start_;
        // Until the first frame lands the run is loading the models -- on a
        // machine's first run, compiling them for Core ML, which is minutes.
        if (made > 0) row += " \xC2\xB7 " + time_left((total - done) * elapsed / made);
        else row = "loading the model";
    } else if (source_ == MatteSource::kVideos) {
        row = "rendered";
    } else if (source_ == MatteSource::kFrames) {
        row = complete ? std::to_string(done) + " frames" : std::to_string(done) + " of " +
                                                              std::to_string(total);
    }
    inspector->set_row("matte", row);

    const bool rematting = job_.running() && job_.kind() == MatteJob::Kind::kRematte;
    const std::size_t wanted = patches_.wanted(), made = patches_.made_count();
    std::string rematte_row = "none";
    if (rematting) {
        rematte_row = std::to_string(made) + " of " + std::to_string(wanted);
        const double elapsed = (SDL_GetTicksNS() - job_started_ns_) / 1e9;
        const std::size_t since = made - std::min(made, made_at_start_);
        if (since > 0)
            rematte_row += " \xC2\xB7 " + time_left((wanted - made) * elapsed / since);
        else
            rematte_row = "loading the model";
    } else if (wanted > 0) {
        rematte_row = made >= wanted ? std::to_string(made) + (made == 1 ? " patch" : " patches")
                                     : std::to_string(made) + " of " + std::to_string(wanted);
    }
    inspector->set_row("re-matte", rematte_row);

    const bool exporting = job_.running() && (job_.kind() == MatteJob::Kind::kExport ||
                                              job_.kind() == MatteJob::Kind::kLights);
    std::string export_row = export_status_;
    std::int64_t exported = 0, export_total = 0;
    if (exporting && job_.export_progress(&exported, &export_total) && exported > 0) {
        const double elapsed = (SDL_GetTicksNS() - job_started_ns_) / 1e9;
        export_row = std::to_string(exported) + " of " + std::to_string(export_total) +
                     " \xC2\xB7 " + time_left((export_total - exported) * elapsed / exported);
    }
    inspector->set_row("export", export_row);

    // The clean plate: its frames, and how far the mattes are refined by it.
    const fixes::PlateRange& plate = fixes_.plate;
    const std::int64_t refined = frames_.refined_count();
    std::string plate_row = "none";
    if (plate.set()) {
        plate_row = "frames " + std::to_string(plate.first) +
                    (plate.last > plate.first ? "\xE2\x80\x93" + std::to_string(plate.last) : "") +
                    (plate.correct ? " \xC2\xB7 override" : "");
        if (job_.running() && job_.kind() == MatteJob::Kind::kPlate) {
            plate_row = "making the plate";
        } else if (!plate_image_ready()) {
            plate_row += " \xC2\xB7 waiting";
        } else if (job_.running() && job_.kind() == MatteJob::Kind::kMatte && complete) {
            const double elapsed = (SDL_GetTicksNS() - job_started_ns_) / 1e9;
            const std::int64_t since = refined - refined_at_start_;
            plate_row = std::to_string(refined) + " of " + std::to_string(total) + " refined";
            if (since > 0) plate_row += " \xC2\xB7 " + time_left((total - refined) * elapsed / since);
        } else if (done > 0 && refined == 0) {
            plate_row += " \xC2\xB7 not refined yet";
        } else if (done > 0 && refined < done) {
            plate_row += " \xC2\xB7 " + std::to_string(refined) + " refined";
        }
    }
    inspector->set_row("plate", plate_row);
    const bool refine_wanted = plate.set() && complete && refined < total;

    const std::size_t to_make = patches_.frames_to_make();
    if (server_.busy()) {
        inspector->set_action("stop matting", true);
    } else if (exporting) {
        // The share done on the pill too: the card's rows are hidden while
        // the light tool's controls are in their place.
        std::string label = "stop exporting";
        if (exported > 0 && export_total > 0)
            label += " \xC2\xB7 " + std::to_string(exported * 100 / export_total) + "%";
        inspector->set_action(label, true);
    } else if (root_.tool() == editor::Tool::kRematte && !(job_.running() && !rematting)) {
        if (!clip_.is_open() || !job_.available())
            inspector->set_action("re-matte", false);
        else if (rematting)
            inspector->set_action("stop re-matting", true);
        else if (to_make > 0)
            inspector->set_action("re-matte " + std::to_string(to_make) +
                                      (to_make == 1 ? " frame" : " frames"),
                                  true);
        else
            inspector->set_action(wanted > 0 ? "re-matted" : "draw a box", false);
    } else if (!clip_.is_open() || !job_.available())
        inspector->set_action("matte clip", false);
    else if (rematting)
        inspector->set_action("stop re-matting", true);
    else if (job_.running() && job_.kind() == MatteJob::Kind::kPlate)
        inspector->set_action("making the plate", false);
    else if (job_.running())
        inspector->set_action(complete && plate.set() ? "stop refining" : "stop matting", true);
    else if (refine_wanted)
        inspector->set_action("refine " + std::to_string(total - refined) + " frames with the plate",
                              plate_image_ready());
    else if (done > 0 && !complete)
        inspector->set_action("matte the rest", true);
    else if (source_ != MatteSource::kNone)
        inspector->set_action("matted", false);
    else
        inspector->set_action("matte clip", true);
}

void EditorApp::toggle_job() {
    // Single frames being matted: the pill stops them.
    if (server_.busy()) {
        server_.stop();
        refresh_matte_ui();
        return;
    }
    if (!job_.running()) make_way();
    if (job_.running()) {
        // A stopped export leaves its .partial behind, never the file itself.
        if (job_.kind() == MatteJob::Kind::kExport || job_.kind() == MatteJob::Kind::kLights)
            export_status_ = "stopped";
        job_.stop();
    } else if (clip_.is_open() && root_.tool() == editor::Tool::kRematte) {
        if (job_.start(MatteJob::Kind::kRematte, clip_.path(), options_.lut_path,
                       options_.render_dir, clip_name_)) {
            job_started_ns_ = SDL_GetTicksNS();
            made_at_start_ = patches_.made_count();
        }
    } else if (clip_.is_open()) {
        // The videos are about to be replaced, and the PNGs are what will
        // be current, so stop reading the old ones now.
        matte_.destroy();
        subject_.destroy();
        if (job_.start(MatteJob::Kind::kMatte, clip_.path(), options_.lut_path,
                       options_.render_dir, clip_name_)) {
            job_started_ns_ = SDL_GetTicksNS();
            done_at_start_ = frames_.done_count();
            refined_at_start_ = frames_.refined_count();
        }
    }
    frames_.rescan();
    patches_.rescan();
    choose_matte_source();
    refresh_matte_ui();
}

void EditorApp::poll_job() {
    if (!job_.running()) return;
    const Uint64 now = SDL_GetTicksNS();
    if (now - last_poll_ns_ < 1000000000ull) return;
    last_poll_ns_ = now;
    // A matte that has just landed for the frame on screen is fetched, not
    // left waiting for the next time the frame changes.
    // Patches the same way: a rescan that finds new ones refreshes the frame
    // on screen if they are its.
    const bool rematting = job_.kind() == MatteJob::Kind::kRematte;
    const bool exporting = job_.kind() == MatteJob::Kind::kExport || job_.kind() == MatteJob::Kind::kLights;
    if ((rematting || exporting) && patches_.rescan()) ++subject_version_;
    else if (frames_.rescan() &&
             ((frames_.matted(shown_) && frames_.shown() != shown_) || frames_.shown_outdated(shown_))) {
        frames_.refresh();
        ++subject_version_;
    }
    bool succeeded = false;
    if (job_.poll_finished(&succeeded)) {
        if (rematting || exporting) patches_.rescan();
        if (exporting) export_status_ = succeeded ? export_to_ : "failed; see the log";
        const bool plating = job_.kind() == MatteJob::Kind::kPlate;
        if (!succeeded)
            SDL_Log("the %s stopped short; see %s",
                    exporting ? "export" : rematting ? "re-matte" : plating ? "plate" : "matting",
                    job_.log_path().c_str());
        // Finished, the new videos are in place and newer than the frames.
        choose_matte_source();
        if (plating) ++subject_version_;
        // A plate asked for while this ran is made now -- once: a plate run
        // that failed is not retried until the plate changes again.
        if (!plating && plate_wanted_) start_plate_job();
    }
    refresh_matte_ui();
}

void EditorApp::fixes_changed(bool save) {
    ++fixes_version_;
    root_.timeline()->set_keys(fixes_.key_frames());
    if (!save || !clip_.is_open()) return;
    // Which patches the boxes want; a moved box wants new ones.
    patches_.set_fixes(fixes_);
    ++subject_version_;
    update_plate();
    refresh_light_controls();
    refresh_matte_ui();
    // A file that would not read is never written over: whatever is in it
    // may be the only copy of someone's work.
    if (!fixes_readable_) return;
    std::string error;
    if (!fixes_.save(render_path("_fixes.json"), &error))
        SDL_Log("cannot save the fixes: %s", error.c_str());
}

void EditorApp::undo(bool redo) {
    if (redo ? history_.redo(fixes_) : history_.undo(fixes_)) fixes_changed(/*save=*/true);
}

void EditorApp::set_playing(bool playing) {
    if (!clip_.is_open()) return;
    playing_ = playing;
    next_due_ = SDL_GetTicksNS();
}

void EditorApp::seek(std::int64_t frame) {
    if (!clip_.is_open()) return;
    target_ = frame;
    // Scrubbing while playing carries on from wherever the head was put.
    next_due_ = SDL_GetTicksNS() + static_cast<Uint64>(1e9 / std::max(1.0, clip_.frame_rate()));
}

void EditorApp::handle(const SDL_Event& event) {
    if (event.type == export_event_) {
        char* path = static_cast<char*>(event.user.data1);
        if (path) start_export(path, event.user.code);
        SDL_free(path);
        return;
    }
    if (event.type == open_event_) {
        char* path = static_cast<char*>(event.user.data1);
        if (path) open_path(path);
        SDL_free(path);
        return;
    }
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            running_ = false;
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data) open_path(event.drop.data);
            break;
        case SDL_EVENT_KEY_UP:
            // Space is play and pause, and held it is also the pan: so it acts
            // on release, and only if the hold was not used to pan.
            if (event.key.key == SDLK_SPACE) {
                editor::Viewer* viewer = root_.viewer();
                viewer->set_space_held(false);
                if (!viewer->space_panned()) set_playing(!playing_);
            }
            break;
        case SDL_EVENT_PINCH_UPDATE: {
            editor::Viewer* viewer = root_.viewer();
            float px = 0.0f, py = 0.0f;
            SDL_GetMouseState(&px, &py);
            if (px >= viewer->abs_x() && px < viewer->abs_x() + viewer->width &&
                py >= viewer->abs_y() && py < viewer->abs_y() + viewer->height)
                viewer->zoom_at(event.pinch.scale, px, py);
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            const std::int64_t at = std::max<std::int64_t>(shown_, 0);
            if (root_.garbage_tool()->handle_key(event.key.key, event.key.mod)) break;
            if (root_.paint_tool()->handle_key(event.key.key, event.key.mod)) break;
            if (root_.rematte_tool()->handle_key(event.key.key, event.key.mod)) break;
            if (root_.light_tool()->handle_key(event.key.key, event.key.mod)) break;
            if ((event.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0 && event.key.key == SDLK_Z) {
                undo(/*redo=*/(event.key.mod & SDL_KMOD_SHIFT) != 0);
                break;
            }
            // Command (Control elsewhere) with these is the view's zoom, as
            // in every Mac app that has one.
            if ((event.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0) {
                editor::Viewer* viewer = root_.viewer();
                // The keypad's keys too, which arrive as keys of their own.
                switch (event.key.key) {
                    case SDLK_0:
                    case SDLK_KP_0: viewer->fit(); break;
                    case SDLK_1:
                    case SDLK_KP_1: viewer->actual_pixels(); break;
                    case SDLK_EQUALS:
                    case SDLK_PLUS:
                    case SDLK_KP_EQUALS:
                    case SDLK_KP_PLUS: viewer->zoom_by(2.0f); break;
                    case SDLK_MINUS:
                    case SDLK_KP_MINUS: viewer->zoom_by(0.5f); break;
                    default: break;
                }
                break;
            }
            switch (event.key.key) {
                case SDLK_SPACE:
                    if (!event.key.repeat) root_.viewer()->set_space_held(true);
                    break;
                case SDLK_RIGHT:
                    if (clip_.is_open()) { set_playing(false); target_ = at + 1; }
                    break;
                case SDLK_LEFT:
                    if (clip_.is_open()) {
                        set_playing(false);
                        target_ = std::max<std::int64_t>(0, at - 1);
                    }
                    break;
                case SDLK_HOME:
                    set_playing(false);
                    target_ = 0;
                    break;
                case SDLK_TAB:
                    if (!event.key.repeat) root_.toggle_panel();
                    break;
                case SDLK_M:
                    if (!event.key.repeat) matte_frame();
                    break;
                case SDLK_1:
                case SDLK_KP_1: root_.inspector()->views()->set_view(editor::View::kFootage); break;
                case SDLK_2:
                case SDLK_KP_2: root_.inspector()->views()->set_view(editor::View::kMatte); break;
                case SDLK_3:
                case SDLK_KP_3: root_.inspector()->views()->set_view(editor::View::kSubject); break;
                case SDLK_4:
                case SDLK_KP_4: root_.inspector()->views()->set_view(editor::View::kOverlay); break;
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

void EditorApp::advance(SDL_GPUCommandBuffer* cmd) {
    if (!clip_.is_open()) return;
    const double fps = clip_.frame_rate() > 0.0 ? clip_.frame_rate() : 24.0;
    const auto period = static_cast<Uint64>(1e9 / fps);

    // Playing, the next frame becomes the one wanted when it is due and the
    // last one made it to the screen. Fallen well behind -- a slow seek, a
    // stall -- the clock restarts from now rather than racing to catch up.
    const Uint64 now = SDL_GetTicksNS();
    if (playing_ && shown_ == target_ && now >= next_due_) {
        target_ = shown_ + 1;
        next_due_ = now > next_due_ + period ? now + period : next_due_ + period;
    }

    // Everything the view reads, asked for the same frame.
    FrameSource* needed[5];
    int count = 0;
    needed[count++] = &clip_;
    // The light tool reads the matte whatever the view: its preview fills in
    // behind the performer.
    const bool light_tool = root_.tool() == editor::Tool::kLight;
    if (view_ != editor::View::kFootage || light_tool) {
        if (source_ == MatteSource::kVideos) {
            needed[count++] = &matte_;
            if (view_ == editor::View::kSubject && subject_.is_open()) needed[count++] = &subject_;
        } else if (source_ == MatteSource::kFrames && frames_.matted(target_)) {
            needed[count++] = &frames_;
        }
        if (source_ != MatteSource::kNone && patches_.wants(target_)) needed[count++] = &patches_;
    }

    const std::int64_t next = playing_ ? target_ + 1 : -1;
    bool all_there = true;
    for (int i = 0; i < count; ++i) {
        FrameSource* s = needed[i];
        s->drop_stale(target_, next);
        if (s->has(target_)) continue;
        s->request(target_);
        // A render with no such frame is not waited for: the view shows the
        // footage alone there.
        if (s->failed() != target_) all_there = false;
    }
    if (clip_.failed() == target_) {
        // Past the end of the clip.
        playing_ = false;
        target_ = std::max<std::int64_t>(shown_, 0);
        return;
    }
    if (!all_there) return;

    for (int i = 0; i < count; ++i)
        if (needed[i]->shown() != target_ && needed[i]->ready() == target_) needed[i]->take(cmd);
    shown_ = target_;
    // Ahead of time: the next frame decodes while this one is on screen.
    if (playing_)
        for (int i = 0; i < count; ++i) needed[i]->request(target_ + 1);
}

bool EditorApp::wants_coverage() const {
    return estimating_subject() || (root_.tool() == editor::Tool::kLight && source_ == MatteSource::kFrames);
}

void EditorApp::request_light_preview() {
    const fixes::LightCrop* crop = fixes_.find_light(root_.light_tool()->selected());
    if (root_.tool() != editor::Tool::kLight || !crop || source_ != MatteSource::kFrames || playing_ ||
        shown_ < 0) {
        if (!crop) light_preview_.clear();
        return;
    }
    if (clip_.shown() != shown_ || frames_.shown() != shown_) return;
    if (patches_.wants(shown_) && patches_.shown() != shown_) return;

    // Everything the preview shows, as one string: asked again only when it
    // changes -- the frame, the matte, the crop's place or colour, the
    // settings.
    const std::vector<fixes::Point> corners = crop->at(shown_);
    char key[512];
    std::snprintf(key, sizeof key, "%lld %llu %d %d | %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | %.1f %.1f %.1f %d",
                  static_cast<long long>(shown_), static_cast<unsigned long long>(subject_version_),
                  fixes_.light_export.linear ? 1 : 0, fixes_.light_export.fill_shadows ? 1 : 0,
                  corners[0].x, corners[0].y, corners[1].x, corners[1].y, corners[2].x, corners[2].y,
                  corners[3].x, corners[3].y, crop->hue, crop->saturation, crop->lightness,
                  crop->colorize ? 1 : 0);
    if (light_preview_asked_ == key) return;

    LightPreview::Crop request;
    request.crop = *crop;
    request.crop.keys = {fixes::ShapeKey{shown_, corners}};
    request.linear = fixes_.light_export.linear;
    std::optional<LightPreview::Frame> frame;
    // Linear light and the look are different decodes of the frame: told
    // apart by the top bit of the version.
    const std::uint64_t frame_version =
        subject_version_ | (fixes_.light_export.linear ? (std::uint64_t{1} << 63) : 0);
    if (!light_preview_.has_frame(shown_, frame_version, fixes_.light_export.fill_shadows)) {
        // Uploaded before the tool wanted it kept: fetched again, keeping it.
        if (frames_.coverage().empty()) {
            frames_.set_keep_coverage(true);
            frames_.refresh();
            return;
        }
        if (frames_.width() != static_cast<int>(ingest_->width()) ||
            frames_.height() != static_cast<int>(ingest_->height()))
            return;
        LightPreview::Frame in;
        in.frame = shown_;
        in.version = frame_version;
        in.fill_shadows = fixes_.light_export.fill_shadows;
        in.width = frames_.width();
        in.height = frames_.height();
        // Read back before this frame's work is recorded, as the subject's
        // estimate does: the frame on screen is already on the GPU. Linear
        // light is the frame decoded again from its planes as scene light --
        // from the camera's log, when the LUT says what that is.
        if (fixes_.light_export.linear) {
            SDL_GPUCommandBuffer* again = SDL_AcquireGPUCommandBuffer(gpu_->device());
            const bool ok = again && ingest_->process_again(
                                         again, gpu::scene_transform_for(clip_.decoder().signal(),
                                                                          options_.lut ? &*options_.lut : nullptr));
            if (again) SDL_SubmitGPUCommandBuffer(again);
            if (!ok) return;
            in.working = ingest_->read_scene_raw();
        } else {
            in.working = ingest_->read_working_raw();
        }
        in.coverage = frames_.coverage();
        if (patches_.shown() == shown_ && patches_.texture()) {
            in.patches = patches_.pixels();
            in.patch_rect = patches_.rect();
        }
        in.fixes = fixes_;
        if (plate_image_ready()) in.plate_path = plate_image_path();
        frame = std::move(in);
    }
    light_preview_.request(std::move(frame), std::move(request));
    light_preview_asked_ = key;
}

bool EditorApp::estimating_subject() const {
    return view_ == editor::View::kSubject && source_ == MatteSource::kFrames;
}

void EditorApp::request_subject() {
    if (!estimating_subject() || playing_ || shown_ < 0) return;
    if (clip_.shown() != shown_ || frames_.shown() != shown_) return;
    if (patches_.wants(shown_) && patches_.shown() != shown_) return;
    if (subject_est_.asked(shown_, subject_version_)) return;
    // Uploaded before the view wanted it kept: fetched again, keeping it.
    if (frames_.coverage().empty()) {
        frames_.set_keep_coverage(true);
        frames_.refresh();
        return;
    }
    if (frames_.width() != static_cast<int>(ingest_->width()) ||
        frames_.height() != static_cast<int>(ingest_->height()))
        return;

    // The working image is read back here, before this frame's work is
    // recorded: the frame on screen was ingested by an earlier command
    // buffer, already submitted, so the copy sees it.
    SubjectEstimate::Inputs in;
    in.frame = shown_;
    in.version = subject_version_;
    in.width = frames_.width();
    in.height = frames_.height();
    in.working = ingest_->read_working_raw();
    in.coverage = frames_.coverage();
    if (patches_.shown() == shown_ && patches_.texture()) {
        in.patches = patches_.pixels();
        in.patch_rect = patches_.rect();
    }
    in.fixes = fixes_;
    if (plate_image_ready()) in.plate_path = plate_image_path();
    subject_est_.request(std::move(in));
}

void EditorApp::prepare(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height, float dt) {
    request_subject();
    request_light_preview();
    if (light_preview_.take(cmd) || !fixes_.find_light(root_.light_tool()->selected())) {
        root_.inspector()->light_controls()->set_preview(
            fixes_.find_light(root_.light_tool()->selected()) ? light_preview_.texture() : nullptr,
            light_preview_.aspect(),
            [&] {
                if (!fixes_.light_export.linear) return std::string("preview \xC2\xB7 the graded look");
                bool from_log = false;
                const gpu::InputTransform scene = gpu::scene_transform_for(
                    clip_.decoder().signal(), options_.lut ? &*options_.lut : nullptr, &from_log);
                return std::string("preview \xC2\xB7 linear light") +
                       (from_log ? std::string(" from ") + color::transfer_name(scene.transfer) : std::string()) +
                       ", this frame's exposure";
            }());
    }
    advance(cmd);
    root_.timeline()->set_frame(std::max<std::int64_t>(shown_, 0));
    root_.timeline()->set_playing(playing_);
    gpu_->resize_if_needed(width, height);

    int lw = 0, lh = 0;
    SDL_GetWindowSize(gpu_->window(), &lw, &lh);
    const float scale = lw > 0 ? static_cast<float>(width) / lw : 1.0f;

    root_.viewer()->set_device_scale(scale);
    root_.garbage_tool()->set_frame(std::max<std::int64_t>(shown_, 0));
    root_.paint_tool()->set_frame(std::max<std::int64_t>(shown_, 0));
    root_.rematte_tool()->set_frame(std::max<std::int64_t>(shown_, 0));
    root_.light_tool()->set_frame(std::max<std::int64_t>(shown_, 0));
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
    if (clip_.is_open() && clip_.shown() >= 0 && ingest_->working()) {
        frame.working = ingest_->working();
        frame.width = ingest_->width();
        frame.height = ingest_->height();
        // The fixes' mask, made again only when they or the frame have
        // changed, and only for a view that uses the matte. None live on
        // this frame -- strokes come and go -- is no mask at all.
        if (!fixes_.empty() && view_ != editor::View::kFootage) {
            if (mask_version_ != fixes_version_ || mask_frame_ != shown_) {
                const int mw = std::max(1, static_cast<int>(frame.width) / 2);
                const int mh = std::max(1, static_cast<int>(frame.height) / 2);
                const float sx = static_cast<float>(mw) / static_cast<float>(frame.width);
                const std::vector<std::uint8_t> mask = fixes::fixes_mask(fixes_, shown_, mw, mh, sx);
                mask_live_ = !mask.empty() && mask_.upload(cmd, mask, mw, mh);
                mask_version_ = fixes_version_;
                mask_frame_ = shown_;
            }
            if (mask_live_) frame.fixes = mask_.texture();
        }
        // Only renders of the frame on screen: one still catching up after a
        // change of view is left out, and the footage shows alone meanwhile.
        if (source_ == MatteSource::kFrames && frames_.shown() == shown_) {
            frame.matte = frames_.texture();
        } else if (source_ == MatteSource::kVideos) {
            if (matte_.shown() == shown_) frame.matte = matte_.working();
            if (subject_.shown() == shown_) frame.subject = subject_.working();
        }
        if (patches_.shown() == shown_ && patches_.texture()) {
            frame.rematte = patches_.texture();
            patches_.rect_uv(frame.rematte_rect);
        }
        // The estimate of this frame -- even one made before the latest
        // change, which only the colour of a fix's edge can tell apart --
        // or the footage until there is one.
        if (estimating_subject()) {
            subject_est_.take(cmd);
            if (subject_est_.shows_frame(shown_)) {
                frame.subject = subject_est_.texture();
                frame.subject_mode = gpu::SubjectMode::kStraight;
            } else {
                frame.subject = nullptr;
                frame.subject_mode = gpu::SubjectMode::kFootage;
            }
        }
        switch (view_) {
            case editor::View::kFootage: view = gpu::DisplayView::kFootage; break;
            case editor::View::kMatte: view = gpu::DisplayView::kMatte; break;
            case editor::View::kSubject: view = gpu::DisplayView::kSubject; break;
            case editor::View::kOverlay: view = gpu::DisplayView::kOverlay; break;
        }
    }
    editor::Viewer* viewer = root_.viewer();
    display_.draw(cmd, gpu_->linear_target(), to_device(root_.viewer_rect(), scale),
                  to_device(viewer->image_rect(), scale), frame, view, scale,
                  viewer->magnified(), viewer_background());
    const gpu::Rect panel = root_.panel_rect();
    nebula_.draw(cmd, gpu_->linear_target(), to_device(panel, scale), panel.w, panel.h,
                 root_.panel_width(), nebula_time_);
}

bool EditorApp::churning() const {
    const SDL_WindowFlags hidden = SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED;
    return root_.panel_rect().w > 0.0f && (SDL_GetWindowFlags(gpu_->window()) & hidden) == 0;
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

void EditorApp::tracking_tick(void* self) {
    auto* app = static_cast<EditorApp*>(self);
    if (!app->running_) return;
    app->poll_job();
    app->poll_server();
    app->render();
}

void EditorApp::run() {
    running_ = true;
#if defined(SDL_PLATFORM_APPLE)
    start_tracking_ticker(&EditorApp::tracking_tick, this);
#endif
    while (running_) {
        SDL_Event event;
        // Block for input when nothing is moving. Playing, wake for the next
        // frame; churning, often enough to move the nebula; animating, keep
        // going. A frame finished on a worker wakes the loop with an event of
        // its own, so waiting on one needs no timeout.
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
        } else if (job_.running() || server_.running()) {
            // Often enough to see each frame arrive -- and, with the matting
            // process idle, to notice when it has been so long enough.
            got = SDL_WaitEventTimeout(&event, server_.busy() ? 250 : 1000);
        } else {
            got = SDL_WaitEvent(&event);
        }
        if (got) {
            do handle(event);
            while (SDL_PollEvent(&event));
        }
        install_menu();
        poll_job();
        poll_server();
        if (!running_ || !render()) break;
    }
#if defined(SDL_PLATFORM_APPLE)
    stop_tracking_ticker();
#endif
    running_ = false;
}

bool EditorApp::screenshot(const char* path) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(gpu_->window(), &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;

    // Everything the view reads, decoded before the first pass: a screenshot
    // is of the frame asked for, not of whatever the workers had got to.
    if (clip_.is_open()) {
        clip_.wait(target_);
        if (view_ != editor::View::kFootage || root_.tool() == editor::Tool::kLight) {
            if (source_ == MatteSource::kVideos) {
                matte_.wait(target_);
                if (view_ == editor::View::kSubject) subject_.wait(target_);
            } else if (source_ == MatteSource::kFrames && frames_.matted(target_)) {
                frames_.wait(target_);
            }
            if (source_ != MatteSource::kNone && patches_.wants(target_)) patches_.wait(target_);
        }
    }

    // The subject estimate reads back the frame on screen, so it needs a pass
    // that puts it there first; then it is made, and waited for.
    if (estimating_subject()) {
        SDL_GPUCommandBuffer* first = SDL_AcquireGPUCommandBuffer(gpu_->device());
        if (!first) return false;
        prepare(first, static_cast<Uint32>(pw), static_cast<Uint32>(ph), 0.0f);
        SDL_SubmitGPUCommandBuffer(first);
        request_subject();
        // The matte may have been fetched again to keep its coverage.
        if (frames_.shown() != shown_ && frames_.wait(target_)) {
            SDL_GPUCommandBuffer* again = SDL_AcquireGPUCommandBuffer(gpu_->device());
            if (!again) return false;
            prepare(again, static_cast<Uint32>(pw), static_cast<Uint32>(ph), 0.0f);
            SDL_SubmitGPUCommandBuffer(again);
            request_subject();
        }
        subject_est_.wait();
    }

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
