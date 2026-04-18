#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in float aAlpha;

uniform mat4 uProj;
uniform mat4 uView;

out float vAlpha;

void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    vAlpha = aAlpha;
    gl_PointSize = 1.5;
}
