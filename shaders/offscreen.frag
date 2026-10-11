#version 450
// offscreen.frag -- the test scene's offscreen pass: an R8 checker texture (set 2, binding 0) scrolled by the
// front's uniform block (set 1) and coloured by a specialization constant.
layout(constant_id = 0) const int kColor = 0;     // 0 orange, 1 cyan

layout(set = 1, binding = 0) uniform Front {
    vec4 scroll;        // x, y: the checker's offset in its texels
} front;
layout(set = 2, binding = 0) uniform sampler2D checker;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    float k = texelFetch(checker, ivec2(mod(v_uv * 8.0 + front.scroll.xy, 4.0)), 0).r;
    vec3 c = kColor == 0 ? vec3(1.0, 0.5, 0.1) : vec3(0.1, 0.8, 0.9);
    out_color = vec4(c * (0.25 + 0.75 * k), 1.0);
}
