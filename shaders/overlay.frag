#version 460

layout(binding = 0) uniform sampler2D glyphAtlas;

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

layout(location = 0) in vec2 fragPixel;
layout(location = 1) in vec2 fragUv;
layout(location = 2) in vec4 fragTint;
layout(location = 0) out vec4 outColour;

float roundedBox(vec2 point, vec2 halfSize, float radius) {
    vec2 span = abs(point) - halfSize + radius;
    return min(max(span.x, span.y), 0.0) + length(max(span, 0.0)) - radius;
}

float segmentField(vec2 point, vec2 from, vec2 to) {
    vec2 span = to - from;
    float along = clamp(dot(point - from, span) / max(dot(span, span), 1.0e-6), 0.0, 1.0);
    return length(point - (from + span * along));
}

void main() {
    uint mode = uint(shape.w);
    float feather = max(shape.z, 0.5);
    vec4 colour = fragTint;
    float coverage = 1.0;
    if (mode == 2u) {
        vec4 stamp = texture(glyphAtlas, fragUv);
        colour.rgb *= stamp.rgb;
        coverage = stamp.a;
    } else if (mode == 3u) {
        vec2 span = rect.zw - rect.xy;
        float reach = max(length(span), 1.0e-6);
        float along = dot(fragPixel - rect.xy, span / reach);
        coverage = smoothstep(feather, -feather, segmentField(fragPixel, rect.xy, rect.zw) - shape.y);
        colour = mix(tone, accent, clamp(along / reach, 0.0, 1.0));
        if (frame.w > 0.0) {
            coverage *= smoothstep(0.58, 0.42, fract(along / frame.w));
        }
    } else if (mode != 4u) {
        vec2 centre = (rect.xy + rect.zw) * 0.5;
        vec2 halfSize = abs(rect.zw - rect.xy) * 0.5;
        float field = roundedBox(fragPixel - centre, halfSize, min(shape.x, min(halfSize.x, halfSize.y)));
        if (mode == 1u) {
            coverage = smoothstep(feather, -feather, field) *
                       smoothstep(-shape.y - feather, -shape.y + feather, field);
        } else if (mode == 5u) {
            coverage = 1.0 - smoothstep(0.0, max(frame.z, 1.0), max(field, 0.0));
            coverage *= coverage;
        } else {
            coverage = smoothstep(feather, -feather, field);
            float axis = frame.w > 0.5
                ? (fragPixel.x - min(rect.x, rect.z)) / max(abs(rect.z - rect.x), 1.0e-6)
                : (fragPixel.y - min(rect.y, rect.w)) / max(abs(rect.w - rect.y), 1.0e-6);
            colour = mix(tone, accent, clamp(axis, 0.0, 1.0));
        }
    }
    float alpha = colour.a * coverage;
    if (alpha <= 0.002) {
        discard;
    }
    outColour = vec4(colour.rgb, alpha);
}
