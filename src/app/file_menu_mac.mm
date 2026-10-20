#include "app/file_menu.hpp"

#import <Cocoa/Cocoa.h>

#include "media/export_format.hpp"

@interface PSFileMenuTarget : NSObject {
@public
    ps::app::FileMenuActions actions_;
}
- (void)openDocument:(id)sender;
- (void)exportAs:(id)sender;
@end

@implementation PSFileMenuTarget
- (void)openDocument:(id)sender {
    (void)sender;
    if (actions_.open) actions_.open(actions_.context);
}
- (void)exportAs:(id)sender {
    if (actions_.export_as) actions_.export_as(actions_.context, (int)[sender tag]);
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if ([item action] == @selector(exportAs:))
        return actions_.can_export && actions_.can_export(actions_.context);
    return YES;
}
@end

static PSFileMenuTarget* g_target = nil;

namespace ps::app {

namespace {

// A submenu of the export formats that are, or are not, the subject's.
NSMenuItem* export_menu(NSString* title, bool subject) {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
    int index = 0;
    for (const media::ExportFormatInfo& info : media::kExportFormats) {
        if (info.subject == subject) {
            // The default -- the first of the subject's -- on Command-E.
            const bool first = subject && info.format == media::ExportFormat::kProRes4444;
            NSMenuItem* item = [[NSMenuItem alloc]
                initWithTitle:[[NSString stringWithUTF8String:info.label] stringByAppendingString:@"…"]
                       action:@selector(exportAs:)
                keyEquivalent:first ? @"e" : @""];
            [item setTag:index];
            [item setTarget:g_target];
            [menu addItem:item];
        }
        ++index;
    }
    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    [holder setSubmenu:menu];
    return holder;
}

}  // namespace

bool install_file_menu(const FileMenuActions& actions) {
    if (g_target) return true;
    NSMenu* bar = [NSApp mainMenu];
    if (!bar) return false;
    g_target = [[PSFileMenuTarget alloc] init];
    g_target->actions_ = actions;

    NSMenu* file = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@"Open…"
                                                  action:@selector(openDocument:)
                                           keyEquivalent:@"o"];
    [item setTarget:g_target];
    [file addItem:item];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItem:export_menu(@"Export Subject", true)];
    [file addItem:export_menu(@"Export Matte", false)];

    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [holder setSubmenu:file];
    // After the application menu, which is always first.
    [bar insertItem:holder atIndex:MIN(1, [bar numberOfItems])];
    return true;
}

}  // namespace ps::app
