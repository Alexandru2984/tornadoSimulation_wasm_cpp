#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uFontTex;
uniform vec3 uColor;
uniform float uAlpha; // <0: use font texture alpha; >=0: solid fill with this alpha
void main() {
    float a;
    if (uAlpha >= 0.0) {
        a = uAlpha;
    } else {
        a = texture(uFontTex, vUV).r;
        if (a < 0.3) discard;
    }
    FragColor = vec4(uColor, a);
}
