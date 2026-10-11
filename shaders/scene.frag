#version 450
// scene.frag -- the M1 test scene. Built twice: plain, and with FDM_VIEW (--fdm-view), which tints each
// fragment by the size of the block the fragment density map shaded it at: 1x1 as is, 1x2 / 2x1 green,
// 2x2 yellow, larger red.
#ifdef FDM_VIEW
#extension GL_EXT_fragment_invocation_density : require
#endif

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    vec3 c = v_color;
#ifdef FDM_VIEW
    int area = gl_FragSizeEXT.x * gl_FragSizeEXT.y;
    if (area == 2) c = mix(c, vec3(0.0, 1.0, 0.0), 0.5);
    else if (area == 4) c = mix(c, vec3(1.0, 1.0, 0.0), 0.5);
    else if (area > 4) c = mix(c, vec3(1.0, 0.0, 0.0), 0.5);
#endif
    out_color = vec4(c, 1.0);
}
