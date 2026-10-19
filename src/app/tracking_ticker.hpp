#pragma once

namespace ps::app {

// Keeps the window drawing while AppKit has the main thread.
//
// Opening a menu in the menu bar, dragging the window, or a modal panel all
// run AppKit's own event loop in place of ours, and the main loop -- with the
// nebula, playback and everything else it drives -- does not get a turn
// until they end. A timer scheduled in the run loop's common modes is still
// fired from inside them, so this one calls `tick` on every one of its
// firings that lands outside our loop. While our loop runs, it does nothing:
// the loop paints its own frames.
//
// sink's answer to the same problem; the pattern is lifted from it.
void start_tracking_ticker(void (*tick)(void* context), void* context);
void stop_tracking_ticker();

}  // namespace ps::app
