#version 450
// imgui.vert -- Dear ImGui's vertices (ui/imgui_render.cpp) into a layer's offscreen target: its pixels (row 0 at
// the top) to the target's, then to clip space. The colour is sRGB as ImGui gives it; the fragment decodes it.
layout(push_constant) uniform Push {
    vec2 scale;         // 2 / the target's size
    vec2 offset;        // ImGui's pixel at the target's (0, 0), negated
    vec4 clip;          // the draw command's clip rectangle in the target's pixels: x0, y0, x1, y1
} push;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_col;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;

void main()
{
    v_uv = in_uv;
    v_col = in_col;
    gl_Position = vec4((in_pos + push.offset) * push.scale - 1.0, 0.0, 1.0);
}
