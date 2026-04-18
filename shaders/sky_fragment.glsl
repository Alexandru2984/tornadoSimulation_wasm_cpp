#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uLightningFlash;
uniform float uTime;
uniform float uTimeOfDay;

void main() {
    float y = vUV.y;

    // Sun height for day/night
    float sunHeight = sin((uTimeOfDay - 0.25) * 3.14159);
    float dayBright = clamp(sunHeight, 0.0, 1.0);
    float duskFactor = 1.0 - abs(sunHeight);
    float isDusk = smoothstep(0.3, 0.8, duskFactor) * dayBright;

    // Night sky colors (storm)
    vec3 nightTop     = vec3(0.01, 0.01, 0.03);
    vec3 nightMid     = vec3(0.04, 0.04, 0.08);
    vec3 nightHorizon = vec3(0.08, 0.06, 0.1);

    // Day sky colors (stormy day)
    vec3 dayTop     = vec3(0.15, 0.18, 0.35);
    vec3 dayMid     = vec3(0.25, 0.3, 0.45);
    vec3 dayHorizon = vec3(0.45, 0.42, 0.48);

    // Sunset/sunrise colors
    vec3 duskTop     = vec3(0.08, 0.06, 0.15);
    vec3 duskMid     = vec3(0.35, 0.15, 0.12);
    vec3 duskHorizon = vec3(0.65, 0.3, 0.15);

    // Blend based on time of day
    vec3 topColor, midColor, horizonColor;
    if (isDusk > 0.1) {
        topColor     = mix(mix(nightTop, dayTop, dayBright), duskTop, isDusk);
        midColor     = mix(mix(nightMid, dayMid, dayBright), duskMid, isDusk);
        horizonColor = mix(mix(nightHorizon, dayHorizon, dayBright), duskHorizon, isDusk);
    } else {
        topColor     = mix(nightTop, dayTop, dayBright);
        midColor     = mix(nightMid, dayMid, dayBright);
        horizonColor = mix(nightHorizon, dayHorizon, dayBright);
    }

    vec3 sky;
    if (y > 0.5) {
        sky = mix(midColor, topColor, (y - 0.5) * 2.0);
    } else {
        sky = mix(horizonColor, midColor, y * 2.0);
    }

    // Animated swirling cloud wisps (stronger during day)
    float cloudAlpha = mix(0.3, 1.0, dayBright);
    float cx = vUV.x * 6.0 + uTime * 0.15;
    float cy = vUV.y * 3.0 + uTime * 0.08;
    float cloud = sin(cx) * cos(cy + sin(cx * 0.5)) * 0.5 + 0.5;
    cloud *= smoothstep(0.25, 0.65, y);
    sky += cloud * vec3(0.03, 0.03, 0.04) * cloudAlpha;

    // Dark swirl center (above tornado)
    float swirl = sin(vUV.x * 10.0 + uTime * 0.5 + vUV.y * 5.0)
                * cos(vUV.y * 8.0 - uTime * 0.3 + vUV.x * 3.0) * 0.5 + 0.5;
    float centerDist = length(vUV - vec2(0.5, 0.75));
    float swirlMask = smoothstep(0.45, 0.05, centerDist);
    sky += swirl * swirlMask * vec3(0.04, 0.04, 0.06);

    // Stars at night
    if (dayBright < 0.3) {
        float starField = fract(sin(dot(floor(vUV * 200.0), vec2(12.9898, 78.233))) * 43758.5453);
        float star = step(0.997, starField) * (1.0 - dayBright * 3.3);
        sky += vec3(star * 0.8);
    }

    // Sun glow (near horizon during day)
    if (dayBright > 0.05) {
        float sunAngle = (uTimeOfDay - 0.25) * 6.28318;
        vec2 sunPos = vec2(0.5 + cos(sunAngle) * 0.3, 0.3 + sin(sunAngle) * 0.4);
        float sunDist = length(vUV - sunPos);
        float sunGlow = smoothstep(0.25, 0.0, sunDist) * dayBright * 0.4;
        sky += vec3(sunGlow * 1.2, sunGlow * 0.9, sunGlow * 0.4);
    }

    // Lightning flash
    sky += vec3(uLightningFlash * 0.85);

    FragColor = vec4(sky, 1.0);
}
