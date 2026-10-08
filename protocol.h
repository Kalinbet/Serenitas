#ifndef SERENITAS_PROTOCOL_H
#define SERENITAS_PROTOCOL_H

#include <stdint.h>

#define SERVER_PORT 34197
#define PROTOCOL_MAGIC 0x45524553u
#define TICK_NANOSECONDS 50000000
#define PACKET_CAP 2048

#define ASTRONOMICAL_UNIT 1.495978707e11
#define ORE_COUNT 3
#define KIND_COUNT 9
#define CELESTIAL_CAP 32
#define SNAPSHOT_YARDS 8
#define BUILD_FULL 1024
#define MINING_RANGE 400.0

enum {
    KIND_INTERCEPTOR,
    KIND_SCOUT,
    KIND_BOMBER,
    KIND_CORVETTE,
    KIND_DRONE,
    KIND_STATION,
    KIND_FERROCITE,
    KIND_HALCYTE,
    KIND_VANTALUM,
    KIND_STAR = 200,
    KIND_PLANET_ROCK,
    KIND_PLANET_ICE,
    KIND_ROUND = 210,
    KIND_BOMB
};

#define KIND_ROCK KIND_FERROCITE

enum { ORDER_MOVE, ORDER_ORBIT, ORDER_ATTACK, ORDER_MINE, ORDER_BUILD, ORDER_DOCK };

enum { PRODUCE_BUILD, PRODUCE_RESEARCH, PRODUCE_LAUNCH };

enum { PACKET_HELLO = 1, PACKET_WELCOME, PACKET_SUBSCRIBE, PACKET_SNAPSHOT, PACKET_COMMAND, PACKET_BYE, PACKET_SHOTS,
       PACKET_CELESTIALS, PACKET_PRODUCE, PACKET_STATIONS };

#define COMMAND_HEADER 48
#define SNAPSHOT_HEADER 32
#define CELESTIAL_HEADER 8
#define SHOTS_HEADER 12

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t length;
    uint32_t sequence;
} PacketHeader;

typedef struct {
    uint64_t player;
    uint32_t tickNanoseconds;
    uint32_t system;
} PacketWelcome;

typedef struct {
    uint32_t entity;
    int32_t position[3];
    int16_t orientation[4];
    uint32_t target;
    uint16_t owner;
    uint8_t kind;
    uint8_t hull;
    uint8_t armor;
    uint8_t shield;
    uint8_t throttle;
    uint8_t cargo;
} SnapshotEntity;

typedef struct {
    uint32_t entity;
    uint32_t store[ORE_COUNT];
    uint32_t queueTicks;
    uint32_t queueTotal;
    uint16_t queueKind;
    uint16_t build;
    uint16_t stored;
    uint16_t pending;
} SnapshotStation;

typedef struct {
    uint32_t tick;
    uint32_t count;
    uint32_t unlocked;
    uint32_t researchKind;
    uint32_t researchProgress;
    uint32_t researchTotal;
} SnapshotYards;

typedef struct {
    double position[3];
    float velocity[3];
    uint32_t owner;
    uint16_t kind;
    uint16_t life;
} SnapshotShot;

typedef struct {
    double position[3];
    uint32_t entity;
    uint8_t kind;
    uint8_t padding[3];
} SnapshotCelestial;

#endif
