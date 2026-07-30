/*
 * cod1_defs.h - CoD1 MP v1.5 reverse-engineered structures and addresses
 *
 * Based on CoDExtended (https://github.com/xtnded/codextended)
 * Target: cod_lnxded (Linux dedicated server) v1.5
 */

#ifndef COD1_DEFS_H
#define COD1_DEFS_H

#include <stdint.h>

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

#define GENTITY_SIZE_V15        0x31C
#define PLAYERSTATE_SIZE        0x22CC
#define CLIENT_T_SIZE_V15       371124

typedef int qboolean;
#define qtrue   1
#define qfalse  0

typedef float vec_t;
typedef vec_t vec3_t[3];

typedef int fileHandle_t;

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

#define EF_DEAD         0x00000001
#define EF_CROUCH       0x00000020
#define EF_PRONE        0x00000040
#define EF_NODRAW       0x00000100
#define EF_AIM          0x00000200
#define EF_FIRING       0x00000400
#define EF_TALK         0x00040000

#define PMF_PRONE       0x01
#define PMF_CROUCH      0x02
#define PMF_LADDER      0x10
#define PMF_SLIDING     0x100

#define SVF_NOCLIENT            0x001
#define SVF_BROADCAST           0x020
#define SVF_CAPSULE             0x200
#define SVF_SINGLECLIENT        0x800
#define SVF_NOTSINGLECLIENT     0x2000

#define KEY_MASK_FIRE           0x01
#define KEY_MASK_RELOAD         0x08
#define KEY_MASK_LEANLEFT       0x10
#define KEY_MASK_LEANRIGHT      0x20
#define KEY_MASK_USE            0x40
#define KEY_MASK_CROUCH         0x80

#define EOFF_S_ETYPE            4
#define EOFF_EFLAGS             8
#define EOFF_S_SVFLAGS          244
#define EOFF_R_CONTENTS         284
#define EOFF_ORIGIN             312
#define EOFF_ANGLES             324
#define EOFF_R_OWNERNUM         336
#define EOFF_R_EVENTTIME        340
#define EOFF_PLAYERSTATE        348
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
#define EOFF_CLIENTNUM          144
#define EOFF_GROUNDENTITYNUM    124
#define EOFF_LOOPSOUND          132

#define POFF_CLIENTNUM          172
#define POFF_EFLAGS             128
#define POFF_PM_TYPE            4
#define POFF_VELOCITY           0x20
#define POFF_ANGLES             0xC0
#define POFF_SESSIONSTATE       8400

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

typedef struct {
    trType_t trType;
    int trTime;
    int trDuration;
    vec3_t trBase;
    vec3_t trDelta;
} trajectory_t;

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

typedef struct {
    int number;
    int eType;
    int eFlags;
    trajectory_t pos;
    trajectory_t apos;
    vec3_t origin2;
    vec3_t angles2;
    int otherEntityNum;
    int otherEntityNum2;
    int groundEntityNum;
    int constantLight;
    int loopSound;
    int surfaceFlags;
    int modelindex;
    int clientNum;
    int weapon;
    int legsAnim;
    int torsoAnim;
    float leanf;
    int loopfxid;
    int hintstring;
    int animMovetype;
} entityState_t;

typedef struct {
    int commandTime;
    int bobCycle;
    int pm_flags;
    int pm_time;
    pmtype_t pm_type;
    vec3_t origin;
    vec3_t velocity;
    int weaponTime;
    int weaponDelay;
    int grenadeTimeLeft;
    float leanf;
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
    int eFlags;
    int eventSequence;
    int events[4];
    int eventParms[4];
    int oldEventSequence;
    int clientNum;
    unsigned int weapon;
    unsigned int weaponstate;
    float fWeaponPosFrac;
    int viewmodelIndex;
    vec3_t viewangles;
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
    char _pad_trace[76];
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
    char _pad_to_session[POFF_SESSIONSTATE - 0x200];
} playerState_t;

typedef struct {
    sessionState_t sessionState;
    int forceSpectatorClient;
    int statusIcon;
    int archiveTime;
    int score;
    int deaths;
    clientConnected_t connected;
    usercmd_t cmd;
    usercmd_t oldcmd;
    qboolean localClient;
    char netname[MAX_NETNAME];
    int maxHealth;
} clientSession_t;

typedef struct gclient_s {
    char _raw[0x4734];
} gclient_t;

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

typedef struct gentity_s {
    entityState_t s;
    char _pad_after_es[EOFF_PLAYERSTATE - sizeof(entityState_t)];
    struct gclient_s *client;
    char _pad_after_client[EOFF_INUSE - EOFF_PLAYERSTATE - sizeof(void*)];
    char inuse;
    char _pad_after_inuse[EOFF_HEALTH - EOFF_INUSE - 1];
    int health;
    char _pad_to_end[GENTITY_SIZE_V15 - EOFF_HEALTH - sizeof(int)];
} gentity_t;

typedef struct {
    gclient_t *clients;
    gentity_t *gentities;
    int gentitySize;
    int num_entities;
    gentity_t *firstFreeEnt;
    gentity_t *lastFreeEnt;
    fileHandle_t logFile;
    int initializing;
    int maxclients;
    int framenum;
    int time;
    int previousTime;
    int frameTime;
    int startTime;
    int teamScores[TEAM_NUM_TEAMS];
    int lastTeammateHealthTime;
    qboolean bUpdateScoresForIntermission;
    int manualNameChange;
    int numConnectedClients;
    int sortedClients[MAX_CLIENTS];
    char voteString[1024];
} level_locals_t;

typedef struct {
    clientConnectState_t state;
    qboolean sendAsActive;
    const char *dropReason;
    char userinfo[1024];
    char _raw[CLIENT_T_SIZE_V15];
} client_t;

#define CLIENT_T_OFF_STATE          0x0000
#define CLIENT_T_OFF_USERINFO       0x000C
#define CLIENT_T_OFF_GENTITY        0x10A40
#define CLIENT_T_OFF_NAME           0x10A44
#define CLIENT_T_OFF_PING           0x5A7C0
#define CLIENT_T_OFF_NETCHAN        (CLIENT_T_SIZE_V15 - sizeof(netchan_t) - 64)

#define SVSCLIENT_STATE(ptr) \
    (*(clientConnectState_t*)((char*)(ptr)))

#define SVSCLIENT_NAME(ptr) \
    ((char*)((char*)(ptr) + CLIENT_T_OFF_NAME))

#define SVSCLIENT_NETCHAN(ptr) \
    ((netchan_t*)((char*)(ptr) + CLIENT_T_OFF_NETCHAN))

#define SVSCLIENT_IP(ptr) \
    (SVSCLIENT_NETCHAN(ptr)->remoteAddress)

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

#define ADDR_SVS                    0x083CCD84
/* NOTE: the real svs.clients pointer is at ADDR_SVS+0x0C = 0x083CCD90 (deref it,
 * e.g. svs->clients). 0x083CCDF0 below is STALE — it is the svs.challenges array. */
#define ADDR_SVS_CLIENTS            0x083CCDF0
#define ADDR_SVS_TIME               0x083CCD88

#define ADDR_COM_PRINTF             0x0806FC90
#define ADDR_COM_ERROR              0x0806FEF4
#define ADDR_SYS_LOADDLL            0x080D3DAD

#define ADDR_SV_INIT                0x080913B3
#define ADDR_SV_DIRECTCONNECT       0x08089E7E
#define ADDR_SV_GETCLIENTSCORE      0x08092421

#define ADDR_SCR_EXECTHREAD         0x080A95EC
#define ADDR_SCR_EXECENTTHREAD      0x080A9674
#define ADDR_SCR_FREETHREAD         0x080A97D4

#define ADDR_SV_SENDSERVERCOMMAND   0x0808B900
#define ADDR_SV_GENTITYNUM          0x08089258
#define ADDR_SV_GAMECLIENTNUM       0x08089270
/* CORRECTED 2026-06-29: 0x08085CF4 was a char-class helper (zero xrefs). The real
 * SV_DropClient(client_t *drop, const char *reason) is at 0x0808AC11 (verified via
 * "Going to CS_ZOMBIE for %s" xref + 18 call sites). */
#define ADDR_SV_DROPCLIENT          0x0808AC11
/* Engine helpers used by cod1reloaded.c (RE'd 2026-06-29): */
#define ADDR_CMD_ARGV               0x080600F4  /* char* Cmd_Argv(int) */
#define ADDR_INFO_VALUEFORKEY       0x08086397  /* char* Info_ValueForKey(info,key) */
#define ADDR_NET_OUTOFBANDPRINT     0x0808428E  /* void(int netsrc, netadr_t, fmt, ...) */
#define ADDR_SV_FRAME               0x0808CDF8
#define ADDR_SV_SHUTDOWNGAMEMODULE  0x0808AD8C
#define ADDR_SV_SPAWNSERVER         0x0808A220
#define ADDR_SV_MAPRESTART          0x08083DE4
#define ADDR_SV_MAP                 0x08083C68

#define ADDR_GETUSERINFO            0x0808B25C
#define ADDR_SETUSERINFO            0x0808B1D0

#define ADDR_CODE_BASE              0x08048000
#define ADDR_CODE_SIZE              0x135000

typedef void (*Com_Printf_t)(const char *fmt, ...);
typedef void (*Com_Error_t)(int code, const char *fmt, ...);

#define GAME_INIT               0
#define GAME_SHUTDOWN           1
#define GAME_CLIENT_CONNECT     2
#define GAME_CLIENT_BEGIN       3
#define GAME_CLIENT_USERINFO    4
#define GAME_CLIENT_DISCONNECT  5
#define GAME_CLIENT_COMMAND     6
#define GAME_CLIENT_THINK       7
#define GAME_RUN_FRAME          8
#define GAME_CONSOLE_COMMAND    9

#endif
