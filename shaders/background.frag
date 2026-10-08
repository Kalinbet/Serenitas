#version 460
#include "surface.glsl"

layout(push_constant) uniform Push {
    vec4 right;
    vec4 up;
    vec4 forward;
};

layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 direction = normalize(forward.xyz + right.xyz * (screen.x * right.w) - up.xyz * (screen.y * up.w));
    outColor = vec4(tonemap(starfield(direction), forward.w), 1.0);
}
