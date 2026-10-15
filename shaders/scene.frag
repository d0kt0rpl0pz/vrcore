#version 450
// scene.frag -- the M1 test scene. Built twice: plain, and with FDM_VIEW (--fdm-view), which tints each
// fragment by the size of the block the fragment density map shaded it at: 1x1 as is, 1x2 / 2x1 green,
// 2x2 yellow, larger red. And with TEXTURED (the controller models): the colour is also multiplied by the base
// colour texture (set 2, binding 0; sRGB, so sampled linear), whose UVs repeat (Index's run well outside 0..1).
#ifdef FDM_VIEW
#extension GL_EXT_fragment_invocation_density : require
#endif

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;
#ifdef TEXTURED
layout(location = 1) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D base_color;
#endif

void main()
{
    vec3 c = v_color;
#ifdef TEXTURED
    c *= texture(base_color, v_uv).rgb;
#endif
#ifdef FDM_VIEW
    int area = gl_FragSizeEXT.x * gl_FragSizeEXT.y;
    if (area == 2) c = mix(c, vec3(0.0, 1.0, 0.0), 0.5);
    else if (area == 4) c = mix(c, vec3(1.0, 1.0, 0.0), 0.5);
    else if (area > 4) c = mix(c, vec3(1.0, 0.0, 0.0), 0.5);
#endif
    out_color = vec4(c, 1.0);
}
