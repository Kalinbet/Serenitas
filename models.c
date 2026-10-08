#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"

#define STRIDE 14
#define MAX_VERTICES 40000
#define MAX_INDICES 120000
#define MAX_RING 40
#define VERTEX_SLOTS 65536
#define NO_VERTEX 0xFFFFFFFFu

/* Procedural surface families resolved in shaders/surface.glsl. */
#define SURFACE_SMOOTH 0.0
#define SURFACE_PLATING 1.0
#define SURFACE_DECKING 2.0
#define SURFACE_RIBBED 3.0
#define SURFACE_BANDED 4.0
#define SURFACE_STONE 5.0

typedef struct {
    double colour[3];
    double emissive;
    double metallic;
    double roughness;
    double pattern;
    double plate;
} Material;

typedef struct {
    double x;
    double y;
    double z;
    double radiusY;
    double radiusZ;
} Section;

static float vertexData[MAX_VERTICES * STRIDE];
static uint32_t vertexCount;
static uint32_t indexData[MAX_INDICES];
static uint32_t indexCount;
static uint32_t vertexHead[VERTEX_SLOTS];
static uint32_t vertexNext[MAX_VERTICES];

static const Material hull = {{0.46, 0.49, 0.54}, 0.0, 0.6, 0.42, SURFACE_PLATING, 5.6};
static const Material plate = {{0.27, 0.29, 0.34}, 0.0, 0.7, 0.52, SURFACE_DECKING, 3.0};
static const Material trim = {{0.62, 0.58, 0.48}, 0.0, 0.90, 0.24, SURFACE_BANDED, 1.1};
static const Material dark = {{0.12, 0.13, 0.15}, 0.0, 0.35, 0.60, SURFACE_RIBBED, 0.85};
static const Material glass = {{0.05, 0.08, 0.13}, 0.0, 0.9, 0.07, SURFACE_SMOOTH, 0.0};
static const Material glow = {{0.34, 0.62, 1.00}, 3.0, 0.0, 0.50, SURFACE_SMOOTH, 0.0};
static const Material lamp = {{0.76, 0.86, 1.00}, 1.05, 0.1, 0.60, SURFACE_SMOOTH, 0.0};
static const Material recess = {{0.028, 0.030, 0.036}, 0.0, 0.0, 0.94, SURFACE_RIBBED, 1.4};
static const Material burn = {{1.00, 0.48, 0.16}, 2.6, 0.0, 0.50, SURFACE_SMOOTH, 0.0};
static const Material owner = {{0.0, 0.0, 0.0}, -1.0, 1.0, 0.6, SURFACE_SMOOTH, 0.0};

/* Plate size is set in world units, so a fighter would wear the same metre-wide
   plates as the station and end up looking like a handful of scattered
   rectangles. Small hulls shrink theirs instead. Structural trim and grating
   already carry their own scale and are left alone. */
static double liveryScale;

static void livery(double scale) {
    liveryScale = scale;
}

static uint32_t vertexSlot(const float* entry) {
    const unsigned char* bytes = (const unsigned char*)entry;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < sizeof(float) * STRIDE; i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash & (VERTEX_SLOTS - 1);
}

static uint32_t pushVertex(const double* position, const double* normal, Material material) {
    double plate = material.plate;
    if (liveryScale > 0.0 && (material.pattern == SURFACE_PLATING || material.pattern == SURFACE_DECKING)) {
        plate *= liveryScale;
    }
    float entry[STRIDE] = {
        (float)position[0], (float)position[1], (float)position[2],
        (float)normal[0], (float)normal[1], (float)normal[2],
        (float)material.colour[0], (float)material.colour[1], (float)material.colour[2], (float)material.emissive,
        (float)material.metallic, (float)material.roughness, (float)material.pattern, (float)plate
    };
    uint32_t slot = vertexSlot(entry);
    for (uint32_t i = vertexHead[slot]; i != NO_VERTEX; i = vertexNext[i]) {
        if (memcmp(vertexData + (size_t)i * STRIDE, entry, sizeof(entry)) == 0) {
            return i;
        }
    }
    memcpy(vertexData + (size_t)vertexCount * STRIDE, entry, sizeof(entry));
    vertexNext[vertexCount] = vertexHead[slot];
    vertexHead[slot] = vertexCount;
    return vertexCount++;
}

static void pushTriangle(const double* a, const double* b, const double* c, const double* inside,
                         const double* normalA, const double* normalB, const double* normalC,
                         Material materialA, Material materialB, Material materialC) {
    double edge0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    double edge1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double face[3] = {
        edge0[1] * edge1[2] - edge0[2] * edge1[1],
        edge0[2] * edge1[0] - edge0[0] * edge1[2],
        edge0[0] * edge1[1] - edge0[1] * edge1[0]
    };
    double length = sqrt(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]);
    if (length < 1e-12) {
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        face[i] /= length;
    }
    double centre[3] = {(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0};
    double away = face[0] * (centre[0] - inside[0]) + face[1] * (centre[1] - inside[1]) + face[2] * (centre[2] - inside[2]);
    int flip = away < 0.0;
    if (flip) {
        for (uint32_t i = 0; i < 3; i++) {
            face[i] = -face[i];
        }
    }
    const double* normals[3] = {normalA ? normalA : face, normalB ? normalB : face, normalC ? normalC : face};
    double resolved[3][3];
    for (uint32_t corner = 0; corner < 3; corner++) {
        double along = normals[corner][0] * face[0] + normals[corner][1] * face[1] + normals[corner][2] * face[2];
        for (uint32_t i = 0; i < 3; i++) {
            resolved[corner][i] = along < 0.0 ? -normals[corner][i] : normals[corner][i];
        }
    }
    const double* corners[3] = {a, b, c};
    const Material materials[3] = {materialA, materialB, materialC};
    uint32_t order[3] = {0, 1, 2};
    if (flip) {
        order[1] = 2;
        order[2] = 1;
    }
    for (uint32_t i = 0; i < 3; i++) {
        indexData[indexCount++] = pushVertex(corners[order[i]], resolved[order[i]], materials[order[i]]);
    }
}

static void ringAround(const Section* section, uint32_t sides, double phase, double* out) {
    for (uint32_t i = 0; i < sides; i++) {
        double angle = phase + 6.283185307179586 * (double)i / (double)sides;
        out[i * 3 + 0] = section->x;
        out[i * 3 + 1] = section->y + section->radiusY * cos(angle);
        out[i * 3 + 2] = section->z + section->radiusZ * sin(angle);
    }
}

static void ringCentre(const double* ring, uint32_t sides, double* out) {
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    for (uint32_t i = 0; i < sides; i++) {
        for (uint32_t axis = 0; axis < 3; axis++) {
            out[axis] += ring[i * 3 + axis];
        }
    }
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] /= (double)sides;
    }
}

static void radialNormal(const double* point, const double* centre, double* out) {
    double offset[3] = {point[0] - centre[0], point[1] - centre[1], point[2] - centre[2]};
    double length = sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    if (length < 1e-9) {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
        return;
    }
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] = offset[axis] / length;
    }
}

static void loft(const double* ringA, const double* ringB, uint32_t sides, int rounded, Material materialA, Material materialB) {
    double centreA[3];
    double centreB[3];
    ringCentre(ringA, sides, centreA);
    ringCentre(ringB, sides, centreB);
    double inside[3] = {
        (centreA[0] + centreB[0]) * 0.5,
        (centreA[1] + centreB[1]) * 0.5,
        (centreA[2] + centreB[2]) * 0.5
    };
    for (uint32_t i = 0; i < sides; i++) {
        uint32_t j = (i + 1) % sides;
        const double* a0 = ringA + i * 3;
        const double* a1 = ringA + j * 3;
        const double* b0 = ringB + i * 3;
        const double* b1 = ringB + j * 3;
        double normalA0[3];
        double normalA1[3];
        double normalB0[3];
        double normalB1[3];
        if (rounded) {
            radialNormal(a0, centreA, normalA0);
            radialNormal(a1, centreA, normalA1);
            radialNormal(b0, centreB, normalB0);
            radialNormal(b1, centreB, normalB1);
        }
        int useA0 = rounded && (normalA0[0] != 0.0 || normalA0[1] != 0.0 || normalA0[2] != 0.0);
        int useA1 = rounded && (normalA1[0] != 0.0 || normalA1[1] != 0.0 || normalA1[2] != 0.0);
        int useB0 = rounded && (normalB0[0] != 0.0 || normalB0[1] != 0.0 || normalB0[2] != 0.0);
        int useB1 = rounded && (normalB1[0] != 0.0 || normalB1[1] != 0.0 || normalB1[2] != 0.0);
        pushTriangle(a0, a1, b1, inside, useA0 ? normalA0 : NULL, useA1 ? normalA1 : NULL, useB1 ? normalB1 : NULL,
                     materialA, materialA, materialB);
        pushTriangle(a0, b1, b0, inside, useA0 ? normalA0 : NULL, useB1 ? normalB1 : NULL, useB0 ? normalB0 : NULL,
                     materialA, materialB, materialB);
    }
}

static void capRing(const double* ring, uint32_t sides, const double* inside, Material material) {
    double centre[3];
    ringCentre(ring, sides, centre);
    for (uint32_t i = 0; i < sides; i++) {
        uint32_t j = (i + 1) % sides;
        pushTriangle(centre, ring + i * 3, ring + j * 3, inside, NULL, NULL, NULL, material, material, material);
    }
}

static void body(const Section* sections, uint32_t count, uint32_t sides, double phase, int rounded,
                 int capFront, int capBack, Material material);

static void rib(double x, double halfWidth, double y, double z, double radiusY, double radiusZ, double bulge,
                uint32_t sides, double phase, Material material) {
    double bite = bulge * 0.5;
    Section band[4] = {
        {x - halfWidth, y, z, radiusY - bite, radiusZ - bite},
        {x - halfWidth * 0.35, y, z, radiusY + bulge, radiusZ + bulge},
        {x + halfWidth * 0.35, y, z, radiusY + bulge, radiusZ + bulge},
        {x + halfWidth, y, z, radiusY - bite, radiusZ - bite}
    };
    body(band, 4, sides, phase, 1, 0, 0, material);
}

static void body(const Section* sections, uint32_t count, uint32_t sides, double phase, int rounded,
                 int capFront, int capBack, Material material) {
    double rings[2][MAX_RING * 3];
    ringAround(&sections[0], sides, phase, rings[0]);
    double axis[3] = {0.0, 0.0, 0.0};
    for (uint32_t i = 0; i < count; i++) {
        axis[0] += sections[i].x / (double)count;
        axis[1] += sections[i].y / (double)count;
        axis[2] += sections[i].z / (double)count;
    }
    if (capBack && sections[0].radiusY > 1e-9) {
        double inside[3] = {sections[0].x + fabs(sections[count - 1].x - sections[0].x), sections[0].y, sections[0].z};
        capRing(rings[0], sides, inside, material);
    }
    for (uint32_t i = 1; i < count; i++) {
        ringAround(&sections[i], sides, phase, rings[i & 1]);
        loft(rings[(i - 1) & 1], rings[i & 1], sides, rounded, material, material);
    }
    if (capFront && sections[count - 1].radiusY > 1e-9) {
        double inside[3] = {
            sections[count - 1].x - fabs(sections[count - 1].x - sections[0].x),
            sections[count - 1].y, sections[count - 1].z
        };
        capRing(rings[(count - 1) & 1], sides, inside, material);
    }
}

static void plank(double leadX, double trailX, double innerZ, double outerZ, double innerY, double outerY,
                  double innerThickness, double outerThickness, double leadSweep, double trailSweep,
                  Material material) {
    double innerRing[12] = {
        leadX, innerY, innerZ,
        (leadX + trailX) * 0.5, innerY + innerThickness, innerZ,
        trailX, innerY, innerZ,
        (leadX + trailX) * 0.5, innerY - innerThickness, innerZ
    };
    double outerRing[12] = {
        leadX + leadSweep, outerY, outerZ,
        (leadX + leadSweep + trailX + trailSweep) * 0.5, outerY + outerThickness, outerZ,
        trailX + trailSweep, outerY, outerZ,
        (leadX + leadSweep + trailX + trailSweep) * 0.5, outerY - outerThickness, outerZ
    };
    loft(innerRing, outerRing, 4, 0, material, material);
    double centre[3];
    ringCentre(innerRing, 4, centre);
    double outward[3] = {centre[0], centre[1], centre[2] + (outerZ - innerZ)};
    capRing(innerRing, 4, outward, material);
    ringCentre(outerRing, 4, centre);
    double inward[3] = {centre[0], centre[1], centre[2] - (outerZ - innerZ)};
    capRing(outerRing, 4, inward, material);
}

static void blade(double rootX, double rootTrailX, double rootY, double tipX, double tipTrailX, double tipY,
                  double rootZ, double tipZ, double thickness, Material material) {
    double rootRing[12] = {
        rootX, rootY, rootZ,
        (rootX + rootTrailX) * 0.5, rootY, rootZ + thickness,
        rootTrailX, rootY, rootZ,
        (rootX + rootTrailX) * 0.5, rootY, rootZ - thickness
    };
    double tipRing[12] = {
        tipX, tipY, tipZ,
        (tipX + tipTrailX) * 0.5, tipY, tipZ + thickness * 0.45,
        tipTrailX, tipY, tipZ,
        (tipX + tipTrailX) * 0.5, tipY, tipZ - thickness * 0.45
    };
    loft(rootRing, tipRing, 4, 0, material, material);
    double centre[3];
    ringCentre(tipRing, 4, centre);
    double below[3] = {centre[0], centre[1] - (tipY - rootY), centre[2]};
    capRing(tipRing, 4, below, material);
}

#define MAX_NOZZLES 8
static double nozzleData[MAX_NOZZLES * 4];
static uint32_t nozzleCount;

#define MAX_HARDPOINTS 8
static double hardpointData[MAX_HARDPOINTS * 6];
static uint32_t hardpointCount;

static void hardpoint(double x, double y, double z, double alongX, double alongY, double alongZ) {
    if (hardpointCount == MAX_HARDPOINTS) {
        return;
    }
    hardpointData[hardpointCount * 6 + 0] = x;
    hardpointData[hardpointCount * 6 + 1] = y;
    hardpointData[hardpointCount * 6 + 2] = z;
    hardpointData[hardpointCount * 6 + 3] = alongX;
    hardpointData[hardpointCount * 6 + 4] = alongY;
    hardpointData[hardpointCount * 6 + 5] = alongZ;
    hardpointCount++;
}

static void strut(const double* from, const double* to, double radius, uint32_t sides, Material material) {
    double axis[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (length < 1e-9) {
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        axis[i] /= length;
    }
    double helper[3] = {0.0, 1.0, 0.0};
    if (axis[1] > 0.9 || axis[1] < -0.9) {
        helper[0] = 1.0;
        helper[1] = 0.0;
    }
    double side[3] = {
        helper[1] * axis[2] - helper[2] * axis[1],
        helper[2] * axis[0] - helper[0] * axis[2],
        helper[0] * axis[1] - helper[1] * axis[0]
    };
    double sideLength = sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    for (uint32_t i = 0; i < 3; i++) {
        side[i] /= sideLength;
    }
    double up[3] = {
        axis[1] * side[2] - axis[2] * side[1],
        axis[2] * side[0] - axis[0] * side[2],
        axis[0] * side[1] - axis[1] * side[0]
    };
    double ringA[MAX_RING * 3];
    double ringB[MAX_RING * 3];
    for (uint32_t i = 0; i < sides; i++) {
        double angle = 6.283185307179586 * (double)i / (double)sides;
        for (uint32_t component = 0; component < 3; component++) {
            double offset = radius * (cos(angle) * side[component] + sin(angle) * up[component]);
            ringA[i * 3 + component] = from[component] + offset;
            ringB[i * 3 + component] = to[component] + offset;
        }
    }
    loft(ringA, ringB, sides, 1, material, material);
    capRing(ringA, sides, to, material);
    capRing(ringB, sides, from, material);
}

static void panel(const double* from, const double* to, const double* wide, double halfWidth, double halfThick,
                  Material material) {
    double axis[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (length < 1e-9) {
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        axis[i] /= length;
    }
    double along = wide[0] * axis[0] + wide[1] * axis[1] + wide[2] * axis[2];
    double flat[3] = {wide[0] - axis[0] * along, wide[1] - axis[1] * along, wide[2] - axis[2] * along};
    double flatLength = sqrt(flat[0] * flat[0] + flat[1] * flat[1] + flat[2] * flat[2]);
    if (flatLength < 1e-9) {
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        flat[i] /= flatLength;
    }
    double thin[3] = {
        axis[1] * flat[2] - axis[2] * flat[1],
        axis[2] * flat[0] - axis[0] * flat[2],
        axis[0] * flat[1] - axis[1] * flat[0]
    };
    static const double corners[4][2] = {{1.0, 1.0}, {1.0, -1.0}, {-1.0, -1.0}, {-1.0, 1.0}};
    double ringA[12];
    double ringB[12];
    for (uint32_t corner = 0; corner < 4; corner++) {
        for (uint32_t component = 0; component < 3; component++) {
            double offset = flat[component] * corners[corner][0] * halfWidth +
                            thin[component] * corners[corner][1] * halfThick;
            ringA[corner * 3 + component] = from[component] + offset;
            ringB[corner * 3 + component] = to[component] + offset;
        }
    }
    loft(ringA, ringB, 4, 0, material, material);
    capRing(ringA, 4, to, material);
    capRing(ringB, 4, from, material);
}

static void hoop(double y, double radius, double tube, uint32_t segments, uint32_t sides, Material material) {
    double rings[2][MAX_RING * 3];
    for (uint32_t i = 0; i <= segments; i++) {
        double turn = 6.283185307179586 * (double)i / (double)segments;
        for (uint32_t side = 0; side < sides; side++) {
            double angle = 6.283185307179586 * (double)side / (double)sides;
            double reach = radius + tube * cos(angle);
            rings[i & 1][side * 3 + 0] = reach * cos(turn);
            rings[i & 1][side * 3 + 1] = y + tube * sin(angle);
            rings[i & 1][side * 3 + 2] = reach * sin(turn);
        }
        if (i) {
            loft(rings[(i - 1) & 1], rings[i & 1], sides, 1, material, material);
        }
    }
}

static void ringVertical(double x, double y, double z, double radius, uint32_t sides, double phase, double* out) {
    for (uint32_t i = 0; i < sides; i++) {
        double angle = phase + 6.283185307179586 * (double)i / (double)sides;
        out[i * 3 + 0] = x + radius * cos(angle);
        out[i * 3 + 1] = y;
        out[i * 3 + 2] = z + radius * sin(angle);
    }
}

static void column(double x, double y, double z, const double* stack, uint32_t count, uint32_t sides,
                   double phase, double sign, int rounded, int capTop, Material material) {
    double rings[2][MAX_RING * 3];
    ringVertical(x, y + stack[0] * sign, z, stack[1], sides, phase, rings[0]);
    for (uint32_t i = 1; i < count; i++) {
        ringVertical(x, y + stack[i * 2] * sign, z, stack[i * 2 + 1], sides, phase, rings[i & 1]);
        loft(rings[(i - 1) & 1], rings[i & 1], sides, rounded, material, material);
    }
    if (capTop) {
        double seat[3] = {x, y, z};
        capRing(rings[(count - 1) & 1], sides, seat, material);
    }
}

static void nozzle(double x, double depth, double y, double z, double outer, double inner, uint32_t sides,
                   Material shell, Material flame) {
    if (nozzleCount < MAX_NOZZLES) {
        nozzleData[nozzleCount * 4 + 0] = x;
        nozzleData[nozzleCount * 4 + 1] = y;
        nozzleData[nozzleCount * 4 + 2] = z;
        nozzleData[nozzleCount * 4 + 3] = inner;
        nozzleCount++;
    }
    Section lip[3] = {
        {x + depth * 0.55, y, z, outer * 0.86, outer * 0.86},
        {x + depth * 0.15, y, z, outer, outer},
        {x, y, z, outer * 0.94, outer * 0.94}
    };
    body(lip, 3, sides, 0.0, 1, 0, 0, shell);
    Section throat[3] = {
        {x + depth * 0.55, y, z, inner * 0.5, inner * 0.5},
        {x + depth * 0.2, y, z, inner * 0.86, inner * 0.86},
        {x - depth * 0.06, y, z, inner, inner}
    };
    body(throat, 3, sides, 0.0, 1, 1, 0, flame);
    double rim[MAX_RING * 3];
    Section mouth = {x, y, z, outer * 0.94, outer * 0.94};
    Section core = {x - depth * 0.06, y, z, inner, inner};
    double outerRing[MAX_RING * 3];
    ringAround(&mouth, sides, 0.0, rim);
    ringAround(&core, sides, 0.0, outerRing);
    loft(rim, outerRing, sides, 0, shell, shell);
}

static void barbette(double x, double y, double z, double sign, double radius) {
    double stack[6] = {-0.014, radius, 0.026, radius * 1.04, 0.044, radius * 0.92};
    column(x, y, z, stack, 3, 12, 0.261799, sign, 0, 0, plate);
    hardpoint(x, y + 0.044 * sign, z, 0.0, sign, 0.0);
}

static void buildInterceptor(void) {
    livery(0.25);
    Section fuselage[8] = {
        {-0.56, 0.006, 0.0, 0.088, 0.100},
        {-0.34, 0.004, 0.0, 0.110, 0.126},
        {-0.05, 0.000, 0.0, 0.122, 0.132},
        { 0.22, 0.000, 0.0, 0.104, 0.110},
        { 0.48, 0.004, 0.0, 0.074, 0.076},
        { 0.68, 0.008, 0.0, 0.046, 0.048},
        { 0.86, 0.010, 0.0, 0.022, 0.023},
        { 1.00, 0.012, 0.0, 0.000, 0.000}
    };
    body(fuselage, 8, 12, 0.261799, 1, 1, 1, hull);
    rib(-0.30, 0.030, 0.004, 0.0, 0.109, 0.125, 0.012, 12, 0.261799, trim);
    rib(0.08, 0.026, 0.000, 0.0, 0.114, 0.122, 0.010, 12, 0.261799, trim);

    Section spine[5] = {
        {-0.44, 0.092, 0.0, 0.044, 0.062},
        {-0.22, 0.116, 0.0, 0.056, 0.070},
        {-0.02, 0.124, 0.0, 0.052, 0.064},
        { 0.18, 0.110, 0.0, 0.036, 0.050},
        { 0.34, 0.084, 0.0, 0.010, 0.020}
    };
    body(spine, 5, 8, 0.392699, 1, 1, 1, plate);

    Section canopy[5] = {
        { 0.08, 0.132, 0.0, 0.038, 0.054},
        { 0.22, 0.144, 0.0, 0.046, 0.058},
        { 0.38, 0.126, 0.0, 0.038, 0.048},
        { 0.54, 0.092, 0.0, 0.020, 0.028},
        { 0.62, 0.070, 0.0, 0.000, 0.000}
    };
    body(canopy, 5, 10, 0.314159, 1, 1, 1, glass);
    rib(0.10, 0.022, 0.132, 0.0, 0.038, 0.054, 0.008, 10, 0.314159, trim);
    rib(0.44, 0.020, 0.113, 0.0, 0.030, 0.040, 0.009, 10, 0.314159, trim);

    for (int side = -1; side <= 1; side += 2) {
        double sign = (double)side;
        plank(0.16, -0.46, 0.084 * sign, 0.86 * sign, -0.004, 0.052, 0.028, 0.010,
              -0.30, -0.04, hull);
        plank(-0.14, -0.50, 0.86 * sign, 0.98 * sign, 0.052, 0.062, 0.010, 0.006,
              -0.02, 0.0, trim);
        plank(0.26, 0.02, 0.086 * sign, 0.30 * sign, 0.030, 0.058, 0.014, 0.008,
              -0.10, -0.02, owner);

        Section nacelle[5] = {
            {-0.62, -0.014, 0.176 * sign, 0.060, 0.058},
            {-0.44, -0.012, 0.176 * sign, 0.076, 0.072},
            {-0.16, -0.008, 0.170 * sign, 0.082, 0.078},
            { 0.10, -0.004, 0.150 * sign, 0.062, 0.058},
            { 0.22, 0.000, 0.140 * sign, 0.030, 0.028}
        };
        body(nacelle, 5, 12, 0.261799, 1, 1, 0, plate);
        nozzle(-0.62, 0.10, -0.014, 0.176 * sign, 0.060, 0.040, 12, dark, glow);

        Section pod[4] = {
            {-0.30, 0.056, 0.86 * sign, 0.020, 0.020},
            {-0.16, 0.056, 0.86 * sign, 0.026, 0.026},
            { 0.06, 0.054, 0.86 * sign, 0.022, 0.022},
            { 0.20, 0.052, 0.86 * sign, 0.000, 0.000}
        };
        body(pod, 4, 8, 0.392699, 1, 1, 1, dark);

        blade(-0.16, -0.56, 0.070, -0.32, -0.54, 0.44, 0.058 * sign, 0.190 * sign, 0.016, plate);

        Section cannon[3] = {
            { 0.20, -0.050, 0.078 * sign, 0.016, 0.016},
            { 0.62, -0.052, 0.076 * sign, 0.013, 0.013},
            { 0.74, -0.048, 0.072 * sign, 0.009, 0.009}
        };
        body(cannon, 3, 6, 0.523599, 1, 1, 1, trim);
        hardpoint(0.74, -0.048, 0.072 * sign, 1.0, 0.0, 0.0);
    }

    Section keel[4] = {
        {-0.44, -0.120, 0.0, 0.038, 0.046},
        {-0.10, -0.146, 0.0, 0.050, 0.060},
        { 0.24, -0.126, 0.0, 0.040, 0.048},
        { 0.46, -0.092, 0.0, 0.008, 0.016}
    };
    body(keel, 4, 8, 0.392699, 1, 1, 1, plate);
}

static void buildBomber(void) {
    livery(0.25);
    Section fuselage[8] = {
        {-0.52, 0.000, 0.0, 0.130, 0.150},
        {-0.28, 0.000, 0.0, 0.152, 0.182},
        { 0.04, -0.004, 0.0, 0.158, 0.192},
        { 0.34, -0.010, 0.0, 0.146, 0.176},
        { 0.60, -0.016, 0.0, 0.120, 0.144},
        { 0.78, -0.022, 0.0, 0.090, 0.106},
        { 0.90, -0.026, 0.0, 0.056, 0.066},
        { 0.96, -0.030, 0.0, 0.022, 0.026}
    };
    body(fuselage, 8, 14, 0.224399, 1, 1, 1, hull);
    rib(-0.16, 0.034, -0.002, 0.0, 0.156, 0.188, 0.014, 14, 0.224399, trim);
    rib(0.22, 0.030, -0.007, 0.0, 0.152, 0.184, 0.012, 14, 0.224399, trim);

    Section chin[4] = {
        { 0.44, -0.146, 0.0, 0.046, 0.098},
        { 0.62, -0.140, 0.0, 0.050, 0.100},
        { 0.80, -0.122, 0.0, 0.038, 0.072},
        { 0.90, -0.104, 0.0, 0.012, 0.024}
    };
    body(chin, 4, 10, 0.314159, 1, 1, 1, dark);

    Section canopy[4] = {
        { 0.30, 0.146, 0.0, 0.050, 0.082},
        { 0.48, 0.140, 0.0, 0.056, 0.080},
        { 0.66, 0.112, 0.0, 0.036, 0.052},
        { 0.78, 0.082, 0.0, 0.008, 0.014}
    };
    body(canopy, 4, 10, 0.314159, 1, 1, 1, glass);

    Section spine[4] = {
        {-0.46, 0.128, 0.0, 0.052, 0.088},
        {-0.16, 0.146, 0.0, 0.060, 0.100},
        { 0.10, 0.138, 0.0, 0.048, 0.082},
        { 0.26, 0.120, 0.0, 0.016, 0.030}
    };
    body(spine, 4, 10, 0.314159, 1, 1, 1, plate);

    Section bay[5] = {
        {-0.36, -0.170, 0.0, 0.062, 0.132},
        {-0.10, -0.186, 0.0, 0.076, 0.156},
        { 0.18, -0.180, 0.0, 0.068, 0.144},
        { 0.38, -0.160, 0.0, 0.046, 0.104},
        { 0.50, -0.146, 0.0, 0.016, 0.044}
    };
    body(bay, 5, 12, 0.261799, 1, 1, 1, plate);

    for (int side = -1; side <= 1; side += 2) {
        double sign = (double)side;
        Section boom[7] = {
            {-0.72, -0.010, 0.440 * sign, 0.096, 0.092},
            {-0.54, -0.008, 0.440 * sign, 0.118, 0.114},
            {-0.22, -0.004, 0.438 * sign, 0.124, 0.120},
            { 0.14, 0.000, 0.434 * sign, 0.114, 0.110},
            { 0.42, 0.004, 0.430 * sign, 0.086, 0.084},
            { 0.60, 0.006, 0.428 * sign, 0.052, 0.050},
            { 0.68, 0.008, 0.426 * sign, 0.016, 0.016}
        };
        body(boom, 7, 12, 0.261799, 1, 1, 1, plate);
        rib(-0.36, 0.030, -0.006, 0.440 * sign, 0.122, 0.118, 0.014, 12, 0.261799, trim);
        nozzle(-0.72, 0.14, -0.010, 0.440 * sign, 0.086, 0.058, 12, dark, burn);

        Section intake[3] = {
            { 0.42, 0.004, 0.430 * sign, 0.080, 0.078},
            { 0.54, 0.006, 0.428 * sign, 0.064, 0.062},
            { 0.60, 0.006, 0.428 * sign, 0.040, 0.040}
        };
        body(intake, 3, 12, 0.261799, 1, 0, 1, dark);

        plank(0.16, -0.40, 0.170 * sign, 0.430 * sign, -0.010, -0.004, 0.050, 0.044,
              0.06, 0.06, hull);
        plank(0.12, -0.36, 0.440 * sign, 0.88 * sign, -0.004, 0.048, 0.038, 0.012,
              -0.10, 0.06, hull);
        plank(0.02, -0.30, 0.85 * sign, 0.97 * sign, 0.045, 0.056, 0.015, 0.008,
              0.0, 0.0, trim);
        plank(0.24, -0.04, 0.184 * sign, 0.400 * sign, 0.042, 0.030, 0.016, 0.014,
              0.02, 0.02, owner);

        for (int rail = 0; rail < 2; rail++) {
            double railZ = (0.60 + 0.16 * (double)rail) * sign;
            double railY = 0.010 + 0.014 * (double)rail;
            Section missile[5] = {
                {-0.30, railY - 0.052, railZ, 0.000, 0.000},
                {-0.24, railY - 0.052, railZ, 0.026, 0.026},
                { 0.02, railY - 0.052, railZ, 0.026, 0.026},
                { 0.18, railY - 0.052, railZ, 0.020, 0.020},
                { 0.28, railY - 0.052, railZ, 0.000, 0.000}
            };
            body(missile, 5, 8, 0.392699, 1, 1, 1, dark);
            Section rack[3] = {
                {-0.14, railY - 0.026, railZ, 0.026, 0.010},
                {-0.02, railY - 0.024, railZ, 0.028, 0.010},
                { 0.10, railY - 0.026, railZ, 0.024, 0.010}
            };
            body(rack, 3, 6, 0.523599, 0, 1, 1, plate);
        }

        for (int slot = 0; slot < 3; slot++) {
            double offset = -0.22 + 0.20 * (double)slot;
            Section bomb[4] = {
                {offset - 0.12, -0.236, 0.098 * sign, 0.000, 0.000},
                {offset - 0.08, -0.236, 0.098 * sign, 0.036, 0.036},
                {offset + 0.06, -0.236, 0.098 * sign, 0.036, 0.036},
                {offset + 0.13, -0.236, 0.098 * sign, 0.008, 0.008}
            };
            body(bomb, 4, 8, 0.392699, 1, 1, 1, dark);
            hardpoint(offset + 0.13, -0.236, 0.098 * sign, 1.0, 0.0, 0.0);
        }

        blade(-0.40, -0.72, 0.070, -0.50, -0.70, 0.52, 0.440 * sign, 0.440 * sign, 0.028, plate);

        Section turret[4] = {
            { 0.02, 0.176, 0.0, 0.058, 0.058},
            { 0.10, 0.206, 0.0, 0.052, 0.052},
            { 0.16, 0.212, 0.0, 0.030, 0.030},
            { 0.20, 0.210, 0.0, 0.008, 0.008}
        };
        if (side < 0) {
            body(turret, 4, 10, 0.314159, 1, 1, 1, dark);
        }
    }
}

static void buildScout(void) {
    livery(0.25);
    Section fuselage[7] = {
        {-0.44, 0.000, 0.0, 0.086, 0.080},
        {-0.22, 0.000, 0.0, 0.102, 0.096},
        { 0.06, 0.000, 0.0, 0.098, 0.094},
        { 0.30, 0.002, 0.0, 0.078, 0.076},
        { 0.50, 0.004, 0.0, 0.052, 0.052},
        { 0.62, 0.004, 0.0, 0.032, 0.034},
        { 0.66, 0.004, 0.0, 0.018, 0.020}
    };
    body(fuselage, 7, 12, 0.261799, 1, 1, 1, hull);
    rib(-0.10, 0.030, 0.000, 0.0, 0.100, 0.095, 0.014, 12, 0.261799, trim);

    Section mast[5] = {
        { 0.64, 0.004, 0.0, 0.022, 0.022},
        { 0.74, 0.004, 0.0, 0.014, 0.014},
        { 0.86, 0.004, 0.0, 0.010, 0.010},
        { 0.96, 0.004, 0.0, 0.006, 0.006},
        { 1.00, 0.004, 0.0, 0.000, 0.000}
    };
    body(mast, 5, 8, 0.392699, 1, 1, 0, trim);

    Section dish[5] = {
        { 0.700, 0.004, 0.0, 0.030, 0.030},
        { 0.716, 0.004, 0.0, 0.150, 0.150},
        { 0.726, 0.004, 0.0, 0.168, 0.168},
        { 0.740, 0.004, 0.0, 0.150, 0.150},
        { 0.752, 0.004, 0.0, 0.034, 0.034}
    };
    body(dish, 5, 20, 0.157080, 1, 1, 1, plate);

    Section canopy[4] = {
        { 0.08, 0.076, 0.0, 0.032, 0.052},
        { 0.24, 0.082, 0.0, 0.036, 0.052},
        { 0.40, 0.070, 0.0, 0.024, 0.036},
        { 0.50, 0.054, 0.0, 0.006, 0.010}
    };
    body(canopy, 4, 10, 0.314159, 1, 1, 1, glass);

    Section engine[5] = {
        {-0.70, 0.000, 0.0, 0.104, 0.100},
        {-0.58, 0.000, 0.0, 0.124, 0.118},
        {-0.40, 0.000, 0.0, 0.116, 0.110},
        {-0.20, 0.000, 0.0, 0.098, 0.094},
        {-0.06, 0.000, 0.0, 0.086, 0.082}
    };
    body(engine, 5, 12, 0.261799, 1, 0, 0, plate);
    nozzle(-0.70, 0.16, 0.000, 0.0, 0.098, 0.070, 14, dark, glow);

    for (int side = -1; side <= 1; side += 2) {
        double sign = (double)side;
        plank(-0.30, -0.56, 0.078 * sign, 0.74 * sign, 0.006, 0.070, 0.024, 0.010,
              0.34, 0.30, hull);
        plank(0.10, -0.16, 0.74 * sign, 0.88 * sign, 0.070, 0.078, 0.012, 0.008,
              0.0, 0.02, trim);
        plank(-0.24, -0.44, 0.088 * sign, 0.26 * sign, 0.016, 0.038, 0.016, 0.012,
              0.24, 0.20, owner);

        Section pod[5] = {
            {-0.22, 0.078, 0.80 * sign, 0.000, 0.000},
            {-0.16, 0.078, 0.80 * sign, 0.028, 0.028},
            { 0.02, 0.078, 0.80 * sign, 0.032, 0.032},
            { 0.16, 0.078, 0.80 * sign, 0.024, 0.024},
            { 0.24, 0.078, 0.80 * sign, 0.000, 0.000}
        };
        body(pod, 5, 10, 0.314159, 1, 1, 1, dark);
        hardpoint(0.24, 0.078, 0.80 * sign, 1.0, 0.0, 0.0);

        blade(-0.30, -0.58, -0.070, -0.40, -0.56, -0.30, 0.040 * sign, 0.120 * sign, 0.012, plate);

        Section antenna[3] = {
            { 0.26, 0.030, 0.062 * sign, 0.008, 0.008},
            { 0.44, 0.090, 0.086 * sign, 0.006, 0.006},
            { 0.52, 0.128, 0.096 * sign, 0.002, 0.002}
        };
        body(antenna, 3, 6, 0.523599, 1, 1, 1, trim);
    }

    Section fin[4] = {
        {-0.30, 0.100, 0.0, 0.034, 0.016},
        {-0.40, 0.220, 0.0, 0.030, 0.012},
        {-0.50, 0.330, 0.0, 0.022, 0.009},
        {-0.58, 0.390, 0.0, 0.007, 0.004}
    };
    body(fin, 4, 6, 0.523599, 1, 1, 1, plate);
}

static void buildCorvette(void) {
    Section frame[12] = {
        {-1.00, 0.0, 0.0, 0.150, 0.200},
        {-0.94, 0.0, 0.0, 0.168, 0.224},
        {-0.90, 0.0, 0.0, 0.172, 0.230},
        {-0.52, 0.0, 0.0, 0.172, 0.230},
        {-0.48, 0.0, 0.0, 0.150, 0.256},
        { 0.10, 0.0, 0.0, 0.150, 0.256},
        { 0.14, 0.0, 0.0, 0.134, 0.222},
        { 0.56, 0.0, 0.0, 0.134, 0.222},
        { 0.60, 0.0, 0.0, 0.120, 0.150},
        { 0.86, 0.0, 0.0, 0.112, 0.086},
        { 1.06, 0.0, 0.0, 0.096, 0.028},
        { 1.12, 0.0, 0.0, 0.070, 0.012}
    };
    body(frame, 12, 8, 0.392699, 0, 1, 1, hull);
    rib(-0.70, 0.026, 0.0, 0.0, 0.172, 0.230, 0.010, 8, 0.392699, trim);
    rib(-0.20, 0.026, 0.0, 0.0, 0.150, 0.256, 0.010, 8, 0.392699, trim);
    rib( 0.44, 0.024, 0.0, 0.0, 0.134, 0.222, 0.009, 8, 0.392699, trim);

    Section thruster[4] = {
        {-1.10, 0.0, 0.0, 0.176, 0.246},
        {-1.06, 0.0, 0.0, 0.190, 0.270},
        {-0.96, 0.0, 0.0, 0.190, 0.270},
        {-0.92, 0.0, 0.0, 0.176, 0.244}
    };
    body(thruster, 4, 8, 0.392699, 0, 0, 1, plate);

    Section house[4] = {
        {-0.44, 0.172, 0.0, 0.062, 0.135},
        {-0.30, 0.176, 0.0, 0.064, 0.138},
        { 0.00, 0.176, 0.0, 0.064, 0.138},
        { 0.06, 0.172, 0.0, 0.058, 0.126}
    };
    body(house, 4, 4, 0.785398, 0, 1, 1, plate);

    Section bridge[4] = {
        {-0.22, 0.242, 0.0, 0.040, 0.100},
        {-0.16, 0.246, 0.0, 0.042, 0.104},
        {-0.02, 0.246, 0.0, 0.042, 0.104},
        { 0.04, 0.238, 0.0, 0.034, 0.086}
    };
    body(bridge, 4, 4, 0.785398, 0, 1, 1, plate);

    Section glazing[3] = {
        {-0.05, 0.252, 0.0, 0.030, 0.101},
        { 0.01, 0.248, 0.0, 0.030, 0.096},
        { 0.05, 0.240, 0.0, 0.024, 0.080}
    };
    body(glazing, 3, 4, 0.785398, 0, 1, 0, glass);

    static const double mastStack[10] = {0.000, 0.022, 0.060, 0.016, 0.120, 0.011, 0.168, 0.008, 0.196, 0.003};
    column(-0.36, 0.212, 0.0, mastStack, 5, 6, 0.523599, 1.0, 0, 1, trim);
    static const double arrayStack[6] = {-0.020, 0.026, 0.030, 0.030, 0.048, 0.012};
    column(-0.30, 0.214, 0.0, arrayStack, 3, 8, 0.392699, 1.0, 0, 1, dark);

    Section dish[4] = {
        {-0.42, 0.268, 0.0, 0.010, 0.010},
        {-0.40, 0.270, 0.0, 0.052, 0.052},
        {-0.38, 0.272, 0.0, 0.056, 0.056},
        {-0.36, 0.274, 0.0, 0.012, 0.012}
    };
    body(dish, 4, 10, 0.314159, 1, 1, 1, plate);

    Section hangar[4] = {
        {-0.34, -0.132, 0.0, 0.022, 0.120},
        {-0.28, -0.136, 0.0, 0.026, 0.126},
        { 0.00, -0.136, 0.0, 0.026, 0.126},
        { 0.06, -0.132, 0.0, 0.020, 0.112}
    };
    body(hangar, 4, 4, 0.785398, 0, 1, 1, dark);

    Section cells[4] = {
        { 0.16, 0.122, 0.0, 0.012, 0.082},
        { 0.20, 0.126, 0.0, 0.014, 0.086},
        { 0.28, 0.126, 0.0, 0.014, 0.086},
        { 0.32, 0.122, 0.0, 0.010, 0.078}
    };
    body(cells, 4, 4, 0.785398, 0, 1, 1, dark);

    static const double collarStack[6] = {0.000, 0.038, 0.014, 0.040, 0.024, 0.030};
    column(0.48, 0.118, 0.0, collarStack, 3, 10, 0.314159, 1.0, 0, 1, trim);

    for (int side = -1; side <= 1; side += 2) {
        double sign = (double)side;
        Section fin[6] = {
            {-0.52, 0.024, (0.2005 + 0.924 * 0.022) * sign, 0.030, 0.022},
            {-0.58, 0.022, (0.2005 + 0.924 * 0.052) * sign, 0.076, 0.052},
            {-0.66, 0.020, (0.2005 + 0.924 * 0.062) * sign, 0.092, 0.062},
            {-0.78, 0.018, (0.2005 + 0.924 * 0.060) * sign, 0.088, 0.060},
            {-0.86, 0.016, (0.2005 + 0.924 * 0.044) * sign, 0.062, 0.044},
            {-0.91, 0.014, (0.2005 + 0.924 * 0.018) * sign, 0.024, 0.018}
        };
        body(fin, 6, 8, 0.392699, 0, 1, 1, trim);

        Section pipe[4] = {
            {-0.56, -0.076, (0.2005 + 0.924 * 0.026) * sign, 0.016, 0.026},
            {-0.66, -0.078, (0.2005 + 0.924 * 0.030) * sign, 0.019, 0.030},
            {-0.80, -0.078, (0.2005 + 0.924 * 0.028) * sign, 0.018, 0.028},
            {-0.88, -0.076, (0.2005 + 0.924 * 0.016) * sign, 0.010, 0.016}
        };
        body(pipe, 4, 6, 0.523599, 1, 1, 1, dark);

        Section strake[4] = {
            {-0.46, 0.058, 0.2365 * sign, 0.020, 0.014},
            {-0.20, 0.060, 0.2365 * sign, 0.024, 0.016},
            { 0.04, 0.058, 0.2365 * sign, 0.022, 0.015},
            { 0.10, 0.054, 0.2365 * sign, 0.010, 0.008}
        };
        body(strake, 4, 4, 0.785398, 0, 1, 1, plate);

        Section belt[4] = {
            {-0.44, -0.060, 0.2365 * sign, 0.028, 0.018},
            {-0.16, -0.062, 0.2365 * sign, 0.032, 0.020},
            { 0.02, -0.060, 0.2365 * sign, 0.028, 0.018},
            { 0.09, -0.056, 0.2365 * sign, 0.012, 0.008}
        };
        body(belt, 4, 4, 0.785398, 0, 1, 1, hull);

        Section badge[4] = {
            {-0.36, 0.028, 0.2365 * sign, 0.034, 0.014},
            {-0.28, 0.030, 0.2365 * sign, 0.040, 0.015},
            {-0.06, 0.030, 0.2365 * sign, 0.040, 0.015},
            { 0.02, 0.028, 0.2365 * sign, 0.030, 0.012}
        };
        body(badge, 4, 4, 0.785398, 0, 1, 1, owner);

        Section flash[3] = {
            { 0.22, 0.022, 0.2051 * sign, 0.028, 0.013},
            { 0.38, 0.022, 0.2051 * sign, 0.032, 0.014},
            { 0.52, 0.020, 0.2051 * sign, 0.020, 0.010}
        };
        body(flash, 3, 4, 0.785398, 0, 1, 1, owner);

        Section berth[4] = {
            {-0.58, 0.086, 0.2125 * sign, 0.014, 0.014},
            {-0.66, 0.088, 0.2125 * sign, 0.030, 0.024},
            {-0.80, 0.086, 0.2125 * sign, 0.028, 0.022},
            {-0.88, 0.082, 0.2125 * sign, 0.010, 0.008}
        };
        body(berth, 4, 8, 0.392699, 1, 1, 1, dark);

        Section jet[3] = {
            { 0.60, 0.052, 0.110 * sign, 0.026, 0.022},
            { 0.72, 0.066, 0.126 * sign, 0.028, 0.024},
            { 0.78, 0.064, 0.120 * sign, 0.016, 0.014}
        };
        body(jet, 3, 6, 0.523599, 0, 1, 1, dark);

        Section marker[3] = {
            { 0.94, 0.014, 0.052 * sign, 0.014, 0.010},
            { 0.99, 0.014, 0.046 * sign, 0.016, 0.011},
            { 1.03, 0.012, 0.040 * sign, 0.006, 0.004}
        };
        body(marker, 3, 6, 0.523599, 0, 1, 1, glow);

        nozzle(-1.10, 0.20, 0.086, 0.128 * sign, 0.080, 0.056, 12, dark, glow);
        nozzle(-1.10, 0.20, -0.086, 0.128 * sign, 0.080, 0.056, 12, dark, glow);
    }

    barbette( 0.34, 0.116, 0.0, 1.0, 0.078);
    barbette(-0.70, 0.150, 0.0, 1.0, 0.084);
    barbette( 0.30, -0.116, 0.0, -1.0, 0.078);
    barbette(-0.66, -0.150, 0.0, -1.0, 0.084);
}

#define TRUNNION 0.044

static void buildTurret(void) {
    static const double house[10] = {0.000, 0.076, 0.018, 0.082, 0.044, 0.078, 0.062, 0.060, 0.074, 0.030};
    column(0.0, 0.0, 0.0, house, 5, 12, 0.261799, 1.0, 0, 1, plate);
    for (int cheek = -1; cheek <= 1; cheek += 2) {
        double offset = 0.044 * (double)cheek;
        Section jaw[3] = {
            {-0.014, TRUNNION, offset, 0.030, 0.016},
            { 0.026, TRUNNION, offset, 0.032, 0.017},
            { 0.052, TRUNNION, offset, 0.020, 0.011}
        };
        body(jaw, 3, 6, 0.523599, 0, 1, 1, dark);
    }
}

static void buildGun(void) {
    Section mantlet[4] = {
        {-0.034, 0.0, 0.0, 0.032, 0.048},
        { 0.016, 0.0, 0.0, 0.034, 0.052},
        { 0.042, 0.0, 0.0, 0.026, 0.040},
        { 0.054, 0.0, 0.0, 0.012, 0.018}
    };
    body(mantlet, 4, 8, 0.392699, 0, 1, 1, dark);
    for (int barrel = -1; barrel <= 1; barrel += 2) {
        double offset = 0.026 * (double)barrel;
        Section tube[4] = {
            { 0.008, 0.0, offset, 0.014, 0.014},
            { 0.108, 0.0, offset, 0.011, 0.011},
            { 0.164, 0.0, offset, 0.010, 0.010},
            { 0.178, 0.0, offset, 0.006, 0.006}
        };
        body(tube, 4, 6, 0.523599, 1, 1, 1, trim);
    }
}

static void buildBomb(void) {
    Section shell[4] = {
        {-0.125, 0.0, 0.0, 0.000, 0.000},
        {-0.085, 0.0, 0.0, 0.036, 0.036},
        { 0.055, 0.0, 0.0, 0.036, 0.036},
        { 0.125, 0.0, 0.0, 0.008, 0.008}
    };
    body(shell, 4, 8, 0.392699, 1, 1, 1, dark);
}

static void buildFlame(void) {
    static const double profile[9][2] = {
        {0.00, 1.00}, {-0.35, 1.06}, {-0.90, 0.96}, {-1.70, 0.80}, {-2.70, 0.62},
        {-3.70, 0.44}, {-4.60, 0.28}, {-5.30, 0.14}, {-5.80, 0.00}
    };
    static const double heat[9][4] = {
        {0.78, 0.90, 1.00, 4.20},
        {0.46, 0.72, 1.00, 3.00},
        {0.30, 0.56, 1.00, 2.10},
        {0.24, 0.46, 1.00, 1.45},
        {0.21, 0.39, 1.00, 0.92},
        {0.19, 0.33, 0.98, 0.54},
        {0.17, 0.27, 0.93, 0.27},
        {0.16, 0.23, 0.87, 0.10},
        {0.15, 0.20, 0.82, 0.00}
    };
    double rings[2][MAX_RING * 3];
    Material shades[9];
    for (uint32_t i = 0; i < 9; i++) {
        shades[i].colour[0] = heat[i][0];
        shades[i].colour[1] = heat[i][1];
        shades[i].colour[2] = heat[i][2];
        shades[i].emissive = heat[i][3];
        shades[i].metallic = 0.0;
        shades[i].roughness = 1.0;
        shades[i].pattern = SURFACE_SMOOTH;
        shades[i].plate = 0.0;
    }
    Section section = {profile[0][0], 0.0, 0.0, profile[0][1], profile[0][1]};
    ringAround(&section, 14, 0.224399, rings[0]);
    for (uint32_t i = 1; i < 9; i++) {
        Section next = {profile[i][0], 0.0, 0.0, profile[i][1], profile[i][1]};
        ringAround(&next, 14, 0.224399, rings[i & 1]);
        loft(rings[(i - 1) & 1], rings[i & 1], 14, 1, shades[i - 1], shades[i]);
    }
}

static void buildDrone(void) {
    livery(0.25);
    Section spine[8] = {
        {-0.62, 0.0, 0.0, 0.130, 0.130},
        {-0.58, 0.0, 0.0, 0.180, 0.180},
        {-0.20, 0.0, 0.0, 0.180, 0.180},
        {-0.18, 0.0, 0.0, 0.230, 0.215},
        { 0.18, 0.0, 0.0, 0.230, 0.215},
        { 0.20, 0.0, 0.0, 0.155, 0.150},
        { 0.44, 0.0, 0.0, 0.155, 0.150},
        { 0.48, 0.0, 0.0, 0.115, 0.110}
    };
    body(spine, 8, 8, 0.392699, 1, 1, 1, hull);
    rib(-0.02, 0.026, 0.0, 0.0, 0.232, 0.217, 0.018, 8, 0.392699, trim);

    Section head[5] = {
        {0.42, 0.0, 0.0, 0.120, 0.115},
        {0.56, 0.0, 0.0, 0.098, 0.094},
        {0.58, 0.0, 0.0, 0.074, 0.072},
        {0.66, 0.0, 0.0, 0.074, 0.072},
        {0.70, 0.0, 0.0, 0.046, 0.046}
    };
    body(head, 5, 6, 0.523599, 1, 0, 0, plate);
    Section lens[3] = {
        {0.700, 0.0, 0.0, 0.046, 0.046},
        {0.716, 0.0, 0.0, 0.034, 0.034},
        {0.724, 0.0, 0.0, 0.000, 0.000}
    };
    body(lens, 3, 6, 0.523599, 1, 1, 0, burn);
    hardpoint(0.724, 0.0, 0.0, 1.0, 0.0, 0.0);

    for (int side = -1; side <= 1; side += 2) {
        double sign = (double)side;
        Section hold[6] = {
            {-0.34, -0.030, 0.330 * sign, 0.000, 0.000},
            {-0.32, -0.030, 0.330 * sign, 0.150, 0.130},
            {-0.30, -0.030, 0.330 * sign, 0.170, 0.150},
            { 0.14, -0.030, 0.330 * sign, 0.170, 0.150},
            { 0.16, -0.030, 0.330 * sign, 0.150, 0.130},
            { 0.18, -0.030, 0.330 * sign, 0.000, 0.000}
        };
        body(hold, 6, 4, 0.785398, 0, 1, 1, plate);
        plank(0.02, -0.18, 0.190 * sign, 0.330 * sign, -0.030, -0.030, 0.030, 0.026, 0.0, 0.0, dark);
        plank(-0.06, -0.22, 0.176 * sign, 0.250 * sign, 0.090, 0.104, 0.020, 0.016, 0.0, 0.0, owner);

        Section boom[4] = {
            {0.26, 0.070, 0.104 * sign, 0.026, 0.024},
            {0.52, 0.104, 0.150 * sign, 0.024, 0.022},
            {0.66, 0.104, 0.150 * sign, 0.020, 0.018},
            {0.70, 0.104, 0.150 * sign, 0.008, 0.008}
        };
        body(boom, 4, 6, 0.523599, 1, 1, 1, trim);
    }

    Section mast[4] = {
        {-0.076, 0.196, 0.0, 0.022, 0.022},
        {-0.16, 0.330, 0.0, 0.016, 0.016},
        {-0.20, 0.400, 0.0, 0.012, 0.012},
        {-0.22, 0.436, 0.0, 0.000, 0.000}
    };
    body(mast, 4, 6, 0.523599, 1, 1, 1, trim);

    Section tank[5] = {
        {-0.56, 0.0, 0.0, 0.150, 0.150},
        {-0.50, 0.0, 0.0, 0.196, 0.196},
        {-0.34, 0.0, 0.0, 0.196, 0.196},
        {-0.28, 0.0, 0.0, 0.182, 0.182},
        {-0.24, 0.0, 0.0, 0.176, 0.176}
    };
    body(tank, 5, 8, 0.392699, 1, 0, 0, dark);
    nozzle(-0.62, 0.14, 0.0, 0.0, 0.132, 0.092, 12, dark, glow);
}

static void deckRing(double y, double radius, uint32_t sides, double phase, Material material, double thickness) {
    double seat = radius - thickness * 0.5;
    double stack[6] = {0.0, seat, thickness * 0.5, radius + thickness * 0.6, thickness, seat};
    column(0.0, y, 0.0, stack, 3, sides, phase, 1.0, 0, 0, material);
}

static void spoke(double innerRadius, double outerRadius, double innerY, double outerY, double angle,
                  double thickness, Material material) {
    double from[3] = {innerRadius * cos(angle), innerY, innerRadius * sin(angle)};
    double to[3] = {outerRadius * cos(angle), outerY, outerRadius * sin(angle)};
    strut(from, to, thickness, 6, material);
}

static void buildStation(void) {
    static const double keel[24] = {
        -1.080, 0.000, -1.060, 0.086, -1.030, 0.128,
        -0.830, 0.128, -0.800, 0.180, -0.350, 0.180,
        -0.320, 0.144, -0.290, 0.144,  0.560, 0.144,
         0.600, 0.128,  1.000, 0.128,  1.030, 0.070
    };
    column(0.0, 0.0, 0.0, keel, 12, 12, 0.261799, 1.0, 0, 1, plate);

    static const double reactor[16] = {
        -1.040, 0.000, -1.024, 0.320, -1.016, 0.500,
        -0.988, 0.600, -0.876, 0.600, -0.848, 0.556,
        -0.820, 0.404, -0.792, 0.128
    };
    column(0.0, 0.0, 0.0, reactor, 8, 16, 0.196350, 1.0, 0, 0, plate);
    static const double core[8] = {
        -0.958, 0.588, -0.946, 0.612, -0.924, 0.612, -0.912, 0.588
    };
    column(0.0, 0.0, 0.0, core, 4, 16, 0.196350, 1.0, 0, 0, burn);
    deckRing(-0.906, 0.606, 16, 0.196350, trim, 0.026);
    for (uint32_t i = 0; i < 8; i++) {
        double angle = 0.392699 + 0.785398 * (double)i;
        static const double vent[8] = {-1.000, 0.000, -0.976, 0.052, -0.900, 0.052, -0.876, 0.000};
        column(0.500 * cos(angle), 0.0, 0.500 * sin(angle), vent, 4, 6, 0.523599, 1.0, 0, 0, dark);
    }

    for (uint32_t i = 0; i < 6; i++) {
        double angle = 1.047198 * (double)i;
        static const double silo[12] = {
            -0.792, 0.000, -0.756, 0.072, -0.728, 0.104,
            -0.400, 0.104, -0.372, 0.072, -0.340, 0.000
        };
        column(0.262 * cos(angle), 0.0, 0.262 * sin(angle), silo, 6, 8, 0.392699, 1.0, 1, 0, hull);
        static const double pipe[8] = {-0.776, 0.000, -0.752, 0.024, -0.388, 0.024, -0.364, 0.000};
        column(0.366 * cos(angle), 0.0, 0.366 * sin(angle), pipe, 4, 6, 0.523599, 1.0, 1, 0, dark);
    }
    hoop(-0.728, 0.262, 0.026, 12, 6, trim);
    hoop(-0.400, 0.262, 0.028, 12, 6, trim);

    static const double block[16] = {
        -0.340, 0.300, -0.300, 0.420, 0.020, 0.420, 0.060, 0.540,
         0.360, 0.540,  0.400, 0.462, 0.430, 0.360, 0.450, 0.220
    };
    column(0.0, 0.0, 0.0, block, 8, 16, 0.196350, 1.0, 0, 0, hull);
    deckRing(-0.256, 0.428, 16, 0.196350, trim, 0.028);
    deckRing( 0.096, 0.548, 16, 0.196350, trim, 0.028);
    deckRing( 0.318, 0.548, 16, 0.196350, trim, 0.028);

    for (uint32_t i = 0; i < 4; i++) {
        double angle = 0.785398 + 1.570796 * (double)i;
        double upright[3] = {0.0, 1.0, 0.0};
        double root[3] = {0.376 * cos(angle), -0.140, 0.376 * sin(angle)};
        double tip[3] = {1.000 * cos(angle), -0.140, 1.000 * sin(angle)};
        panel(root, tip, upright, 0.168, 0.012, dark);
        panel(root, tip, upright, 0.178, 0.005, plate);
        for (int edge = -1; edge <= 1; edge += 2) {
            double rail[3] = {0.376 * cos(angle), -0.140 + 0.174 * (double)edge, 0.376 * sin(angle)};
            double span[3] = {1.000 * cos(angle), -0.140 + 0.174 * (double)edge, 1.000 * sin(angle)};
            strut(rail, span, 0.014, 6, trim);
        }
        spoke(0.380, 0.470, -0.240, -0.240, angle, 0.022, plate);
        spoke(0.380, 0.470, -0.040, -0.040, angle, 0.022, plate);
    }

    for (uint32_t i = 0; i < 3; i++) {
        double angle = 2.094395 * (double)i;
        double outward[3] = {cos(angle), 0.0, sin(angle)};
        double upright[3] = {0.0, 1.0, 0.0};
        double across[3] = {-outward[2], 0.0, outward[0]};
        double back[3] = {0.460 * outward[0], 0.210, 0.460 * outward[2]};
        double face[3] = {0.640 * outward[0], 0.210, 0.640 * outward[2]};
        panel(back, face, upright, 0.144, 0.130, recess);
        for (int edge = -1; edge <= 1; edge += 2) {
            double height = 0.210 + 0.152 * (double)edge;
            double inner[3] = {0.500 * outward[0], height, 0.500 * outward[2]};
            double outer[3] = {0.652 * outward[0], height, 0.652 * outward[2]};
            panel(inner, outer, upright, 0.020, 0.150, dark);
            double side = 0.140 * (double)edge;
            double jamb[3] = {
                0.500 * outward[0] + side * across[0], 0.210, 0.500 * outward[2] + side * across[2]
            };
            double post[3] = {
                0.652 * outward[0] + side * across[0], 0.210, 0.652 * outward[2] + side * across[2]
            };
            panel(jamb, post, upright, 0.164, 0.020, dark);
        }
        hardpoint(0.700 * outward[0], 0.210, 0.700 * outward[2], outward[0], 0.0, outward[2]);
    }

    for (uint32_t i = 0; i < 3; i++) {
        double angle = 2.094395 * (double)i + 1.047198;
        double outward[3] = {cos(angle), 0.0, sin(angle)};
        double upright[3] = {0.0, 1.0, 0.0};
        double lower[3] = {0.500 * outward[0], 0.200, 0.500 * outward[2]};
        double upper[3] = {0.552 * outward[0], 0.200, 0.552 * outward[2]};
        panel(lower, upper, upright, 0.116, 0.124, owner);
    }

    for (uint32_t i = 0; i < 4; i++) {
        double angle = 0.785398 + 1.570796 * (double)i;
        spoke(0.116, 0.560, 0.520, 0.470, angle, 0.030, plate);
        static const double collar[10] = {
            0.396, 0.036, 0.416, 0.078, 0.500, 0.078, 0.520, 0.062, 0.540, 0.030
        };
        column(0.600 * cos(angle), 0.0, 0.600 * sin(angle), collar, 5, 10, 0.314159, 1.0, 0, 1, trim);
        double post[3] = {0.600 * cos(angle), 0.540, 0.600 * sin(angle)};
        double crown[3] = {0.600 * cos(angle), 0.572, 0.600 * sin(angle)};
        strut(post, crown, 0.014, 6, glow);
    }

    hoop(0.720, 0.720, 0.088, 18, 8, hull);
    for (uint32_t i = 0; i < 6; i++) {
        double angle = 1.047198 * (double)i + 0.523599;
        spoke(0.098, 0.724, 0.600, 0.720, angle, 0.026, plate);
    }
    for (uint32_t i = 0; i < 12; i++) {
        double angle = 0.523599 * (double)i + 0.261799;
        double outward[3] = {cos(angle), 0.0, sin(angle)};
        double upright[3] = {0.0, 1.0, 0.0};
        double inner[3] = {0.786 * outward[0], 0.720, 0.786 * outward[2]};
        double outer[3] = {0.818 * outward[0], 0.720, 0.818 * outward[2]};
        panel(inner, outer, upright, 0.022, 0.036, lamp);
    }

    static const double tower[12] = {
        0.780, 0.130, 0.820, 0.200, 1.000, 0.200,
        1.040, 0.176, 1.062, 0.120, 1.076, 0.060
    };
    column(0.0, 0.0, 0.0, tower, 6, 8, 0.392699, 1.0, 0, 1, hull);
    static const double glazing[8] = {
        0.884, 0.188, 0.898, 0.212, 0.944, 0.212, 0.958, 0.188
    };
    column(0.0, 0.0, 0.0, glazing, 4, 8, 0.392699, 1.0, 0, 0, glass);
    for (uint32_t i = 0; i < 4; i++) {
        double angle = 0.785398 + 1.570796 * (double)i;
        double outward[3] = {cos(angle), 0.0, sin(angle)};
        double upright[3] = {0.0, 1.0, 0.0};
        double inner[3] = {0.162 * outward[0], 1.014, 0.162 * outward[2]};
        double outer[3] = {0.216 * outward[0], 1.014, 0.216 * outward[2]};
        panel(inner, outer, upright, 0.018, 0.026, glow);
    }

    static const double mast[10] = {1.020, 0.052, 1.120, 0.040, 1.220, 0.032, 1.300, 0.024, 1.340, 0.016};
    column(0.0, 0.0, 0.0, mast, 5, 6, 0.523599, 1.0, 0, 0, trim);
    static const double beacon[6] = {1.340, 0.016, 1.368, 0.034, 1.400, 0.012};
    column(0.0, 0.0, 0.0, beacon, 3, 6, 0.523599, 1.0, 0, 1, glow);
    for (uint32_t i = 0; i < 3; i++) {
        double angle = 2.094395 * (double)i + 0.4;
        double hub[3] = {0.008 * cos(angle), 1.160, 0.008 * sin(angle)};
        double rim[3] = {0.150 * cos(angle), 1.196, 0.150 * sin(angle)};
        strut(hub, rim, 0.013, 6, trim);
        static const double dish[8] = {1.186, 0.014, 1.204, 0.058, 1.216, 0.068, 1.228, 0.022};
        column(0.150 * cos(angle), 0.0, 0.150 * sin(angle), dish, 4, 10, 0.314159, 1.0, 1, 1, plate);
    }
}

static uint32_t rockHash(uint32_t value) {
    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    return value ^ (value >> 16);
}

static double rockJitter(uint32_t seed, uint32_t ring, uint32_t side) {
    return (double)(rockHash(seed * 9781u + ring * 6151u + side * 379u) & 1023u) / 1023.0 - 0.5;
}

#define SHARD_POINTS 642
#define SHARD_FACES 1280

static double shardPoint[SHARD_POINTS][3];
static uint32_t shardFace[SHARD_FACES][3];
static uint32_t shardPointCount;
static uint32_t shardFaceCount;

static uint32_t shardPush(double x, double y, double z) {
    double length = sqrt(x * x + y * y + z * z);
    x /= length;
    y /= length;
    z /= length;
    for (uint32_t i = 0; i < shardPointCount; i++) {
        if (fabs(shardPoint[i][0] - x) < 1e-9 && fabs(shardPoint[i][1] - y) < 1e-9 &&
            fabs(shardPoint[i][2] - z) < 1e-9) {
            return i;
        }
    }
    shardPoint[shardPointCount][0] = x;
    shardPoint[shardPointCount][1] = y;
    shardPoint[shardPointCount][2] = z;
    return shardPointCount++;
}

static void shardBuild(uint32_t depth) {
    static const double corner[12][3] = {
        {-1.0, 1.618033988749895, 0.0}, {1.0, 1.618033988749895, 0.0},
        {-1.0, -1.618033988749895, 0.0}, {1.0, -1.618033988749895, 0.0},
        {0.0, -1.0, 1.618033988749895}, {0.0, 1.0, 1.618033988749895},
        {0.0, -1.0, -1.618033988749895}, {0.0, 1.0, -1.618033988749895},
        {1.618033988749895, 0.0, -1.0}, {1.618033988749895, 0.0, 1.0},
        {-1.618033988749895, 0.0, -1.0}, {-1.618033988749895, 0.0, 1.0}
    };
    static const uint32_t seed[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
    };
    shardPointCount = 0;
    shardFaceCount = 20;
    for (uint32_t i = 0; i < 12; i++) {
        shardPush(corner[i][0], corner[i][1], corner[i][2]);
    }
    memcpy(shardFace, seed, sizeof(seed));
    for (uint32_t pass = 0; pass < depth; pass++) {
        uint32_t was = shardFaceCount;
        for (uint32_t i = 0; i < was; i++) {
            uint32_t a = shardFace[i][0];
            uint32_t b = shardFace[i][1];
            uint32_t c = shardFace[i][2];
            uint32_t ab = shardPush(shardPoint[a][0] + shardPoint[b][0], shardPoint[a][1] + shardPoint[b][1],
                                    shardPoint[a][2] + shardPoint[b][2]);
            uint32_t bc = shardPush(shardPoint[b][0] + shardPoint[c][0], shardPoint[b][1] + shardPoint[c][1],
                                    shardPoint[b][2] + shardPoint[c][2]);
            uint32_t ca = shardPush(shardPoint[c][0] + shardPoint[a][0], shardPoint[c][1] + shardPoint[a][1],
                                    shardPoint[c][2] + shardPoint[a][2]);
            shardFace[i][0] = a;
            shardFace[i][1] = ab;
            shardFace[i][2] = ca;
            shardFace[shardFaceCount][0] = ab;
            shardFace[shardFaceCount][1] = b;
            shardFace[shardFaceCount][2] = bc;
            shardFaceCount++;
            shardFace[shardFaceCount][0] = ca;
            shardFace[shardFaceCount][1] = bc;
            shardFace[shardFaceCount][2] = c;
            shardFaceCount++;
            shardFace[shardFaceCount][0] = ab;
            shardFace[shardFaceCount][1] = bc;
            shardFace[shardFaceCount][2] = ca;
            shardFaceCount++;
        }
    }
}

static void rockAxis(uint32_t seed, uint32_t index, double* out) {
    double lift = 2.0 * rockJitter(seed, index, 1);
    double turn = 6.283185307179586 * (rockJitter(seed, index, 2) + 0.5);
    double flat = sqrt(1.0 - lift * lift);
    out[0] = flat * cos(turn);
    out[1] = lift;
    out[2] = flat * sin(turn);
}

#define ROCK_LOBES 16
#define ROCK_PITS 10
#define ROCK_CUTS 8
#define ROCK_SEAMS 3

static double rockShell(const double* way, const double (*lobes)[5], uint32_t lobeCount,
                        const double (*pits)[6], uint32_t pitCount, const double (*cuts)[4], uint32_t cutCount) {
    double radius = 1.0;
    for (uint32_t i = 0; i < lobeCount; i++) {
        double along = way[0] * lobes[i][0] + way[1] * lobes[i][1] + way[2] * lobes[i][2];
        if (along > lobes[i][3]) {
            double reach = (along - lobes[i][3]) / (1.0 - lobes[i][3]);
            radius += lobes[i][4] * reach * reach * (3.0 - 2.0 * reach);
        }
    }
    for (uint32_t i = 0; i < pitCount; i++) {
        double along = way[0] * pits[i][0] + way[1] * pits[i][1] + way[2] * pits[i][2];
        if (along > pits[i][3]) {
            double reach = (along - pits[i][3]) / (1.0 - pits[i][3]);
            radius -= pits[i][5] * sqrt(reach) * (1.0 - 0.34 * reach);
        } else if (along > pits[i][4]) {
            double reach = (along - pits[i][4]) / (pits[i][3] - pits[i][4]);
            radius += pits[i][5] * 0.30 * reach * reach;
        }
    }
    for (uint32_t i = 0; i < cutCount; i++) {
        double along = way[0] * cuts[i][0] + way[1] * cuts[i][1] + way[2] * cuts[i][2];
        if (along > 0.06 && radius * along > cuts[i][3]) {
            radius = cuts[i][3] / along;
        }
    }
    return radius < 0.30 ? 0.30 : radius;
}

static void rock(uint32_t seed, uint32_t lobeCount, uint32_t pitCount, uint32_t cutCount, double bite,
                 double cutIn, const double* stretch, Material stone, Material dust, Material ore) {
    double lobes[ROCK_LOBES][5];
    double pits[ROCK_PITS][6];
    double cuts[ROCK_CUTS][4];
    double seams[ROCK_SEAMS][5];
    static double shell[SHARD_POINTS];
    static double place[SHARD_POINTS][3];
    shardBuild(3);
    for (uint32_t i = 0; i < lobeCount; i++) {
        rockAxis(seed + 101u * (i + 1u), i, lobes[i]);
        double spread = rockJitter(seed + 211u, i, 5) + 0.5;
        double weight = rockJitter(seed + 307u, i, 6) + 0.5;
        int knob = i * 2u >= lobeCount;
        lobes[i][3] = knob ? 0.74 + 0.22 * spread : 0.05 + 0.48 * spread;
        lobes[i][4] = bite * (knob ? 0.22 + 0.34 * weight : 0.45 + 0.55 * weight) * ((i & 1u) ? -1.0 : 1.0);
    }
    for (uint32_t i = 0; i < pitCount; i++) {
        rockAxis(seed + 409u * (i + 1u), i + 40u, pits[i]);
        double span = 0.24 + 0.32 * (rockJitter(seed + 521u, i, 7) + 0.5);
        pits[i][3] = cos(span);
        pits[i][4] = cos(span * 1.36);
        pits[i][5] = 0.11 + 0.16 * (rockJitter(seed + 631u, i, 8) + 0.5);
    }
    for (uint32_t i = 0; i < cutCount; i++) {
        rockAxis(seed + 733u * (i + 1u), i + 80u, cuts[i]);
        cuts[i][3] = cutIn + 0.24 * (rockJitter(seed + 853u, i, 9) + 0.5);
    }
    for (uint32_t i = 0; i < ROCK_SEAMS; i++) {
        rockAxis(seed + 967u * (i + 1u), i + 120u, seams[i]);
        seams[i][3] = 0.70 * rockJitter(seed + 1049u, i, 10);
        seams[i][4] = 0.030 + 0.034 * (rockJitter(seed + 1151u, i, 11) + 0.5);
    }
    for (uint32_t i = 0; i < shardPointCount; i++) {
        shell[i] = rockShell(shardPoint[i], lobes, lobeCount, pits, pitCount, cuts, cutCount);
        double grain = shell[i] * (1.0 + 0.016 * rockJitter(seed + 1279u, i, i * 3u + 1u));
        for (uint32_t axis = 0; axis < 3; axis++) {
            place[i][axis] = shardPoint[i][axis] * grain * stretch[axis];
        }
    }
    double origin[3] = {0.0, 0.0, 0.0};
    for (uint32_t i = 0; i < shardFaceCount; i++) {
        const double* a = place[shardFace[i][0]];
        const double* b = place[shardFace[i][1]];
        const double* c = place[shardFace[i][2]];
        double centre[3] = {(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0};
        double hollow = (shell[shardFace[i][0]] + shell[shardFace[i][1]] + shell[shardFace[i][2]]) / 3.0;
        Material face = stone;
        if (hollow < 0.965 + 0.06 * rockJitter(seed + 1487u, i, 2u)) {
            face = dust;
        }
        for (uint32_t vein = 0; vein < ROCK_SEAMS; vein++) {
            double gap = fabs(centre[0] * seams[vein][0] + centre[1] * seams[vein][1] +
                              centre[2] * seams[vein][2] - seams[vein][3]);
            double soak = 1.0 - gap / seams[vein][4];
            if (soak > 0.0 && rockJitter(seed + 1601u, i, vein) + 0.5 < soak * 1.30) {
                face = ore;
            }
        }
        if (hollow < 0.86 && (rockHash(seed + i * 37u) & 7u) == 0) {
            face = ore;
        }
        pushTriangle(a, b, c, origin, NULL, NULL, NULL, face, face, face);
    }
}

static void buildFerrocite(void) {
    static const Material body = {{0.112, 0.100, 0.088}, 0.0, 0.03, 0.95, SURFACE_STONE, 9.0};
    static const Material grain = {{0.158, 0.142, 0.124}, 0.0, 0.02, 0.98, SURFACE_STONE, 5.0};
    static const Material vein = {{0.284, 0.168, 0.094}, 0.0, 0.55, 0.42, SURFACE_STONE, 3.4};
    static const double stretch[3] = {1.22, 0.90, 0.96};
    rock(17u, 14, 5, 6, 0.20, 0.58, stretch, body, grain, vein);
}

static void buildHalcyte(void) {
    static const Material body = {{0.100, 0.110, 0.122}, 0.0, 0.03, 0.93, SURFACE_STONE, 9.0};
    static const Material grain = {{0.146, 0.158, 0.166}, 0.0, 0.02, 0.97, SURFACE_STONE, 5.0};
    static const Material vein = {{0.176, 0.318, 0.352}, 0.0, 0.28, 0.20, SURFACE_STONE, 3.4};
    static const double stretch[3] = {1.06, 0.94, 1.00};
    rock(5303u, 12, 9, 3, 0.24, 0.74, stretch, body, grain, vein);
}

static void buildVantalum(void) {
    static const Material body = {{0.094, 0.086, 0.104}, 0.0, 0.04, 0.91, SURFACE_STONE, 9.0};
    static const Material grain = {{0.132, 0.124, 0.148}, 0.0, 0.03, 0.96, SURFACE_STONE, 5.0};
    static const Material vein = {{0.228, 0.146, 0.290}, 0.0, 0.70, 0.34, SURFACE_STONE, 3.4};
    static const double stretch[3] = {1.40, 0.80, 0.86};
    rock(90101u, 12, 3, 8, 0.19, 0.52, stretch, body, grain, vein);
}

#define RAY_VARIANTS 6
#define RAY_SECTIONS 16
#define RAY_RAGGED 1.84

static uint32_t rayVariant;

static void rayRing(const Section* section, uint32_t sides, uint32_t ring, double* out) {
    for (uint32_t i = 0; i < sides; i++) {
        double angle = 0.261799 + 6.283185307179586 * (double)i / (double)sides;
        double swell = 1.0 + RAY_RAGGED * 2.0 * rockJitter(rayVariant * 977u + 61u, ring, i);
        out[i * 3 + 0] = section->x;
        out[i * 3 + 1] = section->y + section->radiusY * swell * cos(angle);
        out[i * 3 + 2] = section->z + section->radiusZ * swell * sin(angle);
    }
}

static void ray(const double (*profile)[2], const double (*heat)[4], uint32_t count, uint32_t sides) {
    if (count > RAY_SECTIONS) {
        return;
    }
    double rings[2][MAX_RING * 3];
    Material shades[RAY_SECTIONS];
    for (uint32_t i = 0; i < count; i++) {
        shades[i].colour[0] = heat[i][0];
        shades[i].colour[1] = heat[i][1];
        shades[i].colour[2] = heat[i][2];
        shades[i].emissive = heat[i][3];
        shades[i].metallic = 0.0;
        shades[i].roughness = 1.0;
        shades[i].pattern = SURFACE_SMOOTH;
        shades[i].plate = 0.0;
    }
    Section section = {profile[0][0], 0.0, 0.0, profile[0][1], profile[0][1]};
    rayRing(&section, sides, 0, rings[0]);
    for (uint32_t i = 1; i < count; i++) {
        Section next = {profile[i][0], 0.0, 0.0, profile[i][1], profile[i][1]};
        rayRing(&next, sides, i, rings[i & 1]);
        loft(rings[(i - 1) & 1], rings[i & 1], sides, 1, shades[i - 1], shades[i]);
    }
}

static void buildBeam(void) {
    static const double profile[10][2] = {
        {0.000, 0.0013}, {0.030, 0.0031}, {0.140, 0.0027}, {0.280, 0.0024}, {0.420, 0.0026},
        {0.560, 0.0023}, {0.700, 0.0025}, {0.840, 0.0028}, {0.968, 0.0043}, {1.000, 0.0009}
    };
    static const double heat[10][4] = {
        {1.00, 0.99, 0.95, 11.20},
        {1.00, 0.96, 0.86, 8.80},
        {1.00, 0.94, 0.81, 6.20},
        {1.00, 0.92, 0.76, 4.80},
        {1.00, 0.92, 0.76, 5.00},
        {1.00, 0.92, 0.75, 5.20},
        {1.00, 0.93, 0.78, 6.40},
        {1.00, 0.95, 0.84, 9.00},
        {1.00, 0.97, 0.90, 14.00},
        {1.00, 0.99, 0.97, 19.00}
    };
    ray(profile, heat, 10, 10);
}

static void buildPrinter(void) {
    static const double profile[10][2] = {
        {0.000, 0.0011}, {0.040, 0.0027}, {0.150, 0.0023}, {0.290, 0.0020}, {0.430, 0.0021},
        {0.570, 0.0020}, {0.710, 0.0021}, {0.850, 0.0024}, {0.972, 0.0046}, {1.000, 0.0008}
    };
    static const double heat[10][4] = {
        {0.90, 0.97, 1.00, 1.50},
        {0.82, 0.94, 1.00, 1.20},
        {0.78, 0.92, 1.00, 0.90},
        {0.74, 0.89, 1.00, 0.70},
        {0.75, 0.90, 1.00, 0.75},
        {0.75, 0.90, 1.00, 0.80},
        {0.80, 0.92, 1.00, 1.00},
        {0.86, 0.95, 1.00, 1.50},
        {0.88, 0.96, 1.00, 2.10},
        {0.96, 0.99, 1.00, 2.90}
    };
    ray(profile, heat, 10, 10);
}

static double normalise(double longest) {
    if (longest <= 0.0) {
        for (uint32_t i = 0; i < vertexCount; i++) {
            const float* position = vertexData + (size_t)i * STRIDE;
            double length = sqrt((double)position[0] * position[0] + (double)position[1] * position[1] +
                                 (double)position[2] * position[2]);
            if (length > longest) {
                longest = length;
            }
        }
    }
    if (longest < 1e-9) {
        return 1.0;
    }
    for (uint32_t i = 0; i < vertexCount; i++) {
        float* position = vertexData + (size_t)i * STRIDE;
        for (uint32_t axis = 0; axis < 3; axis++) {
            position[axis] = (float)((double)position[axis] / longest);
        }
    }
    for (uint32_t i = 0; i < nozzleCount * 4; i++) {
        nozzleData[i] /= longest;
    }
    for (uint32_t i = 0; i < hardpointCount; i++) {
        for (uint32_t axis = 0; axis < 3; axis++) {
            hardpointData[i * 6 + axis] /= longest;
        }
    }
    return longest;
}

static void emitFloat(FILE* output, uint32_t index, double value) {
    char literal[32];
    snprintf(literal, sizeof(literal), "%.9g", value);
    fprintf(output, "%s%s%sf", index ? "," : "", literal, strpbrk(literal, ".eE") ? "" : ".0");
}

static void emit(FILE* output, const char* name, int mounts) {
    fprintf(output, "static const float %sVertices[] = {", name);
    for (uint32_t i = 0; i < vertexCount * STRIDE; i++) {
        emitFloat(output, i, (double)vertexData[i]);
        if ((i % 12) == 11) {
            fprintf(output, "\n");
        }
    }
    fprintf(output, "};\nstatic const uint32_t %sIndices[] = {", name);
    for (uint32_t i = 0; i < indexCount; i++) {
        fprintf(output, "%s%u", i ? "," : "", indexData[i]);
        if ((i % 24) == 23) {
            fprintf(output, "\n");
        }
    }
    fprintf(output, "};\n");
    if (nozzleCount) {
        fprintf(output, "static const float %sNozzles[] = {", name);
        for (uint32_t i = 0; i < nozzleCount * 4; i++) {
            emitFloat(output, i, nozzleData[i]);
        }
        fprintf(output, "};\n");
    }
    if (mounts && hardpointCount) {
        fprintf(output, "static const float %sMounts[] = {", name);
        for (uint32_t i = 0; i < hardpointCount * 6; i++) {
            emitFloat(output, i, hardpointData[i]);
        }
        fprintf(output, "};\n");
    }
    printf("%-12s %5u vertices %5u triangles %u nozzles %u mounts\n",
           name, vertexCount, indexCount / 3, nozzleCount, mounts ? hardpointCount : 0);
#ifdef SERENITAS_DEBUG
    double extent[3][2] = {{1e9, -1e9}, {1e9, -1e9}, {1e9, -1e9}};
    for (uint32_t i = 0; i < vertexCount; i++) {
        const float* position = vertexData + (size_t)i * STRIDE;
        for (uint32_t axis = 0; axis < 3; axis++) {
            extent[axis][0] = position[axis] < extent[axis][0] ? position[axis] : extent[axis][0];
            extent[axis][1] = position[axis] > extent[axis][1] ? position[axis] : extent[axis][1];
        }
    }
    logDebug("%-12s x %6.3f..%6.3f  y %6.3f..%6.3f  z %6.3f..%6.3f\n", "",
             extent[0][0], extent[0][1], extent[1][0], extent[1][1], extent[2][0], extent[2][1]);
    for (uint32_t i = 0; i < hardpointCount; i++) {
        logDebug("%-12s hardpoint %u   {%9.6f, %9.6f, %9.6f}   along {%4.1f, %4.1f, %4.1f}\n", "", i,
                 hardpointData[i * 6 + 0], hardpointData[i * 6 + 1], hardpointData[i * 6 + 2],
                 hardpointData[i * 6 + 3], hardpointData[i * 6 + 4], hardpointData[i * 6 + 5]);
    }
#endif
}

static void emitExtent(FILE* output, const char* name) {
    double lowest = 1e9;
    double highest = -1e9;
    for (uint32_t i = 0; i < vertexCount; i++) {
        double height = vertexData[(size_t)i * STRIDE + 1];
        lowest = height < lowest ? height : lowest;
        highest = height > highest ? height : highest;
    }
    fprintf(output, "static const float %sLow = %.9gf;\nstatic const float %sHigh = %.9gf;\n",
            name, lowest, name, highest);
}

static double produce(FILE* output, const char* name, void (*build)(void), double divisor, int mounts);

static void emitVariants(FILE* output, const char* name, const char* macro, uint32_t indices) {
    fprintf(output, "#define %s_INDEX_COUNT %u\n", macro, indices);
    fprintf(output, "static const float* const %sVertices[RAY_VARIANTS] = {", name);
    for (uint32_t i = 0; i < RAY_VARIANTS; i++) {
        fprintf(output, "%s%s%uVertices", i ? ", " : "", name, i);
    }
    fprintf(output, "};\nstatic const uint32_t* const %sIndices[RAY_VARIANTS] = {", name);
    for (uint32_t i = 0; i < RAY_VARIANTS; i++) {
        fprintf(output, "%s%s%uIndices", i ? ", " : "", name, i);
    }
    fprintf(output, "};\n");
}

static void produceVariants(FILE* output, const char* name, const char* macro, void (*build)(void)) {
    for (uint32_t i = 0; i < RAY_VARIANTS; i++) {
        char label[32];
        snprintf(label, sizeof(label), "%s%u", name, i);
        rayVariant = i;
        produce(output, label, build, -1.0, 0);
    }
    emitVariants(output, name, macro, indexCount);
}

static double produce(FILE* output, const char* name, void (*build)(void), double divisor, int mounts) {
    vertexCount = 0;
    indexCount = 0;
    nozzleCount = 0;
    hardpointCount = 0;
    memset(vertexHead, 0xFF, sizeof(vertexHead));
    liveryScale = 0.0;
    build();
    if (divisor >= 0.0) {
        divisor = normalise(divisor);
    }
    emit(output, name, mounts);
    return divisor;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        logError("usage: models output.h\n");
        return 1;
    }
    FILE* output = fopen(argv[1], "wb");
    if (!output) {
        logError("cannot write %s\n", argv[1]);
        return 1;
    }
    produce(output, "interceptor", buildInterceptor, 0.0, 0);
    produce(output, "scout", buildScout, 0.0, 0);
    produce(output, "bomber", buildBomber, 0.0, 0);
    double corvetteScale = produce(output, "corvette", buildCorvette, 0.0, 1);
    produce(output, "turret", buildTurret, corvetteScale, 0);
    produce(output, "gun", buildGun, corvetteScale, 0);
    fprintf(output, "static const float gunTrunnion = %.9gf;\n", TRUNNION / corvetteScale);
    produce(output, "bomb", buildBomb, 0.0, 0);
    produce(output, "flame", buildFlame, -1.0, 0);
    produce(output, "drone", buildDrone, 0.0, 1);
    produce(output, "station", buildStation, 0.0, 1);
    emitExtent(output, "station");
    produce(output, "ferrocite", buildFerrocite, 0.0, 0);
    produce(output, "halcyte", buildHalcyte, 0.0, 0);
    produce(output, "vantalum", buildVantalum, 0.0, 0);
    fprintf(output, "#define RAY_VARIANTS %u\n", RAY_VARIANTS);
    produceVariants(output, "beam", "BEAM", buildBeam);
    produceVariants(output, "printer", "PRINTER", buildPrinter);
    fclose(output);
    return 0;
}
