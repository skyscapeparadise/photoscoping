#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The fixes made by hand to a clip's matte, kept apart from the matte itself.
//
// The stack's mattes are written once and never touched; fixes are applied
// on top of them -- live in the viewer, and when the matte leaves the app --
// so changing a fix never means matting again. Every fix is keyframed across
// the clip: set at the frames that need it, interpolated between, held
// before the first key and after the last.
namespace ps::fixes {

// In the clip's image pixels.
struct Point {
    float x = 0.0f;
    float y = 0.0f;
    friend bool operator==(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }
};

struct ShapeKey {
    std::int64_t frame = 0;
    std::vector<Point> points;
};

// Points keyed across the clip: what garbage mattes and paint strokes have
// in common. Every key has the same number of points, so they tween point
// for point.
struct Track {
    int id = 0;
    std::vector<ShapeKey> keys;  // ascending by frame, never empty

    // The points at `frame`: a key's own, or the two around it mixed
    // linearly, or the nearest key held past either end.
    std::vector<Point> at(std::int64_t frame) const;
    bool keyed_at(std::int64_t frame) const;
    // Sets the points at `frame`, adding a key there if there is none.
    void set(std::int64_t frame, std::vector<Point> points);
    // Removes the key at `frame`. False if there is none there, or it is
    // the last key, which removing would leave the track with nothing.
    bool remove_key(std::int64_t frame);
    // A point added between point `after` and the next, `t` of the way
    // along, in every key -- so the track still tweens point for point, and
    // the new point rides along the edge it was put on.
    void insert_point(std::size_t after, float t);
    // Point `index` taken out of every key. False if that would leave fewer
    // than `minimum`.
    bool remove_point(std::size_t index, std::size_t minimum);
    std::size_t point_count() const { return keys.empty() ? 0 : keys.front().points.size(); }
    std::int64_t first_frame() const { return keys.front().frame; }
    std::int64_t last_frame() const { return keys.back().frame; }
    // From the first key to the last: where a fix that does not hold past
    // its ends is there at all.
    bool live_at(std::int64_t frame) const {
        return !keys.empty() && frame >= first_frame() && frame <= last_frame();
    }
};

// A garbage matte: a closed polygon the subject is always inside. Outside
// it, the matte is forced to background. It holds before its first key and
// after its last: where the subject can be does not stop being true.
//
// `feather` softens its edge outward, in image pixels: the matte fades over
// that distance outside the shape rather than stopping at its edge, so what
// is just outside -- a stray hair the shape did not quite take in -- is
// eased out rather than cut. The shape's own edge stays whole.
struct Garbage : Track {
    float feather = 0.0f;
};

// Which way a stroke paints the matte.
enum class PaintMode { kOut, kIn };

// A paint stroke: a line of points with a radius, painting coverage out of
// the matte or back into it. Unlike a garbage matte it is there only from
// its first key to its last -- a stroke fixes the frames it was meant for,
// and one painted on a single frame fixes that frame -- and tweens between.
struct Stroke : Track {
    PaintMode mode = PaintMode::kOut;
    float radius = 20.0f;  // image pixels
    // Edge softness: the outer part of the radius that fades, 0..1.
    float softness = 0.5f;
};

// A local re-matte: a box the stack looks at again, closer, on every frame
// from its first key to its last. Two points, the box's opposite corners,
// in either order. What the closer look makes is kept beside the renders
// (fixes/rematte.hpp) and blended over the matte inside the box.
//
// Every key's corners are kept smaller first -- top left, then bottom right --
// so they tween corner to corner; set() takes them that way.
struct Rematte : Track {
    void normalize();
};

// A light map's crop: four corners on the backdrop -- top left, top right,
// bottom right, bottom left -- whose light, straightened to a rectangle, is
// exported frame by frame as an emissive texture (light/light_map.hpp). Held
// before its first key and after its last, like a garbage matte: a crop is a
// place, and a locked-off shot keeps it there.
//
// Its colour can be adjusted as Photoshop's Hue/Saturation adjusts it: hue
// -180..180, saturation and lightness -100..100; colorized, hue is 0..360
// and saturation 0..100.
struct LightCrop : Track {
    std::string name;
    float hue = 0.0f;
    float saturation = 0.0f;
    float lightness = 0.0f;
    bool colorize = false;
    bool adjusted() const { return colorize || hue != 0.0f || saturation != 0.0f || lightness != 0.0f; }
};

// How the clip's light maps are exported, whatever the format.
struct LightExport {
    // The light itself, linear and unclipped, before any grade -- or the
    // look, as the viewer shows it.
    bool linear = true;
    // Fill in the performer's shadow on the backdrop too, not only the
    // performer.
    bool fill_shadows = false;
    // The long side in pixels; 0 for the crop's own size.
    int size = 0;
    friend bool operator==(const LightExport& a, const LightExport& b) {
        return a.linear == b.linear && a.fill_shadows == b.fill_shadows && a.size == b.size;
    }
};

// A clean plate (matting/plate.hpp): the frames of the clip where the set is
// empty, from first to last, averaged. Optional. By default it refines edges
// and colour only; with `correct` it also overrules the model where the matte
// and the plate clearly disagree.
struct PlateRange {
    std::int64_t first = -1;
    std::int64_t last = -1;
    bool correct = false;
    bool set() const { return first >= 0 && last >= first; }
    friend bool operator==(const PlateRange& a, const PlateRange& b) {
        return a.first == b.first && a.last == b.last && a.correct == b.correct;
    }
};

// Named for the frames they came from -- and the mattes for the mode too --
// so a plate changed is a plate new, never one half made from the old.
//   <clip>_plate_<first>_<last>.png
//   <clip>_mattes_plate_<first>_<last>[_correct]/
std::string plate_image_name(const std::string& clip, const PlateRange& plate);
std::string plate_mattes_name(const std::string& clip, const PlateRange& plate);
// The same plate as scene light -- decoded from the camera's log, for light
// maps -- which runs past white, so in float:
//   <clip>_plate_<first>_<last>_scene.exr
std::string plate_scene_image_name(const std::string& clip, const PlateRange& plate);
// The frames the plate is averaged from: all of them, or `limit` spread
// evenly across.
std::vector<std::int64_t> plate_frames(const PlateRange& plate, int limit = 16);

struct Fixes {
    std::vector<Garbage> garbage;
    // In the order they were painted, which is the order they apply in.
    std::vector<Stroke> strokes;
    std::vector<Rematte> rematte;
    PlateRange plate;
    std::vector<LightCrop> lights;
    LightExport light_export;
    int next_id = 1;

    Garbage* find(int id);
    const Garbage* find(int id) const;
    Stroke* find_stroke(int id);
    const Stroke* find_stroke(int id) const;
    // A new shape with one key, at `frame`. Returns its id.
    int add_garbage(std::int64_t frame, std::vector<Point> points);
    bool remove_garbage(int id);
    // A new stroke with one key, at `frame`. Returns its id.
    int add_stroke(std::int64_t frame, PaintMode mode, float radius, std::vector<Point> points);
    bool remove_stroke(int id);
    Rematte* find_rematte(int id);
    const Rematte* find_rematte(int id) const;
    // A new box with one key, at `frame`. Returns its id.
    int add_rematte(std::int64_t frame, Point a, Point b);
    bool remove_rematte(int id);
    LightCrop* find_light(int id);
    const LightCrop* find_light(int id) const;
    // A new crop with one key, at `frame`, named "light N" for the lowest N
    // not taken. Returns its id.
    int add_light(std::int64_t frame, std::vector<Point> corners);
    bool remove_light(int id);
    bool empty() const { return garbage.empty() && strokes.empty() && rematte.empty(); }
    // Every frame any fix has a key on, ascending, once each.
    std::vector<std::int64_t> key_frames() const;

    // As JSON. A missing file loads as no fixes; a file that is there and
    // unreadable is an error, so it is never silently written over.
    bool load(const std::string& path, std::string* error);
    bool save(const std::string& path, std::string* error) const;
};

// Undo and redo, by whole copies: fixes are small, and a copy cannot get
// out of step with what it copied the way a list of inverse operations can.
class History {
public:
    // Call before an edit, with the fixes as they are about to stop being.
    void record(const Fixes& before);
    bool undo(Fixes& fixes);
    bool redo(Fixes& fixes);
    void clear();

private:
    std::vector<Fixes> undo_;
    std::vector<Fixes> redo_;
};

}  // namespace ps::fixes
