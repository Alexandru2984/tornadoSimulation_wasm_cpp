#version 330 core
in vec3 vCol;
in vec3 vNormal;
in vec3 vWorldPos;
in float vHeight;
in vec2 vUV;
out vec4 FragColor;

uniform vec3 uTint;
uniform vec3 uCamPos;
uniform float uOpacity;
uniform int uObjType; // 0=default,1=house,2=tree,3=ground,4=tornado,5=debris,6=water
uniform int uHasAlbedo;
uniform sampler2D uAlbedo;
uniform float uLightningFlash;
uniform float uTimeOfDay;  // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset
uniform float uTime;
uniform float uWaterLevel;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);

    // ── Day/night sun direction ──
    float sunAngle = (uTimeOfDay - 0.25) * 6.28318;
    vec3 lightDir = normalize(vec3(cos(sunAngle) * 0.6, -sin(sunAngle), -0.3));

    // Day brightness factor
    float sunHeight = sin((uTimeOfDay - 0.25) * 3.14159);
    float dayBright = clamp(sunHeight, 0.0, 1.0);
    float ambientBase = mix(0.04, 0.14, dayBright);

    float diff = max(dot(N, -lightDir), 0.0) * dayBright;
    float spec = pow(max(dot(reflect(lightDir, N), V), 0.0), 24.0) * 0.28 * dayBright;
    vec3 ambient = (ambientBase + uLightningFlash * 0.6) * (vCol * (uTint * 0.95));
    vec3 diffuse = 1.0 * diff * (vCol * (uTint * 0.95));
    vec3 final_col = ambient + diffuse + spec;

    if (uHasAlbedo == 1) {
        vec3 al = texture(uAlbedo, vUV).rgb;
        final_col *= al;
    }

    // Sunset/sunrise warm tint
    float horizonFactor = 1.0 - abs(sunHeight);
    float warmth = smoothstep(0.3, 0.8, horizonFactor) * dayBright * 0.5;
    final_col += vec3(warmth * 0.3, warmth * 0.1, -warmth * 0.05);

    float heightFactor = smoothstep(0.0, 6.0, vHeight);
    float desat = mix(0.2, 0.9, heightFactor);
    vec3 gray = vec3(dot(final_col, vec3(0.299,0.587,0.114)));
    vec3 tornadoFinal = mix(gray, final_col, desat);

    vec3 outCol = final_col;
    float outAlpha = 1.0;

    if (uObjType == 1) {
        // house
        if (vHeight > 1.02) {
            outCol = vec3(0.45, 0.2, 0.16) * (vCol * uTint);
        } else {
            vec2 m = vWorldPos.xz * 2.0;
            float bricks = step(0.0, sin(m.x*3.0)*sin(m.y*6.0));
            outCol = mix(vec3(0.9,0.78,0.6), vec3(0.78,0.6,0.45), bricks) * (vCol * uTint);
            if (vNormal.z < -0.3 && vHeight > 0.5 && vHeight < 0.9) {
                outCol *= 0.35;
            }
            if (vNormal.z < -0.3 && vHeight < 0.5 && abs(vWorldPos.x - 0.0) < 0.18) {
                outCol = mix(outCol, vec3(0.2,0.12,0.08), 0.9);
            }
        }
        if (dayBright < 0.3 && vNormal.z < -0.3 && vHeight > 0.5 && vHeight < 0.9) {
            outCol += vec3(0.35, 0.3, 0.1) * (1.0 - dayBright);
        }
        outCol *= mix(0.3, 1.0, dayBright * 0.8 + 0.2);
        outAlpha = clamp(1.0 * uOpacity, 0.9, 1.0);

    } else if (uObjType == 2) {
        // tree
        if (vHeight < 0.35) {
            outCol = vec3(0.38,0.25,0.14) * (vCol * uTint);
        } else {
            float f = smoothstep(0.35, 1.2, vHeight);
            outCol = mix(vec3(0.08,0.28,0.08), vec3(0.12,0.45,0.12), f) * (vCol * uTint);
        }
        outCol *= mix(0.25, 1.0, dayBright * 0.8 + 0.2);
        outAlpha = clamp(1.0 * uOpacity, 0.85, 1.0);

    } else if (uObjType == 3) {
        // terrain ground
        outCol = vCol * uTint;
        float gr = fract(vWorldPos.x * 0.15) * fract(vWorldPos.z * 0.15);
        outCol *= 0.85 + 0.15 * gr;
        outCol = ambient + diff * outCol + spec * 0.1;
        outCol *= mix(0.2, 1.0, dayBright * 0.85 + 0.15);
        outAlpha = clamp(1.0 * uOpacity, 0.95, 1.0);

    } else if (uObjType == 5) {
        // debris
        outCol = final_col;
        outAlpha = uOpacity;

    } else if (uObjType == 6) {
        // water
        float wx = vWorldPos.x * 0.8 + uTime * 1.2;
        float wz = vWorldPos.z * 0.6 + uTime * 0.9;
        vec3 waveN = normalize(vec3(
            cos(wx) * 0.08 + cos(wz * 1.3 + uTime * 0.7) * 0.05,
            1.0,
            sin(wz) * 0.08 + sin(wx * 0.9 + uTime * 1.1) * 0.05
        ));
        float wDiff = max(dot(waveN, -lightDir), 0.0) * dayBright;
        float wSpec = pow(max(dot(reflect(lightDir, waveN), V), 0.0), 64.0) * 0.6 * dayBright;

        vec3 deepCol  = vec3(0.03, 0.08, 0.18);
        vec3 shallowCol = vec3(0.1, 0.3, 0.45);
        float depthFactor = smoothstep(-2.0, 0.0, vWorldPos.y - uWaterLevel);
        vec3 waterBase = mix(deepCol, shallowCol, depthFactor);

        float fresnel = pow(1.0 - max(dot(waveN, V), 0.0), 3.0);
        vec3 skyReflect = mix(vec3(0.15, 0.2, 0.3), vec3(0.4, 0.5, 0.7), dayBright);
        waterBase = mix(waterBase, skyReflect, fresnel * 0.5);

        outCol = waterBase * (ambientBase + 0.3) + waterBase * wDiff * 0.7 + vec3(wSpec);
        outCol *= mix(0.3, 1.0, dayBright * 0.8 + 0.2);
        outCol += vec3(uLightningFlash * 0.4);
        outAlpha = uOpacity;

    } else {
        // tornado
        float fres = pow(1.0 - max(dot(N, V), 0.0), 1.6);
        float baseAlpha = 1.0 - smoothstep(0.0, 6.0, vHeight);
        baseAlpha *= (1.0 - 0.55 * heightFactor);
        outAlpha = clamp(baseAlpha * (0.6 + 0.3 * fres) * uOpacity, 0.02, 1.0);
        float fog = smoothstep(8.0, 28.0, length(uCamPos - vWorldPos));
        outCol = mix(tornadoFinal, vec3(0.06,0.06,0.08), fog);
    }

    // Distance fog
    float fogDist = length(uCamPos - vWorldPos);
    float fogFactor = smoothstep(80.0, 180.0, fogDist);
    vec3 fogColor = mix(vec3(0.02, 0.02, 0.04), vec3(0.12, 0.12, 0.16), dayBright);
    outCol = mix(outCol, fogColor, fogFactor);

    FragColor = vec4(outCol, outAlpha);
}
