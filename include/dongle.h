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

/* Bump when a packet changes. The dongle and gamepad only talk on the same version. */
#define DONGLE_PROTOCOL_VERSION             2u

/* Every dongle's network is HOJA_WLAN_ followed by its four PIN digits */
#define DONGLE_WLAN_SSID_PREFIX             "HOJA_WLAN_"


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
    DONGLE_MODE_UNKNOWN = 15, // Nothing detected (fits the 4-bit session field)
} dongle_mode_t;

typedef enum
{
    DONGLE_BOARD_UNKNOWN = 0,
    DONGLE_BOARD_HOJA    = 1,
    DONGLE_BOARD_PICO_W  = 2,
    DONGLE_BOARD_PICO2_W = 3,
} dongle_board_t;

/* What the dongle found itself plugged into */
typedef enum
{
    DONGLE_HOST_UNKNOWN  = 0,
    DONGLE_HOST_PC       = 1,
    DONGLE_HOST_SWITCH   = 2,
    DONGLE_HOST_N64      = 3,
    DONGLE_HOST_GAMECUBE = 4,
} dongle_host_kind_t;

/* Payload of the dongle's WAKE beacon: who it is and what it is plugged into */
#pragma pack(push, 1)
typedef struct
{
    uint8_t  protocol;   /* DONGLE_PROTOCOL_VERSION */
    uint8_t  board;      /* dongle_board_t */
    uint8_t  host_kind;  /* dongle_host_kind_t */
    uint8_t  host_mode;  /* dongle_mode_t to use for that host, or DONGLE_MODE_UNKNOWN */
    uint32_t fw_version; /* Dongle firmware build */
} dongle_beacon_s;
#pragma pack(pop)

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
#define DONGLE_WAKE_MANUFACTURER_LEN 20u
#define DONGLE_WAKE_S_LEN            64u

/* dongle_wake_s.flags */
#define DONGLE_WAKE_FLAG_MODE_FORCED 0x01u /* The gamepad chose its mode instead of following the dongle */

#pragma pack(push, 1)
typedef struct
{
    uint16_t session; /* dongle_session_s (packed) */
    uint16_t vid;     /* USB vendor id for enumeration */
    uint16_t pid;     /* USB product id for enumeration */
    uint8_t  protocol;   /* DONGLE_PROTOCOL_VERSION */
    uint8_t  flags;      /* DONGLE_WAKE_FLAG_* */
    uint32_t fw_version; /* Gamepad firmware build */
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

/* WLAN endpoints. Gamepads take an address from the dongle's DHCP server, so several can join
 * one dongle's network without clashing; the dongle answers whichever one it adopted. */
#define DONGLE_WLAN_PORT    4444u
#define DONGLE_GAMEPAD_PORT DONGLE_WLAN_PORT

/* Beacons go to every gamepad on the network until one is adopted */
#define DONGLE_BROADCAST_IP0 192u
#define DONGLE_BROADCAST_IP1 168u
#define DONGLE_BROADCAST_IP2 4u
#define DONGLE_BROADCAST_IP3 255u

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
    uint8_t  pin[4]; /* 4-digit WLAN pin (0000-9999) -> HOJA_WLAN_XXXX / HOJA_XXXX */
    uint8_t  board;  /* dongle_board_t */
    uint32_t fw_version;
} dongle_cfg_host_s;

/* PIN 0000 is never generated by a dongle; on the gamepad it means "no dongle paired" */
#define DONGLE_PIN_NONE 0u

typedef struct
{
    uint8_t pin[4]; /* Paired dongle's PIN, or all zero (DONGLE_PIN_NONE) */
    bool pairing;   /* Pair with the strongest free dongle and report it through hook_paired */
    bool mode_forced; /* Keep this mode rather than follow the dongle's */
    uint32_t fw_version;
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