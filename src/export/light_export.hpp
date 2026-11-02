#pragma once

#include <cstdint>
#include <string>

#include "gpu/gpu_context.hpp"
#include "gpu/ingest_pass.hpp"
#include "media/export_format.hpp"
#include "media/video_decoder.hpp"

namespace ps::exporting {

struct LightRequest {
    std::string clip;
    std::string render_dir;  // its mattes, patches, plate and fixes
    std::string out_dir;     // a file (or folder) for each crop goes in here
    media::ExportFormat format = media::ExportFormat::kLightHevc;
    std::string models;
    bool model_cpu = false;
    std::int64_t first = 0;
    std::int64_t last = -1;  // the whole clip when < 0
    // Linear light is the clip decoded with this, no LUT: from the camera's
    // log, when the LUT says what it expects (`scene_from_log`), else by the
    // file's tags. gpu::scene_transform_for() makes it.
    gpu::InputTransform scene;
    bool scene_from_log = false;
    std::string scene_name;  // "Canon Log 3, Cinema Gamut", for the sidecar
};

// The clip's light maps (light/light_map.hpp), every crop in its fixes at
// once, each to its own file named for the clip and the crop -- "clip light
// 1.mp4" -- with a JSON sidecar beside it saying how to use it in Blender.
//
// As the fixes' LightExport says: linear light, or the look; the performer's
// shadow kept or filled; the crop's own size or a long side. Linear light in
// a video or PNG is scaled by one exposure for every crop and frame -- found
// by a first pass over the clip -- so the brightest light sits just under
// white, and the sidecar's emission strength undoes it; EXR keeps the light
// as it is. Every frame has to have been matted: the matte says where the
// performer hides the backdrop.
bool run_light_maps(const LightRequest& request, gpu::GpuContext& gpu, gpu::IngestPass& ingest,
                    media::VideoDecoder& decoder, const gpu::InputTransform& transform);

}  // namespace ps::exporting
