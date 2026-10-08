#version 460

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 shape;
    vec4 tone;
    vec4 accent;
    vec4 atlas;
    vec4 frame;
    vec4 cornerTop;
    vec4 cornerBottom;
};

layout(location = 0) out vec2 fragPixel;
layout(location = 1) out vec2 fragUv;
layout(location = 2) out vec4 fragTint;

const vec2 pattern[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 corner = pattern[gl_VertexIndex];
    uint mode = uint(shape.w);
    vec2 pixel;
    if (mode == 4u) {
        vec2 upper = mix(cornerTop.xy, cornerTop.zw, corner.x);
        vec2 lower = mix(cornerBottom.xy, cornerBottom.zw, corner.x);
        pixel = mix(upper, lower, corner.y);
        fragTint = mix(tone, accent, corner.y);
    } else {
        float pad = shape.z + frame.z + (mode == 3u ? shape.y : 0.0) + 1.0;
        vec2 lower = min(rect.xy, rect.zw) - pad;
        vec2 upper = max(rect.xy, rect.zw) + pad;
        pixel = mix(lower, upper, corner);
        fragTint = tone;
    }
    fragPixel = pixel;
    fragUv = mix(atlas.xy, atlas.zw, corner);
    gl_Position = vec4(pixel / frame.xy * 2.0 - 1.0, 0.0, 1.0);
}
