#version 450
// wheel.vert -- the hand wheel (rig/wheel_model): one colour, unlit.
#extension GL_EXT_multiview : require
#include "vrcore_views.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;         // linear
} push;

layout(location = 0) in vec3 in_pos;

void main()
{
    gl_Position = views.view_proj[gl_ViewIndex] * (push.model * vec4(in_pos, 1.0));
}
