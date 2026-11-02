#include "app/file_menu.hpp"

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <iterator>

#include "media/export_format.hpp"

@interface PSFileMenuTarget : NSObject {
@public
    ps::app::FileMenuActions actions_;
}
- (void)openDocument:(id)sender;
- (void)exportAs:(id)sender;
- (void)plateAction:(id)sender;
- (void)lightOption:(id)sender;
- (void)matteFrame:(id)sender;
@end

@implementation PSFileMenuTarget
- (void)openDocument:(id)sender {
    (void)sender;
    if (actions_.open) actions_.open(actions_.context);
}
- (void)exportAs:(id)sender {
    if (actions_.export_as) actions_.export_as(actions_.context, (int)[sender tag]);
}
- (void)lightOption:(id)sender {
    if (!actions_.light_option) return;
    const long tag = [sender tag];
    const int option = (int)(tag / 100000);
    int value = (int)(tag % 100000);
    if (option == ps::app::kLightSize && value == 99999) {
        // Custom: asked for, as a whole number of pixels on the long side.
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Light map size"];
        [alert setInformativeText:@"The long side, in pixels."];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 120, 24)];
        const int now = actions_.light_value ? actions_.light_value(actions_.context, ps::app::kLightSize) : 0;
        [field setStringValue:[NSString stringWithFormat:@"%d", now > 0 ? now : 1024]];
        [alert setAccessoryView:field];
        [[alert window] setInitialFirstResponder:field];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
        value = std::clamp([field intValue], 16, 16384);
    }
    actions_.light_option(actions_.context, option, value);
}
- (void)matteFrame:(id)sender {
    (void)sender;
    if (actions_.matte_frame) actions_.matte_frame(actions_.context);
}
- (void)plateAction:(id)sender {
    if (actions_.plate) actions_.plate(actions_.context, (int)[sender tag]);
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if ([item action] == @selector(exportAs:)) {
        const long index = [item tag];
        const bool light = index >= 0 && index < (long)std::size(ps::media::kExportFormats) &&
                           ps::media::kExportFormats[index].kind == ps::media::ExportKind::kLight;
        if (light) return actions_.can_export_lights && actions_.can_export_lights(actions_.context);
        return actions_.can_export && actions_.can_export(actions_.context);
    }
    if ([item action] == @selector(matteFrame:))
        return actions_.can_matte_frame && actions_.can_matte_frame(actions_.context);
    if ([item action] == @selector(lightOption:)) {
        const long tag = [item tag];
        const int option = (int)(tag / 100000), value = (int)(tag % 100000);
        const int now = actions_.light_value ? actions_.light_value(actions_.context, option) : 0;
        if (option == ps::app::kLightSize && value == 99999) {
            const bool custom = now > 0 && now != 4096 && now != 2048 && now != 1024 && now != 512 && now != 256;
            [item setState:custom ? NSControlStateValueOn : NSControlStateValueOff];
            [item setTitle:custom ? [NSString stringWithFormat:@"Custom (%d px)…", now] : @"Custom…"];
        } else if (option == ps::app::kLightFillShadows) {
            [item setState:now ? NSControlStateValueOn : NSControlStateValueOff];
        } else {
            [item setState:now == value ? NSControlStateValueOn : NSControlStateValueOff];
        }
        return actions_.light_value != nullptr;
    }
    if ([item action] == @selector(plateAction:)) {
        const int state = actions_.plate_state ? actions_.plate_state(actions_.context) : 0;
        const long tag = [item tag];
        if (tag == ps::app::kPlateCorrect)
            [item setState:(state & ps::app::kPlateCorrects) ? NSControlStateValueOn : NSControlStateValueOff];
        if (!(state & ps::app::kPlateClipOpen)) return NO;
        if (tag == ps::app::kPlateClear || tag == ps::app::kPlateCorrect) return (state & ps::app::kPlateIsSet) != 0;
        return YES;
    }
    return YES;
}
@end

static PSFileMenuTarget* g_target = nil;

namespace ps::app {

namespace {

NSMenuItem* option_item(NSString* title, int option, int value) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:@selector(lightOption:) keyEquivalent:@""];
    [item setTag:option * 100000 + value];
    [item setTarget:g_target];
    return item;
}

// A submenu of the export formats of one kind -- and for light maps, how
// they are exported after them.
NSMenuItem* export_menu(NSString* title, media::ExportKind kind) {
    const bool subject = kind == media::ExportKind::kSubject;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:title];
    int index = 0;
    for (const media::ExportFormatInfo& info : media::kExportFormats) {
        if (info.kind == kind) {
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
    if (kind == media::ExportKind::kLight) {
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:option_item(@"Linear Light", kLightLinear, 1)];
        [menu addItem:option_item(@"Graded Look", kLightLinear, 0)];
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItem:option_item(@"Fill the Performer's Shadow", kLightFillShadows, 1)];
        NSMenu* sizes = [[NSMenu alloc] initWithTitle:@"Size"];
        [sizes addItem:option_item(@"Crop's Own", kLightSize, 0)];
        for (int px : {4096, 2048, 1024, 512, 256})
            [sizes addItem:option_item([NSString stringWithFormat:@"%d px", px], kLightSize, px)];
        [sizes addItem:option_item(@"Custom…", kLightSize, 99999)];
        NSMenuItem* size_holder = [[NSMenuItem alloc] initWithTitle:@"Size" action:nil keyEquivalent:@""];
        [size_holder setSubmenu:sizes];
        [menu addItem:size_holder];
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
    [file addItem:export_menu(@"Export Subject", media::ExportKind::kSubject)];
    [file addItem:export_menu(@"Export Matte", media::ExportKind::kMatte)];
    [file addItem:export_menu(@"Export Light Maps", media::ExportKind::kLight)];

    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    [holder setSubmenu:file];
    // After the application menu, which is always first.
    [bar insertItem:holder atIndex:MIN(1, [bar numberOfItems])];

    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    // M, with no modifier. The app's own keys take it too; whichever sees
    // it first starts the run, and the other finds it already going.
    NSMenuItem* matte = [[NSMenuItem alloc] initWithTitle:@"Matte This Frame"
                                                   action:@selector(matteFrame:)
                                            keyEquivalent:@"m"];
    [matte setKeyEquivalentModifierMask:0];
    [matte setTarget:g_target];
    [clip addItem:matte];
    [clip addItem:[NSMenuItem separatorItem]];
    struct Entry { NSString* title; int action; NSString* key; };
    const Entry entries[] = {
        {@"Start Clean Plate Here", kPlateStart, @"["},
        {@"End Clean Plate Here", kPlateEnd, @"]"},
        {@"Clear Clean Plate", kPlateClear, @""},
        {nil, -1, nil},
        {@"Plate Corrects the Matte", kPlateCorrect, @""},
    };
    for (const Entry& e : entries) {
        if (!e.title) {
            [clip addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:e.title action:@selector(plateAction:)
                                                keyEquivalent:e.key];
        [entry setTag:e.action];
        [entry setTarget:g_target];
        [clip addItem:entry];
    }
    NSMenuItem* clip_holder = [[NSMenuItem alloc] initWithTitle:@"Clip" action:nil keyEquivalent:@""];
    [clip_holder setSubmenu:clip];
    [bar insertItem:clip_holder atIndex:MIN(2, [bar numberOfItems])];
    return true;
}

}  // namespace ps::app
