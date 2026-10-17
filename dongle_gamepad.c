#include <dongle.h>
#include <dongle_gamepad.h>
#include <dongle_log.h>

_Static_assert(sizeof(dongle_wake_s) == DONGLE_WAKE_S_LEN, "dongle_wake_s must fill the 64-byte data field");

// WebUSB replies waiting to go to the config app: room for the largest config block in one go (the
// input block is 66 replies), so a platform never has to wait for room while answering a command
#define DONGLE_GAMEPAD_CONFIG_QUEUE_LEN 80

// Input reports that must arrive (command replies), waiting for the reliable channel
#define DONGLE_GAMEPAD_LINK_REPORTS 8

typedef struct
{
    uint16_t len;
    uint8_t  data[64];
} dongle_gamepad_config_pkt_s;

typedef enum
{
    DONGLE_GAMEPAD_PHASE_DOWN = 0,      /* No link */
    DONGLE_GAMEPAD_PHASE_CONNECTED = 1, /* Link open, our identity not sent yet */
    DONGLE_GAMEPAD_PHASE_UP = 2,        /* Identity sent: the dongle adopted us */
} dongle_gamepad_phase_t;

typedef struct
{
    dongle_session_s session;
    dongle_wake_s wake;
    dongle_status_s status;

    /* WebUSB traffic for the config app: replies go reliably and in order; the live input
     * report only ever sends its newest copy */
    dongle_gamepad_config_pkt_s config_queue[DONGLE_GAMEPAD_CONFIG_QUEUE_LEN];
    uint8_t  config_head;
    uint8_t  config_tail;
    dongle_gamepad_config_pkt_s bulk;
    bool     bulk_pending;

    dongle_gamepad_phase_t phase;

    /* The dongle's host info */
    dongle_host_info_s dongle;
    bool dongle_seen;
    bool dongle_mismatch; /* It speaks another protocol version */

    /* USB identity / personality sent to the dongle (WAKE) */
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

    bool     link_up;          /* reliable channel open */
    bool     link_wake_due;    /* our identity goes out next */
    uint16_t link_interval_us; /* pacing the dongle asked for */
    dongle_gamepad_config_pkt_s link_reports[DONGLE_GAMEPAD_LINK_REPORTS]; /* input reports that must arrive */
    uint8_t  link_report_head;
    uint8_t  link_report_tail;
    uint8_t  link_peeked;      /* dongle_link_msg_t of the message handed out by peek, 0 when none */
} dongle_gamepad_sm;

static dongle_gamepad_sm _gp = {0};

/* -------------------------------------------------------------------------- */
/* GAMEPAD HOOKS (weak defaults)                                              */
/* -------------------------------------------------------------------------- */

// Implement switching to the dongle's mode (true if it will, then call set_identity)
__attribute__((weak)) bool dongle_api_gamepad_hook_mode_request(dongle_mode_t mode)
{
    (void)mode;
    return false;
}

// Implement obtaining the latest input report
__attribute__((weak)) bool dongle_api_gamepad_hook_get_inputreport(uint8_t data[64], uint16_t *len, bool *reliable)
{
    (void)data;
    (void)len;
    (void)reliable;
    return false;
}

// Implement moving the link's report clock
__attribute__((weak)) void dongle_api_gamepad_hook_shift_reports(int16_t delta_us)
{
    (void)delta_us;
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
 * dongle to re-initialize its console core (never reuse the id). */
static void _gamepad_refresh_wake(void)
{
    _gp.session.mode = (uint16_t)_gp.mode;
    _gp.session.id = _gamepad_new_session_id();

    /* dongle_wake_s.session is the packed uint16_t, not a nested dongle_session_s */
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

/* Forget the dongle we were talking to */
static void _gamepad_clear_session(void)
{
    _gp.status = (dongle_status_s){0};

    _gp.config_head = 0;
    _gp.config_tail = 0;
    _gp.bulk_pending = false;

    _gp.dongle_seen = false;
    _gp.dongle_mismatch = false;
    _gp.mode_pending = false;

    /* Fresh session id for the next connection (never reuse) */
    _gamepad_refresh_wake();
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

/* The dongle's host info: follow its mode unless ours was chosen, then tell it who we are */
static void _gamepad_link_host(const uint8_t *payload, uint16_t len)
{
    dongle_link_host_s host;
    if (len != sizeof(host))
    {
        return;
    }
    memcpy(&host, payload, sizeof(host));

    _gp.dongle = host.info;
    _gp.dongle_seen = true;
    _gp.link_interval_us = host.report_interval_us;

    _gp.dongle_mismatch = (host.info.protocol != DONGLE_PROTOCOL_VERSION);
    if (_gp.dongle_mismatch || _gp.mode_pending)
    {
        return;
    }

    const dongle_mode_t host_mode = (dongle_mode_t)host.info.host_mode;
    if (!_gp.mode_forced && host.info.host_kind != DONGLE_HOST_UNKNOWN && host_mode != DONGLE_MODE_UNKNOWN &&
        host_mode != _gp.mode && dongle_api_gamepad_hook_mode_request(host_mode))
    {
        /* dongle_api_gamepad_set_identity() follows, and sends the WAKE */
        DONGLE_LOGF("[DGP] Dongle wants mode %u, switching from %u\n", (unsigned)host_mode, (unsigned)_gp.mode);
        _gp.mode_pending = true;
        return;
    }

    if (_gp.phase != DONGLE_GAMEPAD_PHASE_UP)
    {
        _gp.link_wake_due = true;
    }
}

/* -------------------------------------------------------------------------- */
/* GAMEPAD API                                                                */
/* -------------------------------------------------------------------------- */

void dongle_api_gamepad_link_init(const dongle_cfg_gamepad_s *cfg)
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
        memcpy(_gp.name, cfg->name, sizeof(_gp.name));
        memcpy(_gp.manufacturer, cfg->manufacturer, sizeof(_gp.manufacturer));
    }
    else
    {
        dongle_wake_strcopy(_gp.name, DONGLE_WAKE_NAME_LEN, NULL);
        dongle_wake_strcopy(_gp.manufacturer, DONGLE_WAKE_MANUFACTURER_LEN, NULL);
    }

    /* Pick the first session id before any traffic flows. */
    _gamepad_refresh_wake();

    DONGLE_LOGF("[DGP] Link init (mode %u, vid 0x%04X, pid 0x%04X)\n", (unsigned)_gp.mode, _gp.vid, _gp.pid);
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

bool dongle_api_gamepad_config_pending(void)
{
    return _gp.config_tail != _gp.config_head;
}

bool dongle_api_gamepad_adopted(void)
{
    return _gp.phase == DONGLE_GAMEPAD_PHASE_UP;
}

bool dongle_api_gamepad_get_dongle(dongle_host_info_s *out)
{
    if (!out || !_gp.dongle_seen)
    {
        return false;
    }

    *out = _gp.dongle;
    return true;
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
    _gp.phase = up ? DONGLE_GAMEPAD_PHASE_CONNECTED : DONGLE_GAMEPAD_PHASE_DOWN;

    DONGLE_LOGF("[DGP] Link %s\n", up ? "UP" : "DOWN");
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
    case DONGLE_LINK_MSG_TIMING:
        if (plen == sizeof(int16_t))
        {
            int16_t delta;
            memcpy(&delta, payload, sizeof(delta));
            dongle_api_gamepad_hook_shift_reports(delta);
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
    if (_gp.phase != DONGLE_GAMEPAD_PHASE_UP)
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
        _gp.phase = DONGLE_GAMEPAD_PHASE_UP;
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
    if (!out || !len || _gp.phase != DONGLE_GAMEPAD_PHASE_UP)
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

    *len = dongle_link_trim_report(report, n, out);
    return true;
}

bool dongle_api_gamepad_link_take_bulk(uint8_t out[64], uint16_t *len)
{
    if (!out || !len || !_gp.bulk_pending || _gp.phase != DONGLE_GAMEPAD_PHASE_UP)
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
