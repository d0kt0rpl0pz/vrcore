/*
 * shots.h -- a picture of the front's world from a camera of its own, for an offline comparison (the sim only;
 * on the headset the request is refused). After the frame being drawn, the host draws one more frame for each
 * request: the front's prepare and draw as ever, then the layers, both views from `eye`, into a width x height
 * eye target (4x MSAA, the eye pass's formats), and writes the first view as an sRGB PNG. A quit asked for in
 * the same frame waits for the shots.
 */
#pragma once

#include <cstdint>

#include "gpu/render.h"

namespace vrcore {

// False (logged) on the headset, or when the request cannot be queued.
bool request_view_shot(const gpu::Eye &eye, uint32_t width, uint32_t height, const char *png_path);

}
