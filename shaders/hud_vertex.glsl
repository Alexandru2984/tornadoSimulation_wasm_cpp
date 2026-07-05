#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
layout(location=3) in float aMode; // 0 = solid quad, 1 = font glyph
out vec2 vUV;
out vec4 vColor;
out float vMode;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
    vMode = aMode;
}
