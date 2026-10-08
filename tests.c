#define SERENITAS_TESTS
#include "server.c"

static const char* componentNames[COMPONENT_COUNT] = {"position", "velocity", "orientation", "damage", "owner", "kind", "system", "order", "round", "cargo", "station"};

static uint64_t worldBytes(World* world) {
    uint64_t total = world->pool.committed;
    total += world->entities.generationCommitted + world->entities.rowCommitted + world->entities.archetypeCommitted;
    for (uint32_t i = 0; i < world->archetypeCount; i++) {
        total += world->archetypes[i].chunksCommitted;
    }
    return total;
}

static uint64_t processBytes(void) {
    FILE* file = fopen("/proc/self/statm", "r");
    if (!file) {
        return 0;
    }
    unsigned long long total = 0;
    unsigned long long resident = 0;
    if (fscanf(file, "%llu %llu", &total, &resident) != 2) {
        resident = 0;
    }
    fclose(file);
    return resident * (unsigned long long)sysconf(_SC_PAGESIZE);
}

static void reportArchetypes(World* world) {
    printf("%-4s %-52s %9s %8s %10s %10s\n", "id", "components", "capacity", "chunks", "entities", "B/entity");
    for (uint32_t i = 0; i < world->archetypeCount; i++) {
        Archetype* archetype = &world->archetypes[i];
        char components[80];
        uint32_t written = 0;
        components[0] = 0;
        for (uint32_t component = 0; component < COMPONENT_COUNT; component++) {
            if (!archetype->present[component]) {
                continue;
            }
            int length = snprintf(components + written, sizeof(components) - written, written ? " %s" : "%s", componentNames[component]);
            if (length < 0 || (uint32_t)length >= sizeof(components) - written) {
                break;
            }
            written += (uint32_t)length;
        }
        double perEntity = (double)CHUNK_SIZE / (double)archetype->capacity;
        printf("%-4u %-52s %9u %8u %10u %10.2f\n", i, components, archetype->capacity, archetype->chunkCount, archetype->entityCount, perEntity);
    }
}

static void reportDensity(World* world, const char* label, uint32_t count) {
    uint64_t bytes = worldBytes(world);
    double perEntity = (double)bytes / (double)count;
    printf("%-20s %8.1f MiB   %6.2f B/entity   %5.2f M per GiB   working set %.1f MiB\n",
           label, (double)bytes / (1024.0 * 1024.0), perEntity,
           (1073741824.0 / perEntity) / 1e6, (double)processBytes() / (1024.0 * 1024.0));
}

static void benchCommand(Session* commander, const double* point, Entity mark, uint32_t type, float radius, Entity ship) {
    unsigned char body[52];
    uint32_t markCompact = mark ? entityCompact(mark) : 0;
    uint32_t shipCompact = entityCompact(ship);
    uint32_t single = 1;
    memset(body, 0, sizeof(body));
    memcpy(body, point, sizeof(double) * 3);
    memcpy(body + 24, &markCompact, sizeof(markCompact));
    memcpy(body + 32, &single, sizeof(single));
    memcpy(body + 36, &type, sizeof(type));
    memcpy(body + 40, &radius, sizeof(radius));
    memcpy(body + 48, &shipCompact, sizeof(shipCompact));
    applyCommand(commander, body, sizeof(body));
}

static void benchProduce(Session* commander, Entity yard, uint16_t kind, uint16_t mode) {
    unsigned char body[8];
    uint32_t yardCompact = entityCompact(yard);
    memcpy(body, &yardCompact, sizeof(yardCompact));
    memcpy(body + 4, &kind, sizeof(kind));
    memcpy(body + 6, &mode, sizeof(mode));
    applyProduce(commander, body, sizeof(body));
}

static Entity benchRock(World* world, const double* place, uint32_t ore) {
    Entity rock = entityCreate(world, rockMask());
    if (!rock) {
        return 0;
    }
    uint16_t kind = (uint16_t)(KIND_ROCK + ore);
    double* position = componentOf(world, rock, COMPONENT_POSITION);
    float* velocity = componentOf(world, rock, COMPONENT_VELOCITY);
    int16_t* orientation = componentOf(world, rock, COMPONENT_ORIENTATION);
    uint16_t* rockKind = componentOf(world, rock, COMPONENT_KIND);
    uint32_t* system = componentOf(world, rock, COMPONENT_SYSTEM);
    Cargo* seam = componentOf(world, rock, COMPONENT_CARGO);
    memcpy(position, place, sizeof(double) * 3);
    memset(velocity, 0, sizeof(float) * 3);
    orientation[0] = 0;
    orientation[1] = 0;
    orientation[2] = 0;
    orientation[3] = 32767;
    *rockKind = kind;
    *system = 0;
    memset(seam, 0, sizeof(*seam));
    seam->ore[ore] = kindProfiles[kind].hold;
    return rock;
}

static int runTests(void) {
    static World world;
    const uint32_t ships = 2000000;
    const uint32_t objects = 8000000;
    const uint32_t count = ships + objects;
    if (!worldCreate(&world, count + (count >> 3), (uint64_t)48 << 30)) {
        printf("reservation failed\n");
        return 1;
    }
    uint64_t commandableMask = shipMask();
    uint64_t objectMask = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_KIND);

    double start = seconds();
    Entity first = 0;
    Entity last = 0;
    for (uint32_t i = 0; i < count; i++) {
        int isShip = i < ships;
        Entity entity = entityCreate(&world, isShip ? commandableMask : objectMask);
        if (!entity) {
            printf("creation failed at %u\n", i);
            return 1;
        }
        if (i == 0) {
            first = entity;
        }
        last = entity;
        double* position = componentOf(&world, entity, COMPONENT_POSITION);
        position[0] = (double)i;
        position[1] = 0.0;
        position[2] = (double)-i;
        if (isShip) {
            float* velocity = componentOf(&world, entity, COMPONENT_VELOCITY);
            Damage* damage = componentOf(&world, entity, COMPONENT_DAMAGE);
            velocity[0] = 1.0f;
            velocity[1] = 0.5f;
            velocity[2] = 0.25f;
            damage->hull = 4200;
            damage->armor = 6800;
            damage->shield = 5300;
            }
    }
    double createTime = seconds() - start;

    printf("population          %u ships + %u inert objects\n\n", ships, objects);
    reportArchetypes(&world);
    printf("\n");
    reportDensity(&world, "mixed world", count);
    double shipPerEntity = (double)CHUNK_SIZE / (double)world.archetypes[0].capacity + 10.0;
    printf("%-20s %8s       %6.2f B/entity   %5.2f M per GiB   (if every entity were a ship)\n",
           "all damageable", "", shipPerEntity, (1073741824.0 / shipPerEntity) / 1e6);
    printf("create              %.2f s (%.1f M/s)\n\n", createTime, (double)count / createTime / 1e6);

    uint64_t moveMask = (1ull << COMPONENT_POSITION) | (1ull << COMPONENT_VELOCITY);
    uint32_t moved = 0;
    start = seconds();
    for (uint32_t pass = 0; pass < 4; pass++) {
        moved = 0;
        for (uint32_t a = 0; a < world.archetypeCount; a++) {
            Archetype* archetype = &world.archetypes[a];
            if ((archetype->mask & moveMask) != moveMask) {
                continue;
            }
            for (uint32_t slot = 0; slot < archetype->chunkCount; slot++) {
                uint32_t fill = chunkFill(archetype, slot);
                double* positions = chunkColumn(&world, archetype, slot, COMPONENT_POSITION);
                float* velocities = chunkColumn(&world, archetype, slot, COMPONENT_VELOCITY);
                for (uint32_t i = 0; i < fill; i++) {
                    positions[i * 3 + 0] += (double)velocities[i * 3 + 0] * 0.0625;
                    positions[i * 3 + 1] += (double)velocities[i * 3 + 1] * 0.0625;
                    positions[i * 3 + 2] += (double)velocities[i * 3 + 2] * 0.0625;
                }
                moved += fill;
            }
        }
    }
    double moveTime = (seconds() - start) / 4.0;
    double* sample = componentOf(&world, first, COMPONENT_POSITION);
    printf("movement query      %u entities matched, %.2f ms (%.1f M/s)\n", moved, moveTime * 1000.0, (double)moved / moveTime / 1e6);
    printf("first position x    %.4f, alive %d, last alive %d\n\n", sample[0], entityAlive(&world, first), entityAlive(&world, last));

    uint32_t ordered = ships / 10;
    start = seconds();
    for (uint32_t i = 0; i < ordered; i++) {
        Entity ship = entityMake(i, world.entities.generation[i]);
        if (componentAdd(&world, ship, COMPONENT_ORDER)) {
            Order* order = componentOf(&world, ship, COMPONENT_ORDER);
            order->target[0] = 1.0;
            order->active = 1;
        }
    }
    double migrateTime = seconds() - start;
    printf("gave orders to      %u ships in %.3f s (%.2f M migrations/s)\n", ordered, migrateTime, (double)ordered / migrateTime / 1e6);
    reportArchetypes(&world);
    reportDensity(&world, "with 10% ordered", count);
    uint32_t sampleIndex = ordered / 2;
    Order* sampleOrder = componentOf(&world, entityMake(sampleIndex, world.entities.generation[sampleIndex]), COMPONENT_ORDER);
    double* movedPosition = componentOf(&world, entityMake(sampleIndex, world.entities.generation[sampleIndex]), COMPONENT_POSITION);
    printf("migrated entity %u   order target %.1f active %u, position preserved %.4f (expected %.4f)\n\n",
           sampleIndex, sampleOrder ? sampleOrder->target[0] : -1.0, sampleOrder ? sampleOrder->active : 0,
           movedPosition ? movedPosition[0] : -1.0, (double)sampleIndex + 0.25);
    for (uint32_t i = 0; i < ordered; i++) {
        componentRemove(&world, entityMake(i, world.entities.generation[i]), COMPONENT_ORDER);
    }
    printf("orders cleared      archetype 0 back to %u entities\n\n", world.archetypes[0].entityCount);

    Entity stale = first;
    entityDestroy(&world, first);
    printf("after destroy       alive %d, stale handle alive %d\n", entityAlive(&world, first), entityAlive(&world, stale));
    Entity recycled = entityCreate(&world, commandableMask);
    printf("recycled index      %u (was %u), generation %u, stale still dead %d\n",
           entityIndexOf(recycled), entityIndexOf(first), entityGenerationOf(recycled), entityAlive(&world, stale));

    uint32_t before = world.archetypes[0].entityCount;
    uint32_t beforeChunks = world.archetypes[0].chunkCount;
    for (uint32_t i = 0; i < 1000000; i++) {
        entityDestroy(&world, entityMake(i + 1000, world.entities.generation[i + 1000]));
    }
    printf("destroyed 1M ships  %u -> %u entities, %u -> %u chunks\n\n",
           before, world.archetypes[0].entityCount, beforeChunks, world.archetypes[0].chunkCount);
    reportDensity(&world, "after destroy", count - 1000000);

    static World physics;
    if (!worldCreate(&physics, 64, (uint64_t)1 << 24)) {
        return 1;
    }
    printf("\nphysics: one ship of each class ordered from origin to x = 5000 m\n");
    printf("%-6s %8s %8s %9s %10s %10s %9s\n", "class", "mass", "thrust", "accel", "max speed", "arrived s", "peak speed");
    for (uint32_t kind = 0; kind <= KIND_DRONE; kind++) {
        Entity ship = entityCreate(&physics, commandableMask);
        double* position = componentOf(&physics, ship, COMPONENT_POSITION);
        float* velocity = componentOf(&physics, ship, COMPONENT_VELOCITY);
        uint16_t* shipKind = componentOf(&physics, ship, COMPONENT_KIND);
        position[0] = 0.0;
        position[1] = 0.0;
        position[2] = 0.0;
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
        velocity[2] = 0.0f;
        *shipKind = (uint16_t)kind;
        componentAdd(&physics, ship, COMPONENT_ORDER);
        Order* order = componentOf(&physics, ship, COMPONENT_ORDER);
        order->target[0] = 5000.0;
        order->target[1] = 0.0;
        order->target[2] = 0.0;
        order->active = 1;
        double peak = 0.0;
        uint32_t ticks = 0;
        for (; ticks < 4000; ticks++) {
            simulate(&physics, 0.05);
            float* current = componentOf(&physics, ship, COMPONENT_VELOCITY);
            double speed = sqrt((double)current[0] * current[0] + (double)current[1] * current[1] + (double)current[2] * current[2]);
            if (speed > peak) {
                peak = speed;
            }
            if (!componentOf(&physics, ship, COMPONENT_ORDER)) {
                break;
            }
        }
        const KindProfile* type = &kindProfiles[kind];
        printf("%-6u %8.0f %8.0f %9.3f %10.2f %10.2f %9.3f\n", kind, type->mass, type->thrust,
               type->thrust / type->mass, type->maxSpeed, (double)(ticks + 1) * 0.05, peak);
        entityDestroy(&physics, ship);
    }

    printf("\nformation: cuboid slots for 12 ships, spacing 200 m\n");
    double previous[3] = {0.0, 0.0, 0.0};
    double minimumGap = 1e9;
    for (uint32_t i = 0; i < 12; i++) {
        double offset[3];
        formationSlot(i, 12, 200.0, offset);
        if (i) {
            double gap = 1e9;
            for (uint32_t j = 0; j < i; j++) {
                double other[3];
                formationSlot(j, 12, 0.44, other);
                double d = sqrt((offset[0] - other[0]) * (offset[0] - other[0]) +
                                (offset[1] - other[1]) * (offset[1] - other[1]) +
                                (offset[2] - other[2]) * (offset[2] - other[2]));
                if (d < gap) {
                    gap = d;
                }
            }
            if (gap < minimumGap) {
                minimumGap = gap;
            }
        }
        previous[0] = offset[0];
        printf("  slot %2u  %6.2f %6.2f %6.2f\n", i, offset[0], offset[1], offset[2]);
    }
    printf("  closest pair %.1f m (spacing 200.0)\n", minimumGap);

    printf("\ncollision: 12 ships released from one point, radius 0.17\n");
    Entity stack[12];
    for (uint32_t i = 0; i < 12; i++) {
        stack[i] = entityCreate(&physics, commandableMask);
        double* position = componentOf(&physics, stack[i], COMPONENT_POSITION);
        float* velocity = componentOf(&physics, stack[i], COMPONENT_VELOCITY);
        uint16_t* kind = componentOf(&physics, stack[i], COMPONENT_KIND);
        position[0] = 0.05 * (double)i;
        position[1] = 0.0;
        position[2] = 0.0;
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
        velocity[2] = 0.0f;
        *kind = 1;
    }
    for (uint32_t tick = 0; tick < 400; tick++) {
        simulate(&physics, 0.05);
    }
    double closest = 1e9;
    for (uint32_t i = 0; i < 12; i++) {
        for (uint32_t j = i + 1; j < 12; j++) {
            double* a = componentOf(&physics, stack[i], COMPONENT_POSITION);
            double* b = componentOf(&physics, stack[j], COMPONENT_POSITION);
            double d = sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            if (d < closest) {
                closest = d;
            }
        }
    }
    printf("  closest pair after 20 s: %.4f, contact distance %.4f, pairs touched last tick %u\n",
           closest, 2.0 * kindProfiles[1].radius, collisionPairs);
    for (uint32_t i = 0; i < 12; i++) {
        entityDestroy(&physics, stack[i]);
    }

    printf("\nformation arrival: 12 mixed ships ordered into a cuboid at the origin\n");
    Entity squad[12];
    double slotTarget[12][3];
    double spacing = 0.0;
    for (uint32_t i = 0; i < 12; i++) {
        double candidate = kindProfiles[i % 4].radius * 2.6;
        if (candidate > spacing) {
            spacing = candidate;
        }
    }
    for (uint32_t i = 0; i < 12; i++) {
        squad[i] = entityCreate(&physics, commandableMask);
        double* position = componentOf(&physics, squad[i], COMPONENT_POSITION);
        float* velocity = componentOf(&physics, squad[i], COMPONENT_VELOCITY);
        uint16_t* kind = componentOf(&physics, squad[i], COMPONENT_KIND);
        double angle = 6.2831853 * (double)i / 12.0;
        position[0] = 5000.0 * cos(angle);
        position[1] = 2000.0 * sin(angle * 2.0);
        position[2] = 5000.0 * sin(angle);
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
        velocity[2] = 0.0f;
        *kind = (uint16_t)(i % 4);
        componentAdd(&physics, squad[i], COMPONENT_ORDER);
        Order* slot = componentOf(&physics, squad[i], COMPONENT_ORDER);
        double offset[3];
        formationSlot(i, 12, spacing, offset);
        slot->type = ORDER_MOVE;
        slot->target[0] = offset[0];
        slot->target[1] = offset[1];
        slot->target[2] = offset[2];
        slot->active = 1;
        slotTarget[i][0] = offset[0];
        slotTarget[i][1] = offset[1];
        slotTarget[i][2] = offset[2];
    }
    uint32_t settled = 0;
    uint32_t stillOrdered = 12;
    for (uint32_t tick = 1; tick <= 3000 && stillOrdered; tick++) {
        simulate(&physics, 0.05);
        stillOrdered = 0;
        for (uint32_t i = 0; i < 12; i++) {
            if (componentOf(&physics, squad[i], COMPONENT_ORDER)) {
                stillOrdered++;
            }
        }
        settled = tick;
    }
    double worstError = 0.0;
    double closestPair = 1e9;
    for (uint32_t i = 0; i < 12; i++) {
        double* p = componentOf(&physics, squad[i], COMPONENT_POSITION);
        double error = sqrt((p[0] - slotTarget[i][0]) * (p[0] - slotTarget[i][0]) +
                            (p[1] - slotTarget[i][1]) * (p[1] - slotTarget[i][1]) +
                            (p[2] - slotTarget[i][2]) * (p[2] - slotTarget[i][2]));
        if (error > worstError) {
            worstError = error;
        }
        for (uint32_t j = i + 1; j < 12; j++) {
            double* q = componentOf(&physics, squad[j], COMPONENT_POSITION);
            double d = sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
            if (d < closestPair) {
                closestPair = d;
            }
        }
    }
    printf("  spacing %.3f, still ordered after %.1f s: %u, worst slot error %.4f, closest pair %.4f\n",
           spacing, (double)settled * 0.05, stillOrdered, worstError, closestPair);
    for (uint32_t i = 0; i < 12; i++) {
        double* p = componentOf(&physics, squad[i], COMPONENT_POSITION);
        float* v = componentOf(&physics, squad[i], COMPONENT_VELOCITY);
        Order* o = componentOf(&physics, squad[i], COMPONENT_ORDER);
        double error = sqrt((p[0] - slotTarget[i][0]) * (p[0] - slotTarget[i][0]) +
                            (p[1] - slotTarget[i][1]) * (p[1] - slotTarget[i][1]) +
                            (p[2] - slotTarget[i][2]) * (p[2] - slotTarget[i][2]));
        printf("    ship %2u kind %u  ordered %d  active %u  error %7.4f  speed %.4f  target %6.2f %6.2f %6.2f\n",
               i, (uint32_t)(i % 4), o ? 1 : 0, o ? o->active : 0, error,
               sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]),
               o ? o->target[0] : slotTarget[i][0], o ? o->target[1] : slotTarget[i][1], o ? o->target[2] : slotTarget[i][2]);
    }
    for (uint32_t i = 0; i < 12; i++) {
        entityDestroy(&physics, squad[i]);
    }

    printf("\norbit: ship starts 4000 m away, ordered to orbit at 1200 m\n");
    Entity anchor = entityCreate(&physics, commandableMask);
    double* anchorPosition = componentOf(&physics, anchor, COMPONENT_POSITION);
    float* anchorVelocity = componentOf(&physics, anchor, COMPONENT_VELOCITY);
    uint16_t* anchorKind = componentOf(&physics, anchor, COMPONENT_KIND);
    anchorPosition[0] = 0.0;
    anchorPosition[1] = 0.0;
    anchorPosition[2] = 0.0;
    anchorVelocity[0] = 0.0f;
    anchorVelocity[1] = 0.0f;
    anchorVelocity[2] = 0.0f;
    *anchorKind = 0;
    Entity orbiter = entityCreate(&physics, commandableMask);
    double* orbiterPosition = componentOf(&physics, orbiter, COMPONENT_POSITION);
    float* orbiterVelocity = componentOf(&physics, orbiter, COMPONENT_VELOCITY);
    uint16_t* orbiterKind = componentOf(&physics, orbiter, COMPONENT_KIND);
    orbiterPosition[0] = 4000.0;
    orbiterPosition[1] = 0.0;
    orbiterPosition[2] = 0.0;
    orbiterVelocity[0] = 0.0f;
    orbiterVelocity[1] = 0.0f;
    orbiterVelocity[2] = 0.0f;
    *orbiterKind = 0;
    componentAdd(&physics, orbiter, COMPONENT_ORDER);
    Order* orbitOrder = componentOf(&physics, orbiter, COMPONENT_ORDER);
    orbitOrder->type = ORDER_ORBIT;
    orbitOrder->targetEntity = anchor;
    orbitOrder->radius = 1200.0f;
    orbitOrder->active = 1;
    for (uint32_t tick = 1; tick <= 400; tick++) {
        simulate(&physics, 0.05);
        if (tick % 50 == 0) {
            double* p = componentOf(&physics, orbiter, COMPONENT_POSITION);
            float* v = componentOf(&physics, orbiter, COMPONENT_VELOCITY);
            double distance = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            double speed = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
            printf("  t %5.1f s  distance %8.1f  speed %7.2f\n", (double)tick * 0.05, distance, speed);
        }
    }
    entityDestroy(&physics, orbiter);
    entityDestroy(&physics, anchor);

    printf("\nattack: strike classes against a capital cruising at 900 m/s, opening range 9 km, 60 s\n");
    printf("%-6s %7s %6s %7s %6s %10s %10s %8s %9s\n",
           "class", "shots", "hits", "damage", "passes", "closest", "farthest", "break m", "flight");
    for (uint32_t kind = 0; kind < KIND_CORVETTE; kind++) {
        Entity mark = entityCreate(&physics, commandableMask);
        Entity strike = entityCreate(&physics, commandableMask);
        Entity pair[2] = {mark, strike};
        for (uint32_t which = 0; which < 2; which++) {
            double* position = componentOf(&physics, pair[which], COMPONENT_POSITION);
            float* velocity = componentOf(&physics, pair[which], COMPONENT_VELOCITY);
            int16_t* orientation = componentOf(&physics, pair[which], COMPONENT_ORIENTATION);
            uint16_t* shipKind = componentOf(&physics, pair[which], COMPONENT_KIND);
            uint64_t* shipOwner = componentOf(&physics, pair[which], COMPONENT_OWNER);
            uint32_t* shipSystem = componentOf(&physics, pair[which], COMPONENT_SYSTEM);
            Damage* health = componentOf(&physics, pair[which], COMPONENT_DAMAGE);
            position[0] = which ? 9000.0 : 0.0;
            position[1] = 0.0;
            position[2] = 0.0;
            velocity[0] = 0.0f;
            velocity[1] = 0.0f;
            velocity[2] = 0.0f;
            orientation[0] = 0;
            orientation[1] = 0;
            orientation[2] = 0;
            orientation[3] = 32767;
            *shipKind = (uint16_t)(which ? kind : KIND_CORVETTE);
            *shipOwner = which ? 1 : 2;
            *shipSystem = 0;
            health->hull = kindProfiles[*shipKind].maxHull;
            health->armor = kindProfiles[*shipKind].maxArmor;
            health->shield = kindProfiles[*shipKind].maxShield;
        }
        componentAdd(&physics, mark, COMPONENT_ORDER);
        Order* cruise = componentOf(&physics, mark, COMPONENT_ORDER);
        memset(cruise, 0, sizeof(*cruise));
        cruise->type = ORDER_MOVE;
        cruise->target[2] = 150000.0;
        cruise->active = 1;
        componentAdd(&physics, strike, COMPONENT_ORDER);
        Order* run = componentOf(&physics, strike, COMPONENT_ORDER);
        memset(run, 0, sizeof(*run));
        run->type = ORDER_ATTACK;
        run->targetEntity = mark;
        run->rounds = (uint8_t)kindProfiles[kind].magazine;
        run->active = 1;
        int32_t before = kindProfiles[KIND_CORVETTE].maxHull + kindProfiles[KIND_CORVETTE].maxArmor +
                         kindProfiles[KIND_CORVETTE].maxShield;
        uint32_t shots = 0;
        uint32_t passes = 0;
        uint32_t flight = 0;
        uint8_t phase = PHASE_RUN;
        double closest = 1e9;
        double farthest = 0.0;
        for (uint32_t tick = 0; tick < 1200; tick++) {
            simulate(&physics, 0.05);
            shots += pendingCount;
            Order* state = componentOf(&physics, strike, COMPONENT_ORDER);
            double* here = componentOf(&physics, strike, COMPONENT_POSITION);
            double* there = componentOf(&physics, mark, COMPONENT_POSITION);
            if (!state || !here || !there) {
                break;
            }
            if (state->phase != phase) {
                passes += state->phase == PHASE_ATTACK_BREAK;
                phase = state->phase;
            }
            double distance = sqrt((here[0] - there[0]) * (here[0] - there[0]) +
                                   (here[1] - there[1]) * (here[1] - there[1]) +
                                   (here[2] - there[2]) * (here[2] - there[2]));
            closest = distance < closest ? distance : closest;
            farthest = distance > farthest ? distance : farthest;
            for (uint32_t a = 0; a < physics.archetypeCount; a++) {
                flight += physics.archetypes[a].present[COMPONENT_ROUND] ? physics.archetypes[a].entityCount : 0;
            }
        }
        Damage* left = componentOf(&physics, mark, COMPONENT_DAMAGE);
        int32_t dealt = left ? before - (left->hull + left->armor + left->shield) : before;
        printf("%-6u %7u %6d %7d %6u %10.1f %10.1f %8.0f %9.1f\n",
               kind, shots, kindProfiles[kind].roundDamage ? dealt / kindProfiles[kind].roundDamage : 0, dealt, passes,
               closest, farthest, kindProfiles[kind].maxSpeed / kindProfiles[kind].turnRate * BREAK_TURNS,
               (double)flight / 1200.0);
        entityDestroy(&physics, strike);
        entityDestroy(&physics, mark);
    }

    printf("\nturrets: which corvette mount bears on a target 3 km away, its own hull in the way\n");
    static const double bearings[4][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}};
    static const char* bearingNames[4] = {"ahead", "above", "below", "abeam"};
    const KindProfile* corvette = &kindProfiles[KIND_CORVETTE];
    double still[3] = {0.0, 0.0, 0.0};
    float rest[3] = {0.0f, 0.0f, 0.0f};
    double level[4] = {0.0, 0.0, 0.0, 1.0};
    printf("%-8s %8s %8s %8s %8s\n", "target", "dorsal", "dorsal", "ventral", "ventral");
    for (uint32_t i = 0; i < 4; i++) {
        double mark[3] = {bearings[i][0] * 3000.0, bearings[i][1] * 3000.0, bearings[i][2] * 3000.0};
        printf("%-8s", bearingNames[i]);
        for (uint32_t station = 0; station < corvette->hardpoints; station++) {
            double muzzle[3];
            double aim[3];
            printf(" %8s", turretShot(corvette, station, level, still, rest, mark, NULL, 0.05, muzzle, aim) ? "fires" : "blocked");
        }
        printf("\n");
    }

    printf("\ncorvette: ordered to orbit an enemy bomber at 1200 m from 4 km out, 60 s\n");
    Entity mount = entityCreate(&physics, commandableMask);
    Entity quarry = entityCreate(&physics, commandableMask);
    Entity duo[2] = {quarry, mount};
    for (uint32_t which = 0; which < 2; which++) {
        double* position = componentOf(&physics, duo[which], COMPONENT_POSITION);
        float* velocity = componentOf(&physics, duo[which], COMPONENT_VELOCITY);
        int16_t* orientation = componentOf(&physics, duo[which], COMPONENT_ORIENTATION);
        uint16_t* shipKind = componentOf(&physics, duo[which], COMPONENT_KIND);
        uint64_t* shipOwner = componentOf(&physics, duo[which], COMPONENT_OWNER);
        uint32_t* shipSystem = componentOf(&physics, duo[which], COMPONENT_SYSTEM);
        Damage* health = componentOf(&physics, duo[which], COMPONENT_DAMAGE);
        position[0] = which ? 4000.0 : 0.0;
        position[1] = 0.0;
        position[2] = 0.0;
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
        velocity[2] = 0.0f;
        orientation[0] = 0;
        orientation[1] = 0;
        orientation[2] = 0;
        orientation[3] = 32767;
        *shipKind = (uint16_t)(which ? KIND_CORVETTE : 2);
        *shipOwner = which ? 1 : 2;
        *shipSystem = 0;
        health->hull = kindProfiles[*shipKind].maxHull;
        health->armor = kindProfiles[*shipKind].maxArmor;
        health->shield = kindProfiles[*shipKind].maxShield;
    }
    componentAdd(&physics, mount, COMPONENT_ORDER);
    Order* circle = componentOf(&physics, mount, COMPONENT_ORDER);
    memset(circle, 0, sizeof(*circle));
    circle->type = ORDER_ORBIT;
    circle->targetEntity = quarry;
    circle->radius = 1200.0f;
    circle->rounds = (uint8_t)corvette->magazine;
    circle->active = 1;
    uint32_t salvos = 0;
    int32_t opening = kindProfiles[2].maxHull + kindProfiles[2].maxArmor + kindProfiles[2].maxShield;
    for (uint32_t tick = 0; tick < 1200; tick++) {
        simulate(&physics, 0.05);
        salvos += pendingCount;
        if (!entityAlive(&physics, quarry)) {
            break;
        }
    }
    Damage* struck = componentOf(&physics, quarry, COMPONENT_DAMAGE);
    int32_t taken = struck ? opening - (struck->hull + struck->armor + struck->shield) : opening;
    double* held = componentOf(&physics, mount, COMPONENT_POSITION);
    printf("  %u rounds fired, %d of %d damage dealt, %d hits, orbit held at %.1f m\n",
           salvos, taken, opening, taken / corvette->roundDamage,
           held ? sqrt(held[0] * held[0] + held[1] * held[1] + held[2] * held[2]) : 0.0);
    entityDestroy(&physics, mount);
    entityDestroy(&physics, quarry);

    printf("\nkill: a bomber pass against a drifting interceptor\n");
    Entity prey = entityCreate(&physics, commandableMask);
    Entity hunter = entityCreate(&physics, commandableMask);
    Entity duel[2] = {prey, hunter};
    for (uint32_t which = 0; which < 2; which++) {
        double* position = componentOf(&physics, duel[which], COMPONENT_POSITION);
        float* velocity = componentOf(&physics, duel[which], COMPONENT_VELOCITY);
        int16_t* orientation = componentOf(&physics, duel[which], COMPONENT_ORIENTATION);
        uint16_t* shipKind = componentOf(&physics, duel[which], COMPONENT_KIND);
        uint64_t* shipOwner = componentOf(&physics, duel[which], COMPONENT_OWNER);
        uint32_t* shipSystem = componentOf(&physics, duel[which], COMPONENT_SYSTEM);
        Damage* health = componentOf(&physics, duel[which], COMPONENT_DAMAGE);
        position[0] = which ? 7000.0 : 0.0;
        position[1] = 0.0;
        position[2] = 0.0;
        velocity[0] = 0.0f;
        velocity[1] = 0.0f;
        velocity[2] = 0.0f;
        orientation[0] = 0;
        orientation[1] = 0;
        orientation[2] = 0;
        orientation[3] = 32767;
        *shipKind = which ? 2 : 0;
        *shipOwner = which ? 1 : 2;
        *shipSystem = 0;
        health->hull = kindProfiles[*shipKind].maxHull;
        health->armor = kindProfiles[*shipKind].maxArmor;
        health->shield = kindProfiles[*shipKind].maxShield;
    }
    componentAdd(&physics, hunter, COMPONENT_ORDER);
    Order* pass = componentOf(&physics, hunter, COMPONENT_ORDER);
    memset(pass, 0, sizeof(*pass));
    pass->type = ORDER_ATTACK;
    pass->targetEntity = prey;
    pass->rounds = (uint8_t)kindProfiles[2].magazine;
    pass->active = 1;
    uint32_t killed = 0;
    uint32_t dropped = 0;
    for (uint32_t tick = 1; tick <= 1200 && !killed; tick++) {
        simulate(&physics, 0.05);
        dropped += pendingCount;
        if (!entityAlive(&physics, prey)) {
            killed = tick;
        }
    }
    Damage* remains = componentOf(&physics, prey, COMPONENT_DAMAGE);
    printf("  %u bombs dropped, %d of %d health left, destroyed after %.1f s, attacker still ordered %d\n",
           dropped, remains ? remains->hull + remains->armor + remains->shield : 0,
           kindProfiles[0].maxHull + kindProfiles[0].maxArmor + kindProfiles[0].maxShield,
           (double)killed * 0.05, componentOf(&physics, hunter, COMPONENT_ORDER) != NULL);
    simulate(&physics, 0.05);
    printf("  one tick later the order is dropped: still ordered %d\n",
           componentOf(&physics, hunter, COMPONENT_ORDER) != NULL);

    printf("\norders: an attack order on a drifting capital, issued once against re-issued every tick, 60 s\n");
    printf("%-6s %-11s %7s %7s %8s %10s\n", "class", "issued", "shots", "passes", "rounds", "closest");
    commandWorld = &physics;
    for (uint32_t sample = 0; sample < 4; sample++) {
        uint32_t attacker = sample < 2 ? 0 : KIND_CORVETTE;
        Entity capital = entityCreate(&physics, commandableMask);
        Entity shooter = entityCreate(&physics, commandableMask);
        Entity pair[2] = {capital, shooter};
        for (uint32_t which = 0; which < 2; which++) {
            double* position = componentOf(&physics, pair[which], COMPONENT_POSITION);
            float* velocity = componentOf(&physics, pair[which], COMPONENT_VELOCITY);
            int16_t* orientation = componentOf(&physics, pair[which], COMPONENT_ORIENTATION);
            uint16_t* shipKind = componentOf(&physics, pair[which], COMPONENT_KIND);
            uint64_t* shipOwner = componentOf(&physics, pair[which], COMPONENT_OWNER);
            uint32_t* shipSystem = componentOf(&physics, pair[which], COMPONENT_SYSTEM);
            Damage* health = componentOf(&physics, pair[which], COMPONENT_DAMAGE);
            position[0] = which ? 9000.0 : 0.0;
            position[1] = 0.0;
            position[2] = 0.0;
            velocity[0] = 0.0f;
            velocity[1] = 0.0f;
            velocity[2] = 0.0f;
            orientation[0] = 0;
            orientation[1] = 0;
            orientation[2] = 0;
            orientation[3] = 32767;
            *shipKind = (uint16_t)(which ? attacker : KIND_CORVETTE);
            *shipOwner = which ? 3 : 4;
            *shipSystem = 0;
            health->hull = kindProfiles[*shipKind].maxHull;
            health->armor = kindProfiles[*shipKind].maxArmor;
            health->shield = kindProfiles[*shipKind].maxShield;
        }
        Session commander = {.player = 3};
        unsigned char body[52];
        double point[3] = {0.0, 0.0, 0.0};
        uint32_t markCompact = entityCompact(capital);
        uint32_t single = 1;
        uint32_t attack = ORDER_ATTACK;
        float orbit = 1200.0f;
        uint32_t shipCompact = entityCompact(shooter);
        memset(body, 0, sizeof(body));
        memcpy(body, point, sizeof(point));
        memcpy(body + 24, &markCompact, sizeof(markCompact));
        memcpy(body + 32, &single, sizeof(single));
        memcpy(body + 36, &attack, sizeof(attack));
        memcpy(body + 40, &orbit, sizeof(orbit));
        memcpy(body + 48, &shipCompact, sizeof(shipCompact));
        applyCommand(&commander, body, sizeof(body));
        uint32_t shots = 0;
        uint32_t passes = 0;
        uint8_t phase = PHASE_RUN;
        double closest = 1e9;
        for (uint32_t tick = 0; tick < 1200; tick++) {
            if (sample & 1) {
                applyCommand(&commander, body, sizeof(body));
            }
            simulate(&physics, 0.05);
            shots += pendingCount;
            Order* state = componentOf(&physics, shooter, COMPONENT_ORDER);
            double* here = componentOf(&physics, shooter, COMPONENT_POSITION);
            double* there = componentOf(&physics, capital, COMPONENT_POSITION);
            if (!state || !here || !there) {
                break;
            }
            if (state->phase != phase) {
                passes += state->phase == PHASE_ATTACK_BREAK;
                phase = state->phase;
            }
            double distance = sqrt((here[0] - there[0]) * (here[0] - there[0]) +
                                   (here[1] - there[1]) * (here[1] - there[1]) +
                                   (here[2] - there[2]) * (here[2] - there[2]));
            closest = distance < closest ? distance : closest;
        }
        Order* held = componentOf(&physics, shooter, COMPONENT_ORDER);
        printf("%-6u %-11s %7u %7u %8u %10.1f\n", attacker, (sample & 1) ? "every tick" : "once", shots,
               passes, held ? held->rounds : 0, closest);
        entityDestroy(&physics, capital);
        entityDestroy(&physics, shooter);
    }

    printf("\nproduction: one worker, three seams, and the chain from ore to a repaired hull\n");
    commandWorld = &physics;
    Session foreman = {.player = 7};
    Entity worker = seedWorker(&physics, 7);
    double* workerPlace = componentOf(&physics, worker, COMPONENT_POSITION);
    double origin[3] = {workerPlace[0], workerPlace[1], workerPlace[2]};
    double site[3] = {origin[0] + 1800.0, origin[1] + 200.0, origin[2] + 1200.0};
    Entity seams[ORE_COUNT];
    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
        double place[3] = {
            origin[0] - 2600.0 + 1500.0 * (double)ore,
            origin[1] - 400.0 + 300.0 * (double)ore,
            origin[2] - 2400.0 - 700.0 * (double)ore
        };
        seams[ore] = benchRock(&physics, place, ore);
    }
    double zero[3] = {0.0, 0.0, 0.0};
    benchCommand(&foreman, zero, seams[0], ORDER_MINE, 0.0f, worker);
    uint32_t elapsed = 0;
    for (; elapsed < 2000; elapsed++) {
        simulate(&physics, 0.05);
        Cargo* filling = componentOf(&physics, worker, COMPONENT_CARGO);
        if (filling && cargoTotal(filling) >= kindProfiles[KIND_DRONE].hold) {
            break;
        }
    }
    printf("  worker mined %u of %u hold in %.1f s at %.0f m standoff\n",
           cargoTotal(componentOf(&physics, worker, COMPONENT_CARGO)), kindProfiles[KIND_DRONE].hold,
           (double)elapsed * 0.05,
           separation(componentOf(&physics, worker, COMPONENT_POSITION),
                      componentOf(&physics, seams[0], COMPONENT_POSITION)));

    benchCommand(&foreman, site, 0, ORDER_BUILD, 0.0f, worker);
    Entity yard = 0;
    uint32_t printing = 0;
    uint32_t stalls = 0;
    uint16_t seen = 0;
    for (; printing < 40000; printing++) {
        simulate(&physics, 0.05);
        Order* job = componentOf(&physics, worker, COMPONENT_ORDER);
        if (!job) {
            break;
        }
        if (job->targetEntity) {
            yard = job->targetEntity;
            Station* raising = componentOf(&physics, yard, COMPONENT_STATION);
            if (raising) {
                stalls += raising->build == seen;
                seen = raising->build;
            }
        }
    }
    Station* state = componentOf(&physics, yard, COMPONENT_STATION);
    Cargo* store = componentOf(&physics, yard, COMPONENT_CARGO);
    printf("  station printed in %.1f s of hauling and printing, build %u of %u, %u ticks stalled for ore\n",
           (double)printing * 0.05, state ? state->build : 0, BUILD_FULL, stalls);
    printf("  it cost %u %u %u and the worker fetched it a load at a time\n",
           kindProfiles[KIND_STATION].cost[0], kindProfiles[KIND_STATION].cost[1], kindProfiles[KIND_STATION].cost[2]);

    uint32_t wanted[ORE_COUNT];
    for (uint32_t ore = 0; ore < ORE_COUNT; ore++) {
        wanted[ore] = kindProfiles[KIND_INTERCEPTOR].researchCost[ore] + kindProfiles[KIND_INTERCEPTOR].cost[ore] +
                      (uint32_t)((double)kindProfiles[KIND_CORVETTE].cost[ore] *
                                 (HULL_REPAIR_SHARE * 0.75 + ARMOR_REPAIR_SHARE)) + 2000;
        benchCommand(&foreman, zero, seams[ore], ORDER_MINE, 0.0f, worker);
        for (uint32_t tick = 0; tick < 60000; tick++) {
            simulate(&physics, 0.05);
            if (store && store->ore[ore] >= wanted[ore]) {
                break;
            }
        }
    }
    printf("  hauled to the station: %u ferrocite, %u halcyte, %u vantalum, enough for what follows\n",
           store ? store->ore[0] : 0, store ? store->ore[1] : 0, store ? store->ore[2] : 0);

    Player* holder = playerOf(7);
    benchProduce(&foreman, yard, KIND_INTERCEPTOR, 1);
    uint32_t research = 0;
    uint32_t researchStore = store ? store->ore[0] : 0;
    for (; research < kindProfiles[KIND_INTERCEPTOR].researchTicks + 100 && holder && holder->active; research++) {
        simulate(&physics, 0.05);
    }
    printf("  researched the interceptor in %.1f s with one station, unlocked %04x, %u ferrocite spent of %u\n",
           (double)research * 0.05, holder ? holder->unlocked : 0,
           researchStore - (store ? store->ore[0] : 0), kindProfiles[KIND_INTERCEPTOR].researchCost[0]);

    uint32_t population = 0;
    for (uint32_t a = 0; a < physics.archetypeCount; a++) {
        population += physics.archetypes[a].entityCount;
    }
    uint32_t spent[ORE_COUNT] = {store ? store->ore[0] : 0, store ? store->ore[1] : 0, store ? store->ore[2] : 0};
    benchProduce(&foreman, yard, KIND_INTERCEPTOR, 0);
    benchProduce(&foreman, yard, KIND_DRONE, 0);
    uint32_t building = 0;
    uint32_t berthed = 0;
    for (; building < 600 && state && state->queueKind != QUEUE_NONE; building++) {
        simulate(&physics, 0.05);
        berthed = berthed > state->stored ? berthed : state->stored;
    }
    simulate(&physics, 0.05);
    uint32_t grown = 0;
    for (uint32_t a = 0; a < physics.archetypeCount; a++) {
        grown += physics.archetypes[a].entityCount;
    }
    printf("  built an interceptor in %.1f s inside the hangar and launched it from a door: entities %u -> %u\n",
           (double)building * 0.05, population, grown);
    printf("  it was paid for a tick at a time: %u %u %u ore of the %u %u %u it costs, second order refused while busy\n",
           spent[0] - (store ? store->ore[0] : 0), spent[1] - (store ? store->ore[1] : 0),
           spent[2] - (store ? store->ore[2] : 0), kindProfiles[KIND_INTERCEPTOR].cost[0],
           kindProfiles[KIND_INTERCEPTOR].cost[1], kindProfiles[KIND_INTERCEPTOR].cost[2]);

    double* parked = componentOf(&physics, worker, COMPONENT_POSITION);
    double park[3] = {parked[0], parked[1], parked[2]};
    benchCommand(&foreman, park, 0, ORDER_MOVE, 0.0f, worker);
    Entity wreck = entityCreate(&physics, shipMask());
    double berth[3] = {site[0] + 2400.0, site[1], site[2]};
    const KindProfile* hulk = &kindProfiles[KIND_CORVETTE];
    double* wreckPlace = componentOf(&physics, wreck, COMPONENT_POSITION);
    float* wreckPace = componentOf(&physics, wreck, COMPONENT_VELOCITY);
    int16_t* wreckFacing = componentOf(&physics, wreck, COMPONENT_ORIENTATION);
    Damage* wreckHealth = componentOf(&physics, wreck, COMPONENT_DAMAGE);
    uint64_t* wreckOwner = componentOf(&physics, wreck, COMPONENT_OWNER);
    uint16_t* wreckKind = componentOf(&physics, wreck, COMPONENT_KIND);
    uint32_t* wreckSystem = componentOf(&physics, wreck, COMPONENT_SYSTEM);
    memcpy(wreckPlace, berth, sizeof(berth));
    memset(wreckPace, 0, sizeof(float) * 3);
    wreckFacing[0] = 0;
    wreckFacing[1] = 0;
    wreckFacing[2] = 0;
    wreckFacing[3] = 32767;
    wreckHealth->hull = hulk->maxHull / 4;
    wreckHealth->armor = 0;
    wreckHealth->shield = hulk->maxShield;
    *wreckOwner = 7;
    *wreckKind = KIND_CORVETTE;
    *wreckSystem = 0;
    memset(componentOf(&physics, wreck, COMPONENT_CARGO), 0, sizeof(Cargo));
    uint32_t paid[ORE_COUNT] = {store ? store->ore[0] : 0, store ? store->ore[1] : 0, store ? store->ore[2] : 0};
    uint32_t mending = 0;
    for (; mending < 1200; mending++) {
        simulate(&physics, 0.05);
        if (wreckHealth->hull >= hulk->maxHull && wreckHealth->armor >= hulk->maxArmor) {
            break;
        }
    }
    printf("  repaired a corvette from 25%% hull and no armour in %.1f s for %u %u %u ore (of %u %u %u full price)\n",
           (double)mending * 0.05,
           paid[0] - (store ? store->ore[0] : 0), paid[1] - (store ? store->ore[1] : 0),
           paid[2] - (store ? store->ore[2] : 0),
           (uint32_t)(hulk->cost[0] * (HULL_REPAIR_SHARE * 0.75 + ARMOR_REPAIR_SHARE)),
           (uint32_t)(hulk->cost[1] * (HULL_REPAIR_SHARE * 0.75 + ARMOR_REPAIR_SHARE)),
           (uint32_t)(hulk->cost[2] * (HULL_REPAIR_SHARE * 0.75 + ARMOR_REPAIR_SHARE)));

    wreckHealth->hull = hulk->maxHull / 2;
    benchCommand(&foreman, zero, yard, ORDER_DOCK, 0.0f, wreck);
    uint32_t docking = 0;
    uint8_t boarded = 0;
    for (; docking < 1200 && entityAlive(&physics, wreck); docking++) {
        Damage* health = componentOf(&physics, wreck, COMPONENT_DAMAGE);
        boarded = health ? healthFraction(health->hull, hulk->maxHull) : boarded;
        simulate(&physics, 0.05);
    }
    printf("  docked a corvette in %.1f s from 2400 m, hangar holds %u, hull %u in and %u stored\n",
           (double)docking * 0.05, state ? state->stored : 0, boarded,
           state && state->stored ? state->hangarHull[0] : 0);
    uint8_t kept = state && state->stored ? state->hangarHull[0] : 0;
    uint32_t afloat = 0;
    for (uint32_t a = 0; a < physics.archetypeCount; a++) {
        afloat += physics.archetypes[a].present[COMPONENT_DAMAGE] && !physics.archetypes[a].present[COMPONENT_STATION]
                      ? physics.archetypes[a].entityCount : 0;
    }
    benchProduce(&foreman, yard, 0, 2);
    uint32_t releasing = 0;
    for (; releasing < 200 && state && state->pending; releasing++) {
        simulate(&physics, 0.05);
    }
    simulate(&physics, 0.05);
    uint32_t returned = 0;
    for (uint32_t a = 0; a < physics.archetypeCount; a++) {
        returned += physics.archetypes[a].present[COMPONENT_DAMAGE] && !physics.archetypes[a].present[COMPONENT_STATION]
                        ? physics.archetypes[a].entityCount : 0;
    }
    printf("  undocked through a door in %.1f s: hangar holds %u, hulls afloat %u -> %u, restored to %u of %u hull\n",
           (double)releasing * 0.05, state ? state->stored : 0, afloat, returned,
           (uint32_t)((double)hulk->maxHull * (double)kept / 255.0), hulk->maxHull);

    for (uint32_t ore = 1; ore < ORE_COUNT; ore++) {
        entityDestroy(&physics, seams[ore]);
    }
    double lean[3] = {site[0] - 5200.0, site[1], site[2] + 3100.0};
    benchCommand(&foreman, lean, 0, ORDER_BUILD, 0.0f, worker);
    uint16_t reached = 0;
    for (uint32_t partial = 0; partial < 30000 && reached < BUILD_FULL / 3; partial++) {
        simulate(&physics, 0.05);
        Order* job = componentOf(&physics, worker, COMPONENT_ORDER);
        Station* raising = job && job->targetEntity ? componentOf(&physics, job->targetEntity, COMPONENT_STATION) : NULL;
        reached = raising && raising->build > reached ? raising->build : reached;
    }
    printf("  a second site with only ferrocite in reach climbs to %u of %u, one ore of three, and holds there\n",
           reached, BUILD_FULL);
    return 0;
}

int main(void) {
    return runTests();
}
