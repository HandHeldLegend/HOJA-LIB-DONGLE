#include <dongle.h>
#include <dongle_gamepad.h>
#include <dongle_log.h>

/*
 * Compile-time guard from the guide's bring-up checklist (§14): with the
 * mandatory #pragma pack(push,1) the wire struct is exactly 71 bytes
 * (2 + 2 + 1 + 2 + 64). If padding sneaks in, the dongle's length filter
 * silently drops every datagram we send.
 */
_Static_assert(sizeof(dongle_pkt_s) == 71, "dongle_pkt_s must be packed to 71 bytes");

static uint8_t _dongle_gamepad_address[4] = {DONGLE_GAMEPAD_IP0, DONGLE_GAMEPAD_IP1, DONGLE_GAMEPAD_IP2, DONGLE_GAMEPAD_IP3};
static uint8_t _dongle_host_address[4] = {DONGLE_HOST_IP0, DONGLE_HOST_IP1, DONGLE_HOST_IP2, DONGLE_HOST_IP3};
static uint8_t _dongle_host_mask[4] = {DONGLE_NETMASK0, DONGLE_NETMASK1, DONGLE_NETMASK2, DONGLE_NETMASK3};

#define DONGLE_GAMEPAD_REPORT_LOG_INTERVAL_MS 4000
#define DONGLE_GAMEPAD_LINK_STATUS_INTERVAL_MS 1000
#define DONGLE_GAMEPAD_CONNECTION_FAIL_TIMEOUT_MS 10000

typedef enum
{
    DONGLE_GAMEPAD_CONNPHASE_UNDEFINED = 0,
    DONGLE_GAMEPAD_CONNPHASE_DOWN = 1,
    DONGLE_GAMEPAD_CONNPHASE_CONNECTING = 2,
    DONGLE_GAMEPAD_CONNPHASE_CONNECTED = 3,
    DONGLE_GAMEPAD_CONNPHASE_UP = 4,
} dongle_gamepad_connphase_t;

typedef struct
{
    uint32_t rx_packets;    /* datagrams received this interval */
    uint32_t input_reports; /* unreliable input reports sent this interval */
    uint32_t rx_dropped;    /* free-running drop counter (reserved for FIFO mode) */
    uint32_t last_ms;       /* timestamp of the last stats print */
    uint32_t last_dropped;  /* rx_dropped snapshot at the last print */
} dongle_gamepad_logging_sm;

typedef struct
{
    dongle_session_s session;
    dongle_wake_s wake;
    dongle_status_s status;

    uint16_t last_reliable_ack;
    bool have_reliable_ack;

    bool wake_replied;

    dongle_link_status_t wlan_link;
    uint64_t wlan_connection_deadline_us;

    dongle_gamepad_connphase_t wlan_connphase;

    /* USB identity / personality advertised to the dongle in WAKE replies. */
    dongle_mode_t mode;
    uint16_t pid;
    uint16_t vid;

    bool rx_got;

    dongle_gamepad_logging_sm logging;
} dongle_gamepad_sm;

static dongle_gamepad_sm _gp = {0};

/* -------------------------------------------------------------------------- */
/* GAMEPAD HOOKS (weak defaults)                                              */
/* -------------------------------------------------------------------------- */

// Implement WiFI Link Status Checking
__attribute__((weak)) dongle_link_status_t dongle_api_gamepad_hook_link_up(void)
{
    return DONGLE_LINK_DOWN;
}

// Implement setting static IP
__attribute__((weak)) bool dongle_api_gamepad_hook_apply_static_ip(uint8_t addr[4], uint8_t mask[4], uint8_t gateway[4])
{
    (void)addr;
    (void)mask;
    (void)gateway;
    return false;
}

// Implement SSID connection
__attribute__((weak)) void dongle_api_gamepad_hook_connect_async(const char *ssid, const char *pw)
{
    (void)ssid;
    (void)pw;
}

// Implement UDP transmission
__attribute__((weak)) void dongle_api_gamepad_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port)
{
    (void)pkt;
    (void)ip;
    (void)port;
}

// Implement obtaining the latest input report
__attribute__((weak)) bool dongle_api_gamepad_hook_get_inputreport(uint8_t data[64], uint16_t *len, bool *reliable)
{
    (void)data;
    (void)len;
    (void)reliable;
    return false;
}

// Implement forwarding output reports
__attribute__((weak)) void dongle_api_gamepad_hook_set_outputreport(const uint8_t data[64], uint16_t len)
{
    (void)data;
    (void)len;
}

// Implement player number change callback
__attribute__((weak)) void dongle_api_gamepad_hook_set_player(uint8_t player_number)
{
    (void)player_number;
}

// Implement transport status callback
__attribute__((weak)) void dongle_api_gamepad_hook_set_transport(bool connected)
{
    (void)connected;
}

// Implement rumble change callback
__attribute__((weak)) void dongle_api_gamepad_hook_set_rumble(uint8_t left, uint8_t right, uint8_t left_brake, uint8_t right_brake)
{
    (void)left;
    (void)right;
    (void)left_brake;
    (void)right_brake;
}

// Implement network reset callback (weak stub)
__attribute__((weak)) void dongle_api_gamepad_hook_reset_network(void)
{
    // Default: do nothing. Strong implementation should reset the network stack.
}

/* -------------------------------------------------------------------------- */
/* GAMEPAD APP                                                                */
/* -------------------------------------------------------------------------- */

/* Pick a fresh 12-bit session id (1..0xFFF). A new id tells the dongle that a
 * new client attached or the gamepad rebooted, forcing a clean core_init. */
static uint16_t _gamepad_new_session_id(void)
{
    uint16_t sid = dongle_api_hook_get_rand_u16() & 0xFFFu;
    if (sid == 0)
    {
        sid = 1;
    }
    return sid;
}

/* Refresh the cached WAKE body for the current mode / session / USB identity.
 * A new random session id is chosen here so every (re)connection forces the
 * dongle to re-initialize its console core (guide §4: never reuse the id). */
static void _gamepad_refresh_wake(void)
{
    _gp.session.mode = (uint16_t)_gp.mode;
    _gp.session.id = _gamepad_new_session_id();

    /*
     * dongle_wake_s.session is the PACKED uint16_t (identical to pkt->session),
     * NOT a nested dongle_session_s struct (guide §4). Pack it explicitly.
     */
    _gp.wake.session = dongle_session_pack(&_gp.session);
    _gp.wake.vid = _gp.vid;
    _gp.wake.pid = _gp.pid;

    DONGLE_LOGF("[DGP] New session id 0x%03X (mode %u, vid 0x%04X, pid 0x%04X)\n",
                _gp.session.id, (unsigned)_gp.session.mode, _gp.vid, _gp.pid);
}

static void _gamepad_reset(uint64_t now_us)
{
    (void)now_us;

    /* Preserve the configured identity (mode/vid/pid); reset only link state. */
    _gp.status = (dongle_status_s){0};

    _gp.last_reliable_ack = 0;
    _gp.have_reliable_ack = false;

    _gp.wake_replied = false;

    _gp.wlan_link = DONGLE_LINK_UNDEFINED;
    _gp.wlan_connection_deadline_us = 0;
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;

    _gp.rx_got = false;

    /* Fresh session id for the next connection (guide §4: never reuse). */
    _gamepad_refresh_wake();

    // Notify upper layers to reset their network stack/state
    dongle_api_gamepad_hook_reset_network();
}

static bool _gamepad_connect_attempt(uint64_t now_us)
{
    // Do not spam attempts
    if(_gp.wlan_connection_deadline_us>0)
    {
        if(now_us >= _gp.wlan_connection_deadline_us)
        {
            _gp.wlan_connection_deadline_us = 0;
        }
        else
        {
            return false;
        }
    }

    // Start async connection
    dongle_api_gamepad_hook_connect_async(DONGLE_DEFAULT_WLAN_SSID, DONGLE_DEFAULT_WLAN_PASSWORD);

    _gp.wlan_connection_deadline_us = now_us + (DONGLE_GAMEPAD_CONNECTION_FAIL_TIMEOUT_MS*1000);

    return true;
}

static void _gamepad_poll(uint64_t now_us)
{
    static uint64_t last_check = 0;
    uint64_t delta = now_us - last_check;

    if(delta >= (DONGLE_GAMEPAD_LINK_STATUS_INTERVAL_MS*1000))
    {
        dongle_link_status_t link = dongle_api_gamepad_hook_link_up();

        if(_gp.wlan_link != link)
        {
            // New link status
            switch(link)
            {
                default:
                case DONGLE_LINK_UNDEFINED:
                break;

                case DONGLE_LINK_DOWN:
                // Adjust behavior based on prior state
                if(_gp.wlan_link == DONGLE_LINK_UP)
                {
                    _gamepad_reset(now_us);
                    return;
                }
                else 
                {
                    if(_gamepad_connect_attempt(now_us))
                    {
                        _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_CONNECTING;
                    }
                }
                break;

                case DONGLE_LINK_UP:
                _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_CONNECTED;
                break;
            }

            // Set the link
            _gp.wlan_link = link;
        }

        switch(_gp.wlan_connphase)
        {
            default:
            break;

            case DONGLE_GAMEPAD_CONNPHASE_CONNECTING:
            if(now_us >= _gp.wlan_connection_deadline_us)
            {
                // Connection failed: reset and mark for retry
                _gamepad_reset(now_us);
                return;
            }
            break;

            case DONGLE_GAMEPAD_CONNPHASE_CONNECTED:
            if(dongle_api_gamepad_hook_apply_static_ip(_dongle_gamepad_address, _dongle_host_mask, _dongle_host_address))
            {
                _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_UP;
            }
            else
            {
                // Failed to apply IP: reset state and retry
                _gamepad_reset(now_us);
                return;
            }
            break; 
        }

        last_check = now_us;
    }
}

static void _gamepad_apply_status(const dongle_status_s *status)
{
    if(_gp.status.transport_status != status->transport_status)
    {
        dongle_api_gamepad_hook_set_transport(status->transport_status == DONGLE_TRANSPORT_CONNECTED);
    }

    if(_gp.status.player_number != status->player_number)
    {
        dongle_api_gamepad_hook_set_player(status->player_number);
    }

    if(_gp.status.rumble_value != status->rumble_value)
    {
        dongle_api_gamepad_hook_set_rumble(
            status->rumble.left, status->rumble.right, 
            status->brake.left, status->brake.right
        );
    }

    _gp.status = *status;
}

static void _gamepad_build_input_reply(dongle_pkt_s *tx)
{
    uint8_t report[64] = {0};
    uint16_t len = 0;
    bool reliable = false;

    tx->id = DONGLE_PID_CORE_UNRELIABLE;

    if(dongle_api_gamepad_hook_get_inputreport(report, &len, &reliable))
    {
        if(reliable)
        {
            tx->id = DONGLE_PID_CORE_RELIABLE;
        }
        else
        {
            /* Count steady-state unreliable input reports for the stats line. */
            _gp.logging.input_reports++;
        }

        if(len > sizeof(tx->data))
        {
            len = sizeof(tx->data);
        }

        tx->len = len;
        memcpy(tx->data, report, len);
    }
    else
    {
        tx->len = 0;
    }
}

static void _gamepad_build_wake_reply(dongle_pkt_s *tx)
{
    // Session already exists
    tx->id      = DONGLE_PID_WAKE;
    tx->len     = (uint16_t)sizeof(dongle_wake_s);
    memcpy(tx->data, &_gp.wake, sizeof(dongle_wake_s));

    DONGLE_LOGF("[DGP] Replying to WAKE beacon (session 0x%03X)\n", _gp.session.id);
}

static void _gamepad_process_packet(const dongle_pkt_s *rx)
{
    _gp.rx_got = true;
    /*
     * Prepare a zeroed reply tagged with our session. Echo this datagram's ack
     * token directly: the dongle checks pkt->ack on every RX, so echoing rx.ack
     * on any reply opportunistically retires whatever it has inflight and is the
     * documented, robust behavior (guide §9). It is harmless on non-reliable
     * traffic, where rx.ack is simply 0.
     */
    dongle_pkt_s tx = {0};
    tx.session = dongle_session_pack(&_gp.session);
    tx.ack     = rx->ack;

    bool send_reply = true;

    switch ((dongle_pid_t)rx->id)
    {
    case DONGLE_PID_WAKE:
        /* Only beacons (len == 0) get a WAKE reply, and only the first one in a
         * run of repeats: the dongle floods beacons until we are acknowledged,
         * but replying to each restarts our input/session and spams the host. */
        if (rx->len == 0 && !_gp.wake_replied)
        {
            _gamepad_build_wake_reply(&tx);
            _gp.wake_replied = true;
        }
        else
        {
            send_reply = false;
        }
        break;

    case DONGLE_PID_STATUS:
        /* Any non-WAKE traffic means the dongle has advanced past the beacon
         * phase, so re-arm WAKE handling for the next beacon run. */
        _gp.wake_replied = false;

        /* A STATUS payload is exactly sizeof(dongle_status_s) bytes. */
        if (rx->len == sizeof(dongle_status_s))
        {
            _gamepad_apply_status((const dongle_status_s *)rx->data);
        }
        _gamepad_build_input_reply(&tx);
        break;

    case DONGLE_PID_CORE_RELIABLE:
        /* Non-WAKE traffic: re-arm WAKE handling (see STATUS case above). */
        _gp.wake_replied = false;

        /* Host OUT (Switch commands / rumble). Hand it to the protocol engine;
         * any reply it queues is popped by the input report build below and
         * delivered in this same reply. rx->ack was already echoed into tx.ack
         * above, which retires the dongle's inflight reliable packet (guide §9).
         * The output handler should be idempotent on resends. */
        if (rx->len > 0)
        {
            /* Stop-and-wait dedup: the dongle resends the same CORE_RELIABLE
             * packet (identical ack token) until it sees the token echoed back.
             * Tunnel each host OUT exactly once; a resend carrying an ack we have
             * already processed is a duplicate and must not be re-queued (doing so
             * would emit duplicate command replies). We still fall through to send
             * a reply below, echoing rx.ack so the dongle can retire the packet. */
            if (_gp.have_reliable_ack && rx->ack == _gp.last_reliable_ack)
            {
                /* Duplicate resend; silently re-ack below without re-tunneling. */
            }
            else
            {
                dongle_api_gamepad_hook_set_outputreport(rx->data, rx->len);
                _gp.last_reliable_ack = rx->ack;
                _gp.have_reliable_ack = true;
            }
        }
        _gamepad_build_input_reply(&tx);
        break;

    case DONGLE_PID_BULK_UNRELIABLE:
    case DONGLE_PID_CONFIG_RELIABLE:
    default:
        /* Dongle sent something we do not answer. Stay silent (no proactive TX). */
        send_reply = false;
        break;
    }

    if (send_reply)
    {
        dongle_api_gamepad_hook_udp_tx(&tx, _dongle_host_address, DONGLE_HOST_PORT);
    }
}

/* Print a single throughput line once per DONGLE_GAMEPAD_REPORT_LOG_INTERVAL_MS.
 * Compiled out unless DONGLE_LIB_DEBUG_GAMEPADSTATS is enabled. */
static void _gamepad_report_stats(uint64_t now_us)
{
#if defined(DONGLE_LIB_DEBUG_GAMEPADSTATS) && (DONGLE_LIB_DEBUG_GAMEPADSTATS == 1)
    uint32_t now_ms = (uint32_t)(now_us / 1000u);
    uint32_t elapsed = now_ms - _gp.logging.last_ms;
    if (elapsed < DONGLE_GAMEPAD_REPORT_LOG_INTERVAL_MS)
    {
        return;
    }

    uint32_t rx_packets = _gp.logging.rx_packets;
    _gp.logging.rx_packets = 0;
    uint32_t reports = _gp.logging.input_reports;
    _gp.logging.input_reports = 0;
    _gp.logging.last_ms = now_ms;

    uint32_t dropped_total = _gp.logging.rx_dropped;
    uint32_t dropped = dropped_total - _gp.logging.last_dropped;
    _gp.logging.last_dropped = dropped_total;

    uint32_t rx_rate = (elapsed > 0) ? (rx_packets * 1000u) / elapsed : rx_packets;
    uint32_t report_rate = (elapsed > 0) ? (reports * 1000u) / elapsed : reports;

    DONGLE_LOGF("[DGP] rx %lu pkt/s (from host) | input %lu rep/s (to dongle) | dropped %lu\n",
                (unsigned long)rx_rate, (unsigned long)report_rate, (unsigned long)dropped);
#else
    (void)now_us;
#endif
}

/* -------------------------------------------------------------------------- */
/* GAMEPAD API                                                                */
/* -------------------------------------------------------------------------- */

void dongle_api_gamepad_wlan_init(const dongle_cfg_gamepad_s *cfg)
{
    memset(&_gp, 0, sizeof(_gp));

    if (cfg != NULL)
    {
        _gp.mode = cfg->mode;
        _gp.vid = cfg->vid;
        _gp.pid = cfg->pid;
    }

    _gp.wlan_link = DONGLE_LINK_UNDEFINED;
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;

    /* Pick the first session id before any traffic flows. */
    _gamepad_refresh_wake();
}

void dongle_api_gamepad_udp_rx(const dongle_pkt_s *pkt)
{
    if (pkt == NULL)
    {
        return;
    }

    _gp.logging.rx_packets++;

    /* Inline processing: the platform calls this only when a valid, fixed-size
     * datagram from the dongle endpoint has arrived. At most one reply is
     * emitted (one TX per RX). */
    _gamepad_process_packet(pkt);
}

void dongle_api_gamepad_wlan_task(void)
{
    uint64_t now_us = dongle_api_hook_get_time_us_u64();

    // Poll connection / link state machine
    _gamepad_poll(now_us);

    // Report stats (compiled out unless DONGLE_LIB_DEBUG_GAMEPADSTATS == 1)
    _gamepad_report_stats(now_us);
}
