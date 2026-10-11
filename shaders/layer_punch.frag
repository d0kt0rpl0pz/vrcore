#version 450
// layer_punch.frag -- an underlay's hole in the eye pass (app/layers.h layer_punch): transparent black, so the
// compositor shows the layer under the eye pass's picture there; what is drawn after it and in front (the depth
// test) covers the hole.
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(0.0);
}
