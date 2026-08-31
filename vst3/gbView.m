/*
 * GenBridge - bridge any CoreAudio device into a DAW.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The drawing surface, and nothing else.
//
// Plain Objective-C, not Objective-C++: nothing here needs C++, and gbEditor.mm is Objective-C++
// only because IPlugView is a C++ interface that has to hand a Cocoa view to the host. Keeping the
// languages separated that way is G2-Edit's arrangement and it is worth copying.
//
// METAL, so a PLAIN NSView. Under Metal there is no context for the view to own - it is
// layer-hosting and the CAMetalLayer is the surface - which is why there is no NSOpenGLView here
// and no -prepareOpenGL or -reshape. G2-Edit's equivalent picks its superclass at BUILD time
// because a superclass is fixed when the file is compiled; this only ever wants Metal, so there is
// no choice to make.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "gbDraw.h"
#include "gbView.h"
#include "renderBackend.h"

@interface GbView : NSView
@property (nonatomic, assign) tGbEditCallback callback;
@property (nonatomic, assign) void *          user;
@property (nonatomic, strong) NSTimer *       timer;
@property (nonatomic, assign) int            statusSlot;
@property (nonatomic, assign) BOOL           isInstrument;
@end

@implementation GbView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];

    if (self == nil) {
        return nil;
    }

    // The application chooses its backend from a saved setting at start-up; a plug-in has no such
    // setting and no window of its own, so the choice is asserted here before anything touches the
    // backend.
    gfx_backend_choose(eRenderBackendMetal);

    // LAYER-HOSTING, AND THE ORDER MATTERS: gfx_attach_window() assigns the layer and only then is
    // wantsLayer set, which is what tells AppKit the contents belong to the layer and that it must
    // not draw over them. It is handed the VIEW rather than a window - in a plug-in the window
    // belongs to the host, and we may never see it.
    gfx_attach_window((__bridge void *)self);

    gb_draw_init();

    // A timer rather than a CVDisplayLink. The panel shows meters and drift telemetry, so it has to
    // repaint continuously rather than on demand, but nothing here is worth a display link's
    // complications - and a link fires on its own thread, which would mean marshalling every frame
    // back to the main one before touching AppKit.
    self.timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0)
                                                  target:self
                                                selector:@selector(tick:)
                                                userInfo:nil
                                                 repeats:YES];

    // Without this the timer stops while a menu is tracking or the window is being resized, and the
    // meters freeze at whatever they last showed - which reads as the plug-in having crashed.
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];

    return self;
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// A host click that lands on the plug-in's window should reach the control it hit, rather than
// being swallowed as the click that merely focuses the window.
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    [self redraw];
}

- (void)redraw {
    NSRect backing = [self convertRectToBacking:[self bounds]];

    // SELECT THIS VIEW'S CONTEXT FIRST. With two editors open, whichever drew last left the backend
    // pointing at its own layer; drawing without claiming ours would paint into the other one's
    // window. Attaching an already-known view is just a pointer assignment, so this is cheap enough
    // to do every frame and removes any need to track whose turn it is.
    gfx_attach_window((__bridge void *)self);

    // Both of these are file-scope in the draw layer, so they are asserted per frame rather than
    // once - with two editors open, whichever drew last would otherwise speak for both.
    gb_draw_set_status_slot(self.statusSlot);
    gb_draw_set_instrument(self.isInstrument ? true : false);

    gb_draw_frame((int)backing.size.width, (int)backing.size.height);
    gfx_present();
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint local = [self convertPoint:[event locationInWindow] fromView:nil];

    // AppKit's origin is bottom left and the canvas's is top left, so y is flipped here rather than
    // in the drawing code - the renderer's coordinate space is shared with the applications and
    // must not be bent to suit one host view.
    double scale = [self bounds].size.width / GB_CANVAS_W;
    double x     = local.x / scale;
    double y     = ([self bounds].size.height - local.y) / scale;

    // ASSERTED BEFORE THE HIT TEST, not just before the draw. The draw layer's notion of which
    // editor it is serving is file-scope, and the hit test consults it - gb_draw_click() only offers
    // the Measure and Offset controls when it believes it is drawing an instrument. With an effect
    // and an instrument both open, the effect's 30 Hz repaint had already set that flag false by the
    // time a click arrived on the instrument, so those two controls silently did nothing while every
    // other control worked.
    gb_draw_set_status_slot(self.statusSlot);
    gb_draw_set_instrument(self.isInstrument ? true : false);

    tGbEditRequest request;

    if (gb_draw_click(x, y, &request) && (self.callback != NULL)) {
        self.callback(self.user, &request);
    }

    [self redraw];
}

- (void)mouseDragged:(NSEvent *)event {
    // Only the trim responds to a drag; the steppers are discrete. Routing a drag through the same
    // hit test keeps that decision in one place.
    [self mouseDown:event];
}

- (void)removeFromSuperview {
    [self.timer invalidate];        // the timer retains self; leaving it running leaks the view
    self.timer = nil;

    // Hand back the layer and render targets. Without this a host that opens and closes editors
    // would exhaust the backend's window slots, since every new view is a different pointer.
    gfx_detach_window((__bridge void *)self);

    [super removeFromSuperview];
}

@end

void * gb_view_create(double width, double height, tGbEditCallback callback, void * user,
                      int statusSlot, bool instrument) {
    GbView * view = [[GbView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];

    view.callback     = callback;
    view.user         = user;
    view.statusSlot   = statusSlot;
    view.isInstrument = instrument ? YES : NO;

    return (__bridge_retained void *)view;
}

void gb_view_set_status_slot(void * view, int statusSlot) {
    if (view != NULL) {
        ((__bridge GbView *)view).statusSlot = statusSlot;
    }
}

void gb_view_destroy(void * view) {
    if (view == NULL) {
        return;
    }

    GbView * v = (__bridge_transfer GbView *)view;

    [v.timer invalidate];
    v.timer = nil;
    [v removeFromSuperview];
    gfx_detach_window((__bridge void *)v);
}

void gb_view_set_values(void * view, double device, double rate, double frames, double trim,
                        double mode, double firstChannel, double midiDest, double offset,
                        double midiChannel) {
    (void)view;
    gb_draw_set_values(device, rate, frames, trim, mode, firstChannel, midiDest, offset, midiChannel);
}
