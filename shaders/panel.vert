#version 450
// panel.vert -- the test scene's panel (render API v1): a quad from a dynamic buffer, textured with an
// offscreen target, in the eye pass.
#extension GL_EXT_multiview : require
#include "vrcore_views.glsl"

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 v_uv;

void main()
{
    gl_Position = views.view_proj[gl_ViewIndex] * vec4(in_pos, 1.0);
    v_uv = in_uv;
}
