#include <string.h>
#include <stddef.h>

#include <dongle.h>
#include <dongle_host.h>
#include <dongle_log.h>
#include <dongle_crosscore.h>

/*
 * Wire-format guard (matches dongle_gamepad.c): with #pragma pack(push,1) the
 * packet is exactly 73 bytes (2 + 2 + 2 + 1 + 2 + 64). If padding sneaks in, the
 * peer's length filter silently drops every datagram.
 */
_Static_assert(sizeof(dongle_pkt_s) == 73, "dongle_pkt_s must be packed to 73 bytes");
_Static_assert(sizeof(dongle_wake_s) == DONGLE_WAKE_S_LEN, "dongle_wake_s must fill the 64-byte data field");

#define DONGLE_HOST_TIMEOUT_MS 5000
#define DONGLE_HOST_PUMP_MAX_WAIT_MS 1000
#define DONGLE_HOST_PUMP_MAX_WAIT_US (DONGLE_HOST_PUMP_MAX_WAIT_MS*1000)

#define DONGLE_HOST_PUMP_MIN_ALLOWED_US 1900

#define DONGLE_HOST_MAILBOX_LEN 64

static uint8_t _broadcast_address[4] = {DONGLE_BROADCAST_IP0, DONGLE_BROADCAST_IP1, DONGLE_BROADCAST_IP2, DONGLE_BROADCAST_IP3};
static uint8_t _host_address[4]    = {DONGLE_HOST_IP0, DONGLE_HOST_IP1, DONGLE_HOST_IP2, DONGLE_HOST_IP3};

/* Who we are, sent in every beacon (set once at init, read by the wlan task) */
static uint8_t  _host_board = DONGLE_BOARD_UNKNOWN;
static uint32_t _host_fw_version = 0;
static uint8_t _host_mask[4]       = {DONGLE_NETMASK0, DONGLE_NETMASK1, DONGLE_NETMASK2, DONGLE_NETMASK3};

/* ==========================================================================
 * Cross-core state (strict single-producer / single-consumer)
 *
 *   W = wlan task (radio)        T = transport task (console)
 *
 *   object                producer  consumer  purpose
 *   --------------------  --------  --------  --------------------------------
 *   _session_host_ss        T         W       adopted session (mode/id)
 *   _status_host_ss         T         W       rumble/player/transport out
 *   _pump_host_ss           T         W       host-paced TX deadline
 *   _link_host_ss           W         T       wlan link status (UP/DOWN)
 *   _unreliable_rx_host_ss  W         T       latest unreliable input report
 *   _reliable_rx_host_ss    W         T       reliable input report queue
 *   _control_rx_host_ss     W         T       WAKE / control packets
 *   _reliable_tx_host_ss    T         W       reliable host->gamepad OUT queue
 *   _detect_host_ss         T         W       what the dongle is plugged into
 *   _config_rx_host_ss      W         T       gamepad WebUSB replies for the app
 *   _bulk_rx_host_ss        W         T       gamepad's latest WebUSB input report
 * ========================================================================== */

DONGLE_CROSSCORE_SNAPSHOT_TYPE(session, dongle_session_s);
dongle_snapshot_session_t _session_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(status, dongle_status_s);
dongle_snapshot_status_t _status_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(pump, uint64_t);
dongle_snapshot_pump_t _pump_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(link, uint8_t);
dongle_snapshot_link_t _link_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(packet, dongle_pkt_s);
dongle_snapshot_packet_t _unreliable_rx_host_ss;

typedef struct
{
    uint8_t host_kind; /* dongle_host_kind_t, DONGLE_HOST_UNKNOWN until detected */
    uint8_t host_mode; /* dongle_mode_t */
    uint8_t done;      /* detection finished: beacons start */
} dongle_host_detect_s;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(detect, dongle_host_detect_s);
dongle_snapshot_detect_t _detect_host_ss;

DONGLE_CROSSCORE_FIFO_TYPE(packet, dongle_pkt_s, DONGLE_HOST_MAILBOX_LEN);
dongle_fifo_packet_t _reliable_rx_host_ss;
dongle_fifo_packet_t _control_rx_host_ss;
dongle_fifo_packet_t _reliable_tx_host_ss;
dongle_fifo_packet_t _config_rx_host_ss;

/* Numbered so the transport task sends each report to the app once */
typedef struct
{
    uint32_t     seq;
    dongle_pkt_s pkt;
} dongle_host_bulk_s;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(bulk, dongle_host_bulk_s);
dongle_snapshot_bulk_t _bulk_rx_host_ss;

/* WLAN-task-local state machine (owned/written only by the wlan task). */
typedef struct
{
    uint64_t timeout_us;     /* link watchdog deadline (absolute us) */
    uint64_t last_pump_us;   /* time of our last pump */
    uint16_t inflight_token; /* non-zero while a reliable OUT awaits its echo */
    dongle_pkt_s inflight_pkt;
    uint16_t gamepad_token;  /* last reliable token from the gamepad, echoed in every packet */
    uint32_t bulk_seq;
    bool     link_up;        /* cached link state for edge detection */
    bool     gamepad_set;    /* a gamepad is adopted, everything else is ignored */
    uint8_t  gamepad_ip[4];  /* its DHCP address */
} dongle_host_sm;

static dongle_host_sm _hsm;

/* TRANSPORT-task-local state (owned/written only by the transport task). */
typedef struct
{
    dongle_link_status_t prev_link; /* last observed link snapshot (edge detect) */
    dongle_wake_s        wake;      /* the adopted gamepad's identity, while the link is up */
} dongle_host_transport_sm;

static dongle_host_transport_sm _thsm = { .prev_link = DONGLE_LINK_DOWN };

/* -------------------------------------------------------------------------- */
/* HOST APP WLAN                                                              */
/* -------------------------------------------------------------------------- */

static bool _host_wlan_is_link_up(void)
{
    return _hsm.link_up;
}

static volatile bool _timeout_reset = false;
static void _host_wlan_schedule_timeout_reset(void)
{
    _timeout_reset = true;
}

static void _host_wlan_link_timeout_reset(uint64_t now_us)
{
    _hsm.timeout_us = now_us + (1000 * DONGLE_HOST_TIMEOUT_MS);
}

/* Clear the reliable OUT lane: drop the inflight packet and flush the TX queue.
 * Those packets were stamped for the old session, so they must not survive a
 * link drop into a fresh session. The wlan task is the sole consumer of the TX
 * FIFO, so it is the only context allowed to pop/flush it. */
static void _host_wlan_flush_reliable_lane(void)
{
    _hsm.inflight_token = 0;
    _hsm.gamepad_token = 0;
    memset(&_hsm.inflight_pkt, 0, sizeof(_hsm.inflight_pkt));

    dongle_pkt_s scratch;
    while (dongle_fifo_packet_pop(&_reliable_tx_host_ss, &scratch)) { }
}

/* Publish a link-state change to the transport task and, on a DOWN edge, scrub
 * the WLAN-owned lanes so nothing stale carries into the next session. */
static void _host_wlan_set_link(bool up)
{
    if (_hsm.link_up == up)
    {
        return;
    }

    _hsm.link_up = up;

    uint8_t status = up ? (uint8_t)DONGLE_LINK_UP : (uint8_t)DONGLE_LINK_DOWN;
    dongle_snapshot_link_write(&_link_host_ss, &status);

    if (!up)
    {
        DONGLE_LOGF("[DHOST] Link DOWN\n");
        _host_wlan_flush_reliable_lane();

        /* Free again: the next gamepad to answer a beacon is adopted */
        _hsm.gamepad_set = false;

        /* Invalidate the latest unreliable input so a new session does not read
         * a report left over from the previous one (wlan owns this snapshot). */
        dongle_pkt_s empty = {0};
        dongle_snapshot_packet_write(&_unreliable_rx_host_ss, &empty);
    }
    else
    {
        DONGLE_LOGF("[DHOST] Link UP\n");
    }
}

static void _host_wlan_reset(void)
{
    memset(&_hsm, 0, sizeof(_hsm));

    _host_wlan_flush_reliable_lane();

    uint8_t down = (uint8_t)DONGLE_LINK_DOWN;
    dongle_snapshot_link_write(&_link_host_ss, &down);
}

/* Transmit one datagram to the adopted gamepad (the only unicast TX path). */
static void _host_wlan_send_to_gamepad(const dongle_pkt_s *pkt)
{
    if (!_hsm.gamepad_set)
    {
        return;
    }

    /* Every packet acknowledges the gamepad's last reliable one */
    dongle_pkt_s tx = *pkt;
    tx.ack = _hsm.gamepad_token;

    uint8_t ip[4] = {_hsm.gamepad_ip[0], _hsm.gamepad_ip[1], _hsm.gamepad_ip[2], _hsm.gamepad_ip[3]};
    dongle_api_host_wlan_hook_udp_tx(&tx, ip, DONGLE_GAMEPAD_PORT);
}

/* Load the next queued reliable OUT packet into the inflight slot, but only
 * while the lane is idle so we never clobber a packet still awaiting its ack. */
static void _host_wlan_arm_inflight_reliable(void)
{
    if(_hsm.inflight_token != 0)
    {
        return;
    }

    dongle_pkt_s pkt;
    if(dongle_fifo_packet_pop(&_reliable_tx_host_ss, &pkt))
    {
        _hsm.inflight_token = pkt.token;
        _hsm.inflight_pkt = pkt;
    }
}

void _host_wlan_send_to_transport(const dongle_pkt_s *pkt)
{
    switch (pkt->id)
    {
    case DONGLE_PID_CORE_UNRELIABLE:
        /* Latest-wins: only the newest input report matters. */
        dongle_snapshot_packet_write(&_unreliable_rx_host_ss, pkt);
        break;

    case DONGLE_PID_CORE_RELIABLE:
        /* Ordered, loss-free: command replies / reliable input. */
        dongle_fifo_packet_push(&_reliable_rx_host_ss, pkt);
        break;

    case DONGLE_PID_WAKE:
        /* WAKE reply carries the gamepad identity; hand to the transport task
         * which owns session adoption + transport bring-up. */
        dongle_fifo_packet_push(&_control_rx_host_ss, pkt);
        break;

    case DONGLE_PID_CONFIG_RELIABLE:
        /* A WebUSB reply for the config app, in order */
        dongle_fifo_packet_push(&_config_rx_host_ss, pkt);
        break;

    case DONGLE_PID_BULK_UNRELIABLE:
    {
        /* The gamepad's live input report for the app: only the newest matters */
        dongle_host_bulk_s bulk = {.seq = ++_hsm.bulk_seq, .pkt = *pkt};
        dongle_snapshot_bulk_write(&_bulk_rx_host_ss, &bulk);
        break;
    }

    default:
        /* STATUS is not received in the host role; ignore. */
        break;
    }
}

static bool _host_wlan_send_reliable(void)
{
    /* Pull the next queued packet in if the lane is currently idle. */
    _host_wlan_arm_inflight_reliable();

    /* Nothing awaiting ack: let the caller fall back to a STATUS packet. */
    if(_hsm.inflight_token == 0)
    {
        return false;
    }

    /* Resend the inflight packet; the gamepad must echo its token to
     * retire it (handled in dongle_api_host_wlan_udp_rx). */
    _host_wlan_send_to_gamepad(&_hsm.inflight_pkt);
    return true;
}

static void _host_wlan_send_status(void)
{
    dongle_status_s status;
    dongle_snapshot_status_read(&_status_host_ss, &status);

    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);

    dongle_pkt_s pkt = {0};
    pkt.session = dongle_session_pack(&session);
    pkt.id = DONGLE_PID_STATUS;
    memcpy(pkt.data, &status, sizeof(status));
    pkt.len = sizeof(status);

    _host_wlan_send_to_gamepad(&pkt);
}

/* The beacon tells every gamepad in range that this dongle is free, which protocol it speaks and
 * what it is plugged into. Only a free dongle beacons, so a gamepad that hears none moves on. */
static void _host_wlan_send_wake(void)
{
    dongle_host_detect_s detect;
    dongle_snapshot_detect_read(&_detect_host_ss, &detect);

    /* Gamepads that follow us must hear what we are plugged into, so wait until we know */
    if (!detect.done)
    {
        return;
    }

    dongle_beacon_s beacon = {
        .protocol   = DONGLE_PROTOCOL_VERSION,
        .board      = _host_board,
        .host_kind  = detect.host_kind,
        .host_mode  = (detect.host_kind == DONGLE_HOST_UNKNOWN) ? DONGLE_MODE_UNKNOWN : detect.host_mode,
        .fw_version = _host_fw_version,
    };

    dongle_pkt_s pkt = {0};
    pkt.session = 0;
    pkt.ack = 0;
    pkt.id  = DONGLE_PID_WAKE;
    pkt.len = sizeof(beacon);
    memcpy(pkt.data, &beacon, sizeof(beacon));

    uint8_t ip[4] = {_broadcast_address[0], _broadcast_address[1], _broadcast_address[2], _broadcast_address[3]};
    dongle_api_host_wlan_hook_udp_tx(&pkt, ip, DONGLE_GAMEPAD_PORT);
}

bool _host_wlan_pump_task(uint64_t now_us)
{
    bool pump_it = false;

    uint64_t next_pump;
    dongle_snapshot_pump_read(&_pump_host_ss, &next_pump);

    // Time since our last pump
    uint64_t delta = now_us - _hsm.last_pump_us;

    // A scheduled deadline newer than our last pump has arrived. Comparing the
    // deadline against _last_pump_us dedupes the schedule, so we never have to
    // write _cc_next_pump from core 1 (core 0 stays its sole writer).
    if (next_pump > _hsm.last_pump_us && now_us >= next_pump)
    {
        pump_it = true;
    }
    // Heartbeat fallback if we've gone too long without a pump.
    else if (delta >= DONGLE_HOST_PUMP_MAX_WAIT_US)
    {
        pump_it = true;
    }

    if (pump_it)
    {
        _hsm.last_pump_us = now_us;
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* HOST APP TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

void _host_transport_send_to_wlan(const dongle_pkt_s *pkt)
{
    /* Transport task is the sole producer of the reliable OUT lane. The wlan
     * task pops these, assigns them to its inflight slot, and resends until the
     * gamepad echoes the ack. */
    dongle_fifo_packet_push(&_reliable_tx_host_ss, pkt);
}

/* Adopt a new session from a WAKE reply and (re)bring up the console transport
 * to match the gamepad's advertised personality (mode/vid/pid). A WAKE whose
 * session equals the one we already adopted is a duplicate/heartbeat and is
 * ignored so we never tear the transport down mid-session. */
static void _host_transport_process_wake(const dongle_pkt_s *pkt)
{
    if(pkt->len != sizeof(dongle_wake_s))
    {
        return;
    }

    dongle_wake_s wake;
    memcpy(&wake, pkt->data, sizeof(wake));

    if(wake.protocol != DONGLE_PROTOCOL_VERSION)
    {
        return;
    }

    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);
    uint16_t current = dongle_session_pack(&session);

    if(wake.session != current)
    {
        DONGLE_LOGF("[DHOST] New session 0x%04X (was 0x%04X), bringing up transport (name \"%s\", mfg \"%s\")\n",
                    wake.session, current,
                    (const char *)wake.name, (const char *)wake.manufacturer);

        /* Platform owns the (re)enumeration; it may teardown internally first. */
        _thsm.wake = wake;
        dongle_api_host_transport_hook_transport_bringup(&wake);

        dongle_session_unpack(wake.session, &session);
        dongle_snapshot_session_write(&_session_host_ss, &session);
    }
}

/* -------------------------------------------------------------------------- */
/* HOST API WLAN                                                              */
/* -------------------------------------------------------------------------- */

void dongle_api_host_wlan_init(const dongle_cfg_host_s *cfg)
{
    _host_wlan_reset();

    uint8_t ip[4]   = {_host_address[0], _host_address[1], _host_address[2], _host_address[3]};
    uint8_t mask[4] = {_host_mask[0], _host_mask[1], _host_mask[2], _host_mask[3]};

    char ssid[32];
    char password[16];

    if (cfg != NULL)
    {
        _host_board = cfg->board;
        _host_fw_version = cfg->fw_version;

        dongle_wlan_format_ssid(cfg->pin, ssid, sizeof(ssid));
        dongle_wlan_format_password(cfg->pin, password, sizeof(password));
    }
    else
    {
        strncpy(ssid, DONGLE_DEFAULT_WLAN_SSID, sizeof(ssid));
        ssid[sizeof(ssid) - 1u] = '\0';
        strncpy(password, DONGLE_DEFAULT_WLAN_PASSWORD, sizeof(password));
        password[sizeof(password) - 1u] = '\0';
    }

    if(!dongle_api_host_wlan_hook_ap_bringup(ssid, password, ip, mask))
    {
        DONGLE_LOGF("[DHOST] AP bring-up reported failure\n");
    }
}

void dongle_api_host_wlan_task(void)
{
    uint64_t now_us = dongle_api_hook_get_time_us_u64();

    /* A matching/WAKE packet arrived since the last tick: refresh the watchdog
     * and raise the link. Read-and-clear (the flag may be set from the RX path
     * between this check and the clear; a missed pulse self-corrects next RX). */
    if(_timeout_reset)
    {
        _timeout_reset = false;
        _host_wlan_link_timeout_reset(now_us);
        _host_wlan_set_link(true);
    }

    /* Watchdog: drop the link if no matching packet has arrived within the
     * timeout. The DOWN edge scrubs the reliable lane (see _host_wlan_set_link). */
    if(_hsm.link_up && now_us >= _hsm.timeout_us)
    {
        _host_wlan_set_link(false);
    }

    if(_host_wlan_pump_task(now_us))
    {
        if(_host_wlan_is_link_up())
        {
            if(!_host_wlan_send_reliable())
            {
                _host_wlan_send_status();
            }
        }
        else
        {
            _host_wlan_send_wake();
        }
    }
}

void dongle_api_host_wlan_udp_rx(const dongle_pkt_s *pkt, const uint8_t ip[4])
{
    if(!pkt || !ip)
    {
        return;
    }

    /* One gamepad at a time. While one is adopted, other gamepads that joined our network hear no
     * beacons and get no answers, so they give up and try another dongle. */
    if(_hsm.gamepad_set)
    {
        if(memcmp(ip, _hsm.gamepad_ip, 4) != 0)
        {
            return;
        }
    }
    else
    {
        /* Adopt the first gamepad that answers a beacon on our protocol */
        if(pkt->id != DONGLE_PID_WAKE || pkt->len != sizeof(dongle_wake_s) ||
           pkt->data[offsetof(dongle_wake_s, protocol)] != DONGLE_PROTOCOL_VERSION)
        {
            return;
        }

        memcpy(_hsm.gamepad_ip, ip, 4);
        _hsm.gamepad_set = true;
        DONGLE_LOGF("[DHOST] Adopted gamepad %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    }

    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);
    uint16_t sessionu16 = dongle_session_pack(&session);

    if((pkt->id == DONGLE_PID_WAKE) || (pkt->session == sessionu16))
    {
        _host_wlan_schedule_timeout_reset();
    }

    if((_hsm.inflight_token != 0) && (pkt->ack == _hsm.inflight_token))
    {
        _hsm.inflight_token = 0;
        _host_wlan_arm_inflight_reliable();
    }

    /* A reliable packet from the gamepad is echoed from now on and passed on only once (the
     * gamepad resends it until it sees the echo) */
    if(pkt->token != 0)
    {
        if(pkt->token == _hsm.gamepad_token)
        {
            return;
        }
        _hsm.gamepad_token = pkt->token;
    }

    _host_wlan_send_to_transport(pkt);
}

/* -------------------------------------------------------------------------- */
/* HOST API LINK (Bluetooth)                                                  */
/* -------------------------------------------------------------------------- */
/*
 * Same lanes as the WLAN task: received traffic goes to the transport task through
 * _host_wlan_send_to_transport(), the link state through _host_wlan_set_link(), and the
 * reliable OUT lane (console output reports, config traffic) is drained here. The link
 * itself delivers in order and resends, so there are no tokens or acks.
 */

static dongle_pkt_s _link_tx_pending; /* popped from the OUT lane, not yet sent */
static bool _link_tx_have;

void dongle_api_host_link_init(const dongle_cfg_host_s *cfg)
{
    _host_wlan_reset();
    _link_tx_have = false;

    if (cfg != NULL)
    {
        _host_board = cfg->board;
        _host_fw_version = cfg->fw_version;
    }
}

void dongle_api_host_link_set_up(bool up)
{
    _hsm.gamepad_set = up;
    _host_wlan_set_link(up);
    if (!up)
    {
        _link_tx_have = false;
    }
}

bool dongle_api_host_link_get_host(dongle_link_host_s *out)
{
    dongle_host_detect_s detect;
    dongle_snapshot_detect_read(&_detect_host_ss, &detect);

    if (!out || !detect.done)
    {
        return false;
    }

    out->beacon.protocol   = DONGLE_PROTOCOL_VERSION;
    out->beacon.board      = _host_board;
    out->beacon.host_kind  = detect.host_kind;
    out->beacon.host_mode  = (detect.host_kind == DONGLE_HOST_UNKNOWN) ? DONGLE_MODE_UNKNOWN : detect.host_mode;
    out->beacon.fw_version = _host_fw_version;
    out->report_interval_us = (detect.host_kind == DONGLE_HOST_PC) ? DONGLE_LINK_INTERVAL_PC_US
                                                                     : DONGLE_LINK_INTERVAL_DEFAULT_US;
    return true;
}

void dongle_api_host_link_rx_report(const uint8_t *payload, uint16_t len)
{
    dongle_pkt_s pkt = {0};
    if (!payload || !dongle_link_untrim_report(payload, len, pkt.data, &pkt.len))
    {
        return;
    }
    pkt.id = DONGLE_PID_CORE_UNRELIABLE;
    _host_wlan_send_to_transport(&pkt);
}

void dongle_api_host_link_rx_bulk(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0 || len > 64)
    {
        return;
    }
    dongle_pkt_s pkt = {0};
    pkt.id = DONGLE_PID_BULK_UNRELIABLE;
    pkt.len = len;
    memcpy(pkt.data, data, len);
    _host_wlan_send_to_transport(&pkt);
}

void dongle_api_host_link_rx_msg(const uint8_t *msg, uint16_t len)
{
    if (!msg || len < 1 || len > DONGLE_LINK_MSG_MAX)
    {
        return;
    }

    dongle_pkt_s pkt = {0};
    switch ((dongle_link_msg_t)msg[0])
    {
    case DONGLE_LINK_MSG_WAKE:
        pkt.id = DONGLE_PID_WAKE;
        break;
    case DONGLE_LINK_MSG_REPORT:
        pkt.id = DONGLE_PID_CORE_RELIABLE;
        break;
    case DONGLE_LINK_MSG_CONFIG:
        pkt.id = DONGLE_PID_CONFIG_RELIABLE;
        break;
    default:
        return; /* HOST and OUTPUT only go the other way */
    }

    pkt.len = (uint16_t)(len - 1u);
    memcpy(pkt.data, &msg[1], pkt.len);
    _host_wlan_send_to_transport(&pkt);
}

bool dongle_api_host_link_peek_msg(uint8_t msg[DONGLE_LINK_MSG_MAX], uint16_t *len)
{
    if (!msg || !len)
    {
        return false;
    }
    if (!_link_tx_have)
    {
        if (!dongle_fifo_packet_pop(&_reliable_tx_host_ss, &_link_tx_pending))
        {
            return false;
        }
        _link_tx_have = true;
    }

    uint16_t n = (_link_tx_pending.len > 64u) ? 64u : _link_tx_pending.len;
    msg[0] = (_link_tx_pending.id == DONGLE_PID_CONFIG_RELIABLE) ? (uint8_t)DONGLE_LINK_MSG_CONFIG
                                                                 : (uint8_t)DONGLE_LINK_MSG_OUTPUT;
    memcpy(&msg[1], _link_tx_pending.data, n);
    *len = (uint16_t)(n + 1u);
    return true;
}

void dongle_api_host_link_msg_sent(void)
{
    _link_tx_have = false;
}

void dongle_api_host_link_get_status(dongle_status_s *out)
{
    if (out)
    {
        dongle_snapshot_status_read(&_status_host_ss, out);
    }
}

/* -------------------------------------------------------------------------- */
/* HOST API TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

void dongle_api_host_transport_task(void)
{
    /* Drain the control mailbox: WAKE replies drive session adoption + bring-up. */
    dongle_pkt_s pkt;
    while(dongle_fifo_packet_pop(&_control_rx_host_ss, &pkt))
    {
        if(pkt.id == DONGLE_PID_WAKE)
        {
            _host_transport_process_wake(&pkt);
        }
    }

    /* React to the wlan link state machine. On the UP->DOWN edge the gamepad is
     * gone: tear the console transport down and flush the reliable input queue
     * so nothing carries into the next session. */
    uint8_t link_u8;
    dongle_snapshot_link_read(&_link_host_ss, &link_u8);
    dongle_link_status_t link = (dongle_link_status_t)link_u8;

    if(_thsm.prev_link == DONGLE_LINK_UP && link == DONGLE_LINK_DOWN)
    {
        DONGLE_LOGF("[DHOST] Transport teardown (link lost)\n");
        dongle_api_host_transport_hook_transport_teardown();

        dongle_pkt_s scratch;
        while(dongle_fifo_packet_pop(&_reliable_rx_host_ss, &scratch)) { }
        while(dongle_fifo_packet_pop(&_config_rx_host_ss, &scratch)) { }
    }

    _thsm.prev_link = link;
}

void dongle_api_host_transport_set_outputreport(const uint8_t data[64], uint16_t len)
{
    if(!data || len==0) return;
    if(len>64) len = 64;

    dongle_pkt_s pkt;
    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);

    pkt.ack = 0;
    pkt.token = dongle_api_generate_ack();
    memcpy(pkt.data, data, len);
    pkt.len = len;
    pkt.id = DONGLE_PID_CORE_RELIABLE;
    pkt.session = dongle_session_pack(&session);
    _host_transport_send_to_wlan(&pkt);
}

bool dongle_api_host_transport_send_config(const uint8_t *data, uint16_t len)
{
    if(!data || len == 0 || len > 64)
    {
        return false;
    }

    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);

    dongle_pkt_s pkt = {0};
    pkt.session = dongle_session_pack(&session);
    pkt.token = dongle_api_generate_ack();
    pkt.id = DONGLE_PID_CONFIG_RELIABLE;
    pkt.len = len;
    memcpy(pkt.data, data, len);
    return dongle_fifo_packet_push(&_reliable_tx_host_ss, &pkt);
}

bool dongle_api_host_transport_get_config(uint8_t data[64], uint16_t *len)
{
    static uint32_t sent_seq = 0;
    dongle_pkt_s pkt;

    /* Replies first, in order; then the newest live input report not yet sent */
    if(!dongle_fifo_packet_pop(&_config_rx_host_ss, &pkt))
    {
        dongle_host_bulk_s bulk;
        if(!dongle_snapshot_bulk_read(&_bulk_rx_host_ss, &bulk) || bulk.seq == sent_seq)
        {
            return false;
        }
        sent_seq = bulk.seq;
        pkt = bulk.pkt;
    }

    uint16_t n = (pkt.len > sizeof(pkt.data)) ? (uint16_t)sizeof(pkt.data) : pkt.len;
    memcpy(data, pkt.data, n);
    *len = n;
    return n > 0;
}

/* Shared input selection: drain reliable input first (ordered, loss-free),
 * otherwise fall back to the latest unreliable snapshot. Returns true and fills
 * @p pkt only when a packet with a non-empty payload is available. */
static bool _host_transport_pop_input(dongle_pkt_s *pkt)
{
    if(!dongle_fifo_packet_pop(&_reliable_rx_host_ss, pkt))
    {
        dongle_snapshot_packet_read(&_unreliable_rx_host_ss, pkt);
    }

    return pkt->len > 0;
}

bool dongle_api_host_transport_get_inputreport(uint8_t data[64], uint16_t *len)
{
    dongle_pkt_s pkt;

    if(_host_transport_pop_input(&pkt))
    {
        uint16_t n = (pkt.len > sizeof(pkt.data)) ? (uint16_t)sizeof(pkt.data) : pkt.len;
        memcpy(data, pkt.data, n);
        *len = n;
        return true;
    }

    *len = 0;
    return false;
}

bool dongle_api_host_transport_get_inputpacket(dongle_pkt_s *pkt)
{
    if(!pkt)
    {
        return false;
    }

    return _host_transport_pop_input(pkt);
}

void dongle_api_host_transport_set_rumble(uint8_t left, uint8_t right, uint8_t brake_left, uint8_t brake_right)
{
    dongle_status_s status;
    dongle_snapshot_status_read(&_status_host_ss, &status);
    status.rumble.left = left;
    status.rumble.right = right;
    status.brake.left = brake_left;
    status.brake.right = brake_right;
    dongle_snapshot_status_write(&_status_host_ss, &status);
}

void dongle_api_host_transport_set_transport(bool connected)
{
    dongle_status_s status;
    dongle_snapshot_status_read(&_status_host_ss, &status);
    status.transport_status = connected ? DONGLE_TRANSPORT_CONNECTED : DONGLE_TRANSPORT_IDLE;
    dongle_snapshot_status_write(&_status_host_ss, &status);
}

void dongle_api_host_transport_set_player(uint8_t player_number)
{
    dongle_status_s status;
    dongle_snapshot_status_read(&_status_host_ss, &status);
    status.player_number = player_number;
    dongle_snapshot_status_write(&_status_host_ss, &status);
}

void dongle_api_host_transport_set_detected(dongle_host_kind_t host_kind, dongle_mode_t host_mode)
{
    dongle_host_detect_s detect = {
        .host_kind = (uint8_t)host_kind,
        .host_mode = (uint8_t)host_mode,
        .done = 1,
    };
    dongle_snapshot_detect_write(&_detect_host_ss, &detect);
}

void dongle_api_host_transport_get_status(dongle_status_s *out)
{
    if(out)
    {
        dongle_snapshot_status_read(&_status_host_ss, out);
    }
}

bool dongle_api_host_transport_get_gamepad(dongle_wake_s *out)
{
    if(!out || dongle_api_host_transport_get_link_status() != DONGLE_LINK_UP)
    {
        return false;
    }

    *out = _thsm.wake;
    return true;
}

dongle_link_status_t dongle_api_host_transport_get_link_status(void)
{
    uint8_t link;
    dongle_snapshot_link_read(&_link_host_ss, &link);
    return (dongle_link_status_t)link;
}

void dongle_api_host_transport_mark_sent(void)
{
    static uint64_t last_time = 0;
    uint64_t this_time = dongle_api_hook_get_time_us_u64();

    uint64_t delta = this_time - last_time;
    uint64_t next_time = 0;

    // Skip if our delta is too small (time between polls)
    if (delta <= DONGLE_HOST_PUMP_MIN_ALLOWED_US)
    {
        return;
    }
    else
    {
        last_time = this_time;
        next_time = this_time + (delta >> 1);
        dongle_snapshot_pump_write(&_pump_host_ss, &next_time);
   }
}

/* -------------------------------------------------------------------------- */
/* HOST API WLAN HOOKS                                                        */
/* -------------------------------------------------------------------------- */

__attribute__((weak)) void dongle_api_host_wlan_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port)
{
    (void)pkt;
    (void)ip;
    (void)port;
}

__attribute__((weak)) bool dongle_api_host_wlan_hook_ap_bringup(const char *ssid, const char *password, uint8_t ip[4], uint8_t mask[4])
{
    (void)ssid;
    (void)password;
    (void)ip;
    (void)mask;
    return false;
}

__attribute__((weak)) void dongle_api_host_wlan_hook_reset_network(void)
{
    /* Default: nothing. A platform may reset its radio / socket stack here. */
}

/* -------------------------------------------------------------------------- */
/* HOST API TRANSPORT HOOKS                                                   */
/* -------------------------------------------------------------------------- */

__attribute__((weak)) void dongle_api_host_transport_hook_transport_bringup(const dongle_wake_s *wake)
{
    (void)wake;
}

__attribute__((weak)) void dongle_api_host_transport_hook_transport_teardown(void)
{

}