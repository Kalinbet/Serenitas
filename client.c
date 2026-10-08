#define VK_USE_PLATFORM_WIN32_KHR
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "debug.h"
#include "protocol.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")

#define WINDOW_TITLE L"Serenitas"
#define FRAMES_IN_FLIGHT 2
#define EXPOSURE 1.0f
#define CAMERA_SPEED 5000.0
#define FOG_SCATTERING 1.0e-6f
#define FOG_PHASE 0.35f
#define FOG_STEPS 12.0f
#define FOG_RANGE 150000.0f
#define MAX_NETWORK_ENTITIES 288
#define MAX_EXHAUSTS 12
#define EXHAUST_GLOW 2.5e5f
#define SNAPSHOT_HISTORY 4
#define SNAPSHOT_DELAY 1.5
#define SNAPSHOT_CATCHUP 0.25
#define NETWORK_TRACKS 512
#define TRACK_GRACE 5.0
#define TRACK_REACH 3.0
#define MAX_SHOTS 512
#define BEAM_TRACKS 64
#define BEAM_RAYS 2
#define BEAM_ATTACK 6.5f
#define BEAM_RELEASE 2.8f
#define BEAM_FLOW 3.5f
#define BEAM_IGNITION 11.0f
#define BEAM_FLASH 2.2f
#define BEAM_GLOW 9.0e6f
#define BEAM_CHURN 34.0f
#define BEAM_TWIST 2.6f
#define BUILD_BEAM_GAIN 0.45f

enum {
    ACTION_PRODUCE_DRONE, ACTION_PRODUCE_INTERCEPTOR, ACTION_PRODUCE_SCOUT, ACTION_PRODUCE_BOMBER, ACTION_PRODUCE_CORVETTE,
    ACTION_STUDY_INTERCEPTOR, ACTION_STUDY_SCOUT, ACTION_STUDY_BOMBER, ACTION_STUDY_CORVETTE,
    ACTION_RAISE_STATION, ACTION_LAUNCH, ACTION_COUNT
};
#define ACTION_PRODUCE_COUNT 5
#define ACTION_STUDY_COUNT 4

static const uint16_t actionKind[ACTION_COUNT] = {
    KIND_DRONE, KIND_INTERCEPTOR, KIND_SCOUT, KIND_BOMBER, KIND_CORVETTE,
    KIND_INTERCEPTOR, KIND_SCOUT, KIND_BOMBER, KIND_CORVETTE, KIND_STATION, 0
};
static const uint16_t actionMode[ACTION_COUNT] = {
    PRODUCE_BUILD, PRODUCE_BUILD, PRODUCE_BUILD, PRODUCE_BUILD, PRODUCE_BUILD,
    PRODUCE_RESEARCH, PRODUCE_RESEARCH, PRODUCE_RESEARCH, PRODUCE_RESEARCH, PRODUCE_BUILD, PRODUCE_LAUNCH
};
static const int actionDefaultKey[ACTION_COUNT] = {
    '1', '2', '3', '4', '5', VK_F1, VK_F2, VK_F3, VK_F4, 'B', '0'
};
static const char* const kindName[KIND_COUNT] = {
    "Interceptor", "Scout", "Bomber", "Corvette", "Drone", "Station", "Ferrocite", "Halcyte", "Vantalum"
};

typedef struct {
    int pathTracing;
    int actionKey[ACTION_COUNT];
} Settings;

static Settings settings;
static char settingsPath[MAX_PATH];
static int menuOpen = 0;
static int rebindAction = -1;
static float wheelTravel = 0.0f;

static void settingsDefaults(void) {
    settings.pathTracing = 1;
    for (uint32_t i = 0; i < ACTION_COUNT; i++) {
        settings.actionKey[i] = actionDefaultKey[i];
    }
}

static void settingsSave(void) {
    if (!settingsPath[0]) {
        return;
    }
    FILE* file = fopen(settingsPath, "w");
    if (!file) {
        logError("settings: cannot write %s\n", settingsPath);
        return;
    }
    fprintf(file, "pathtracing %d\n", settings.pathTracing);
    for (uint32_t i = 0; i < ACTION_COUNT; i++) {
        fprintf(file, "key %u %d\n", i, settings.actionKey[i]);
    }
    fclose(file);
}

static void settingsLoad(void) {
    settingsDefaults();
    const char* roaming = getenv("APPDATA");
    if (!roaming) {
        settingsPath[0] = '\0';
        return;
    }
    char folder[MAX_PATH];
    snprintf(folder, sizeof(folder), "%s\\Serenitas", roaming);
    CreateDirectoryA(folder, NULL);
    snprintf(settingsPath, sizeof(settingsPath), "%s\\settings.cfg", folder);
    FILE* file = fopen(settingsPath, "r");
    if (file) {
        char name[32];
        while (fscanf(file, "%31s", name) == 1) {
            int value = 0;
            int slot = 0;
            if (!strcmp(name, "pathtracing") && fscanf(file, "%d", &value) == 1) {
                settings.pathTracing = value != 0;
            } else if (!strcmp(name, "key") && fscanf(file, "%d %d", &slot, &value) == 2 &&
                       slot >= 0 && slot < ACTION_COUNT) {
                settings.actionKey[slot] = value;
            } else {
                break;
            }
        }
        fclose(file);
    }
    logDebug("settings: %s pipeline %d\n", settingsPath, settings.pathTracing);
}

static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_MOUSEWHEEL:
            wheelTravel += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
            return 0;
        case WM_KEYDOWN:
            if (lParam & 0x40000000) {
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (rebindAction >= 0) {
                    rebindAction = -1;
                } else {
                    menuOpen = !menuOpen;
                }
            } else if (rebindAction >= 0) {
                for (uint32_t i = 0; i < ACTION_COUNT; i++) {
                    if (settings.actionKey[i] == (int)wParam) {
                        settings.actionKey[i] = 0;
                    }
                }
                settings.actionKey[rebindAction] = (int)wParam;
                rebindAction = -1;
                settingsSave();
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

static SnapshotStation networkStations[SNAPSHOT_YARDS];
static uint32_t networkStationCount;
static SnapshotYards networkYards;
static SnapshotCelestial networkCelestials[CELESTIAL_CAP];
static uint32_t networkCelestialCount;
typedef struct {
    double muzzle[3];
    double current[3];
    float velocity[3];
    double fireTick;
    double endTick;
    double drawn;
    uint32_t owner;
    uint16_t kind;
} NetworkShot;
static NetworkShot networkShots[MAX_SHOTS];
static uint32_t networkShotCount;
typedef struct {
    uint32_t entity;
    double position[3];
    float orientation[4];
    float throttle;
    uint32_t owner;
    uint32_t target;
    uint8_t kind;
    uint8_t hull;
    uint8_t armor;
    uint8_t shield;
    uint8_t cargo;
} NetworkEntity;
typedef struct {
    uint32_t entity;
    double tick[SNAPSHOT_HISTORY];
    double position[SNAPSHOT_HISTORY][3];
    float orientation[SNAPSHOT_HISTORY][4];
    float throttle[SNAPSHOT_HISTORY];
    uint32_t owner;
    uint32_t target;
    uint8_t kind;
    uint8_t hull;
    uint8_t armor;
    uint8_t shield;
    uint8_t cargo;
    uint8_t samples;
} NetworkTrack;
static NetworkTrack networkTracks[NETWORK_TRACKS];
static uint32_t networkTrackCount;
static NetworkEntity networkEntities[NETWORK_TRACKS + CELESTIAL_CAP];
static uint32_t networkEntityCount;
static uint64_t networkPlayer;
static uint32_t selectedEntities[MAX_NETWORK_ENTITIES];
static uint32_t selectedCount;

static int isSelected(uint32_t entity) {
    for (uint32_t i = 0; i < selectedCount; i++) {
        if (selectedEntities[i] == entity) {
            return 1;
        }
    }
    return 0;
}

static void crossAxes(const float* forward, float* sideA, float* sideB) {
    float helper[3] = {0.0f, 1.0f, 0.0f};
    if (forward[1] > 0.9f || forward[1] < -0.9f) {
        helper[0] = 1.0f;
        helper[1] = 0.0f;
    }
    sideA[0] = forward[1] * helper[2] - forward[2] * helper[1];
    sideA[1] = forward[2] * helper[0] - forward[0] * helper[2];
    sideA[2] = forward[0] * helper[1] - forward[1] * helper[0];
    float length = sqrtf(sideA[0] * sideA[0] + sideA[1] * sideA[1] + sideA[2] * sideA[2]);
    for (uint32_t axis = 0; axis < 3; axis++) {
        sideA[axis] /= length;
    }
    sideB[0] = forward[1] * sideA[2] - forward[2] * sideA[1];
    sideB[1] = forward[2] * sideA[0] - forward[0] * sideA[2];
    sideB[2] = forward[0] * sideA[1] - forward[1] * sideA[0];
}

static void relativeTo(const double* point, const double* camera, float* out) {
    out[0] = (float)(point[0] - camera[0]);
    out[1] = (float)(point[1] - camera[1]);
    out[2] = (float)(point[2] - camera[2]);
}

static void directionAngles(const float* direction, const float* right, const float* up, const float* forward, float* outAngles) {
    float x = direction[0] * right[0] + direction[1] * right[1] + direction[2] * right[2];
    float y = direction[0] * up[0] + direction[1] * up[1] + direction[2] * up[2];
    float depth = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2];
    outAngles[0] = atan2f(x, depth);
    outAngles[1] = atan2f(y, depth);
}

static float normaliseVector(float* value) {
    float length = sqrtf(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length > 1.0e-6f) {
        value[0] /= length;
        value[1] /= length;
        value[2] /= length;
    }
    return length;
}

static void crossVector(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void sphereAngles(const float* direction, const float* axisR, const float* axisU, const float* axisF, float* out) {
    float x = direction[0] * axisR[0] + direction[1] * axisR[1] + direction[2] * axisR[2];
    float y = direction[0] * axisU[0] + direction[1] * axisU[1] + direction[2] * axisU[2];
    float z = direction[0] * axisF[0] + direction[1] * axisF[1] + direction[2] * axisF[2];
    float reach = sqrtf(x * x + y * y + z * z);
    float height = reach > 1.0e-6f ? y / reach : 0.0f;
    out[0] = atan2f(x, z);
    out[1] = asinf(height < -1.0f ? -1.0f : (height > 1.0f ? 1.0f : height));
}

static void sphereDirection(float azimuth, float elevation, const float* axisR, const float* axisU, const float* axisF,
                            float* out) {
    float flat = cosf(elevation);
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] = axisR[axis] * (sinf(azimuth) * flat) + axisU[axis] * sinf(elevation) +
                    axisF[axis] * (cosf(azimuth) * flat);
    }
}

#define SELECTION_REACH 1.45f
#define SELECTION_LEAST 0.018f

/* The patch hangs off the world vertical, not off the camera: its side edges
   stay square to the horizontal plane however far the view is pitched. */
static void selectionBounds(const float* from, const float* to, const float* cameraForward,
                            float* axisR, float* axisU, float* axisF, float* azimuth, float* elevation) {
    static const float worldUp[3] = {0.0f, 1.0f, 0.0f};
    for (uint32_t axis = 0; axis < 3; axis++) {
        axisF[axis] = from[axis] + to[axis];
    }
    if (normaliseVector(axisF) < 1.0e-3f) {
        axisF[0] = from[0];
        axisF[1] = from[1];
        axisF[2] = from[2];
        normaliseVector(axisF);
    }
    crossVector(worldUp, axisF, axisR);
    if (normaliseVector(axisR) < 1.0e-3f) {
        crossVector(cameraForward, axisF, axisR);
        normaliseVector(axisR);
    }
    crossVector(axisF, axisR, axisU);
    float head[2];
    float tail[2];
    sphereAngles(from, axisR, axisU, axisF, head);
    sphereAngles(to, axisR, axisU, axisF, tail);
    azimuth[0] = head[0] < tail[0] ? head[0] : tail[0];
    azimuth[1] = head[0] > tail[0] ? head[0] : tail[0];
    elevation[0] = head[1] < tail[1] ? head[1] : tail[1];
    elevation[1] = head[1] > tail[1] ? head[1] : tail[1];
    for (uint32_t edge = 0; edge < 2; edge++) {
        azimuth[edge] = azimuth[edge] < -SELECTION_REACH ? -SELECTION_REACH
                                                         : (azimuth[edge] > SELECTION_REACH ? SELECTION_REACH : azimuth[edge]);
        elevation[edge] = elevation[edge] < -SELECTION_REACH ? -SELECTION_REACH
                                                             : (elevation[edge] > SELECTION_REACH ? SELECTION_REACH : elevation[edge]);
    }
}

/* A tap leaves the patch with no width at all, so picking - and only picking -
   widens it enough to catch whatever sits under the crosshair. */
static void selectionPad(float* azimuth, float* elevation) {
    if (azimuth[1] - azimuth[0] >= SELECTION_LEAST || elevation[1] - elevation[0] >= SELECTION_LEAST) {
        return;
    }
    float middle[2] = {(azimuth[0] + azimuth[1]) * 0.5f, (elevation[0] + elevation[1]) * 0.5f};
    azimuth[0] = middle[0] - SELECTION_LEAST;
    azimuth[1] = middle[0] + SELECTION_LEAST;
    elevation[0] = middle[1] - SELECTION_LEAST;
    elevation[1] = middle[1] + SELECTION_LEAST;
}

static float angleToNdc(float angle, float tangent) {
    float limit = 1.5533f;
    float clamped = angle > limit ? limit : (angle < -limit ? -limit : angle);
    return tanf(clamped) / tangent;
}

static int projectPoint(const double* point, const double* camera, const float* right, const float* up, const float* forward, float tangentX, float tangentY, float* outNdc) {
    float direction[3];
    relativeTo(point, camera, direction);
    if (direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2] < 0.15f) {
        return 0;
    }
    float angles[2];
    directionAngles(direction, right, up, forward, angles);
    outNdc[0] = angleToNdc(angles[0], tangentX);
    outNdc[1] = -angleToNdc(angles[1], tangentY);
    return 1;
}
static SOCKET networkSocket = INVALID_SOCKET;
static int networkConnected = 0;
static uint32_t networkSequence = 0;
static uint32_t networkSnapshotTick = 0;
static uint32_t networkStagingCount = 0;
static uint32_t networkStagingMask = 0;
static NetworkEntity networkStaging[MAX_NETWORK_ENTITIES];
static uint64_t networkNewestTick = 0;
static int networkHaveTick = 0;
static double networkPlayback = 0.0;
static double networkTickRate = 20.0;
static float networkHelloTimer = 0.0f;
static float networkKeepAliveTimer = 0.0f;
static float networkSilence = 0.0f;

static void networkSend(uint16_t type, uint32_t sequence, const void* body, uint16_t bodySize) {
    unsigned char buffer[PACKET_CAP];
    PacketHeader header = {PROTOCOL_MAGIC, type, bodySize, sequence};
    memcpy(buffer, &header, sizeof(header));
    if (bodySize) {
        memcpy(buffer + sizeof(header), body, bodySize);
    }
    send(networkSocket, (const char*)buffer, (int)(sizeof(header) + bodySize), 0);
}

static void networkCommand(const double* target, uint32_t targetEntity, uint32_t type, float radius, const uint32_t* entities, uint32_t count) {
    if (networkSocket == INVALID_SOCKET || !networkConnected) {
        return;
    }
    unsigned char body[COMMAND_HEADER + sizeof(uint32_t) * MAX_NETWORK_ENTITIES];
    memset(body, 0, COMMAND_HEADER);
    memcpy(body, target, sizeof(double) * 3);
    memcpy(body + 24, &targetEntity, sizeof(targetEntity));
    memcpy(body + 32, &count, sizeof(count));
    memcpy(body + 36, &type, sizeof(type));
    memcpy(body + 40, &radius, sizeof(radius));
    memcpy(body + COMMAND_HEADER, entities, count * sizeof(uint32_t));
    networkSend(PACKET_COMMAND, ++networkSequence, body, (uint16_t)(COMMAND_HEADER + count * sizeof(uint32_t)));
}

static void networkProduce(uint32_t station, uint16_t kind, uint16_t mode) {
    if (networkSocket == INVALID_SOCKET || !networkConnected) {
        return;
    }
    unsigned char body[8];
    memcpy(body, &station, sizeof(station));
    memcpy(body + 4, &kind, sizeof(kind));
    memcpy(body + 6, &mode, sizeof(mode));
    networkSend(PACKET_PRODUCE, ++networkSequence, body, sizeof(body));
}

static int stationIndexOf(uint32_t entity) {
    for (uint32_t yard = 0; yard < networkStationCount; yard++) {
        if ((networkStations[yard].entity & 0xFFFFFFu) == (entity & 0xFFFFFFu)) {
            return (int)yard;
        }
    }
    return -1;
}

static void performAction(uint32_t action, int station, int worker, int* buildPending, int* orderHeightStage) {
    if (action == ACTION_RAISE_STATION) {
        if (!worker) {
            return;
        }
        *buildPending = !*buildPending;
        *orderHeightStage = 0;
        logDebug("%s\n", *buildPending ? "build site: right click a point" : "build cancelled");
        return;
    }
    if (station < 0) {
        return;
    }
    networkProduce(networkStations[station].entity, actionKind[action], actionMode[action]);
    logDebug("action %u kind %u at station %u\n", action, actionKind[action],
             networkStations[station].entity & 0xFFFFFFu);
}

static void networkStart(const char* host) {
    WSADATA startup;
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0) {
        return;
    }
    networkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (networkSocket == INVALID_SOCKET) {
        return;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(SERVER_PORT)};
    if (InetPtonA(AF_INET, host, &address.sin_addr) != 1 ||
        connect(networkSocket, (struct sockaddr*)&address, sizeof(address)) != 0) {
        closesocket(networkSocket);
        networkSocket = INVALID_SOCKET;
        return;
    }
    u_long nonBlocking = 1;
    ioctlsocket(networkSocket, FIONBIO, &nonBlocking);
    logDebug("connecting to %s:%d\n", host, SERVER_PORT);
}

static void quaternionUnpack(const int16_t* packed, float* out) {
    float length = 0.0f;
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = (float)packed[i] / 32767.0f;
        length += out[i] * out[i];
    }
    length = sqrtf(length);
    if (length < 0.5f) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        out[i] /= length;
    }
}

static void quaternionBlend(const float* from, const float* to, float alpha, float* out) {
    float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    float sign = dot < 0.0f ? -1.0f : 1.0f;
    float scaleFrom = 1.0f - alpha;
    float scaleTo = alpha * sign;
    dot *= sign;
    if (dot < 0.9995f) {
        float angle = acosf(dot);
        float sine = sinf(angle);
        scaleFrom = sinf((1.0f - alpha) * angle) / sine;
        scaleTo = sinf(alpha * angle) / sine * sign;
    }
    float length = 0.0f;
    for (uint32_t i = 0; i < 4; i++) {
        out[i] = from[i] * scaleFrom + to[i] * scaleTo;
        length += out[i] * out[i];
    }
    length = sqrtf(length);
    for (uint32_t i = 0; i < 4; i++) {
        out[i] /= length;
    }
}

static NetworkTrack* networkTrackOf(uint32_t entity, uint32_t hint) {
    if (hint < networkTrackCount && networkTracks[hint].entity == entity) {
        return &networkTracks[hint];
    }
    for (uint32_t i = 0; i < networkTrackCount; i++) {
        if (networkTracks[i].entity == entity) {
            return &networkTracks[i];
        }
    }
    if (networkTrackCount == NETWORK_TRACKS) {
        return NULL;
    }
    NetworkTrack* track = &networkTracks[networkTrackCount++];
    memset(track, 0, sizeof(*track));
    track->entity = entity;
    return track;
}

static void networkApply(uint64_t tick, const NetworkEntity* list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        NetworkTrack* track = networkTrackOf(list[i].entity, i);
        if (!track) {
            continue;
        }
        if (track->samples && track->tick[0] >= (double)tick) {
            continue;
        }
        for (uint32_t s = SNAPSHOT_HISTORY - 1; s; s--) {
            track->tick[s] = track->tick[s - 1];
            memcpy(track->position[s], track->position[s - 1], sizeof(track->position[s]));
            memcpy(track->orientation[s], track->orientation[s - 1], sizeof(track->orientation[s]));
            track->throttle[s] = track->throttle[s - 1];
        }
        track->tick[0] = (double)tick;
        memcpy(track->position[0], list[i].position, sizeof(track->position[0]));
        memcpy(track->orientation[0], list[i].orientation, sizeof(track->orientation[0]));
        track->throttle[0] = list[i].throttle;
        track->owner = list[i].owner;
        track->target = list[i].target;
        track->kind = list[i].kind;
        track->hull = list[i].hull;
        track->armor = list[i].armor;
        track->shield = list[i].shield;
        track->cargo = list[i].cargo;
        if (track->samples < SNAPSHOT_HISTORY) {
            track->samples++;
        }
    }
}

static void appearanceOf(uint32_t kind, uint32_t owner, float* scale, uint32_t* material);

static float entityScale[NETWORK_TRACKS + CELESTIAL_CAP];
static uint32_t entityMaterial[NETWORK_TRACKS + CELESTIAL_CAP];

static void cacheAppearances(void) {
    for (uint32_t i = 0; i < networkEntityCount; i++) {
        appearanceOf(networkEntities[i].kind, networkEntities[i].owner, &entityScale[i], &entityMaterial[i]);
    }
}

typedef struct {
    uint32_t entity;
    double aim[BEAM_RAYS][3];
    float trim[BEAM_RAYS];
    float level;
    float age;
    float flow;
    uint8_t rays;
    uint8_t site;
    uint8_t touched;
} BeamTrack;

static BeamTrack beamTracks[BEAM_TRACKS];
static uint32_t beamTrackCount;

static BeamTrack* beamTrackOf(uint32_t entity, int lit, float delta) {
    BeamTrack* beam = NULL;
    for (uint32_t i = 0; i < beamTrackCount; i++) {
        if (beamTracks[i].entity == entity) {
            beam = &beamTracks[i];
            break;
        }
    }
    if (!beam) {
        if (!lit || beamTrackCount == BEAM_TRACKS) {
            return NULL;
        }
        beam = &beamTracks[beamTrackCount++];
        memset(beam, 0, sizeof(*beam));
        beam->entity = entity;
        beam->flow = (float)(entity & 15u) / 16.0f;
    }
    beam->touched = 1;
    if (lit) {
        if (beam->level <= 0.0f) {
            beam->age = 0.0f;
        }
        beam->age += delta;
        beam->level += BEAM_ATTACK * delta;
        if (beam->level > 1.0f) {
            beam->level = 1.0f;
        }
    } else {
        beam->level -= BEAM_RELEASE * delta;
        if (beam->level < 0.0f) {
            beam->level = 0.0f;
        }
    }
    beam->flow += BEAM_FLOW * delta;
    beam->flow -= floorf(beam->flow);
    return beam;
}

static void beamTracksSweep(void) {
    for (uint32_t i = 0; i < beamTrackCount;) {
        if (beamTracks[i].touched && beamTracks[i].level > 0.0f) {
            beamTracks[i].touched = 0;
            i++;
            continue;
        }
        beamTracks[i] = beamTracks[--beamTrackCount];
    }
}

static const NetworkEntity* entityOfTarget(uint32_t target) {
    if (!target) {
        return NULL;
    }
    for (uint32_t i = 0; i < networkEntityCount; i++) {
        if ((networkEntities[i].entity & 0xFFFFFFu) == target) {
            return &networkEntities[i];
        }
    }
    return NULL;
}

static int shotBlocked(const double* from, const double* travel, double span, double reach, uint32_t owner) {
    for (uint32_t i = 0; i < networkEntityCount; i++) {
        if (networkEntities[i].owner == owner) {
            continue;
        }
        double offset[3];
        double along = 0.0;
        double gap = 0.0;
        for (uint32_t axis = 0; axis < 3; axis++) {
            offset[axis] = from[axis] - networkEntities[i].position[axis];
            gap += offset[axis] * offset[axis];
        }
        double radius = (double)entityScale[i];
        if (gap > (reach + radius) * (reach + radius)) {
            continue;
        }
        gap -= radius * radius;
        if (gap <= 0.0) {
            return 1;
        }
        for (uint32_t axis = 0; axis < 3; axis++) {
            along += offset[axis] * travel[axis];
        }
        double discriminant = along * along - span * gap;
        if (discriminant < 0.0) {
            continue;
        }
        double moment = (-along - sqrt(discriminant)) / span;
        if (moment >= 0.0 && moment <= 1.0) {
            return 1;
        }
    }
    return 0;
}

static void networkShotsAdvance(void) {
    for (uint32_t i = 0; i < networkShotCount;) {
        NetworkShot* shot = &networkShots[i];
        double elapsed = (networkPlayback - shot->fireTick) / networkTickRate;
        if (elapsed < 0.0) {
            elapsed = 0.0;
        }
        double travel[3];
        double span = 0.0;
        for (uint32_t axis = 0; axis < 3; axis++) {
            double reached = shot->muzzle[axis] + (double)shot->velocity[axis] * elapsed;
            travel[axis] = reached - shot->current[axis];
            span += travel[axis] * travel[axis];
            shot->current[axis] = reached;
        }
        int spent = networkPlayback >= shot->endTick;
        if (!spent && networkPlayback > shot->drawn && span > 1e-9) {
            double origin[3] = {
                shot->current[0] - travel[0], shot->current[1] - travel[1], shot->current[2] - travel[2]
            };
            spent = shotBlocked(origin, travel, span, sqrt(span), shot->owner);
        }
        shot->drawn = networkPlayback;
        if (spent) {
            networkShots[i] = networkShots[--networkShotCount];
            continue;
        }
        i++;
    }
}

static void networkInterpolate(float delta) {
    if (!networkHaveTick) {
        networkEntityCount = 0;
        networkShotCount = 0;
        networkCelestialCount = 0;
        return;
    }
    double newest = (double)networkNewestTick;
    double drift = newest - SNAPSHOT_DELAY - networkPlayback;
    double rate = 1.0 + (drift > SNAPSHOT_CATCHUP ? SNAPSHOT_CATCHUP : (drift < -SNAPSHOT_CATCHUP ? -SNAPSHOT_CATCHUP : drift));
    networkPlayback += (double)delta * networkTickRate * rate;
    if (networkPlayback > newest) {
        networkPlayback = newest;
    }
    if (networkPlayback < newest - SNAPSHOT_HISTORY) {
        networkPlayback = newest - SNAPSHOT_HISTORY;
    }
    networkEntityCount = 0;
    for (uint32_t i = 0; i < networkCelestialCount; i++) {
        NetworkEntity* out = &networkEntities[networkEntityCount++];
        memset(out, 0, sizeof(*out));
        out->entity = networkCelestials[i].entity;
        memcpy(out->position, networkCelestials[i].position, sizeof(out->position));
        out->orientation[3] = 1.0f;
        out->kind = networkCelestials[i].kind;
    }
    for (uint32_t i = 0; i < networkTrackCount;) {
        NetworkTrack* track = &networkTracks[i];
        if (networkPlayback - track->tick[0] > TRACK_GRACE) {
            networkTracks[i] = networkTracks[--networkTrackCount];
            continue;
        }
        NetworkEntity* out = &networkEntities[networkEntityCount++];
        out->entity = track->entity;
        out->owner = track->owner;
        out->target = track->target;
        out->kind = track->kind;
        out->hull = track->hull;
        out->armor = track->armor;
        out->shield = track->shield;
        out->cargo = track->cargo;
        uint32_t older = 0;
        while (older + 1 < track->samples && track->tick[older] > networkPlayback) {
            older++;
        }
        uint32_t recent = older ? older - 1 : 0;
        if (!older && track->samples > 1) {
            older = 1;
        }
        double span = track->tick[recent] - track->tick[older];
        if (span <= 0.0) {
            memcpy(out->position, track->position[recent], sizeof(out->position));
            memcpy(out->orientation, track->orientation[recent], sizeof(out->orientation));
            out->throttle = track->throttle[recent];
            i++;
            continue;
        }
        double reach = 1.0 + TRACK_REACH / span;
        double along = (networkPlayback - track->tick[older]) / span;
        along = along < 0.0 ? 0.0 : (along > reach ? reach : along);
        for (uint32_t axis = 0; axis < 3; axis++) {
            out->position[axis] = track->position[older][axis] +
                                  (track->position[recent][axis] - track->position[older][axis]) * along;
        }
        float blend = (float)(along > 1.0 ? 1.0 : along);
        out->throttle = track->throttle[older] + (track->throttle[recent] - track->throttle[older]) * blend;
        quaternionBlend(track->orientation[older], track->orientation[recent], blend, out->orientation);
        i++;
    }
    cacheAppearances();
    networkShotsAdvance();
}

static void networkUpdate(float delta) {
    if (networkSocket == INVALID_SOCKET) {
        return;
    }
    for (;;) {
        unsigned char buffer[PACKET_CAP];
        int size = recv(networkSocket, (char*)buffer, sizeof(buffer), 0);
        if (size < (int)sizeof(PacketHeader)) {
            break;
        }
        PacketHeader header;
        memcpy(&header, buffer, sizeof(header));
        if (header.magic != PROTOCOL_MAGIC) {
            continue;
        }
        if (header.type == PACKET_WELCOME && size >= (int)(sizeof(PacketHeader) + sizeof(PacketWelcome))) {
            if (!networkConnected) {
                logDebug("connected\n");
                networkTrackCount = 0;
                networkShotCount = 0;
                networkHaveTick = 0;
            }
            networkConnected = 1;
            networkSilence = 0.0f;
            PacketWelcome welcome;
            memcpy(&welcome, buffer + sizeof(PacketHeader), sizeof(welcome));
            networkPlayer = welcome.player;
            if (welcome.tickNanoseconds) {
                networkTickRate = 1e9 / (double)welcome.tickNanoseconds;
            }
            uint32_t system = 0;
            networkSend(PACKET_SUBSCRIBE, ++networkSequence, &system, sizeof(system));
        } else if (header.type == PACKET_SNAPSHOT && size >= (int)sizeof(PacketHeader) + SNAPSHOT_HEADER) {
            unsigned char* body = buffer + sizeof(PacketHeader);
            uint32_t tick;
            uint16_t count;
            double origin[3];
            memcpy(&tick, body, sizeof(tick));
            memcpy(&count, body + 4, sizeof(count));
            uint8_t index = body[6];
            uint8_t packets = body[7];
            memcpy(origin, body + 8, sizeof(origin));
            if (size < (int)(sizeof(PacketHeader) + SNAPSHOT_HEADER + count * sizeof(SnapshotEntity)) ||
                !packets || index >= packets || packets > 32) {
                continue;
            }
            if (tick != networkSnapshotTick) {
                networkSnapshotTick = tick;
                networkStagingCount = 0;
                networkStagingMask = 0;
            }
            networkStagingMask |= 1u << index;
            for (uint32_t i = 0; i < count && networkStagingCount < MAX_NETWORK_ENTITIES; i++) {
                SnapshotEntity entry;
                memcpy(&entry, body + SNAPSHOT_HEADER + i * sizeof(SnapshotEntity), sizeof(entry));
                NetworkEntity* target = &networkStaging[networkStagingCount++];
                target->entity = entry.entity;
                target->position[0] = origin[0] + (double)entry.position[0] * 0.01;
                target->position[1] = origin[1] + (double)entry.position[1] * 0.01;
                target->position[2] = origin[2] + (double)entry.position[2] * 0.01;
                quaternionUnpack(entry.orientation, target->orientation);
                target->throttle = (float)entry.throttle / 255.0f;
                target->owner = entry.owner;
                target->target = entry.target;
                target->kind = entry.kind;
                target->hull = entry.hull;
                target->armor = entry.armor;
                target->shield = entry.shield;
                target->cargo = entry.cargo;
            }
            uint32_t whole = packets == 32 ? 0xFFFFFFFFu : (1u << packets) - 1u;
            if (networkStagingMask == whole && (!networkHaveTick || tick > networkNewestTick)) {
                networkApply(tick, networkStaging, networkStagingCount);
                networkNewestTick = tick;
                if (!networkHaveTick) {
                    networkHaveTick = 1;
                    networkPlayback = (double)tick;
                }
                networkSilence = 0.0f;
            }
        } else if (header.type == PACKET_CELESTIALS && size >= (int)sizeof(PacketHeader) + CELESTIAL_HEADER) {
            unsigned char* body = buffer + sizeof(PacketHeader);
            uint32_t count;
            memcpy(&count, body + 4, sizeof(count));
            if (count > CELESTIAL_CAP ||
                size < (int)(sizeof(PacketHeader) + CELESTIAL_HEADER + count * sizeof(SnapshotCelestial))) {
                continue;
            }
            memcpy(networkCelestials, body + CELESTIAL_HEADER, count * sizeof(SnapshotCelestial));
            networkCelestialCount = count;
        } else if (header.type == PACKET_STATIONS && size >= (int)(sizeof(PacketHeader) + sizeof(SnapshotYards))) {
            unsigned char* body = buffer + sizeof(PacketHeader);
            SnapshotYards yards;
            memcpy(&yards, body, sizeof(yards));
            if (yards.count > SNAPSHOT_YARDS ||
                size < (int)(sizeof(PacketHeader) + sizeof(yards) + yards.count * sizeof(SnapshotStation))) {
                continue;
            }
            memcpy(networkStations, body + sizeof(yards), yards.count * sizeof(SnapshotStation));
            networkStationCount = yards.count;
            networkYards = yards;
        } else if (header.type == PACKET_SHOTS && size >= (int)sizeof(PacketHeader) + SHOTS_HEADER) {
            unsigned char* body = buffer + sizeof(PacketHeader);
            uint64_t tick;
            uint32_t count;
            memcpy(&tick, body, sizeof(tick));
            memcpy(&count, body + 8, sizeof(count));
            if (size < (int)(sizeof(PacketHeader) + SHOTS_HEADER + count * sizeof(SnapshotShot))) {
                continue;
            }
            for (uint32_t i = 0; i < count && networkShotCount < MAX_SHOTS; i++) {
                SnapshotShot entry;
                memcpy(&entry, body + SHOTS_HEADER + i * sizeof(SnapshotShot), sizeof(entry));
                NetworkShot* shot = &networkShots[networkShotCount++];
                memcpy(shot->muzzle, entry.position, sizeof(shot->muzzle));
                memcpy(shot->current, entry.position, sizeof(shot->current));
                memcpy(shot->velocity, entry.velocity, sizeof(shot->velocity));
                shot->fireTick = (double)tick;
                shot->endTick = (double)tick + (double)entry.life;
                shot->drawn = (double)tick;
                shot->owner = entry.owner;
                shot->kind = entry.kind;
            }
        }
    }
    if (networkConnected) {
        networkSilence += delta;
        networkKeepAliveTimer -= delta;
        if (networkKeepAliveTimer <= 0.0f) {
            uint32_t system = 0;
            networkSend(PACKET_SUBSCRIBE, ++networkSequence, &system, sizeof(system));
            networkKeepAliveTimer = 2.0f;
        }
        if (networkSilence > 3.0f) {
            logDebug("server silent\n");
            networkConnected = 0;
            networkTrackCount = 0;
            networkShotCount = 0;
            networkHaveTick = 0;
        }
    } else {
        networkHelloTimer -= delta;
        if (networkHelloTimer <= 0.0f) {
            networkSend(PACKET_HELLO, 0, NULL, 0);
            networkHelloTimer = 0.5f;
        }
    }
    networkInterpolate(delta);
}
static const uint32_t sceneVert[] =
#include "build/scene.vert.h"
;
static const uint32_t sceneFrag[] =
#include "build/scene.frag.h"
;
static const uint32_t pathtraceComp[] =
#include "build/pathtrace.comp.h"
;
static const uint32_t resolveComp[] =
#include "build/resolve.comp.h"
;
static const uint32_t overlayVert[] =
#include "build/overlay.vert.h"
;
static const uint32_t overlayFrag[] =
#include "build/overlay.frag.h"
;
static const uint32_t backgroundVert[] =
#include "build/background.vert.h"
;
static const uint32_t backgroundFrag[] =
#include "build/background.frag.h"
;
enum { IMAGE_ILLUMINATION, IMAGE_ALBEDO, IMAGE_FOG, IMAGE_OUTPUT, IMAGE_COUNT };
#define BINDING_COUNT 8

enum { UI_FILL, UI_OUTLINE, UI_GLYPH, UI_CAPSULE, UI_QUAD, UI_GLOW };

typedef struct {
    float red;
    float green;
    float blue;
    float alpha;
} Colour;

#define INK_VOID 0x070A0Fu
#define INK_PANEL 0x0D131Cu
#define INK_RAISED 0x172130u
#define INK_LINE 0x2A3A4Eu
#define INK_FAINT 0x60748Cu
#define INK_MUTED 0x93A6BEu
#define INK_TEXT 0xDDE7F4u
#define INK_AMBER 0xFFC04Au
#define INK_TEAL 0x46D8DEu
#define INK_AZURE 0x5AA8FFu
#define INK_ROSE 0xFF6A58u
#define INK_MINT 0x6BE09Cu

static float toLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static float channelToLinear(uint32_t byteValue) {
    return toLinear((float)byteValue / 255.0f);
}

static Colour colourOf(uint32_t rgb, float alpha) {
    Colour out;
    out.red = channelToLinear((rgb >> 16) & 0xFFu);
    out.green = channelToLinear((rgb >> 8) & 0xFFu);
    out.blue = channelToLinear(rgb & 0xFFu);
    out.alpha = alpha;
    return out;
}

static Colour colourFade(Colour colour, float alpha) {
    colour.alpha *= alpha;
    return colour;
}

static Colour colourMix(Colour from, Colour to, float weight) {
    Colour out;
    out.red = from.red + (to.red - from.red) * weight;
    out.green = from.green + (to.green - from.green) * weight;
    out.blue = from.blue + (to.blue - from.blue) * weight;
    out.alpha = from.alpha + (to.alpha - from.alpha) * weight;
    return out;
}

static Colour colourLift(Colour colour, float gain) {
    colour.red *= gain;
    colour.green *= gain;
    colour.blue *= gain;
    return colour;
}

static float approach(float value, float target, float rate, float delta) {
    return value + (target - value) * (1.0f - expf(-rate * delta));
}

static float easeOut(float value) {
    float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

static float pulseOf(float seconds, float period) {
    return 0.5f + 0.5f * sinf(seconds * 6.28318531f / period);
}

static float uiScale = 1.0f;

static float uiUnit(float value) {
    return value * uiScale;
}

static struct {
    VkCommandBuffer cmd;
    VkPipelineLayout layout;
    float width;
    float height;
    float pivotX;
    float pivotY;
    float scale;
    float shiftY;
    float alpha;
} uiPass;

static void uiReset(void) {
    uiPass.pivotX = 0.0f;
    uiPass.pivotY = 0.0f;
    uiPass.scale = 1.0f;
    uiPass.shiftY = 0.0f;
    uiPass.alpha = 1.0f;
}

static void uiBegin(VkCommandBuffer cmd, VkPipelineLayout layout, VkExtent2D extent) {
    uiPass.cmd = cmd;
    uiPass.layout = layout;
    uiPass.width = (float)extent.width;
    uiPass.height = (float)extent.height;
    uiReset();
}

static void uiTransform(float pivotX, float pivotY, float scale, float shiftY, float alpha) {
    uiPass.pivotX = pivotX;
    uiPass.pivotY = pivotY;
    uiPass.scale = scale;
    uiPass.shiftY = shiftY;
    uiPass.alpha = alpha;
}

static float uiMapX(float x) {
    return uiPass.pivotX + (x - uiPass.pivotX) * uiPass.scale;
}

static float uiMapY(float y) {
    return uiPass.pivotY + (y - uiPass.pivotY) * uiPass.scale + uiPass.shiftY;
}

static void uiSubmit(const float* push) {
    vkCmdPushConstants(uiPass.cmd, uiPass.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(float) * 32, push);
    vkCmdDraw(uiPass.cmd, 6, 1, 0, 0);
}

static void uiShape(uint32_t mode, float x0, float y0, float x1, float y1, float radius, float border,
                    float glow, float axis, Colour tone, Colour accent, const float* atlas) {
    float push[32] = {0.0f};
    push[0] = uiMapX(x0);
    push[1] = uiMapY(y0);
    push[2] = uiMapX(x1);
    push[3] = uiMapY(y1);
    if (mode == UI_GLYPH) {
        float snapX = floorf(push[0] + 0.5f) - push[0];
        float snapY = floorf(push[1] + 0.5f) - push[1];
        push[0] += snapX;
        push[1] += snapY;
        push[2] += snapX;
        push[3] += snapY;
    }
    push[4] = radius * uiPass.scale;
    push[5] = border * uiPass.scale;
    push[6] = 0.75f;
    push[7] = (float)mode;
    push[8] = tone.red;
    push[9] = tone.green;
    push[10] = tone.blue;
    push[11] = tone.alpha * uiPass.alpha;
    push[12] = accent.red;
    push[13] = accent.green;
    push[14] = accent.blue;
    push[15] = accent.alpha * uiPass.alpha;
    if (atlas) {
        memcpy(&push[16], atlas, sizeof(float) * 4);
    }
    push[20] = uiPass.width;
    push[21] = uiPass.height;
    push[22] = glow * uiPass.scale;
    push[23] = axis;
    uiSubmit(push);
}

static void uiFill(float x0, float y0, float x1, float y1, float radius, Colour tone) {
    uiShape(UI_FILL, x0, y0, x1, y1, radius, 0.0f, 0.0f, 0.0f, tone, tone, NULL);
}

static void uiGradient(float x0, float y0, float x1, float y1, float radius, Colour tone, Colour accent, int sideways) {
    uiShape(UI_FILL, x0, y0, x1, y1, radius, 0.0f, 0.0f, sideways ? 1.0f : 0.0f, tone, accent, NULL);
}

static void uiOutline(float x0, float y0, float x1, float y1, float radius, float border, Colour tone) {
    uiShape(UI_OUTLINE, x0, y0, x1, y1, radius, border, 0.0f, 0.0f, tone, tone, NULL);
}

static void uiGlow(float x0, float y0, float x1, float y1, float radius, float spread, Colour tone) {
    uiShape(UI_GLOW, x0, y0, x1, y1, radius, 0.0f, spread, 0.0f, tone, tone, NULL);
}

static void uiCapsule(float x0, float y0, float x1, float y1, float thickness, float dash, Colour from, Colour to) {
    uiShape(UI_CAPSULE, x0, y0, x1, y1, 0.0f, thickness * 0.5f, 0.0f, dash, from, to, NULL);
}

#define MARQUEE_STEPS 12

static int directionToPixels(const float* direction, const float* right, const float* up, const float* forward,
                             float tangentX, float tangentY, VkExtent2D extent, float* out) {
    float depth = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2];
    if (depth < 0.06f) {
        return 0;
    }
    float x = direction[0] * right[0] + direction[1] * right[1] + direction[2] * right[2];
    float y = direction[0] * up[0] + direction[1] * up[1] + direction[2] * up[2];
    out[0] = (x / (depth * tangentX) * 0.5f + 0.5f) * (float)extent.width;
    out[1] = (-y / (depth * tangentY) * 0.5f + 0.5f) * (float)extent.height;
    return 1;
}

static int projectPixels(const double* point, const double* camera, const float* right, const float* up,
                         const float* forward, float tangentX, float tangentY, VkExtent2D extent, float* out) {
    float ndc[2];
    if (!projectPoint(point, camera, right, up, forward, tangentX, tangentY, ndc)) {
        return 0;
    }
    out[0] = (ndc[0] * 0.5f + 0.5f) * (float)extent.width;
    out[1] = (ndc[1] * 0.5f + 0.5f) * (float)extent.height;
    return 1;
}

static void uiQuad(const float* corners, Colour tone, Colour accent) {
    float push[32] = {0.0f};
    push[7] = (float)UI_QUAD;
    push[8] = tone.red;
    push[9] = tone.green;
    push[10] = tone.blue;
    push[11] = tone.alpha * uiPass.alpha;
    push[12] = accent.red;
    push[13] = accent.green;
    push[14] = accent.blue;
    push[15] = accent.alpha * uiPass.alpha;
    push[20] = uiPass.width;
    push[21] = uiPass.height;
    for (uint32_t i = 0; i < 4; i++) {
        push[24 + i * 2] = uiMapX(corners[i * 2]);
        push[25 + i * 2] = uiMapY(corners[i * 2 + 1]);
    }
    uiSubmit(push);
}

static void uiRing(const double* centre, const double* camera, const float* right, const float* up, const float* forward,
                   float tangentX, float tangentY, VkExtent2D extent, float scale, float thickness, Colour tone) {
    float direction[3] = {
        (float)(centre[0] - camera[0]), (float)(centre[1] - camera[1]), (float)(centre[2] - camera[2])
    };
    float depth = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2];
    if (depth < 0.15f) {
        return;
    }
    float radius = depth * 44.0f * tangentY / (float)extent.height;
    radius = (radius < 40.0f ? 40.0f : radius) * scale;
    float previous[2] = {0.0f, 0.0f};
    int previousValid = 0;
    for (uint32_t step = 0; step <= 40; step++) {
        float angle = (float)step * (6.28318531f / 40.0f);
        double point[3] = {
            centre[0] + (double)(cosf(angle) * radius), centre[1], centre[2] + (double)(sinf(angle) * radius)
        };
        float pixel[2];
        int valid = projectPixels(point, camera, right, up, forward, tangentX, tangentY, extent, pixel);
        if (valid && previousValid) {
            float sweep = 0.55f + 0.45f * cosf(angle * 2.0f);
            uiCapsule(previous[0], previous[1], pixel[0], pixel[1], thickness, 0.0f,
                      colourFade(tone, sweep), colourFade(tone, sweep));
        }
        previous[0] = pixel[0];
        previous[1] = pixel[1];
        previousValid = valid;
    }
}

static const uint32_t oreTint[ORE_COUNT] = {0xE0913Fu, 0x46D8DEu, 0x9E76E8u};

#define PING_CAP 12
#define PING_LIFE 1.35f

typedef struct {
    double position[3];
    double base;
    float age;
    uint32_t tint;
} OrderPing;

static OrderPing orderPings[PING_CAP];
static uint32_t orderPingCursor;

static void pushOrderPing(const double* position, double plane, uint32_t tint) {
    OrderPing* ping = &orderPings[orderPingCursor % PING_CAP];
    orderPingCursor++;
    ping->position[0] = position[0];
    ping->position[1] = position[1];
    ping->position[2] = position[2];
    ping->base = plane;
    ping->age = 0.0f;
    ping->tint = tint;
}

static void drawOrderPings(const double* camera, const float* right, const float* up, const float* forward,
                           float tangentX, float tangentY, VkExtent2D extent, float delta) {
    for (uint32_t i = 0; i < PING_CAP; i++) {
        OrderPing* ping = &orderPings[i];
        if (ping->age >= PING_LIFE) {
            continue;
        }
        ping->age += delta;
        float life = ping->age / PING_LIFE;
        life = life > 1.0f ? 1.0f : life;
        float fade = (1.0f - life) * (1.0f - life);
        Colour tone = colourOf(ping->tint, 1.0f);
        double footPoint[3] = {ping->position[0], ping->base, ping->position[2]};
        float head[2];
        float foot[2];
        int headOk = projectPixels(ping->position, camera, right, up, forward, tangentX, tangentY, extent, head);
        int footOk = projectPixels(footPoint, camera, right, up, forward, tangentX, tangentY, extent, foot);
        if (headOk && footOk) {
            uiCapsule(foot[0], foot[1], head[0], head[1], uiUnit(2.0f), 0.0f,
                      colourFade(tone, 0.0f), colourFade(tone, 0.55f * fade));
        }
        for (uint32_t wave = 0; wave < 3; wave++) {
            float phase = life * 1.6f - (float)wave * 0.22f;
            if (phase <= 0.0f || phase >= 1.0f) {
                continue;
            }
            uiRing(footPoint, camera, right, up, forward, tangentX, tangentY, extent,
                   0.25f + phase * 1.35f, uiUnit(2.4f) * (1.0f - phase),
                   colourFade(tone, 0.75f * (1.0f - phase) * (1.0f - life)));
        }
        if (headOk) {
            float span = uiUnit(4.0f) + uiUnit(14.0f) * (1.0f - fade);
            uiFill(head[0] - span * 0.16f, head[1] - span, head[0] + span * 0.16f, head[1] + span, span * 0.16f,
                   colourFade(tone, 0.35f * fade));
            uiFill(head[0] - span, head[1] - span * 0.16f, head[0] + span, head[1] + span * 0.16f, span * 0.16f,
                   colourFade(tone, 0.35f * fade));
            uiGlow(head[0] - uiUnit(3.0f), head[1] - uiUnit(3.0f), head[0] + uiUnit(3.0f), head[1] + uiUnit(3.0f),
                   uiUnit(3.0f), uiUnit(18.0f) * (0.4f + life), colourFade(tone, 0.55f * fade));
        }
    }
}

#define FONT_FIRST 32
#define FONT_LAST 126
#define FONT_GLYPHS (FONT_LAST - FONT_FIRST + 1)
#define FONT_FACES 4
#define ATLAS_WIDTH 1024
#define ATLAS_HEIGHT 1536
#define ICON_COUNT 6
#define ICON_SIZE 192
#define ICON_RENDER (ICON_SIZE * 2)
enum { FACE_SMALL, FACE_BODY, FACE_HEAD, FACE_TITLE };

typedef struct {
    float atlas[4];
    float width;
    float height;
    float advance;
    float bearing;
} Glyph;

static Glyph fontGlyphs[FONT_FACES][FONT_GLYPHS];
static float fontLine[FONT_FACES];
static float iconPatch[KIND_COUNT][4];
static int iconReady[KIND_COUNT];
static unsigned char atlasPixels[ATLAS_WIDTH * ATLAS_HEIGHT * 4];
static uint32_t atlasCursor;

static void buildFontAtlas(void) {
    static const int faceSize[FONT_FACES] = {13, 16, 17, 30};
    static const int faceWeight[FONT_FACES] = {FW_SEMIBOLD, FW_MEDIUM, FW_BOLD, FW_LIGHT};
    HDC screen = GetDC(NULL);
    HDC canvas = CreateCompatibleDC(screen);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = ATLAS_WIDTH;
    info.bmiHeader.biHeight = -ATLAS_HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP surface = CreateDIBSection(canvas, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ previousBitmap = SelectObject(canvas, surface);
    RECT wipe = {0, 0, ATLAS_WIDTH, ATLAS_HEIGHT};
    FillRect(canvas, &wipe, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(canvas, TRANSPARENT);
    SetTextColor(canvas, RGB(255, 255, 255));
    float penX = 0.0f;
    float penY = 0.0f;
    float rowHeight = 0.0f;
    for (uint32_t face = 0; face < FONT_FACES; face++) {
        HFONT font = CreateFontW(-(int)(uiUnit((float)faceSize[face]) + 0.5f), 0, 0, 0, faceWeight[face], FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 VARIABLE_PITCH, L"Segoe UI");
        HGDIOBJ previousFont = SelectObject(canvas, font);
        TEXTMETRICW metrics;
        GetTextMetricsW(canvas, &metrics);
        fontLine[face] = (float)metrics.tmHeight;
        for (uint32_t index = 0; index < FONT_GLYPHS; index++) {
            WCHAR symbol = (WCHAR)(FONT_FIRST + index);
            SIZE extent;
            GetTextExtentPoint32W(canvas, &symbol, 1, &extent);
            float cellWidth = (float)extent.cx + 6.0f;
            float cellHeight = (float)metrics.tmHeight + 2.0f;
            if (penX + cellWidth > (float)ATLAS_WIDTH) {
                penX = 0.0f;
                penY += rowHeight + 1.0f;
                rowHeight = 0.0f;
            }
            TextOutW(canvas, (int)penX + 3, (int)penY, &symbol, 1);
            Glyph* glyph = &fontGlyphs[face][index];
            glyph->atlas[0] = penX / (float)ATLAS_WIDTH;
            glyph->atlas[1] = penY / (float)ATLAS_HEIGHT;
            glyph->atlas[2] = (penX + cellWidth) / (float)ATLAS_WIDTH;
            glyph->atlas[3] = (penY + cellHeight) / (float)ATLAS_HEIGHT;
            glyph->width = cellWidth;
            glyph->height = cellHeight;
            glyph->advance = (float)extent.cx;
            glyph->bearing = 3.0f;
            penX += cellWidth + 1.0f;
            rowHeight = cellHeight > rowHeight ? cellHeight : rowHeight;
        }
        SelectObject(canvas, previousFont);
        DeleteObject(font);
    }
    const unsigned char* source = (const unsigned char*)bits;
    for (uint32_t i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; i++) {
        atlasPixels[i * 4 + 0] = 255;
        atlasPixels[i * 4 + 1] = 255;
        atlasPixels[i * 4 + 2] = 255;
        atlasPixels[i * 4 + 3] = source[i * 4];
    }
    SelectObject(canvas, previousBitmap);
    DeleteObject(surface);
    DeleteDC(canvas);
    ReleaseDC(NULL, screen);
    atlasCursor = (uint32_t)(penY + rowHeight + 4.0f);
    logDebug("font rows to %u of %d\n", atlasCursor, ATLAS_HEIGHT);
}

static void uiIcon(float x0, float y0, float x1, float y1, uint32_t kind, Colour tone) {
    if (kind >= KIND_COUNT || !iconReady[kind]) {
        return;
    }
    uiShape(UI_GLYPH, x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.0f, tone, tone, iconPatch[kind]);
}

static float uiTextWidth(uint32_t face, const char* text, float tracking) {
    float total = 0.0f;
    for (uint32_t i = 0; text[i]; i++) {
        unsigned char symbol = (unsigned char)text[i];
        if (symbol < FONT_FIRST || symbol > FONT_LAST) {
            continue;
        }
        total += fontGlyphs[face][symbol - FONT_FIRST].advance + tracking;
    }
    return total > 0.0f ? total - tracking : 0.0f;
}

static void uiText(float x, float y, uint32_t face, const char* text, float tracking, Colour tone) {
    float pen = x;
    for (uint32_t i = 0; text[i]; i++) {
        unsigned char symbol = (unsigned char)text[i];
        if (symbol < FONT_FIRST || symbol > FONT_LAST) {
            continue;
        }
        const Glyph* glyph = &fontGlyphs[face][symbol - FONT_FIRST];
        if (symbol != ' ') {
            float left = pen - glyph->bearing;
            uiShape(UI_GLYPH, left, y, left + glyph->width, y + glyph->height, 0.0f, 0.0f, 0.0f, 0.0f,
                    tone, tone, glyph->atlas);
        }
        pen += glyph->advance + tracking;
    }
}

static void uiTextCentred(float centre, float y, uint32_t face, const char* text, float tracking, Colour tone) {
    uiText(centre - uiTextWidth(face, text, tracking) * 0.5f, y, face, text, tracking, tone);
}

static void uiTextRight(float right, float y, uint32_t face, const char* text, float tracking, Colour tone) {
    uiText(right - uiTextWidth(face, text, tracking), y, face, text, tracking, tone);
}

static void keyName(int key, char* out, size_t capacity) {
    if (!key) {
        snprintf(out, capacity, "NONE");
        return;
    }
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
        snprintf(out, capacity, "%c", (char)key);
        return;
    }
    if (key >= VK_F1 && key <= VK_F12) {
        snprintf(out, capacity, "F%d", key - VK_F1 + 1);
        return;
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        snprintf(out, capacity, "Num %d", key - VK_NUMPAD0);
        return;
    }
    switch (key) {
        case VK_SPACE: snprintf(out, capacity, "Space"); return;
        case VK_TAB: snprintf(out, capacity, "Tab"); return;
        case VK_RETURN: snprintf(out, capacity, "Enter"); return;
        case VK_SHIFT: snprintf(out, capacity, "Shift"); return;
        case VK_MENU: snprintf(out, capacity, "Alt"); return;
        case VK_BACK: snprintf(out, capacity, "Backspace"); return;
        case VK_OEM_MINUS: snprintf(out, capacity, "-"); return;
        case VK_OEM_PLUS: snprintf(out, capacity, "+"); return;
        case VK_OEM_COMMA: snprintf(out, capacity, ","); return;
        case VK_OEM_PERIOD: snprintf(out, capacity, "."); return;
        case VK_OEM_1: snprintf(out, capacity, ";"); return;
        case VK_OEM_2: snprintf(out, capacity, "/"); return;
        case VK_OEM_3: snprintf(out, capacity, "`"); return;
        case VK_OEM_4: snprintf(out, capacity, "["); return;
        case VK_OEM_5: snprintf(out, capacity, "\\"); return;
        case VK_OEM_6: snprintf(out, capacity, "]"); return;
        default: break;
    }
    snprintf(out, capacity, "Key %d", key);
}

static int pointInside(float x, float y, float x0, float y0, float x1, float y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
    uint32_t action;
    const char* header;
} HudButton;

#define HUD_TILE uiUnit(112.0f)
#define HUD_ICON uiUnit(76.0f)
#define HUD_BUTTON_GAP uiUnit(6.0f)
#define HUD_COLUMN_GAP uiUnit(20.0f)
#define HUD_PAD uiUnit(18.0f)
#define HUD_HEADER uiUnit(34.0f)
#define HUD_BUTTON_CAP ACTION_COUNT
#define HUD_PRODUCE_COLUMNS 3
#define HUD_STUDY_COLUMNS 2

static const uint32_t hudProduce[ACTION_PRODUCE_COUNT + 1] = {
    ACTION_PRODUCE_DRONE, ACTION_PRODUCE_INTERCEPTOR, ACTION_PRODUCE_SCOUT, ACTION_PRODUCE_BOMBER,
    ACTION_PRODUCE_CORVETTE, ACTION_LAUNCH
};
static const uint32_t hudStudy[ACTION_STUDY_COUNT] = {
    ACTION_STUDY_INTERCEPTOR, ACTION_STUDY_SCOUT, ACTION_STUDY_BOMBER, ACTION_STUDY_CORVETTE
};
static const uint32_t hudRaise[1] = {ACTION_RAISE_STATION};

static float hudGroupWidth(uint32_t columns) {
    return (float)columns * HUD_TILE + (float)(columns - 1) * HUD_BUTTON_GAP;
}

static uint32_t hudGrid(HudButton* out, uint32_t count, const char* header, const uint32_t* actions, uint32_t total,
                        uint32_t columns, float left, float top) {
    for (uint32_t i = 0; i < total; i++) {
        HudButton* button = &out[count + i];
        button->x0 = left + (float)(i % columns) * (HUD_TILE + HUD_BUTTON_GAP);
        button->y0 = top + (float)(i / columns) * (HUD_TILE + HUD_BUTTON_GAP);
        button->x1 = button->x0 + HUD_TILE;
        button->y1 = button->y0 + HUD_TILE;
        button->action = actions[i];
        button->header = i ? NULL : header;
    }
    return count + total;
}

static uint32_t hudLayout(VkExtent2D extent, int station, int worker, HudButton* out) {
    if (!station && !worker) {
        return 0;
    }
    float span = 0.0f;
    if (station) {
        span += hudGroupWidth(HUD_PRODUCE_COLUMNS) + HUD_COLUMN_GAP + hudGroupWidth(HUD_STUDY_COLUMNS);
    }
    if (worker) {
        span += (station ? HUD_COLUMN_GAP : 0.0f) + hudGroupWidth(1);
    }
    float left = floorf(((float)extent.width - span) * 0.5f);
    uint32_t rows = station ? 2u : 1u;
    float top = floorf((float)extent.height - uiUnit(36.0f) - HUD_PAD -
                       (float)rows * (HUD_TILE + HUD_BUTTON_GAP) + HUD_BUTTON_GAP);
    uint32_t count = 0;
    if (station) {
        count = hudGrid(out, count, "PRODUCE", hudProduce, ACTION_PRODUCE_COUNT + 1, HUD_PRODUCE_COLUMNS, left, top);
        left += hudGroupWidth(HUD_PRODUCE_COLUMNS) + HUD_COLUMN_GAP;
        count = hudGrid(out, count, "RESEARCH", hudStudy, ACTION_STUDY_COUNT, HUD_STUDY_COLUMNS, left, top);
        left += hudGroupWidth(HUD_STUDY_COLUMNS) + HUD_COLUMN_GAP;
    }
    if (worker) {
        count = hudGrid(out, count, "CONSTRUCT", hudRaise, 1, 1, left, top);
    }
    return count;
}

enum { MENU_TAB_GRAPHICS, MENU_TAB_CONTROLS, MENU_TAB_COUNT };
enum { MENU_TAB, MENU_PIPELINE, MENU_BIND, MENU_RESET, MENU_QUIT, MENU_RESUME };

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
    uint32_t kind;
    uint32_t action;
} MenuItem;

#define MENU_WIDTH uiUnit(640.0f)
#define MENU_HEIGHT uiUnit(560.0f)
#define MENU_PAD uiUnit(32.0f)
#define MENU_TAB_HEIGHT uiUnit(34.0f)
#define MENU_ROW_HEIGHT uiUnit(38.0f)
#define MENU_ROW_GAP uiUnit(4.0f)
#define MENU_FOOTER uiUnit(58.0f)
#define MENU_ITEM_CAP (MENU_TAB_COUNT + ACTION_COUNT + 5)

static const char* const menuTabName[MENU_TAB_COUNT] = {"GRAPHICS", "CONTROLS"};

static float menuLeft(VkExtent2D extent) {
    return floorf(((float)extent.width - MENU_WIDTH) * 0.5f);
}

static float menuTop(VkExtent2D extent) {
    return floorf(((float)extent.height - MENU_HEIGHT) * 0.5f);
}

static float menuTabLeft(VkExtent2D extent, uint32_t tab) {
    return menuLeft(extent) + MENU_PAD + (float)tab * uiUnit(136.0f);
}

static float menuTabTop(VkExtent2D extent) {
    return menuTop(extent) + uiUnit(88.0f);
}

static float menuContentTop(VkExtent2D extent) {
    return menuTabTop(extent) + MENU_TAB_HEIGHT + uiUnit(24.0f);
}

static float menuContentBottom(VkExtent2D extent) {
    return menuTop(extent) + MENU_HEIGHT - MENU_FOOTER - uiUnit(18.0f);
}

static float menuRowTop(VkExtent2D extent, uint32_t row) {
    return menuContentTop(extent) + (float)row * (MENU_ROW_HEIGHT + MENU_ROW_GAP);
}

static float menuContentSpan(uint32_t tab) {
    uint32_t rows = tab == MENU_TAB_CONTROLS ? ACTION_COUNT + 1 : 7;
    return (float)rows * (MENU_ROW_HEIGHT + MENU_ROW_GAP);
}

static float menuScrollLimit(VkExtent2D extent, uint32_t tab) {
    float slack = menuContentSpan(tab) - (menuContentBottom(extent) - menuContentTop(extent));
    return slack > 0.0f ? slack : 0.0f;
}

static uint32_t menuLayout(VkExtent2D extent, uint32_t tab, float scroll, MenuItem* out) {
    float left = menuLeft(extent) + MENU_PAD;
    float right = menuLeft(extent) + MENU_WIDTH - MENU_PAD;
    uint32_t count = 0;
    for (uint32_t i = 0; i < MENU_TAB_COUNT; i++) {
        float tabLeft = menuTabLeft(extent, i);
        out[count++] = (MenuItem){tabLeft, menuTabTop(extent), tabLeft + uiUnit(124.0f),
                                  menuTabTop(extent) + MENU_TAB_HEIGHT, MENU_TAB, i};
    }
    if (tab == MENU_TAB_GRAPHICS) {
        float top = menuRowTop(extent, 0) - scroll;
        out[count++] = (MenuItem){right - uiUnit(258.0f), top + uiUnit(4.0f), right - uiUnit(132.0f),
                                  top + MENU_ROW_HEIGHT - uiUnit(4.0f), MENU_PIPELINE, 1};
        out[count++] = (MenuItem){right - uiUnit(128.0f), top + uiUnit(4.0f), right,
                                  top + MENU_ROW_HEIGHT - uiUnit(4.0f), MENU_PIPELINE, 0};
    } else {
        for (uint32_t i = 0; i < ACTION_COUNT; i++) {
            float top = menuRowTop(extent, i) - scroll;
            out[count++] = (MenuItem){left, top, right - uiUnit(10.0f), top + MENU_ROW_HEIGHT, MENU_BIND, i};
        }
        float top = menuRowTop(extent, ACTION_COUNT) - scroll + uiUnit(4.0f);
        out[count++] = (MenuItem){right - uiUnit(190.0f), top, right - uiUnit(10.0f), top + MENU_ROW_HEIGHT,
                                  MENU_RESET, 0};
    }
    float footer = menuTop(extent) + MENU_HEIGHT - MENU_FOOTER;
    out[count++] = (MenuItem){left, footer, left + uiUnit(158.0f), footer + uiUnit(42.0f), MENU_RESUME, 0};
    out[count++] = (MenuItem){right - uiUnit(158.0f), footer, right, footer + uiUnit(42.0f), MENU_QUIT, 0};
    return count;
}

static int menuItemClipped(const MenuItem* item) {
    return item->kind == MENU_BIND || item->kind == MENU_RESET || item->kind == MENU_PIPELINE;
}

static int menuItemVisible(VkExtent2D extent, const MenuItem* item) {
    if (!menuItemClipped(item)) {
        return 1;
    }
    return item->y1 > menuContentTop(extent) && item->y0 < menuContentBottom(extent);
}

static int menuItemHit(VkExtent2D extent, const MenuItem* item, float x, float y) {
    float top = item->y0;
    float bottom = item->y1;
    if (menuItemClipped(item)) {
        float viewTop = menuContentTop(extent);
        float viewBottom = menuContentBottom(extent);
        top = top < viewTop ? viewTop : top;
        bottom = bottom > viewBottom ? viewBottom : bottom;
    }
    return bottom > top && pointInside(x, y, item->x0, top, item->x1, bottom);
}

static void actionLabel(uint32_t action, char* out, size_t capacity) {
    if (action == ACTION_LAUNCH) {
        snprintf(out, capacity, "Undock");
        return;
    }
    if (action == ACTION_RAISE_STATION) {
        snprintf(out, capacity, "Station");
        return;
    }
    snprintf(out, capacity, "%s", kindName[actionKind[action]]);
}

static void actionMenuLabel(uint32_t action, char* out, size_t capacity) {
    if (action == ACTION_LAUNCH) {
        snprintf(out, capacity, "Undock stored ships");
        return;
    }
    if (action == ACTION_RAISE_STATION) {
        snprintf(out, capacity, "Build station");
        return;
    }
    snprintf(out, capacity, "%s %s", actionMode[action] == PRODUCE_RESEARCH ? "Research" : "Produce",
             kindName[actionKind[action]]);
}

static const VkFormat imageFormats[IMAGE_COUNT] = {
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT
};
#define SPHERE_SEGMENTS 20
#define SPHERE_RINGS 10
#define SPHERE_VERTICES (SPHERE_SEGMENTS * SPHERE_RINGS * 6)
#define MATERIAL_STAR 6
#define MATERIAL_ROCK 7
#define MATERIAL_ICE 8
typedef struct {
    float position[4];
    float normal[4];
    float material[4];
    float physical[4];
} Vertex;

static void spherePoint(float ring, float segment, float* out) {
    float polar = 3.14159265f * ring / (float)SPHERE_RINGS;
    float azimuth = 6.28318531f * segment / (float)SPHERE_SEGMENTS;
    out[0] = sinf(polar) * cosf(azimuth);
    out[1] = cosf(polar);
    out[2] = sinf(polar) * sinf(azimuth);
    out[3] = 1.0f;
}

static void buildSphere(Vertex* vertices) {
    uint32_t written = 0;
    for (uint32_t ring = 0; ring < SPHERE_RINGS; ring++) {
        for (uint32_t segment = 0; segment < SPHERE_SEGMENTS; segment++) {
            float corner[4][4];
            spherePoint((float)ring, (float)segment, corner[0]);
            spherePoint((float)(ring + 1), (float)segment, corner[1]);
            spherePoint((float)(ring + 1), (float)(segment + 1), corner[2]);
            spherePoint((float)ring, (float)(segment + 1), corner[3]);
            static const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
            for (uint32_t i = 0; i < 6; i++) {
                memcpy(vertices[written].position, corner[order[i]], sizeof(float) * 4);
                memcpy(vertices[written].normal, corner[order[i]], sizeof(float) * 4);
                vertices[written].normal[3] = 0.0f;
                vertices[written].material[0] = 0.0f;
                vertices[written].material[1] = 0.0f;
                vertices[written].material[2] = 0.0f;
                vertices[written].material[3] = -1.0f;
                vertices[written].physical[0] = 0.0f;
                vertices[written].physical[1] = 1.0f;
                vertices[written].physical[2] = 0.0f;
                vertices[written].physical[3] = 0.0f;
                written++;
            }
        }
    }
}

static const float kindRadius[KIND_COUNT] = {25.0f, 30.0f, 35.0f, 180.0f, 12.0f, 400.0f, 150.0f, 120.0f, 95.0f};
static const uint32_t kindCost[KIND_COUNT][ORE_COUNT] = {
    {2400, 800, 200}, {3600, 1800, 600}, {6400, 3200, 1200}, {32000, 18000, 6000},
    {1200, 400, 0}, {20000, 10000, 15000}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}
};

#include "build/models.h"
#define MESH_FLAME 4
#define MESH_BOMB 5
#define MESH_CORVETTE 6
#define MESH_TURRET 7
#define MESH_GUN 8
#define MESH_DRONE 9
#define MESH_STATION 10
#define MESH_ROCK 11
#define MESH_BEAM 14
#define MESH_PRINTER (MESH_BEAM + RAY_VARIANTS)
#define MESH_SECTION (MESH_PRINTER + RAY_VARIANTS)
#define SECTION_SLOTS 4
#define SECTION_MESHES (SECTION_SLOTS * FRAMES_IN_FLIGHT)
#define SECTION_SEGMENTS 2048
#define SECTION_CAPACITY 4608
#define MESH_COUNT (MESH_SECTION + SECTION_MESHES)
#define NO_CEILING 1.0e18f
#define TURRET_CONE -0.0872f
#define ROUND_SCALE 6.0f
#define BOMB_SCALE 5.0f
#define INSTANCE_FLOATS 12
#define INSTANCE_FACTS 8
#define INTERCEPTOR_TRIANGLE_VERTICES (sizeof(interceptorIndices) / sizeof(interceptorIndices[0]))
#define BOMBER_TRIANGLE_VERTICES (sizeof(bomberIndices) / sizeof(bomberIndices[0]))
#define SCOUT_TRIANGLE_VERTICES (sizeof(scoutIndices) / sizeof(scoutIndices[0]))
#define FLAME_TRIANGLE_VERTICES (sizeof(flameIndices) / sizeof(flameIndices[0]))
#define BOMB_TRIANGLE_VERTICES (sizeof(bombIndices) / sizeof(bombIndices[0]))
#define CORVETTE_TRIANGLE_VERTICES (sizeof(corvetteIndices) / sizeof(corvetteIndices[0]))
#define TURRET_TRIANGLE_VERTICES (sizeof(turretIndices) / sizeof(turretIndices[0]))
#define GUN_TRIANGLE_VERTICES (sizeof(gunIndices) / sizeof(gunIndices[0]))
#define DRONE_TRIANGLE_VERTICES (sizeof(droneIndices) / sizeof(droneIndices[0]))
#define BEAM_TRIANGLE_VERTICES (BEAM_INDEX_COUNT * RAY_VARIANTS)
#define PRINTER_TRIANGLE_VERTICES (PRINTER_INDEX_COUNT * RAY_VARIANTS)
static const uint32_t kindMesh[KIND_COUNT] = {
    1, 3, 2, MESH_CORVETTE, MESH_DRONE, MESH_STATION, MESH_ROCK, MESH_ROCK + 1, MESH_ROCK + 2
};
static uint32_t meshVertexCount[MESH_COUNT];
static Vertex meshVertices[SPHERE_VERTICES + INTERCEPTOR_TRIANGLE_VERTICES +
                           BOMBER_TRIANGLE_VERTICES + SCOUT_TRIANGLE_VERTICES + FLAME_TRIANGLE_VERTICES +
                           BOMB_TRIANGLE_VERTICES + CORVETTE_TRIANGLE_VERTICES + TURRET_TRIANGLE_VERTICES +
                           GUN_TRIANGLE_VERTICES + DRONE_TRIANGLE_VERTICES +
                           sizeof(stationIndices) / sizeof(uint32_t) + SECTION_MESHES * SECTION_CAPACITY +
                           sizeof(ferrociteIndices) / sizeof(uint32_t) +
                           sizeof(halcyteIndices) / sizeof(uint32_t) +
                           sizeof(vantalumIndices) / sizeof(uint32_t) +
                           BEAM_TRIANGLE_VERTICES + PRINTER_TRIANGLE_VERTICES];
static uint32_t meshBase[MESH_COUNT];
static const float* meshNozzles[MESH_COUNT];
static uint32_t meshNozzleCount[MESH_COUNT];
static const float* meshMounts[MESH_COUNT];
static uint32_t meshMountCount[MESH_COUNT];
#define ENTITY_INSTANCES (1 + 2 * (sizeof(corvetteMounts) / sizeof(float) / 6) + sizeof(corvetteNozzles) / sizeof(float) / 4)
#define MAX_INSTANCES ((NETWORK_TRACKS + CELESTIAL_CAP) * ENTITY_INSTANCES + MAX_SHOTS)

static void expandMesh(uint32_t mesh, const float* vertices, const uint32_t* indices) {
    for (uint32_t i = 0; i < meshVertexCount[mesh]; i++) {
        const float* source = vertices + (size_t)indices[i] * 14;
        Vertex* target = &meshVertices[meshBase[mesh] + i];
        target->position[0] = source[0];
        target->position[1] = source[1];
        target->position[2] = source[2];
        target->position[3] = 1.0f;
        target->normal[0] = source[3];
        target->normal[1] = source[4];
        target->normal[2] = source[5];
        target->normal[3] = 0.0f;
        target->material[0] = source[6];
        target->material[1] = source[7];
        target->material[2] = source[8];
        target->material[3] = source[9];
        target->physical[0] = source[10];
        target->physical[1] = source[11];
        target->physical[2] = source[12];
        target->physical[3] = source[13];
    }
}

static void buildMeshes(void) {
    meshVertexCount[0] = SPHERE_VERTICES;
    meshVertexCount[1] = (uint32_t)INTERCEPTOR_TRIANGLE_VERTICES;
    meshVertexCount[2] = (uint32_t)BOMBER_TRIANGLE_VERTICES;
    meshVertexCount[3] = (uint32_t)SCOUT_TRIANGLE_VERTICES;
    meshVertexCount[MESH_FLAME] = (uint32_t)FLAME_TRIANGLE_VERTICES;
    meshVertexCount[MESH_BOMB] = (uint32_t)BOMB_TRIANGLE_VERTICES;
    meshVertexCount[MESH_CORVETTE] = (uint32_t)CORVETTE_TRIANGLE_VERTICES;
    meshVertexCount[MESH_TURRET] = (uint32_t)TURRET_TRIANGLE_VERTICES;
    meshVertexCount[MESH_GUN] = (uint32_t)GUN_TRIANGLE_VERTICES;
    meshVertexCount[MESH_DRONE] = (uint32_t)DRONE_TRIANGLE_VERTICES;
    meshVertexCount[MESH_STATION] = (uint32_t)(sizeof(stationIndices) / sizeof(uint32_t));
    meshVertexCount[MESH_ROCK] = (uint32_t)(sizeof(ferrociteIndices) / sizeof(uint32_t));
    meshVertexCount[MESH_ROCK + 1] = (uint32_t)(sizeof(halcyteIndices) / sizeof(uint32_t));
    meshVertexCount[MESH_ROCK + 2] = (uint32_t)(sizeof(vantalumIndices) / sizeof(uint32_t));
    for (uint32_t variant = 0; variant < RAY_VARIANTS; variant++) {
        meshVertexCount[MESH_BEAM + variant] = BEAM_INDEX_COUNT;
        meshVertexCount[MESH_PRINTER + variant] = PRINTER_INDEX_COUNT;
    }
    for (uint32_t slot = 0; slot < SECTION_MESHES; slot++) {
        meshVertexCount[MESH_SECTION + slot] = SECTION_CAPACITY;
    }
    meshNozzles[1] = interceptorNozzles;
    meshNozzles[2] = bomberNozzles;
    meshNozzles[3] = scoutNozzles;
    meshNozzles[MESH_CORVETTE] = corvetteNozzles;
    meshNozzles[MESH_DRONE] = droneNozzles;
    meshNozzleCount[1] = (uint32_t)(sizeof(interceptorNozzles) / sizeof(float) / 4);
    meshNozzleCount[2] = (uint32_t)(sizeof(bomberNozzles) / sizeof(float) / 4);
    meshNozzleCount[3] = (uint32_t)(sizeof(scoutNozzles) / sizeof(float) / 4);
    meshNozzleCount[MESH_CORVETTE] = (uint32_t)(sizeof(corvetteNozzles) / sizeof(float) / 4);
    meshNozzleCount[MESH_DRONE] = (uint32_t)(sizeof(droneNozzles) / sizeof(float) / 4);
    meshMounts[MESH_CORVETTE] = corvetteMounts;
    meshMountCount[MESH_CORVETTE] = (uint32_t)(sizeof(corvetteMounts) / sizeof(float) / 6);
    for (uint32_t mesh = 1; mesh < MESH_COUNT; mesh++) {
        meshBase[mesh] = meshBase[mesh - 1] + meshVertexCount[mesh - 1];
    }
    if (meshBase[MESH_COUNT - 1] + meshVertexCount[MESH_COUNT - 1] > sizeof(meshVertices) / sizeof(Vertex)) {
        logError("mesh buffer short by %u vertices\n",
                 (uint32_t)(meshBase[MESH_COUNT - 1] + meshVertexCount[MESH_COUNT - 1] -
                            sizeof(meshVertices) / sizeof(Vertex)));
        exit(1);
    }
    buildSphere(meshVertices);
    expandMesh(1, interceptorVertices, interceptorIndices);
    expandMesh(2, bomberVertices, bomberIndices);
    expandMesh(3, scoutVertices, scoutIndices);
    expandMesh(MESH_FLAME, flameVertices, flameIndices);
    expandMesh(MESH_BOMB, bombVertices, bombIndices);
    expandMesh(MESH_CORVETTE, corvetteVertices, corvetteIndices);
    expandMesh(MESH_TURRET, turretVertices, turretIndices);
    expandMesh(MESH_GUN, gunVertices, gunIndices);
    expandMesh(MESH_DRONE, droneVertices, droneIndices);
    expandMesh(MESH_STATION, stationVertices, stationIndices);
    expandMesh(MESH_ROCK, ferrociteVertices, ferrociteIndices);
    expandMesh(MESH_ROCK + 1, halcyteVertices, halcyteIndices);
    expandMesh(MESH_ROCK + 2, vantalumVertices, vantalumIndices);
    for (uint32_t variant = 0; variant < RAY_VARIANTS; variant++) {
        expandMesh(MESH_BEAM + variant, beamVertices[variant], beamIndices[variant]);
        expandMesh(MESH_PRINTER + variant, printerVertices[variant], printerIndices[variant]);
    }
}

static void quaternionLook(const float* forward, const float* reference, float* out) {
    float upward[3];
    float length = 0.0f;
    float along = forward[0] * reference[0] + forward[1] * reference[1] + forward[2] * reference[2];
    for (uint32_t i = 0; i < 3; i++) {
        upward[i] = reference[i] - forward[i] * along;
        length += upward[i] * upward[i];
    }
    if (length < 1e-6f) {
        float helper[3] = {0.0f, 1.0f, 0.0f};
        if (forward[1] > 0.9f || forward[1] < -0.9f) {
            helper[0] = 1.0f;
            helper[1] = 0.0f;
        }
        along = forward[0] * helper[0] + forward[1] * helper[1] + forward[2] * helper[2];
        length = 0.0f;
        for (uint32_t i = 0; i < 3; i++) {
            upward[i] = helper[i] - forward[i] * along;
            length += upward[i] * upward[i];
        }
    }
    length = sqrtf(length);
    for (uint32_t i = 0; i < 3; i++) {
        upward[i] /= length;
    }
    float side[3] = {
        forward[1] * upward[2] - forward[2] * upward[1],
        forward[2] * upward[0] - forward[0] * upward[2],
        forward[0] * upward[1] - forward[1] * upward[0]
    };
    float trace = forward[0] + upward[1] + side[2];
    if (trace > 0.0f) {
        float root = sqrtf(trace + 1.0f) * 2.0f;
        out[3] = 0.25f * root;
        out[0] = (upward[2] - side[1]) / root;
        out[1] = (side[0] - forward[2]) / root;
        out[2] = (forward[1] - upward[0]) / root;
    } else if (forward[0] > upward[1] && forward[0] > side[2]) {
        float root = sqrtf(1.0f + forward[0] - upward[1] - side[2]) * 2.0f;
        out[3] = (upward[2] - side[1]) / root;
        out[0] = 0.25f * root;
        out[1] = (upward[0] + forward[1]) / root;
        out[2] = (side[0] + forward[2]) / root;
    } else if (upward[1] > side[2]) {
        float root = sqrtf(1.0f + upward[1] - forward[0] - side[2]) * 2.0f;
        out[3] = (side[0] - forward[2]) / root;
        out[0] = (upward[0] + forward[1]) / root;
        out[1] = 0.25f * root;
        out[2] = (side[1] + upward[2]) / root;
    } else {
        float root = sqrtf(1.0f + side[2] - forward[0] - upward[1]) * 2.0f;
        out[3] = (forward[1] - upward[0]) / root;
        out[0] = (side[0] + forward[2]) / root;
        out[1] = (side[1] + upward[2]) / root;
        out[2] = 0.25f * root;
    }
}

static void turnVector(const float* rotation, const float* value, float* out) {
    float twice[3] = {
        2.0f * (rotation[1] * value[2] - rotation[2] * value[1]),
        2.0f * (rotation[2] * value[0] - rotation[0] * value[2]),
        2.0f * (rotation[0] * value[1] - rotation[1] * value[0])
    };
    out[0] = value[0] + rotation[3] * twice[0] + rotation[1] * twice[2] - rotation[2] * twice[1];
    out[1] = value[1] + rotation[3] * twice[1] + rotation[2] * twice[0] - rotation[0] * twice[2];
    out[2] = value[2] + rotation[3] * twice[2] + rotation[0] * twice[1] - rotation[1] * twice[0];
}

static uint32_t stationCrossSection(float height, Vertex* out, uint32_t capacity) {
    static float segments[SECTION_SEGMENTS][4];
    static float events[SECTION_SEGMENTS * 2];
    uint32_t segmentCount = 0;
    for (uint32_t i = 0; i + 2 < sizeof(stationIndices) / sizeof(uint32_t) && segmentCount < SECTION_SEGMENTS; i += 3) {
        const float* corner[3] = {
            stationVertices + (size_t)stationIndices[i] * 14,
            stationVertices + (size_t)stationIndices[i + 1] * 14,
            stationVertices + (size_t)stationIndices[i + 2] * 14
        };
        float crossing[2][2];
        uint32_t crossings = 0;
        for (uint32_t edge = 0; edge < 3 && crossings < 2; edge++) {
            float low = corner[edge][1];
            float high = corner[(edge + 1) % 3][1];
            if ((low < height) == (high < height) || low == high) {
                continue;
            }
            float along = (height - low) / (high - low);
            crossing[crossings][0] = corner[edge][0] + (corner[(edge + 1) % 3][0] - corner[edge][0]) * along;
            crossing[crossings][1] = corner[edge][2] + (corner[(edge + 1) % 3][2] - corner[edge][2]) * along;
            crossings++;
        }
        if (crossings != 2) {
            continue;
        }
        segments[segmentCount][0] = crossing[0][0];
        segments[segmentCount][1] = crossing[0][1];
        segments[segmentCount][2] = crossing[1][0];
        segments[segmentCount][3] = crossing[1][1];
        events[segmentCount * 2] = crossing[0][1];
        events[segmentCount * 2 + 1] = crossing[1][1];
        segmentCount++;
    }
    uint32_t eventCount = segmentCount * 2;
    for (uint32_t i = 1; i < eventCount; i++) {
        float value = events[i];
        uint32_t slot = i;
        while (slot && events[slot - 1] > value) {
            events[slot] = events[slot - 1];
            slot--;
        }
        events[slot] = value;
    }
    uint32_t written = 0;
    for (uint32_t band = 0; band + 1 < eventCount && written + 6 <= capacity; band++) {
        float lower = events[band];
        float upper = events[band + 1];
        float middle = (lower + upper) * 0.5f;
        if (upper - lower < 1e-7f) {
            continue;
        }
        float place[256];
        uint32_t owner[256];
        uint32_t edgeCount = 0;
        for (uint32_t i = 0; i < segmentCount && edgeCount < 256; i++) {
            float low = segments[i][1];
            float high = segments[i][3];
            if ((low < middle) == (high < middle) || low == high) {
                continue;
            }
            place[edgeCount] = segments[i][0] + (segments[i][2] - segments[i][0]) * (middle - low) / (high - low);
            owner[edgeCount] = i;
            edgeCount++;
        }
        for (uint32_t i = 1; i < edgeCount; i++) {
            float value = place[i];
            uint32_t which = owner[i];
            uint32_t slot = i;
            while (slot && place[slot - 1] > value) {
                place[slot] = place[slot - 1];
                owner[slot] = owner[slot - 1];
                slot--;
            }
            place[slot] = value;
            owner[slot] = which;
        }
        for (uint32_t i = 0; i + 1 < edgeCount && written + 6 <= capacity; i += 2) {
            float corner[2][2];
            for (uint32_t side = 0; side < 2; side++) {
                const float* segment = segments[owner[i + side]];
                corner[side][0] = segment[0] + (segment[2] - segment[0]) * (lower - segment[1]) / (segment[3] - segment[1]);
                corner[side][1] = segment[0] + (segment[2] - segment[0]) * (upper - segment[1]) / (segment[3] - segment[1]);
            }
            if (place[i + 1] - place[i] < 1e-6f) {
                continue;
            }
            float quad[4][3] = {
                {corner[0][0], 0.0f, lower}, {corner[1][0], 0.0f, lower},
                {corner[1][1], 0.0f, upper}, {corner[0][1], 0.0f, upper}
            };
            static const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
            int edge = i == 0 || i + 2 >= edgeCount;
            for (uint32_t corner4 = 0; corner4 < 6; corner4++) {
                Vertex* vertex = &out[written++];
                memcpy(vertex->position, quad[order[corner4]], sizeof(float) * 3);
                vertex->position[3] = 1.0f;
                vertex->normal[0] = 0.0f;
                vertex->normal[1] = 1.0f;
                vertex->normal[2] = 0.0f;
                vertex->normal[3] = 0.0f;
                vertex->material[0] = edge ? 0.88f : 0.58f;
                vertex->material[1] = edge ? 0.97f : 0.84f;
                vertex->material[2] = 1.0f;
                vertex->material[3] = edge ? 6.4f : 2.5f;
                vertex->physical[0] = 0.0f;
                vertex->physical[1] = 1.0f;
                vertex->physical[2] = 0.0f;
                vertex->physical[3] = 0.0f;
            }
        }
    }
    return written;
}

static void offerExhaust(float* points, float* ranges, uint32_t* count, const float* centre, float strength) {
    float reach = centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2];
    uint32_t slot = *count;
    if (slot == MAX_EXHAUSTS) {
        slot = 0;
        for (uint32_t other = 1; other < MAX_EXHAUSTS; other++) {
            if (ranges[other] > ranges[slot]) {
                slot = other;
            }
        }
        if (ranges[slot] <= reach) {
            return;
        }
    } else {
        (*count)++;
    }
    ranges[slot] = reach;
    points[slot * 4 + 0] = centre[0];
    points[slot * 4 + 1] = centre[1];
    points[slot * 4 + 2] = centre[2];
    points[slot * 4 + 3] = strength;
}

static uint32_t meshOfKind(uint32_t kind) {
    if (kind == KIND_ROUND) {
        return MESH_FLAME;
    }
    if (kind == KIND_BOMB) {
        return MESH_BOMB;
    }
    return kind >= KIND_STAR ? 0 : kindMesh[kind % KIND_COUNT];
}

static void appearanceOf(uint32_t kind, uint32_t owner, float* scale, uint32_t* material) {
    if (kind == KIND_STAR) {
        *scale = 6.96e8f;
        *material = MATERIAL_STAR;
    } else if (kind == KIND_PLANET_ROCK) {
        *scale = 6.4e6f;
        *material = MATERIAL_ROCK;
    } else if (kind == KIND_PLANET_ICE) {
        *scale = 3.2e6f;
        *material = MATERIAL_ICE;
    } else if (kind == KIND_ROUND) {
        *scale = ROUND_SCALE;
        *material = 0;
    } else if (kind == KIND_BOMB) {
        *scale = BOMB_SCALE;
        *material = 0;
    } else if (kind >= KIND_ROCK && kind < KIND_COUNT) {
        *scale = kindRadius[kind];
        *material = 0;
    } else {
        *scale = kindRadius[kind % KIND_COUNT];
        *material = owner ? owner : 0;
    }
}
static VkPhysicalDeviceMemoryProperties deviceMemory;



static uint32_t memoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < deviceMemory.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (deviceMemory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}
static void createBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer, VkDeviceMemory* memory) {
    VkBufferCreateInfo bufferCreateInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    vkCreateBuffer(device, &bufferCreateInfo, NULL, buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    VkMemoryAllocateFlagsInfo allocateFlagsInfo = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &allocateFlagsInfo, .allocationSize = requirements.size,
        .memoryTypeIndex = memoryTypeIndex(requirements.memoryTypeBits, properties)
    };
    vkAllocateMemory(device, &allocateInfo, NULL, memory);
    vkBindBufferMemory(device, *buffer, *memory, 0);
}
static void createImage(VkDevice device, VkExtent2D extent, VkFormat format, VkImage* image, VkDeviceMemory* memory, VkImageView* view) {
    VkImageCreateInfo imageCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {extent.width, extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    vkCreateImage(device, &imageCreateInfo, NULL, image);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, *image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = memoryTypeIndex(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    vkAllocateMemory(device, &allocateInfo, NULL, memory);
    vkBindImageMemory(device, *image, *memory, 0);
    VkImageViewCreateInfo viewCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    vkCreateImageView(device, &viewCreateInfo, NULL, view);
}
static VkDeviceAddress bufferAddress(VkDevice device, VkBuffer buffer) {
    VkBufferDeviceAddressInfo addressInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer};
    return vkGetBufferDeviceAddress(device, &addressInfo);
}
static void matrixMultiply(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            out[4 * c + r] = a[r] * b[4 * c] + a[4 + r] * b[4 * c + 1] + a[8 + r] * b[4 * c + 2] + a[12 + r] * b[4 * c + 3];
        }
    }
}
int main(int argc, char** argv) {
    typedef BOOL(WINAPI * DpiContextProc)(HANDLE);
    DpiContextProc setDpiContext = (DpiContextProc)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                                        "SetProcessDpiAwarenessContext");
    if (!setDpiContext || !setDpiContext((HANDLE)-4)) {
        SetProcessDPIAware();
    }
    settingsLoad();
    networkStart(argc > 1 ? argv[1] : "127.0.0.1");
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    uiScale = (float)screenHeight / 1080.0f;
    uiScale = uiScale < 1.0f ? 1.0f : (uiScale > 2.4f ? 2.4f : uiScale);
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    WNDCLASSEXW windowClass = {.cbSize = sizeof(windowClass), .lpfnWndProc = windowProc, .hInstance = hInstance, .lpszClassName = WINDOW_TITLE};
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, WINDOW_TITLE, WINDOW_TITLE, WS_POPUP, 0, 0, screenWidth, screenHeight, NULL, NULL, hInstance, NULL);
    ShowWindow(window, SW_SHOW);
    ShowCursor(FALSE);
    MSG msg;
    LARGE_INTEGER frequency;
    LARGE_INTEGER previousTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previousTime);
    buildMeshes();
    const char* extensions[] = {
        "VK_KHR_surface",
        "VK_KHR_win32_surface",
    };
    const char* deviceExtensions[] = {"VK_KHR_swapchain", "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_deferred_host_operations"};
    VkApplicationInfo appInfo = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4};
    VkInstanceCreateInfo instanceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo, .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions
    };
#ifdef SERENITAS_DEBUG
    const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
    instanceCreateInfo.enabledLayerCount = 1;
    instanceCreateInfo.ppEnabledLayerNames = validationLayers;
#endif
    VkInstance instance;
    vkCreateInstance(&instanceCreateInfo, NULL, &instance);
    uint32_t physicalDeviceCount = 8;
    VkPhysicalDevice physicalDevices[8];
    VkPhysicalDevice physicalDevice;
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices);
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        physicalDevice = physicalDevices[i];
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            break;
        }
    }
    VkSurfaceKHR surface;
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = hInstance, .hwnd = window};
    vkCreateWin32SurfaceKHR(instance, &surfaceCreateInfo, NULL, &surface);
    uint32_t queueFamilyIndex = 0;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, NULL);
    VkQueueFamilyProperties queueFamilies[queueFamilyCount];
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies);
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupported);
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && presentSupported) {
            queueFamilyIndex = i;
            break;
        }
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo deviceQueueCreateInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queueFamilyIndex, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .rayQuery = VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rayQueryFeatures, .accelerationStructure = VK_TRUE
    };
    VkPhysicalDeviceVulkan12Features vulkan12Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &accelerationStructureFeatures,
        .descriptorIndexing = VK_TRUE, .bufferDeviceAddress = VK_TRUE
    };
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &vulkan12Features, .dynamicRendering = VK_TRUE
    };
    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dynamicRendering,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &deviceQueueCreateInfo,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = deviceExtensions
    };
    VkDevice device;
    vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
    VkQueue queue;
    vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemory);

    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, NULL);
    VkSurfaceFormatKHR* formats = malloc(sizeof(VkSurfaceFormatKHR) * formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats);
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = formats[i];
            break;
        }
    }
    free(formats);
    VkFormat swapchainFormat = chosen.format;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkSwapchainCreateInfoKHR swapchainCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = capabilities.minImageCount + 1,
        .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace, .imageExtent = capabilities.currentExtent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = capabilities.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE
    };
    VkSwapchainKHR swapchain;
    vkCreateSwapchainKHR(device, &swapchainCreateInfo, NULL, &swapchain);
    uint32_t imageCount;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, NULL);
    VkImage swapchainImages[imageCount];
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages);
    VkSemaphoreCreateInfo semaphoreCreateInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore renderFinished[imageCount];
    VkImageView swapchainImageViews[imageCount];
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = swapchainImages[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = swapchainFormat,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        vkCreateImageView(device, &viewCreateInfo, NULL, &swapchainImageViews[i]);
        vkCreateSemaphore(device, &semaphoreCreateInfo, NULL, &renderFinished[i]);
    }
    VkImageCreateInfo depthImageCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = depthFormat,
        .extent = {capabilities.currentExtent.width, capabilities.currentExtent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage depthImage;
    vkCreateImage(device, &depthImageCreateInfo, NULL, &depthImage);
    VkMemoryRequirements depthRequirements;
    vkGetImageMemoryRequirements(device, depthImage, &depthRequirements);
    VkMemoryAllocateInfo depthAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = depthRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    VkDeviceMemory depthMemory;
    vkAllocateMemory(device, &depthAllocateInfo, NULL, &depthMemory);
    vkBindImageMemory(device, depthImage, depthMemory, 0);
    VkImageViewCreateInfo depthViewCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = depthImage, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = depthFormat,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}
    };
    VkImageView depthImageView;
    vkCreateImageView(device, &depthViewCreateInfo, NULL, &depthImageView);

    VkShaderModuleCreateInfo vertCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(sceneVert), .pCode = sceneVert};
    VkShaderModuleCreateInfo fragCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(sceneFrag), .pCode = sceneFrag};
    VkShaderModule vertModule;
    VkShaderModule fragModule;
    vkCreateShaderModule(device, &vertCreateInfo, NULL, &vertModule);
    vkCreateShaderModule(device, &fragCreateInfo, NULL, &fragModule);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertModule, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragModule, .pName = "main"}
    };
    VkVertexInputBindingDescription vertexBindings[2] = {
        {.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
        {.binding = 1, .stride = sizeof(float) * INSTANCE_FLOATS, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE}
    };
    VkVertexInputAttributeDescription vertexAttributes[7] = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = sizeof(float) * 4},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 8},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 12},
        {.location = 4, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
        {.location = 5, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 4},
        {.location = 6, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = sizeof(float) * 8}
    };
    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = vertexBindings,
        .vertexAttributeDescriptionCount = 7, .pVertexAttributeDescriptions = vertexAttributes
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0.0f, 0.0f, (float)capabilities.currentExtent.width, (float)capabilities.currentExtent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, capabilities.currentExtent};
    VkPipelineViewportStateCreateInfo viewportState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rasterizationState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisampleState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depthStencilState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_GREATER, .maxDepthBounds = 1.0f
    };
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo colorBlendState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &colorBlendAttachment};
    VkPushConstantRange rasterPushRange = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 24};
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &rasterPushRange};
    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, NULL, &pipelineLayout);
    VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapchainFormat, .depthAttachmentFormat = depthFormat
    };
    VkDynamicState sceneDynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo sceneDynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
        .pDynamicStates = sceneDynamicStates
    };
    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pipelineRenderingCreateInfo,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertexInputState, .pInputAssemblyState = &inputAssemblyState, .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState, .pMultisampleState = &multisampleState, .pDepthStencilState = &depthStencilState,
        .pColorBlendState = &colorBlendState, .pDynamicState = &sceneDynamicState, .layout = pipelineLayout
    };
    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, NULL, &pipeline);
    vkDestroyShaderModule(device, vertModule, NULL);
    vkDestroyShaderModule(device, fragModule, NULL);

    VkShaderModuleCreateInfo overlayVertCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(overlayVert), .pCode = overlayVert};
    VkShaderModuleCreateInfo overlayFragCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(overlayFrag), .pCode = overlayFrag};
    VkShaderModule overlayVertModule;
    VkShaderModule overlayFragModule;
    vkCreateShaderModule(device, &overlayVertCreateInfo, NULL, &overlayVertModule);
    vkCreateShaderModule(device, &overlayFragCreateInfo, NULL, &overlayFragModule);
    VkPipelineShaderStageCreateInfo overlayStages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = overlayVertModule, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = overlayFragModule, .pName = "main"}
    };
    VkPipelineVertexInputStateCreateInfo overlayVertexInput = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkDescriptorSetLayoutBinding atlasBinding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo atlasSetLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &atlasBinding
    };
    VkDescriptorSetLayout atlasSetLayout;
    vkCreateDescriptorSetLayout(device, &atlasSetLayoutCreateInfo, NULL, &atlasSetLayout);
    VkPushConstantRange overlayPushRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 32
    };
    VkPipelineLayoutCreateInfo overlayLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &atlasSetLayout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &overlayPushRange
    };
    VkPipelineLayout overlayPipelineLayout;
    vkCreatePipelineLayout(device, &overlayLayoutCreateInfo, NULL, &overlayPipelineLayout);
    VkPipelineRenderingCreateInfo overlayRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &swapchainFormat
    };
    VkPipelineColorBlendAttachmentState overlayBlendAttachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
    };
    VkPipelineColorBlendStateCreateInfo overlayBlendState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &overlayBlendAttachment
    };
    VkDynamicState overlayDynamicStates[1] = {VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo overlayDynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1,
        .pDynamicStates = overlayDynamicStates
    };
    VkGraphicsPipelineCreateInfo overlayPipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &overlayRenderingCreateInfo,
        .stageCount = 2, .pStages = overlayStages,
        .pVertexInputState = &overlayVertexInput, .pInputAssemblyState = &inputAssemblyState, .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState, .pMultisampleState = &multisampleState,
        .pColorBlendState = &overlayBlendState, .pDynamicState = &overlayDynamicState, .layout = overlayPipelineLayout
    };
    VkPipeline overlayPipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &overlayPipelineCreateInfo, NULL, &overlayPipeline);
    vkDestroyShaderModule(device, overlayVertModule, NULL);
    vkDestroyShaderModule(device, overlayFragModule, NULL);

    VkShaderModuleCreateInfo backgroundVertCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(backgroundVert), .pCode = backgroundVert};
    VkShaderModuleCreateInfo backgroundFragCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(backgroundFrag), .pCode = backgroundFrag};
    VkShaderModule backgroundVertModule;
    VkShaderModule backgroundFragModule;
    vkCreateShaderModule(device, &backgroundVertCreateInfo, NULL, &backgroundVertModule);
    vkCreateShaderModule(device, &backgroundFragCreateInfo, NULL, &backgroundFragModule);
    VkPipelineShaderStageCreateInfo backgroundStages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = backgroundVertModule, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = backgroundFragModule, .pName = "main"}
    };
    VkPushConstantRange backgroundPushRange = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 12};
    VkPipelineLayoutCreateInfo backgroundLayoutCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &backgroundPushRange};
    VkPipelineLayout backgroundPipelineLayout;
    vkCreatePipelineLayout(device, &backgroundLayoutCreateInfo, NULL, &backgroundPipelineLayout);
    VkPipelineDepthStencilStateCreateInfo backgroundDepthState = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
    VkGraphicsPipelineCreateInfo backgroundPipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pipelineRenderingCreateInfo,
        .stageCount = 2, .pStages = backgroundStages,
        .pVertexInputState = &overlayVertexInput, .pInputAssemblyState = &inputAssemblyState, .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState, .pMultisampleState = &multisampleState, .pDepthStencilState = &backgroundDepthState,
        .pColorBlendState = &colorBlendState, .layout = backgroundPipelineLayout
    };
    VkPipeline backgroundPipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &backgroundPipelineCreateInfo, NULL, &backgroundPipeline);
    vkDestroyShaderModule(device, backgroundVertModule, NULL);
    vkDestroyShaderModule(device, backgroundFragModule, NULL);

    VkCommandBuffer commandBuffers[FRAMES_IN_FLIGHT];
    VkCommandPoolCreateInfo commandPoolCreateInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = queueFamilyIndex};
    VkCommandPool commandPool;
    vkCreateCommandPool(device, &commandPoolCreateInfo, NULL, &commandPool);
    VkCommandBufferAllocateInfo commandBufferAllocateInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES_IN_FLIGHT};
    vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, commandBuffers);


    PFN_vkCreateAccelerationStructureKHR pfnCreateAccelerationStructure = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
    PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccelerationStructure = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR");
    PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetAccelerationStructureBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR");
    PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAccelerationStructures = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR");
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelerationStructureDeviceAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &accelerationStructureProperties};
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    VkDeviceSize scratchAlignment = accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexMemory;
    createBuffer(device, sizeof(meshVertices),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertexBuffer, &vertexMemory);
    Vertex* vertexScratch;
    vkMapMemory(device, vertexMemory, 0, sizeof(meshVertices), 0, (void**)&vertexScratch);
    memcpy(vertexScratch, meshVertices, sizeof(meshVertices));

    VkBuffer instanceBufferGraphics;
    VkDeviceMemory instanceMemoryGraphics;
    VkDeviceSize instanceBytes = sizeof(float) * INSTANCE_FLOATS * MAX_INSTANCES;
    createBuffer(device, instanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instanceBufferGraphics, &instanceMemoryGraphics);
    float* graphicsInstances;
    vkMapMemory(device, instanceMemoryGraphics, 0, instanceBytes, 0, (void**)&graphicsInstances);
    memset(graphicsInstances, 0, instanceBytes);

    VkBuffer exhaustBuffer[FRAMES_IN_FLIGHT];
    VkDeviceMemory exhaustMemory[FRAMES_IN_FLIGHT];
    float* exhaustLights[FRAMES_IN_FLIGHT];
    VkDeviceSize exhaustBytes = sizeof(float) * 4 * MAX_EXHAUSTS;
    VkBuffer factBuffer[FRAMES_IN_FLIGHT];
    VkDeviceMemory factMemory[FRAMES_IN_FLIGHT];
    float* instanceFacts[FRAMES_IN_FLIGHT];
    VkDeviceSize factBytes = sizeof(float) * INSTANCE_FACTS * MAX_INSTANCES;
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        createBuffer(device, exhaustBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &exhaustBuffer[i], &exhaustMemory[i]);
        vkMapMemory(device, exhaustMemory[i], 0, exhaustBytes, 0, (void**)&exhaustLights[i]);
        memset(exhaustLights[i], 0, exhaustBytes);
        createBuffer(device, factBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &factBuffer[i], &factMemory[i]);
        vkMapMemory(device, factMemory[i], 0, factBytes, 0, (void**)&instanceFacts[i]);
        memset(instanceFacts[i], 0, factBytes);
    }

    VkDeviceAddress vertexAddress = bufferAddress(device, vertexBuffer);
    VkAccelerationStructureGeometryKHR bottomGeometry[MESH_COUNT];
    VkAccelerationStructureBuildGeometryInfoKHR bottomBuildInfo[MESH_COUNT];
    VkAccelerationStructureBuildRangeInfoKHR bottomRange[MESH_COUNT];
    VkBuffer bottomBuffer[MESH_COUNT];
    VkDeviceMemory bottomMemory[MESH_COUNT];
    VkAccelerationStructureKHR bottomLevel[MESH_COUNT];
    VkDeviceSize bottomScratch = 0;
    for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
        uint32_t triangleCount = meshVertexCount[mesh] / 3;
        bottomGeometry[mesh] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry.triangles = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR, .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                .vertexData.deviceAddress = vertexAddress + (VkDeviceSize)meshBase[mesh] * sizeof(Vertex),
                .vertexStride = sizeof(Vertex), .maxVertex = meshVertexCount[mesh] - 1, .indexType = VK_INDEX_TYPE_NONE_KHR
            },
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR
        };
        bottomBuildInfo[mesh] = (VkAccelerationStructureBuildGeometryInfoKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = 1, .pGeometries = &bottomGeometry[mesh]
        };
        bottomGeometry[mesh].flags = mesh == MESH_STATION ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureBuildSizesInfoKHR bottomSizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        pfnGetAccelerationStructureBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bottomBuildInfo[mesh], &triangleCount, &bottomSizes);
        createBuffer(device, bottomSizes.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &bottomBuffer[mesh], &bottomMemory[mesh]);
        VkAccelerationStructureCreateInfoKHR bottomCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = bottomBuffer[mesh],
            .size = bottomSizes.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
        };
        pfnCreateAccelerationStructure(device, &bottomCreateInfo, NULL, &bottomLevel[mesh]);
        bottomRange[mesh] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = triangleCount};
        if (bottomSizes.buildScratchSize > bottomScratch) {
            bottomScratch = bottomSizes.buildScratchSize;
        }
    }

    uint32_t maxInstances = MAX_INSTANCES;
    VkBuffer instanceBuffer[FRAMES_IN_FLIGHT];
    VkDeviceMemory instanceMemory[FRAMES_IN_FLIGHT];
    VkAccelerationStructureInstanceKHR* instances[FRAMES_IN_FLIGHT];
    VkDeviceAddress instanceAddress[FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        createBuffer(device, sizeof(VkAccelerationStructureInstanceKHR) * maxInstances,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &instanceBuffer[i], &instanceMemory[i]);
        vkMapMemory(device, instanceMemory[i], 0, sizeof(VkAccelerationStructureInstanceKHR) * maxInstances, 0, (void**)&instances[i]);
        memset(instances[i], 0, sizeof(VkAccelerationStructureInstanceKHR) * maxInstances);
        instanceAddress[i] = bufferAddress(device, instanceBuffer[i]);
    }
    uint64_t meshReference[MESH_COUNT];
    for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
        VkAccelerationStructureDeviceAddressInfoKHR bottomAddressInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = bottomLevel[mesh]
        };
        meshReference[mesh] = pfnGetAccelerationStructureDeviceAddress(device, &bottomAddressInfo);
    }

    VkAccelerationStructureGeometryKHR topGeometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR, .arrayOfPointers = VK_FALSE,
            .data.deviceAddress = instanceAddress[0]
        },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR
    };
    VkAccelerationStructureBuildGeometryInfoKHR topBuildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1, .pGeometries = &topGeometry
    };
    VkAccelerationStructureBuildSizesInfoKHR topSizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    pfnGetAccelerationStructureBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topBuildInfo, &maxInstances, &topSizes);
    VkBuffer topBuffer[FRAMES_IN_FLIGHT];
    VkDeviceMemory topMemory[FRAMES_IN_FLIGHT];
    VkAccelerationStructureKHR topLevel[FRAMES_IN_FLIGHT];
    VkBuffer topScratchBuffer[FRAMES_IN_FLIGHT];
    VkDeviceMemory topScratchMemory[FRAMES_IN_FLIGHT];
    VkDeviceAddress topScratchAddress[FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        createBuffer(device, topSizes.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &topBuffer[i], &topMemory[i]);
        VkAccelerationStructureCreateInfoKHR topCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, .buffer = topBuffer[i],
            .size = topSizes.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        };
        pfnCreateAccelerationStructure(device, &topCreateInfo, NULL, &topLevel[i]);
        createBuffer(device, topSizes.buildScratchSize + scratchAlignment,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &topScratchBuffer[i], &topScratchMemory[i]);
        topScratchAddress[i] = (bufferAddress(device, topScratchBuffer[i]) + scratchAlignment - 1) & ~(scratchAlignment - 1);
    }

    VkBuffer scratchBuffer;
    VkDeviceMemory scratchMemory;
    createBuffer(device, bottomScratch + scratchAlignment,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scratchBuffer, &scratchMemory);
    VkDeviceAddress scratchAddress = (bufferAddress(device, scratchBuffer) + scratchAlignment - 1) & ~(scratchAlignment - 1);

    VkCommandBufferBeginInfo buildBeginInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkMemoryBarrier buildBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
    };
    vkBeginCommandBuffer(commandBuffers[0], &buildBeginInfo);
    for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
        bottomBuildInfo[mesh].dstAccelerationStructure = bottomLevel[mesh];
        bottomBuildInfo[mesh].scratchData.deviceAddress = scratchAddress;
        const VkAccelerationStructureBuildRangeInfoKHR* bottomRanges = &bottomRange[mesh];
        pfnCmdBuildAccelerationStructures(commandBuffers[0], 1, &bottomBuildInfo[mesh], &bottomRanges);
        vkCmdPipelineBarrier(commandBuffers[0], VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &buildBarrier, 0, NULL, 0, NULL);
    }
    vkEndCommandBuffer(commandBuffers[0]);
    VkSubmitInfo buildSubmitInfo = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commandBuffers[0]};
    vkQueueSubmit(queue, 1, &buildSubmitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    VkImage images[IMAGE_COUNT];
    VkDeviceMemory imageMemories[IMAGE_COUNT];
    VkImageView imageViews[IMAGE_COUNT];
    for (uint32_t i = 0; i < IMAGE_COUNT; i++) {
        createImage(device, capabilities.currentExtent, imageFormats[i], &images[i], &imageMemories[i], &imageViews[i]);
    }
    VkImageMemoryBarrier initialBarriers[IMAGE_COUNT];
    for (uint32_t i = 0; i < IMAGE_COUNT; i++) {
        initialBarriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
    }
    vkResetCommandBuffer(commandBuffers[0], 0);
    vkBeginCommandBuffer(commandBuffers[0], &buildBeginInfo);
    vkCmdPipelineBarrier(commandBuffers[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, IMAGE_COUNT, initialBarriers);
    VkClearColorValue clearColor = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange clearRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < IMAGE_COUNT; i++) {
        vkCmdClearColorImage(commandBuffers[0], images[i], VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
    }
    vkEndCommandBuffer(commandBuffers[0]);
    vkQueueSubmit(queue, 1, &buildSubmitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    buildFontAtlas();
    /* Icons are drawn by the engine itself: every hull is rendered once through
       the scene pipeline at twice the icon size and boxed down, so the command
       deck shows the same plating and livery the ship wears in flight. */
    VkImage iconTarget;
    VkDeviceMemory iconTargetMemory;
    VkImageView iconTargetView;
    VkImageCreateInfo iconTargetCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = swapchainFormat,
        .extent = {ICON_RENDER, ICON_RENDER, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    vkCreateImage(device, &iconTargetCreateInfo, NULL, &iconTarget);
    VkMemoryRequirements iconTargetRequirements;
    vkGetImageMemoryRequirements(device, iconTarget, &iconTargetRequirements);
    VkMemoryAllocateInfo iconTargetAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = iconTargetRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(iconTargetRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    vkAllocateMemory(device, &iconTargetAllocateInfo, NULL, &iconTargetMemory);
    vkBindImageMemory(device, iconTarget, iconTargetMemory, 0);
    VkImageViewCreateInfo iconTargetViewCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = iconTarget, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchainFormat, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    vkCreateImageView(device, &iconTargetViewCreateInfo, NULL, &iconTargetView);
    VkImage iconDepth;
    VkDeviceMemory iconDepthMemory;
    VkImageView iconDepthView;
    VkImageCreateInfo iconDepthCreateInfo = iconTargetCreateInfo;
    iconDepthCreateInfo.format = depthFormat;
    iconDepthCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vkCreateImage(device, &iconDepthCreateInfo, NULL, &iconDepth);
    VkMemoryRequirements iconDepthRequirements;
    vkGetImageMemoryRequirements(device, iconDepth, &iconDepthRequirements);
    VkMemoryAllocateInfo iconDepthAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = iconDepthRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(iconDepthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    vkAllocateMemory(device, &iconDepthAllocateInfo, NULL, &iconDepthMemory);
    vkBindImageMemory(device, iconDepth, iconDepthMemory, 0);
    VkImageViewCreateInfo iconDepthViewCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = iconDepth, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = depthFormat, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}
    };
    vkCreateImageView(device, &iconDepthViewCreateInfo, NULL, &iconDepthView);
    VkDeviceSize iconSheetBytes = (VkDeviceSize)ICON_RENDER * ICON_RENDER * 4 * ICON_COUNT;
    VkBuffer iconReadback;
    VkDeviceMemory iconReadbackMemory;
    createBuffer(device, iconSheetBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &iconReadback, &iconReadbackMemory);
    static const uint32_t iconKinds[ICON_COUNT] = {
        KIND_INTERCEPTOR, KIND_SCOUT, KIND_BOMBER, KIND_CORVETTE, KIND_DRONE, KIND_STATION
    };
    float iconPush[ICON_COUNT][24];
    for (uint32_t i = 0; i < ICON_COUNT; i++) {
        uint32_t kind = iconKinds[i];
        uint32_t mesh = meshOfKind(kind);
        float hullScale;
        uint32_t hullMaterial;
        appearanceOf(kind, 3, &hullScale, &hullMaterial);
        float lowest[3] = {1.0e30f, 1.0e30f, 1.0e30f};
        float highest[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
        for (uint32_t slot = 0; slot < meshVertexCount[mesh]; slot++) {
            const float* point = meshVertices[meshBase[mesh] + slot].position;
            for (uint32_t axis = 0; axis < 3; axis++) {
                lowest[axis] = point[axis] < lowest[axis] ? point[axis] : lowest[axis];
                highest[axis] = point[axis] > highest[axis] ? point[axis] : highest[axis];
            }
        }
        float middle[3];
        float reach = 0.0f;
        for (uint32_t axis = 0; axis < 3; axis++) {
            middle[axis] = (lowest[axis] + highest[axis]) * 0.5f;
            float half = (highest[axis] - lowest[axis]) * 0.5f;
            reach += half * half;
        }
        reach = sqrtf(reach) * hullScale;
        float eye[3] = {0.58f, 0.42f, 0.70f};
        normaliseVector(eye);
        float iconForward[3] = {-eye[0], -eye[1], -eye[2]};
        float worldUp[3] = {0.0f, 1.0f, 0.0f};
        float iconRight[3];
        crossVector(worldUp, iconForward, iconRight);
        normaliseVector(iconRight);
        float iconUp[3];
        crossVector(iconForward, iconRight, iconUp);
        float halfAngle = 0.40f;
        float distance = reach / tanf(halfAngle) * 1.06f;
        float centre[3] = {iconForward[0] * distance, iconForward[1] * distance, iconForward[2] * distance};
        float* entry = graphicsInstances + (size_t)i * INSTANCE_FLOATS;
        for (uint32_t axis = 0; axis < 3; axis++) {
            entry[axis] = centre[axis] - middle[axis] * hullScale;
        }
        entry[3] = hullScale;
        entry[4] = 0.0f;
        entry[5] = 0.0f;
        entry[6] = 0.0f;
        entry[7] = 1.0f;
        entry[8] = (float)hullMaterial;
        entry[9] = NO_CEILING;
        entry[10] = 1.0f;
        entry[11] = 0.0f;
        float glance[3] = {-0.46f, 0.66f, -0.60f};
        float lightAxis[3];
        for (uint32_t axis = 0; axis < 3; axis++) {
            lightAxis[axis] = iconRight[axis] * glance[0] + iconUp[axis] * glance[1] + iconForward[axis] * glance[2];
        }
        normaliseVector(lightAxis);
        float view[16] = {
            iconRight[0], iconUp[0], iconForward[0], 0.0f,
            iconRight[1], iconUp[1], iconForward[1], 0.0f,
            iconRight[2], iconUp[2], iconForward[2], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        float focal = 1.0f / tanf(halfAngle);
        float projection[16] = {
            focal, 0.0f, 0.0f, 0.0f,
            0.0f, -focal, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, reach * 0.05f, 0.0f
        };
        matrixMultiply(projection, view, iconPush[i]);
        for (uint32_t axis = 0; axis < 3; axis++) {
            iconPush[i][16 + axis] = centre[axis] + lightAxis[axis] * 1.4959787e11f;
        }
        iconPush[i][19] = 6.96e8f;
        iconPush[i][20] = 36000.0f;
        iconPush[i][21] = 28000.0f;
        iconPush[i][22] = 38000.0f;
        iconPush[i][23] = EXPOSURE;
    }
    VkCommandBuffer iconCommand;
    VkCommandBufferAllocateInfo iconCommandAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1
    };
    vkAllocateCommandBuffers(device, &iconCommandAllocateInfo, &iconCommand);
    VkCommandBufferBeginInfo iconBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(iconCommand, &iconBeginInfo);
    VkViewport iconViewport = {0.0f, 0.0f, (float)ICON_RENDER, (float)ICON_RENDER, 0.0f, 1.0f};
    VkRect2D iconScissor = {{0, 0}, {ICON_RENDER, ICON_RENDER}};
    for (uint32_t i = 0; i < ICON_COUNT; i++) {
        VkImageMemoryBarrier iconToDraw[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = iconTarget, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = iconDepth, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}
            }
        };
        vkCmdPipelineBarrier(iconCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 0, NULL, 0, NULL, 2, iconToDraw);
        VkRenderingAttachmentInfo iconColourAttachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iconTargetView,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}
        };
        VkRenderingAttachmentInfo iconDepthAttachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iconDepthView,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {0.0f, 0}}
        };
        VkRenderingInfo iconRendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = iconScissor, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &iconColourAttachment,
            .pDepthAttachment = &iconDepthAttachment
        };
        vkCmdBeginRendering(iconCommand, &iconRendering);
        vkCmdBindPipeline(iconCommand, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(iconCommand, 0, 1, &iconViewport);
        vkCmdSetScissor(iconCommand, 0, 1, &iconScissor);
        vkCmdPushConstants(iconCommand, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(iconPush[i]), iconPush[i]);
        VkBuffer iconBuffers[2] = {vertexBuffer, instanceBufferGraphics};
        VkDeviceSize iconOffsets[2] = {0, 0};
        vkCmdBindVertexBuffers(iconCommand, 0, 2, iconBuffers, iconOffsets);
        uint32_t iconMesh = meshOfKind(iconKinds[i]);
        vkCmdDraw(iconCommand, meshVertexCount[iconMesh], 1, meshBase[iconMesh], i);
        vkCmdEndRendering(iconCommand);
        VkImageMemoryBarrier iconToRead = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = iconTarget, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        vkCmdPipelineBarrier(iconCommand, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &iconToRead);
        VkBufferImageCopy iconRegion = {
            .bufferOffset = (VkDeviceSize)i * ICON_RENDER * ICON_RENDER * 4,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {ICON_RENDER, ICON_RENDER, 1}
        };
        vkCmdCopyImageToBuffer(iconCommand, iconTarget, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, iconReadback, 1, &iconRegion);
    }
    vkEndCommandBuffer(iconCommand);
    VkSubmitInfo iconSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &iconCommand};
    vkQueueSubmit(queue, 1, &iconSubmit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, commandPool, 1, &iconCommand);
    const unsigned char* iconSheet;
    vkMapMemory(device, iconReadbackMemory, 0, iconSheetBytes, 0, (void**)&iconSheet);
    uint32_t iconRow = ATLAS_WIDTH / (ICON_SIZE + 2);
    for (uint32_t i = 0; i < ICON_COUNT; i++) {
        uint32_t originX = (i % iconRow) * (ICON_SIZE + 2);
        uint32_t originY = atlasCursor + (i / iconRow) * (ICON_SIZE + 2);
        if (originX + ICON_SIZE > ATLAS_WIDTH || originY + ICON_SIZE > ATLAS_HEIGHT) {
            logError("atlas: no room for icon %u\n", i);
            break;
        }
        const unsigned char* sheet = iconSheet + (size_t)i * ICON_RENDER * ICON_RENDER * 4;
        for (uint32_t y = 0; y < ICON_SIZE; y++) {
            for (uint32_t x = 0; x < ICON_SIZE; x++) {
                float gathered[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (uint32_t sample = 0; sample < 4; sample++) {
                    const unsigned char* texel = sheet +
                        (((size_t)(y * 2 + sample / 2) * ICON_RENDER) + x * 2 + sample % 2) * 4;
                    float cover = (float)texel[3] / 255.0f;
                    gathered[0] += toLinear((float)texel[2] / 255.0f) * cover;
                    gathered[1] += toLinear((float)texel[1] / 255.0f) * cover;
                    gathered[2] += toLinear((float)texel[0] / 255.0f) * cover;
                    gathered[3] += cover;
                }
                unsigned char* target = &atlasPixels[((size_t)(originY + y) * ATLAS_WIDTH + originX + x) * 4];
                float cover = gathered[3] * 0.25f;
                for (uint32_t channel = 0; channel < 3; channel++) {
                    float value = gathered[3] > 0.0f ? gathered[channel] / gathered[3] : 0.0f;
                    value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                    target[channel] = (unsigned char)(value * 255.0f + 0.5f);
                }
                target[3] = (unsigned char)(cover * 255.0f + 0.5f);
            }
        }
        float* patch = iconPatch[iconKinds[i]];
        patch[0] = (float)originX / (float)ATLAS_WIDTH;
        patch[1] = (float)originY / (float)ATLAS_HEIGHT;
        patch[2] = (float)(originX + ICON_SIZE) / (float)ATLAS_WIDTH;
        patch[3] = (float)(originY + ICON_SIZE) / (float)ATLAS_HEIGHT;
        iconReady[iconKinds[i]] = 1;
    }
    vkUnmapMemory(device, iconReadbackMemory);
    vkDestroyBuffer(device, iconReadback, NULL);
    vkFreeMemory(device, iconReadbackMemory, NULL);
    vkDestroyImageView(device, iconTargetView, NULL);
    vkDestroyImage(device, iconTarget, NULL);
    vkFreeMemory(device, iconTargetMemory, NULL);
    vkDestroyImageView(device, iconDepthView, NULL);
    vkDestroyImage(device, iconDepth, NULL);
    vkFreeMemory(device, iconDepthMemory, NULL);
    VkImage atlasImage;
    VkDeviceMemory atlasMemory;
    VkImageView atlasView;
    VkImageCreateInfo atlasCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {ATLAS_WIDTH, ATLAS_HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    vkCreateImage(device, &atlasCreateInfo, NULL, &atlasImage);
    VkMemoryRequirements atlasRequirements;
    vkGetImageMemoryRequirements(device, atlasImage, &atlasRequirements);
    VkMemoryAllocateInfo atlasAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = atlasRequirements.size,
        .memoryTypeIndex = memoryTypeIndex(atlasRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    vkAllocateMemory(device, &atlasAllocateInfo, NULL, &atlasMemory);
    vkBindImageMemory(device, atlasImage, atlasMemory, 0);
    VkImageViewCreateInfo atlasViewCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = atlasImage, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    vkCreateImageView(device, &atlasViewCreateInfo, NULL, &atlasView);
    VkBuffer atlasStaging;
    VkDeviceMemory atlasStagingMemory;
    createBuffer(device, sizeof(atlasPixels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &atlasStaging, &atlasStagingMemory);
    void* atlasScratch;
    vkMapMemory(device, atlasStagingMemory, 0, sizeof(atlasPixels), 0, &atlasScratch);
    memcpy(atlasScratch, atlasPixels, sizeof(atlasPixels));
    vkUnmapMemory(device, atlasStagingMemory);
    VkCommandBuffer upload;
    VkCommandBufferAllocateInfo uploadAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1
    };
    vkAllocateCommandBuffers(device, &uploadAllocateInfo, &upload);
    VkCommandBufferBeginInfo uploadBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(upload, &uploadBeginInfo);
    VkImageMemoryBarrier atlasToWrite = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = atlasImage, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &atlasToWrite);
    VkBufferImageCopy atlasCopy = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {ATLAS_WIDTH, ATLAS_HEIGHT, 1}
    };
    vkCmdCopyBufferToImage(upload, atlasStaging, atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &atlasCopy);
    VkImageMemoryBarrier atlasToRead = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = atlasImage, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &atlasToRead);
    vkEndCommandBuffer(upload);
    VkSubmitInfo uploadSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &upload};
    vkQueueSubmit(queue, 1, &uploadSubmit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, commandPool, 1, &upload);
    vkDestroyBuffer(device, atlasStaging, NULL);
    vkFreeMemory(device, atlasStagingMemory, NULL);
    VkSamplerCreateInfo atlasSamplerCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    VkSampler atlasSampler;
    vkCreateSampler(device, &atlasSamplerCreateInfo, NULL, &atlasSampler);
    VkDescriptorPoolSize atlasPoolSize = {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo atlasPoolCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &atlasPoolSize
    };
    VkDescriptorPool atlasPool;
    vkCreateDescriptorPool(device, &atlasPoolCreateInfo, NULL, &atlasPool);
    VkDescriptorSetAllocateInfo atlasSetAllocateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = atlasPool,
        .descriptorSetCount = 1, .pSetLayouts = &atlasSetLayout
    };
    VkDescriptorSet atlasSet;
    vkAllocateDescriptorSets(device, &atlasSetAllocateInfo, &atlasSet);
    VkDescriptorImageInfo atlasImageInfo = {
        .sampler = atlasSampler, .imageView = atlasView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    VkWriteDescriptorSet atlasWrite = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = atlasSet, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &atlasImageInfo
    };
    vkUpdateDescriptorSets(device, 1, &atlasWrite, 0, NULL);

    VkDescriptorSetLayoutBinding bindings[BINDING_COUNT];
    for (uint32_t i = 0; i < BINDING_COUNT; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
    }
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = BINDING_COUNT, .pBindings = bindings};
    VkDescriptorSetLayout descriptorSetLayout;
    vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, NULL, &descriptorSetLayout);
    VkDescriptorPoolSize poolSizes[3] = {
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 8},
        {.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, .descriptorCount = 2},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 6}
    };
    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = poolSizes};
    VkDescriptorPool descriptorPool;
    vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, NULL, &descriptorPool);
    VkDescriptorSetLayout setLayouts[2] = {descriptorSetLayout, descriptorSetLayout};
    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = descriptorPool, .descriptorSetCount = 2, .pSetLayouts = setLayouts};
    VkDescriptorSet descriptorSets[2];
    vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, descriptorSets);
    VkDescriptorBufferInfo vertexBufferInfo = {.buffer = vertexBuffer, .offset = 0, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSetAccelerationStructureKHR accelerationStructureWrites[2];
    for (uint32_t s = 0; s < 2; s++) {
        accelerationStructureWrites[s] = (VkWriteDescriptorSetAccelerationStructureKHR){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, .accelerationStructureCount = 1, .pAccelerationStructures = &topLevel[s]
        };
    }
    for (uint32_t s = 0; s < 2; s++) {
        static const uint32_t mapping[BINDING_COUNT] = {
            IMAGE_ILLUMINATION, 0, 0, IMAGE_ALBEDO, IMAGE_FOG, 0, 0, IMAGE_OUTPUT
        };
        VkDescriptorImageInfo imageInfos[BINDING_COUNT];
        VkDescriptorBufferInfo exhaustBufferInfo = {.buffer = exhaustBuffer[s], .offset = 0, .range = VK_WHOLE_SIZE};
        VkDescriptorBufferInfo factBufferInfo = {.buffer = factBuffer[s], .offset = 0, .range = VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[BINDING_COUNT];
        for (uint32_t i = 0; i < BINDING_COUNT; i++) {
            imageInfos[i] = (VkDescriptorImageInfo){.imageView = imageViews[mapping[i]], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSets[s], .dstBinding = i,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &imageInfos[i]
            };
        }
        writes[1].pNext = &accelerationStructureWrites[s];
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[1].pImageInfo = NULL;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pImageInfo = NULL;
        writes[2].pBufferInfo = &vertexBufferInfo;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].pImageInfo = NULL;
        writes[5].pBufferInfo = &exhaustBufferInfo;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pImageInfo = NULL;
        writes[6].pBufferInfo = &factBufferInfo;
        vkUpdateDescriptorSets(device, BINDING_COUNT, writes, 0, NULL);
    }

    VkPushConstantRange computePushRange = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(float) * 32};
    VkPipelineLayoutCreateInfo computeLayoutCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &descriptorSetLayout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &computePushRange
    };
    VkPipelineLayout computePipelineLayout;
    vkCreatePipelineLayout(device, &computeLayoutCreateInfo, NULL, &computePipelineLayout);
    const uint32_t* computeCode[2] = {pathtraceComp, resolveComp};
    size_t computeSizes[2] = {sizeof(pathtraceComp), sizeof(resolveComp)};
    VkPipeline computePipelines[2];
    for (uint32_t i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo compCreateInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = computeSizes[i], .pCode = computeCode[i]};
        VkShaderModule compModule;
        vkCreateShaderModule(device, &compCreateInfo, NULL, &compModule);
        VkComputePipelineCreateInfo computePipelineCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compModule, .pName = "main"},
            .layout = computePipelineLayout
        };
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, NULL, &computePipelines[i]);
        vkDestroyShaderModule(device, compModule, NULL);
    }

    VkFenceCreateInfo fenceCreateInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence fences[FRAMES_IN_FLIGHT];
    VkSemaphore imageAvailable[FRAMES_IN_FLIGHT];
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkCreateFence(device, &fenceCreateInfo, NULL, &fences[i]);
        vkCreateSemaphore(device, &semaphoreCreateInfo, NULL, &imageAvailable[i]);
    }
    double cameraPosition[3] = {0.80 * ASTRONOMICAL_UNIT + 23300.0, 900.0, 0.31 * ASTRONOMICAL_UNIT - 31000.0};
    float cameraYaw = -1.941f;
    float cameraPitch = 0.0f;
    float aspect = (float)capabilities.currentExtent.width / (float)capabilities.currentExtent.height;
    float tanHalfFovY = tanf(0.5f * 1.0472f);
    uint32_t frameIndex = 0;
    uint32_t frameCounter = 0;
    int previousLeft = 0;
    int previousRight = 0;
    int selecting = 0;
    int orderHeightStage = 0;
    int pointerCaptured = 0;
    int cursorShown = 0;
    int buildPending = 0;
    float wallSeconds = 0.0f;
    int previousKeys[ACTION_COUNT] = {0};
    HudButton hudButtons[HUD_BUTTON_CAP];
    uint32_t hudButtonCount = 0;
    MenuItem menuItems[MENU_ITEM_CAP];
    uint32_t menuItemCount = 0;
    float hudMotion[ACTION_COUNT] = {0.0f};
    float menuMotion[MENU_ITEM_CAP] = {0.0f};
    float hudReveal = 0.0f;
    float menuReveal = 0.0f;
    float selectionEase = 1.0f;
    float tabSlide = 0.0f;
    float menuScroll = 0.0f;
    uint32_t menuTab = MENU_TAB_GRAPHICS;
    float frameSmooth = 1.0f / 60.0f;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    char deviceLabel[64];
    snprintf(deviceLabel, sizeof(deviceLabel), "%s", properties2.properties.deviceName);
    float selectionStart[3] = {0.0f, 0.0f, 1.0f};
    float selectionEnd[3] = {0.0f, 0.0f, 1.0f};
    double orderTarget[3] = {0.0, 0.0, 0.0};
    uint32_t instanceCount = 0;
    uint32_t sectionSlots = 0;
    uint32_t sectionDrawn[SECTION_MESHES] = {0};
    uint32_t exhaustCount = 0;
    float exhaustPoints[MAX_EXHAUSTS * 4];
    float exhaustRange[MAX_EXHAUSTS];
    static uint32_t instanceTints[MAX_INSTANCES];
    static uint32_t instanceMeshes[MAX_INSTANCES];
    static float instanceRotations[MAX_INSTANCES * 4];
    static float instanceScales[MAX_INSTANCES];
    static float instanceCeilings[MAX_INSTANCES];
    static float instanceGlow[MAX_INSTANCES];
    static float instanceFlow[MAX_INSTANCES];
    static float instancePositions[MAX_INSTANCES * 3];
    uint32_t meshFirstInstance[MESH_COUNT];
    uint32_t meshInstanceCount[MESH_COUNT];
    uint32_t meshCursor[MESH_COUNT];
    float starCentre[3] = {0.0f, 0.0f, 0.0f};
    float starRadius = 6.96e8f;
    short running = 1;
    QueryPerformanceCounter(&previousTime);
    while (running) {
        float delta = 0.0f;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        delta = (float)(currentTime.QuadPart - previousTime.QuadPart) / (float)frequency.QuadPart;
        previousTime = currentTime;
        wallSeconds += delta;
        frameSmooth = approach(frameSmooth, delta, 4.0f, delta);
        int windowActive = GetActiveWindow() == window;
        int hudVisible = windowActive && !menuOpen && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        int pointerFree = windowActive && (menuOpen || hudVisible);
        if (pointerFree != cursorShown) {
            ShowCursor(pointerFree ? TRUE : FALSE);
            cursorShown = pointerFree;
        }
        if (pointerFree) {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            pointerX = (float)cursor.x;
            pointerY = (float)cursor.y;
        }
        if (windowActive && !pointerFree) {
            POINT cursor;
            GetCursorPos(&cursor);
            if (pointerCaptured) {
                cameraYaw += (float)(cursor.x - screenWidth / 2) * 0.0022f;
                cameraPitch -= (float)(cursor.y - screenHeight / 2) * 0.0022f;
            }
            SetCursorPos(screenWidth / 2, screenHeight / 2);
            pointerCaptured = 1;
        } else {
            pointerCaptured = 0;
        }
        if (windowActive && !menuOpen) {
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
                cameraYaw -= 1.5f * delta;
            }
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                cameraYaw += 1.5f * delta;
            }
            if (GetAsyncKeyState(VK_UP) & 0x8000) {
                cameraPitch += 1.5f * delta;
            }
            if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
                cameraPitch -= 1.5f * delta;
            }
        }
        if (cameraPitch > 1.5f) {
            cameraPitch = 1.5f;
        }
        if (cameraPitch < -1.5f) {
            cameraPitch = -1.5f;
        }
        float forward[3] = {cosf(cameraPitch) * sinf(cameraYaw), sinf(cameraPitch), cosf(cameraPitch) * cosf(cameraYaw)};
        float right[3] = {cosf(cameraYaw), 0.0f, -sinf(cameraYaw)};
        float up[3] = {
            forward[1] * right[2] - forward[2] * right[1],
            forward[2] * right[0] - forward[0] * right[2],
            forward[0] * right[1] - forward[1] * right[0]
        };
        float desiredVelocity[3] = {0.0f, 0.0f, 0.0f};
        if (windowActive && !menuOpen) {
            float alongForward = (float)(((GetAsyncKeyState('W') & 0x8000) != 0) - ((GetAsyncKeyState('S') & 0x8000) != 0));
            float alongRight = (float)(((GetAsyncKeyState('D') & 0x8000) != 0) - ((GetAsyncKeyState('A') & 0x8000) != 0));
            for (uint32_t i = 0; i < 3; i++) {
                desiredVelocity[i] = forward[i] * alongForward + right[i] * alongRight;
            }
        }
        for (uint32_t i = 0; i < 3; i++) {
            cameraPosition[i] += (double)desiredVelocity[i] * CAMERA_SPEED * (double)delta;
        }
        networkUpdate(delta);

        int leftDown = windowActive && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        int rightDown = windowActive && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        int leftPressed = leftDown && !previousLeft;
        int leftReleased = !leftDown && previousLeft;
        int rightPressed = rightDown && !previousRight;
        previousLeft = leftDown;
        previousRight = rightDown;
        int pointerPressed = leftPressed && pointerFree;
        if (pointerFree) {
            selecting = 0;
            leftPressed = 0;
            leftReleased = 0;
            rightPressed = 0;
        }

        if (leftPressed) {
            selecting = 1;
            memcpy(selectionStart, forward, sizeof(selectionStart));
        }
        if (selecting) {
            memcpy(selectionEnd, forward, sizeof(selectionEnd));
        }
        if (leftReleased && selecting) {
            selecting = 0;
            float axisR[3];
            float axisU[3];
            float axisF[3];
            float azimuth[2];
            float elevation[2];
            selectionBounds(selectionStart, selectionEnd, forward, axisR, axisU, axisF, azimuth, elevation);
            selectionPad(azimuth, elevation);
            selectedCount = 0;
            for (uint32_t i = 0; i < networkEntityCount && selectedCount < MAX_NETWORK_ENTITIES; i++) {
                if (networkEntities[i].owner != (uint32_t)networkPlayer || networkEntities[i].kind >= KIND_STAR) {
                    continue;
                }
                float direction[3];
                relativeTo(networkEntities[i].position, cameraPosition, direction);
                if (direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2] <= 0.0f) {
                    continue;
                }
                float angles[2];
                sphereAngles(direction, axisR, axisU, axisF, angles);
                if (angles[0] >= azimuth[0] && angles[0] <= azimuth[1] &&
                    angles[1] >= elevation[0] && angles[1] <= elevation[1]) {
                    selectedEntities[selectedCount++] = networkEntities[i].entity;
                }
            }
            selectionEase = 0.0f;
            logDebug("selected %u of %u\n", selectedCount, networkEntityCount);
        }

        double planeHeight = 0.0;
        double planeSpread = 1.0;
        uint32_t planeCount = 0;
        for (uint32_t i = 0; i < networkEntityCount; i++) {
            if (networkEntities[i].kind >= KIND_ROCK) {
                continue;
            }
            planeHeight += networkEntities[i].position[1];
            planeCount++;
        }
        if (planeCount) {
            planeHeight /= (double)planeCount;
            for (uint32_t i = 0; i < networkEntityCount; i++) {
                if (networkEntities[i].kind >= KIND_ROCK) {
                    continue;
                }
                double deviation = networkEntities[i].position[1] - planeHeight;
                if (deviation < 0.0) {
                    deviation = -deviation;
                }
                if (deviation > planeSpread) {
                    planeSpread = deviation;
                }
            }
        }
        int activeStation = -1;
        for (uint32_t i = 0; i < selectedCount && activeStation < 0; i++) {
            activeStation = stationIndexOf(selectedEntities[i]);
        }
        int droneSelected = 0;
        for (uint32_t i = 0; i < networkEntityCount && !droneSelected; i++) {
            droneSelected = networkEntities[i].kind == KIND_DRONE && isSelected(networkEntities[i].entity);
        }
        if (!droneSelected) {
            buildPending = 0;
        }
        if (windowActive) {
            int keys[ACTION_COUNT];
            for (uint32_t i = 0; i < ACTION_COUNT; i++) {
                keys[i] = settings.actionKey[i] && (GetAsyncKeyState(settings.actionKey[i]) & 0x8000) != 0;
            }
            if (!menuOpen) {
                for (uint32_t i = 0; i < ACTION_COUNT; i++) {
                    if (keys[i] && !previousKeys[i]) {
                        performAction(i, activeStation, droneSelected, &buildPending, &orderHeightStage);
                    }
                }
            }
            memcpy(previousKeys, keys, sizeof(previousKeys));
        }
        if (menuOpen) {
            float limit = menuScrollLimit(capabilities.currentExtent, menuTab);
            menuScroll -= wheelTravel * uiUnit(58.0f);
            menuScroll = menuScroll < 0.0f ? 0.0f : (menuScroll > limit ? limit : menuScroll);
            menuItemCount = menuLayout(capabilities.currentExtent, menuTab, menuScroll, menuItems);
        } else if (menuReveal < 0.004f) {
            menuItemCount = 0;
        }
        wheelTravel = 0.0f;
        if (hudVisible) {
            hudButtonCount = hudLayout(capabilities.currentExtent, activeStation >= 0, droneSelected, hudButtons);
        } else if (hudReveal < 0.004f) {
            hudButtonCount = 0;
        }
        menuReveal = approach(menuReveal, menuOpen ? 1.0f : 0.0f, 16.0f, delta);
        hudReveal = approach(hudReveal, hudVisible && hudButtonCount ? 1.0f : 0.0f, 18.0f, delta);
        selectionEase = approach(selectionEase, 1.0f, 9.0f, delta);
        for (uint32_t i = 0; i < ACTION_COUNT; i++) {
            int over = 0;
            for (uint32_t slot = 0; slot < hudButtonCount && !over; slot++) {
                over = hudButtons[slot].action == i && menuReveal < 0.004f &&
                       pointInside(pointerX, pointerY, hudButtons[slot].x0, hudButtons[slot].y0,
                                   hudButtons[slot].x1, hudButtons[slot].y1);
            }
            hudMotion[i] = approach(hudMotion[i], over ? 1.0f : 0.0f, 14.0f, delta);
        }
        for (uint32_t i = 0; i < menuItemCount; i++) {
            int over = menuOpen && menuItemHit(capabilities.currentExtent, &menuItems[i], pointerX, pointerY);
            menuMotion[i] = approach(menuMotion[i], over ? 1.0f : 0.0f, 14.0f, delta);
        }
        float tabTarget = menuTabLeft(capabilities.currentExtent, menuTab);
        tabSlide = tabSlide < 1.0f ? tabTarget : approach(tabSlide, tabTarget, 18.0f, delta);
        for (uint32_t i = 0; i < menuItemCount && pointerPressed; i++) {
            if (!menuItemHit(capabilities.currentExtent, &menuItems[i], pointerX, pointerY)) {
                continue;
            }
            switch (menuItems[i].kind) {
                case MENU_TAB:
                    menuTab = menuItems[i].action;
                    menuScroll = 0.0f;
                    rebindAction = -1;
                    break;
                case MENU_PIPELINE:
                    settings.pathTracing = (int)menuItems[i].action;
                    break;
                case MENU_BIND:
                    rebindAction = (int)menuItems[i].action;
                    break;
                case MENU_RESET:
                    for (uint32_t slot = 0; slot < ACTION_COUNT; slot++) {
                        settings.actionKey[slot] = actionDefaultKey[slot];
                    }
                    rebindAction = -1;
                    break;
                case MENU_QUIT:
                    running = 0;
                    break;
                case MENU_RESUME:
                    menuOpen = 0;
                    break;
                default:
                    break;
            }
            settingsSave();
            pointerPressed = 0;
            break;
        }
        if (!running) {
            break;
        }
        if (pointerPressed && menuOpen &&
            !pointInside(pointerX, pointerY, menuLeft(capabilities.currentExtent), menuTop(capabilities.currentExtent),
                         menuLeft(capabilities.currentExtent) + MENU_WIDTH, menuTop(capabilities.currentExtent) + MENU_HEIGHT)) {
            menuOpen = 0;
            rebindAction = -1;
            pointerPressed = 0;
        }
        for (uint32_t i = 0; i < hudButtonCount && pointerPressed && !menuOpen; i++) {
            if (!pointInside(pointerX, pointerY, hudButtons[i].x0, hudButtons[i].y0, hudButtons[i].x1, hudButtons[i].y1)) {
                continue;
            }
            performAction(hudButtons[i].action, activeStation, droneSelected, &buildPending, &orderHeightStage);
            break;
        }
        int previewValid = 0;
        int previewTarget = -1;
        double previewPoint[3] = {0.0, 0.0, 0.0};
        if (selectedCount && !orderHeightStage && !buildPending) {
            float nearest = 0.055f;
            for (uint32_t i = 0; i < networkEntityCount; i++) {
                uint32_t candidate = networkEntities[i].kind;
                int seam = droneSelected && candidate >= KIND_ROCK && candidate < KIND_COUNT;
                int berth = candidate == KIND_STATION && networkEntities[i].owner == (uint32_t)networkPlayer;
                int foe = networkEntities[i].owner && networkEntities[i].owner != (uint32_t)networkPlayer;
                if (candidate >= KIND_STAR || (!seam && !berth && !foe)) {
                    continue;
                }
                float direction[3];
                relativeTo(networkEntities[i].position, cameraPosition, direction);
                if (direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2] < 0.1f) {
                    continue;
                }
                float angles[2];
                directionAngles(direction, right, up, forward, angles);
                float offset = sqrtf(angles[0] * angles[0] + angles[1] * angles[1]);
                if (offset < nearest) {
                    nearest = offset;
                    previewTarget = (int)i;
                }
            }
        }
        if (previewTarget >= 0) {
            previewPoint[0] = networkEntities[previewTarget].position[0];
            previewPoint[1] = networkEntities[previewTarget].position[1];
            previewPoint[2] = networkEntities[previewTarget].position[2];
            previewValid = 1;
        } else if (selectedCount) {
            if (!orderHeightStage) {
                if (forward[1] < -0.02f || forward[1] > 0.02f) {
                    double distance = (planeHeight - cameraPosition[1]) / (double)forward[1];
                    if (distance > 0.05) {
                        for (uint32_t i = 0; i < 3; i++) {
                            previewPoint[i] = cameraPosition[i] + (double)forward[i] * distance;
                        }
                        previewPoint[1] = planeHeight;
                        previewValid = 1;
                    }
                }
            } else {
                float offset[3] = {
                    (float)(cameraPosition[0] - orderTarget[0]),
                    (float)(cameraPosition[1] - planeHeight),
                    (float)(cameraPosition[2] - orderTarget[2])
                };
                float projection = forward[1];
                float alongRay = forward[0] * offset[0] + forward[1] * offset[1] + forward[2] * offset[2];
                float denominator = 1.0f - projection * projection;
                previewPoint[0] = orderTarget[0];
                previewPoint[1] = planeHeight;
                previewPoint[2] = orderTarget[2];
                if (denominator > 0.0005f) {
                    float reach = sqrtf(offset[0] * offset[0] + offset[2] * offset[2]) * 2.0f + 100.0f;
                    float height = (offset[1] - projection * alongRay) / denominator;
                    previewPoint[1] = planeHeight + (double)(height < -reach ? -reach : (height > reach ? reach : height));
                }
                previewValid = 1;
            }
        }
        if (rightPressed && previewValid) {
            for (uint32_t i = 0; i < 3; i++) {
                orderTarget[i] = previewPoint[i];
            }
            if (previewTarget >= 0) {
                uint32_t mark = networkEntities[previewTarget].kind;
                uint32_t order = ORDER_ATTACK;
                float radius = 1200.0f;
                if (mark >= KIND_ROCK && mark < KIND_COUNT) {
                    order = ORDER_MINE;
                    radius = 0.0f;
                } else if (mark == KIND_STATION && networkEntities[previewTarget].owner == (uint32_t)networkPlayer) {
                    order = ORDER_DOCK;
                    radius = 0.0f;
                }
                networkCommand(orderTarget, networkEntities[previewTarget].entity, order, radius, selectedEntities, selectedCount);
                pushOrderPing(orderTarget, planeHeight,
                              order == ORDER_MINE ? INK_TEAL : (order == ORDER_DOCK ? INK_AZURE : INK_ROSE));
                logDebug("order %u on entity %u with %u ships\n", order,
                         (uint32_t)networkEntities[previewTarget].entity, selectedCount);
            } else if (!orderHeightStage) {
                orderHeightStage = 1;
            } else {
                orderHeightStage = 0;
                networkCommand(orderTarget, 0, buildPending ? ORDER_BUILD : ORDER_MOVE, 0.0f, selectedEntities, selectedCount);
                pushOrderPing(orderTarget, planeHeight, buildPending ? INK_AZURE : INK_AMBER);
                logDebug("%s %.2f %.2f %.2f with %u ships\n", buildPending ? "build" : "move",
                         orderTarget[0], orderTarget[1], orderTarget[2], selectedCount);
                buildPending = 0;
            }
        }

        instanceCount = 0;
        sectionSlots = 0;
        exhaustCount = 0;
        starCentre[0] = 0.0f;
        starCentre[1] = 0.0f;
        starCentre[2] = 0.0f;
        starRadius = 6.96e8f;
        for (uint32_t i = 0; i < networkEntityCount; i++) {
            float scale = entityScale[i];
            uint32_t material = entityMaterial[i];
            if (material == MATERIAL_STAR) {
                relativeTo(networkEntities[i].position, cameraPosition, starCentre);
                starRadius = scale;
            }
            uint32_t mesh = meshOfKind(networkEntities[i].kind);
            if (instanceCount == MAX_INSTANCES) {
                continue;
            }
            instanceTints[instanceCount] = material;
            instanceMeshes[instanceCount] = mesh;
            instanceScales[instanceCount] = scale;
            instanceCeilings[instanceCount] = networkEntities[i].kind == KIND_STATION && networkEntities[i].throttle < 0.999f
                                                  ? stationLow + (stationHigh - stationLow) * networkEntities[i].throttle
                                                  : NO_CEILING;
            instanceGlow[instanceCount] = 1.0f;
            instanceFlow[instanceCount] = 0.0f;
            memcpy(instanceRotations + instanceCount * 4, networkEntities[i].orientation, sizeof(float) * 4);
            relativeTo(networkEntities[i].position, cameraPosition, instancePositions + instanceCount * 3);
            const float* hull = instancePositions + instanceCount * 3;
            const float* facing = instanceRotations + instanceCount * 4;
            instanceCount++;
            if (networkEntities[i].kind == KIND_STATION && networkEntities[i].throttle < 0.999f &&
                instanceCount < MAX_INSTANCES && sectionSlots < SECTION_SLOTS) {
                float front = stationLow + (stationHigh - stationLow) * networkEntities[i].throttle;
                uint32_t slot = frameIndex * SECTION_SLOTS + sectionSlots++;
                sectionDrawn[slot] = stationCrossSection(front, vertexScratch + meshBase[MESH_SECTION + slot], SECTION_CAPACITY);
                float upright[3] = {0.0f, 1.0f, 0.0f};
                float lifted[3];
                turnVector(facing, upright, lifted);
                instanceTints[instanceCount] = 0;
                instanceMeshes[instanceCount] = MESH_SECTION + slot;
                instanceScales[instanceCount] = scale;
                instanceCeilings[instanceCount] = NO_CEILING;
                instanceGlow[instanceCount] = 1.0f;
                instanceFlow[instanceCount] = 0.0f;
                memcpy(instanceRotations + instanceCount * 4, facing, sizeof(float) * 4);
                for (uint32_t axis = 0; axis < 3; axis++) {
                    instancePositions[instanceCount * 3 + axis] = hull[axis] + lifted[axis] * (front + 0.0015f) * scale;
                }
                offerExhaust(exhaustPoints, exhaustRange, &exhaustCount, instancePositions + instanceCount * 3,
                             EXHAUST_GLOW * scale * scale * 0.0015f);
                instanceCount++;
            }
            if (meshMountCount[mesh]) {
                float ahead[3] = {1.0f, 0.0f, 0.0f};
                float heading[3];
                turnVector(facing, ahead, heading);
                const NetworkEntity* quarry = entityOfTarget(networkEntities[i].target);
                for (uint32_t mount = 0; mount < meshMountCount[mesh] && instanceCount + 1 < MAX_INSTANCES; mount++) {
                    const float* seat = meshMounts[mesh] + mount * 6;
                    float offset[3] = {seat[0] * scale, seat[1] * scale, seat[2] * scale};
                    float turned[3];
                    float bearing[3];
                    turnVector(facing, offset, turned);
                    turnVector(facing, seat + 3, bearing);
                    float station[3] = {hull[0] + turned[0], hull[1] + turned[1], hull[2] + turned[2]};
                    float look[3] = {heading[0], heading[1], heading[2]};
                    if (quarry) {
                        float toward[3];
                        relativeTo(quarry->position, cameraPosition, toward);
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            toward[axis] -= station[axis];
                        }
                        float reach = sqrtf(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
                        if (reach > 1.0f) {
                            for (uint32_t axis = 0; axis < 3; axis++) {
                                toward[axis] /= reach;
                            }
                            if (toward[0] * bearing[0] + toward[1] * bearing[1] + toward[2] * bearing[2] > TURRET_CONE) {
                                memcpy(look, toward, sizeof(look));
                            }
                        }
                    }
                    float along = look[0] * bearing[0] + look[1] * bearing[1] + look[2] * bearing[2];
                    float traverse[3];
                    float span = 0.0f;
                    for (uint32_t axis = 0; axis < 3; axis++) {
                        traverse[axis] = look[axis] - bearing[axis] * along;
                        span += traverse[axis] * traverse[axis];
                    }
                    if (span < 1e-6f) {
                        along = heading[0] * bearing[0] + heading[1] * bearing[1] + heading[2] * bearing[2];
                        span = 0.0f;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            traverse[axis] = heading[axis] - bearing[axis] * along;
                            span += traverse[axis] * traverse[axis];
                        }
                    }
                    span = sqrtf(span);
                    for (uint32_t axis = 0; axis < 3; axis++) {
                        traverse[axis] /= span;
                    }
                    instanceTints[instanceCount] = material;
                    instanceMeshes[instanceCount] = MESH_TURRET;
                    instanceScales[instanceCount] = scale;
                    instanceCeilings[instanceCount] = NO_CEILING;
                    instanceGlow[instanceCount] = 1.0f;
                    instanceFlow[instanceCount] = 0.0f;
                    quaternionLook(traverse, bearing, instanceRotations + instanceCount * 4);
                    instancePositions[instanceCount * 3 + 0] = station[0];
                    instancePositions[instanceCount * 3 + 1] = station[1];
                    instancePositions[instanceCount * 3 + 2] = station[2];
                    instanceCount++;
                    instanceTints[instanceCount] = material;
                    instanceMeshes[instanceCount] = MESH_GUN;
                    instanceScales[instanceCount] = scale;
                    instanceCeilings[instanceCount] = NO_CEILING;
                    instanceGlow[instanceCount] = 1.0f;
                    instanceFlow[instanceCount] = 0.0f;
                    quaternionLook(look, bearing, instanceRotations + instanceCount * 4);
                    for (uint32_t axis = 0; axis < 3; axis++) {
                        instancePositions[instanceCount * 3 + axis] = station[axis] + bearing[axis] * gunTrunnion * scale;
                    }
                    instanceCount++;
                }
            }
            if (networkEntities[i].kind == KIND_DRONE) {
                const NetworkEntity* work = entityOfTarget(networkEntities[i].target);
                float seamScale = 0.0f;
                if (work) {
                    uint32_t seamMaterial;
                    appearanceOf(work->kind, work->owner, &seamScale, &seamMaterial);
                }
                int seam = work && work->kind >= KIND_ROCK && work->kind < KIND_COUNT;
                int site = work && work->kind == KIND_STATION && work->throttle < 0.999f;
                float muzzle[3];
                float offset[3] = {droneMounts[0] * scale, droneMounts[1] * scale, droneMounts[2] * scale};
                float turned[3];
                turnVector(facing, offset, turned);
                for (uint32_t axis = 0; axis < 3; axis++) {
                    muzzle[axis] = hull[axis] + turned[axis];
                }
                int lit = 0;
                float centre[3] = {0.0f, 0.0f, 0.0f};
                if (seam || site) {
                    relativeTo(work->position, cameraPosition, centre);
                    float gap = sqrtf((centre[0] - muzzle[0]) * (centre[0] - muzzle[0]) +
                                      (centre[1] - muzzle[1]) * (centre[1] - muzzle[1]) +
                                      (centre[2] - muzzle[2]) * (centre[2] - muzzle[2]));
                    lit = gap < seamScale + kindRadius[KIND_DRONE] + (seam ? MINING_RANGE : 900.0f) + 80.0f;
                }
                BeamTrack* beam = beamTrackOf(networkEntities[i].entity, lit, delta);
                if (beam && lit) {
                    beam->rays = (uint8_t)(seam ? 1 : 2);
                    beam->site = (uint8_t)site;
                    for (uint32_t ray = 0; ray < beam->rays; ray++) {
                        float aim[3] = {centre[0], centre[1], centre[2]};
                        beam->trim[ray] = seamScale;
                        if (site) {
                            float pace = ray ? -1.7f : 2.6f;
                            float sweep = wallSeconds * pace + 1.9f * (float)ray;
                            float front = stationLow + (stationHigh - stationLow) * work->throttle;
                            float crawl = 0.05f * sinf(wallSeconds * 3.1f + 2.0f * (float)ray);
                            float spread = 0.30f + 0.26f * (0.5f + 0.5f * sinf(wallSeconds * 1.3f + (float)ray));
                            aim[0] += seamScale * spread * cosf(sweep);
                            aim[1] += seamScale * (front + crawl);
                            aim[2] += seamScale * spread * sinf(sweep);
                            beam->trim[ray] = 0.0f;
                        }
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            beam->aim[ray][axis] = cameraPosition[axis] + (double)aim[axis];
                        }
                    }
                }
                if (beam && beam->level > 0.002f) {
                    float reached = beam->level * beam->level;
                    float flash = 1.0f + BEAM_FLASH * expf(-beam->age * BEAM_IGNITION);
                    float flicker = 1.0f + 0.09f * sinf(wallSeconds * 13.0f + (float)(beam->entity & 63u));
                    float gain = reached * flash * flicker * (beam->site ? BUILD_BEAM_GAIN : 1.0f);
                    for (uint32_t ray = 0; ray < beam->rays && instanceCount < MAX_INSTANCES; ray++) {
                        float aim[3];
                        float toward[3];
                        float reach = 0.0f;
                        relativeTo(beam->aim[ray], cameraPosition, aim);
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            toward[axis] = aim[axis] - muzzle[axis];
                            reach += toward[axis] * toward[axis];
                        }
                        reach = sqrtf(reach);
                        if (reach <= beam->trim[ray] + 1.0f) {
                            continue;
                        }
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            toward[axis] /= reach;
                        }
                        float span = (reach - beam->trim[ray]) * beam->level;
                        float sideA[3];
                        float sideB[3];
                        crossAxes(toward, sideA, sideB);
                        float seed = (float)(beam->entity & 31u) + 3.1f * (float)ray;
                        float twist = wallSeconds * BEAM_TWIST + seed;
                        float upward[3];
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            upward[axis] = sideA[axis] * cosf(twist) + sideB[axis] * sinf(twist);
                        }
                        uint32_t churn = (uint32_t)(wallSeconds * BEAM_CHURN + seed) % RAY_VARIANTS;
                        instanceTints[instanceCount] = 0;
                        instanceMeshes[instanceCount] = (beam->site ? MESH_PRINTER : MESH_BEAM) + churn;
                        instanceScales[instanceCount] = span;
                        instanceCeilings[instanceCount] = NO_CEILING;
                        instanceGlow[instanceCount] = gain;
                        instanceFlow[instanceCount] = 1.0f + beam->flow;
                        quaternionLook(toward, upward, instanceRotations + instanceCount * 4);
                        instancePositions[instanceCount * 3 + 0] = muzzle[0];
                        instancePositions[instanceCount * 3 + 1] = muzzle[1];
                        instancePositions[instanceCount * 3 + 2] = muzzle[2];
                        instanceCount++;
                        float tip[3] = {
                            muzzle[0] + toward[0] * span,
                            muzzle[1] + toward[1] * span,
                            muzzle[2] + toward[2] * span
                        };
                        offerExhaust(exhaustPoints, exhaustRange, &exhaustCount, tip, BEAM_GLOW * gain);
                    }
                }
            }
            float burn = networkEntities[i].throttle;
            if (burn <= 0.02f || networkEntities[i].kind == KIND_STATION) {
                continue;
            }
            for (uint32_t vent = 0; vent < meshNozzleCount[mesh] && instanceCount < MAX_INSTANCES; vent++) {
                const float* nozzle = meshNozzles[mesh] + vent * 4;
                float plume = scale * nozzle[3] * (0.45f + 0.55f * burn);
                float offset[3] = {nozzle[0] * scale, nozzle[1] * scale, nozzle[2] * scale};
                float turned[3];
                turnVector(facing, offset, turned);
                instanceTints[instanceCount] = 0;
                instanceMeshes[instanceCount] = MESH_FLAME;
                instanceScales[instanceCount] = plume;
                instanceCeilings[instanceCount] = NO_CEILING;
                instanceGlow[instanceCount] = 1.0f;
                instanceFlow[instanceCount] = 0.0f;
                memcpy(instanceRotations + instanceCount * 4, facing, sizeof(float) * 4);
                instancePositions[instanceCount * 3 + 0] = hull[0] + turned[0];
                instancePositions[instanceCount * 3 + 1] = hull[1] + turned[1];
                instancePositions[instanceCount * 3 + 2] = hull[2] + turned[2];
                instanceCount++;
                float axis[3] = {-2.2f * plume, 0.0f, 0.0f};
                float trail[3];
                turnVector(facing, axis, trail);
                float centre[3] = {
                    hull[0] + turned[0] + trail[0],
                    hull[1] + turned[1] + trail[1],
                    hull[2] + turned[2] + trail[2]
                };
                offerExhaust(exhaustPoints, exhaustRange, &exhaustCount, centre, EXHAUST_GLOW * plume * plume * burn);
            }
        }
        beamTracksSweep();
        for (uint32_t i = 0; i < networkShotCount && instanceCount < MAX_INSTANCES; i++) {
            if (networkPlayback < networkShots[i].fireTick) {
                continue;
            }
            float scale;
            uint32_t material;
            appearanceOf(networkShots[i].kind, 0, &scale, &material);
            float nose[3] = {networkShots[i].velocity[0], networkShots[i].velocity[1], networkShots[i].velocity[2]};
            float speed = sqrtf(nose[0] * nose[0] + nose[1] * nose[1] + nose[2] * nose[2]);
            if (speed < 1e-3f) {
                continue;
            }
            for (uint32_t axis = 0; axis < 3; axis++) {
                nose[axis] /= speed;
            }
            static const float upward[3] = {0.0f, 1.0f, 0.0f};
            instanceTints[instanceCount] = material;
            instanceMeshes[instanceCount] = meshOfKind(networkShots[i].kind);
            instanceScales[instanceCount] = scale;
            instanceCeilings[instanceCount] = NO_CEILING;
            instanceGlow[instanceCount] = 1.0f;
            instanceFlow[instanceCount] = 0.0f;
            quaternionLook(nose, upward, instanceRotations + instanceCount * 4);
            relativeTo(networkShots[i].current, cameraPosition, instancePositions + instanceCount * 3);
            instanceCount++;
        }
        memset(meshInstanceCount, 0, sizeof(meshInstanceCount));
        for (uint32_t i = 0; i < instanceCount; i++) {
            meshInstanceCount[instanceMeshes[i]]++;
        }
        uint32_t written = 0;
        for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
            meshFirstInstance[mesh] = written;
            meshCursor[mesh] = written;
            written += meshInstanceCount[mesh];
        }
        for (uint32_t i = 0; i < instanceCount; i++) {
            float* entry = graphicsInstances + meshCursor[instanceMeshes[i]]++ * INSTANCE_FLOATS;
            entry[0] = instancePositions[i * 3 + 0];
            entry[1] = instancePositions[i * 3 + 1];
            entry[2] = instancePositions[i * 3 + 2];
            entry[3] = instanceScales[i];
            entry[4] = instanceRotations[i * 4 + 0];
            entry[5] = instanceRotations[i * 4 + 1];
            entry[6] = instanceRotations[i * 4 + 2];
            entry[7] = instanceRotations[i * 4 + 3];
            entry[8] = (float)instanceTints[i];
            entry[9] = instanceCeilings[i];
            entry[10] = instanceGlow[i];
            entry[11] = instanceFlow[i];
        }
        float view[16] = {
            right[0], up[0], forward[0], 0.0f,
            right[1], up[1], forward[1], 0.0f,
            right[2], up[2], forward[2], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        float nearPlane = 1.0f;
        float focal = 1.0f / tanHalfFovY;
        float projection[16] = {
            focal / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, -focal, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, nearPlane, 0.0f
        };
        float viewProjection[16];
        matrixMultiply(projection, view, viewProjection);
        float cameraPush[32] = {
            0.0f, 0.0f, 0.0f, 0.0f,
            right[0], right[1], right[2], tanHalfFovY * aspect,
            up[0], up[1], up[2], tanHalfFovY,
            forward[0], forward[1], forward[2], 0.0f,
            starCentre[0], starCentre[1], starCentre[2], starRadius,
            36000.0f, 28000.0f, 38000.0f, 0.0f,
            FOG_SCATTERING, FOG_PHASE, FOG_STEPS, FOG_RANGE,
            (float)exhaustCount, 0.0f, 0.0f, 0.0f
        };
        memcpy(&cameraPush[3], &frameCounter, sizeof(frameCounter));
        float rasterPush[24];
        memcpy(rasterPush, viewProjection, sizeof(viewProjection));
        rasterPush[16] = starCentre[0];
        rasterPush[17] = starCentre[1];
        rasterPush[18] = starCentre[2];
        rasterPush[19] = starRadius;
        rasterPush[20] = 36000.0f;
        rasterPush[21] = 28000.0f;
        rasterPush[22] = 38000.0f;
        rasterPush[23] = EXPOSURE;

        vkWaitForFences(device, 1, &fences[frameIndex], VK_TRUE, 0xFFFFFFFFFFFFFFFF);
        uint32_t imageIndex;
        vkAcquireNextImageKHR(device, swapchain, 0xFFFFFFFFFFFFFFFF, imageAvailable[frameIndex], VK_NULL_HANDLE, &imageIndex);
        vkResetFences(device, 1, &fences[frameIndex]);
        VkCommandBuffer cmd = commandBuffers[frameIndex];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd, &beginInfo);
        if (settings.pathTracing) {
            uint32_t groupsX = (capabilities.currentExtent.width + 7) / 8;
            uint32_t groupsY = (capabilities.currentExtent.height + 7) / 8;
            memcpy(exhaustLights[frameIndex], exhaustPoints, sizeof(float) * 4 * exhaustCount);
            for (uint32_t i = 0; i < instanceCount; i++) {
                float scale = instanceScales[i];
                float qx = instanceRotations[i * 4 + 0];
                float qy = instanceRotations[i * 4 + 1];
                float qz = instanceRotations[i * 4 + 2];
                float qw = instanceRotations[i * 4 + 3];
                VkAccelerationStructureInstanceKHR* target = &instances[frameIndex][i];
                *target = (VkAccelerationStructureInstanceKHR){
                    .transform = {{{(1.0f - 2.0f * (qy * qy + qz * qz)) * scale, 2.0f * (qx * qy - qz * qw) * scale, 2.0f * (qx * qz + qy * qw) * scale, instancePositions[i * 3 + 0]},
                                   {2.0f * (qx * qy + qz * qw) * scale, (1.0f - 2.0f * (qx * qx + qz * qz)) * scale, 2.0f * (qy * qz - qx * qw) * scale, instancePositions[i * 3 + 1]},
                                   {2.0f * (qx * qz - qy * qw) * scale, 2.0f * (qy * qz + qx * qw) * scale, (1.0f - 2.0f * (qx * qx + qy * qy)) * scale, instancePositions[i * 3 + 2]}}},
                    .instanceCustomIndex = 0, .mask = 0xFF,
                    .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                             (instanceCeilings[i] < NO_CEILING ? 0u : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR),
                    .accelerationStructureReference = meshReference[instanceMeshes[i]]
                };
                float* facts = instanceFacts[frameIndex] + (size_t)i * INSTANCE_FACTS;
                facts[0] = (float)meshBase[instanceMeshes[i]];
                facts[1] = (float)instanceTints[i];
                facts[2] = instanceCeilings[i];
                facts[3] = instanceGlow[i];
                facts[4] = instanceFlow[i];
            }
            for (uint32_t slot = 0; slot < sectionSlots; slot++) {
                uint32_t mesh = MESH_SECTION + frameIndex * SECTION_SLOTS + slot;
                bottomRange[mesh].primitiveCount = sectionDrawn[mesh - MESH_SECTION] / 3;
                bottomBuildInfo[mesh].dstAccelerationStructure = bottomLevel[mesh];
                bottomBuildInfo[mesh].scratchData.deviceAddress = scratchAddress;
                const VkAccelerationStructureBuildRangeInfoKHR* sectionRanges = &bottomRange[mesh];
                pfnCmdBuildAccelerationStructures(cmd, 1, &bottomBuildInfo[mesh], &sectionRanges);
                VkMemoryBarrier sectionBarrier = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
                };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &sectionBarrier, 0, NULL, 0, NULL);
            }
            topGeometry.geometry.instances.data.deviceAddress = instanceAddress[frameIndex];
            topBuildInfo.dstAccelerationStructure = topLevel[frameIndex];
            topBuildInfo.scratchData.deviceAddress = topScratchAddress[frameIndex];
            VkAccelerationStructureBuildRangeInfoKHR topRange = {.primitiveCount = instanceCount};
            const VkAccelerationStructureBuildRangeInfoKHR* topRanges = &topRange;
            pfnCmdBuildAccelerationStructures(cmd, 1, &topBuildInfo, &topRanges);
            VkMemoryBarrier topBarrier = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &topBarrier, 0, NULL, 0, NULL);
            VkMemoryBarrier passBarrier = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
            };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &descriptorSets[frameIndex], 0, NULL);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelines[0]);
            vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cameraPush), cameraPush);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &passBarrier, 0, NULL, 0, NULL);
            float resolvePush[4] = {0.0f, 0.0f, 0.0f, EXPOSURE};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelines[1]);
            vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(resolvePush), resolvePush);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);
            VkImageMemoryBarrier toTransferSrc = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[IMAGE_OUTPUT], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
            };
            VkImageMemoryBarrier toTransferDst = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchainImages[imageIndex], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
            };
            VkImageMemoryBarrier blitBarriers[2] = {toTransferSrc, toTransferDst};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, blitBarriers);
            VkImageBlit blitRegion = {
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .srcOffsets = {{0, 0, 0}, {(int32_t)capabilities.currentExtent.width, (int32_t)capabilities.currentExtent.height, 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .dstOffsets = {{0, 0, 0}, {(int32_t)capabilities.currentExtent.width, (int32_t)capabilities.currentExtent.height, 1}}
            };
            vkCmdBlitImage(cmd, images[IMAGE_OUTPUT], VK_IMAGE_LAYOUT_GENERAL, swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion, VK_FILTER_NEAREST);
            VkImageMemoryBarrier toColour = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchainImages[imageIndex], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toColour);
        } else {
            VkImageMemoryBarrier toColorAttachment = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchainImages[imageIndex], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
            };
            VkImageMemoryBarrier toDepthAttachment = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = depthImage, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}
            };
            VkImageMemoryBarrier attachmentBarriers[2] = {toColorAttachment, toDepthAttachment};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 0, NULL, 0, NULL, 2, attachmentBarriers);
            VkRenderingAttachmentInfo colorAttachment = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = swapchainImageViews[imageIndex],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}
            };
            VkRenderingAttachmentInfo depthAttachment = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = depthImageView,
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clearValue = {.depthStencil = {0.0f, 0}}
            };
            VkRenderingInfo renderingInfo = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = {{0, 0}, capabilities.currentExtent},
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &colorAttachment,
                .pDepthAttachment = &depthAttachment
            };
            vkCmdBeginRendering(cmd, &renderingInfo);
            float backgroundPush[12] = {
                right[0], right[1], right[2], tanHalfFovY * aspect,
                up[0], up[1], up[2], tanHalfFovY,
                forward[0], forward[1], forward[2], EXPOSURE
            };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline);
            vkCmdPushConstants(cmd, backgroundPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(backgroundPush), backgroundPush);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport sceneViewport = {
                0.0f, 0.0f, (float)capabilities.currentExtent.width, (float)capabilities.currentExtent.height, 0.0f, 1.0f
            };
            VkRect2D sceneScissor = {{0, 0}, capabilities.currentExtent};
            vkCmdSetViewport(cmd, 0, 1, &sceneViewport);
            vkCmdSetScissor(cmd, 0, 1, &sceneScissor);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rasterPush), rasterPush);
            VkBuffer vertexBuffers[2] = {vertexBuffer, instanceBufferGraphics};
            VkDeviceSize vertexOffsets[2] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, vertexOffsets);
            for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
                uint32_t drawn = mesh >= MESH_SECTION ? sectionDrawn[mesh - MESH_SECTION] : meshVertexCount[mesh];
                if (meshInstanceCount[mesh] && drawn) {
                    vkCmdDraw(cmd, drawn, meshInstanceCount[mesh], meshBase[mesh], meshFirstInstance[mesh]);
                }
            }
            vkCmdEndRendering(cmd);
        }
        VkRenderingAttachmentInfo overlayAttachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = swapchainImageViews[imageIndex],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE
        };
        VkRenderingInfo overlayRendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, capabilities.currentExtent},
            .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &overlayAttachment
        };
        vkCmdBeginRendering(cmd, &overlayRendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipelineLayout, 0, 1, &atlasSet, 0, NULL);
        VkExtent2D extent = capabilities.currentExtent;
        uiBegin(cmd, overlayPipelineLayout, extent);
        float viewWidth = (float)extent.width;
        float viewHeight = (float)extent.height;
        float centreX = floorf(viewWidth * 0.5f);
        float centreY = floorf(viewHeight * 0.5f);
        Colour amber = colourOf(INK_AMBER, 1.0f);
        Colour teal = colourOf(INK_TEAL, 1.0f);
        Colour azure = colourOf(INK_AZURE, 1.0f);
        Colour rose = colourOf(INK_ROSE, 1.0f);
        Colour textTone = colourOf(INK_TEXT, 1.0f);
        Colour mutedTone = colourOf(INK_MUTED, 1.0f);
        Colour faintTone = colourOf(INK_FAINT, 1.0f);
        vkCmdSetScissor(cmd, 0, 1, &(VkRect2D){{0, 0}, extent});
        drawOrderPings(cameraPosition, right, up, forward, tanHalfFovY * aspect, tanHalfFovY, extent, delta);
        if (!pointerFree) {
            float breath = 0.55f + 0.25f * pulseOf(wallSeconds, 3.4f);
            float reach = uiUnit(11.0f);
            float gap = uiUnit(4.0f);
            float weight = uiUnit(1.6f);
            uiCapsule(centreX - reach, centreY, centreX - gap, centreY, weight, 0.0f,
                      colourFade(textTone, breath), colourFade(textTone, breath));
            uiCapsule(centreX + gap, centreY, centreX + reach, centreY, weight, 0.0f,
                      colourFade(textTone, breath), colourFade(textTone, breath));
            uiCapsule(centreX, centreY - reach, centreX, centreY - gap, weight, 0.0f,
                      colourFade(textTone, breath), colourFade(textTone, breath));
            uiCapsule(centreX, centreY + gap, centreX, centreY + reach, weight, 0.0f,
                      colourFade(textTone, breath), colourFade(textTone, breath));
            uiFill(centreX - uiUnit(1.0f), centreY - uiUnit(1.0f), centreX + uiUnit(1.0f), centreY + uiUnit(1.0f),
                   uiUnit(1.0f), colourFade(amber, 0.9f));
        }
        if (selectedCount) {
            for (uint32_t i = 0; i < networkEntityCount; i++) {
                if (networkEntities[i].kind >= KIND_ROCK || !isSelected(networkEntities[i].entity)) {
                    continue;
                }
                double base[3] = {networkEntities[i].position[0], planeHeight, networkEntities[i].position[2]};
                float head[2];
                float foot[2];
                if (!projectPixels(networkEntities[i].position, cameraPosition, right, up, forward,
                                   tanHalfFovY * aspect, tanHalfFovY, extent, head) ||
                    !projectPixels(base, cameraPosition, right, up, forward,
                                   tanHalfFovY * aspect, tanHalfFovY, extent, foot)) {
                    continue;
                }
                float lift = (float)((networkEntities[i].position[1] - planeHeight) / planeSpread);
                lift = lift < -1.0f ? -1.0f : (lift > 1.0f ? 1.0f : lift);
                Colour above = colourMix(faintTone, amber, lift > 0.0f ? lift : 0.0f);
                Colour below = colourMix(faintTone, azure, lift < 0.0f ? -lift : 0.0f);
                Colour tint = lift < 0.0f ? below : above;
                uiCapsule(foot[0], foot[1], head[0], head[1], uiUnit(1.4f), uiUnit(9.0f),
                          colourFade(tint, 0.05f), colourFade(tint, 0.65f));
                uiFill(foot[0] - uiUnit(2.5f), foot[1] - uiUnit(2.5f), foot[0] + uiUnit(2.5f), foot[1] + uiUnit(2.5f),
                       uiUnit(2.5f), colourFade(tint, 0.75f));
            }
        }
        if (orderHeightStage && previewValid) {
            double base[3] = {orderTarget[0], planeHeight, orderTarget[2]};
            float head[2];
            float foot[2];
            if (projectPixels(previewPoint, cameraPosition, right, up, forward, tanHalfFovY * aspect, tanHalfFovY, extent, head) &&
                projectPixels(base, cameraPosition, right, up, forward, tanHalfFovY * aspect, tanHalfFovY, extent, foot)) {
                uiCapsule(foot[0], foot[1], head[0], head[1], uiUnit(2.0f), uiUnit(11.0f),
                          colourFade(amber, 0.15f), colourFade(amber, 0.95f));
            }
            uiRing(base, cameraPosition, right, up, forward, tanHalfFovY * aspect, tanHalfFovY, extent,
                   1.0f, uiUnit(1.8f), colourFade(amber, 0.45f));
            uiRing(previewPoint, cameraPosition, right, up, forward, tanHalfFovY * aspect, tanHalfFovY, extent,
                   0.82f + 0.06f * pulseOf(wallSeconds, 1.6f), uiUnit(2.2f), colourFade(amber, 0.95f));
        }
        if (selecting) {
            float axisR[3];
            float axisU[3];
            float axisF[3];
            float azimuth[2];
            float elevation[2];
            selectionBounds(selectionStart, selectionEnd, forward, axisR, axisU, axisF, azimuth, elevation);
            float patch[(MARQUEE_STEPS + 1) * (MARQUEE_STEPS + 1) * 2];
            int reached[(MARQUEE_STEPS + 1) * (MARQUEE_STEPS + 1)];
            for (uint32_t row = 0; row <= MARQUEE_STEPS; row++) {
                float height = elevation[1] + (elevation[0] - elevation[1]) * (float)row / (float)MARQUEE_STEPS;
                for (uint32_t column = 0; column <= MARQUEE_STEPS; column++) {
                    uint32_t slot = row * (MARQUEE_STEPS + 1) + column;
                    float sweep = azimuth[0] + (azimuth[1] - azimuth[0]) * (float)column / (float)MARQUEE_STEPS;
                    float ray[3];
                    sphereDirection(sweep, height, axisR, axisU, axisF, ray);
                    reached[slot] = directionToPixels(ray, right, up, forward, tanHalfFovY * aspect, tanHalfFovY,
                                                      extent, &patch[slot * 2]);
                }
            }
            Colour sheenNear = colourFade(amber, 0.13f);
            Colour sheenFar = colourFade(teal, 0.05f);
            for (uint32_t row = 0; row < MARQUEE_STEPS; row++) {
                for (uint32_t column = 0; column < MARQUEE_STEPS; column++) {
                    uint32_t a = row * (MARQUEE_STEPS + 1) + column;
                    uint32_t b = a + 1;
                    uint32_t c = a + MARQUEE_STEPS + 1;
                    uint32_t d = c + 1;
                    if (!reached[a] || !reached[b] || !reached[c] || !reached[d]) {
                        continue;
                    }
                    float corners[8] = {
                        patch[a * 2], patch[a * 2 + 1], patch[b * 2], patch[b * 2 + 1],
                        patch[c * 2], patch[c * 2 + 1], patch[d * 2], patch[d * 2 + 1]
                    };
                    float upper = (float)row / (float)MARQUEE_STEPS;
                    float lower = (float)(row + 1) / (float)MARQUEE_STEPS;
                    uiQuad(corners, colourMix(sheenNear, sheenFar, upper), colourMix(sheenNear, sheenFar, lower));
                }
            }
            float crawl = wallSeconds * 1.6f;
            for (uint32_t step = 0; step < MARQUEE_STEPS; step++) {
                uint32_t rims[4][2] = {
                    {step, step + 1},
                    {MARQUEE_STEPS * (MARQUEE_STEPS + 1) + step, MARQUEE_STEPS * (MARQUEE_STEPS + 1) + step + 1},
                    {step * (MARQUEE_STEPS + 1), (step + 1) * (MARQUEE_STEPS + 1)},
                    {step * (MARQUEE_STEPS + 1) + MARQUEE_STEPS, (step + 1) * (MARQUEE_STEPS + 1) + MARQUEE_STEPS}
                };
                float ripple = 0.55f + 0.45f * sinf((float)step * 0.9f - crawl);
                for (uint32_t edge = 0; edge < 4; edge++) {
                    uint32_t from = rims[edge][0];
                    uint32_t to = rims[edge][1];
                    if (!reached[from] || !reached[to]) {
                        continue;
                    }
                    uiCapsule(patch[from * 2], patch[from * 2 + 1], patch[to * 2], patch[to * 2 + 1], uiUnit(6.0f), 0.0f,
                              colourFade(amber, 0.12f * ripple), colourFade(amber, 0.12f * ripple));
                    uiCapsule(patch[from * 2], patch[from * 2 + 1], patch[to * 2], patch[to * 2 + 1], uiUnit(1.8f), 0.0f,
                              colourFade(amber, 0.55f + 0.40f * ripple), colourFade(amber, 0.55f + 0.40f * ripple));
                }
            }
            uint32_t marks[4] = {
                0, MARQUEE_STEPS, MARQUEE_STEPS * (MARQUEE_STEPS + 1),
                MARQUEE_STEPS * (MARQUEE_STEPS + 1) + MARQUEE_STEPS
            };
            float tick = uiUnit(4.0f);
            for (uint32_t corner = 0; corner < 4; corner++) {
                uint32_t slot = marks[corner];
                if (!reached[slot]) {
                    continue;
                }
                float x = patch[slot * 2];
                float y = patch[slot * 2 + 1];
                uiFill(x - tick, y - tick, x + tick, y + tick, tick, colourFade(amber, 0.95f));
                uiGlow(x - tick, y - tick, x + tick, y + tick, tick, uiUnit(11.0f), colourFade(amber, 0.40f));
            }
        }
        for (uint32_t i = 0; i < networkEntityCount; i++) {
            if (networkEntities[i].kind >= KIND_STAR) {
                continue;
            }
            int marked = isSelected(networkEntities[i].entity);
            int seam = networkEntities[i].kind >= KIND_ROCK && networkEntities[i].kind < KIND_COUNT;
            if (seam && !marked) {
                continue;
            }
            float direction[3];
            relativeTo(networkEntities[i].position, cameraPosition, direction);
            float depth = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2];
            if (depth < 0.15f) {
                continue;
            }
            float angles[2];
            directionAngles(direction, right, up, forward, angles);
            if (angles[0] < -1.3f || angles[0] > 1.3f || angles[1] < -1.3f || angles[1] > 1.3f) {
                continue;
            }
            float bodyRadius = entityScale[i] * 1.35f;
            float boxX = (angleToNdc(angles[0], tanHalfFovY * aspect) * 0.5f + 0.5f) * viewWidth;
            float boxY = (-angleToNdc(angles[1], tanHalfFovY) * 0.5f + 0.5f) * viewHeight;
            float halfX = bodyRadius / (depth * tanHalfFovY * aspect) * 0.5f * viewWidth;
            float halfY = bodyRadius / (depth * tanHalfFovY) * 0.5f * viewHeight;
            float floorSpan = uiUnit(10.0f);
            halfX = halfX < floorSpan ? floorSpan : (halfX > 0.4f * viewWidth ? 0.4f * viewWidth : halfX);
            halfY = halfY < floorSpan ? floorSpan : (halfY > 0.4f * viewHeight ? 0.4f * viewHeight : halfY);
            float bloom = marked ? (1.0f - easeOut(selectionEase)) * 0.30f : 0.0f;
            halfX *= 1.0f + bloom;
            halfY *= 1.0f + bloom;
            float left = floorf(boxX - halfX);
            float rightEdge = floorf(boxX + halfX);
            float top = floorf(boxY - halfY);
            float bottom = floorf(boxY + halfY);
            if (marked) {
                float armX = floorf((rightEdge - left) * 0.32f);
                float armY = floorf((bottom - top) * 0.32f);
                armX = armX < uiUnit(7.0f) ? uiUnit(7.0f) : armX;
                armY = armY < uiUnit(7.0f) ? uiUnit(7.0f) : armY;
                float fade = easeOut(selectionEase);
                Colour arm = colourFade(amber, 0.92f * fade);
                Colour haze = colourFade(amber, 0.30f * fade);
                uiGlow(left, top, rightEdge, bottom, uiUnit(6.0f), uiUnit(22.0f), colourFade(amber, 0.10f * fade));
                float corners[4][2] = {{left, top}, {rightEdge, top}, {left, bottom}, {rightEdge, bottom}};
                for (uint32_t corner = 0; corner < 4; corner++) {
                    float x = corners[corner][0];
                    float y = corners[corner][1];
                    float toX = corner & 1 ? x - armX : x + armX;
                    float toY = corner & 2 ? y - armY : y + armY;
                    uiCapsule(x, y, toX, y, uiUnit(4.5f), 0.0f, haze, colourFade(haze, 0.0f));
                    uiCapsule(x, y, x, toY, uiUnit(4.5f), 0.0f, haze, colourFade(haze, 0.0f));
                    uiCapsule(x, y, toX, y, uiUnit(2.0f), 0.0f, arm, colourFade(arm, 0.15f));
                    uiCapsule(x, y, x, toY, uiUnit(2.0f), 0.0f, arm, colourFade(arm, 0.15f));
                }
            }
            if (!seam) {
                static const uint32_t barTint[3] = {INK_AZURE, INK_AMBER, INK_ROSE};
                uint8_t levels[3] = {networkEntities[i].shield, networkEntities[i].armor, networkEntities[i].hull};
                float barWidth = rightEdge - left;
                float slot = 0.0f;
                for (uint32_t bar = 0; bar < 3; bar++) {
                    if (!levels[bar]) {
                        continue;
                    }
                    float barBottom = top - uiUnit(5.0f) - slot * uiUnit(6.0f);
                    float barTop = barBottom - uiUnit(3.5f);
                    Colour tint = colourOf(barTint[bar], 0.95f);
                    uiFill(left, barTop, rightEdge, barBottom, uiUnit(1.75f), colourOf(INK_VOID, 0.65f));
                    float filled = left + barWidth * (float)levels[bar] / 255.0f;
                    if (filled > left + 1.0f) {
                        uiGradient(left, barTop, filled, barBottom, uiUnit(1.75f), colourLift(tint, 1.25f), tint, 1);
                    }
                    slot += 1.0f;
                }
                if (networkEntities[i].cargo) {
                    float barTop = bottom + uiUnit(4.0f);
                    Colour ore = colourOf(INK_AMBER, 0.9f);
                    uiFill(left, barTop, rightEdge, barTop + uiUnit(3.0f), uiUnit(1.5f), colourOf(INK_VOID, 0.6f));
                    float filled = left + barWidth * (float)networkEntities[i].cargo / 255.0f;
                    if (filled > left + 1.0f) {
                        uiFill(left, barTop, filled, barTop + uiUnit(3.0f), uiUnit(1.5f), ore);
                    }
                }
            }
            int yard = marked ? stationIndexOf(networkEntities[i].entity) : -1;
            if (yard >= 0) {
                float row = top;
                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                    if (!networkStations[yard].store[ore]) {
                        continue;
                    }
                    char amount[16];
                    snprintf(amount, sizeof(amount), "%u", networkStations[yard].store[ore]);
                    float amountWidth = uiTextWidth(FACE_SMALL, amount, 0.0f);
                    float amountLeft = left - uiUnit(15.0f) - amountWidth;
                    uiText(amountLeft, row, FACE_SMALL, amount, 0.0f, colourFade(textTone, 0.92f));
                    Colour tint = colourOf(oreTint[ore], 0.95f);
                    float dotTop = row + (fontLine[FACE_SMALL] - uiUnit(6.0f)) * 0.5f;
                    uiFill(amountLeft - uiUnit(12.0f), dotTop, amountLeft - uiUnit(6.0f), dotTop + uiUnit(6.0f),
                           uiUnit(3.0f), tint);
                    uiGlow(amountLeft - uiUnit(12.0f), dotTop, amountLeft - uiUnit(6.0f), dotTop + uiUnit(6.0f),
                           uiUnit(3.0f), uiUnit(7.0f), colourFade(tint, 0.35f));
                    row += fontLine[FACE_SMALL] + uiUnit(3.0f);
                }
            }
        }
        if (hudReveal > 0.004f && hudButtonCount) {
            const SnapshotStation* yard = activeStation >= 0 ? &networkStations[activeStation] : NULL;
            float panelLeft = hudButtons[0].x0;
            float panelRight = hudButtons[0].x1;
            float panelTop = hudButtons[0].y0;
            float panelBottom = hudButtons[0].y1;
            for (uint32_t i = 1; i < hudButtonCount; i++) {
                panelLeft = hudButtons[i].x0 < panelLeft ? hudButtons[i].x0 : panelLeft;
                panelRight = hudButtons[i].x1 > panelRight ? hudButtons[i].x1 : panelRight;
                panelTop = hudButtons[i].y0 < panelTop ? hudButtons[i].y0 : panelTop;
                panelBottom = hudButtons[i].y1 > panelBottom ? hudButtons[i].y1 : panelBottom;
            }
            panelLeft -= HUD_PAD;
            panelRight += HUD_PAD;
            panelTop -= HUD_PAD + HUD_HEADER;
            panelBottom += HUD_PAD;
            float ease = easeOut(hudReveal);
            uiTransform((panelLeft + panelRight) * 0.5f, panelBottom, 0.98f + 0.02f * ease, (1.0f - ease) * 22.0f, ease);
            uiGlow(panelLeft, panelTop, panelRight, panelBottom, uiUnit(16.0f), uiUnit(30.0f), colourOf(INK_VOID, 0.55f));
            uiGradient(panelLeft, panelTop, panelRight, panelBottom, uiUnit(16.0f),
                       colourOf(INK_RAISED, 0.93f), colourOf(INK_PANEL, 0.95f), 0);
            uiOutline(panelLeft, panelTop, panelRight, panelBottom, uiUnit(16.0f), uiUnit(1.0f), colourOf(INK_LINE, 0.95f));
            uiCapsule(panelLeft + uiUnit(26.0f), panelTop + uiUnit(1.0f), panelRight - uiUnit(26.0f), panelTop + uiUnit(1.0f),
                      uiUnit(2.0f), 0.0f, colourFade(amber, 0.0f), colourFade(amber, 0.55f));
            char caption[64];
            if (yard) {
                snprintf(caption, sizeof(caption), "STATION   %u DOCKED", yard->stored);
            } else {
                snprintf(caption, sizeof(caption), "ENGINEERING");
            }
            float captionLeft = panelLeft + HUD_PAD;
            float captionTop = panelTop + uiUnit(13.0f);
            uiText(captionLeft, captionTop, FACE_SMALL, caption, 2.2f, colourFade(mutedTone, 0.95f));
            if (yard && yard->build < BUILD_FULL) {
                float captionEnd = captionLeft + uiTextWidth(FACE_SMALL, caption, 2.2f);
                float barRight = panelRight - HUD_PAD;
                float barSpan = uiUnit(150.0f);
                float barLeft = barRight - barSpan;
                float wordWidth = uiTextWidth(FACE_SMALL, "ASSEMBLING", 2.2f);
                int roomForWord = barLeft - wordWidth - uiUnit(12.0f) > captionEnd + uiUnit(14.0f);
                if (!roomForWord && barLeft < captionEnd + uiUnit(14.0f)) {
                    barSpan = barRight - captionEnd - uiUnit(14.0f);
                    barSpan = barSpan < uiUnit(40.0f) ? uiUnit(40.0f) : barSpan;
                    barLeft = barRight - barSpan;
                }
                float barTop = panelTop + uiUnit(17.0f);
                uiFill(barLeft, barTop, barRight, barTop + uiUnit(5.0f), uiUnit(2.5f), colourOf(INK_VOID, 0.75f));
                float done = barLeft + barSpan * (float)yard->build / (float)BUILD_FULL;
                uiGradient(barLeft, barTop, done, barTop + uiUnit(5.0f), uiUnit(2.5f), colourLift(azure, 1.3f), azure, 1);
                if (roomForWord) {
                    uiTextRight(barLeft - uiUnit(12.0f), captionTop, FACE_SMALL, "ASSEMBLING", 2.2f, colourFade(azure, 0.9f));
                }
            }
            for (uint32_t i = 0; i < hudButtonCount; i++) {
                const HudButton* button = &hudButtons[i];
                uint32_t action = button->action;
                uint32_t kind = actionKind[action];
                int ready = 1;
                int afford = 1;
                float progress = 0.0f;
                Colour tint = amber;
                if (actionMode[action] == PRODUCE_RESEARCH) {
                    ready = !((networkYards.unlocked >> kind) & 1);
                    tint = teal;
                    if (!ready) {
                        progress = 1.0f;
                    } else if (networkYards.researchTotal && networkYards.researchKind == kind) {
                        progress = (float)networkYards.researchProgress / (float)networkYards.researchTotal;
                    }
                } else if (action == ACTION_LAUNCH) {
                    ready = yard && yard->stored;
                } else if (action == ACTION_RAISE_STATION) {
                    tint = azure;
                    progress = buildPending ? 1.0f : 0.0f;
                } else {
                    ready = (networkYards.unlocked >> kind) & 1;
                    afford = yard && yard->store[0] >= kindCost[kind][0] && yard->store[1] >= kindCost[kind][1] &&
                             yard->store[2] >= kindCost[kind][2];
                    if (yard && yard->queueTotal && yard->queueKind == kind) {
                        progress = (float)yard->queueTicks / (float)yard->queueTotal;
                    }
                }
                float glowHover = hudMotion[action];
                int live = ready && afford;
                float round = uiUnit(12.0f);
                Colour base = colourMix(colourOf(INK_RAISED, 0.92f), colourOf(INK_LINE, 0.95f), glowHover);
                if (!ready) {
                    base = colourOf(INK_PANEL, 0.78f);
                }
                uiGradient(button->x0, button->y0, button->x1, button->y1, round,
                           colourLift(base, 1.15f), base, 0);
                uiOutline(button->x0, button->y0, button->x1, button->y1, round, uiUnit(1.0f),
                          colourFade(live ? colourMix(colourOf(INK_LINE, 1.0f), tint, glowHover)
                                          : colourOf(INK_LINE, 1.0f), 0.55f + 0.45f * glowHover));
                if (glowHover > 0.01f && live) {
                    uiGlow(button->x0, button->y0, button->x1, button->y1, round, uiUnit(20.0f),
                           colourFade(tint, 0.22f * glowHover));
                }
                float iconSpan = HUD_ICON;
                float iconLeft = (button->x0 + button->x1 - iconSpan) * 0.5f;
                float iconTop = button->y0 + uiUnit(6.0f);
                Colour iconTone = live ? (Colour){1.0f + 0.35f * glowHover, 1.0f + 0.35f * glowHover,
                                                  1.0f + 0.35f * glowHover, 1.0f}
                                       : (Colour){0.55f, 0.58f, 0.62f, 0.55f};
                if (action == ACTION_LAUNCH) {
                    float midX = (button->x0 + button->x1) * 0.5f;
                    float baseY = iconTop + iconSpan * 0.74f;
                    float wing = iconSpan * 0.30f;
                    for (uint32_t layer = 0; layer < 3; layer++) {
                        float lift = (float)layer * iconSpan * 0.20f;
                        float alpha = live ? (0.95f - 0.22f * (float)layer) : 0.35f;
                        alpha *= 0.65f + 0.35f * pulseOf(wallSeconds - (float)layer * 0.12f, 1.4f);
                        uiCapsule(midX - wing, baseY - lift, midX, baseY - lift - wing * 0.7f, uiUnit(3.4f), 0.0f,
                                  colourFade(iconTone, alpha), colourFade(iconTone, alpha));
                        uiCapsule(midX, baseY - lift - wing * 0.7f, midX + wing, baseY - lift, uiUnit(3.4f), 0.0f,
                                  colourFade(iconTone, alpha), colourFade(iconTone, alpha));
                    }
                } else {
                    uiIcon(iconLeft, iconTop, iconLeft + iconSpan, iconTop + iconSpan, kind, iconTone);
                }
                char label[32];
                actionLabel(action, label, sizeof(label));
                Colour labelTone = live ? textTone : colourFade(mutedTone, 0.55f);
                float labelTop = button->y1 - uiUnit(26.0f);
                uiTextCentred((button->x0 + button->x1) * 0.5f, labelTop, FACE_SMALL, label, 0.3f, labelTone);
                int showCost = actionMode[action] == PRODUCE_BUILD && action != ACTION_RAISE_STATION;
                if (showCost) {
                    uint32_t shown = 0;
                    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                        shown += kindCost[kind][ore] != 0;
                    }
                    float dotSpan = uiUnit(6.0f);
                    float dotGap = uiUnit(4.0f);
                    float dotX = (button->x0 + button->x1) * 0.5f -
                                 ((float)shown * dotSpan + (float)(shown ? shown - 1 : 0) * dotGap) * 0.5f;
                    float dotTop = button->y1 - uiUnit(13.0f);
                    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                        if (!kindCost[kind][ore]) {
                            continue;
                        }
                        int enough = yard && yard->store[ore] >= kindCost[kind][ore];
                        Colour dot = colourOf(enough ? oreTint[ore] : INK_ROSE, enough ? 0.9f : 0.8f);
                        uiFill(dotX, dotTop, dotX + dotSpan, dotTop + dotSpan, dotSpan * 0.5f, dot);
                        dotX += dotSpan + dotGap;
                    }
                }
                if (progress > 0.0f) {
                    float railLeft = button->x0 + uiUnit(10.0f);
                    float railRight = button->x1 - uiUnit(10.0f);
                    float railTop = button->y1 - uiUnit(6.0f);
                    uiFill(railLeft, railTop, railRight, railTop + uiUnit(3.0f), uiUnit(1.5f), colourOf(INK_VOID, 0.7f));
                    float done = railLeft + (railRight - railLeft) * progress;
                    uiGradient(railLeft, railTop, done, railTop + uiUnit(3.0f), uiUnit(1.5f),
                               colourLift(tint, 1.3f), tint, 1);
                }
                char key[16];
                keyName(settings.actionKey[action], key, sizeof(key));
                float chipWidth = uiTextWidth(FACE_SMALL, key, 0.6f) + uiUnit(12.0f);
                chipWidth = chipWidth < uiUnit(22.0f) ? uiUnit(22.0f) : chipWidth;
                float chipHeight = uiUnit(18.0f);
                float chipLeft = button->x0 + uiUnit(6.0f);
                float chipTop = button->y0 + uiUnit(6.0f);
                uiFill(chipLeft, chipTop, chipLeft + chipWidth, chipTop + chipHeight, uiUnit(5.0f),
                       colourFade(live ? tint : mutedTone, live ? 0.20f + 0.16f * glowHover : 0.10f));
                uiTextCentred(chipLeft + chipWidth * 0.5f, chipTop + (chipHeight - fontLine[FACE_SMALL]) * 0.5f,
                              FACE_SMALL, key, 0.6f, colourFade(live ? tint : mutedTone, live ? 0.98f : 0.55f));
                if (button->header) {
                    float headerRight = button->x1;
                    for (uint32_t slot = i + 1; slot < hudButtonCount && !hudButtons[slot].header; slot++) {
                        headerRight = hudButtons[slot].x1 > headerRight ? hudButtons[slot].x1 : headerRight;
                    }
                    uiText(button->x0 + uiUnit(2.0f), button->y0 - uiUnit(21.0f), FACE_SMALL, button->header,
                           uiUnit(2.4f), colourFade(faintTone, 0.95f));
                    float ruleLeft = button->x0 + uiUnit(2.0f) +
                                     uiTextWidth(FACE_SMALL, button->header, uiUnit(2.4f)) + uiUnit(10.0f);
                    uiCapsule(ruleLeft, button->y0 - uiUnit(13.0f), headerRight, button->y0 - uiUnit(13.0f),
                              uiUnit(1.0f), 0.0f, colourOf(INK_LINE, 0.9f), colourFade(colourOf(INK_LINE, 0.9f), 0.1f));
                }
            }
            uiReset();
        }
        if (menuReveal > 0.004f) {
            float ease = easeOut(menuReveal);
            uiFill(0.0f, 0.0f, viewWidth, viewHeight, 0.0f, colourOf(INK_VOID, 0.62f * ease));
            float left = menuLeft(extent);
            float top = menuTop(extent);
            float right = left + MENU_WIDTH;
            float bottom = top + MENU_HEIGHT;
            float rowLeft = left + MENU_PAD;
            float rowRight = right - MENU_PAD;
            float viewTop = menuContentTop(extent);
            float viewBottom = menuContentBottom(extent);
            uiTransform((left + right) * 0.5f, (top + bottom) * 0.5f, 0.965f + 0.035f * ease, (1.0f - ease) * -18.0f, ease);
            uiGlow(left, top, right, bottom, uiUnit(20.0f), uiUnit(46.0f), colourOf(INK_VOID, 0.65f));
            uiGradient(left, top, right, bottom, uiUnit(20.0f), colourOf(INK_RAISED, 0.98f), colourOf(INK_PANEL, 0.99f), 0);
            uiOutline(left, top, right, bottom, uiUnit(20.0f), uiUnit(1.4f), colourOf(INK_LINE, 1.0f));
            uiCapsule(left + uiUnit(40.0f), top + uiUnit(1.2f), right - uiUnit(40.0f), top + uiUnit(1.2f), uiUnit(2.4f),
                      0.0f, colourFade(amber, 0.0f), colourFade(amber, 0.7f));
            uiText(rowLeft, top + uiUnit(30.0f), FACE_TITLE, "SERENITAS", uiUnit(7.0f), colourFade(textTone, 0.97f));
            uiTextRight(rowRight, top + uiUnit(44.0f), FACE_SMALL, "SETTINGS", uiUnit(3.0f), colourFade(faintTone, 0.95f));
            float tabTop = menuTabTop(extent);
            float tabRule = tabTop + MENU_TAB_HEIGHT + uiUnit(6.0f);
            uiCapsule(rowLeft, tabRule, rowRight, tabRule, uiUnit(1.0f), 0.0f,
                      colourOf(INK_LINE, 0.85f), colourOf(INK_LINE, 0.85f));
            uiCapsule(tabSlide, tabRule, tabSlide + uiUnit(124.0f), tabRule, uiUnit(2.6f), 0.0f,
                      amber, colourFade(teal, 0.85f));
            float scrollLimit = menuScrollLimit(extent, menuTab);
            if (scrollLimit > 0.5f) {
                float trackTop = viewTop + uiUnit(2.0f);
                float trackBottom = viewBottom - uiUnit(2.0f);
                float thumbSpan = (trackBottom - trackTop) * (viewBottom - viewTop) / menuContentSpan(menuTab);
                thumbSpan = thumbSpan < uiUnit(28.0f) ? uiUnit(28.0f) : thumbSpan;
                float thumbTop = trackTop + (trackBottom - trackTop - thumbSpan) * (menuScroll / scrollLimit);
                uiFill(rowRight - uiUnit(3.0f), trackTop, rowRight, trackBottom, uiUnit(1.5f), colourOf(INK_LINE, 0.55f));
                uiFill(rowRight - uiUnit(3.0f), thumbTop, rowRight, thumbTop + thumbSpan, uiUnit(1.5f),
                       colourFade(amber, 0.75f));
            }
            for (uint32_t i = 0; i < menuItemCount; i++) {
                const MenuItem* item = &menuItems[i];
                int clipped = item->kind == MENU_BIND || item->kind == MENU_RESET || item->kind == MENU_PIPELINE;
                if (clipped && !menuItemVisible(extent, item)) {
                    continue;
                }
                VkRect2D clipRect = {{0, 0}, extent};
                if (clipped) {
                    float clipTop = uiMapY(viewTop);
                    float clipBottom = uiMapY(viewBottom);
                    clipTop = clipTop < 0.0f ? 0.0f : clipTop;
                    clipBottom = clipBottom > (float)extent.height ? (float)extent.height : clipBottom;
                    clipRect.offset.y = (int32_t)clipTop;
                    clipRect.extent.height = (uint32_t)(clipBottom > clipTop ? clipBottom - clipTop : 0.0f);
                }
                vkCmdSetScissor(cmd, 0, 1, &clipRect);
                float motion = menuMotion[i];
                float textTop = item->y0 + (item->y1 - item->y0) * 0.5f - fontLine[FACE_BODY] * 0.5f;
                char label[48];
                char value[64];
                switch (item->kind) {
                    case MENU_TAB: {
                        int active = menuTab == item->action;
                        Colour tone = active ? textTone : colourMix(faintTone, textTone, motion);
                        uiTextCentred((item->x0 + item->x1) * 0.5f,
                                      item->y0 + (item->y1 - item->y0 - fontLine[FACE_HEAD]) * 0.5f, FACE_HEAD,
                                      menuTabName[item->action], uiUnit(2.6f), tone);
                        break;
                    }
                    case MENU_PIPELINE: {
                        int active = settings.pathTracing == (int)item->action;
                        Colour tone = active ? colourOf(INK_VOID, 0.95f) : colourMix(mutedTone, textTone, motion);
                        if (active) {
                            uiGradient(item->x0, item->y0, item->x1, item->y1, uiUnit(7.0f),
                                       colourLift(amber, 1.1f), amber, 0);
                        } else {
                            uiFill(item->x0, item->y0, item->x1, item->y1, uiUnit(7.0f),
                                   colourOf(INK_RAISED, 0.55f + 0.35f * motion));
                        }
                        uiTextCentred((item->x0 + item->x1) * 0.5f,
                                      item->y0 + (item->y1 - item->y0 - fontLine[FACE_BODY]) * 0.5f, FACE_BODY,
                                      item->action ? "Path traced" : "Raster", 0.4f, tone);
                        break;
                    }
                    case MENU_BIND: {
                        int listening = rebindAction == (int)item->action;
                        if (motion > 0.01f || listening) {
                            uiFill(item->x0, item->y0, item->x1, item->y1, uiUnit(8.0f),
                                   colourOf(INK_RAISED, 0.35f + 0.45f * (listening ? 1.0f : motion)));
                        }
                        float labelLeft = item->x0 + uiUnit(12.0f);
                        uint32_t badge = actionKind[item->action];
                        if (item->action != ACTION_LAUNCH && iconReady[badge]) {
                            float span = (item->y1 - item->y0) - uiUnit(8.0f);
                            uiIcon(labelLeft, item->y0 + uiUnit(4.0f), labelLeft + span, item->y0 + uiUnit(4.0f) + span,
                                   badge, (Colour){0.85f + 0.35f * motion, 0.85f + 0.35f * motion,
                                                   0.85f + 0.35f * motion, 1.0f});
                            labelLeft += span + uiUnit(10.0f);
                        }
                        actionMenuLabel(item->action, label, sizeof(label));
                        uiText(labelLeft, textTop, FACE_BODY, label, 0.2f, textTone);
                        if (listening) {
                            snprintf(value, sizeof(value), "press a key");
                        } else {
                            keyName(settings.actionKey[item->action], value, sizeof(value));
                        }
                        float chipWidth = uiTextWidth(FACE_SMALL, value, 0.6f) + uiUnit(22.0f);
                        float chipRight = item->x1 - uiUnit(12.0f);
                        float chipHeight = uiUnit(24.0f);
                        float chipTop = item->y0 + (item->y1 - item->y0 - chipHeight) * 0.5f;
                        Colour chipTone = listening ? colourFade(teal, 0.25f + 0.20f * pulseOf(wallSeconds, 1.1f))
                                                    : colourOf(INK_PANEL, 0.9f);
                        uiFill(chipRight - chipWidth, chipTop, chipRight, chipTop + chipHeight, uiUnit(6.0f), chipTone);
                        uiOutline(chipRight - chipWidth, chipTop, chipRight, chipTop + chipHeight, uiUnit(6.0f),
                                  uiUnit(1.0f), colourFade(listening ? teal : colourOf(INK_LINE, 1.0f), 0.9f));
                        uiTextCentred(chipRight - chipWidth * 0.5f, chipTop + (chipHeight - fontLine[FACE_SMALL]) * 0.5f,
                                      FACE_SMALL, value, 0.6f, listening ? teal : colourFade(textTone, 0.92f));
                        break;
                    }
                    case MENU_RESET: {
                        uiFill(item->x0, item->y0, item->x1, item->y1, uiUnit(8.0f),
                               colourOf(INK_RAISED, 0.35f + 0.4f * motion));
                        uiOutline(item->x0, item->y0, item->x1, item->y1, uiUnit(8.0f), uiUnit(1.0f),
                                  colourOf(INK_LINE, 0.9f));
                        uiTextCentred((item->x0 + item->x1) * 0.5f,
                                      item->y0 + (item->y1 - item->y0 - fontLine[FACE_SMALL]) * 0.5f, FACE_SMALL,
                                      "RESTORE DEFAULTS", uiUnit(1.8f), colourMix(mutedTone, textTone, motion));
                        break;
                    }
                    case MENU_RESUME: {
                        uiGradient(item->x0, item->y0, item->x1, item->y1, uiUnit(9.0f),
                                   colourLift(amber, 1.05f + 0.15f * motion), amber, 0);
                        uiGlow(item->x0, item->y0, item->x1, item->y1, uiUnit(9.0f), uiUnit(16.0f),
                               colourFade(amber, 0.30f * motion));
                        uiTextCentred((item->x0 + item->x1) * 0.5f,
                                      item->y0 + (item->y1 - item->y0 - fontLine[FACE_HEAD]) * 0.5f, FACE_HEAD,
                                      "RESUME", uiUnit(2.2f), colourOf(INK_VOID, 0.95f));
                        break;
                    }
                    case MENU_QUIT: {
                        uiFill(item->x0, item->y0, item->x1, item->y1, uiUnit(9.0f),
                               colourFade(rose, 0.10f + 0.22f * motion));
                        uiOutline(item->x0, item->y0, item->x1, item->y1, uiUnit(9.0f), uiUnit(1.2f),
                                  colourFade(rose, 0.55f + 0.4f * motion));
                        uiTextCentred((item->x0 + item->x1) * 0.5f,
                                      item->y0 + (item->y1 - item->y0 - fontLine[FACE_HEAD]) * 0.5f, FACE_HEAD,
                                      "QUIT", uiUnit(2.2f), colourMix(colourFade(rose, 0.9f), textTone, motion * 0.4f));
                        break;
                    }
                    default:
                        break;
                }
            }
            vkCmdSetScissor(cmd, 0, 1, &(VkRect2D){{0, 0}, extent});
            if (menuTab == MENU_TAB_GRAPHICS) {
                float row = menuRowTop(extent, 0);
                uiText(rowLeft, row + (MENU_ROW_HEIGHT - fontLine[FACE_BODY]) * 0.5f, FACE_BODY, "Renderer", 0.2f,
                       textTone);
                static const char* const facts[3] = {"Device", "Surface", "Frame"};
                char values[3][64];
                snprintf(values[0], sizeof(values[0]), "%s", deviceLabel);
                snprintf(values[1], sizeof(values[1]), "%u x %u", extent.width, extent.height);
                snprintf(values[2], sizeof(values[2]), "%.1f ms   %.0f fps", frameSmooth * 1000.0f,
                         frameSmooth > 1.0e-5f ? 1.0f / frameSmooth : 0.0f);
                for (uint32_t fact = 0; fact < 3; fact++) {
                    float factTop = menuRowTop(extent, fact + 2);
                    uiCapsule(rowLeft, factTop, rowRight, factTop, uiUnit(1.0f), 0.0f,
                              colourOf(INK_LINE, 0.55f), colourFade(colourOf(INK_LINE, 0.55f), 0.1f));
                    uiText(rowLeft, factTop + uiUnit(13.0f), FACE_BODY, facts[fact], 0.2f, colourFade(mutedTone, 0.9f));
                    uiTextRight(rowRight, factTop + uiUnit(13.0f), FACE_BODY, values[fact], 0.2f,
                                colourFade(textTone, 0.9f));
                }
                uiText(rowLeft, menuRowTop(extent, 6) + uiUnit(13.0f), FACE_SMALL,
                       "HOLD CTRL FOR THE COMMAND DECK", uiUnit(2.4f), colourFade(faintTone, 0.85f));
            }
            uiReset();
        }
        vkCmdEndRendering(cmd);
        VkImageMemoryBarrier toPresent = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImages[imageIndex], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags waitStage = settings.pathTracing ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &imageAvailable[frameIndex], .pWaitDstStageMask = &waitStage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &renderFinished[imageIndex]
        };
        vkQueueSubmit(queue, 1, &submitInfo, fences[frameIndex]);
        VkPresentInfoKHR presentInfo = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &imageIndex, .waitSemaphoreCount = 1, .pWaitSemaphores = &renderFinished[imageIndex]};
        vkQueuePresentKHR(queue, &presentInfo);
        frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
        frameCounter++;
    }
    vkDeviceWaitIdle(device);
    for (uint32_t i = 0; i < 2; i++) {
        vkDestroyPipeline(device, computePipelines[i], NULL);
    }
    vkDestroyPipelineLayout(device, computePipelineLayout, NULL);
    vkDestroyDescriptorPool(device, descriptorPool, NULL);
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, NULL);
    for (uint32_t i = 0; i < IMAGE_COUNT; i++) {
        vkDestroyImageView(device, imageViews[i], NULL);
        vkDestroyImage(device, images[i], NULL);
        vkFreeMemory(device, imageMemories[i], NULL);
    }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        pfnDestroyAccelerationStructure(device, topLevel[i], NULL);
        vkDestroyBuffer(device, topBuffer[i], NULL);
        vkFreeMemory(device, topMemory[i], NULL);
        vkDestroyBuffer(device, topScratchBuffer[i], NULL);
        vkFreeMemory(device, topScratchMemory[i], NULL);
        vkUnmapMemory(device, instanceMemory[i]);
        vkDestroyBuffer(device, instanceBuffer[i], NULL);
        vkFreeMemory(device, instanceMemory[i], NULL);
    }
    for (uint32_t mesh = 0; mesh < MESH_COUNT; mesh++) {
        pfnDestroyAccelerationStructure(device, bottomLevel[mesh], NULL);
        vkDestroyBuffer(device, bottomBuffer[mesh], NULL);
        vkFreeMemory(device, bottomMemory[mesh], NULL);
    }
    vkDestroyBuffer(device, scratchBuffer, NULL);
    vkFreeMemory(device, scratchMemory, NULL);
    vkDestroyBuffer(device, vertexBuffer, NULL);
    vkFreeMemory(device, vertexMemory, NULL);
    vkUnmapMemory(device, instanceMemoryGraphics);
    vkDestroyBuffer(device, instanceBufferGraphics, NULL);
    vkFreeMemory(device, instanceMemoryGraphics, NULL);
    vkDestroyPipeline(device, overlayPipeline, NULL);
    vkDestroyPipeline(device, backgroundPipeline, NULL);
    vkDestroyPipelineLayout(device, backgroundPipelineLayout, NULL);
    vkDestroyPipelineLayout(device, overlayPipelineLayout, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyImageView(device, depthImageView, NULL);
    vkDestroyImage(device, depthImage, NULL);
    vkFreeMemory(device, depthMemory, NULL);
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(device, fences[i], NULL);
        vkDestroySemaphore(device, imageAvailable[i], NULL);
    }
    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroyImageView(device, swapchainImageViews[i], NULL);
        vkDestroySemaphore(device, renderFinished[i], NULL);
    }
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    DestroyWindow(window);
    UnregisterClassW(WINDOW_TITLE, hInstance);
    return 0;
}
