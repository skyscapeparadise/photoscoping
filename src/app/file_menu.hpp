#pragma once

namespace ps::app {

// What the File menu calls back into. Every call is on the main thread.
struct FileMenuActions {
    void* context = nullptr;
    // Open...: what to open is the caller's to ask.
    void (*open)(void* context) = nullptr;
    // An export format chosen, by its index in media::kExportFormats.
    void (*export_as)(void* context, int format) = nullptr;
    // Whether exporting is possible now; the export items are greyed when not.
    bool (*can_export)(void* context) = nullptr;
};

// A File menu in the menu bar, between the application menu and SDL's Window
// menu:
//
//   Open...                 Command-O
//   Export Subject  >       ProRes 4444 with Alpha (Command-E), VP9 with
//                           Alpha, PNG Sequence
//   Export Matte    >       ProRes 422, ProRes 422 LT, HEVC, H.264
//
// False while there is no menu bar to add to: SDL builds it as the
// application finishes launching, on the first pass of the event loop, which
// can come after the window does. Call again until it is true.
bool install_file_menu(const FileMenuActions& actions);

}  // namespace ps::app
