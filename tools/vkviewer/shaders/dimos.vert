#version 450
/*
 * A full-screen triangle. No vertex buffer: the three corners are
 * derived from gl_VertexIndex, which is the standard trick for a
 * post-processing pass.
 *
 *   index 0 -> (-1,-1)  uv (0,0)
 *   index 1 -> ( 3,-1)  uv (2,0)
 *   index 2 -> (-1, 3)  uv (0,2)
 *
 * The triangle is twice the size of the screen, so the visible part of
 * it is exactly the viewport and uv runs 0..1 across it.
 */

layout(location = 0) out vec2 out_uv;

void main() {
    out_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);
}
