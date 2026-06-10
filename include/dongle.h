#ifndef HOJA_LIB_DONGLE_H
#define HOJA_LIB_DONGLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//#ifndef DONGLE_LIB_DEBUG_LOG
//#define DONGLE_LIB_DEBUG_LOG 0
//#endif

//#ifndef DONGLE_LIB_DEBUG_GAMEPADSTATS
//#define DONGLE_LIB_DEBUG_GAMEPADSTATS 0
//#endif

#define DONGLE_DEFAULT_WLAN_SSID            "HOJA_WLAN_1234"
#define DONGLE_DEFAULT_WLAN_PASSWORD        "HOJA_1234"


typedef enum
{
    DONGLE_LINK_UNDEFINED,
    DONGLE_LINK_DOWN,
    DONGLE_LINK_UP,
} dongle_link_status_t;

typedef enum
{
    DONGLE_TRANSPORT_IDLE,
    DONGLE_TRANSPORT_CONNECTED,
} dongle_transport_status_t;

#pragma pack(push, 1)
typedef struct 
{
    uint8_t transport_status; // dongle_transport_status_t;
    uint8_t player_number;
    union
    {
        struct
        {
        
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
        uint32_t rumble_value;
    };
} dongle_status_s;
#pragma pack(pop)

#define DONGLE_STATUS_U_LEN sizeof(dongle_status_s)

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

#define DONGLE_WAKE_NAME_LEN         32u
#define DONGLE_WAKE_MANUFACTURER_LEN 26u
#define DONGLE_WAKE_S_LEN            64u

#pragma pack(push, 1)
typedef struct
{
    uint16_t session; /* dongle_session_s (packed) */
    uint16_t vid;     /* USB vendor id for enumeration */
    uint16_t pid;     /* USB product id for enumeration */
    uint8_t  name[DONGLE_WAKE_NAME_LEN];         /* USB product string (NUL-terminated) */
    uint8_t  manufacturer[DONGLE_WAKE_MANUFACTURER_LEN]; /* USB manufacturer string (NUL-terminated) */
} dongle_wake_s;
#pragma pack(pop)

/** Copy a null-terminated C string into a fixed wake field (always NUL-terminated). */
static inline void dongle_wake_strcopy(uint8_t *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }

    if (src == NULL)
    {
        memset(dst, 0, dst_len);
        return;
    }

    size_t n = dst_len - 1u;
    strncpy((char *)dst, src, n);
    dst[n] = '\0';
}

/* Fixed WLAN endpoints — gamepad firmware uses the same values. */
#define DONGLE_WLAN_PORT    4444u

#define DONGLE_GAMEPAD_IP0  192u
#define DONGLE_GAMEPAD_IP1  168u
#define DONGLE_GAMEPAD_IP2  4u
#define DONGLE_GAMEPAD_IP3  16u
#define DONGLE_GAMEPAD_PORT DONGLE_WLAN_PORT

/* The dongle (AP) lives at 192.168.4.1 and listens on DONGLE_WLAN_PORT. */
#define DONGLE_HOST_IP0      192u
#define DONGLE_HOST_IP1      168u
#define DONGLE_HOST_IP2      4u
#define DONGLE_HOST_IP3      1u
#define DONGLE_HOST_PORT     DONGLE_WLAN_PORT

#define DONGLE_NETMASK0        255
#define DONGLE_NETMASK1        255
#define DONGLE_NETMASK2        255
#define DONGLE_NETMASK3        0

typedef enum 
{
    DONGLE_PID_WAKE = 0, // Wake packet, this is sent when the dongle is awaiting traffic from the gamepad
    DONGLE_PID_CORE_RELIABLE, // Reliable USB tunnel data (command replies, etc.)
    DONGLE_PID_CORE_UNRELIABLE, // High-rate input reports
    DONGLE_PID_STATUS, // Packet containing dongle_status_u data
    DONGLE_PID_BULK_UNRELIABLE, // Unreliable USB bulk tunnel for webUSB reports
    DONGLE_PID_CONFIG_RELIABLE, // Reliable USB tunnel data for configuration data
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

#pragma pack(push, 1)
typedef struct
{
    dongle_pkt_s pkt;
    uint8_t ip[4];
    uint16_t port;
} dongle_udp_frame_s;
#pragma pack(pop)

typedef struct
{
    bool rumble;
    bool player_number;
    bool transport_status;
} dongle_status_evt_subscription_s;

typedef struct
{
    uint8_t pin[4]; /* 4-digit WLAN pin (0000-9999) -> HOJA_WLAN_XXXX / HOJA_XXXX */
} dongle_cfg_host_s;

typedef struct
{
    uint8_t pin[4]; /* 4-digit WLAN pin (0000-9999) -> HOJA_WLAN_XXXX / HOJA_XXXX */
    dongle_mode_t mode;
    dongle_status_evt_subscription_s evt;
    uint16_t vid;
    uint16_t pid;
    uint8_t  name[DONGLE_WAKE_NAME_LEN];
    uint8_t  manufacturer[DONGLE_WAKE_MANUFACTURER_LEN];
} dongle_cfg_gamepad_s;

/* -------------------------------------------------------------------------- */
/* UTILS API                                                                  */
/* -------------------------------------------------------------------------- */

uint64_t dongle_api_hook_get_time_us_u64(void);

uint16_t dongle_api_hook_get_rand_u16(void);

uint16_t dongle_api_generate_ack(void);

/** Expand a stored key (0-9999) into four decimal digits. */
static inline void dongle_wlan_pin_from_u16(uint16_t key, uint8_t pin[4])
{
    key = (uint16_t)(key % 10000u);

    if (pin == NULL)
    {
        return;
    }

    pin[0] = (uint8_t)(key / 1000u);
    pin[1] = (uint8_t)((key / 100u) % 10u);
    pin[2] = (uint8_t)((key / 10u) % 10u);
    pin[3] = (uint8_t)(key % 10u);
}

/** Pack four decimal digits into a 0-9999 key. */
static inline uint16_t dongle_wlan_pin_to_u16(const uint8_t pin[4])
{
    if (pin == NULL)
    {
        return 0;
    }

    return (uint16_t)(((pin[0] % 10u) * 1000u) + ((pin[1] % 10u) * 100u) +
                      ((pin[2] % 10u) * 10u) + (pin[3] % 10u));
}

void dongle_wlan_format_ssid(const uint8_t pin[4], char *dst, size_t dst_len);

void dongle_wlan_format_password(const uint8_t pin[4], char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif