#version 330 core
out vec2 vUV;
void main() {
    // Fullscreen triangle via gl_VertexID (3 verts cover entire screen)
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.9999, 1.0);
}
