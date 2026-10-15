#include "app/lockstep_stream.hpp"

namespace ps::app {

bool LockstepStream::open(SDL_GPUDevice* device, const std::string& path, Kind kind) {
    destroy();
    kind_ = kind;
    if (!decoder_.open(path, /*hardware=*/true) || !ingest_.create(device)) {
        destroy();
        return false;
    }
    open_ = true;
    return true;
}

void LockstepStream::destroy() {
    ingest_.destroy();
    decoder_.close();
    open_ = false;
    ingested_ = -1;
    in_step_ = false;
}

bool LockstepStream::sync(SDL_GPUCommandBuffer* cmd, std::int64_t frame) {
    if (!open_ || frame < 0) return false;
    if (frame == ingested_) return in_step_;

    const std::int64_t at = decoder_.frame_index();
    bool ok = at == frame || (frame == at + 1 ? decoder_.read_next() : decoder_.seek_frame(frame));
    ok = ok && decoder_.frame_index() == frame;
    ingested_ = frame;
    in_step_ = ok;
    if (!ok) return false;

    gpu::InputTransform t = gpu::transform_from_tags(decoder_.signal());
    if (kind_ == Kind::kCoverage) {
        // Range expansion still applies -- a limited-range matte's black is
        // code 16 -- but no transfer curve and no change of primaries can,
        // since nothing here is light. Grey stays grey through the gamut
        // matrix either way.
        t.transfer = color::Transfer::kLinear;
        t.gamut = color::Gamut::kRec709;
    }
    return ingest_.process(cmd, decoder_, t);
}

}  // namespace ps::app
