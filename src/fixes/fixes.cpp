#include "fixes/fixes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ps::fixes {

namespace {

constexpr int kFormatVersion = 1;
constexpr std::size_t kHistoryDepth = 200;

}  // namespace

std::vector<Point> Garbage::at(std::int64_t frame) const {
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

bool Garbage::keyed_at(std::int64_t frame) const {
    return std::any_of(keys.begin(), keys.end(), [&](const ShapeKey& k) { return k.frame == frame; });
}

void Garbage::set(std::int64_t frame, std::vector<Point> points) {
    auto it = std::lower_bound(keys.begin(), keys.end(), frame,
                               [](const ShapeKey& k, std::int64_t f) { return k.frame < f; });
    if (it != keys.end() && it->frame == frame) it->points = std::move(points);
    else keys.insert(it, ShapeKey{frame, std::move(points)});
}

bool Garbage::remove_key(std::int64_t frame) {
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
    for (const auto& g : doc.value("garbage", nlohmann::json::array())) {
        Garbage shape;
        shape.id = g.value("id", 0);
        for (const auto& k : g.value("keys", nlohmann::json::array())) {
            ShapeKey key;
            key.frame = k.value("frame", std::int64_t{0});
            for (const auto& p : k.value("points", nlohmann::json::array()))
                if (p.is_array() && p.size() == 2)
                    key.points.push_back({p[0].get<float>(), p[1].get<float>()});
            shape.keys.push_back(std::move(key));
        }
        std::sort(shape.keys.begin(), shape.keys.end(),
                  [](const ShapeKey& a, const ShapeKey& b) { return a.frame < b.frame; });
        // A shape whose keys disagree on how many points it has cannot tween;
        // it is left out rather than drawn wrong.
        const bool consistent =
            !shape.keys.empty() && shape.keys.front().points.size() >= 3 &&
            std::all_of(shape.keys.begin(), shape.keys.end(), [&](const ShapeKey& k) {
                return k.points.size() == shape.keys.front().points.size();
            });
        if (consistent) garbage.push_back(std::move(shape));
        next_id = std::max(next_id, garbage.empty() ? 1 : garbage.back().id + 1);
    }
    return true;
}

bool Fixes::save(const std::string& path, std::string* error) const {
    nlohmann::json doc;
    doc["version"] = kFormatVersion;
    doc["next_id"] = next_id;
    nlohmann::json shapes = nlohmann::json::array();
    for (const Garbage& g : garbage) {
        nlohmann::json keys = nlohmann::json::array();
        for (const ShapeKey& k : g.keys) {
            nlohmann::json points = nlohmann::json::array();
            for (const Point& p : k.points) points.push_back({p.x, p.y});
            keys.push_back({{"frame", k.frame}, {"points", points}});
        }
        shapes.push_back({{"id", g.id}, {"keys", keys}});
    }
    doc["garbage"] = shapes;

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
