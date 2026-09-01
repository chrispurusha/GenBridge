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

// The IPlugView half: the host owns an NSView and hands it over, and this puts our view inside it.
//
// Objective-C++ only because IPlugView is a C++ vtable that has to hand a Cocoa view to the host.
// The drawing surface is plain Objective-C (gbView.m) and the drawing itself plain C (gbDraw.c);
// each language earns its place rather than spreading.
//
// ATTACHMENT WAS NEVER THE HARD PART: cast the parent to NSView *, add ours as a subview, reverse
// it in removed(). The host quirks below are the ones G2-Edit's notes record having actually paid
// for, and they cost nothing to carry.

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstring>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "gbEditor.h"
#include "gbView.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// Same gate as the processor's log_line(): touch /tmp/genbridge-log to turn it on. Resize is
// negotiated between host and plug-in over several calls per pointer move, and no amount of staring
// at the code shows which rects a given host actually asks for - two versions of
// checkSizeConstraint() were reasoned out and both were wrong. This is how the next one gets
// evidence instead.
static void gb_editor_log(const char * format, ...) {
    if (access("/tmp/genbridge-log", F_OK) != 0) {
        return;
    }

    FILE * file = fopen("/tmp/genbridge.log", "a");

    if (file == nullptr) {
        return;
    }
    va_list args;

    va_start(args, format);
    fprintf(file, "[editor] ");
    vfprintf(file, format, args);
    fprintf(file, "\n");
    va_end(args);
    fclose(file);
}

class GenBridgeEditorView : public IPlugView {
public:
    GenBridgeEditorView(IEditController * controllerIn, IComponentHandler * handlerIn, int slot,
                        bool instrument, double widthIn, double heightIn,
                        tGbEditorGone goneIn, tGbEditorResized resizedIn, void * userIn)
        : refCount(1), statusSlot(slot), isInstrument(instrument),
          controller(controllerIn), handler(handlerIn), gone(goneIn), resized(resizedIn),
          callbackUser(userIn), width(widthIn), height(heightIn) {
        if (controller != nullptr) {
            controller->addRef();
        }
    }

    virtual ~GenBridgeEditorView(void) {
        if (view != nullptr) {
            gb_view_destroy(view);
            view = nullptr;
        }

        // BEFORE the controller reference goes, since that is what has been keeping the controller
        // alive to be told. The reference this holds is also why the callback is safe: the controller
        // cannot have been destroyed while an editor of its own still exists.
        if (gone != nullptr) {
            gone(callbackUser);
            gone = nullptr;
        }

        if (controller != nullptr) {
            controller->release();
        }
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void ** obj) SMTG_OVERRIDE {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugView)
        QUERY_INTERFACE(iid, obj, IPlugView::iid, IPlugView)
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef(void) SMTG_OVERRIDE { return (uint32)++refCount; }

    uint32 PLUGIN_API release(void) SMTG_OVERRIDE {
        int32 c = --refCount;

        if (c == 0) {
            delete this;
            return 0;
        }
        return (uint32)c;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE {
        return (strcmp(type, kPlatformTypeNSView) == 0) ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API attached(void * parent, FIDString type) SMTG_OVERRIDE {
        if ((parent == nullptr) || (strcmp(type, kPlatformTypeNSView) != 0)) {
            return kInvalidArgument;
        }

        NSView * host = (__bridge NSView *)parent;

        view = gb_view_create(width, height, on_edit, this, statusSlot, isInstrument);

        if (view == nullptr) {
            return kResultFalse;
        }

        NSView * ours = (__bridge NSView *)view;

        // AUTORESIZING, and the earlier decision to turn it off was wrong. The reasoning was that
        // onSize() should be the single authority, since AppKit and onSize() both writing the frame
        // in no guaranteed order is a real hazard. What that missed is WHEN each of them acts: AppKit
        // resizes this view with its parent on every frame of a live drag, while a host calls
        // onSize() when the drag ENDS. Take the mask away and the view simply sits at its old size
        // until the user lets go - "the components only seem to be resized after the window resize
        // completes". CT reported exactly that, and it was this line that did it.
        //
        // They do not in fact fight. This view is created filling its parent, and the mask keeps
        // margins rather than scaling proportionally, so it goes on filling it; onSize() then asks
        // for a frame it already has, and the setFrame there is guarded to a no-op when it agrees.
        [ours setFrame:NSMakeRect(0.0, 0.0, width, height)];
        [ours setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
        [host addSubview:ours];

        push_values();

        // ASK, if the host did not take the remembered size. Most hosts call getSize() before
        // attaching and hand over a parent of exactly that size, and for those this does nothing.
        // A host that attached a default-sized parent instead is the case the size persistence was
        // built for and would otherwise still lose it, and resizeView() is the only way to say so -
        // which is what plugFrame has been sitting here for. Once, not in a loop: if the host
        // declines, the view stays the size it was given.
        if (plugFrame != nullptr) {
            NSRect parent = [host bounds];

            if (  (fabs(parent.size.width - width) > 1.0)
               || (fabs(parent.size.height - height) > 1.0)) {
                ViewRect want = { 0, 0, (int32)lround(width), (int32)lround(height) };

                plugFrame->resizeView(this, &want);
            }
        }

        return kResultOk;
    }

    tresult PLUGIN_API removed(void) SMTG_OVERRIDE {
        if (view != nullptr) {
            gb_view_destroy(view);
            view = nullptr;
        }

        return kResultOk;
    }

    tresult PLUGIN_API onWheel(float distance) SMTG_OVERRIDE { (void)distance; return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16 key, int16 code, int16 mods) SMTG_OVERRIDE {
        (void)key; (void)code; (void)mods; return kResultFalse;
    }
    tresult PLUGIN_API onKeyUp(char16 key, int16 code, int16 mods) SMTG_OVERRIDE {
        (void)key; (void)code; (void)mods; return kResultFalse;
    }

    tresult PLUGIN_API getSize(ViewRect * size) SMTG_OVERRIDE {
        if (size == nullptr) {
            return kInvalidArgument;
        }

        size->left   = 0;
        size->top    = 0;
        size->right  = (int32)width;
        size->bottom = (int32)height;

        return kResultOk;
    }

    // Remember what the host set, so a later getSize() reports that rather than the original
    // default. G2-Edit's notes record Ableton asking in that order.
    tresult PLUGIN_API onSize(ViewRect * newSize) SMTG_OVERRIDE {
        if (newSize == nullptr) {
            return kInvalidArgument;
        }

        width  = (double)(newSize->right - newSize->left);
        height = (double)(newSize->bottom - newSize->top);

        gb_editor_log("onSize %.0fx%.0f", width, height);

        // Straight on to the controller, which is what outlives this view and what the host asks for
        // its state. Remembering it here alone is what made the size revert every time the editor was
        // reopened: createView() builds a new view each time, and a new view starts at the default.
        if (resized != nullptr) {
            resized(callbackUser, width, height);
        }

        // ONLY WHEN IT DISAGREES. The autoresizing mask has usually put the view here already, and
        // setting a frame it already has still runs a layout pass and still marks the layer dirty -
        // per resize, for nothing. Writing it unconditionally is also what made the two of them look
        // like rival authorities; guarded, whichever got there first is simply right.
        if (view != nullptr) {
            NSView * ours = (__bridge NSView *)view;
            NSRect   want = NSMakeRect(0.0, 0.0, width, height);

            if (!NSEqualRects([ours frame], want)) {
                [ours setFrame:want];
            }
        }

        return kResultOk;
    }

    tresult PLUGIN_API onFocus(TBool state) SMTG_OVERRIDE { (void)state; return kResultOk; }

    tresult PLUGIN_API setFrame(IPlugFrame * frame) SMTG_OVERRIDE {
        plugFrame = frame;
        return kResultOk;
    }

    tresult PLUGIN_API canResize(void) SMTG_OVERRIDE { return kResultTrue; }

    // The panel is a fixed logical canvas that simply scales, so any size works as long as the
    // aspect is kept - otherwise the layout stretches and the text goes with it.
    //
    // NO HISTORY. THAT IS THE WHOLE POINT OF THIS VERSION. Two earlier ones asked which edge the user
    // was moving by comparing the proposed rect against the size we believed we were at, and both
    // were wrong in the same way: `width`/`height` only advance when the host calls onSize(), and
    // they are updated from our OWN previous answers, so the reply to a given rect depended on what
    // had happened before it. A host that asks twice for one pointer position - or asks with the
    // pointer's raw rect, whose unmoved dimension is still the PRE-DRAG value - then gets two
    // different answers and applies both. CT: "the whole plugin window resizes randomly and restores".
    //
    // Averaging the two candidate widths depends on nothing but the rect in hand. It is idempotent,
    // so feeding our own answer back returns it unchanged and the size cannot oscillate; and it is
    // continuous, so there is no branch to flip and no tie to break. Dragging one edge moves the
    // other axis at half rate for a frame or two and converges - which for an aspect-locked window
    // is what should happen anyway, and it eases rather than jumping.
    tresult PLUGIN_API checkSizeConstraint(ViewRect * rect) SMTG_OVERRIDE {
        if (rect == nullptr) {
            return kInvalidArgument;
        }

        double aspect = GB_CANVAS_H / GB_CANVAS_W;
        double fromW  = (double)(rect->right - rect->left);
        double fromH  = (double)(rect->bottom - rect->top) / aspect;
        double wanted = (fromW + fromH) * 0.5;

        if (wanted < (GB_CANVAS_W * 0.75)) {
            wanted = GB_CANVAS_W * 0.75;
        } else if (wanted > (GB_CANVAS_W * 2.0)) {
            wanted = GB_CANVAS_W * 2.0;
        }

        // ROUNDED, NOT TRUNCATED. A host asks this repeatedly through a drag and feeds each answer
        // back in, so a truncation is not a one-off half pixel - it is a step taken every time.
        int32 outW = (int32)lround(wanted);
        int32 outH = (int32)lround(wanted * aspect);

        gb_editor_log("checkSizeConstraint %dx%d -> %dx%d  (at %.0fx%.0f)",
                      rect->right - rect->left, rect->bottom - rect->top,
                      outW, outH, width, height);

        rect->right  = rect->left + outW;
        rect->bottom = rect->top + outH;

        return kResultTrue;
    }

private:
    // EVERY EDIT GOES THROUGH THE HOST, never straight into the plug-in. beginEdit/performEdit/
    // endEdit is what puts the change into the host's automation and its saved state, and it is
    // also how the value reaches the PROCESSOR - which the controller has no direct route to.
    // Writing to the processor behind the host's back would leave a project that reopens with
    // settings the user never chose.
    static void on_edit(void * user, const tGbEditRequest * request) {
        ((GenBridgeEditorView *)user)->apply(request);
    }

    void apply(const tGbEditRequest * request) {
        ParamID id = 0;

        switch (request->which) {
            case eGbEditDevice: id = 0; break;
            case eGbEditRate:   id = 2; break;
            case eGbEditFrames: id = 3; break;
            case eGbEditTrim:   id = 1; break;
            case eGbEditMode:   id = 4; break;
            case eGbEditFirstChannel: id = 5; break;
            case eGbEditMidiDest: id = 6; break;
            case eGbEditMidiChannel: id = 7; break;
            case eGbEditMeasure:  id = 8; break;
            case eGbEditOffset:   id = 9; break;
            default: return;
        }

        if (handler != nullptr) {
            handler->beginEdit(id);
            handler->performEdit(id, request->normalized);
            handler->endEdit(id);

            // The measure control is a momentary trigger, so it must return to rest - otherwise the
            // rising edge never comes round again and the button works exactly once.
            if (request->which == eGbEditMeasure) {
                handler->beginEdit(id);
                handler->performEdit(id, 0.0);
                handler->endEdit(id);
            }
        }

        if (controller != nullptr) {
            controller->setParamNormalized(id,
                                           (request->which == eGbEditMeasure) ? 0.0
                                                                              : request->normalized);
        }

        push_values();
    }

    void push_values(void) {
        if ((view == nullptr) || (controller == nullptr)) {
            return;
        }

        gb_view_set_values(view,
                           controller->getParamNormalized(0),
                           controller->getParamNormalized(2),
                           controller->getParamNormalized(3),
                           controller->getParamNormalized(1),
                           controller->getParamNormalized(4),
                           controller->getParamNormalized(5),
                           controller->getParamNormalized(6),
                           controller->getParamNormalized(9),
                           controller->getParamNormalized(7));
    }

public:
    void refresh_values(void) {
        push_values();
    }

    void set_status_slot(int slot) {
        statusSlot = slot;

        if (view != nullptr) {
            gb_view_set_status_slot(view, slot);
        }
    }

private:
    std::atomic<int32>  refCount;
    int                 statusSlot = -1;
    bool                isInstrument = false;
    IEditController *   controller = nullptr;
    IComponentHandler * handler    = nullptr;
    IPlugFrame *        plugFrame  = nullptr;
    tGbEditorGone       gone         = nullptr;
    tGbEditorResized    resized      = nullptr;
    void *              callbackUser = nullptr;
    void *              view         = nullptr;
    double              width        = GB_CANVAS_W;
    double              height       = GB_CANVAS_H;
};

IPlugView * gb_create_editor_view(IEditController * controller, IComponentHandler * handler,
                                  int statusSlot, bool instrument,
                                  double width, double height,
                                  tGbEditorGone gone, tGbEditorResized resized, void * user) {
    return new GenBridgeEditorView(controller, handler, statusSlot, instrument, width, height,
                                   gone, resized, user);
}

void gb_editor_refresh_values(IPlugView * view) {
    if (view != nullptr) {
        ((GenBridgeEditorView *)view)->refresh_values();
    }
}

void gb_editor_set_status_slot(IPlugView * view, int statusSlot) {
    if (view != nullptr) {
        ((GenBridgeEditorView *)view)->set_status_slot(statusSlot);
    }
}
