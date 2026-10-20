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

// A garbage matte: a closed polygon the subject is always inside. Outside
// it, the matte is forced to background. Every key has the same number of
// points, so a shape tweens point for point.
struct Garbage {
    int id = 0;
    std::vector<ShapeKey> keys;  // ascending by frame, never empty

    // The shape at `frame`: a key's own, or the two around it mixed
    // linearly, or the nearest key held past either end.
    std::vector<Point> at(std::int64_t frame) const;
    bool keyed_at(std::int64_t frame) const;
    // Sets the shape at `frame`, adding a key there if there is none.
    void set(std::int64_t frame, std::vector<Point> points);
    // Removes the key at `frame`. False if there is none there, or it is
    // the last key, which removing would leave the shape without a shape.
    bool remove_key(std::int64_t frame);
    std::size_t point_count() const { return keys.empty() ? 0 : keys.front().points.size(); }
};

struct Fixes {
    std::vector<Garbage> garbage;
    int next_id = 1;

    Garbage* find(int id);
    const Garbage* find(int id) const;
    // A new shape with one key, at `frame`. Returns its id.
    int add_garbage(std::int64_t frame, std::vector<Point> points);
    bool remove_garbage(int id);
    bool empty() const { return garbage.empty(); }
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
