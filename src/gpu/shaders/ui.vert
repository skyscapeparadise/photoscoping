#version 450

// SDL_GPU resource sets (SDL_gpu.h, SDL_CreateGPUShader):
//   vertex   -> set 0 sampled textures, set 1 uniform buffers
//   fragment -> set 2 sampled textures, set 3 uniform buffers
// spirv-cross maps these onto the MSL indices SDL expects.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_fill;
layout(location = 3) in vec4 in_border;
layout(location = 4) in vec4 in_rect;
layout(location = 5) in vec4 in_params;
layout(location = 6) in vec4 in_misc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_fill;
layout(location = 2) out vec4 v_border;
layout(location = 3) out vec4 v_rect;
layout(location = 4) out vec4 v_params;
layout(location = 5) out vec4 v_misc;
layout(location = 6) out vec2 v_pixel;

layout(set = 1, binding = 0) uniform Viewport {
    vec2 size;   // drawable size in pixels
    vec2 pad;
} viewport;

void main() {
    // Pixel space (origin top-left, y down) -> NDC (origin centre, y up).
    vec2 ndc = (in_pos / viewport.size) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_uv = in_uv;
    v_fill = in_fill;
    v_border = in_border;
    v_rect = in_rect;
    v_params = in_params;
    v_misc = in_misc;
    v_pixel = in_pos;
}
