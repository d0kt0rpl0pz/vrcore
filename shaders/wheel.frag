#version 450
// wheel.frag -- the hand wheel: its colour as it is.
layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
} push;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(push.color.rgb, 1.0);
}
