#version 460
#include "surface.glsl"

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 star;
    vec4 starEmission;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 vertexSurface;
layout(location = 3) in vec4 vertexPhysical;
layout(location = 4) in vec4 instancePlacement;
layout(location = 5) in vec4 instanceRotation;
layout(location = 6) in vec4 instanceSurface;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec3 fragAlbedo;
layout(location = 3) out vec3 fragEmission;
layout(location = 4) out vec2 fragPhysical;
layout(location = 5) out vec3 fragObject;
layout(location = 6) out vec3 fragGlow;
layout(location = 7) out vec3 fragObjectNormal;
layout(location = 8) out vec3 fragDetail;

vec3 turn(vec4 rotation, vec3 value) {
    return value + 2.0 * cross(rotation.xyz, cross(rotation.xyz, value) + rotation.w * value);
}

void main() {
    vec3 world = turn(instanceRotation, position) * instancePlacement.w + instancePlacement.xyz;
    gl_Position = viewProjection * vec4(world, 1.0);
    vec3 albedo;
    vec3 emission;
    materialOf(uint(instanceSurface.x), albedo, emission);
    float metallic = 0.0;
    float roughness = 1.0;
    vec2 detail = vec2(0.0);
    if (vertexSurface.w >= 0.0) {
        albedo = vertexSurface.rgb;
        emission = vertexSurface.rgb * vertexSurface.w;
        metallic = vertexPhysical.x;
        roughness = vertexPhysical.y;
        detail = vertexPhysical.zw;
    }
    worldPosition = world;
    worldNormal = turn(instanceRotation, normal);
    fragAlbedo = albedo;
    fragEmission = emission;
    fragPhysical = vec2(metallic, roughness);
    fragObject = position;
    fragGlow = vec3(instanceSurface.zw, instanceSurface.y);
    fragObjectNormal = normal;
    fragDetail = vec3(detail, instancePlacement.w);
}
