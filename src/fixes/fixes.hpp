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
struct Garbage : Track {};

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

struct Fixes {
    std::vector<Garbage> garbage;
    // In the order they were painted, which is the order they apply in.
    std::vector<Stroke> strokes;
    std::vector<Rematte> rematte;
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
