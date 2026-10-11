#version 450
// imgui.frag -- one ImGui draw command: the vertex colour (sRGB, decoded) times its texture (set 2, binding 0: the
// font atlas or a front's image), clipped to the command's rectangle (the render API has no scissor), written
// premultiplied (Blend::Premultiplied) so the layer's alpha is the boxes' coverage.
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    vec4 clip;
} push;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 0) out vec4 out_color;

vec3 linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
    if (gl_FragCoord.x < push.clip.x || gl_FragCoord.y < push.clip.y || gl_FragCoord.x >= push.clip.z ||
        gl_FragCoord.y >= push.clip.w)
        discard;
    vec4 c = vec4(linear(v_col.rgb), v_col.a) * texture(tex, v_uv);
    out_color = vec4(c.rgb * c.a, c.a);
}
