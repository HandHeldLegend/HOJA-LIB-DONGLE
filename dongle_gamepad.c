#include <dongle.h>
#include <dongle_gamepad.h>
#include <dongle_log.h>

/*
 * Compile-time guard from the guide's bring-up checklist (§14): with the
 * mandatory #pragma pack(push,1) the wire struct is exactly 73 bytes
 * (2 + 2 + 2 + 1 + 2 + 64). If padding sneaks in, the dongle's length filter
 * silently drops every datagram we send.
 */
_Static_assert(sizeof(dongle_pkt_s) == 73, "dongle_pkt_s must be packed to 73 bytes");
_Static_assert(sizeof(dongle_wake_s) == DONGLE_WAKE_S_LEN, "dongle_wake_s must fill the 64-byte data field");

static uint8_t _dongle_host_address[4] = {DONGLE_HOST_IP0, DONGLE_HOST_IP1, DONGLE_HOST_IP2, DONGLE_HOST_IP3};

#define DONGLE_GAMEPAD_REPORT_LOG_INTERVAL_MS 4000
#define DONGLE_GAMEPAD_LINK_STATUS_INTERVAL_MS 100
// Joining a dongle and getting an address
#define DONGLE_GAMEPAD_CONNECTION_FAIL_TIMEOUT_MS 10000
// A free dongle beacons at least once a second, and sends STATUS soon after adopting us. Silence
// that long means it is busy with another gamepad (or not one of ours).
#define DONGLE_GAMEPAD_DONGLE_WAIT_MS 2500
// Nothing to join: look again after this long
#define DONGLE_GAMEPAD_RESCAN_MS 1000

#define DONGLE_GAMEPAD_CANDIDATES_MAX 8

// WebUSB replies waiting to go to the config app (a 2 KB config block is 35 of them)
#define DONGLE_GAMEPAD_CONFIG_QUEUE_LEN 48

// Bluetooth link: input reports that must arrive (command replies), waiting for the reliable channel
#define DONGLE_GAMEPAD_LINK_REPORTS 8

typedef struct
{
    uint16_t len;
    uint8_t  data[64];
} dongle_gamepad_config_pkt_s;

typedef enum
{
    DONGLE_GAMEPAD_CONNPHASE_UNDEFINED = 0,
    DONGLE_GAMEPAD_CONNPHASE_DOWN = 1,       /* Waiting to scan */
    DONGLE_GAMEPAD_CONNPHASE_SCANNING = 2,   /* Looking for dongles in range */
    DONGLE_GAMEPAD_CONNPHASE_CONNECTING = 3, /* Joining one dongle's network */
    DONGLE_GAMEPAD_CONNPHASE_CONNECTED = 4,  /* On its network, waiting for the dongle to adopt us */
    DONGLE_GAMEPAD_CONNPHASE_UP = 5,         /* Adopted: the dongle sends us STATUS */
} dongle_gamepad_connphase_t;

/* A dongle found by the scan */
typedef struct
{
    uint16_t key;  /* Its PIN */
    int16_t  rssi; /* Strongest signal seen for it */
    bool     tried;
} dongle_gamepad_candidate_s;

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

    uint16_t last_core_token;   /* last CORE_RELIABLE from the dongle, to drop its resends */
    bool have_core_token;
    uint16_t last_config_token; /* same for CONFIG_RELIABLE */
    bool have_config_token;

    /* WebUSB traffic for the config app: replies go reliably, one at a time until the dongle
     * echoes their token; the live input report only ever sends its newest copy */
    dongle_gamepad_config_pkt_s config_queue[DONGLE_GAMEPAD_CONFIG_QUEUE_LEN];
    uint8_t  config_head;
    uint8_t  config_tail;
    uint16_t config_token; /* inflight reply's token, 0 when none */
    dongle_gamepad_config_pkt_s config_inflight;
    dongle_gamepad_config_pkt_s bulk;
    bool     bulk_pending;
    bool     last_reply_config; /* replies alternate with input reports while both have data */

    bool wake_replied;

    dongle_link_status_t wlan_link;
    dongle_gamepad_connphase_t wlan_connphase;
    uint64_t wlan_deadline_us; /* Ends the current scan wait, join or wait for the dongle */

    /* Dongles from the last scan, and the one being joined */
    dongle_gamepad_candidate_s candidates[DONGLE_GAMEPAD_CANDIDATES_MAX];
    uint8_t candidate_count;
    int8_t  candidate; /* Index being joined, or -1 */

    /* The last beacon of the dongle we joined */
    dongle_beacon_s dongle;
    bool dongle_seen;
    bool dongle_mismatch; /* It speaks another protocol version */

    /* USB identity / personality advertised to the dongle in WAKE replies. */
    dongle_mode_t mode;
    uint16_t pid;
    uint16_t vid;
    uint8_t  name[DONGLE_WAKE_NAME_LEN];
    uint8_t  manufacturer[DONGLE_WAKE_MANUFACTURER_LEN];
    bool     mode_forced;
    bool     mode_pending; /* Asked the platform to change to the dongle's mode */
    uint32_t fw_version;

    /* Which STATUS-derived callback events the app wants delivered. Events with
     * their flag cleared are parsed but never dispatched to the hooks. */
    dongle_status_evt_subscription_s evt;

    uint16_t paired_key; /* DONGLE_PIN_NONE when no dongle is paired */
    bool pairing;

    bool rx_got;

    /* Bluetooth link (dongle_link.h): no scanning, joining or tokens, the link does that */
    bool     link_up;          /* reliable channel open */
    bool     link_wake_due;    /* our identity goes out next */
    uint16_t link_interval_us; /* pacing the dongle asked for */
    dongle_gamepad_config_pkt_s link_reports[DONGLE_GAMEPAD_LINK_REPORTS]; /* input reports that must arrive */
    uint8_t  link_report_head;
    uint8_t  link_report_tail;
    uint8_t  link_peeked;      /* dongle_link_msg_t of the message handed out by peek, 0 when none */

    dongle_gamepad_logging_sm logging;
} dongle_gamepad_sm;

static dongle_gamepad_sm _gp = {0};

/* -------------------------------------------------------------------------- */
/* GAMEPAD HOOKS (weak defaults)                                              */
/* -------------------------------------------------------------------------- */

// Implement WiFI Link Status Checking (up once associated and given an address)
__attribute__((weak)) dongle_link_status_t dongle_api_gamepad_hook_link_up(void)
{
    return DONGLE_LINK_DOWN;
}

// Implement starting a scan for networks (results go to dongle_api_gamepad_scan_result)
__attribute__((weak)) bool dongle_api_gamepad_hook_scan_start(void)
{
    return false;
}

// Implement reporting whether the scan is still running
__attribute__((weak)) bool dongle_api_gamepad_hook_scan_active(void)
{
    return false;
}

// Implement SSID connection
__attribute__((weak)) void dongle_api_gamepad_hook_connect_async(const char *ssid, const char *pw)
{
    (void)ssid;
    (void)pw;
}

// Implement leaving the current network
__attribute__((weak)) void dongle_api_gamepad_hook_disconnect(void)
{
}

// Implement saving a newly paired dongle
__attribute__((weak)) void dongle_api_gamepad_hook_paired(const uint8_t pin[4])
{
    (void)pin;
}

// Implement switching to the dongle's mode (true if it will, then call set_identity)
__attribute__((weak)) bool dongle_api_gamepad_hook_mode_request(dongle_mode_t mode)
{
    (void)mode;
    return false;
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

// Implement receiving a WebUSB packet from the config app
__attribute__((weak)) void dongle_api_gamepad_hook_config_rx(const uint8_t data[64], uint16_t len)
{
    (void)data;
    (void)len;
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
    _gp.wake.protocol = DONGLE_PROTOCOL_VERSION;
    _gp.wake.flags = _gp.mode_forced ? DONGLE_WAKE_FLAG_MODE_FORCED : 0;
    _gp.wake.fw_version = _gp.fw_version;
    memcpy(_gp.wake.name, _gp.name, sizeof(_gp.wake.name));
    memcpy(_gp.wake.manufacturer, _gp.manufacturer, sizeof(_gp.wake.manufacturer));

    DONGLE_LOGF("[DGP] New session id 0x%03X (mode %u, vid 0x%04X, pid 0x%04X, name \"%s\", mfg \"%s\")\n",
                _gp.session.id, (unsigned)_gp.session.mode, _gp.vid, _gp.pid,
                (const char *)_gp.wake.name, (const char *)_gp.wake.manufacturer);
}

/* Forget the dongle we were talking to (session state only, not the scan) */
static void _gamepad_clear_session(void)
{
    _gp.status = (dongle_status_s){0};

    _gp.last_core_token = 0;
    _gp.have_core_token = false;
    _gp.last_config_token = 0;
    _gp.have_config_token = false;

    _gp.config_head = 0;
    _gp.config_tail = 0;
    _gp.config_token = 0;
    _gp.bulk_pending = false;
    _gp.last_reply_config = false;

    _gp.wake_replied = false;

    _gp.dongle_seen = false;
    _gp.dongle_mismatch = false;
    _gp.mode_pending = false;

    _gp.rx_got = false;

    /* Fresh session id for the next connection (guide §4: never reuse). */
    _gamepad_refresh_wake();
}

static void _gamepad_reset(uint64_t now_us)
{
    _gamepad_clear_session();

    _gp.wlan_link = DONGLE_LINK_UNDEFINED;
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;
    _gp.wlan_deadline_us = now_us;
    _gp.candidate = -1;

    DONGLE_LOGF("[DGP] Reset link/session state\n");

    // Notify upper layers to reset their network stack/state
    dongle_api_gamepad_hook_reset_network();
}

/* Parse "HOJA_WLAN_1234" into its PIN; false for any other network */
static bool _gamepad_ssid_key(const char *ssid, uint8_t len, uint16_t *key)
{
    const uint8_t prefix_len = (uint8_t)(sizeof(DONGLE_WLAN_SSID_PREFIX) - 1u);

    if (!ssid || len != prefix_len + 4u || memcmp(ssid, DONGLE_WLAN_SSID_PREFIX, prefix_len) != 0)
    {
        return false;
    }

    uint16_t k = 0;
    for (uint8_t i = prefix_len; i < len; i++)
    {
        if (ssid[i] < '0' || ssid[i] > '9')
        {
            return false;
        }
        k = (uint16_t)(k * 10u + (uint16_t)(ssid[i] - '0'));
    }

    *key = k;
    return true;
}

/* The paired dongle first (unless pairing), then the strongest one not yet tried. -1 when none. */
static int8_t _gamepad_next_candidate(void)
{
    int8_t best = -1;

    for (uint8_t i = 0; i < _gp.candidate_count; i++)
    {
        const dongle_gamepad_candidate_s *c = &_gp.candidates[i];
        if (c->tried)
        {
            continue;
        }

        if (!_gp.pairing && _gp.paired_key != DONGLE_PIN_NONE && c->key == _gp.paired_key)
        {
            return (int8_t)i;
        }

        if (best < 0 || c->rssi > _gp.candidates[best].rssi)
        {
            best = (int8_t)i;
        }
    }

    return best;
}

static void _gamepad_scan_start(uint64_t now_us)
{
    _gp.candidate_count = 0;
    _gp.candidate = -1;

    if (dongle_api_gamepad_hook_scan_start())
    {
        DONGLE_LOGF("[DGP] Scanning for dongles\n");
        _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_SCANNING;
    }
    else
    {
        _gp.wlan_deadline_us = now_us + (DONGLE_GAMEPAD_RESCAN_MS * 1000);
    }
}

/* Join the next dongle in line, or scan again once all were tried */
static void _gamepad_join_next(uint64_t now_us)
{
    _gp.candidate = _gamepad_next_candidate();

    if (_gp.candidate < 0)
    {
        DONGLE_LOGF("[DGP] No free dongle found\n");
        _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;
        _gp.wlan_deadline_us = now_us + (DONGLE_GAMEPAD_RESCAN_MS * 1000);
        return;
    }

    dongle_gamepad_candidate_s *c = &_gp.candidates[_gp.candidate];
    c->tried = true;

    uint8_t pin[4];
    dongle_wlan_pin_from_u16(c->key, pin);

    static char ssid[32];
    static char password[16];
    dongle_wlan_format_ssid(pin, ssid, sizeof(ssid));
    dongle_wlan_format_password(pin, password, sizeof(password));

    DONGLE_LOGF("[DGP] Joining \"%s\" (rssi %d)\n", ssid, c->rssi);
    _gamepad_clear_session();
    dongle_api_gamepad_hook_connect_async(ssid, password);

    _gp.wlan_link = DONGLE_LINK_DOWN;
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_CONNECTING;
    _gp.wlan_deadline_us = now_us + (DONGLE_GAMEPAD_CONNECTION_FAIL_TIMEOUT_MS * 1000);
}

/* This dongle didn't work out (gone, busy or another version): leave it and try the next */
static void _gamepad_join_failed(uint64_t now_us, const char *why)
{
    DONGLE_LOGF("[DGP] Dongle %s, trying the next one\n", why);
    dongle_api_gamepad_hook_disconnect();
    _gamepad_join_next(now_us);
}

static void _gamepad_poll(uint64_t now_us)
{
    static uint64_t last_check = 0;
    uint64_t delta = now_us - last_check;

    if (delta < (DONGLE_GAMEPAD_LINK_STATUS_INTERVAL_MS * 1000))
    {
        return;
    }
    last_check = now_us;

    dongle_link_status_t link = dongle_api_gamepad_hook_link_up();
    bool expired = now_us >= _gp.wlan_deadline_us;

    switch (_gp.wlan_connphase)
    {
    default:
    case DONGLE_GAMEPAD_CONNPHASE_DOWN:
        if (expired)
        {
            _gamepad_scan_start(now_us);
        }
        break;

    case DONGLE_GAMEPAD_CONNPHASE_SCANNING:
        if (!dongle_api_gamepad_hook_scan_active())
        {
            DONGLE_LOGF("[DGP] Scan found %u dongle(s)\n", _gp.candidate_count);
            _gamepad_join_next(now_us);
        }
        break;

    case DONGLE_GAMEPAD_CONNPHASE_CONNECTING:
        if (link == DONGLE_LINK_UP)
        {
            DONGLE_LOGF("[DGP] Joined, waiting for the dongle\n");
            _gp.wlan_link = DONGLE_LINK_UP;
            _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_CONNECTED;
            _gp.wlan_deadline_us = now_us + (DONGLE_GAMEPAD_DONGLE_WAIT_MS * 1000);
        }
        else if (expired)
        {
            _gamepad_join_failed(now_us, "did not let us join");
        }
        break;

    case DONGLE_GAMEPAD_CONNPHASE_CONNECTED:
        if (link != DONGLE_LINK_UP)
        {
            _gamepad_join_failed(now_us, "dropped us");
        }
        else if (_gp.dongle_mismatch)
        {
            _gamepad_join_failed(now_us, "speaks another protocol version");
        }
        else if (expired)
        {
            _gamepad_join_failed(now_us, _gp.dongle_seen ? "did not adopt us" : "is busy");
        }
        break;

    case DONGLE_GAMEPAD_CONNPHASE_UP:
        if (link != DONGLE_LINK_UP)
        {
            DONGLE_LOGF("[DGP] Link DOWN (was UP), resetting\n");
            _gamepad_reset(now_us);
        }
        break;
    }
}

static void _gamepad_apply_status(const dongle_status_s *status)
{
    /* Each event is dispatched only if the app subscribed to it in cfg->evt.
     * Unsubscribed events are still tracked (via _gp.status below) so change
     * detection stays correct if the app later re-subscribes, but their hooks
     * are never invoked. */
    if(_gp.evt.transport_status && _gp.status.transport_status != status->transport_status)
    {
        DONGLE_LOGF("[DGP] Console transport %s\n",
                    (status->transport_status == DONGLE_TRANSPORT_CONNECTED) ? "CONNECTED" : "IDLE");
        dongle_api_gamepad_hook_set_transport(status->transport_status == DONGLE_TRANSPORT_CONNECTED);
    }

    if(_gp.evt.player_number && _gp.status.player_number != status->player_number)
    {
        DONGLE_LOGF("[DGP] Player number %u\n", status->player_number);
        dongle_api_gamepad_hook_set_player(status->player_number);
    }

    if(_gp.evt.rumble && _gp.status.rumble_value != status->rumble_value)
    {
        dongle_api_gamepad_hook_set_rumble(
            status->rumble.left, status->rumble.right,
            status->brake.left, status->brake.right
        );
    }

    _gp.status = *status;
}

/* The first STATUS means the dongle adopted us */
static void _gamepad_adopted(void)
{
    if (_gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_CONNECTED || _gp.candidate < 0)
    {
        return;
    }

    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_UP;

    uint16_t key = _gp.candidates[_gp.candidate].key;
    DONGLE_LOGF("[DGP] Adopted by dongle %04u\n", key);

    if (_gp.pairing)
    {
        _gp.pairing = false;
        _gp.paired_key = key;

        uint8_t pin[4];
        dongle_wlan_pin_from_u16(key, pin);
        dongle_api_gamepad_hook_paired(pin);
    }
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

/* Config traffic for the app, when there is some: the inflight reply (sent again until echoed),
 * else the latest live input report. False when there is nothing to send. */
static bool _gamepad_build_config_reply(dongle_pkt_s *tx)
{
    if (_gp.config_token == 0 && _gp.config_tail != _gp.config_head)
    {
        _gp.config_inflight = _gp.config_queue[_gp.config_tail];
        _gp.config_tail = (uint8_t)((_gp.config_tail + 1u) % DONGLE_GAMEPAD_CONFIG_QUEUE_LEN);
        _gp.config_token = dongle_api_generate_ack();
    }

    if (_gp.config_token != 0)
    {
        tx->id = DONGLE_PID_CONFIG_RELIABLE;
        tx->token = _gp.config_token;
        tx->len = _gp.config_inflight.len;
        memcpy(tx->data, _gp.config_inflight.data, _gp.config_inflight.len);
        return true;
    }

    if (_gp.bulk_pending)
    {
        _gp.bulk_pending = false;
        tx->id = DONGLE_PID_BULK_UNRELIABLE;
        tx->len = _gp.bulk.len;
        memcpy(tx->data, _gp.bulk.data, _gp.bulk.len);
        return true;
    }

    return false;
}

/* Every reply carries config traffic or an input report. While the app is connected they take
 * turns, so neither the console nor the app is starved. */
static void _gamepad_build_reply(dongle_pkt_s *tx)
{
    if (!_gp.last_reply_config && _gamepad_build_config_reply(tx))
    {
        _gp.last_reply_config = true;
        return;
    }

    _gp.last_reply_config = false;
    _gamepad_build_input_reply(tx);
}

static void _gamepad_build_wake_reply(dongle_pkt_s *tx)
{
    // Session already exists
    tx->id      = DONGLE_PID_WAKE;
    tx->len     = (uint16_t)sizeof(dongle_wake_s);
    memcpy(tx->data, &_gp.wake, sizeof(dongle_wake_s));

    DONGLE_LOGF("[DGP] Replying to WAKE beacon (session 0x%03X)\n", _gp.session.id);
}

/* A beacon: remember what the dongle told us, and answer it if we can talk to it */
static bool _gamepad_process_beacon(const dongle_pkt_s *rx)
{
    if (rx->len != sizeof(dongle_beacon_s) || _gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_CONNECTED)
    {
        return false;
    }

    memcpy(&_gp.dongle, rx->data, sizeof(dongle_beacon_s));
    _gp.dongle_seen = true;

    if (_gp.dongle.protocol != DONGLE_PROTOCOL_VERSION)
    {
        _gp.dongle_mismatch = true;
        return false;
    }

    // Unless a mode was chosen on the gamepad, take the dongle's. The platform changes mode and
    // gives us the new identity; a beacon after that gets our reply.
    if (_gp.mode_pending)
    {
        return false;
    }

    const dongle_mode_t host_mode = (dongle_mode_t)_gp.dongle.host_mode;
    if (!_gp.mode_forced && _gp.dongle.host_kind != DONGLE_HOST_UNKNOWN && host_mode != DONGLE_MODE_UNKNOWN &&
        host_mode != _gp.mode && dongle_api_gamepad_hook_mode_request(host_mode))
    {
        DONGLE_LOGF("[DGP] Dongle wants mode %u, switching from %u\n", (unsigned)host_mode, (unsigned)_gp.mode);
        _gp.mode_pending = true;
        _gp.wlan_deadline_us = dongle_api_hook_get_time_us_u64() + (DONGLE_GAMEPAD_DONGLE_WAIT_MS * 1000);
        return false;
    }

    return true;
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
    tx.ack     = rx->token;

    /* The dongle has our config reply: the next one can go */
    if (_gp.config_token != 0 && rx->ack == _gp.config_token)
    {
        _gp.config_token = 0;
    }

    bool send_reply = true;

    switch ((dongle_pid_t)rx->id)
    {
    case DONGLE_PID_WAKE:
        /* Only the first beacon in a run of repeats gets a WAKE reply: the dongle floods beacons
         * until we are acknowledged, but replying to each restarts our input/session and spams
         * the host. Beacons are broadcast, so they also reach us while another dongle's network
         * is being joined; those are ignored until we are on one. */
        if (_gamepad_process_beacon(rx) && !_gp.wake_replied)
        {
            _gamepad_build_wake_reply(&tx);
            _gp.wake_replied = true;
            // Give the dongle time to adopt us from here
            _gp.wlan_deadline_us = dongle_api_hook_get_time_us_u64() + (DONGLE_GAMEPAD_DONGLE_WAIT_MS * 1000);
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
        _gamepad_adopted();

        /* A STATUS payload is exactly sizeof(dongle_status_s) bytes. */
        if (rx->len == sizeof(dongle_status_s))
        {
            _gamepad_apply_status((const dongle_status_s *)rx->data);
        }
        _gamepad_build_reply(&tx);
        break;

    case DONGLE_PID_CORE_RELIABLE:
        /* Non-WAKE traffic: re-arm WAKE handling (see STATUS case above). */
        _gp.wake_replied = false;
        _gamepad_adopted();

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
            if (_gp.have_core_token && rx->token == _gp.last_core_token)
            {
                /* Duplicate resend; silently re-ack below without re-tunneling. */
                DONGLE_LOGF("[DGP] Duplicate reliable OUT (token 0x%04X), re-acking\n", rx->token);
            }
            else
            {
                dongle_api_gamepad_hook_set_outputreport(rx->data, rx->len);
                _gp.last_core_token = rx->token;
                _gp.have_core_token = true;
            }
        }
        _gamepad_build_reply(&tx);
        break;

    case DONGLE_PID_CONFIG_RELIABLE:
        _gamepad_adopted();

        /* A command from the config app; its resends carry the same token */
        if (rx->len > 0 && !(_gp.have_config_token && rx->token == _gp.last_config_token))
        {
            dongle_api_gamepad_hook_config_rx(rx->data, rx->len);
            _gp.last_config_token = rx->token;
            _gp.have_config_token = true;
        }
        _gamepad_build_reply(&tx);
        break;

    case DONGLE_PID_BULK_UNRELIABLE:
    default:
        /* Dongle sent something we do not answer. Stay silent (no proactive TX). */
        DONGLE_LOGF("[DGP] Ignoring packet id %u (len %u)\n", (unsigned)rx->id, rx->len);
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

/* Identity, events and pairing choice from cfg; everything else starts empty */
static void _gamepad_init(const dongle_cfg_gamepad_s *cfg)
{
    memset(&_gp, 0, sizeof(_gp));

    if (cfg != NULL)
    {
        _gp.mode = cfg->mode;
        _gp.vid = cfg->vid;
        _gp.pid = cfg->pid;
        _gp.evt = cfg->evt;
        _gp.mode_forced = cfg->mode_forced;
        _gp.fw_version = cfg->fw_version;
        _gp.pairing = cfg->pairing;
        _gp.paired_key = dongle_wlan_pin_to_u16(cfg->pin);
        memcpy(_gp.name, cfg->name, sizeof(_gp.name));
        memcpy(_gp.manufacturer, cfg->manufacturer, sizeof(_gp.manufacturer));
    }
    else
    {
        dongle_wake_strcopy(_gp.name, DONGLE_WAKE_NAME_LEN, NULL);
        dongle_wake_strcopy(_gp.manufacturer, DONGLE_WAKE_MANUFACTURER_LEN, NULL);
    }
}

void dongle_api_gamepad_wlan_init(const dongle_cfg_gamepad_s *cfg)
{
    _gamepad_init(cfg);

    _gp.wlan_link = DONGLE_LINK_UNDEFINED;
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;
    _gp.wlan_deadline_us = 0;
    _gp.candidate = -1;

    DONGLE_LOGF("[DGP] Init (mode %u, vid 0x%04X, pid 0x%04X, paired %04u%s)\n",
                (unsigned)_gp.mode, _gp.vid, _gp.pid, _gp.paired_key, _gp.pairing ? ", pairing" : "");
    DONGLE_LOGF("[DGP] Event subscriptions: rumble %u, player %u, transport %u\n",
                (unsigned)_gp.evt.rumble, (unsigned)_gp.evt.player_number,
                (unsigned)_gp.evt.transport_status);

    /* Pick the first session id before any traffic flows. */
    _gamepad_refresh_wake();
}

void dongle_api_gamepad_scan_result(const char *ssid, uint8_t ssid_len, int16_t rssi)
{
    uint16_t key;
    if (_gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_SCANNING || !_gamepad_ssid_key(ssid, ssid_len, &key))
    {
        return;
    }

    // A dongle shows up once per channel and beacon, keep its strongest signal
    for (uint8_t i = 0; i < _gp.candidate_count; i++)
    {
        if (_gp.candidates[i].key == key)
        {
            if (rssi > _gp.candidates[i].rssi)
            {
                _gp.candidates[i].rssi = rssi;
            }
            return;
        }
    }

    if (_gp.candidate_count < DONGLE_GAMEPAD_CANDIDATES_MAX)
    {
        _gp.candidates[_gp.candidate_count++] = (dongle_gamepad_candidate_s){.key = key, .rssi = rssi};
    }
}

void dongle_api_gamepad_set_identity(dongle_mode_t mode, uint16_t vid, uint16_t pid,
                                     const uint8_t name[DONGLE_WAKE_NAME_LEN],
                                     const uint8_t manufacturer[DONGLE_WAKE_MANUFACTURER_LEN])
{
    _gp.mode = mode;
    _gp.vid = vid;
    _gp.pid = pid;
    if (name)
    {
        memcpy(_gp.name, name, sizeof(_gp.name));
    }
    if (manufacturer)
    {
        memcpy(_gp.manufacturer, manufacturer, sizeof(_gp.manufacturer));
    }
    _gp.mode_pending = false;

    /* A new session makes the dongle bring its console up as us */
    _gamepad_refresh_wake();
    _gp.link_wake_due = _gp.link_up;
}

bool dongle_api_gamepad_config_send(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0 || len > 64)
    {
        return false;
    }

    uint8_t next = (uint8_t)((_gp.config_head + 1u) % DONGLE_GAMEPAD_CONFIG_QUEUE_LEN);
    if (next == _gp.config_tail)
    {
        return false;
    }

    _gp.config_queue[_gp.config_head].len = len;
    memcpy(_gp.config_queue[_gp.config_head].data, data, len);
    _gp.config_head = next;
    return true;
}

void dongle_api_gamepad_bulk_send(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0 || len > 64)
    {
        return;
    }

    _gp.bulk.len = len;
    memcpy(_gp.bulk.data, data, len);
    _gp.bulk_pending = true;
}

bool dongle_api_gamepad_adopted(void)
{
    return _gp.wlan_connphase == DONGLE_GAMEPAD_CONNPHASE_UP;
}

bool dongle_api_gamepad_get_dongle(dongle_beacon_s *out)
{
    if (!out || !_gp.dongle_seen)
    {
        return false;
    }

    *out = _gp.dongle;
    return true;
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

/* -------------------------------------------------------------------------- */
/* GAMEPAD API LINK (Bluetooth)                                               */
/* -------------------------------------------------------------------------- */

void dongle_api_gamepad_link_init(const dongle_cfg_gamepad_s *cfg)
{
    _gamepad_init(cfg);
    _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_DOWN;
    _gp.candidate = -1;

    /* Pick the first session id before any traffic flows. */
    _gamepad_refresh_wake();

    DONGLE_LOGF("[DGP] Link init (mode %u, vid 0x%04X, pid 0x%04X)\n", (unsigned)_gp.mode, _gp.vid, _gp.pid);
}

void dongle_api_gamepad_link_set_up(bool up)
{
    /* A new session either way: what was queued belonged to the last one */
    _gamepad_clear_session();
    _gp.link_up = up;
    _gp.link_wake_due = false; /* the dongle's host info comes first */
    _gp.link_interval_us = 0;
    _gp.link_report_head = 0;
    _gp.link_report_tail = 0;
    _gp.link_peeked = 0;
    _gp.wlan_connphase = up ? DONGLE_GAMEPAD_CONNPHASE_CONNECTED : DONGLE_GAMEPAD_CONNPHASE_DOWN;

    DONGLE_LOGF("[DGP] Link %s\n", up ? "UP" : "DOWN");
}

/* The dongle's host info: follow its mode unless ours was chosen, then tell it who we are */
static void _gamepad_link_host(const uint8_t *payload, uint16_t len)
{
    dongle_link_host_s host;
    if (len != sizeof(host))
    {
        return;
    }
    memcpy(&host, payload, sizeof(host));

    _gp.dongle = host.beacon;
    _gp.dongle_seen = true;
    _gp.link_interval_us = host.report_interval_us;

    _gp.dongle_mismatch = (host.beacon.protocol != DONGLE_PROTOCOL_VERSION);
    if (_gp.dongle_mismatch || _gp.mode_pending)
    {
        return;
    }

    const dongle_mode_t host_mode = (dongle_mode_t)host.beacon.host_mode;
    if (!_gp.mode_forced && host.beacon.host_kind != DONGLE_HOST_UNKNOWN && host_mode != DONGLE_MODE_UNKNOWN &&
        host_mode != _gp.mode && dongle_api_gamepad_hook_mode_request(host_mode))
    {
        /* dongle_api_gamepad_set_identity() follows, and sends the WAKE */
        DONGLE_LOGF("[DGP] Dongle wants mode %u, switching from %u\n", (unsigned)host_mode, (unsigned)_gp.mode);
        _gp.mode_pending = true;
        return;
    }

    if (_gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_UP)
    {
        _gp.link_wake_due = true;
    }
}

void dongle_api_gamepad_link_rx_msg(const uint8_t *msg, uint16_t len)
{
    if (!msg || len < 1 || len > DONGLE_LINK_MSG_MAX || !_gp.link_up)
    {
        return;
    }

    const uint8_t *payload = &msg[1];
    const uint16_t plen = (uint16_t)(len - 1u);
    uint8_t data[64] = {0};
    memcpy(data, payload, plen);

    switch ((dongle_link_msg_t)msg[0])
    {
    case DONGLE_LINK_MSG_HOST:
        _gamepad_link_host(payload, plen);
        break;
    case DONGLE_LINK_MSG_OUTPUT:
        if (plen > 0)
        {
            dongle_api_gamepad_hook_set_outputreport(data, plen);
        }
        break;
    case DONGLE_LINK_MSG_CONFIG:
        if (plen > 0)
        {
            dongle_api_gamepad_hook_config_rx(data, plen);
        }
        break;
    default:
        break; /* WAKE and REPORT only go the other way */
    }
}

void dongle_api_gamepad_link_rx_status(const uint8_t *data, uint16_t len)
{
    if (data && len == sizeof(dongle_status_s) && _gp.link_up)
    {
        dongle_status_s status;
        memcpy(&status, data, sizeof(status));
        _gamepad_apply_status(&status);
    }
}

bool dongle_api_gamepad_link_peek_msg(uint8_t msg[DONGLE_LINK_MSG_MAX], uint16_t *len)
{
    if (!msg || !len || !_gp.link_up)
    {
        return false;
    }

    if (_gp.link_wake_due)
    {
        _gp.link_peeked = DONGLE_LINK_MSG_WAKE;
        msg[0] = DONGLE_LINK_MSG_WAKE;
        memcpy(&msg[1], &_gp.wake, sizeof(_gp.wake));
        *len = (uint16_t)(1u + sizeof(_gp.wake));
        return true;
    }
    if (_gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_UP)
    {
        return false;
    }

    const dongle_gamepad_config_pkt_s *pkt;
    if (_gp.link_report_tail != _gp.link_report_head)
    {
        _gp.link_peeked = DONGLE_LINK_MSG_REPORT;
        pkt = &_gp.link_reports[_gp.link_report_tail];
    }
    else if (_gp.config_tail != _gp.config_head)
    {
        _gp.link_peeked = DONGLE_LINK_MSG_CONFIG;
        pkt = &_gp.config_queue[_gp.config_tail];
    }
    else
    {
        return false;
    }

    msg[0] = _gp.link_peeked;
    memcpy(&msg[1], pkt->data, pkt->len);
    *len = (uint16_t)(1u + pkt->len);
    return true;
}

void dongle_api_gamepad_link_msg_sent(void)
{
    switch ((dongle_link_msg_t)_gp.link_peeked)
    {
    case DONGLE_LINK_MSG_WAKE:
        _gp.link_wake_due = false;
        _gp.wlan_connphase = DONGLE_GAMEPAD_CONNPHASE_UP;
        DONGLE_LOGF("[DGP] Identity sent (session 0x%03X)\n", _gp.session.id);
        break;
    case DONGLE_LINK_MSG_REPORT:
        _gp.link_report_tail = (uint8_t)((_gp.link_report_tail + 1u) % DONGLE_GAMEPAD_LINK_REPORTS);
        break;
    case DONGLE_LINK_MSG_CONFIG:
        _gp.config_tail = (uint8_t)((_gp.config_tail + 1u) % DONGLE_GAMEPAD_CONFIG_QUEUE_LEN);
        break;
    default:
        break;
    }
    _gp.link_peeked = 0;
}

bool dongle_api_gamepad_link_next_report(uint8_t out[DONGLE_LINK_REPORT_MAX], uint16_t *len)
{
    if (!out || !len || _gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_UP)
    {
        return false;
    }

    uint8_t report[64] = {0};
    uint16_t n = 0;
    bool reliable = false;
    if (!dongle_api_gamepad_hook_get_inputreport(report, &n, &reliable))
    {
        return false;
    }
    if (n > 64)
    {
        n = 64;
    }

    if (reliable)
    {
        uint8_t next = (uint8_t)((_gp.link_report_head + 1u) % DONGLE_GAMEPAD_LINK_REPORTS);
        if (next != _gp.link_report_tail && n > 0)
        {
            _gp.link_reports[_gp.link_report_head].len = n;
            memcpy(_gp.link_reports[_gp.link_report_head].data, report, n);
            _gp.link_report_head = next;
        }
        return false;
    }

    _gp.logging.input_reports++;
    *len = dongle_link_trim_report(report, n, out);
    return true;
}

bool dongle_api_gamepad_link_take_bulk(uint8_t out[64], uint16_t *len)
{
    if (!out || !len || !_gp.bulk_pending || _gp.wlan_connphase != DONGLE_GAMEPAD_CONNPHASE_UP)
    {
        return false;
    }
    _gp.bulk_pending = false;
    *len = _gp.bulk.len;
    memcpy(out, _gp.bulk.data, _gp.bulk.len);
    return true;
}

uint16_t dongle_api_gamepad_link_interval_us(void)
{
    return _gp.link_interval_us;
}

void dongle_api_gamepad_wlan_task(void)
{
    uint64_t now_us = dongle_api_hook_get_time_us_u64();

    // Poll connection / link state machine
    _gamepad_poll(now_us);

    // Report stats (compiled out unless DONGLE_LIB_DEBUG_GAMEPADSTATS == 1)
    _gamepad_report_stats(now_us);
}
