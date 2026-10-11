#version 450
// panel.frag -- the panel: its target's picture (set 2, binding 0), times the push constant's colour (the tint
// draw: a stencil-masked pass over the panel alone, alpha blended, binding 0 unbound and so black).
layout(set = 2, binding = 0) uniform sampler2D target;
layout(push_constant) uniform Push {
    vec4 color;
    float textured;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 c = push.textured > 0.5 ? texture(target, v_uv) : vec4(1.0);
    out_color = c * push.color;
}
