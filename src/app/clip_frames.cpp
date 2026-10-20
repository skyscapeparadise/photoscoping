#include "app/clip_frames.hpp"

namespace ps::app {

bool ClipFrames::open(const std::string& path, bool hardware, gpu::IngestPass* ingest) {
    close();
    auto decoder = std::make_unique<media::VideoDecoder>();
    if (!decoder->open(path, hardware)) return false;
    decoder_ = std::move(decoder);
    ingest_ = ingest;
    path_ = path;
    frame_rate_ = decoder_->frame_rate();
    frame_count_ = decoder_->frame_count();
    // On the worker: step to the next frame or seek to any other. Playback is
    // almost all steps, which is what keeps it cheap.
    start([this](std::int64_t frame) {
        media::VideoDecoder& d = *decoder_;
        if (d.frame_index() == frame) return true;
        const bool ok = frame == d.frame_index() + 1 ? d.read_next() : d.seek_frame(frame);
        return ok && d.frame_index() == frame;
    });
    return true;
}

void ClipFrames::close() {
    stop();
    decoder_.reset();
    path_.clear();
    frame_rate_ = 0.0;
    frame_count_ = 0;
}

void ClipFrames::set_lut(const color::CubeLut* lut) {
    lut_ = lut;
    refresh();
}

bool ClipFrames::upload(SDL_GPUCommandBuffer* cmd) {
    // The signal is described per frame, so the transform is too.
    return ingest_->process(cmd, *decoder_, gpu::transform_for(decoder_->signal(), lut_));
}

}  // namespace ps::app
