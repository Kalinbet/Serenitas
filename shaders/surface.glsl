vec3 tonemap(vec3 colour, float exposure) {
    vec3 x = colour * exposure;
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void materialOf(uint index, out vec3 albedo, out vec3 emission) {
    albedo = vec3(0.75);
    emission = vec3(0.0);
    if (index == 1u) {
        albedo = vec3(0.90, 0.25, 0.20);
    } else if (index == 2u) {
        albedo = vec3(0.25, 0.85, 0.30);
    } else if (index == 3u) {
        albedo = vec3(0.30, 0.40, 1.00);
    } else if (index == 4u) {
        albedo = vec3(0.20, 1.00, 1.00);
    } else if (index == 5u) {
        albedo = vec3(0.08, 0.35, 0.35);
    } else if (index == 6u) {
        albedo = vec3(0.0);
        emission = vec3(36000.0, 28000.0, 19000.0);
    } else if (index == 7u) {
        albedo = vec3(0.55, 0.44, 0.32);
    } else if (index == 8u) {
        albedo = vec3(0.38, 0.52, 0.68);
    }
}

/* ---- procedural surface detail ------------------------------------------
   Models carry no texture coordinates, so plating is generated from the
   object-space hit point scaled to world size. Working in object space keeps
   the pattern locked to the hull as the ship moves; scaling by the instance
   size keeps a plate the same physical size on a fighter and on a station. */

uint plateHash(ivec2 cell, uint axis) {
    uint value = uint(cell.x + 4096) * 73856093u ^ uint(cell.y + 4096) * 19349663u ^ (axis + 1u) * 83492791u;
    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    return value ^ (value >> 16);
}

/* x = plate-to-plate tone in -1..1, y = seam occlusion in 0..1 */
vec2 plateField(vec2 coord, uint axis, float seamWidth, vec2 stagger) {
    coord.x += stagger.x * floor(coord.y);
    vec2 cell = floor(coord);
    vec2 local = coord - cell;
    uint seed = plateHash(ivec2(cell), axis);
    float tone = float(seed & 255u) / 255.0 - 0.5;
    vec2 edge = min(local, 1.0 - local);
    float seam = 1.0 - smoothstep(0.0, seamWidth, min(edge.x, edge.y) * stagger.y);
    return vec2(tone * 2.0, seam);
}

float speckle(vec3 point, float frequency) {
    ivec3 cell = ivec3(floor(point * frequency));
    uint seed = plateHash(cell.xy, uint(cell.z & 1023));
    return float(seed & 1023u) / 1023.0 - 0.5;
}

/* Plate coordinates run one unit per plate, so every frequency below is read
   in plates rather than in metres and stays put when a hull is rescaled. */

void surfaceDetail(vec3 objectPoint, vec3 objectNormal, float pattern, float plate, float span,
                   inout vec3 albedo, inout float roughness) {
    if (pattern < 0.5 || plate <= 0.0 || span <= 0.0) {
        return;
    }
    vec3 point = objectPoint * span / plate;
    vec3 weight = abs(normalize(objectNormal));
    weight *= weight;
    weight *= weight;
    weight /= max(weight.x + weight.y + weight.z, 1e-4);

    float tone = 0.0;
    float seam = 0.0;
    if (pattern < 2.5) {
        /* plating and decking: staggered rectangular panels on every face */
        float seamWidth = pattern < 1.5 ? 0.045 : 0.070;
        vec2 stagger = pattern < 1.5 ? vec2(0.5, 1.0) : vec2(0.25, 1.6);
        vec2 xField = plateField(point.zy, 0u, seamWidth, stagger);
        vec2 yField = plateField(point.xz, 1u, seamWidth, stagger);
        vec2 zField = plateField(point.xy, 2u, seamWidth, stagger);
        tone = dot(vec3(xField.x, yField.x, zField.x), weight);
        seam = dot(vec3(xField.y, yField.y, zField.y), weight);
    } else if (pattern < 3.5) {
        /* ribbed: one-dimensional grooves running along the hull */
        float ribs = fract(dot(point, vec3(0.62, 0.31, 0.72)));
        seam = 1.0 - smoothstep(0.0, 0.30, min(ribs, 1.0 - ribs));
        tone = 0.70 * speckle(point, 0.7);
    } else if (pattern < 4.5) {
        /* banded trim: fine machined rings, no plate breaks */
        float bands = fract(dot(point, vec3(0.35, 0.94, 0.28)));
        seam = 0.55 * (1.0 - smoothstep(0.0, 0.42, min(bands, 1.0 - bands)));
        tone = 0.80 * speckle(point, 0.9);
    } else {
        /* stone: blotches across two scales, no seams at all */
        tone = 1.30 * speckle(point, 0.34) + 0.70 * speckle(point + 11.7, 1.05);
    }

    float grain = speckle(point, 2.6);
    albedo *= clamp(1.0 + tone * 0.16 + grain * 0.07 - seam * 0.46, 0.05, 1.6);
    roughness = clamp(roughness + seam * 0.22 + tone * 0.10 + grain * 0.05, 0.03, 1.0);
}

const vec3 AMBIENT = vec3(0.040, 0.046, 0.060);

vec3 ambientLight(vec3 albedo, float metallic) {
    vec3 reflectance = mix(vec3(0.04), albedo, metallic);
    return (albedo * (1.0 - metallic) + reflectance) * AMBIENT;
}

vec3 brdf(vec3 albedo, float metallic, float roughness, vec3 normal, vec3 outgoing, vec3 incoming) {
    float cosOut = dot(normal, outgoing);
    float cosIn = dot(normal, incoming);
    if (cosIn <= 0.0 || cosOut <= 0.0) {
        return vec3(0.0);
    }
    vec3 halfway = normalize(outgoing + incoming);
    float width = max(roughness * roughness, 0.004);
    float widthSquared = width * width;
    float cosHalf = max(dot(normal, halfway), 0.0);
    float denominator = cosHalf * cosHalf * (widthSquared - 1.0) + 1.0;
    float distribution = widthSquared / (3.14159265 * denominator * denominator);
    float occlusion = width * 0.5;
    float shadowOut = cosOut / (cosOut * (1.0 - occlusion) + occlusion);
    float shadowIn = cosIn / (cosIn * (1.0 - occlusion) + occlusion);
    vec3 reflectance = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = reflectance + (1.0 - reflectance) * pow(1.0 - max(dot(halfway, incoming), 0.0), 5.0);
    vec3 specular = distribution * shadowOut * shadowIn * fresnel / (4.0 * cosOut * cosIn);
    vec3 diffuse = albedo * (1.0 - metallic) * (1.0 - fresnel) / 3.14159265;
    return (diffuse + specular) * cosIn;
}

uint starHash(uvec3 cell) {
    uint value = cell.x * 73856093u ^ cell.y * 19349663u ^ cell.z * 83492791u;
    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    return value ^ (value >> 16);
}

vec3 starfield(vec3 direction) {
    vec3 scaled = direction * 240.0;
    vec3 cell = floor(scaled);
    uint seed = starHash(uvec3(ivec3(cell) + 512));
    if ((seed & 15u) != 0u) {
        return vec3(0.0);
    }
    vec3 offset = vec3(float((seed >> 4) & 255u), float((seed >> 12) & 255u), float((seed >> 20) & 255u)) / 255.0;
    float distance = length(scaled - cell - offset);
    float point = exp(-distance * distance * 34.0);
    float brightness = 0.25 + 2.4 * float((seed >> 28) & 15u) / 15.0;
    float warmth = float((seed >> 24) & 15u) / 15.0;
    vec3 tint = mix(vec3(0.72, 0.82, 1.00), vec3(1.00, 0.86, 0.68), warmth);
    return tint * (point * brightness);
}

float beamPulse(float along, float flow) {
    float wave = sin((along - flow) * 6.28318531);
    return 0.74 + 0.48 * wave * wave;
}

vec3 animateEmission(vec3 emission, float flow, float along) {
    if (flow >= 1.0) {
        return emission * beamPulse(along, flow - 1.0);
    }
    return emission;
}
