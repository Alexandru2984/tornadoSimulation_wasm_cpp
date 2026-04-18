#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uLightningFlash;
uniform float uTime;

void main() {
    float y = vUV.y;

    // Storm sky gradient — dark, ominous
    vec3 topColor     = vec3(0.02, 0.02, 0.05);
    vec3 midColor     = vec3(0.08, 0.09, 0.14);
    vec3 horizonColor = vec3(0.18, 0.16, 0.22);

    vec3 sky;
    if (y > 0.5) {
        sky = mix(midColor, topColor, (y - 0.5) * 2.0);
    } else {
        sky = mix(horizonColor, midColor, y * 2.0);
    }

    // Animated swirling cloud wisps
    float cx = vUV.x * 6.0 + uTime * 0.15;
    float cy = vUV.y * 3.0 + uTime * 0.08;
    float cloud = sin(cx) * cos(cy + sin(cx * 0.5)) * 0.5 + 0.5;
    cloud *= smoothstep(0.25, 0.65, y);
    sky += cloud * vec3(0.03, 0.03, 0.04);

    // Dark swirl center (above tornado)
    float swirl = sin(vUV.x * 10.0 + uTime * 0.5 + vUV.y * 5.0)
                * cos(vUV.y * 8.0 - uTime * 0.3 + vUV.x * 3.0) * 0.5 + 0.5;
    float centerDist = length(vUV - vec2(0.5, 0.75));
    float swirlMask = smoothstep(0.45, 0.05, centerDist);
    sky += swirl * swirlMask * vec3(0.04, 0.04, 0.06);

    // Lightning flash — whole sky brightens
    sky += vec3(uLightningFlash * 0.85);

    FragColor = vec4(sky, 1.0);
}
