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

    // NO TIMER YET. There is no window at this point, so there is nothing to repaint for;
    // -viewDidMoveToWindow starts one once there is. See -updateTimer.
    return self;
}

// Whether a repaint would be seen by anyone. THE EDITOR BEING CLOSED IS NOT THE ONLY WAY TO STOP
// SHOWING IT: the host calls removed() for that and the timer goes with the view, but a window that
// is minimised, completely covered by another, or sitting on an inactive Space is just as invisible
// and the view is still in the hierarchy. So is one in a host that HIDES its plug-in view rather than
// removing it, which some do when switching between panels in a rack. In every one of those cases
// this used to go on drawing thirty full Metal frames a second, each a complete redraw - gb_draw_frame()
// has no dirty check - into a surface nobody was looking at.
- (BOOL)shouldRepaint {
    NSWindow * window = [self window];

    if ((window == nil) || [self isHiddenOrHasHiddenAncestor]) {
        return NO;
    }

    return ([window occlusionState] & NSWindowOcclusionStateVisible) != 0;
}

// The timer exists exactly while it is worth having. Starting one is cheap, so this is driven from
// the notifications rather than by letting a tick fire and return early: a tick that returns early
// still wakes the process thirty times a second, which is most of what there was to save on a
// machine that has gone to sleep with a project open.
- (void)updateTimer {
    BOOL wanted = [self shouldRepaint];

    if (wanted && (self.timer == nil)) {
        // A timer rather than a CVDisplayLink. The panel shows meters and drift telemetry, so it has
        // to repaint continuously rather than on demand, but nothing here is worth a display link's
        // complications - and a link fires on its own thread, which would mean marshalling every
        // frame back to the main one before touching AppKit.
        self.timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 30.0)
                                                      target:self
                                                    selector:@selector(tick:)
                                                    userInfo:nil
                                                     repeats:YES];

        // Without this the timer stops while a menu is tracking or the window is being resized, and
        // the meters freeze at whatever they last showed - which reads as the plug-in having crashed.
        [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSRunLoopCommonModes];

        // At once, rather than up to a thirtieth of a second later: coming back to an uncovered
        // window should not show a frame of whatever the meters read when it was covered.
        [self redraw];
    } else if (!wanted && (self.timer != nil)) {
        [self.timer invalidate];    // the timer retains self, so this is also what lets the view go
        self.timer = nil;
    }
}

// PER WINDOW, not once: the notification is observed against a specific window and a plug-in view is
// moved between them - re-parented as a host opens the editor in a floating window, docks it in a
// rack, or closes it. Registering against nil instead would catch every window in the host.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];

    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSWindowDidChangeOcclusionStateNotification
                                                  object:nil];

    if ([self window] != nil) {
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(occlusionChanged:)
                                                     name:NSWindowDidChangeOcclusionStateNotification
                                                   object:[self window]];
    }

    [self updateTimer];
}

- (void)occlusionChanged:(NSNotification *)note {
    (void)note;
    [self updateTimer];
}

- (void)viewDidHide {
    [super viewDidHide];
    [self updateTimer];
}

- (void)viewDidUnhide {
    [super viewDidUnhide];
    [self updateTimer];
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

    // A view with no window has no drawable behind it, and a zero-sized one would ask the backend for
    // render targets it cannot make. Both are reachable: -redraw is called from -mouseDown: and from
    // -updateTimer as well as from the tick.
    if (([self window] == nil) || (backing.size.width < 1.0) || (backing.size.height < 1.0)) {
        return;
    }

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
    [[NSNotificationCenter defaultCenter] removeObserver:self];
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

    [[NSNotificationCenter defaultCenter] removeObserver:v];
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
