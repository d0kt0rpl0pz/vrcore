/*
 * layers.h -- a front's quad layers: a picture on a rectangle in the room, drawn by the headset's compositor
 * (OpenXR's XR_KHR_composition_layer_quad: its own swapchain, sampled once straight to the display, at no cost
 * to the eye pass): Daytona's HUD panel and screen. In the sim (no compositor) each
 * is a textured quad drawn in the eye pass after the front's draw, over everything (no depth test).
 *
 *   at init:      Layer *l = layer_create(r, {width, height, opaque});
 *   in prepare:   when its picture changes: gpu::Target *t = layer_target(f, l); f.offscreen(t, ...) ... draws
 *                 ... f.end_offscreen(). A frame that draws none keeps the last picture.
 *   every frame:  layer_place(l, pos, rot, width_m, height_m, up)
 *   optionally:   layer_crop(l, w, h): only the picture's top left w x h pixels are shown (a box whose size
 *                 changes, drawn into a layer made for its largest)
 *
 * The picture is shown as drawn: the target's row 0 is the rectangle's top. Opaque, or straight (not
 * premultiplied) alpha over what is behind. The rectangle's middle is at `pos` in the room (OpenXR's LOCAL
 * space), its face towards +z of `rot` (a quad at (0, 0, -2) with no turn faces a viewer at the origin).
 * Layers are composited after the eye pass's picture, in the order they were made.
 *
 * AN UNDERLAY (LayerDesc::underlay) is composited UNDER the eye pass's picture instead, and seen only where the
 * front punched a hole for it: layer_punch, in the eye pass (the front's draw), draws its rectangle with the depth
 * test, transparent black, so 3D drawn in front of it (the hand wheel over the game's screen) stays. On the
 * headset the projection layer is then blended by its alpha (premultiplied: the eye pass's clear and draws leave
 * alpha 1 elsewhere), only on frames with an underlay up. The sim (no compositor) draws the picture itself there,
 * with the depth test.
 */
#pragma once

#include <cstdint>

#include "gpu/render.h"
#include "gpu/vmath.h"

namespace vrcore {

struct Layer;

struct LayerDesc {
    uint32_t width = 0, height = 0;         // the picture's pixels (RGBA8 sRGB)
    bool opaque = false;                    // false: straight alpha
    int eye = 0;                            // 0 both eyes see it, 1 the left only, 2 the right only (a stereo
                                            //   picture: two layers, one an eye)
    bool underlay = false;                  // under the eye pass's picture, seen through layer_punch's hole
};

Layer *layer_create(gpu::Renderer &r, const LayerDesc &d);     // null on failure (logged)
void layer_destroy(gpu::Renderer &r, Layer *l);
// The target to draw the layer's picture into this frame (in the front's prepare); null when there is none to
// be had this frame (logged; the last picture stays).
gpu::Target *layer_target(gpu::Frame &f, Layer *l);
// Where the layer hangs this frame and whether it is shown.
void layer_place(Layer *l, const float pos[3], const Quat &rot, float width_m, float height_m, bool up);
// The part of the picture shown on the rectangle: its top left `width` x `height` pixels (0, 0: all of it, the
// default). Kept until changed.
void layer_crop(Layer *l, uint32_t width, uint32_t height);
// An underlay's hole, in the eye pass (the front's draw, before what may stand in front of it), where it is placed
// this frame; nothing when it is not up. Not for an overlay.
void layer_punch(gpu::Frame &f, Layer *l);

}
