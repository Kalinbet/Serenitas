#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "debug.h"
#include "protocol.h"

#define CHUNK_SIZE 16384
#define COMMIT_GRANULARITY (1u << 20)
#define RESERVE_GRANULARITY (1u << 16)
#define INVALID 0xFFFFFFFFu

enum {
    COMPONENT_POSITION,
    COMPONENT_VELOCITY,
    COMPONENT_ORIENTATION,
    COMPONENT_DAMAGE,
    COMPONENT_OWNER,
    COMPONENT_KIND,
    COMPONENT_SYSTEM,
    COMPONENT_ORDER,
    COMPONENT_ROUND,
    COMPONENT_CARGO,
    COMPONENT_STATION,
    COMPONENT_COUNT
};

#define PHASE_RUN 0
#define PHASE_ATTACK_BREAK 1
#define PHASE_HAUL 1
#define PHASE_LOAD 2
#define HANGAR_CAP 16

typedef struct {
    double target[3];
    uint64_t targetEntity;
    float radius;
    uint32_t hold;
    uint16_t type;
    uint8_t active;
    uint8_t throttle;
    uint8_t rounds;
    uint8_t phase;
    uint16_t cooldown;
} Order;

typedef struct {
    int32_t hull;
    int32_t armor;
    int32_t shield;
} Damage;

typedef struct {
    int32_t damage;
    uint16_t life;
} Round;

typedef struct {
    uint32_t ore[ORE_COUNT];
} Cargo;

typedef struct {
    uint32_t paid[ORE_COUNT];
    uint16_t owed[ORE_COUNT];
    uint16_t queueKind;
    uint16_t queueTicks;
    uint16_t launchTicks;
    uint16_t pending;
    uint16_t build;
    uint16_t stored;
    uint16_t hangarKind[HANGAR_CAP];
    uint8_t hangarHull[HANGAR_CAP];
    uint8_t hangarArmor[HANGAR_CAP];
    uint8_t hangarShield[HANGAR_CAP];
} Station;

static const uint32_t componentSizes[COMPONENT_COUNT] = {
    sizeof(double) * 3, sizeof(float) * 3, sizeof(int16_t) * 4, sizeof(Damage),
    sizeof(uint64_t), sizeof(uint16_t), sizeof(uint32_t), sizeof(Order), sizeof(Round),
    sizeof(Cargo), sizeof(Station)
};
static const uint32_t componentAligns[COMPONENT_COUNT] = {
    alignof(double), alignof(float), alignof(int16_t), alignof(Damage),
    alignof(uint64_t), alignof(uint16_t), alignof(uint32_t), alignof(Order), alignof(Round),
    alignof(Cargo), alignof(Station)
};

#define ARRIVAL_TIME 0.9
#define CELL_SIZE 512.0
#define ARRIVAL_DISTANCE 4.0
#define ARRIVAL_SPEED 1.5
#define COLLIDER_CAP 16384
#define GRID_SLOTS 8192
#define BREAK_TURNS 1.6
#define ROUND_CAP 4096
#define HARDPOINT_CAP 6
#define GATHER_CAP 8192
#define INTEREST_CELL 40000.0
#define INTEREST_SLOTS 4096

#define MINING_RATE 100
#define BUILD_RANGE 900.0
#define BUILD_RATE 100
#define DOCK_RANGE 600.0
#define HAUL_RANGE 200000.0
#define REPAIR_RANGE 3000.0
#define REPAIR_RATE 0.005
#define HULL_REPAIR_SHARE 0.20
#define ARMOR_REPAIR_SHARE 0.05
#define STATION_CAP 64
#define SPAWN_CAP 256

typedef struct {
    double mass;
    double thrust;
    double maxSpeed;
    double turnRate;
    double radius;
    double sensorRange;
    int32_t maxHull;
    int32_t maxArmor;
    int32_t maxShield;
    int32_t roundDamage;
    double roundSpeed;
    double roundRange;
    uint16_t roundKind;
    uint16_t roundLife;
    uint16_t magazine;
    uint16_t cadence;
    uint16_t reload;
    uint16_t hardpoints;
    uint16_t turret;
    double turretCone;
    double tracking;
    uint32_t hold;
    uint32_t cost[ORE_COUNT];
    uint32_t buildTicks;
    uint32_t researchTicks;
    uint32_t researchCost[ORE_COUNT];
    uint32_t worker;
    double hardpoint[HARDPOINT_CAP][3];
    double hardpointAxis[HARDPOINT_CAP][3];
} KindProfile;

static const KindProfile kindProfiles[KIND_COUNT] = {
    {1.2e6, 7.2e8, 3500.0, 2.40, 25.0, 15000.0, 1800, 1200, 2400, 140, 6000.0, 7000.0, KIND_ROUND, 30, 10, 2, 50, 2, 0, 0.0, 0.0,
     400, {2400, 800, 200}, 140, 6000, {8000, 3000, 1500}, 0,
     {{0.671551, -0.043560, -0.065340}, {0.671551, -0.043560, 0.065340}},
     {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}},
    {1.3e7, 4.6e9, 2200.0, 1.10, 30.0, 40000.0, 4200, 6800, 5300, 260, 6000.0, 7000.0, KIND_ROUND, 30, 8, 3, 70, 2, 0, 0.0, 0.0,
     900, {3600, 1800, 600}, 200, 21600, {16000, 8000, 4000}, 0,
     {{0.239998, 0.077999, -0.799994}, {0.239998, 0.077999, 0.799994}},
     {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}},
    {3.0e7, 1.3e10, 1500.0, 0.55, 35.0, 12000.0, 11000, 14000, 9000, 900, 2500.0, 7000.0, KIND_BOMB, 40, 6, 5, 110, 6, 0, 0.0, 0.0,
     1400, {6400, 3200, 1200}, 300, 54000, {30000, 16000, 9000}, 0,
     {{-0.085025, -0.222956, -0.092583}, {0.103920, -0.222956, -0.092583}, {0.292865, -0.222956, -0.092583},
      {-0.085025, -0.222956, 0.092583}, {0.103920, -0.222956, 0.092583}, {0.292865, -0.222956, 0.092583}},
     {{1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}},
    {4.0e8, 3.2e10, 900.0, 0.14, 180.0, 25000.0, 60000, 48000, 52000, 700, 6000.0, 7000.0, KIND_ROUND, 30, 8, 8, 60, 4, 1, -0.0872, 0.35,
     8000, {32000, 18000, 6000}, 900, 216000, {90000, 60000, 30000}, 0,
     {{0.300439, 0.141383, 0.0}, {-0.618551, 0.171427, 0.0}, {0.265093, -0.141383, 0.0}, {-0.583206, -0.171427, 0.0}},
     {{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, -1.0, 0.0}}},
    {3.0e5, 9.0e7, 1600.0, 1.80, 12.0, 25000.0, 150, 50, 100, 0, 0.0, 0.0, KIND_ROUND, 0, 0, 0, 0, 0, 0, 0.0, 0.0,
     300, {1200, 400, 0}, 100, 0, {0, 0, 0}, 1, {{0.0, 0.0, 0.0}}, {{1.0, 0.0, 0.0}}},
    {4.0e9, 0.0, 0.0, 0.0, 400.0, 60000.0, 120000, 90000, 60000, 0, 0.0, 0.0, KIND_ROUND, 0, 0, 0, 0, 3, 0, 0.0, 0.0,
     500000, {2000, 1000, 1500}, 0, 0, {0, 0, 0}, 0,
     {{0.428556, 0.149994, 0.0}, {-0.214278, 0.149994, 0.371140}, {-0.214278, 0.149994, -0.371140}},
     {{1.0, 0.0, 0.0}, {-0.5, 0.0, 0.866025}, {-0.5, 0.0, -0.866025}}},
    {0.0, 0.0, 0.0, 0.0, 150.0, 0.0, 0, 0, 0, 0, 0.0, 0.0, KIND_ROUND, 0, 0, 0, 0, 0, 0, 0.0, 0.0,
     240000, {0, 0, 0}, 0, 0, {0, 0, 0}, 0, {{0.0, 0.0, 0.0}}, {{1.0, 0.0, 0.0}}},
    {0.0, 0.0, 0.0, 0.0, 120.0, 0.0, 0, 0, 0, 0, 0.0, 0.0, KIND_ROUND, 0, 0, 0, 0, 0, 0, 0.0, 0.0,
     180000, {0, 0, 0}, 0, 0, {0, 0, 0}, 0, {{0.0, 0.0, 0.0}}, {{1.0, 0.0, 0.0}}},
    {0.0, 0.0, 0.0, 0.0, 95.0, 0.0, 0, 0, 0, 0, 0.0, 0.0, KIND_ROUND, 0, 0, 0, 0, 0, 0, 0.0, 0.0,
     120000, {0, 0, 0}, 0, 0, {0, 0, 0}, 0, {{0.0, 0.0, 0.0}}, {{1.0, 0.0, 0.0}}}
};

#define PLAYER_CAP 64
#define LAUNCH_PERIOD 24
#define LAUNCH_SPEED 70.0
#define QUEUE_NONE 0xFFFFu
#define ROCK_CAP 128
#define BELT_ROCKS 28
#define ADOPT_RANGE 2500.0

typedef struct {
    uint64_t id;
    uint32_t unlocked;
    uint32_t active;
    uint32_t progress[KIND_COUNT];
} Player;

static Player players[PLAYER_CAP];
static uint32_t playerCount;

static Player* playerOf(uint64_t id) {
    for (uint32_t i = 0; i < playerCount; i++) {
        if (players[i].id == id) {
            return &players[i];
        }
    }
    if (playerCount == PLAYER_CAP) {
        return NULL;
    }
    Player* player = &players[playerCount++];
    memset(player, 0, sizeof(*player));
    player->id = id;
    player->unlocked = 1u << KIND_DRONE;
    return player;
}

static double separation(const double* from, const double* to) {
    double gap = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        double delta = to[axis] - from[axis];
        gap += delta * delta;
    }
    return sqrt(gap);
}

static uint32_t cargoTotal(const Cargo* hold) {
    uint32_t total = 0;
    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
        total += hold->ore[ore];
    }
    return total;
}

static uint32_t cargoLoad(Cargo* hold, uint32_t ore, uint32_t amount, uint32_t capacity) {
    uint32_t used = cargoTotal(hold);
    uint32_t room = capacity > used ? capacity - used : 0;
    if (amount > room) {
        amount = room;
    }
    hold->ore[ore] += amount;
    return amount;
}

static uint8_t cargoFill(const Cargo* hold, uint32_t capacity) {
    if (!capacity) {
        return 0;
    }
    uint32_t used = cargoTotal(hold);
    return used >= capacity ? 255 : (uint8_t)((uint64_t)used * 255 / capacity);
}

typedef uint64_t Entity;

typedef struct {
    unsigned char* base;
    uint64_t reserved;
    uint64_t committed;
    uint32_t chunkCount;
    uint32_t freeHead;
} ChunkPool;

typedef struct {
    uint32_t* generation;
    uint32_t* row;
    uint16_t* archetype;
    uint64_t generationCommitted;
    uint64_t rowCommitted;
    uint64_t archetypeCommitted;
    uint32_t capacity;
    uint32_t count;
    uint32_t freeHead;
} EntityIndex;

typedef struct {
    uint64_t mask;
    uint32_t offsets[COMPONENT_COUNT];
    uint8_t present[COMPONENT_COUNT];
    uint32_t capacity;
    uint32_t entityCount;
    uint32_t* chunks;
    uint32_t chunkCount;
    uint32_t chunkCapacity;
    uint64_t chunksCommitted;
} Archetype;

typedef struct {
    ChunkPool pool;
    EntityIndex entities;
    Archetype archetypes[64];
    uint32_t archetypeCount;
} World;

static uint64_t roundReserve(uint64_t size) {
    return (size + RESERVE_GRANULARITY - 1) & ~(uint64_t)(RESERVE_GRANULARITY - 1);
}

static void* reserveMemory(uint64_t size) {
    uint64_t rounded = roundReserve(size);
    void* mapped = mmap(NULL, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return mapped == MAP_FAILED ? NULL : mapped;
}

static int commitMemory(void* base, uint64_t offset, uint64_t size) {
    return mprotect((unsigned char*)base + offset, size, PROT_READ | PROT_WRITE) == 0;
}

static int ensureCommitted(void* base, uint64_t needed, uint64_t* committed, uint64_t limit) {
    if (needed <= *committed) {
        return 1;
    }
    if (needed > limit) {
        return 0;
    }
    uint64_t reserved = roundReserve(limit);
    uint64_t target = (needed + COMMIT_GRANULARITY - 1) & ~(uint64_t)(COMMIT_GRANULARITY - 1);
    if (target > reserved) {
        target = reserved;
    }
    if (!commitMemory(base, *committed, target - *committed)) {
        return 0;
    }
    *committed = target;
    return 1;
}

static uint32_t entityIndexOf(Entity entity) {
    return (uint32_t)entity;
}

static uint32_t entityGenerationOf(Entity entity) {
    return (uint32_t)(entity >> 32);
}

static Entity entityMake(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << 32) | (uint64_t)index;
}

static uint32_t entityCompact(Entity entity) {
    return ((entityGenerationOf(entity) & 0xFFu) << 24) | (entityIndexOf(entity) & 0xFFFFFFu);
}

static uint32_t chunkAllocate(ChunkPool* pool) {
    if (pool->freeHead != INVALID) {
        uint32_t index = pool->freeHead;
        memcpy(&pool->freeHead, pool->base + (uint64_t)index * CHUNK_SIZE, sizeof(uint32_t));
        return index;
    }
    uint64_t needed = ((uint64_t)pool->chunkCount + 1) * CHUNK_SIZE;
    if (!ensureCommitted(pool->base, needed, &pool->committed, pool->reserved)) {
        return INVALID;
    }
    return pool->chunkCount++;
}

static void chunkRelease(ChunkPool* pool, uint32_t index) {
    memcpy(pool->base + (uint64_t)index * CHUNK_SIZE, &pool->freeHead, sizeof(uint32_t));
    pool->freeHead = index;
}

static unsigned char* chunkAt(ChunkPool* pool, uint32_t index) {
    return pool->base + (uint64_t)index * CHUNK_SIZE;
}

static int worldCreate(World* world, uint32_t maxEntities, uint64_t poolBytes) {
    memset(world, 0, sizeof(*world));
    world->pool.reserved = poolBytes;
    world->pool.base = reserveMemory(poolBytes);
    world->pool.freeHead = INVALID;
    world->entities.capacity = maxEntities;
    world->entities.freeHead = INVALID;
    world->entities.generation = reserveMemory((uint64_t)maxEntities * sizeof(uint32_t));
    world->entities.row = reserveMemory((uint64_t)maxEntities * sizeof(uint32_t));
    world->entities.archetype = reserveMemory((uint64_t)maxEntities * sizeof(uint16_t));
    return world->pool.base && world->entities.generation && world->entities.row && world->entities.archetype;
}

static void archetypeLayout(Archetype* archetype) {
    uint32_t order[COMPONENT_COUNT];
    uint32_t ordered = 0;
    for (uint32_t align = 8; align >= 1; align >>= 1) {
        for (uint32_t component = 0; component < COMPONENT_COUNT; component++) {
            if ((archetype->mask & (1ull << component)) && componentAligns[component] == align) {
                order[ordered++] = component;
            }
        }
    }
    uint32_t perEntity = sizeof(uint32_t);
    for (uint32_t i = 0; i < ordered; i++) {
        perEntity += componentSizes[order[i]];
    }
    uint32_t capacity = CHUNK_SIZE / perEntity;
    while (capacity > 0) {
        uint64_t offset = (uint64_t)capacity * sizeof(uint32_t);
        for (uint32_t i = 0; i < ordered; i++) {
            uint32_t component = order[i];
            uint32_t align = componentAligns[component];
            offset = (offset + align - 1) & ~(uint64_t)(align - 1);
            archetype->offsets[component] = (uint32_t)offset;
            offset += (uint64_t)capacity * componentSizes[component];
        }
        if (offset <= CHUNK_SIZE) {
            break;
        }
        capacity--;
    }
    archetype->capacity = capacity;
    for (uint32_t component = 0; component < COMPONENT_COUNT; component++) {
        archetype->present[component] = (archetype->mask & (1ull << component)) ? 1 : 0;
    }
}

static uint32_t archetypeFind(World* world, uint64_t mask) {
    for (uint32_t i = 0; i < world->archetypeCount; i++) {
        if (world->archetypes[i].mask == mask) {
            return i;
        }
    }
    if (world->archetypeCount == 64) {
        return INVALID;
    }
    uint32_t index = world->archetypeCount++;
    Archetype* archetype = &world->archetypes[index];
    memset(archetype, 0, sizeof(*archetype));
    archetype->mask = mask;
    archetypeLayout(archetype);
    archetype->chunkCapacity = 1u << 20;
    archetype->chunks = reserveMemory((uint64_t)archetype->chunkCapacity * sizeof(uint32_t));
    if (!archetype->chunks || !archetype->capacity) {
        world->archetypeCount--;
        return INVALID;
    }
    return index;
}

static uint32_t* chunkEntities(World* world, Archetype* archetype, uint32_t slot) {
    return (uint32_t*)chunkAt(&world->pool, archetype->chunks[slot]);
}

static void* chunkColumn(World* world, Archetype* archetype, uint32_t slot, uint32_t component) {
    return chunkAt(&world->pool, archetype->chunks[slot]) + archetype->offsets[component];
}

static uint32_t chunkFill(Archetype* archetype, uint32_t slot) {
    uint32_t full = archetype->entityCount / archetype->capacity;
    return slot < full ? archetype->capacity : archetype->entityCount - full * archetype->capacity;
}

static uint32_t archetypeAppend(World* world, Archetype* archetype, uint32_t entityIndex) {
    uint32_t slot = archetype->entityCount / archetype->capacity;
    if (slot == archetype->chunkCount) {
        if (archetype->chunkCount == archetype->chunkCapacity) {
            return INVALID;
        }
        if (!ensureCommitted(archetype->chunks, ((uint64_t)archetype->chunkCount + 1) * sizeof(uint32_t), &archetype->chunksCommitted, (uint64_t)archetype->chunkCapacity * sizeof(uint32_t))) {
            return INVALID;
        }
        uint32_t chunk = chunkAllocate(&world->pool);
        if (chunk == INVALID) {
            return INVALID;
        }
        archetype->chunks[archetype->chunkCount++] = chunk;
    }
    uint32_t row = archetype->entityCount++;
    chunkEntities(world, archetype, row / archetype->capacity)[row % archetype->capacity] = entityIndex;
    return row;
}

static void* rowPointer(World* world, Archetype* archetype, uint32_t row, uint32_t component) {
    return (unsigned char*)chunkColumn(world, archetype, row / archetype->capacity, component) +
           (uint64_t)(row % archetype->capacity) * componentSizes[component];
}

static void archetypeRemove(World* world, Archetype* archetype, uint32_t row) {
    uint32_t last = archetype->entityCount - 1;
    if (row != last) {
        uint32_t movedIndex = chunkEntities(world, archetype, last / archetype->capacity)[last % archetype->capacity];
        chunkEntities(world, archetype, row / archetype->capacity)[row % archetype->capacity] = movedIndex;
        for (uint32_t component = 0; component < COMPONENT_COUNT; component++) {
            if (archetype->present[component]) {
                memcpy(rowPointer(world, archetype, row, component), rowPointer(world, archetype, last, component), componentSizes[component]);
            }
        }
        world->entities.row[movedIndex] = row;
    }
    archetype->entityCount = last;
    if (last % archetype->capacity == 0 && archetype->chunkCount > 0 && last / archetype->capacity == archetype->chunkCount - 1) {
        chunkRelease(&world->pool, archetype->chunks[--archetype->chunkCount]);
    }
}

static Entity entityCreate(World* world, uint64_t mask) {
    uint32_t archetypeIndex = archetypeFind(world, mask);
    if (archetypeIndex == INVALID) {
        return 0;
    }
    Archetype* archetype = &world->archetypes[archetypeIndex];
    EntityIndex* index = &world->entities;
    uint32_t entityIndex;
    if (index->freeHead != INVALID) {
        entityIndex = index->freeHead;
        index->freeHead = index->row[entityIndex];
    } else {
        if (index->count == index->capacity) {
            return 0;
        }
        entityIndex = index->count++;
        if (!ensureCommitted(index->generation, (uint64_t)index->count * sizeof(uint32_t), &index->generationCommitted, (uint64_t)index->capacity * sizeof(uint32_t)) ||
            !ensureCommitted(index->row, (uint64_t)index->count * sizeof(uint32_t), &index->rowCommitted, (uint64_t)index->capacity * sizeof(uint32_t)) ||
            !ensureCommitted(index->archetype, (uint64_t)index->count * sizeof(uint16_t), &index->archetypeCommitted, (uint64_t)index->capacity * sizeof(uint16_t))) {
            index->count--;
            return 0;
        }
        index->generation[entityIndex] = 1;
    }
    uint32_t row = archetypeAppend(world, archetype, entityIndex);
    if (row == INVALID) {
        index->row[entityIndex] = index->freeHead;
        index->freeHead = entityIndex;
        return 0;
    }
    index->row[entityIndex] = row;
    index->archetype[entityIndex] = (uint16_t)archetypeIndex;
    return entityMake(entityIndex, index->generation[entityIndex]);
}

static int entityAlive(World* world, Entity entity) {
    uint32_t entityIndex = entityIndexOf(entity);
    return entityIndex < world->entities.count &&
           world->entities.generation[entityIndex] == entityGenerationOf(entity) &&
           world->entities.archetype[entityIndex] != 0xFFFF;
}

static void entityDestroy(World* world, Entity entity) {
    if (!entityAlive(world, entity)) {
        return;
    }
    EntityIndex* index = &world->entities;
    uint32_t entityIndex = entityIndexOf(entity);
    Archetype* archetype = &world->archetypes[index->archetype[entityIndex]];
    archetypeRemove(world, archetype, index->row[entityIndex]);
    index->generation[entityIndex]++;
    index->archetype[entityIndex] = 0xFFFF;
    index->row[entityIndex] = index->freeHead;
    index->freeHead = entityIndex;
}

static int entityMove(World* world, Entity entity, uint64_t mask) {
    if (!entityAlive(world, entity)) {
        return 0;
    }
    uint32_t entityIndex = entityIndexOf(entity);
    uint32_t sourceIndex = world->entities.archetype[entityIndex];
    if (world->archetypes[sourceIndex].mask == mask) {
        return 1;
    }
    uint32_t targetIndex = archetypeFind(world, mask);
    if (targetIndex == INVALID) {
        return 0;
    }
    Archetype* source = &world->archetypes[sourceIndex];
    Archetype* target = &world->archetypes[targetIndex];
    uint32_t sourceRow = world->entities.row[entityIndex];
    uint32_t targetRow = archetypeAppend(world, target, entityIndex);
    if (targetRow == INVALID) {
        return 0;
    }
    for (uint32_t component = 0; component < COMPONENT_COUNT; component++) {
        if (!target->present[component]) {
            continue;
        }
        void* destination = rowPointer(world, target, targetRow, component);
        if (source->present[component]) {
            memcpy(destination, rowPointer(world, source, sourceRow, component), componentSizes[component]);
        } else {
            memset(destination, 0, componentSizes[component]);
        }
    }
    archetypeRemove(world, source, sourceRow);
    world->entities.archetype[entityIndex] = (uint16_t)targetIndex;
    world->entities.row[entityIndex] = targetRow;
    return 1;
}

static int componentAdd(World* world, Entity entity, uint32_t component) {
    if (!entityAlive(world, entity)) {
        return 0;
    }
    uint64_t mask = world->archetypes[world->entities.archetype[entityIndexOf(entity)]].mask;
    return entityMove(world, entity, mask | (1ull << component));
}

static int componentRemove(World* world, Entity entity, uint32_t component) {
    if (!entityAlive(world, entity)) {
        return 0;
    }
    uint64_t mask = world->archetypes[world->entities.archetype[entityIndexOf(entity)]].mask;
    return entityMove(world, entity, mask & ~(1ull << component));
}

static void* componentOf(World* world, Entity entity, uint32_t component) {
    if (!entityAlive(world, entity)) {
        return NULL;
    }
    uint32_t entityIndex = entityIndexOf(entity);
    Archetype* archetype = &world->archetypes[world->entities.archetype[entityIndex]];
    if (!archetype->present[component]) {
        return NULL;
    }
    uint32_t row = world->entities.row[entityIndex];
    return (unsigned char*)chunkColumn(world, archetype, row / archetype->capacity, component) +
           (uint64_t)(row % archetype->capacity) * componentSizes[component];
}

#if defined(SERENITAS_DEBUG) || defined(SERENITAS_TESTS)
static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}
#endif

#define SESSION_SLOTS 4096
#define SESSION_TIMEOUT_TICKS 200
#define SLOT_EMPTY 0
#define SLOT_ACTIVE 1
#define SLOT_TOMBSTONE 2

#define SNAPSHOT_PER_PACKET 36
#define SNAPSHOT_PACKETS_PER_TICK 8
#define SHOTS_PER_PACKET 28
#define SHOT_PACKETS_PER_TICK 4
#define CELESTIAL_PERIOD 20

typedef struct {
    uint64_t entity;
    double position[3];
    int16_t orientation[4];
    uint32_t owner;
    uint32_t target;
    uint8_t kind;
    uint8_t hull;
    uint8_t armor;
    uint8_t shield;
    uint8_t throttle;
    uint8_t cargo;
} GatheredEntity;

static uint8_t healthFraction(int32_t value, int32_t maximum) {
    if (maximum <= 0 || value <= 0) {
        return 0;
    }
    if (value >= maximum) {
        return 255;
    }
    return (uint8_t)((value * 255) / maximum);
}

typedef struct {
    uint32_t address;
    uint16_t port;
    uint16_t state;
    uint64_t player;
    uint32_t system;
    uint32_t subscribeSequence;
    uint64_t lastTick;
    uint64_t celestialTick;
} Session;

static Session sessions[SESSION_SLOTS];
static uint32_t sessionCount;

static uint32_t sessionHash(uint32_t address, uint16_t port) {
    uint32_t hash = address * 2654435761u + (uint32_t)port * 40503u;
    hash ^= hash >> 15;
    return hash & (SESSION_SLOTS - 1);
}

static Session* sessionFind(uint32_t address, uint16_t port) {
    uint32_t slot = sessionHash(address, port);
    for (uint32_t probe = 0; probe < SESSION_SLOTS; probe++) {
        Session* session = &sessions[(slot + probe) & (SESSION_SLOTS - 1)];
        if (session->state == SLOT_EMPTY) {
            return NULL;
        }
        if (session->state == SLOT_ACTIVE && session->address == address && session->port == port) {
            return session;
        }
    }
    return NULL;
}

static Session* sessionInsert(uint32_t address, uint16_t port) {
    uint32_t slot = sessionHash(address, port);
    Session* reuse = NULL;
    for (uint32_t probe = 0; probe < SESSION_SLOTS; probe++) {
        Session* session = &sessions[(slot + probe) & (SESSION_SLOTS - 1)];
        if (session->state == SLOT_ACTIVE && session->address == address && session->port == port) {
            return session;
        }
        if (session->state == SLOT_TOMBSTONE && !reuse) {
            reuse = session;
        }
        if (session->state == SLOT_EMPTY) {
            Session* target = reuse ? reuse : session;
            memset(target, 0, sizeof(*target));
            target->address = address;
            target->port = port;
            target->state = SLOT_ACTIVE;
            sessionCount++;
            return target;
        }
    }
    return NULL;
}

static void sessionRelease(Session* session) {
    memset(session, 0, sizeof(*session));
    session->state = SLOT_TOMBSTONE;
    sessionCount--;
}

static int setNonBlocking(int descriptor) {
    int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void sendPacket(int socketDescriptor, uint32_t address, uint16_t port, uint16_t type, uint32_t sequence, const void* body, uint16_t bodySize) {
    unsigned char buffer[PACKET_CAP];
    PacketHeader header = {.magic = PROTOCOL_MAGIC, .type = type, .length = bodySize, .sequence = sequence};
    memcpy(buffer, &header, sizeof(header));
    if (bodySize) {
        memcpy(buffer + sizeof(header), body, bodySize);
    }
    struct sockaddr_in destination = {.sin_family = AF_INET, .sin_port = port, .sin_addr = {.s_addr = address}};
    sendto(socketDescriptor, buffer, sizeof(header) + bodySize, 0, (struct sockaddr*)&destination, sizeof(destination));
}

static uint64_t shipMask(void) {
    return (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY) | (1ull << COMPONENT_ORIENTATION) |
           (1ull << COMPONENT_DAMAGE) | (1ull << COMPONENT_OWNER) | (1ull << COMPONENT_KIND) |
           (1ull << COMPONENT_SYSTEM) | (1ull << COMPONENT_CARGO);
}

static uint64_t stationMask(void) {
    return shipMask() | (1ull << COMPONENT_STATION);
}

static uint64_t rockMask(void) {
    return (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY) | (1ull << COMPONENT_ORIENTATION) |
           (1ull << COMPONENT_KIND) | (1ull << COMPONENT_SYSTEM) | (1ull << COMPONENT_CARGO);
}

static uint64_t roundMask(void) {
    return (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY) | (1ull << COMPONENT_ORIENTATION) |
           (1ull << COMPONENT_OWNER) | (1ull << COMPONENT_KIND) | (1ull << COMPONENT_SYSTEM) | (1ull << COMPONENT_ROUND);
}

static uint64_t nextPlayer = 1;

static World* commandWorld;

static const double beltCentre[3] = {
    0.80 * ASTRONOMICAL_UNIT + 14000.0, 0.0, 0.31 * ASTRONOMICAL_UNIT - 34600.0
};

static void seedRocks(World* world, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        Entity rock = entityCreate(world, rockMask());
        if (!rock) {
            return;
        }
        double angle = 6.2831853 * (double)i / (double)count;
        double sweep = 4500.0 + 1400.0 * cos(angle * 5.0);
        uint32_t ore = (i % 7) == 6 ? 2 : ((i % 7) >= 4 ? 1 : 0);
        uint16_t kind = (uint16_t)(KIND_ROCK + ore);
        double* position = componentOf(world, rock, COMPONENT_POSITION);
        float* velocity = componentOf(world, rock, COMPONENT_VELOCITY);
        int16_t* orientation = componentOf(world, rock, COMPONENT_ORIENTATION);
        uint16_t* rockKind = componentOf(world, rock, COMPONENT_KIND);
        uint32_t* system = componentOf(world, rock, COMPONENT_SYSTEM);
        Cargo* seam = componentOf(world, rock, COMPONENT_CARGO);
        position[0] = beltCentre[0] + sweep * cos(angle);
        position[1] = beltCentre[1] + 700.0 * sin(angle * 3.0);
        position[2] = beltCentre[2] + sweep * sin(angle);
        memset(velocity, 0, sizeof(float) * 3);
        double axis[3] = {sin(angle * 2.3), cos(angle * 1.7), sin(angle * 0.9) + 0.4};
        double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        double half = angle * 1.3;
        for (uint32_t component = 0; component < 3; component++) {
            orientation[component] = (int16_t)(axis[component] / length * sin(half) * 32767.0);
        }
        orientation[3] = (int16_t)(cos(half) * 32767.0);
        *rockKind = kind;
        *system = 0;
        memset(seam, 0, sizeof(*seam));
        seam->ore[ore] = kindProfiles[kind].hold;
    }
}

static Entity seedWorker(World* world, uint64_t player) {
    Entity drone = entityCreate(world, shipMask());
    if (!drone) {
        return 0;
    }
    const KindProfile* type = &kindProfiles[KIND_DRONE];
    double* position = componentOf(world, drone, COMPONENT_POSITION);
    float* velocity = componentOf(world, drone, COMPONENT_VELOCITY);
    int16_t* orientation = componentOf(world, drone, COMPONENT_ORIENTATION);
    Damage* damage = componentOf(world, drone, COMPONENT_DAMAGE);
    uint64_t* owner = componentOf(world, drone, COMPONENT_OWNER);
    uint16_t* kind = componentOf(world, drone, COMPONENT_KIND);
    uint32_t* system = componentOf(world, drone, COMPONENT_SYSTEM);
    Cargo* hold = componentOf(world, drone, COMPONENT_CARGO);
    position[0] = beltCentre[0] + 2600.0 + 700.0 * (double)(player % 4);
    position[1] = beltCentre[1] + 260.0;
    position[2] = beltCentre[2] - 2200.0;
    memset(velocity, 0, sizeof(float) * 3);
    orientation[0] = 0;
    orientation[1] = 0;
    orientation[2] = 0;
    orientation[3] = 32767;
    damage->hull = type->maxHull;
    damage->armor = type->maxArmor;
    damage->shield = type->maxShield;
    *owner = player;
    *kind = KIND_DRONE;
    *system = 0;
    memset(hold, 0, sizeof(*hold));
    return drone;
}

static void formationSlot(uint32_t index, uint32_t total, double spacing, double* offset) {
    uint32_t columns = 1;
    while (columns * columns * columns < total) {
        columns++;
    }
    uint32_t rows = 1;
    while (columns * rows * rows < total) {
        rows++;
    }
    uint32_t layers = (total + columns * rows - 1) / (columns * rows);
    uint32_t x = index % columns;
    uint32_t y = (index / columns) % rows;
    uint32_t z = index / (columns * rows);
    offset[0] = ((double)x - (double)(columns - 1) * 0.5) * spacing;
    offset[1] = ((double)y - (double)(rows - 1) * 0.5) * spacing;
    offset[2] = ((double)z - (double)(layers - 1) * 0.5) * spacing;
}

static Entity entityExpand(World* world, uint32_t compact) {
    uint32_t index = compact & 0xFFFFFFu;
    if (index >= world->entities.capacity) {
        return 0;
    }
    uint32_t generation = world->entities.generation[index];
    if ((generation & 0xFFu) != (compact >> 24)) {
        return 0;
    }
    return entityMake(index, generation);
}

static void applyCommand(Session* session, unsigned char* body, uint32_t bodySize) {
    if (bodySize < COMMAND_HEADER) {
        return;
    }
    double target[3];
    uint32_t targetCompact;
    uint32_t count;
    uint32_t type;
    float radius;
    memcpy(target, body, sizeof(target));
    memcpy(&targetCompact, body + 24, sizeof(targetCompact));
    memcpy(&count, body + 32, sizeof(count));
    memcpy(&type, body + 36, sizeof(type));
    memcpy(&radius, body + 40, sizeof(radius));
    if (bodySize < COMMAND_HEADER + count * sizeof(uint32_t)) {
        return;
    }
    Entity targetEntity = entityExpand(commandWorld, targetCompact);
    double spacing = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t compact;
        memcpy(&compact, body + COMMAND_HEADER + i * sizeof(uint32_t), sizeof(compact));
        Entity handle = entityExpand(commandWorld, compact);
        uint16_t* kind = componentOf(commandWorld, handle, COMPONENT_KIND);
        if (kind) {
            double candidate = kindProfiles[*kind % KIND_COUNT].radius * 2.6;
            if (candidate > spacing) {
                spacing = candidate;
            }
        }
    }
    uint32_t placed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t compact;
        memcpy(&compact, body + COMMAND_HEADER + i * sizeof(uint32_t), sizeof(compact));
        Entity handle = entityExpand(commandWorld, compact);
        uint64_t* owner = componentOf(commandWorld, handle, COMPONENT_OWNER);
        if (!owner || *owner != session->player) {
            continue;
        }
        uint16_t* kind = componentOf(commandWorld, handle, COMPONENT_KIND);
        const KindProfile* profile = &kindProfiles[kind ? *kind % KIND_COUNT : 0];
        if (profile->thrust <= 0.0 ||
            ((type == ORDER_MINE || type == ORDER_BUILD) && !profile->worker)) {
            continue;
        }
        uint16_t resolved = (uint16_t)(type == ORDER_ATTACK && (profile->turret || !profile->magazine) ? ORDER_ORBIT : type);
        Order* current = componentOf(commandWorld, handle, COMPONENT_ORDER);
        uint8_t rounds = current ? current->rounds : (uint8_t)profile->magazine;
        uint8_t phase = current && current->type == resolved && current->targetEntity == targetEntity ? current->phase : PHASE_RUN;
        uint16_t cooldown = current ? current->cooldown : 0;
        if (!componentAdd(commandWorld, handle, COMPONENT_ORDER)) {
            continue;
        }
        Order* order = componentOf(commandWorld, handle, COMPONENT_ORDER);
        if (!order) {
            continue;
        }
        order->type = resolved;
        order->targetEntity = targetEntity;
        order->radius = radius;
        order->active = 1;
        order->hold = 0;
        order->throttle = 0;
        order->rounds = rounds;
        order->phase = phase;
        order->cooldown = cooldown;
        if (type != ORDER_MOVE) {
            order->target[0] = target[0];
            order->target[1] = target[1];
            order->target[2] = target[2];
        } else {
            double offset[3];
            formationSlot(placed, count, spacing, offset);
            order->target[0] = target[0] + offset[0];
            order->target[1] = target[1] + offset[1];
            order->target[2] = target[2] + offset[2];
        }
        placed++;
    }
}

static void applyProduce(Session* session, unsigned char* body, uint32_t bodySize) {
    if (bodySize < 8) {
        return;
    }
    uint32_t compact;
    uint16_t kind;
    uint16_t mode;
    memcpy(&compact, body, sizeof(compact));
    memcpy(&kind, body + 4, sizeof(kind));
    memcpy(&mode, body + 6, sizeof(mode));
    Entity handle = entityExpand(commandWorld, compact);
    uint64_t* owner = componentOf(commandWorld, handle, COMPONENT_OWNER);
    Station* state = componentOf(commandWorld, handle, COMPONENT_STATION);
    Cargo* store = componentOf(commandWorld, handle, COMPONENT_CARGO);
    double* place = componentOf(commandWorld, handle, COMPONENT_POSITION);
    uint32_t* system = componentOf(commandWorld, handle, COMPONENT_SYSTEM);
    if (!owner || !state || !store || !place || !system || *owner != session->player || state->build < BUILD_FULL) {
        return;
    }
    Player* player = playerOf(session->player);
    if (!player) {
        return;
    }
    if (mode == PRODUCE_LAUNCH) {
        state->pending = state->stored;
        return;
    }
    if (kind >= KIND_COUNT) {
        return;
    }
    const KindProfile* type = &kindProfiles[kind];
    if (mode == PRODUCE_RESEARCH) {
        if (!type->researchTicks || (player->unlocked & (1u << kind))) {
            return;
        }
        player->active = kind + 1;
        return;
    }
    if (state->queueKind != QUEUE_NONE || !type->buildTicks || !(player->unlocked & (1u << kind))) {
        return;
    }
    state->queueKind = kind;
    state->queueTicks = 0;
    memset(state->paid, 0, sizeof(state->paid));
}

static void handlePacket(int socketDescriptor, struct sockaddr_in* from, unsigned char* data, uint32_t size, uint64_t currentTick) {
    if (size < sizeof(PacketHeader)) {
        return;
    }
    PacketHeader header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != PROTOCOL_MAGIC || header.length + sizeof(header) > size) {
        return;
    }
    uint32_t address = from->sin_addr.s_addr;
    uint16_t port = from->sin_port;
    unsigned char* body = data + sizeof(header);

    if (header.type == PACKET_HELLO) {
        Session* session = sessionInsert(address, port);
        if (!session) {
            return;
        }
        if (!session->player) {
            session->player = nextPlayer++;
            session->system = 0;
            seedWorker(commandWorld, session->player);
        }
        session->lastTick = currentTick;
        PacketWelcome welcome = {session->player, TICK_NANOSECONDS, session->system};
        sendPacket(socketDescriptor, address, port, PACKET_WELCOME, 0, &welcome, sizeof(welcome));
        return;
    }

    Session* session = sessionFind(address, port);
    if (!session) {
        return;
    }
    session->lastTick = currentTick;

    if (header.type == PACKET_SUBSCRIBE && header.length >= sizeof(uint32_t)) {
        if ((int32_t)(header.sequence - session->subscribeSequence) <= 0) {
            return;
        }
        session->subscribeSequence = header.sequence;
        memcpy(&session->system, body, sizeof(uint32_t));
    } else if (header.type == PACKET_COMMAND) {
        applyCommand(session, body, header.length);
    } else if (header.type == PACKET_PRODUCE) {
        applyProduce(session, body, header.length);
    } else if (header.type == PACKET_BYE) {
        sessionRelease(session);
    }
}

static const double boundsMinimum[3] = {-3.0 * ASTRONOMICAL_UNIT, -0.5 * ASTRONOMICAL_UNIT, -3.0 * ASTRONOMICAL_UNIT};
static const double boundsMaximum[3] = {3.0 * ASTRONOMICAL_UNIT, 0.5 * ASTRONOMICAL_UNIT, 3.0 * ASTRONOMICAL_UNIT};

static Entity completedOrders[8192];
static uint32_t completedCount;
static Entity destroyedShips[1024];
static uint32_t destroyedCount;
static Entity spentRounds[ROUND_CAP];
static uint32_t spentCount;

typedef struct {
    double position[3];
    float velocity[3];
    int16_t orientation[4];
    uint64_t owner;
    uint32_t system;
    int32_t damage;
    uint16_t kind;
    uint16_t life;
} Shot;

static Shot pendingShots[ROUND_CAP];
static uint32_t pendingCount;

static double* colliderPosition[COLLIDER_CAP];
static float* colliderVelocity[COLLIDER_CAP];
static Entity colliderHandle[COLLIDER_CAP];
static double colliderRadius[COLLIDER_CAP];
static uint64_t colliderOwner[COLLIDER_CAP];
static Damage* colliderDamage[COLLIDER_CAP];
static uint8_t colliderOwned[COLLIDER_CAP];
static uint8_t colliderMobile[COLLIDER_CAP];
static double colliderCorrection[COLLIDER_CAP][3];
static double colliderImpulse[COLLIDER_CAP][3];
static uint32_t colliderNext[COLLIDER_CAP];
static uint32_t gridHead[GRID_SLOTS];
static uint32_t colliderCount;
static uint32_t collisionPairs;

static uint32_t cellHash(int32_t x, int32_t y, int32_t z) {
    uint32_t hash = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    hash ^= hash >> 13;
    return hash & (GRID_SLOTS - 1);
}

static int32_t cellOf(double value) {
    return (int32_t)floor(value / CELL_SIZE);
}

static void separate(uint32_t a, uint32_t b) {
    double delta[3];
    double lengthSquared = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        delta[axis] = colliderPosition[b][axis] - colliderPosition[a][axis];
        lengthSquared += delta[axis] * delta[axis];
    }
    double contact = colliderRadius[a] + colliderRadius[b];
    if (lengthSquared >= contact * contact) {
        return;
    }
    double distance = sqrt(lengthSquared);
    double direction[3];
    if (distance < 1e-9) {
        direction[0] = ((a + b) & 1) ? 1.0 : 0.0;
        direction[1] = ((a + b) & 2) ? 1.0 : 0.0;
        direction[2] = direction[0] == 0.0 && direction[1] == 0.0 ? 1.0 : 0.0;
        distance = 0.0;
    } else {
        for (uint32_t axis = 0; axis < 3; axis++) {
            direction[axis] = delta[axis] / distance;
        }
    }
    double overlap = contact - distance;
    double shareA = colliderMobile[a] ? (colliderMobile[b] ? 0.5 : 1.0) : 0.0;
    double shareB = colliderMobile[b] ? (colliderMobile[a] ? 0.5 : 1.0) : 0.0;
    double approach = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        colliderCorrection[a][axis] -= direction[axis] * overlap * shareA;
        colliderCorrection[b][axis] += direction[axis] * overlap * shareB;
        approach += ((double)colliderVelocity[b][axis] - (double)colliderVelocity[a][axis]) * direction[axis];
    }
    if (approach < 0.0) {
        for (uint32_t axis = 0; axis < 3; axis++) {
            colliderImpulse[a][axis] += direction[axis] * approach * shareA;
            colliderImpulse[b][axis] -= direction[axis] * approach * shareB;
        }
    }
    collisionPairs++;
}

static void applyCorrections(void) {
    for (uint32_t i = 0; i < colliderCount; i++) {
        double length = sqrt(colliderCorrection[i][0] * colliderCorrection[i][0] +
                             colliderCorrection[i][1] * colliderCorrection[i][1] +
                             colliderCorrection[i][2] * colliderCorrection[i][2]);
        double scale = length > colliderRadius[i] ? colliderRadius[i] / length : 1.0;
        for (uint32_t axis = 0; axis < 3; axis++) {
            colliderPosition[i][axis] += colliderCorrection[i][axis] * scale;
            colliderVelocity[i][axis] += (float)colliderImpulse[i][axis];
        }
    }
}

static void resolveCollisions(World* world) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY) | (1ull << COMPONENT_KIND);
    colliderCount = 0;
    collisionPairs = 0;
    for (uint32_t a = 0; a < world->archetypeCount && colliderCount < COLLIDER_CAP; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required || archetype->present[COMPONENT_ROUND]) {
            continue;
        }
        int hasOwner = archetype->present[COMPONENT_OWNER];
        int hasDamage = archetype->present[COMPONENT_DAMAGE];
        for (uint32_t slot = 0; slot < archetype->chunkCount && colliderCount < COLLIDER_CAP; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            float* velocities = chunkColumn(world, archetype, slot, COMPONENT_VELOCITY);
            uint16_t* kinds = chunkColumn(world, archetype, slot, COMPONENT_KIND);
            uint64_t* owners = hasOwner ? chunkColumn(world, archetype, slot, COMPONENT_OWNER) : NULL;
            Damage* damages = hasDamage ? chunkColumn(world, archetype, slot, COMPONENT_DAMAGE) : NULL;
            for (uint32_t i = 0; i < fill && colliderCount < COLLIDER_CAP; i++) {
                const KindProfile* profile = &kindProfiles[kinds[i] % KIND_COUNT];
                colliderPosition[colliderCount] = positions + i * 3;
                colliderVelocity[colliderCount] = velocities + i * 3;
                colliderHandle[colliderCount] = entityMake(indices[i], world->entities.generation[indices[i]]);
                colliderRadius[colliderCount] = profile->radius;
                colliderMobile[colliderCount] = profile->thrust > 0.0;
                colliderOwner[colliderCount] = owners ? owners[i] : 0;
                colliderOwned[colliderCount] = owners ? 1 : 0;
                colliderDamage[colliderCount] = damages ? damages + i : NULL;
                memset(colliderCorrection[colliderCount], 0, sizeof(colliderCorrection[0]));
                memset(colliderImpulse[colliderCount], 0, sizeof(colliderImpulse[0]));
                colliderCount++;
            }
        }
    }
    memset(gridHead, 0xFF, sizeof(gridHead));
    for (uint32_t i = 0; i < colliderCount; i++) {
        uint32_t slot = cellHash(cellOf(colliderPosition[i][0]), cellOf(colliderPosition[i][1]), cellOf(colliderPosition[i][2]));
        colliderNext[i] = gridHead[slot];
        gridHead[slot] = i;
    }
    if (colliderCount < 2) {
        return;
    }
    for (uint32_t i = 0; i < colliderCount; i++) {
        int32_t base[3] = {cellOf(colliderPosition[i][0]), cellOf(colliderPosition[i][1]), cellOf(colliderPosition[i][2])};
        for (int32_t x = -1; x <= 1; x++) {
            for (int32_t y = -1; y <= 1; y++) {
                for (int32_t z = -1; z <= 1; z++) {
                    uint32_t slot = cellHash(base[0] + x, base[1] + y, base[2] + z);
                    for (uint32_t j = gridHead[slot]; j != INVALID; j = colliderNext[j]) {
                        if (j > i) {
                            separate(i, j);
                        }
                    }
                }
            }
        }
    }
    applyCorrections();
}

static void orientationWrite(const double* forward, int16_t* orientation) {
    double reference[3] = {0.0, 1.0, 0.0};
    if (forward[1] > 0.99 || forward[1] < -0.99) {
        reference[1] = 0.0;
        reference[2] = 1.0;
    }
    double along = forward[0] * reference[0] + forward[1] * reference[1] + forward[2] * reference[2];
    double upward[3];
    double upwardLength = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        upward[axis] = reference[axis] - forward[axis] * along;
        upwardLength += upward[axis] * upward[axis];
    }
    upwardLength = sqrt(upwardLength);
    for (uint32_t axis = 0; axis < 3; axis++) {
        upward[axis] /= upwardLength;
    }
    double side[3] = {
        forward[1] * upward[2] - forward[2] * upward[1],
        forward[2] * upward[0] - forward[0] * upward[2],
        forward[0] * upward[1] - forward[1] * upward[0]
    };
    double trace = forward[0] + upward[1] + side[2];
    double quaternion[4];
    if (trace > 0.0) {
        double root = sqrt(trace + 1.0) * 2.0;
        quaternion[3] = 0.25 * root;
        quaternion[0] = (upward[2] - side[1]) / root;
        quaternion[1] = (side[0] - forward[2]) / root;
        quaternion[2] = (forward[1] - upward[0]) / root;
    } else if (forward[0] > upward[1] && forward[0] > side[2]) {
        double root = sqrt(1.0 + forward[0] - upward[1] - side[2]) * 2.0;
        quaternion[3] = (upward[2] - side[1]) / root;
        quaternion[0] = 0.25 * root;
        quaternion[1] = (upward[0] + forward[1]) / root;
        quaternion[2] = (side[0] + forward[2]) / root;
    } else if (upward[1] > side[2]) {
        double root = sqrt(1.0 + upward[1] - forward[0] - side[2]) * 2.0;
        quaternion[3] = (side[0] - forward[2]) / root;
        quaternion[0] = (upward[0] + forward[1]) / root;
        quaternion[1] = 0.25 * root;
        quaternion[2] = (side[1] + upward[2]) / root;
    } else {
        double root = sqrt(1.0 + side[2] - forward[0] - upward[1]) * 2.0;
        quaternion[3] = (forward[1] - upward[0]) / root;
        quaternion[0] = (side[0] + forward[2]) / root;
        quaternion[1] = (side[1] + upward[2]) / root;
        quaternion[2] = 0.25 * root;
    }
    for (uint32_t axis = 0; axis < 4; axis++) {
        double clamped = quaternion[axis] < -1.0 ? -1.0 : (quaternion[axis] > 1.0 ? 1.0 : quaternion[axis]);
        orientation[axis] = (int16_t)(clamped * 32767.0);
    }
}

static int quaternionRead(const int16_t* orientation, double* quaternion) {
    double length = 0.0;
    for (uint32_t axis = 0; axis < 4; axis++) {
        quaternion[axis] = (double)orientation[axis] / 32767.0;
        length += quaternion[axis] * quaternion[axis];
    }
    if (length < 0.25) {
        return 0;
    }
    length = sqrt(length);
    for (uint32_t axis = 0; axis < 4; axis++) {
        quaternion[axis] /= length;
    }
    return 1;
}

static void turnVector(const double* quaternion, const double* value, double* out) {
    double twice[3] = {
        2.0 * (quaternion[1] * value[2] - quaternion[2] * value[1]),
        2.0 * (quaternion[2] * value[0] - quaternion[0] * value[2]),
        2.0 * (quaternion[0] * value[1] - quaternion[1] * value[0])
    };
    out[0] = value[0] + quaternion[3] * twice[0] + quaternion[1] * twice[2] - quaternion[2] * twice[1];
    out[1] = value[1] + quaternion[3] * twice[1] + quaternion[2] * twice[0] - quaternion[0] * twice[2];
    out[2] = value[2] + quaternion[3] * twice[2] + quaternion[0] * twice[1] - quaternion[1] * twice[0];
}

static int forwardRead(const int16_t* orientation, double* forward) {
    double quaternion[4];
    if (!quaternionRead(orientation, quaternion)) {
        return 0;
    }
    forward[0] = 1.0 - 2.0 * (quaternion[1] * quaternion[1] + quaternion[2] * quaternion[2]);
    forward[1] = 2.0 * (quaternion[0] * quaternion[1] + quaternion[2] * quaternion[3]);
    forward[2] = 2.0 * (quaternion[0] * quaternion[2] - quaternion[1] * quaternion[3]);
    return 1;
}

static void turnToward(const double* from, const double* to, double limit, double* out) {
    double along = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    along = along > 1.0 ? 1.0 : (along < -1.0 ? -1.0 : along);
    double angle = acos(along);
    if (angle <= limit) {
        out[0] = to[0];
        out[1] = to[1];
        out[2] = to[2];
        return;
    }
    double axis[3] = {
        from[1] * to[2] - from[2] * to[1],
        from[2] * to[0] - from[0] * to[2],
        from[0] * to[1] - from[1] * to[0]
    };
    double axisLength = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (axisLength < 1e-9) {
        double helper[3] = {0.0, 1.0, 0.0};
        if (from[1] > 0.9 || from[1] < -0.9) {
            helper[1] = 0.0;
            helper[2] = 1.0;
        }
        axis[0] = from[1] * helper[2] - from[2] * helper[1];
        axis[1] = from[2] * helper[0] - from[0] * helper[2];
        axis[2] = from[0] * helper[1] - from[1] * helper[0];
        axisLength = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    }
    for (uint32_t i = 0; i < 3; i++) {
        axis[i] /= axisLength;
    }
    double sideways[3] = {
        axis[1] * from[2] - axis[2] * from[1],
        axis[2] * from[0] - axis[0] * from[2],
        axis[0] * from[1] - axis[1] * from[0]
    };
    double turned = 0.0;
    for (uint32_t i = 0; i < 3; i++) {
        out[i] = from[i] * cos(limit) + sideways[i] * sin(limit);
        turned += out[i] * out[i];
    }
    turned = sqrt(turned);
    for (uint32_t i = 0; i < 3; i++) {
        out[i] /= turned;
    }
}

static uint8_t flyForward(const KindProfile* type, double* position, float* velocity, int16_t* orientation,
                          const double* desired, double step) {
    double target = sqrt(desired[0] * desired[0] + desired[1] * desired[1] + desired[2] * desired[2]);
    double speed = sqrt((double)velocity[0] * velocity[0] + (double)velocity[1] * velocity[1] +
                        (double)velocity[2] * velocity[2]);
    double forward[3] = {1.0, 0.0, 0.0};
    if (!orientation || !forwardRead(orientation, forward)) {
        if (speed > 1e-6) {
            for (uint32_t axis = 0; axis < 3; axis++) {
                forward[axis] = (double)velocity[axis] / speed;
            }
        } else if (target > 1e-9) {
            for (uint32_t axis = 0; axis < 3; axis++) {
                forward[axis] = desired[axis] / target;
            }
        }
    }
    double alignment = 1.0;
    if (target > 1e-9) {
        double direction[3];
        for (uint32_t axis = 0; axis < 3; axis++) {
            direction[axis] = desired[axis] / target;
        }
        turnToward(forward, direction, type->turnRate * step, forward);
        alignment = forward[0] * direction[0] + forward[1] * direction[1] + forward[2] * direction[2];
        if (alignment < 0.0) {
            alignment = 0.0;
        }
    }
    double budget = type->thrust / type->mass * step;
    double change = target * alignment - speed;
    if (change > budget) {
        change = budget;
    }
    if (change < -budget) {
        change = -budget;
    }
    speed += change;
    if (speed < 0.0) {
        speed = 0.0;
    }
    for (uint32_t axis = 0; axis < 3; axis++) {
        velocity[axis] = (float)(forward[axis] * speed);
        position[axis] += (double)velocity[axis] * step;
    }
    if (orientation) {
        orientationWrite(forward, orientation);
    }
    if (change <= 0.0 || budget <= 0.0) {
        return 0;
    }
    double burn = change / budget * 255.0;
    return (uint8_t)(burn > 255.0 ? 255.0 : burn);
}

static double leadDirection(const double* delta, const double* relative, double speed, double* out) {
    double closing = relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2] - speed * speed;
    double along = delta[0] * relative[0] + delta[1] * relative[1] + delta[2] * relative[2];
    double span = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
    double time = 0.0;
    if (closing < -1e-9 || closing > 1e-9) {
        double discriminant = along * along - closing * span;
        if (discriminant >= 0.0) {
            double root = sqrt(discriminant);
            double first = (-along - root) / closing;
            double second = (-along + root) / closing;
            if (first < 0.0 || (second > 0.0 && second < first)) {
                first = second;
            }
            if (first > 0.0) {
                time = first;
            }
        }
    } else if (along < -1e-9) {
        time = -span / (2.0 * along);
    }
    double length = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] = delta[axis] + relative[axis] * time;
        length += out[axis] * out[axis];
    }
    length = sqrt(length);
    if (length < 1e-9) {
        out[0] = 1.0;
        out[1] = 0.0;
        out[2] = 0.0;
        return 0.0;
    }
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] /= length;
    }
    return time;
}

static void hardpointWorld(const KindProfile* type, uint32_t station, const double* quaternion, const double* position,
                           double* out) {
    double local[3] = {
        type->hardpoint[station][0] * type->radius,
        type->hardpoint[station][1] * type->radius,
        type->hardpoint[station][2] * type->radius
    };
    double mount[3];
    turnVector(quaternion, local, mount);
    for (uint32_t axis = 0; axis < 3; axis++) {
        out[axis] = position[axis] + mount[axis];
    }
}

static int turretShot(const KindProfile* type, uint32_t station, const double* quaternion, const double* position,
                      const float* velocity, const double* mark, const float* pace, double step,
                      double* muzzle, double* aim) {
    hardpointWorld(type, station, quaternion, position, muzzle);
    double delta[3];
    double relative[3];
    double range = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        delta[axis] = mark[axis] - muzzle[axis];
        relative[axis] = (pace ? (double)pace[axis] : 0.0) - (double)velocity[axis];
        range += delta[axis] * delta[axis];
    }
    range = sqrt(range);
    if (range < 1e-3 || range > type->roundRange) {
        return 0;
    }
    double flight = leadDirection(delta, relative, type->roundSpeed, aim);
    if (flight <= 0.0 || flight > (double)type->roundLife * step) {
        return 0;
    }
    double closing = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        closing += relative[axis] * delta[axis] / range;
    }
    double sweep = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        double lateral = relative[axis] - delta[axis] / range * closing;
        sweep += lateral * lateral;
    }
    if (sweep > type->tracking * type->tracking * range * range) {
        return 0;
    }
    double bearing[3];
    turnVector(quaternion, type->hardpointAxis[station], bearing);
    return aim[0] * bearing[0] + aim[1] * bearing[1] + aim[2] * bearing[2] > type->turretCone;
}

static void fireRound(const KindProfile* type, const double* muzzle, const float* velocity, const double* direction,
                      uint64_t owner, uint32_t system) {
    if (pendingCount == ROUND_CAP) {
        return;
    }
    Shot* shot = &pendingShots[pendingCount++];
    for (uint32_t axis = 0; axis < 3; axis++) {
        shot->position[axis] = muzzle[axis];
        shot->velocity[axis] = (float)((double)velocity[axis] + direction[axis] * type->roundSpeed);
    }
    orientationWrite(direction, shot->orientation);
    shot->owner = owner;
    shot->system = system;
    shot->damage = type->roundDamage;
    shot->kind = type->roundKind;
    shot->life = type->roundLife;
}

static void resolveRounds(World* world, double step) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY) | (1ull << COMPONENT_ROUND);
    for (uint32_t a = 0; a < world->archetypeCount; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required) {
            continue;
        }
        int hasOwner = archetype->present[COMPONENT_OWNER];
        for (uint32_t slot = 0; slot < archetype->chunkCount; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            float* velocities = chunkColumn(world, archetype, slot, COMPONENT_VELOCITY);
            Round* rounds = chunkColumn(world, archetype, slot, COMPONENT_ROUND);
            uint64_t* owners = hasOwner ? chunkColumn(world, archetype, slot, COMPONENT_OWNER) : NULL;
            for (uint32_t i = 0; i < fill; i++) {
                double travel[3];
                double middle[3];
                double span = 0.0;
                for (uint32_t axis = 0; axis < 3; axis++) {
                    travel[axis] = (double)velocities[i * 3 + axis] * step;
                    middle[axis] = positions[i * 3 + axis] + travel[axis] * 0.5;
                    span += travel[axis] * travel[axis];
                }
                uint32_t struck = INVALID;
                double nearest = 1.0;
                int32_t base[3] = {cellOf(middle[0]), cellOf(middle[1]), cellOf(middle[2])};
                for (int32_t x = -1; x <= 1; x++) {
                    for (int32_t y = -1; y <= 1; y++) {
                        for (int32_t z = -1; z <= 1; z++) {
                            uint32_t cell = cellHash(base[0] + x, base[1] + y, base[2] + z);
                            for (uint32_t j = gridHead[cell]; j != INVALID; j = colliderNext[j]) {
                                double offset[3];
                                double along = 0.0;
                                double gap = 0.0;
                                for (uint32_t axis = 0; axis < 3; axis++) {
                                    offset[axis] = positions[i * 3 + axis] - colliderPosition[j][axis];
                                    along += offset[axis] * travel[axis];
                                    gap += offset[axis] * offset[axis];
                                }
                                gap -= colliderRadius[j] * colliderRadius[j];
                                double moment = 0.0;
                                if (gap > 0.0) {
                                    double discriminant = along * along - span * gap;
                                    if (discriminant < 0.0 || span < 1e-9) {
                                        continue;
                                    }
                                    moment = (-along - sqrt(discriminant)) / span;
                                }
                                if (moment < 0.0 || moment > nearest) {
                                    continue;
                                }
                                if (owners && colliderOwned[j] && colliderOwner[j] == owners[i]) {
                                    continue;
                                }
                                nearest = moment;
                                struck = j;
                            }
                        }
                    }
                }
                for (uint32_t axis = 0; axis < 3; axis++) {
                    positions[i * 3 + axis] += travel[axis] * (struck == INVALID ? 1.0 : nearest);
                }
                if (struck != INVALID) {
                    Damage* damage = colliderDamage[struck];
                    if (damage && damage->hull > 0) {
                        int32_t left = rounds[i].damage;
                        if (damage->shield > 0) {
                            int32_t absorbed = damage->shield < left ? damage->shield : left;
                            damage->shield -= absorbed;
                            left -= absorbed;
                        }
                        if (left > 0 && damage->armor > 0) {
                            int32_t absorbed = damage->armor < left ? damage->armor : left;
                            damage->armor -= absorbed;
                            left -= absorbed;
                        }
                        if (left > 0) {
                            damage->hull -= left;
                            if (damage->hull <= 0) {
                                damage->hull = 0;
                                if (destroyedCount < 1024) {
                                    destroyedShips[destroyedCount++] = colliderHandle[struck];
                                }
                            }
                        }
                    }
                }
                if ((struck != INVALID || !--rounds[i].life) && spentCount < ROUND_CAP) {
                    spentRounds[spentCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                }
            }
        }
    }
}

typedef struct {
    double position[3];
    double velocity[3];
    Entity builder;
    uint64_t owner;
    uint32_t system;
    uint16_t kind;
    uint8_t hull;
    uint8_t armor;
    uint8_t shield;
} Spawn;

static Spawn pendingSpawns[SPAWN_CAP];
static uint32_t spawnCount;
static uint32_t spawnRotation;

static double stationPlace[STATION_CAP][3];
static uint64_t stationOwner[STATION_CAP];
static uint32_t stationSystem[STATION_CAP];
static Entity stationHandle[STATION_CAP];
static Cargo* stationStore[STATION_CAP];
static Station* stationState[STATION_CAP];
static uint32_t stationCount;

static double rockPlace[ROCK_CAP][3];
static Entity rockHandle[ROCK_CAP];
static uint32_t rockOre[ROCK_CAP];
static uint32_t rockCount;

static int payStage(uint32_t* paid, Cargo* store, const uint32_t* cost, uint32_t stage, uint32_t stages) {
    uint32_t due[ORE_COUNT];
    if (!stages) {
        return 0;
    }
    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
        uint32_t want = (uint32_t)((uint64_t)cost[ore] * stage / stages);
        due[ore] = want > paid[ore] ? want - paid[ore] : 0;
        if (store->ore[ore] < due[ore]) {
            return 0;
        }
    }
    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
        store->ore[ore] -= due[ore];
        paid[ore] += due[ore];
    }
    return 1;
}

static void gatherStations(World* world) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_OWNER) | (1ull << COMPONENT_SYSTEM) |
                        (1ull << COMPONENT_CARGO) | (1ull << COMPONENT_STATION);
    stationCount = 0;
    for (uint32_t a = 0; a < world->archetypeCount && stationCount < STATION_CAP; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required) {
            continue;
        }
        for (uint32_t slot = 0; slot < archetype->chunkCount && stationCount < STATION_CAP; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            uint64_t* owners = chunkColumn(world, archetype, slot, COMPONENT_OWNER);
            uint32_t* systems = chunkColumn(world, archetype, slot, COMPONENT_SYSTEM);
            Cargo* stores = chunkColumn(world, archetype, slot, COMPONENT_CARGO);
            Station* states = chunkColumn(world, archetype, slot, COMPONENT_STATION);
            for (uint32_t i = 0; i < fill && stationCount < STATION_CAP; i++) {
                stationPlace[stationCount][0] = positions[i * 3 + 0];
                stationPlace[stationCount][1] = positions[i * 3 + 1];
                stationPlace[stationCount][2] = positions[i * 3 + 2];
                stationOwner[stationCount] = owners[i];
                stationSystem[stationCount] = systems[i];
                stationHandle[stationCount] = entityMake(indices[i], world->entities.generation[indices[i]]);
                stationStore[stationCount] = stores + i;
                stationState[stationCount] = states + i;
                stationCount++;
            }
        }
    }
}

static void gatherRocks(World* world) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_KIND) | (1ull << COMPONENT_CARGO);
    rockCount = 0;
    for (uint32_t a = 0; a < world->archetypeCount && rockCount < ROCK_CAP; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required || archetype->present[COMPONENT_OWNER]) {
            continue;
        }
        for (uint32_t slot = 0; slot < archetype->chunkCount && rockCount < ROCK_CAP; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            uint16_t* kinds = chunkColumn(world, archetype, slot, COMPONENT_KIND);
            for (uint32_t i = 0; i < fill && rockCount < ROCK_CAP; i++) {
                if (kinds[i] < KIND_ROCK || kinds[i] >= KIND_COUNT) {
                    continue;
                }
                rockPlace[rockCount][0] = positions[i * 3 + 0];
                rockPlace[rockCount][1] = positions[i * 3 + 1];
                rockPlace[rockCount][2] = positions[i * 3 + 2];
                rockHandle[rockCount] = entityMake(indices[i], world->entities.generation[indices[i]]);
                rockOre[rockCount] = (uint32_t)(kinds[i] - KIND_ROCK) % ORE_COUNT;
                rockCount++;
            }
        }
    }
}

static Entity nearestRock(const double* from, double limit, uint32_t wanted) {
    Entity found = 0;
    double best = limit;
    for (uint32_t i = 0; i < rockCount; i++) {
        if (!(wanted & (1u << rockOre[i]))) {
            continue;
        }
        double gap = separation(from, rockPlace[i]);
        if (gap <= best) {
            best = gap;
            found = rockHandle[i];
        }
    }
    return found;
}

static int nearestStation(uint64_t owner, const double* from, double limit) {
    int found = -1;
    double best = limit;
    for (uint32_t i = 0; i < stationCount; i++) {
        if (stationOwner[i] != owner || stationState[i]->build < BUILD_FULL) {
            continue;
        }
        double gap = separation(from, stationPlace[i]);
        if (gap <= best) {
            best = gap;
            found = (int)i;
        }
    }
    return found;
}

static void queueSpawn(const double* position, const double* velocity, Entity builder, uint64_t owner, uint32_t system,
                       uint16_t kind, uint8_t hull, uint8_t armor, uint8_t shield) {
    if (spawnCount == SPAWN_CAP) {
        return;
    }
    Spawn* entry = &pendingSpawns[spawnCount++];
    memcpy(entry->position, position, sizeof(entry->position));
    if (velocity) {
        memcpy(entry->velocity, velocity, sizeof(entry->velocity));
    } else {
        memset(entry->velocity, 0, sizeof(entry->velocity));
    }
    entry->builder = builder;
    entry->owner = owner;
    entry->system = system;
    entry->kind = kind;
    entry->hull = hull;
    entry->armor = armor;
    entry->shield = shield;
}

static uint8_t flyToward(const KindProfile* type, double* position, float* velocity, int16_t* orientation,
                         const double* point, double standoff, double step) {
    double delta[3];
    double distance = 0.0;
    for (uint32_t axis = 0; axis < 3; axis++) {
        delta[axis] = point[axis] - position[axis];
        distance += delta[axis] * delta[axis];
    }
    distance = sqrt(distance);
    double desired[3] = {0.0, 0.0, 0.0};
    double gap = distance - standoff;
    if (distance > 1e-6 && gap > 0.0) {
        double approach = gap / ARRIVAL_TIME;
        double braking = 0.9 * sqrt(2.0 * type->thrust / type->mass * gap);
        double speed = approach < type->maxSpeed ? approach : type->maxSpeed;
        if (braking < speed) {
            speed = braking;
        }
        for (uint32_t axis = 0; axis < 3; axis++) {
            desired[axis] = delta[axis] / distance * speed;
        }
    }
    return flyForward(type, position, velocity, orientation, desired, step);
}

static int mineInto(World* world, const KindProfile* type, double* position, float* velocity, int16_t* orientation,
                    Cargo* hold, Entity seamEntity, double step, uint8_t* throttle) {
    double* rock = componentOf(world, seamEntity, COMPONENT_POSITION);
    Cargo* seam = componentOf(world, seamEntity, COMPONENT_CARGO);
    uint16_t* seamKind = componentOf(world, seamEntity, COMPONENT_KIND);
    if (!rock || !seam || !seamKind || *seamKind < KIND_ROCK || *seamKind >= KIND_COUNT) {
        return 0;
    }
    uint32_t ore = (uint32_t)(*seamKind - KIND_ROCK) % ORE_COUNT;
    double standoff = kindProfiles[*seamKind].radius + type->radius + MINING_RANGE;
    *throttle = flyToward(type, position, velocity, orientation, rock, standoff * 0.7, step);
    if (separation(position, rock) > standoff) {
        return 1;
    }
    uint32_t take = seam->ore[ore] < MINING_RATE ? seam->ore[ore] : MINING_RATE;
    uint32_t moved = cargoLoad(hold, ore, take, type->hold);
    seam->ore[ore] -= moved;
    *throttle = 0;
    return seam->ore[ore] ? 1 : 2;
}

static void repairShips(World* world) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_DAMAGE) |
                        (1ull << COMPONENT_KIND) | (1ull << COMPONENT_OWNER);
    for (uint32_t a = 0; a < world->archetypeCount; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required || archetype->present[COMPONENT_ROUND] ||
            archetype->present[COMPONENT_STATION]) {
            continue;
        }
        for (uint32_t slot = 0; slot < archetype->chunkCount; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            Damage* damages = chunkColumn(world, archetype, slot, COMPONENT_DAMAGE);
            uint16_t* kinds = chunkColumn(world, archetype, slot, COMPONENT_KIND);
            uint64_t* owners = chunkColumn(world, archetype, slot, COMPONENT_OWNER);
            for (uint32_t i = 0; i < fill; i++) {
                const KindProfile* type = &kindProfiles[kinds[i] % KIND_COUNT];
                if (damages[i].hull <= 0 ||
                    (damages[i].hull >= type->maxHull && damages[i].armor >= type->maxArmor)) {
                    continue;
                }
                int index = nearestStation(owners[i], positions + i * 3, REPAIR_RANGE);
                if (index < 0) {
                    continue;
                }
                int32_t hullStep = (int32_t)((double)type->maxHull * REPAIR_RATE) + 1;
                int32_t armorStep = (int32_t)((double)type->maxArmor * REPAIR_RATE) + 1;
                if (hullStep > type->maxHull - damages[i].hull) {
                    hullStep = type->maxHull - damages[i].hull;
                }
                if (armorStep > type->maxArmor - damages[i].armor) {
                    armorStep = type->maxArmor - damages[i].armor;
                }
                hullStep = hullStep > 0 ? hullStep : 0;
                armorStep = armorStep > 0 ? armorStep : 0;
                double share = 0.0;
                if (type->maxHull > 0) {
                    share += (double)hullStep / (double)type->maxHull * HULL_REPAIR_SHARE;
                }
                if (type->maxArmor > 0) {
                    share += (double)armorStep / (double)type->maxArmor * ARMOR_REPAIR_SHARE;
                }
                Cargo* store = stationStore[index];
                Station* state = stationState[index];
                int payable = 1;
                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                    if (type->cost[ore] && !store->ore[ore]) {
                        payable = 0;
                    }
                }
                if (!payable) {
                    continue;
                }
                damages[i].hull += hullStep;
                damages[i].armor += armorStep;
                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                    state->owed[ore] = (uint16_t)(state->owed[ore] + (uint16_t)(share * (double)type->cost[ore] * 1024.0));
                    while (state->owed[ore] >= 1024 && store->ore[ore]) {
                        store->ore[ore]--;
                        state->owed[ore] = (uint16_t)(state->owed[ore] - 1024);
                    }
                }
            }
        }
    }
}

static void launchShip(uint32_t index) {
    Station* state = stationState[index];
    const KindProfile* host = &kindProfiles[KIND_STATION];
    const KindProfile* made = &kindProfiles[state->hangarKind[0] % KIND_COUNT];
    uint32_t door = host->hardpoints ? (spawnRotation++) % host->hardpoints : 0;
    double place[3];
    double pace[3];
    for (uint32_t axis = 0; axis < 3; axis++) {
        pace[axis] = host->hardpointAxis[door][axis] * LAUNCH_SPEED;
        place[axis] = stationPlace[index][axis] + host->hardpoint[door][axis] * host->radius +
                      host->hardpointAxis[door][axis] * (made->radius + 60.0);
    }
    queueSpawn(place, pace, 0, stationOwner[index], stationSystem[index], state->hangarKind[0],
               state->hangarHull[0], state->hangarArmor[0], state->hangarShield[0]);
    state->stored--;
    state->pending--;
    for (uint32_t slot = 0; slot < state->stored; slot++) {
        state->hangarKind[slot] = state->hangarKind[slot + 1];
        state->hangarHull[slot] = state->hangarHull[slot + 1];
        state->hangarArmor[slot] = state->hangarArmor[slot + 1];
        state->hangarShield[slot] = state->hangarShield[slot + 1];
    }
    state->launchTicks = LAUNCH_PERIOD;
}

static void simulateStations(World* world) {
    for (uint32_t i = 0; i < stationCount; i++) {
        Station* state = stationState[i];
        Cargo* store = stationStore[i];
        if (state->build < BUILD_FULL) {
            continue;
        }
        if (state->queueKind != QUEUE_NONE) {
            const KindProfile* made = &kindProfiles[state->queueKind % KIND_COUNT];
            if (payStage(state->paid, store, made->cost, (uint32_t)state->queueTicks + 1, made->buildTicks)) {
                state->queueTicks++;
                if (state->queueTicks >= made->buildTicks) {
                    if (state->stored < HANGAR_CAP) {
                        state->hangarKind[state->stored] = state->queueKind;
                        state->hangarHull[state->stored] = 255;
                        state->hangarArmor[state->stored] = 255;
                        state->hangarShield[state->stored] = 255;
                        state->stored++;
                        state->pending++;
                    }
                    state->queueKind = QUEUE_NONE;
                    state->queueTicks = 0;
                    memset(state->paid, 0, sizeof(state->paid));
                }
            }
        }
        Player* player = playerOf(stationOwner[i]);
        if (player && player->active) {
            uint32_t kind = (player->active - 1) % KIND_COUNT;
            const KindProfile* subject = &kindProfiles[kind];
            int payable = subject->researchTicks && player->progress[kind] < subject->researchTicks;
            for (uint32_t ore = 0; ore < ORE_COUNT && payable; ore++) {
                payable = !subject->researchCost[ore] || store->ore[ore] > 0;
            }
            if (payable) {
                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                    state->owed[ore] = (uint16_t)(state->owed[ore] +
                        (uint16_t)((uint64_t)subject->researchCost[ore] * 1024 / subject->researchTicks));
                    while (state->owed[ore] >= 1024 && store->ore[ore]) {
                        store->ore[ore]--;
                        state->owed[ore] = (uint16_t)(state->owed[ore] - 1024);
                    }
                }
                player->progress[kind]++;
                if (player->progress[kind] >= subject->researchTicks) {
                    player->unlocked |= 1u << kind;
                    player->active = 0;
                }
            }
        }
        if (state->launchTicks) {
            state->launchTicks--;
        } else if (state->pending && state->stored) {
            launchShip(i);
        }
    }
    repairShips(world);
}

static void simulate(World* world, double step) {
    uint64_t moveMask = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY);
    completedCount = 0;
    destroyedCount = 0;
    spentCount = 0;
    pendingCount = 0;
    spawnCount = 0;
    gatherStations(world);
    gatherRocks(world);
    for (uint32_t a = 0; a < world->archetypeCount; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & moveMask) != moveMask || archetype->present[COMPONENT_ROUND]) {
            continue;
        }
        int hasOrder = archetype->present[COMPONENT_ORDER];
        for (uint32_t slot = 0; slot < archetype->chunkCount; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            float* velocities = chunkColumn(world, archetype, slot, COMPONENT_VELOCITY);
            Order* orders = hasOrder ? chunkColumn(world, archetype, slot, COMPONENT_ORDER) : NULL;
            uint32_t* indices = hasOrder ? chunkEntities(world, archetype, slot) : NULL;
            uint16_t* kinds = archetype->present[COMPONENT_KIND] ? chunkColumn(world, archetype, slot, COMPONENT_KIND) : NULL;
            int16_t* orientations = archetype->present[COMPONENT_ORIENTATION] ? chunkColumn(world, archetype, slot, COMPONENT_ORIENTATION) : NULL;
            uint64_t* owners = archetype->present[COMPONENT_OWNER] ? chunkColumn(world, archetype, slot, COMPONENT_OWNER) : NULL;
            uint32_t* systems = archetype->present[COMPONENT_SYSTEM] ? chunkColumn(world, archetype, slot, COMPONENT_SYSTEM) : NULL;
            Cargo* holds = archetype->present[COMPONENT_CARGO] ? chunkColumn(world, archetype, slot, COMPONENT_CARGO) : NULL;
            Damage* healths = archetype->present[COMPONENT_DAMAGE] ? chunkColumn(world, archetype, slot, COMPONENT_DAMAGE) : NULL;
            for (uint32_t i = 0; i < fill; i++) {
                if (orders && orders[i].active) {
                    const KindProfile* type = &kindProfiles[kinds ? kinds[i] % KIND_COUNT : 0];
                    if (orders[i].cooldown) {
                        orders[i].cooldown--;
                    } else if (!orders[i].rounds) {
                        orders[i].rounds = (uint8_t)type->magazine;
                    }
                    if (orders[i].type == ORDER_ATTACK) {
                        double* mark = componentOf(world, orders[i].targetEntity, COMPONENT_POSITION);
                        if (!mark || !orientations) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        float* pace = componentOf(world, orders[i].targetEntity, COMPONENT_VELOCITY);
                        uint16_t* markKind = componentOf(world, orders[i].targetEntity, COMPONENT_KIND);
                        double bore = kindProfiles[markKind ? *markKind % KIND_COUNT : 0].radius;
                        double delta[3];
                        double relative[3];
                        double distance = 0.0;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            delta[axis] = mark[axis] - positions[i * 3 + axis];
                            relative[axis] = (pace ? (double)pace[axis] : 0.0) - (double)velocities[i * 3 + axis];
                            distance += delta[axis] * delta[axis];
                        }
                        distance = sqrt(distance);
                        if (distance < 1e-3) {
                            delta[0] = 1.0;
                            distance = 1.0;
                        }
                        double aim[3];
                        double reach = leadDirection(delta, relative, type->roundSpeed, aim);
                        double extend = type->maxSpeed / type->turnRate * BREAK_TURNS;
                        if (orders[i].phase == PHASE_ATTACK_BREAK) {
                            if (!orders[i].cooldown && distance > extend) {
                                orders[i].phase = PHASE_RUN;
                                orders[i].rounds = (uint8_t)type->magazine;
                            }
                        } else if (distance < extend) {
                            orders[i].phase = PHASE_ATTACK_BREAK;
                            orders[i].cooldown = type->reload;
                        }
                        double desired[3];
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            desired[axis] = (orders[i].phase == PHASE_ATTACK_BREAK ? -delta[axis] / distance : aim[axis]) * type->maxSpeed;
                        }
                        orders[i].throttle = flyForward(type, positions + i * 3, velocities + i * 3, orientations + i * 4, desired, step);
                        distance = 0.0;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            delta[axis] = mark[axis] - positions[i * 3 + axis];
                            relative[axis] = (pace ? (double)pace[axis] : 0.0) - (double)velocities[i * 3 + axis];
                            distance += delta[axis] * delta[axis];
                        }
                        distance = sqrt(distance);
                        reach = leadDirection(delta, relative, type->roundSpeed, aim);
                        if (orders[i].phase == PHASE_RUN && orders[i].rounds && !orders[i].cooldown &&
                            distance < type->roundRange && reach > 0.0 && reach < (double)type->roundLife * step) {
                            double nose[3];
                            double aligned = 0.0;
                            if (forwardRead(orientations + i * 4, nose)) {
                                aligned = nose[0] * aim[0] + nose[1] * aim[1] + nose[2] * aim[2];
                            }
                            double quaternion[4];
                            double muzzle[3];
                            if (aligned > 0.0 && (1.0 - aligned * aligned) * distance * distance < bore * bore &&
                                quaternionRead(orientations + i * 4, quaternion)) {
                                hardpointWorld(type, (uint32_t)(type->magazine - orders[i].rounds) % type->hardpoints,
                                               quaternion, positions + i * 3, muzzle);
                                fireRound(type, muzzle, velocities + i * 3, nose,
                                          owners ? owners[i] : 0, systems ? systems[i] : 0);
                                orders[i].rounds--;
                                orders[i].cooldown = orders[i].rounds ? type->cadence : type->reload;
                                if (!orders[i].rounds) {
                                    orders[i].phase = PHASE_ATTACK_BREAK;
                                }
                            }
                        }
                        continue;
                    }
                    if (orders[i].type == ORDER_ORBIT) {
                        double* centre = componentOf(world, orders[i].targetEntity, COMPONENT_POSITION);
                        if (!centre) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        double radial[3];
                        double radialLength = 0.0;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            radial[axis] = positions[i * 3 + axis] - centre[axis];
                            radialLength += radial[axis] * radial[axis];
                        }
                        radialLength = sqrt(radialLength);
                        if (radialLength < 1e-6) {
                            radial[0] = 1.0;
                            radial[1] = 0.0;
                            radial[2] = 0.0;
                            radialLength = 1.0;
                        }
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            radial[axis] /= radialLength;
                        }
                        double tangent[3];
                        double tangentLength = 0.0;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            tangent[axis] = (double)velocities[i * 3 + axis];
                        }
                        double along = tangent[0] * radial[0] + tangent[1] * radial[1] + tangent[2] * radial[2];
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            tangent[axis] -= radial[axis] * along;
                            tangentLength += tangent[axis] * tangent[axis];
                        }
                        tangentLength = sqrt(tangentLength);
                        if (tangentLength < 1e-4) {
                            double helper[3] = {0.0, 1.0, 0.0};
                            if (radial[1] > 0.9 || radial[1] < -0.9) {
                                helper[0] = 1.0;
                                helper[1] = 0.0;
                            }
                            tangent[0] = helper[1] * radial[2] - helper[2] * radial[1];
                            tangent[1] = helper[2] * radial[0] - helper[0] * radial[2];
                            tangent[2] = helper[0] * radial[1] - helper[1] * radial[0];
                            tangentLength = sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
                        }
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            tangent[axis] /= tangentLength;
                        }
                        double maxAcceleration = type->thrust / type->mass;
                        double sustainable = 0.8 * sqrt(maxAcceleration * radialLength);
                        double turnable = type->turnRate * radialLength;
                        double cruise = type->maxSpeed < sustainable ? type->maxSpeed : sustainable;
                        if (turnable < cruise) {
                            cruise = turnable;
                        }
                        double closing = (radialLength - (double)orders[i].radius) / ARRIVAL_TIME;
                        double limit = cruise * 0.7;
                        if (closing > limit) {
                            closing = limit;
                        }
                        if (closing < -limit) {
                            closing = -limit;
                        }
                        double orbital = sqrt(cruise * cruise - closing * closing);
                        double desired[3];
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            desired[axis] = tangent[axis] * orbital - radial[axis] * closing;
                        }
                        orders[i].throttle = flyForward(type, positions + i * 3, velocities + i * 3,
                                                        orientations ? orientations + i * 4 : NULL, desired, step);
                        double quaternion[4];
                        if (!type->turret || !orders[i].rounds || orders[i].cooldown || !orientations ||
                            !quaternionRead(orientations + i * 4, quaternion)) {
                            continue;
                        }
                        float* pace = componentOf(world, orders[i].targetEntity, COMPONENT_VELOCITY);
                        for (uint32_t slot = 0; slot < type->hardpoints; slot++) {
                            uint32_t station = ((uint32_t)(type->magazine - orders[i].rounds) + slot) % type->hardpoints;
                            double muzzle[3];
                            double aim[3];
                            if (!turretShot(type, station, quaternion, positions + i * 3, velocities + i * 3,
                                            centre, pace, step, muzzle, aim)) {
                                continue;
                            }
                            fireRound(type, muzzle, velocities + i * 3, aim,
                                      owners ? owners[i] : 0, systems ? systems[i] : 0);
                            orders[i].rounds--;
                            orders[i].cooldown = orders[i].rounds ? type->cadence : type->reload;
                            break;
                        }
                        continue;
                    }
                    if (orders[i].type == ORDER_MINE) {
                        const KindProfile* berth = &kindProfiles[KIND_STATION];
                        double dock = berth->radius + type->radius + DOCK_RANGE;
                        if (!holds) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        uint16_t* seamKind = componentOf(world, orders[i].targetEntity, COMPONENT_KIND);
                        if (seamKind && *seamKind >= KIND_ROCK && *seamKind < KIND_COUNT) {
                            orders[i].rounds = (uint8_t)(*seamKind - KIND_ROCK + 1);
                        }
                        if (orders[i].phase == PHASE_HAUL) {
                            int home = nearestStation(owners ? owners[i] : 0, positions + i * 3, HAUL_RANGE);
                            if (home < 0) {
                                orders[i].phase = PHASE_RUN;
                                continue;
                            }
                            orders[i].target[0] = stationPlace[home][0];
                            orders[i].target[1] = stationPlace[home][1];
                            orders[i].target[2] = stationPlace[home][2];
                            orders[i].hold = entityIndexOf(stationHandle[home]);
                            orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                           orientations ? orientations + i * 4 : NULL,
                                                           orders[i].target, dock * 0.7, step);
                            if (separation(positions + i * 3, stationPlace[home]) <= dock) {
                                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                                    holds[i].ore[ore] -= cargoLoad(stationStore[home], ore, holds[i].ore[ore], berth->hold);
                                }
                                orders[i].phase = PHASE_RUN;
                            }
                            continue;
                        }
                        int cut = mineInto(world, type, positions + i * 3, velocities + i * 3,
                                           orientations ? orientations + i * 4 : NULL, &holds[i],
                                           orders[i].targetEntity, step, &orders[i].throttle);
                        if (cut == 2 && destroyedCount < 1024) {
                            destroyedShips[destroyedCount++] = orders[i].targetEntity;
                        }
                        if (!cut) {
                            uint32_t ore = orders[i].rounds ? (uint32_t)(orders[i].rounds - 1) % ORE_COUNT : 0;
                            Entity next = nearestRock(positions + i * 3, HAUL_RANGE, 1u << ore);
                            if (!next) {
                                next = nearestRock(positions + i * 3, HAUL_RANGE, (1u << ORE_COUNT) - 1);
                            }
                            if (next) {
                                orders[i].targetEntity = next;
                                continue;
                            }
                            int home = nearestStation(owners ? owners[i] : 0, positions + i * 3, HAUL_RANGE);
                            if (home >= 0) {
                                orders[i].type = ORDER_DOCK;
                                orders[i].targetEntity = stationHandle[home];
                                orders[i].phase = PHASE_RUN;
                                continue;
                            }
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        orders[i].hold = entityIndexOf(orders[i].targetEntity);
                        if (cargoTotal(&holds[i]) >= type->hold &&
                            nearestStation(owners ? owners[i] : 0, positions + i * 3, HAUL_RANGE) >= 0) {
                            orders[i].phase = PHASE_HAUL;
                        }
                        continue;
                    }
                    if (orders[i].type == ORDER_BUILD) {
                        const KindProfile* plan = &kindProfiles[KIND_STATION];
                        if (!holds) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        if (orders[i].phase == PHASE_RUN) {
                            int adopted = -1;
                            for (uint32_t s = 0; s < stationCount && adopted < 0; s++) {
                                if (stationOwner[s] == (owners ? owners[i] : 0) && stationState[s]->build < BUILD_FULL &&
                                    separation(orders[i].target, stationPlace[s]) <= ADOPT_RANGE) {
                                    adopted = (int)s;
                                }
                            }
                            if (adopted >= 0) {
                                orders[i].targetEntity = stationHandle[adopted];
                                orders[i].phase = cargoTotal(&holds[i]) ? PHASE_HAUL : PHASE_LOAD;
                                continue;
                            }
                            orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                           orientations ? orientations + i * 4 : NULL,
                                                           orders[i].target, BUILD_RANGE * 0.5, step);
                            if (separation(positions + i * 3, orders[i].target) > BUILD_RANGE) {
                                continue;
                            }
                            queueSpawn(orders[i].target, NULL, entityMake(indices[i], world->entities.generation[indices[i]]),
                                       owners ? owners[i] : 0, systems ? systems[i] : 0, KIND_STATION, 255, 255, 255);
                            orders[i].phase = PHASE_HAUL;
                            continue;
                        }
                        Station* site = componentOf(world, orders[i].targetEntity, COMPONENT_STATION);
                        double* place = componentOf(world, orders[i].targetEntity, COMPONENT_POSITION);
                        Cargo* pile = componentOf(world, orders[i].targetEntity, COMPONENT_CARGO);
                        if (!site || !place || !pile) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        double reach = plan->radius + type->radius + BUILD_RANGE;
                        int feedable = 0;
                        for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                            feedable |= pile->ore[ore] && site->paid[ore] < plan->cost[ore];
                        }
                        uint32_t deficit[ORE_COUNT];
                        uint32_t chosen = 0;
                        uint32_t deepest = 0;
                        for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                            uint32_t banked = site->paid[ore] + pile->ore[ore] + holds[i].ore[ore];
                            deficit[ore] = plan->cost[ore] > banked ? plan->cost[ore] - banked : 0;
                            if (deficit[ore] > deepest && nearestRock(positions + i * 3, HAUL_RANGE, 1u << ore)) {
                                deepest = deficit[ore];
                                chosen = ore;
                            }
                        }
                        if (orders[i].phase == PHASE_LOAD) {
                            if (!orders[i].rounds) {
                                if (!deepest) {
                                    orders[i].phase = PHASE_HAUL;
                                    continue;
                                }
                                orders[i].rounds = (uint8_t)(chosen + 1);
                            }
                            uint32_t ore = (uint32_t)(orders[i].rounds - 1) % ORE_COUNT;
                            if (cargoTotal(&holds[i]) >= type->hold || !deficit[ore]) {
                                orders[i].phase = PHASE_HAUL;
                                orders[i].rounds = 0;
                                continue;
                            }
                            int source = nearestStation(owners ? owners[i] : 0, positions + i * 3, HAUL_RANGE);
                            if (source >= 0 && stationHandle[source] != orders[i].targetEntity &&
                                cargoTotal(stationStore[source])) {
                                double dock = plan->radius + type->radius + DOCK_RANGE;
                                orders[i].hold = entityIndexOf(stationHandle[source]);
                                orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                               orientations ? orientations + i * 4 : NULL,
                                                               stationPlace[source], dock * 0.7, step);
                                if (separation(positions + i * 3, stationPlace[source]) <= dock) {
                                    for (uint32_t wanted = 0; wanted < ORE_COUNT; wanted++) {
                                        uint32_t take = stationStore[source]->ore[wanted] < deficit[wanted]
                                                            ? stationStore[source]->ore[wanted] : deficit[wanted];
                                        stationStore[source]->ore[wanted] -= cargoLoad(&holds[i], wanted, take, type->hold);
                                    }
                                    orders[i].throttle = 0;
                                    orders[i].phase = PHASE_HAUL;
                                    orders[i].rounds = 0;
                                }
                                continue;
                            }
                            Entity seam = nearestRock(positions + i * 3, HAUL_RANGE, 1u << ore);
                            orders[i].hold = seam ? entityIndexOf(seam) : 0;
                            int cut = seam ? mineInto(world, type, positions + i * 3, velocities + i * 3,
                                                      orientations ? orientations + i * 4 : NULL, &holds[i],
                                                      seam, step, &orders[i].throttle)
                                           : 0;
                            if (cut == 2 && destroyedCount < 1024) {
                                destroyedShips[destroyedCount++] = seam;
                            }
                            if (!cut) {
                                orders[i].phase = PHASE_HAUL;
                                orders[i].rounds = 0;
                            }
                            continue;
                        }
                        orders[i].hold = entityIndexOf(orders[i].targetEntity);
                        if (cargoTotal(&holds[i])) {
                            orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                           orientations ? orientations + i * 4 : NULL,
                                                           place, reach * 0.7, step);
                            if (separation(positions + i * 3, place) <= reach) {
                                for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                                    holds[i].ore[ore] -= cargoLoad(pile, ore, holds[i].ore[ore], plan->hold);
                                }
                                orders[i].throttle = 0;
                            }
                            continue;
                        }
                        if (feedable) {
                            orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                           orientations ? orientations + i * 4 : NULL,
                                                           place, reach * 0.7, step);
                            if (separation(positions + i * 3, place) > reach) {
                                continue;
                            }
                            double raised = 0.0;
                            uint32_t kinds = 0;
                            for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                                if (!plan->cost[ore]) {
                                    continue;
                                }
                                uint32_t bite = plan->cost[ore] * BUILD_RATE / BUILD_FULL + 1;
                                uint32_t room = plan->cost[ore] - site->paid[ore];
                                bite = bite > room ? room : bite;
                                bite = bite > pile->ore[ore] ? pile->ore[ore] : bite;
                                pile->ore[ore] -= bite;
                                site->paid[ore] += bite;
                                raised += (double)site->paid[ore] / (double)plan->cost[ore];
                                kinds++;
                            }
                            site->build = (uint16_t)(kinds ? raised / (double)kinds * BUILD_FULL : BUILD_FULL);
                            orders[i].throttle = 0;
                            if (site->build >= BUILD_FULL) {
                                memset(site->paid, 0, sizeof(site->paid));
                                orders[i].active = 0;
                                if (completedCount < 8192) {
                                    completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                                }
                            }
                            continue;
                        }
                        if (deepest) {
                            orders[i].phase = PHASE_LOAD;
                        }
                        continue;
                    }
                    if (orders[i].type == ORDER_DOCK) {
                        double* place = componentOf(world, orders[i].targetEntity, COMPONENT_POSITION);
                        Station* berth = componentOf(world, orders[i].targetEntity, COMPONENT_STATION);
                        Cargo* store = componentOf(world, orders[i].targetEntity, COMPONENT_CARGO);
                        uint64_t* keeper = componentOf(world, orders[i].targetEntity, COMPONENT_OWNER);
                        if (!place || !berth || !store || !keeper || berth->build < BUILD_FULL ||
                            (owners && *keeper != owners[i])) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                            continue;
                        }
                        const KindProfile* host = &kindProfiles[KIND_STATION];
                        double dock = host->radius + type->radius + DOCK_RANGE;
                        orders[i].throttle = flyToward(type, positions + i * 3, velocities + i * 3,
                                                       orientations ? orientations + i * 4 : NULL,
                                                       place, dock * 0.6, step);
                        if (separation(positions + i * 3, place) > dock) {
                            continue;
                        }
                        if (holds) {
                            for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
                                holds[i].ore[ore] -= cargoLoad(store, ore, holds[i].ore[ore], host->hold);
                            }
                        }
                        if (berth->stored < HANGAR_CAP && kinds && destroyedCount < 1024) {
                            berth->hangarKind[berth->stored] = kinds[i];
                            berth->hangarHull[berth->stored] = healths ? healthFraction(healths[i].hull, type->maxHull) : 255;
                            berth->hangarArmor[berth->stored] = healths ? healthFraction(healths[i].armor, type->maxArmor) : 255;
                            berth->hangarShield[berth->stored] = healths ? healthFraction(healths[i].shield, type->maxShield) : 255;
                            berth->stored++;
                            destroyedShips[destroyedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                        }
                        continue;
                    }
                    double delta[3] = {
                        orders[i].target[0] - positions[i * 3 + 0],
                        orders[i].target[1] - positions[i * 3 + 1],
                        orders[i].target[2] - positions[i * 3 + 2]
                    };
                    double distance = sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
                    double speed = sqrt((double)velocities[i * 3 + 0] * velocities[i * 3 + 0] +
                                        (double)velocities[i * 3 + 1] * velocities[i * 3 + 1] +
                                        (double)velocities[i * 3 + 2] * velocities[i * 3 + 2]);
                    if (distance < ARRIVAL_DISTANCE && speed < ARRIVAL_SPEED) {
                        positions[i * 3 + 0] = orders[i].target[0];
                        positions[i * 3 + 1] = orders[i].target[1];
                        positions[i * 3 + 2] = orders[i].target[2];
                        velocities[i * 3 + 0] = 0.0f;
                        velocities[i * 3 + 1] = 0.0f;
                        velocities[i * 3 + 2] = 0.0f;
                        orders[i].throttle = 0;
                        if (++orders[i].hold >= 60) {
                            orders[i].active = 0;
                            if (completedCount < 8192) {
                                completedOrders[completedCount++] = entityMake(indices[i], world->entities.generation[indices[i]]);
                            }
                        }
                        continue;
                    }
                    orders[i].hold = 0;
                    double approach = distance / ARRIVAL_TIME;
                    double braking = 0.9 * sqrt(2.0 * type->thrust / type->mass * distance);
                    double desiredSpeed = approach < type->maxSpeed ? approach : type->maxSpeed;
                    if (braking < desiredSpeed) {
                        desiredSpeed = braking;
                    }
                    double desired[3] = {0.0, 0.0, 0.0};
                    for (uint32_t axis = 0; axis < 3 && distance > 1e-6; axis++) {
                        desired[axis] = delta[axis] / distance * desiredSpeed;
                    }
                    orders[i].throttle = flyForward(type, positions + i * 3, velocities + i * 3,
                                                    orientations ? orientations + i * 4 : NULL, desired, step);
                    continue;
                }
                double damping = 1.0 / (1.0 + 1.5 * step);
                for (uint32_t axis = 0; axis < 3; axis++) {
                    velocities[i * 3 + axis] = (float)((double)velocities[i * 3 + axis] * damping);
                    double next = positions[i * 3 + axis] + (double)velocities[i * 3 + axis] * step;
                    if (next < boundsMinimum[axis] || next > boundsMaximum[axis]) {
                        velocities[i * 3 + axis] = -velocities[i * 3 + axis];
                        next = positions[i * 3 + axis];
                    }
                    positions[i * 3 + axis] = next;
                }
            }
        }
    }
    resolveCollisions(world);
    resolveRounds(world, step);
    simulateStations(world);
    for (uint32_t i = 0; i < completedCount; i++) {
        componentRemove(world, completedOrders[i], COMPONENT_ORDER);
    }
    for (uint32_t i = 0; i < destroyedCount; i++) {
        entityDestroy(world, destroyedShips[i]);
    }
    for (uint32_t i = 0; i < spentCount; i++) {
        entityDestroy(world, spentRounds[i]);
    }
    for (uint32_t i = 0; i < pendingCount; i++) {
        Entity round = entityCreate(world, roundMask());
        if (!round) {
            break;
        }
        double* position = componentOf(world, round, COMPONENT_POSITION);
        float* velocity = componentOf(world, round, COMPONENT_VELOCITY);
        int16_t* orientation = componentOf(world, round, COMPONENT_ORIENTATION);
        uint64_t* owner = componentOf(world, round, COMPONENT_OWNER);
        uint16_t* kind = componentOf(world, round, COMPONENT_KIND);
        uint32_t* system = componentOf(world, round, COMPONENT_SYSTEM);
        Round* shot = componentOf(world, round, COMPONENT_ROUND);
        memcpy(position, pendingShots[i].position, sizeof(pendingShots[i].position));
        memcpy(velocity, pendingShots[i].velocity, sizeof(pendingShots[i].velocity));
        memcpy(orientation, pendingShots[i].orientation, sizeof(pendingShots[i].orientation));
        *owner = pendingShots[i].owner;
        *kind = pendingShots[i].kind;
        *system = pendingShots[i].system;
        shot->damage = pendingShots[i].damage;
        shot->life = pendingShots[i].life;
    }
    for (uint32_t i = 0; i < spawnCount; i++) {
        Spawn* entry = &pendingSpawns[i];
        int structure = entry->kind == KIND_STATION;
        Entity made = entityCreate(world, structure ? stationMask() : shipMask());
        if (!made) {
            break;
        }
        const KindProfile* type = &kindProfiles[entry->kind % KIND_COUNT];
        double* position = componentOf(world, made, COMPONENT_POSITION);
        float* velocity = componentOf(world, made, COMPONENT_VELOCITY);
        int16_t* orientation = componentOf(world, made, COMPONENT_ORIENTATION);
        Damage* damage = componentOf(world, made, COMPONENT_DAMAGE);
        uint64_t* owner = componentOf(world, made, COMPONENT_OWNER);
        uint16_t* kind = componentOf(world, made, COMPONENT_KIND);
        uint32_t* system = componentOf(world, made, COMPONENT_SYSTEM);
        Cargo* hold = componentOf(world, made, COMPONENT_CARGO);
        memcpy(position, entry->position, sizeof(entry->position));
        double heading[3] = {1.0, 0.0, 0.0};
        double pace = sqrt(entry->velocity[0] * entry->velocity[0] + entry->velocity[1] * entry->velocity[1] +
                           entry->velocity[2] * entry->velocity[2]);
        for (uint32_t axis = 0; axis < 3; axis++) {
            velocity[axis] = (float)entry->velocity[axis];
            heading[axis] = pace > 1e-6 ? entry->velocity[axis] / pace : heading[axis];
        }
        orientationWrite(heading, orientation);
        damage->hull = (int32_t)((double)type->maxHull * (double)entry->hull / 255.0);
        damage->armor = (int32_t)((double)type->maxArmor * (double)entry->armor / 255.0);
        damage->shield = (int32_t)((double)type->maxShield * (double)entry->shield / 255.0);
        *owner = entry->owner;
        *kind = entry->kind;
        *system = entry->system;
        memset(hold, 0, sizeof(*hold));
        if (!structure) {
            continue;
        }
        Station* state = componentOf(world, made, COMPONENT_STATION);
        memset(state, 0, sizeof(*state));
        state->queueKind = QUEUE_NONE;
        state->build = entry->builder ? 0 : BUILD_FULL;
        if (!entry->builder) {
            continue;
        }
        Order* order = componentOf(world, entry->builder, COMPONENT_ORDER);
        if (order && order->type == ORDER_BUILD) {
            order->targetEntity = made;
        }
    }
}

static uint32_t collectSystem(World* world, uint32_t system, GatheredEntity* out, uint32_t capacity) {
    uint64_t required = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_SYSTEM);
    uint32_t written = 0;
    for (uint32_t a = 0; a < world->archetypeCount && written < capacity; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required || archetype->present[COMPONENT_ROUND]) {
            continue;
        }
        int hasOrientation = archetype->present[COMPONENT_ORIENTATION];
        int hasKind = archetype->present[COMPONENT_KIND];
        int hasOwner = archetype->present[COMPONENT_OWNER];
        int hasDamage = archetype->present[COMPONENT_DAMAGE];
        int hasOrder = archetype->present[COMPONENT_ORDER];
        int hasCargo = archetype->present[COMPONENT_CARGO];
        int hasStation = archetype->present[COMPONENT_STATION];
        for (uint32_t slot = 0; slot < archetype->chunkCount && written < capacity; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            double* positions = chunkColumn(world, archetype, slot, COMPONENT_POSITION);
            uint32_t* systems = chunkColumn(world, archetype, slot, COMPONENT_SYSTEM);
            int16_t* orientations = hasOrientation ? chunkColumn(world, archetype, slot, COMPONENT_ORIENTATION) : NULL;
            uint16_t* kinds = hasKind ? chunkColumn(world, archetype, slot, COMPONENT_KIND) : NULL;
            uint64_t* owners = hasOwner ? chunkColumn(world, archetype, slot, COMPONENT_OWNER) : NULL;
            Damage* damages = hasDamage ? chunkColumn(world, archetype, slot, COMPONENT_DAMAGE) : NULL;
            Order* running = hasOrder ? chunkColumn(world, archetype, slot, COMPONENT_ORDER) : NULL;
            Cargo* holds = hasCargo ? chunkColumn(world, archetype, slot, COMPONENT_CARGO) : NULL;
            Station* states = hasStation ? chunkColumn(world, archetype, slot, COMPONENT_STATION) : NULL;
            for (uint32_t i = 0; i < fill && written < capacity; i++) {
                if (systems[i] != system) {
                    continue;
                }
                GatheredEntity* entry = &out[written++];
                memset(entry, 0, sizeof(*entry));
                entry->entity = entityMake(indices[i], world->entities.generation[indices[i]]);
                entry->position[0] = positions[i * 3 + 0];
                entry->position[1] = positions[i * 3 + 1];
                entry->position[2] = positions[i * 3 + 2];
                if (orientations) {
                    memcpy(entry->orientation, orientations + i * 4, sizeof(entry->orientation));
                }
                entry->kind = kinds ? (uint8_t)kinds[i] : 0;
                entry->owner = owners ? (uint32_t)owners[i] : 0;
                entry->throttle = running && running[i].active ? running[i].throttle : 0;
                entry->target = running && running[i].active ? (uint32_t)running[i].targetEntity : 0;
                if (running && running[i].active && running[i].hold &&
                    (running[i].type == ORDER_MINE || running[i].type == ORDER_BUILD)) {
                    entry->target = running[i].hold;
                }
                if (damages) {
                    const KindProfile* type = &kindProfiles[entry->kind % KIND_COUNT];
                    entry->hull = healthFraction(damages[i].hull, type->maxHull);
                    entry->armor = healthFraction(damages[i].armor, type->maxArmor);
                    entry->shield = healthFraction(damages[i].shield, type->maxShield);
                }
                if (holds && entry->kind < KIND_COUNT) {
                    entry->cargo = cargoFill(&holds[i], kindProfiles[entry->kind].hold);
                }
                if (states) {
                    entry->throttle = (uint8_t)(states[i].build * 255 / BUILD_FULL);
                }
            }
        }
    }
    return written;
}

static GatheredEntity gathered[GATHER_CAP];
static uint32_t gatheredCount;
static uint32_t gatheredSystem;
static int gatheredValid;
static uint32_t interestHead[INTEREST_SLOTS];
static uint32_t interestNext[GATHER_CAP];
static uint32_t interestStamp[GATHER_CAP];
static uint32_t interestEpoch;
static uint32_t interestPick[GATHER_CAP];
static double interestRange[GATHER_CAP];

static int32_t interestCellOf(double value) {
    return (int32_t)floor(value / INTEREST_CELL);
}

static uint32_t interestHash(int32_t x, int32_t y, int32_t z) {
    uint32_t hash = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    hash ^= hash >> 13;
    return hash & (INTEREST_SLOTS - 1);
}

static void gatherSystem(World* world, uint32_t system) {
    if (gatheredValid && gatheredSystem == system) {
        return;
    }
    gatheredSystem = system;
    gatheredValid = 1;
    gatheredCount = collectSystem(world, system, gathered, GATHER_CAP);
    for (uint32_t i = 0; i < INTEREST_SLOTS; i++) {
        interestHead[i] = INVALID;
    }
    for (uint32_t i = 0; i < gatheredCount; i++) {
        uint32_t cell = interestHash(interestCellOf(gathered[i].position[0]),
                                     interestCellOf(gathered[i].position[1]),
                                     interestCellOf(gathered[i].position[2]));
        interestNext[i] = interestHead[cell];
        interestHead[cell] = i;
    }
}

static void interestSink(uint32_t slot, uint32_t count) {
    for (;;) {
        uint32_t larger = slot;
        uint32_t left = slot * 2 + 1;
        uint32_t right = left + 1;
        if (left < count && interestRange[interestPick[left]] > interestRange[interestPick[larger]]) {
            larger = left;
        }
        if (right < count && interestRange[interestPick[right]] > interestRange[interestPick[larger]]) {
            larger = right;
        }
        if (larger == slot) {
            return;
        }
        uint32_t swap = interestPick[slot];
        interestPick[slot] = interestPick[larger];
        interestPick[larger] = swap;
        slot = larger;
    }
}

static void interestOffer(uint32_t entry, uint32_t* count, uint32_t capacity) {
    if (*count < capacity) {
        uint32_t slot = (*count)++;
        interestPick[slot] = entry;
        while (slot && interestRange[interestPick[(slot - 1) / 2]] < interestRange[interestPick[slot]]) {
            uint32_t swap = interestPick[slot];
            interestPick[slot] = interestPick[(slot - 1) / 2];
            interestPick[(slot - 1) / 2] = swap;
            slot = (slot - 1) / 2;
        }
        return;
    }
    if (interestRange[entry] >= interestRange[interestPick[0]]) {
        return;
    }
    interestPick[0] = entry;
    interestSink(0, capacity);
}

static uint32_t selectInterest(uint64_t player, GatheredEntity* out, uint32_t capacity) {
    interestEpoch++;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < gatheredCount; i++) {
        if (gathered[i].kind >= KIND_STAR) {
            interestStamp[i] = interestEpoch;
        }
    }
    for (uint32_t own = 0; own < gatheredCount; own++) {
        if (gathered[own].owner != (uint32_t)player || gathered[own].kind >= KIND_STAR) {
            continue;
        }
        double reach = kindProfiles[gathered[own].kind % KIND_COUNT].sensorRange;
        int32_t base[3] = {
            interestCellOf(gathered[own].position[0]),
            interestCellOf(gathered[own].position[1]),
            interestCellOf(gathered[own].position[2])
        };
        int32_t span = (int32_t)(reach / INTEREST_CELL) + 1;
        for (int32_t x = -span; x <= span; x++) {
            for (int32_t y = -span; y <= span; y++) {
                for (int32_t z = -span; z <= span; z++) {
                    uint32_t cell = interestHash(base[0] + x, base[1] + y, base[2] + z);
                    for (uint32_t j = interestHead[cell]; j != INVALID; j = interestNext[j]) {
                        if (interestStamp[j] == interestEpoch) {
                            continue;
                        }
                        double gap = 0.0;
                        for (uint32_t axis = 0; axis < 3; axis++) {
                            double delta = gathered[j].position[axis] - gathered[own].position[axis];
                            gap += delta * delta;
                        }
                        if (gap > reach * reach) {
                            continue;
                        }
                        interestStamp[j] = interestEpoch;
                        interestRange[j] = gap;
                        interestOffer(j, &chosen, capacity);
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < chosen; i++) {
        out[i] = gathered[interestPick[i]];
    }
    return chosen;
}

static uint32_t collectCelestials(SnapshotCelestial* out, uint32_t capacity) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < gatheredCount && written < capacity; i++) {
        if (gathered[i].kind < KIND_STAR || gathered[i].kind >= KIND_ROUND) {
            continue;
        }
        SnapshotCelestial* entry = &out[written++];
        entry->position[0] = gathered[i].position[0];
        entry->position[1] = gathered[i].position[1];
        entry->position[2] = gathered[i].position[2];
        entry->entity = entityCompact(gathered[i].entity);
        entry->kind = gathered[i].kind;
        entry->padding[0] = 0;
        entry->padding[1] = 0;
        entry->padding[2] = 0;
    }
    return written;
}

static uint32_t collectShots(uint32_t system, SnapshotShot* out, uint32_t capacity) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < pendingCount && written < capacity; i++) {
        if (pendingShots[i].system != system) {
            continue;
        }
        SnapshotShot* entry = &out[written++];
        memcpy(entry->position, pendingShots[i].position, sizeof(entry->position));
        memcpy(entry->velocity, pendingShots[i].velocity, sizeof(entry->velocity));
        entry->owner = (uint32_t)pendingShots[i].owner;
        entry->kind = pendingShots[i].kind;
        entry->life = pendingShots[i].life;
    }
    return written;
}

static int positionEncodable(const GatheredEntity* entry, const double* origin) {
    for (uint32_t axis = 0; axis < 3; axis++) {
        double offset = (entry->position[axis] - origin[axis]) * 100.0;
        if (offset < -2147483000.0 || offset > 2147483000.0) {
            return 0;
        }
    }
    return 1;
}

static void encodeEntity(const GatheredEntity* entry, const double* origin, SnapshotEntity* out) {
    out->entity = entityCompact(entry->entity);
    for (uint32_t axis = 0; axis < 3; axis++) {
        out->position[axis] = (int32_t)((entry->position[axis] - origin[axis]) * 100.0);
    }
    memcpy(out->orientation, entry->orientation, sizeof(out->orientation));
    out->target = entry->target;
    out->owner = (uint16_t)entry->owner;
    out->kind = entry->kind;
    out->hull = entry->hull;
    out->armor = entry->armor;
    out->shield = entry->shield;
    out->throttle = entry->throttle;
    out->cargo = entry->cargo;
}

static uint32_t collectStations(World* world, uint64_t player, uint32_t system, SnapshotStation* out, uint32_t capacity) {
    uint64_t required = (1ull << COMPONENT_OWNER) | (1ull << COMPONENT_SYSTEM) |
                        (1ull << COMPONENT_CARGO) | (1ull << COMPONENT_STATION);
    uint32_t written = 0;
    for (uint32_t a = 0; a < world->archetypeCount && written < capacity; a++) {
        Archetype* archetype = &world->archetypes[a];
        if ((archetype->mask & required) != required) {
            continue;
        }
        for (uint32_t slot = 0; slot < archetype->chunkCount && written < capacity; slot++) {
            uint32_t fill = chunkFill(archetype, slot);
            uint32_t* indices = chunkEntities(world, archetype, slot);
            uint64_t* owners = chunkColumn(world, archetype, slot, COMPONENT_OWNER);
            uint32_t* systems = chunkColumn(world, archetype, slot, COMPONENT_SYSTEM);
            Cargo* stores = chunkColumn(world, archetype, slot, COMPONENT_CARGO);
            Station* states = chunkColumn(world, archetype, slot, COMPONENT_STATION);
            for (uint32_t i = 0; i < fill && written < capacity; i++) {
                if (owners[i] != player || systems[i] != system) {
                    continue;
                }
                SnapshotStation* entry = &out[written++];
                entry->entity = entityCompact(entityMake(indices[i], world->entities.generation[indices[i]]));
                memcpy(entry->store, stores[i].ore, sizeof(entry->store));
                entry->queueKind = states[i].queueKind;
                entry->queueTicks = states[i].queueTicks;
                entry->queueTotal = states[i].queueKind == QUEUE_NONE
                                        ? 0 : kindProfiles[states[i].queueKind % KIND_COUNT].buildTicks;
                entry->build = states[i].build;
                entry->stored = states[i].stored;
                entry->pending = states[i].pending;
            }
        }
    }
    return written;
}

static void broadcast(World* world, int socketDescriptor, uint64_t currentTick) {
    static GatheredEntity snapshot[SNAPSHOT_PER_PACKET * SNAPSHOT_PACKETS_PER_TICK];
    static SnapshotShot salvo[SHOTS_PER_PACKET * SHOT_PACKETS_PER_TICK];
    static unsigned char body[PACKET_CAP];
    gatheredValid = 0;
    for (uint32_t i = 0; i < SESSION_SLOTS; i++) {
        Session* session = &sessions[i];
        if (session->state != SLOT_ACTIVE) {
            continue;
        }
        if (currentTick - session->lastTick > SESSION_TIMEOUT_TICKS) {
            sessionRelease(session);
            continue;
        }
        gatherSystem(world, session->system);
        if (currentTick - session->celestialTick >= CELESTIAL_PERIOD) {
            session->celestialTick = currentTick;
            static SnapshotCelestial sky[CELESTIAL_CAP];
            uint32_t bodies = collectCelestials(sky, CELESTIAL_CAP);
            uint32_t tick = (uint32_t)currentTick;
            memcpy(body, &tick, sizeof(tick));
            memcpy(body + 4, &bodies, sizeof(bodies));
            memcpy(body + 8, sky, bodies * sizeof(SnapshotCelestial));
            sendPacket(socketDescriptor, session->address, session->port, PACKET_CELESTIALS, (uint32_t)currentTick,
                       body, (uint16_t)(CELESTIAL_HEADER + bodies * sizeof(SnapshotCelestial)));
        }
        if ((currentTick & 3) == 0) {
            static SnapshotStation yards[SNAPSHOT_YARDS];
            Player* player = playerOf(session->player);
            uint32_t kind = player && player->active ? (player->active - 1) % KIND_COUNT : 0;
            SnapshotYards header = {
                .tick = (uint32_t)currentTick,
                .count = collectStations(world, session->player, session->system, yards, SNAPSHOT_YARDS),
                .unlocked = player ? player->unlocked : 0,
                .researchKind = player && player->active ? kind : 0xFFFFFFFFu,
                .researchProgress = player && player->active ? player->progress[kind] : 0,
                .researchTotal = player && player->active ? kindProfiles[kind].researchTicks : 0
            };
            memcpy(body, &header, sizeof(header));
            memcpy(body + sizeof(header), yards, header.count * sizeof(SnapshotStation));
            sendPacket(socketDescriptor, session->address, session->port, PACKET_STATIONS, (uint32_t)currentTick,
                       body, (uint16_t)(sizeof(header) + header.count * sizeof(SnapshotStation)));
        }
        uint32_t total = selectInterest(session->player, snapshot, SNAPSHOT_PER_PACKET * SNAPSHOT_PACKETS_PER_TICK);
        uint32_t bounds[SNAPSHOT_PACKETS_PER_TICK + 1];
        uint8_t packets = 0;
        uint32_t offset = 0;
        while (packets < SNAPSHOT_PACKETS_PER_TICK && (offset < total || !packets)) {
            bounds[packets++] = offset;
            if (offset >= total) {
                break;
            }
            double origin[3] = {snapshot[offset].position[0], snapshot[offset].position[1], snapshot[offset].position[2]};
            uint32_t count = 1;
            while (offset + count < total && count < SNAPSHOT_PER_PACKET &&
                   positionEncodable(&snapshot[offset + count], origin)) {
                count++;
            }
            offset += count;
        }
        total = offset;
        bounds[packets] = total;
        for (uint8_t index = 0; index < packets; index++) {
            uint32_t start = bounds[index];
            uint16_t count = (uint16_t)(bounds[index + 1] - start);
            uint32_t tick = (uint32_t)currentTick;
            double origin[3] = {0.0, 0.0, 0.0};
            if (count) {
                origin[0] = snapshot[start].position[0];
                origin[1] = snapshot[start].position[1];
                origin[2] = snapshot[start].position[2];
            }
            memcpy(body, &tick, sizeof(tick));
            memcpy(body + 4, &count, sizeof(count));
            body[6] = index;
            body[7] = packets;
            memcpy(body + 8, origin, sizeof(origin));
            for (uint32_t i = 0; i < count; i++) {
                SnapshotEntity wire;
                encodeEntity(&snapshot[start + i], origin, &wire);
                memcpy(body + SNAPSHOT_HEADER + i * sizeof(SnapshotEntity), &wire, sizeof(wire));
            }
            sendPacket(socketDescriptor, session->address, session->port, PACKET_SNAPSHOT, (uint32_t)currentTick,
                       body, (uint16_t)(SNAPSHOT_HEADER + count * sizeof(SnapshotEntity)));
        }
        uint32_t shots = collectShots(session->system, salvo, SHOTS_PER_PACKET * SHOT_PACKETS_PER_TICK);
        for (uint32_t fired = 0; fired < shots;) {
            uint32_t count = shots - fired;
            if (count > SHOTS_PER_PACKET) {
                count = SHOTS_PER_PACKET;
            }
            uint64_t tick = currentTick;
            memcpy(body, &tick, sizeof(tick));
            memcpy(body + 8, &count, sizeof(count));
            memcpy(body + SHOTS_HEADER, salvo + fired, count * sizeof(SnapshotShot));
            sendPacket(socketDescriptor, session->address, session->port, PACKET_SHOTS, (uint32_t)currentTick,
                       body, (uint16_t)(SHOTS_HEADER + count * sizeof(SnapshotShot)));
            fired += count;
        }
    }
}

#ifndef SERENITAS_TESTS
static int runServer(void) {
    static World world;
    if (!worldCreate(&world, 1000000, (uint64_t)8 << 30)) {
        logError("reservation failed\n");
        return 1;
    }
    int socketDescriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketDescriptor < 0) {
        logError("socket failed: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(SERVER_PORT), .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
    if (bind(socketDescriptor, (struct sockaddr*)&address, sizeof(address)) < 0) {
        logError("bind failed: %s\n", strerror(errno));
        return 1;
    }
    if (!setNonBlocking(socketDescriptor)) {
        logError("nonblocking failed\n");
        return 1;
    }
    int timerDescriptor = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec interval = {
        .it_interval = {.tv_sec = 0, .tv_nsec = TICK_NANOSECONDS},
        .it_value = {.tv_sec = 0, .tv_nsec = TICK_NANOSECONDS}
    };
    if (timerDescriptor < 0 || timerfd_settime(timerDescriptor, 0, &interval, NULL) < 0) {
        logError("timer failed: %s\n", strerror(errno));
        return 1;
    }
    int epollDescriptor = epoll_create1(0);
    struct epoll_event socketEvent = {.events = EPOLLIN, .data = {.fd = socketDescriptor}};
    struct epoll_event timerEvent = {.events = EPOLLIN, .data = {.fd = timerDescriptor}};
    if (epollDescriptor < 0 ||
        epoll_ctl(epollDescriptor, EPOLL_CTL_ADD, socketDescriptor, &socketEvent) < 0 ||
        epoll_ctl(epollDescriptor, EPOLL_CTL_ADD, timerDescriptor, &timerEvent) < 0) {
        logError("epoll failed: %s\n", strerror(errno));
        return 1;
    }
    commandWorld = &world;
    uint64_t celestialMask = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_KIND) | (1ull << COMPONENT_SYSTEM);
    static const double celestialPlace[3][4] = {
        {0.0, 0.0, 0.0, KIND_STAR},
        {0.82 * ASTRONOMICAL_UNIT, 0.0, 0.31 * ASTRONOMICAL_UNIT, KIND_PLANET_ROCK},
        {-0.95 * ASTRONOMICAL_UNIT, 0.04 * ASTRONOMICAL_UNIT, 1.4 * ASTRONOMICAL_UNIT, KIND_PLANET_ICE}
    };
    for (uint32_t i = 0; i < 3; i++) {
        Entity body = entityCreate(&world, celestialMask);
        if (!body) {
            break;
        }
        double* position = componentOf(&world, body, COMPONENT_POSITION);
        uint16_t* kind = componentOf(&world, body, COMPONENT_KIND);
        uint32_t* system = componentOf(&world, body, COMPONENT_SYSTEM);
        position[0] = celestialPlace[i][0];
        position[1] = celestialPlace[i][1];
        position[2] = celestialPlace[i][2];
        *kind = (uint16_t)celestialPlace[i][3];
        *system = 0;
    }
    seedRocks(&world, BELT_ROCKS);
    logDebug("listening on udp %d, tick %.1f ms, %u rocks in system 0\n",
             SERVER_PORT, TICK_NANOSECONDS / 1e6, BELT_ROCKS);

    uint64_t currentTick = 0;
    unsigned char buffer[PACKET_CAP];
#ifdef SERENITAS_DEBUG
    uint64_t received = 0;
    uint64_t sent = 0;
    uint64_t lateTicks = 0;
    double worstTick = 0.0;
    double report = seconds();
#endif
    for (;;) {
        struct epoll_event events[4];
        int ready = epoll_wait(epollDescriptor, events, 4, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            logError("epoll_wait failed: %s\n", strerror(errno));
            return 1;
        }
        for (int i = 0; i < ready; i++) {
            if (events[i].data.fd == socketDescriptor) {
                for (;;) {
                    struct sockaddr_in from;
                    socklen_t fromSize = sizeof(from);
                    ssize_t size = recvfrom(socketDescriptor, buffer, sizeof(buffer), 0, (struct sockaddr*)&from, &fromSize);
                    if (size < 0) {
                        break;
                    }
#ifdef SERENITAS_DEBUG
                    received++;
#endif
                    handlePacket(socketDescriptor, &from, buffer, (uint32_t)size, currentTick);
                }
            } else if (events[i].data.fd == timerDescriptor) {
                uint64_t expirations = 0;
                if (read(timerDescriptor, &expirations, sizeof(expirations)) != sizeof(expirations)) {
                    continue;
                }
#ifdef SERENITAS_DEBUG
                if (expirations > 1) {
                    lateTicks += expirations - 1;
                }
                double tickStart = seconds();
#endif
                for (uint64_t step = 0; step < expirations; step++) {
                    currentTick++;
                    simulate(&world, TICK_NANOSECONDS / 1e9);
                }
                broadcast(&world, socketDescriptor, currentTick);
#ifdef SERENITAS_DEBUG
                sent += sessionCount;
                double elapsed = seconds() - tickStart;
                if (elapsed > worstTick) {
                    worstTick = elapsed;
                }
                if (seconds() - report >= 5.0) {
                    uint32_t total = 0;
                    uint32_t ordered = 0;
                    for (uint32_t a = 0; a < world.archetypeCount; a++) {
                        total += world.archetypes[a].entityCount;
                        if (world.archetypes[a].present[COMPONENT_ORDER]) {
                            ordered += world.archetypes[a].entityCount;
                        }
                    }
                    logDebug("tick %llu  sessions %u  entities %u  ordered %u  in %llu  out %llu  late %llu  worst %.3f ms\n",
                             (unsigned long long)currentTick, sessionCount, total, ordered,
                             (unsigned long long)received, (unsigned long long)sent,
                             (unsigned long long)lateTicks, worstTick * 1000.0);
                    report = seconds();
                    worstTick = 0.0;
                }
#endif
            }
        }
    }
}

int main(void) {
    return runServer();
}
#endif
