#version 460
#include "surface.glsl"

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 star;
    vec4 starEmission;
};

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 fragAlbedo;
layout(location = 3) in vec3 fragEmission;
layout(location = 4) in vec2 fragPhysical;
layout(location = 5) in vec3 fragObject;
layout(location = 6) in vec3 fragGlow;
layout(location = 7) in vec3 fragObjectNormal;
layout(location = 8) in vec3 fragDetail;
layout(location = 0) out vec4 outColor;

void main() {
    if (fragObject.y > fragGlow.z) {
        discard;
    }
    vec3 emission = animateEmission(fragEmission * fragGlow.x, fragGlow.y, fragObject.x);
    vec3 normal = normalize(worldNormal);
    vec3 albedo = fragAlbedo;
    float roughness = fragPhysical.y;
    surfaceDetail(fragObject, fragObjectNormal, fragDetail.x, fragDetail.y, fragDetail.z, albedo, roughness);
    vec3 toStar = star.xyz - worldPosition;
    float distanceSquared = max(dot(toStar, toStar), 0.001);
    vec3 lightDirection = toStar * inversesqrt(distanceSquared);
    float solidAngle = 3.14159265 * star.w * star.w / distanceSquared;
    vec3 outgoing = normalize(-worldPosition);
    vec3 light = starEmission.rgb * solidAngle * brdf(albedo, fragPhysical.x, roughness, normal, outgoing, lightDirection);
    outColor = vec4(tonemap(light + emission + ambientLight(albedo, fragPhysical.x), starEmission.w), 1.0);
}
