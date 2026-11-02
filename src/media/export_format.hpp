#pragma once

#include <string>

namespace ps::media {

// What photoscoping exports: the matted subject, with its alpha; the matte
// alone, white on black; or light maps, the backdrop's light in crops of it.
// The first of each is the default.
enum class ExportFormat {
    // The subject.
    kProRes4444,   // ProRes 4444 with alpha, .mov
    kVp9Alpha,     // VP9 with alpha, .webm -- the web's transparent video
    kPngSequence,  // 16-bit RGBA PNGs, a folder of them
    // The matte.
    kMatteProRes422,
    kMatteProRes422Lt,
    kMatteHevc,  // 10-bit, .mp4
    kMatteH264,  // .mp4
    // Light maps: colour, no alpha, a file (or folder) for each crop.
    kLightHevc,  // 10-bit, .mp4 -- small, and what Blender plays smoothly
    kLightProRes422,
    kLightProRes422Lt,
    kLightProRes4444,  // 4:4:4, no alpha
    kLightH264,
    kLightVp9,
    kLightPng,  // 16-bit RGB PNGs
    kLightExr,  // half-float OpenEXR: linear light, unclipped
};

enum class ExportKind { kSubject, kMatte, kLight };

struct ExportFormatInfo {
    ExportFormat format;
    const char* id;     // on the command line
    const char* label;  // in the menu
    const char* extension;  // with the dot; empty for a folder
    ExportKind kind;
    bool subject() const { return kind == ExportKind::kSubject; }
    bool sequence() const { return extension[0] == 0; }
};

inline constexpr ExportFormatInfo kExportFormats[] = {
    {ExportFormat::kProRes4444, "prores4444", "ProRes 4444 with Alpha", ".mov", ExportKind::kSubject},
    {ExportFormat::kVp9Alpha, "vp9", "VP9 with Alpha", ".webm", ExportKind::kSubject},
    {ExportFormat::kPngSequence, "png", "PNG Sequence", "", ExportKind::kSubject},
    {ExportFormat::kMatteProRes422, "matte-prores422", "ProRes 422", ".mov", ExportKind::kMatte},
    {ExportFormat::kMatteProRes422Lt, "matte-prores422lt", "ProRes 422 LT", ".mov", ExportKind::kMatte},
    {ExportFormat::kMatteHevc, "matte-hevc", "HEVC", ".mp4", ExportKind::kMatte},
    {ExportFormat::kMatteH264, "matte-h264", "H.264", ".mp4", ExportKind::kMatte},
    {ExportFormat::kLightHevc, "light-hevc", "HEVC", ".mp4", ExportKind::kLight},
    {ExportFormat::kLightProRes422, "light-prores422", "ProRes 422", ".mov", ExportKind::kLight},
    {ExportFormat::kLightProRes422Lt, "light-prores422lt", "ProRes 422 LT", ".mov", ExportKind::kLight},
    {ExportFormat::kLightProRes4444, "light-prores4444", "ProRes 4444", ".mov", ExportKind::kLight},
    {ExportFormat::kLightH264, "light-h264", "H.264", ".mp4", ExportKind::kLight},
    {ExportFormat::kLightVp9, "light-vp9", "VP9", ".webm", ExportKind::kLight},
    {ExportFormat::kLightPng, "light-png", "PNG Sequence", "", ExportKind::kLight},
    {ExportFormat::kLightExr, "light-exr", "EXR Sequence", "", ExportKind::kLight},
};

inline const ExportFormatInfo& export_format_info(ExportFormat format) {
    for (const ExportFormatInfo& info : kExportFormats)
        if (info.format == format) return info;
    return kExportFormats[0];
}

inline bool parse_export_format(const std::string& id, ExportFormat* format) {
    for (const ExportFormatInfo& info : kExportFormats)
        if (id == info.id) {
            *format = info.format;
            return true;
        }
    return false;
}

}  // namespace ps::media
