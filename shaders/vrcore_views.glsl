// vrcore_views.glsl -- the Views block every vrcore pipeline sees (set 0, binding 0; render.h). Both eyes'
// matrices; gl_ViewIndex picks one. Reverse-Z projections.
layout(set = 0, binding = 0) uniform Views {
    mat4 view_proj[2];
    mat4 view[2];
    mat4 proj[2];
    vec4 eye_pos[2];
    vec4 time;          // x: seconds since start
} views;
