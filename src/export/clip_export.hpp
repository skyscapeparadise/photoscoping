#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fixes/rematte.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/ingest_pass.hpp"
#include "matting/preprocess.hpp"
#include "matting/stack.hpp"
#include "media/export_format.hpp"
#include "media/video_decoder.hpp"

// The headless runs that make what the editor shows and what leaves it:
// re-matte patches, and exports.
namespace ps::exporting {

// The patch for `box` on a frame: the stack's closer look around it
// (MattingStack::closer), over `base`, the frame's matte, cut to the box.
// `detail` is set to how far the crop was magnified. Empty on failure.
std::vector<float> make_patch(matting::MattingStack& stack, const matting::MattingFrame& frame,
                              const std::vector<float>& base, const fixes::PixelRect& box,
                              float* detail, std::string* error);
// Written under a temporary name and renamed into place, so the viewer never
// reads half of one.
bool write_patch(const std::string& path, const std::vector<float>& patch,
                 const fixes::PixelRect& box, std::string* error);

// The clip's matte for a frame, as the run wrote it: 16 bits, the frame's
// size. Empty when the frame has none.
std::vector<float> read_matte(const std::string& render_dir, const std::string& clip_stem,
                              std::int64_t frame, int width, int height);

struct ExportRequest {
    std::string clip;        // for its name
    std::string render_dir;  // where its mattes, patches and fixes are
    std::string out;         // the file, or for a PNG sequence the folder
    media::ExportFormat format = media::ExportFormat::kProRes4444;
    std::string models;
    bool model_cpu = false;
    // Inclusive; the whole clip when last < 0.
    std::int64_t first = 0;
    std::int64_t last = -1;
};

// The clip, matted and fixed, out: each frame's matte read, the re-matte
// patches it wants blended in -- made first, if the run that makes them has
// not -- and the fixes applied, all at full resolution in float, the way the
// viewer shows them. The subject's formats then estimate its own colour,
// free of the wall's, and write it with that alpha; the matte's write the
// alpha alone.
//
// Every frame has to have been matted. The output is written under a
// temporary name and renamed when it is whole, so a stopped export never
// leaves something that looks finished. Progress goes to stdout, a line a
// frame, for the log.
bool run_export(const ExportRequest& request, gpu::GpuContext& gpu, gpu::IngestPass& ingest,
                media::VideoDecoder& decoder, const gpu::InputTransform& transform);

}  // namespace ps::exporting
