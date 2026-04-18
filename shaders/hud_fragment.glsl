#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uFontTex;
uniform vec3 uColor;
void main() {
    float a = texture(uFontTex, vUV).r;
    if (a < 0.3) discard;
    FragColor = vec4(uColor, a);
}
