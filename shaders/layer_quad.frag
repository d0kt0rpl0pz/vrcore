#version 450
// layer_quad.frag -- the layer's picture as is (straight alpha: the pipeline blends by it; opaque: it is not).
layout(set = 2, binding = 0) uniform sampler2D layer_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(layer_tex, v_uv);
}
