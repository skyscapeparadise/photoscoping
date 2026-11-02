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
    // A Clip menu item about the clean plate (a PlateAction).
    void (*plate)(void* context, int action) = nullptr;
    // What the Clip menu shows (PlateState bits).
    int (*plate_state)(void* context) = nullptr;
    // Whether light maps can be exported now.
    bool (*can_export_lights)(void* context) = nullptr;
    // A light-map export setting (LightOption) set to a value, and what each
    // is now.
    void (*light_option)(void* context, int option, int value) = nullptr;
    int (*light_value)(void* context, int option) = nullptr;
};

enum LightOption { kLightLinear = 0, kLightFillShadows = 1, kLightSize = 2 };

enum PlateAction { kPlateStart = 0, kPlateEnd = 1, kPlateClear = 2, kPlateCorrect = 3 };
enum PlateState { kPlateClipOpen = 1, kPlateIsSet = 2, kPlateCorrects = 4 };

// A File menu in the menu bar, between the application menu and SDL's Window
// menu:
//
//   Open...                 Command-O
//   Export Subject  >       ProRes 4444 with Alpha (Command-E), VP9 with
//                           Alpha, PNG Sequence
//   Export Matte    >       ProRes 422, ProRes 422 LT, HEVC, H.264
//   Export Light Maps >     HEVC, ProRes 422, 422 LT, 4444, H.264, VP9, PNG
//                           and EXR Sequences; Linear Light or Graded Look;
//                           Fill the Performer's Shadow; Size >
//
// and a Clip menu, for the clean plate:
//
//   Start Clean Plate Here      Command-[
//   End Clean Plate Here        Command-]
//   Clear Clean Plate
//   Plate Corrects the Matte    (checked when on)
//
// False while there is no menu bar to add to: SDL builds it as the
// application finishes launching, on the first pass of the event loop, which
// can come after the window does. Call again until it is true.
bool install_file_menu(const FileMenuActions& actions);

}  // namespace ps::app
