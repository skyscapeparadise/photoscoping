#pragma once

namespace ps::app {

// A File menu in the menu bar, between the application menu and SDL's Window
// menu, with Open... on Command-O. `open` runs on the main thread when it is
// chosen; what to open is the caller's to ask.
//
// False while there is no menu bar to add to: SDL builds it as the
// application finishes launching, on the first pass of the event loop, which
// can come after the window does. Call again until it is true.
bool install_file_menu(void (*open)(void* context), void* context);

}  // namespace ps::app
