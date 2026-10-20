#include "fixes/fixes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ps::fixes {

namespace {

constexpr int kFormatVersion = 1;
constexpr std::size_t kHistoryDepth = 200;

// A track's keys from JSON, sorted, or false if they cannot tween: none, too
// few points, or keys that disagree on how many there are.
bool read_keys(const nlohmann::json& track, std::size_t min_points, std::vector<ShapeKey>* keys) {
    for (const auto& k : track.value("keys", nlohmann::json::array())) {
        ShapeKey key;
        key.frame = k.value("frame", std::int64_t{0});
        for (const auto& p : k.value("points", nlohmann::json::array()))
            if (p.is_array() && p.size() == 2) key.points.push_back({p[0].get<float>(), p[1].get<float>()});
        keys->push_back(std::move(key));
    }
    std::sort(keys->begin(), keys->end(),
              [](const ShapeKey& a, const ShapeKey& b) { return a.frame < b.frame; });
    return !keys->empty() && keys->front().points.size() >= min_points &&
           std::all_of(keys->begin(), keys->end(), [&](const ShapeKey& k) {
               return k.points.size() == keys->front().points.size();
           });
}

nlohmann::json write_keys(const std::vector<ShapeKey>& keys) {
    nlohmann::json out = nlohmann::json::array();
    for (const ShapeKey& k : keys) {
        nlohmann::json points = nlohmann::json::array();
        for (const Point& p : k.points) points.push_back({p.x, p.y});
        out.push_back({{"frame", k.frame}, {"points", points}});
    }
    return out;
}

}  // namespace

std::vector<Point> Track::at(std::int64_t frame) const {
    if (keys.empty()) return {};
    if (frame <= keys.front().frame) return keys.front().points;
    if (frame >= keys.back().frame) return keys.back().points;
    const auto after = std::upper_bound(
        keys.begin(), keys.end(), frame,
        [](std::int64_t f, const ShapeKey& k) { return f < k.frame; });
    const ShapeKey& b = *after;
    const ShapeKey& a = *(after - 1);
    if (a.frame == frame) return a.points;
    const float t = static_cast<float>(frame - a.frame) / static_cast<float>(b.frame - a.frame);
    std::vector<Point> out(a.points.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = {a.points[i].x + (b.points[i].x - a.points[i].x) * t,
                  a.points[i].y + (b.points[i].y - a.points[i].y) * t};
    return out;
}

bool Track::keyed_at(std::int64_t frame) const {
    return std::any_of(keys.begin(), keys.end(), [&](const ShapeKey& k) { return k.frame == frame; });
}

void Track::set(std::int64_t frame, std::vector<Point> points) {
    auto it = std::lower_bound(keys.begin(), keys.end(), frame,
                               [](const ShapeKey& k, std::int64_t f) { return k.frame < f; });
    if (it != keys.end() && it->frame == frame) it->points = std::move(points);
    else keys.insert(it, ShapeKey{frame, std::move(points)});
}

bool Track::remove_key(std::int64_t frame) {
    if (keys.size() <= 1) return false;
    auto it = std::find_if(keys.begin(), keys.end(), [&](const ShapeKey& k) { return k.frame == frame; });
    if (it == keys.end()) return false;
    keys.erase(it);
    return true;
}

Garbage* Fixes::find(int id) {
    for (Garbage& g : garbage)
        if (g.id == id) return &g;
    return nullptr;
}

const Garbage* Fixes::find(int id) const {
    for (const Garbage& g : garbage)
        if (g.id == id) return &g;
    return nullptr;
}

Stroke* Fixes::find_stroke(int id) {
    for (Stroke& s : strokes)
        if (s.id == id) return &s;
    return nullptr;
}

const Stroke* Fixes::find_stroke(int id) const {
    for (const Stroke& s : strokes)
        if (s.id == id) return &s;
    return nullptr;
}

int Fixes::add_stroke(std::int64_t frame, PaintMode mode, float radius, std::vector<Point> points) {
    Stroke s;
    s.id = next_id++;
    s.mode = mode;
    s.radius = radius;
    s.keys.push_back(ShapeKey{frame, std::move(points)});
    strokes.push_back(std::move(s));
    return strokes.back().id;
}

bool Fixes::remove_stroke(int id) {
    const auto before = strokes.size();
    strokes.erase(std::remove_if(strokes.begin(), strokes.end(),
                                 [&](const Stroke& s) { return s.id == id; }),
                  strokes.end());
    return strokes.size() != before;
}

int Fixes::add_garbage(std::int64_t frame, std::vector<Point> points) {
    Garbage g;
    g.id = next_id++;
    g.keys.push_back(ShapeKey{frame, std::move(points)});
    garbage.push_back(std::move(g));
    return garbage.back().id;
}

bool Fixes::remove_garbage(int id) {
    const auto before = garbage.size();
    garbage.erase(std::remove_if(garbage.begin(), garbage.end(),
                                 [&](const Garbage& g) { return g.id == id; }),
                  garbage.end());
    return garbage.size() != before;
}

std::vector<std::int64_t> Fixes::key_frames() const {
    std::vector<std::int64_t> frames;
    for (const Garbage& g : garbage)
        for (const ShapeKey& k : g.keys) frames.push_back(k.frame);
    for (const Stroke& s : strokes)
        for (const ShapeKey& k : s.keys) frames.push_back(k.frame);
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

bool Fixes::load(const std::string& path, std::string* error) {
    *this = Fixes{};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::ifstream in(path);
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error) *error = "not JSON";
        return false;
    }
    if (doc.value("version", 0) != kFormatVersion) {
        if (error) *error = "an unknown version of the fixes format";
        return false;
    }
    next_id = doc.value("next_id", 1);
    // Anything that cannot tween is left out rather than drawn wrong.
    for (const auto& g : doc.value("garbage", nlohmann::json::array())) {
        Garbage shape;
        shape.id = g.value("id", 0);
        if (read_keys(g, 3, &shape.keys)) garbage.push_back(std::move(shape));
    }
    for (const auto& st : doc.value("strokes", nlohmann::json::array())) {
        Stroke stroke;
        stroke.id = st.value("id", 0);
        stroke.mode = st.value("mode", std::string("out")) == "in" ? PaintMode::kIn : PaintMode::kOut;
        stroke.radius = st.value("radius", 20.0f);
        stroke.softness = st.value("softness", 0.5f);
        if (read_keys(st, 1, &stroke.keys)) strokes.push_back(std::move(stroke));
    }
    for (const Garbage& g : garbage) next_id = std::max(next_id, g.id + 1);
    for (const Stroke& st : strokes) next_id = std::max(next_id, st.id + 1);
    return true;
}

bool Fixes::save(const std::string& path, std::string* error) const {
    nlohmann::json doc;
    doc["version"] = kFormatVersion;
    doc["next_id"] = next_id;
    nlohmann::json shapes = nlohmann::json::array();
    for (const Garbage& g : garbage) shapes.push_back({{"id", g.id}, {"keys", write_keys(g.keys)}});
    doc["garbage"] = shapes;
    nlohmann::json strokes_json = nlohmann::json::array();
    for (const Stroke& st : strokes)
        strokes_json.push_back({{"id", st.id},
                                {"mode", st.mode == PaintMode::kIn ? "in" : "out"},
                                {"radius", st.radius},
                                {"softness", st.softness},
                                {"keys", write_keys(st.keys)}});
    doc["strokes"] = strokes_json;

    // Written beside and renamed into place, so a crash mid-write leaves the
    // last good file, not half of a new one.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string partial = path + ".partial";
    {
        std::ofstream out(partial);
        out << doc.dump(1) << "\n";
        if (!out) {
            if (error) *error = "cannot write " + partial;
            return false;
        }
    }
    std::filesystem::rename(partial, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

void History::record(const Fixes& before) {
    undo_.push_back(before);
    if (undo_.size() > kHistoryDepth) undo_.erase(undo_.begin());
    redo_.clear();
}

bool History::undo(Fixes& fixes) {
    if (undo_.empty()) return false;
    redo_.push_back(fixes);
    fixes = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool History::redo(Fixes& fixes) {
    if (redo_.empty()) return false;
    undo_.push_back(fixes);
    fixes = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace ps::fixes
