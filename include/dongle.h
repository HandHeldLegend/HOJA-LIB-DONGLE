#ifndef HOJA_LIB_DONGLE_H
#define HOJA_LIB_DONGLE_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DONGLE_LIB_DEBUG_LOG
#define DONGLE_LIB_DEBUG_LOG 1
#endif

typedef enum
{
    DONGLE_LINK_DOWN,
    DONGLE_LINK_UP,
} dongle_link_status_t;

typedef enum
{
    DONGLE_TRANSPORT_IDLE,
    DONGLE_TRANSPORT_CONNECTED,
} dongle_transport_status_t;

#pragma pack(push, 1)
typedef union
{
    struct
    {
        uint8_t transport_status; // dongle_transport_status_t;
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
    uint16_t session; // dongle_session_s
    uint16_t vid;     /* USB vendor id for enumeration */
    uint16_t pid;     /* USB product id for enumeration */
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

typedef struct
{
    uint8_t pin[4]; // 4 digit pin, each number 0-9 used to generate the SSID+Password (HOJA_DONGLE_XXXX)
} dongle_cfg_host_s;

typedef struct
{
    uint8_t pin[4]; // 4 digit pin, each number 0-9, used to determine the SSID+Password (HOJA_DONGLE_XXXX)
    dongle_mode_t mode;
    uint16_t vid;
    uint16_t pid;
} dongle_cfg_gp_s;

/* -------------------------------------------------------------------------- */
/* HOST API                                                                */
/* -------------------------------------------------------------------------- */

void dongle_api_host_init(dongle_cfg_host_s *cfg);
void dongle_api_host_task(void);

// Called by the API when the host needs to bring up its SSID/Password
void dongle_api_host_hook_bringup(const char *ssid, const char *pw);

// Call on the dongle host in your UDP receiving ISR
// This feeds the packet into the internal queue
void dongle_api_host_udp_rx(dongle_pkt_s *in);


// Call when we recieve outputreports over USB. The API queues these automatically
void dongle_api_host_add_outputreport(const uint8_t *data, uint16_t len);
// Call to retrieve the most up to date inputreport for USB or other input modes
bool dongle_api_host_get_inputreport(const uint8_t *data, uint16_t len);


// Called by the API when it has a dongle_pkt_s ready to transmit
void dongle_api_host_hook_udp_tx(dongle_pkt_s *out, uint8_t ip[4], uint16_t port);
// Called by the API when the link status changes
void dongle_api_host_hook_linkstatus(dongle_link_status_t status);

// Call on our transport layer to mark when
// a packet was sent to help gate the packet
// timing
void dongle_api_host_pump_mark_sent(void);
// Call to set rumble status
void dongle_api_host_set_rumble(uint8_t rumble_left, uint8_t rumble_right, uint8_t brake_left, uint8_t brake_right);
// Call to set transport connected status
void dongle_api_host_set_transport(bool connected);
// Call to set player number
void dongle_api_host_set_player(uint8_t player_number);

/* -------------------------------------------------------------------------- */
/* GAMEPAD API                                                                */
/* -------------------------------------------------------------------------- */

void dongle_api_gp_init(dongle_cfg_gp_s *cfg);
void dongle_api_gp_task(void);

// Call on the gamepad in your UDP receiving ISR
void dongle_api_gp_udp_rx(dongle_pkt_s *in);
// Called by the API when there's a dongle_pkt_s that is ready to transmit
void dongle_api_gp_hook_udp_tx(const dongle_pkt_s *out, uint8_t ip[4], uint16_t port);

// This is called by the API task loop when it needs to form the next packet
// It's up to the gamepad to decide what will be treated as reliable or not
bool dongle_api_gp_hook_get_inputreport(const uint8_t *data, uint16_t *len, bool *reliable);
// The API calls this when there is an outputreport data that must be forwarded
void dongle_api_gp_hook_set_outputreport(const uint8_t *data, uint16_t len);

// Call this when the gamepad connects
void dongle_api_gp_onconnect(void);
// Call this when the gamepad disconnects
void dongle_api_gp_ondisconnect(void);
// Call this when the gamepad connection attempt fails
void dongle_api_gp_onconnectfail(void);

// API calls this when we are ready to attempt
// a connection (ASYNC, handled by api task)
void dongle_api_gp_hook_connect(const char *ssid, const char *pw);

// Return the system time in microseconds
uint64_t dongle_api_hook_time_us(void);

void dongle_api_gp_hook_set_rumble(uint8_t rumble_left, uint8_t rumble_right, uint8_t brake_left, uint8_t brake_right);
void dongle_api_gp_hook_set_transport(bool connected);
void dongle_api_gp_hook_set_player(uint8_t player_number);

void dongle_api_hook_set_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]);
void dongle_api_hook_port_bind(uint16_t port);
void dongle_api_hook_port_unbind(void);
void dongle_api_hook_bringup(void);
void dongle_api_hook_teardown(void);

#ifdef __cplusplus
}
#endif

#endif