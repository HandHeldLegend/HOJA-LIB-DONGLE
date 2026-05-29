#ifndef HOJA_LIB_DONGLE_H
#define HOJA_LIB_DONGLE_H

#include <stdint.h>
#include <string.h>

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
    uint16_t id : 12;  // Random session id (gamepad); new value per session / reboot
} dongle_session_s;
#pragma pack(pop)

#define DONGLE_SESSION_S_LEN sizeof(dongle_session_s)

/** Pack session bitfield into the 16-bit wire value (pkt->session). */
static inline uint16_t dongle_session_pack(const dongle_session_s *s)
{
    uint16_t v = 0;
    memcpy(&v, s, sizeof(uint16_t));
    return v;
}

/** Unpack pkt->session into dongle_session_s. */
static inline void dongle_session_unpack(uint16_t packed, dongle_session_s *s)
{
    memcpy(s, &packed, sizeof(uint16_t));
}

#pragma pack(push, 1)
typedef struct
{
    dongle_session_s session; /* mode (4) + session id (12) — must match pkt->session */
    uint16_t vid;             /* USB vendor id for enumeration */
    uint16_t pid;             /* USB product id for enumeration */
} dongle_wake_s;
#pragma pack(pop)

/* Fixed WLAN endpoints — gamepad firmware uses the same values. */
#define DONGLE_WLAN_PORT 4444u
#define DONGLE_GAMEPAD_IP0 192u
#define DONGLE_GAMEPAD_IP1 168u
#define DONGLE_GAMEPAD_IP2 4u
#define DONGLE_GAMEPAD_IP3 16u
#define DONGLE_GAMEPAD_PORT DONGLE_WLAN_PORT

typedef enum 
{
    DONGLE_PID_WAKE = 0, // Wake packet, this is sent when the dongle is awaiting traffic from the gamepad
    DONGLE_PID_CORE_RELIABLE, // Reliable USB tunnel data (command replies, etc.)
    DONGLE_PID_CORE_UNRELIABLE, // High-rate input reports
    DONGLE_PID_STATUS, // Packet containing dongle_status_u data
} dongle_pid_t;

#pragma pack(push, 1)
typedef struct
{
    uint16_t session;   // dongle_session_s 
    uint16_t ack;       // Reliable packet ACK. 
    uint8_t id;         // dongle_pid_t
    uint16_t len;       // Data container used length
    uint8_t data[64];   // Data container
} dongle_pkt_s;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif