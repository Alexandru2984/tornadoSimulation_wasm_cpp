#version 330 core
in float vAlpha;
out vec4 FragColor;

void main() {
    vec2 uv = gl_PointCoord.xy - 0.5;
    float d = length(uv);
    float mask = smoothstep(0.5, 0.2, d);
    float a = vAlpha * mask * 0.35;
    FragColor = vec4(0.6, 0.65, 0.8, a);
}
