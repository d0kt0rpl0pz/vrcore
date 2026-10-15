#version 450
// panel.frag -- the panel: its target's picture (set 2, binding 0), times the push constant's colour (the tint
// draw: a stencil-masked pass over the panel alone, alpha blended, binding 0 unbound and so black). A lod of 0 or
// more samples that mip level (the KTX2 test quads); below 0 the level the screen derivatives pick.
layout(set = 2, binding = 0) uniform sampler2D target;
layout(push_constant) uniform Push {
    vec4 color;
    float textured;
    float lod;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 c = vec4(1.0);
    if (push.textured > 0.5) c = push.lod >= 0.0 ? textureLod(target, v_uv, push.lod) : texture(target, v_uv);
    out_color = c * push.color;
}
