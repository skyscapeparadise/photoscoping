#pragma once

namespace ps::app {

// A File menu in the menu bar, between the application menu and SDL's Window
// menu, with Open... on Command-O. `open` runs on the main thread when it is
// chosen; what to open is the caller's to ask.
void install_file_menu(void (*open)(void* context), void* context);

}  // namespace ps::app
