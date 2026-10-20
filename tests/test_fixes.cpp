// Fixes: keyframing, the file they are kept in, undo, and the garbage mask.
// Headless: no window or GPU.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "fixes/fixes.hpp"
#include "fixes/mask.hpp"

using namespace ps::fixes;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

std::vector<Point> square(float x, float y, float size) {
    return {{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}};
}

void test_keyframes() {
    Fixes f;
    const int id = f.add_garbage(10, square(0, 0, 100));
    Garbage& g = *f.find(id);
    g.set(20, square(100, 0, 100));

    // Held before the first key and after the last.
    CHECK(g.at(0)[0] == (Point{0, 0}));
    CHECK(g.at(99)[0] == (Point{100, 0}));
    // Halfway between, halfway along.
    CHECK(near(g.at(15)[0].x, 50.0f, 1e-4f));
    CHECK(near(g.at(15)[2].x, 150.0f, 1e-4f));
    CHECK(g.keyed_at(10) && g.keyed_at(20) && !g.keyed_at(15));

    // A key in the middle, kept in order.
    g.set(12, square(0, 50, 100));
    CHECK(g.keys.size() == 3 && g.keys[1].frame == 12);
    CHECK(near(g.at(12)[0].y, 50.0f, 1e-6f));
    // Setting where there is a key replaces it.
    g.set(12, square(0, 60, 100));
    CHECK(g.keys.size() == 3 && near(g.at(12)[0].y, 60.0f, 1e-6f));

    CHECK(g.remove_key(12) && g.keys.size() == 2);
    CHECK(!g.remove_key(12));
    CHECK(g.remove_key(20));
    CHECK(!g.remove_key(10));  // the last key stays: a shape needs one

    f.add_garbage(30, square(0, 0, 10));
    const auto frames = f.key_frames();
    CHECK(frames.size() == 2 && frames[0] == 10 && frames[1] == 30);
    CHECK(f.remove_garbage(id) && f.garbage.size() == 1 && !f.find(id));
}

void test_file() {
    const std::string path =
        (std::filesystem::temp_directory_path() / "photoscoping_test_fixes.json").string();
    std::filesystem::remove(path);

    Fixes missing;
    std::string error;
    CHECK(missing.load(path, &error) && missing.empty());

    Fixes f;
    const int id = f.add_garbage(3, square(1.5f, 2.25f, 40));
    f.find(id)->set(9, square(10, 20, 40));
    f.add_garbage(5, {{0, 0}, {10, 0}, {5, 8}});
    CHECK(f.save(path, &error));

    Fixes back;
    CHECK(back.load(path, &error));
    CHECK(back.garbage.size() == 2 && back.next_id == f.next_id);
    CHECK(back.find(id) && back.find(id)->keys.size() == 2);
    CHECK(back.find(id)->at(3)[0] == (Point{1.5f, 2.25f}));
    CHECK(near(back.find(id)->at(6)[0].x, (1.5f + 10.0f) / 2.0f, 1e-4f));

    // Unreadable is an error, not an empty start that would then be saved over.
    { std::FILE* bad = std::fopen(path.c_str(), "w"); std::fputs("{ nope", bad); std::fclose(bad); }
    Fixes broken;
    CHECK(!broken.load(path, &error));
    std::filesystem::remove(path);
}

void test_history() {
    Fixes f;
    History h;
    h.record(f);
    f.add_garbage(0, square(0, 0, 1));
    h.record(f);
    f.add_garbage(0, square(5, 5, 1));
    CHECK(f.garbage.size() == 2);
    CHECK(h.undo(f) && f.garbage.size() == 1);
    CHECK(h.undo(f) && f.garbage.empty());
    CHECK(!h.undo(f));
    CHECK(h.redo(f) && f.garbage.size() == 1);
    // A new edit after an undo drops what could have been redone.
    h.record(f);
    f.add_garbage(0, square(9, 9, 1));
    CHECK(!h.redo(f));
}

void test_mask() {
    Fixes none;
    CHECK(garbage_mask(none, 0, 16, 16, 1.0f).empty());

    Fixes f;
    f.add_garbage(0, square(4, 4, 8));  // pixels 4..11 inside, exactly
    auto m = garbage_mask(f, 0, 16, 16, 1.0f);
    CHECK(m.size() == 256);
    CHECK(m[8 * 16 + 8] == 255 && m[4 * 16 + 4] == 255 && m[11 * 16 + 11] == 255);
    CHECK(m[0] == 0 && m[3 * 16 + 8] == 0 && m[12 * 16 + 8] == 0 && m[8 * 16 + 12] == 0);

    // An edge through the middle of a column covers it by half.
    Fixes half;
    half.add_garbage(0, {{0, 0}, {4.5f, 0}, {4.5f, 16}, {0, 16}});
    m = garbage_mask(half, 0, 16, 16, 1.0f);
    CHECK(m[8 * 16 + 3] == 255 && m[8 * 16 + 5] == 0);
    CHECK(m[8 * 16 + 4] > 120 && m[8 * 16 + 4] < 136);

    // Scaled: the same square at half size.
    m = garbage_mask(f, 0, 8, 8, 0.5f);
    CHECK(m[4 * 8 + 4] == 255 && m[0] == 0);

    // Two shapes are their union.
    f.add_garbage(0, square(0, 0, 2));
    m = garbage_mask(f, 0, 16, 16, 1.0f);
    CHECK(m[1 * 16 + 1] == 255 && m[8 * 16 + 8] == 255 && m[2 * 16 + 8] == 0);
}

}  // namespace

int main() {
    test_keyframes();
    test_file();
    test_history();
    test_mask();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("fixes: all checks passed\n");
    return 0;
}
