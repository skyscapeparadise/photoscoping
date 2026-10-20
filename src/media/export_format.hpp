#pragma once

#include <string>

namespace ps::media {

// What photoscoping exports: the matted subject, with its alpha, or the matte
// alone, white on black. The first of each is the default.
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
};

struct ExportFormatInfo {
    ExportFormat format;
    const char* id;     // on the command line
    const char* label;  // in the menu
    const char* extension;  // with the dot; empty for a folder
    bool subject;       // the subject with alpha, not the matte
};

inline constexpr ExportFormatInfo kExportFormats[] = {
    {ExportFormat::kProRes4444, "prores4444", "ProRes 4444 with Alpha", ".mov", true},
    {ExportFormat::kVp9Alpha, "vp9", "VP9 with Alpha", ".webm", true},
    {ExportFormat::kPngSequence, "png", "PNG Sequence", "", true},
    {ExportFormat::kMatteProRes422, "matte-prores422", "ProRes 422", ".mov", false},
    {ExportFormat::kMatteProRes422Lt, "matte-prores422lt", "ProRes 422 LT", ".mov", false},
    {ExportFormat::kMatteHevc, "matte-hevc", "HEVC", ".mp4", false},
    {ExportFormat::kMatteH264, "matte-h264", "H.264", ".mp4", false},
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
