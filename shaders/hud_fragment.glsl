#version 330 core
in vec2 vUV;
in vec4 vColor;
in float vMode;
out vec4 FragColor;
uniform sampler2D uFontTex;
void main() {
    float a = vColor.a;
    if (vMode > 0.5) {
        float t = texture(uFontTex, vUV).r;
        if (t < 0.3) discard;
        a *= t;
    }
    FragColor = vec4(vColor.rgb, a);
}
