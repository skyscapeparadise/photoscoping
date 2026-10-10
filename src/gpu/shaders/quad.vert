#version 450

layout(location = 0) out vec2 v_uv;

layout(set = 1, binding = 0) uniform Quad {
    vec4 rect;   // x0, y0, x1, y1 in NDC
} quad;

// One quad from gl_VertexIndex, placed by a uniform rect. No vertex buffer:
// both passes draw exactly one quad.
void main() {
    vec2 corners[6] = vec2[6](
        vec2(quad.rect.x, quad.rect.y), vec2(quad.rect.z, quad.rect.y),
        vec2(quad.rect.x, quad.rect.w), vec2(quad.rect.x, quad.rect.w),
        vec2(quad.rect.z, quad.rect.y), vec2(quad.rect.z, quad.rect.w));
    // NDC y points up and texture v points down, so the bottom corners
    // (y = -1) sample v = 1. Pairing them naively flips the image, which looks
    // like a decode bug and is not one.
    vec2 uvs[6] = vec2[6](
        vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    v_uv = uvs[gl_VertexIndex];
}
