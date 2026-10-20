#include "app/file_menu.hpp"

#import <Cocoa/Cocoa.h>

@interface PSFileMenuTarget : NSObject {
@public
    void (*open_)(void*);
    void* context_;
}
- (void)openDocument:(id)sender;
@end

@implementation PSFileMenuTarget
- (void)openDocument:(id)sender {
    (void)sender;
    if (open_) open_(context_);
}
@end

static PSFileMenuTarget* g_target = nil;

namespace ps::app {

void install_file_menu(void (*open)(void* context), void* context) {
    NSMenu* bar = [NSApp mainMenu];
    if (!bar || g_target) return;
    g_target = [[PSFileMenuTarget alloc] init];
    g_target->open_ = open;
    g_target->context_ = context;

    NSMenu* file = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"Open…"
                                                  action:@selector(openDocument:)
                                           keyEquivalent:@"o"];
    [item setTarget:g_target];
    [file addItem:item];

    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [holder setSubmenu:file];
    // After the application menu, which is always first.
    [bar insertItem:holder atIndex:MIN(1, [bar numberOfItems])];
}

}  // namespace ps::app
