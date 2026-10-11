/*
 * sim_layers.h -- the sim's side of app/layers.h (sim_layers.cpp): the pipelines, and the layers drawn as quads
 * at the end of the eye pass. Host-internal.
 */
#pragma once

#include "../gpu/render.h"

namespace vrcore::sim {

bool layers_init(gpu::Renderer &r);         // after the renderer, before the front's init
void layers_draw(gpu::Frame &f);            // in the eye pass, after the front's draw
void layers_shutdown(gpu::Renderer &r);     // after the front's shutdown

}
