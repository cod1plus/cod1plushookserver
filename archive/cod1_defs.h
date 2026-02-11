/*
 * cod1_defs.h - CoD1 MP v1.5 reverse-engineered structures and addresses
 *
 * Based on CoDExtended (https://github.com/xtnded/codextended)
 * Target: cod_lnxded (Linux dedicated server) v1.5
 */

#ifndef COD1_DEFS_H
#define COD1_DEFS_H

#include <stdint.h>

/* ========================================================================
 * Constants
 * ======================================================================== */

#define MAX_CLIENTS             64
#define MAX_GENTITIES           1024
#define MAX_WEAPONS             64
#define MAX_OBJECTIVES          16
#define MAX_CONFIGSTRINGS       2048
#define MAX_RELIABLE_COMMANDS   64
#define MAX_CHALLENGES          1024
#define MAX_NETNAME             36
#define MAX_NAME_LENGTH         32
#define MAX_INFO_STRING         1024
#define MAX_MSGLEN              0x4000
#define MAX_DOWNLOAD_WINDOW     8
#define PACKET_BACKUP           32

/* Struct sizes for v1.5 */
#define GENTITY_SIZE_V15        0x31C   /* 796 bytes */
#define PLAYERSTATE_SIZE        0x22CC  /* 8908 bytes */
#define CLIENT_T_SIZE_V15       371124  /* bytes per client_t */

/* ========================================================================
 * Basic types (Quake 3 / id Tech 3 style)
 * ======================================================================== */

typedef int qboolean;
#define qtrue   1
#define qfalse  0

typedef float vec_t;
typedef vec_t vec3_t[3];

typedef int fileHandle_t;

/* ========================================================================
 * Enumerations
 * ======================================================================== */

typedef enum {
    ET_GENERAL = 0,
    ET_PLAYER = 1,
    ET_CORPSE = 2,
    ET_ITEM = 3,
    ET_MISSILE = 4,
    ET_MOVER = 5,
    ET_PORTAL = 6,
    ET_INVISIBLE = 7,
    ET_SCRIPTMOVER = 8,
    ET_UNKNOWN = 9,
    ET_FX = 10,
    ET_TURRET = 11,
    ET_EVENTS = 12
} entityType_t;

typedef enum {
    PM_NORMAL = 0,
    PM_NORMAL_LINKED = 1,
    PM_NOCLIP = 2,
    PM_UFO = 3,
    PM_SPECTATOR = 4,
    PM_INTERMISSION = 5,
    PM_DEAD = 6,
    PM_DEAD_LINKED = 7
} pmtype_t;

typedef enum {
    TEAM_NONE = 0,
    TEAM_AXIS = 1,
    TEAM_ALLIES = 2,
    TEAM_SPECTATOR = 3,
    TEAM_NUM_TEAMS = 4
} team_t;

typedef enum {
    CON_DISCONNECTED = 0,
    CON_CONNECTING = 1,
    CON_CONNECTED = 2
} clientConnected_t;

typedef enum {
    CS_FREE = 0,
    CS_ZOMBIE = 1,
    CS_CONNECTED = 2,
    CS_PRIMED = 3,
    CS_ACTIVE = 4
} clientConnectState_t;

typedef enum {
    SESS_STATE_PLAYING = 0,
    SESS_STATE_DEAD = 1,
    SESS_STATE_SPECTATOR = 2,
    SESS_STATE_INTERMISSION = 3
} sessionState_t;

typedef enum {
    TR_STATIONARY = 0,
    TR_INTERPOLATE = 1,
    TR_LINEAR = 2,
    TR_LINEAR_STOP = 3,
    TR_SINE = 4,
    TR_GRAVITY = 5
} trType_t;

typedef enum {
    NA_BOT = 0,
    NA_BAD = 1,
    NA_LOOPBACK = 2,
    NA_BROADCAST = 3,
    NA_IP = 4,
    NA_IPX = 5,
    NA_BROADCAST_IPX = 6
} netadrtype_t;

typedef enum {
    NS_CLIENT = 0,
    NS_SERVER = 1
} netsrc_t;

typedef enum {
    WEAPON_READY = 0,
    WEAPON_RAISING = 1,
    WEAPON_DROPPING = 2,
    WEAPON_FIRING = 3,
    WEAPON_RECHAMBERING = 4,
    WEAPON_RELOADING = 5,
    WEAPON_RELOADING_INTERUPT = 6,
    WEAPON_RELOAD_START = 7,
    WEAPON_RELOAD_START_INTERUPT = 8,
    WEAPON_RELOAD_END = 9,
    WEAPON_MELEE_INIT = 10,
    WEAPON_MELEE_FIRE = 11
} weaponstate_t;

/* Entity flags */
#define EF_DEAD         0x00000001
#define EF_CROUCH       0x00000020
#define EF_PRONE        0x00000040
#define EF_NODRAW       0x00000100
#define EF_AIM          0x00000200
#define EF_FIRING       0x00000400
#define EF_TALK         0x00040000

/* PM flags */
#define PMF_PRONE       0x01
#define PMF_CROUCH      0x02
#define PMF_LADDER      0x10
#define PMF_SLIDING     0x100

/* SVF flags */
#define SVF_NOCLIENT            0x001
#define SVF_BROADCAST           0x020
#define SVF_CAPSULE             0x200
#define SVF_SINGLECLIENT        0x800
#define SVF_NOTSINGLECLIENT     0x2000

/* Button masks */
#define KEY_MASK_FIRE           0x01
#define KEY_MASK_RELOAD         0x08
#define KEY_MASK_LEANLEFT       0x10
#define KEY_MASK_LEANRIGHT      0x20
#define KEY_MASK_USE            0x40
#define KEY_MASK_CROUCH         0x80

/* ========================================================================
 * Entity field offsets for v1.5
 * ======================================================================== */

#define EOFF_S_ETYPE            4
#define EOFF_EFLAGS             8
#define EOFF_S_SVFLAGS          244
#define EOFF_R_CONTENTS         284
#define EOFF_ORIGIN             312
#define EOFF_ANGLES             324
#define EOFF_R_OWNERNUM         336
#define EOFF_R_EVENTTIME        340
#define EOFF_PLAYERSTATE        348   /* client pointer */
#define EOFF_INUSE              356
#define EOFF_PHYSICSOBJECT      357
#define EOFF_TAKEDAMAGE         373
#define EOFF_MOVERSTATE         376
#define EOFF_CLASSNAME          380
#define EOFF_SPAWNFLAGS         384
#define EOFF_FLAGS              388
#define EOFF_EVENTTIME          392
#define EOFF_FREEAFTEREVENT     396
#define EOFF_UNLINKAFTEREVENT   400
#define EOFF_PARENT             416
#define EOFF_TEAM               480
#define EOFF_SPEED              488
#define EOFF_NEXTTHINK          516
#define EOFF_THINK              520
#define EOFF_USE                536
#define EOFF_PAIN               540
#define EOFF_DIE                544
#define EOFF_HEALTH             568
#define EOFF_DAMAGE             576
#define EOFF_SPLASHDAMAGE       580
#define EOFF_SPLASHRADIUS       584
#define EOFF_SPLASHMETHODOFDEATH 588
#define EOFF_TEAMCHAIN          616
#define EOFF_TEAMMASTER         620
#define EOFF_WAIT               624
#define EOFF_RANDOM             628
#define EOFF_CLIENTNUM          144   /* entityState_t.clientNum */
#define EOFF_GROUNDENTITYNUM    124
#define EOFF_LOOPSOUND          132

/* Player state offsets */
#define POFF_CLIENTNUM          172   /* 0xAC */
#define POFF_EFLAGS             128   /* 0x80 */
#define POFF_PM_TYPE            4
#define POFF_VELOCITY           0x20
#define POFF_ANGLES             0xC0
#define POFF_SESSIONSTATE       8400  /* 0x20D0 - offset to clientSession_t */

/* ========================================================================
 * Network structures
 * ======================================================================== */

typedef struct {
    netadrtype_t type;
    unsigned char ip[4];
    unsigned char ipx[10];
    unsigned short port;
} netadr_t;

typedef struct {
    netsrc_t sock;
    int dropped;
    netadr_t remoteAddress;
    int qport;
    int incomingSequence;
    int outgoingSequence;
    int fragmentSequence;
    int fragmentLength;
    char fragmentBuffer[32768];
    int unsentFragments;
    int unsentLength;
    char unsentBuffer[32768];
} netchan_t;

/* ========================================================================
 * Trajectory
 * ======================================================================== */

typedef struct {
    trType_t trType;
    int trTime;
    int trDuration;
    vec3_t trBase;
    vec3_t trDelta;
} trajectory_t;

/* ========================================================================
 * User command
 * ======================================================================== */

typedef struct {
    int serverTime;
    uint8_t buttons;
    uint8_t wbuttons;
    uint8_t weapon;
    uint8_t flags;
    int angles[3];
    int8_t forwardmove;
    int8_t rightmove;
    int8_t upmove;
    uint8_t unknown;
} usercmd_t;

/* ========================================================================
 * Entity state (shared between server and client)
 * ======================================================================== */

typedef struct {
    int number;
    int eType;              /* +0x04 */
    int eFlags;             /* +0x08 */
    trajectory_t pos;       /* position trajectory */
    trajectory_t apos;      /* angular trajectory */
    vec3_t origin2;
    vec3_t angles2;
    int otherEntityNum;
    int otherEntityNum2;
    int groundEntityNum;    /* +0x7C (124) */
    int constantLight;
    int loopSound;          /* +0x84 (132) */
    int surfaceFlags;
    int modelindex;
    int clientNum;          /* +0x90 (144) */
    int weapon;
    int legsAnim;
    int torsoAnim;
    float leanf;
    int loopfxid;
    int hintstring;
    int animMovetype;
} entityState_t;

/* ========================================================================
 * Player state (0x22CC bytes)
 * ======================================================================== */

typedef struct {
    int commandTime;            /* +0x00 */
    int bobCycle;               /* +0x04 */
    int pm_flags;               /* +0x08 */
    int pm_time;                /* +0x0C */
    pmtype_t pm_type;           /* +0x10 */
    vec3_t origin;              /* +0x14 */
    vec3_t velocity;            /* +0x20 */
    int weaponTime;             /* +0x2C */
    int weaponDelay;            /* +0x30 */
    int grenadeTimeLeft;        /* +0x34 */
    float leanf;                /* +0x38 */
    float gravity;
    float speed;
    vec3_t delta_angles;
    int groundEntityNum;
    vec3_t vLadderVec;
    int jumpTime;
    float fJumpOriginZ;
    int legsTimer;
    int legsAnim;
    int torsoTimer;
    int torsoAnim;
    int movementDir;
    int eFlags;                 /* +0x80 (128) */
    int eventSequence;
    int events[4];
    int eventParms[4];
    int oldEventSequence;
    int clientNum;              /* +0xAC (172) */
    unsigned int weapon;        /* +0xB0 (176) */
    unsigned int weaponstate;   /* +0xB4 */
    float fWeaponPosFrac;
    int viewmodelIndex;
    vec3_t viewangles;          /* +0xC0 */
    int viewHeightTarget;
    float viewHeightCurrent;
    int viewHeightLerpTime;
    int viewHeightLerpTarget;
    int viewHeightLerpDown;
    int viewHeightLerpPosAdj;
    int damageEvent;
    int damageYaw;
    int damagePitch;
    int damageCount;
    int stats[6];
    int ammo[MAX_WEAPONS];
    int ammoclip[MAX_WEAPONS];
    unsigned int weapons[2];
    uint8_t weaponslots[8];
    unsigned int weaponrechamber[2];
    vec3_t mins;
    vec3_t maxs;
    int proneViewHeight;
    int crouchViewHeight;
    int standViewHeight;
    int deadViewHeight;
    float walkSpeedScale;
    float runSpeedScale;
    float proneSpeedScale;
    float crouchSpeedScale;
    float strafeSpeedScale;
    float backSpeedScale;
    float proneDirection;
    float proneDirectionPitch;
    float proneTorsoPitch;
    int viewlocked;
    int viewlocked_entNum;
    float friction;
    int gunfx;
    int serverCursorHint;
    int serverCursorHintVal;
    /* trace_t serverCursorHintTrace - variable size, padded in the struct */
    char _pad_trace[76];        /* approximate trace_t size */
    int iCompassFriendInfo;
    float fTorsoHeight;
    float fTorsoPitch;
    float fWaistPitch;
    int entityEventSequence;
    int weapAnim;
    float aimSpreadScale;
    int shellshockIndex;
    int shellshockTime;
    int shellshockDuration;
    /* objective_t objective[16] + hudElemState_t hud + ping + deltaTime */
    /* We pad to the known total size */
    char _pad_to_session[POFF_SESSIONSTATE - 0x200]; /* approximate padding */
} playerState_t;

/* ========================================================================
 * Client session (contains score and deaths)
 * Located at gclient_t + POFF_SESSIONSTATE (0x20D0)
 * ======================================================================== */

typedef struct {
    sessionState_t sessionState;    /* +0x00 */
    int forceSpectatorClient;       /* +0x04 */
    int statusIcon;                 /* +0x08 */
    int archiveTime;                /* +0x0C */
    int score;                      /* +0x10 -- PLAYER SCORE (= kills in most gametypes) */
    int deaths;                     /* +0x14 -- PLAYER DEATHS */
    clientConnected_t connected;    /* +0x18 */
    usercmd_t cmd;                  /* +0x1C */
    usercmd_t oldcmd;
    qboolean localClient;
    char netname[MAX_NETNAME];
    int maxHealth;
} clientSession_t;

/* ========================================================================
 * gclient_t - Game client structure
 *
 * In practice we access fields via raw offset from the base pointer:
 *   session = (clientSession_t*)((char*)gclient + POFF_SESSIONSTATE)
 *
 * The full struct is complex; we define accessors instead of the
 * complete layout.
 * ======================================================================== */

typedef struct gclient_s {
    /* playerState_t starts at offset 0 */
    char _raw[0x4734]; /* approximate total size, includes ps + session + extra */
} gclient_t;

/* Accessor macros for gclient_t */
#define GCLIENT_PS(gc) \
    ((playerState_t*)(gc))

#define GCLIENT_SESSION(gc) \
    ((clientSession_t*)((char*)(gc) + POFF_SESSIONSTATE))

#define GCLIENT_SCORE(gc) \
    (*(int*)((char*)(gc) + POFF_SESSIONSTATE + 0x10))

#define GCLIENT_DEATHS(gc) \
    (*(int*)((char*)(gc) + POFF_SESSIONSTATE + 0x14))

#define GCLIENT_CONNECTED(gc) \
    (*(clientConnected_t*)((char*)(gc) + POFF_SESSIONSTATE + 0x18))

#define GCLIENT_SESSIONSTATE(gc) \
    (*(sessionState_t*)((char*)(gc) + POFF_SESSIONSTATE))

#define GCLIENT_NETNAME(gc) \
    ((char*)((char*)(gc) + POFF_SESSIONSTATE + 0x1C + sizeof(usercmd_t) * 2 + sizeof(qboolean)))

#define GCLIENT_WEAPON(gc) \
    (*(unsigned int*)((char*)(gc) + 0xB0))

#define GCLIENT_CLIENTNUM(gc) \
    (*(int*)((char*)(gc) + 0xAC))

/* ========================================================================
 * gentity_t - Game entity
 * Size: GENTITY_SIZE_V15 (0x31C = 796 bytes) for v1.5
 * ======================================================================== */

typedef struct gentity_s {
    entityState_t s;                        /* +0x000 */
    char _pad_after_es[EOFF_PLAYERSTATE - sizeof(entityState_t)];
    struct gclient_s *client;               /* +0x15C (EOFF_PLAYERSTATE=348) */
    char _pad_after_client[EOFF_INUSE - EOFF_PLAYERSTATE - sizeof(void*)];
    char inuse;                             /* +0x164 (EOFF_INUSE=356) */
    char _pad_after_inuse[EOFF_HEALTH - EOFF_INUSE - 1];
    int health;                             /* +0x238 (EOFF_HEALTH=568) */
    char _pad_to_end[GENTITY_SIZE_V15 - EOFF_HEALTH - sizeof(int)];
} gentity_t;

/* ========================================================================
 * level_locals_t - Level state
 * ======================================================================== */

typedef struct {
    gclient_t *clients;                 /* +0x00 */
    gentity_t *gentities;               /* +0x04 */
    int gentitySize;                    /* +0x08 */
    int num_entities;                   /* +0x0C */
    gentity_t *firstFreeEnt;            /* +0x10 */
    gentity_t *lastFreeEnt;             /* +0x14 */
    fileHandle_t logFile;               /* +0x18 */
    int initializing;                   /* +0x1C */
    int maxclients;                     /* +0x20 */
    int framenum;                       /* +0x24 */
    int time;                           /* +0x28 */
    int previousTime;                   /* +0x2C */
    int frameTime;                      /* +0x30 */
    int startTime;                      /* +0x34 */
    int teamScores[TEAM_NUM_TEAMS];     /* +0x38 */
    int lastTeammateHealthTime;         /* +0x48 */
    qboolean bUpdateScoresForIntermission; /* +0x4C */
    int manualNameChange;               /* +0x50 */
    int numConnectedClients;            /* +0x54 */
    int sortedClients[MAX_CLIENTS];     /* +0x58 */
    char voteString[1024];              /* +0x158 */
} level_locals_t;

/* ========================================================================
 * client_t - Server-side client info (from serverStatic_t.clients)
 * Size: CLIENT_T_SIZE_V15 (371124 bytes) for v1.5
 *
 * We only define the fields we need; the rest is padding.
 * ======================================================================== */

typedef struct {
    clientConnectState_t state;         /* +0x0000 */
    qboolean sendAsActive;              /* +0x0004 */
    const char *dropReason;             /* +0x0008 */
    char userinfo[1024];                /* +0x000C */
    /* ... lots of fields ... */
    /* We use raw offset access for name, ping, netchan */
    char _raw[CLIENT_T_SIZE_V15];
} client_t;

/* Offsets within client_t (approximate, from CoDExtended) */
/* These may need verification against your specific binary */
#define CLIENT_T_OFF_STATE          0x0000
#define CLIENT_T_OFF_USERINFO       0x000C
#define CLIENT_T_OFF_GENTITY        0x10A40   /* pointer to gentity_t */
#define CLIENT_T_OFF_NAME           0x10A44   /* char name[32] */
#define CLIENT_T_OFF_PING           0x5A7C0   /* approximate - needs verification */
/* netchan is near the end of client_t */
#define CLIENT_T_OFF_NETCHAN        (CLIENT_T_SIZE_V15 - sizeof(netchan_t) - 64)

/* Accessor macros for client_t by raw pointer */
#define SVSCLIENT_STATE(ptr) \
    (*(clientConnectState_t*)((char*)(ptr)))

#define SVSCLIENT_NAME(ptr) \
    ((char*)((char*)(ptr) + CLIENT_T_OFF_NAME))

#define SVSCLIENT_NETCHAN(ptr) \
    ((netchan_t*)((char*)(ptr) + CLIENT_T_OFF_NETCHAN))

#define SVSCLIENT_IP(ptr) \
    (SVSCLIENT_NETCHAN(ptr)->remoteAddress)

/* ========================================================================
 * serverStatic_t - Server state
 * ======================================================================== */

typedef struct {
    qboolean initialized;
    int time;
    int snapFlagServerBit;
    client_t *clients;
    int numSnapshotEntities;
    int numSnapshotClients;
    int nextSnapshotEntities;
    int nextSnapshotClients;
    char _gap[0x34];
    int nextHeartbeatTime;
} serverStatic_t;

/* ========================================================================
 * CoD1 v1.5 Linux Server Addresses
 *
 * These are absolute addresses for the cod_lnxded v1.5 binary.
 * They come from CoDExtended (https://github.com/xtnded/codextended)
 * ======================================================================== */

/* serverStatic_t (svs) - Updated from CodExtended v1.5 */
#define ADDR_SVS                    0x083CCD84
#define ADDR_SVS_CLIENTS            0x083CCDF0  /* Updated to match CodExtended v1.5 */
#define ADDR_SVS_TIME               0x083CCD88

/* Engine functions */
#define ADDR_COM_PRINTF             0x0806FC90
#define ADDR_COM_ERROR              0x0806FEF4
#define ADDR_SYS_LOADDLL            0x080D3DAD

/* Server functions */
#define ADDR_SV_INIT                0x080913B3
#define ADDR_SV_DIRECTCONNECT       0x08089E7E
#define ADDR_SV_GETCLIENTSCORE      0x08092421

/* Script functions (in engine) */
#define ADDR_SCR_EXECTHREAD         0x080A95EC
#define ADDR_SCR_EXECENTTHREAD      0x080A9674
#define ADDR_SCR_FREETHREAD         0x080A97D4

/* Other engine functions */
#define ADDR_SV_SENDSERVERCOMMAND   0x0808B900
#define ADDR_SV_GENTITYNUM          0x08089258
#define ADDR_SV_GAMECLIENTNUM       0x08089270
#define ADDR_SV_DROPCLIENT          0x08085CF4
#define ADDR_SV_FRAME               0x0808CDF8
#define ADDR_SV_SHUTDOWNGAMEMODULE  0x0808AD8C  /* SV_Shutdown */
#define ADDR_SV_SPAWNSERVER         0x0808A220
#define ADDR_SV_MAPRESTART          0x08083DE4
#define ADDR_SV_MAP                 0x08083C68

#define ADDR_GETUSERINFO            0x0808B25C
#define ADDR_SETUSERINFO            0x0808B1D0

/* Memory protection */
#define ADDR_CODE_BASE              0x08048000
#define ADDR_CODE_SIZE              0x135000

/* Function pointer types */
typedef void (*Com_Printf_t)(const char *fmt, ...);
typedef void (*Com_Error_t)(int code, const char *fmt, ...);

/* ========================================================================
 * Game module (game.mp.i386.so) - resolved at runtime
 *
 * These are offsets relative to the game module's vmMain function
 * or to the module base address.
 * ======================================================================== */

/* vmMain dispatch codes (game module entry point) */
#define GAME_INIT               0   /* G_InitGame */
#define GAME_SHUTDOWN           1   /* G_ShutdownGame */
#define GAME_CLIENT_CONNECT     2
#define GAME_CLIENT_BEGIN       3
#define GAME_CLIENT_USERINFO    4
#define GAME_CLIENT_DISCONNECT  5
#define GAME_CLIENT_COMMAND     6
#define GAME_CLIENT_THINK       7
#define GAME_RUN_FRAME          8   /* G_RunFrame */
#define GAME_CONSOLE_COMMAND    9

#endif /* COD1_DEFS_H */
