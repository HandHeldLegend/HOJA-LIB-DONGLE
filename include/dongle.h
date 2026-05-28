#ifndef HOJA_LIB_DONGLE_H
#define HOJA_LIB_DONGLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DONGLE_CONN_IDLE,
    DONGLE_CONN_CONNECTED,
} dongle_connection_t;

#pragma pack(push, 1)
typedef union
{
    struct
    {
        uint8_t connection; // dongle_connection_t
        uint8_t player_number;
        struct
        {
            uint8_t left;
            uint8_t right;
        } rumble;
        struct 
        {
            uint8_t left;
            uint8_t right;
        } brake;
    };
    uint64_t value;
} dongle_status_u;
#pragma pack(pop)

#define DONGLE_STATUS_U_LEN sizeof(dongle_status_u)

typedef enum
{
    DONGLE_MODE_SWITCH = 0,
    DONGLE_MODE_SINPUT = 1,
    DONGLE_MODE_XINPUT = 2,
    DONGLE_MODE_SLIPPI = 3,
    DONGLE_MODE_SNES = 4,
    DONGLE_MODE_N64 = 5,
    DONGLE_MODE_GAMECUBE = 6,
} dongle_mode_t;

#pragma pack(push, 1)
typedef struct
{
    uint16_t mode : 4; // dongle_mode_t
    uint16_t id : 12;
} dongle_session_s;
#pragma pack(pop)

#define DONGLE_SESSION_S_LEN sizeof(dongle_session_s)

typedef enum 
{
    DONGLE_PID_WAKE = 0, // Wake packet, this is sent when the dongle is awaiting traffic from the gamepad
    DONGLE_PID_CORE, // Packet containing raw USB data
    DONGLE_PID_STATUS, // Packet containing dongle_status_u data
} dongle_pid_t;

#pragma pack(push, 1)
typedef struct
{
    uint8_t mode;
    uint16_t id;
    uint16_t vid;
    uint16_t pid;
} dongle_wake_s;
#pragma pack(pop)

typedef struct
{
    uint16_t session;   // dongle_session_s 
    uint16_t counter;   // Packet counter. 
    uint8_t id;         // dongle_pid_t
    uint16_t len;       // Data container used length
    uint8_t data[64];   // Data container
} dongle_pkt_s;

#ifdef __cplusplus
}
#endif

#endif