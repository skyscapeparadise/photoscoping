#include "app/tracking_ticker.hpp"

#import <Cocoa/Cocoa.h>

// Sixty a second: as often as playback can want a frame.
static const NSTimeInterval kInterval = 1.0 / 60.0;

@interface PSTrackingTicker : NSObject {
@public
    void (*tick_)(void*);
    void* context_;
}
- (void)fire:(NSTimer*)timer;
@end

@implementation PSTrackingTicker
- (void)fire:(NSTimer*)timer {
    (void)timer;
    // SDL pumps events in the default mode, so a firing there is our own
    // loop's turn, and it paints for itself. Any other mode -- event
    // tracking for a menu or a drag, a modal panel -- is AppKit holding the
    // thread, and the frame is ours to paint.
    NSRunLoopMode mode = [[NSRunLoop currentRunLoop] currentMode];
    if (!mode || [mode isEqualToString:NSDefaultRunLoopMode]) return;
    if (tick_) tick_(context_);
}
@end

static PSTrackingTicker* g_ticker = nil;
static NSTimer* g_timer = nil;

namespace ps::app {

void start_tracking_ticker(void (*tick)(void* context), void* context) {
    stop_tracking_ticker();
    g_ticker = [[PSTrackingTicker alloc] init];
    g_ticker->tick_ = tick;
    g_ticker->context_ = context;
    g_timer = [NSTimer timerWithTimeInterval:kInterval
                                      target:g_ticker
                                    selector:@selector(fire:)
                                    userInfo:nil
                                     repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];
}

void stop_tracking_ticker() {
    [g_timer invalidate];
    g_timer = nil;
    g_ticker = nil;
}

}  // namespace ps::app
