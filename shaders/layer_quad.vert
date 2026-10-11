#version 450
// layer_quad.vert -- the sim's stand-in for a compositor quad layer (app/layers.h): the layer's picture on a
// rectangle in the room, drawn in the eye pass. Four corners as a strip from gl_VertexIndex; the picture's row
// 0 at the top, as a compositor shows a quad layer's image.
#extension GL_EXT_multiview : require
#include "vrcore_views.glsl"

layout(push_constant) uniform Push {
    mat4 model;         // the room from the unit square (-0.5..0.5): the layer's pose times its size
    int eye;            // 0 both eyes, 1 the left only, 2 the right only
    vec2 crop;          // the picture's shown part from its top left, a fraction (layer_crop)
} push;

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 c = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1) - 0.5;   // (-,-) (+,-) (-,+) (+,+)
    v_uv = vec2(c.x + 0.5, 0.5 - c.y) * push.crop;
    gl_Position = views.view_proj[gl_ViewIndex] * push.model * vec4(c, 0.0, 1.0);
    if (push.eye != 0 && push.eye != int(gl_ViewIndex) + 1) gl_Position = vec4(0.0, 0.0, 2.0, 1.0);   // the other eye's
}
