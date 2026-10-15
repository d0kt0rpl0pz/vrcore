#version 450
// scene.vert -- the M1 test scene: a model matrix and a colour per draw. A zero normal is unlit (the grids).
// Built also with TEXTURED (the controller models): a second vertex input, the UV, for scene.frag's base colour.
#extension GL_EXT_multiview : require
#include "vrcore_views.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;         // linear
} push;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 0) out vec3 v_color;
#ifdef TEXTURED
layout(location = 2) in vec2 in_uv;
layout(location = 1) out vec2 v_uv;
#endif

void main()
{
    vec4 world = push.model * vec4(in_pos, 1.0);
    gl_Position = views.view_proj[gl_ViewIndex] * world;
    vec3 n = mat3(push.model) * in_normal;
    float lit = 1.0;
    if (dot(n, n) > 0.0) {
        const vec3 light = normalize(vec3(0.4, 1.0, 0.3));
        lit = 0.15 + 0.85 * max(dot(normalize(n), light), 0.0);
    }
    v_color = push.color.rgb * lit;
#ifdef TEXTURED
    v_uv = in_uv;
#endif
}
