#include <string.h>
#include <stddef.h>

#include <dongle.h>
#include <dongle_host.h>
#include <dongle_log.h>
#include <dongle_crosscore.h>

_Static_assert(sizeof(dongle_wake_s) == DONGLE_WAKE_S_LEN, "dongle_wake_s must fill the 64-byte data field");

/* Two console sends closer than this are one poll (a console that asks twice) */
#define DONGLE_HOST_MARK_MIN_US 1900

#define DONGLE_HOST_MAILBOX_LEN 64

/* Who we are, sent to the gamepad in the host info (set once at init) */
static uint8_t  _host_board = DONGLE_BOARD_UNKNOWN;
static uint32_t _host_fw_version = 0;

/* ==========================================================================
 * Cross-core state (strict single-producer / single-consumer)
 *
 *   R = radio task (the link)    T = transport task (console)
 *
 *   object                producer  consumer  purpose
 *   --------------------  --------  --------  --------------------------------
 *   _session_host_ss        T         R       adopted session (mode/id)
 *   _status_host_ss         T         R       rumble/player/transport out
 *   _mark_host_ss           T         R       last console send (link timing)
 *   _link_host_ss           R         T       link status (UP/DOWN)
 *   _unreliable_rx_host_ss  R         T       latest live input report
 *   _reliable_rx_host_ss    R         T       input reports that must arrive
 *   _control_rx_host_ss     R         T       WAKE (the gamepad's identity)
 *   _reliable_tx_host_ss    T         R       host->gamepad OUT queue
 *   _detect_host_ss         T         R       what the dongle is plugged into
 *   _config_rx_host_ss      R         T       gamepad WebUSB replies for the app
 *   _bulk_rx_host_ss        R         T       gamepad's latest WebUSB input report
 * ========================================================================== */

DONGLE_CROSSCORE_SNAPSHOT_TYPE(session, dongle_session_s);
dongle_snapshot_session_t _session_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(status, dongle_status_s);
dongle_snapshot_status_t _status_host_ss;

/* When the console's last report went out, and how often it takes one (link timing) */
typedef struct
{
    uint64_t t_us;
    uint32_t period_us;
} dongle_host_mark_s;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(mark, dongle_host_mark_s);
dongle_snapshot_mark_t _mark_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(link, uint8_t);
dongle_snapshot_link_t _link_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(packet, dongle_pkt_s);
dongle_snapshot_packet_t _unreliable_rx_host_ss;

/* Counts writes of _unreliable_rx_host_ss (R), so the transport can tell a new report (T) */
static atomic_uint _unreliable_rx_count;
static unsigned _unreliable_tx_count; /* T: the count when the console last took a report */

typedef struct
{
    uint8_t host_kind; /* dongle_host_kind_t, DONGLE_HOST_UNKNOWN until detected */
    uint8_t host_mode; /* dongle_mode_t */
    uint8_t done;      /* detection finished: gamepads may hear the host info */
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

/* Radio-task-local state */
typedef struct
{
    uint32_t bulk_seq;
    bool     link_up; /* cached link state for edge detection */
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
/* HOST APP                                                                   */
/* -------------------------------------------------------------------------- */

/* Drop what waits for the gamepad: it belonged to the session that ended. The radio task is the
 * OUT queue's only consumer, so only it may flush it. */
static void _host_flush_out_lane(void)
{
    dongle_pkt_s scratch;
    while (dongle_fifo_packet_pop(&_reliable_tx_host_ss, &scratch)) { }
}

/* Publish a link-state change to the transport task and, on a DOWN edge, scrub
 * the lanes so nothing stale carries into the next session. */
static void _host_set_link(bool up)
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
        _host_flush_out_lane();

        /* Invalidate the latest live input so a new session does not read a report left over
         * from the previous one (the radio task owns this snapshot). */
        dongle_pkt_s empty = {0};
        dongle_snapshot_packet_write(&_unreliable_rx_host_ss, &empty);
    }
    else
    {
        DONGLE_LOGF("[DHOST] Link UP\n");
    }
}

static void _host_reset(void)
{
    memset(&_hsm, 0, sizeof(_hsm));

    _host_flush_out_lane();

    uint8_t down = (uint8_t)DONGLE_LINK_DOWN;
    dongle_snapshot_link_write(&_link_host_ss, &down);
}

/* What the gamepad sent, to the queue the transport task reads it from */
static void _host_to_transport(const dongle_pkt_s *pkt)
{
    switch (pkt->id)
    {
    case DONGLE_PID_CORE_UNRELIABLE:
        /* Latest-wins: only the newest input report matters. */
        dongle_snapshot_packet_write(&_unreliable_rx_host_ss, pkt);
        /* One writer: no read-modify-write needed (the Cortex-M0+ has none) */
        atomic_store_explicit(&_unreliable_rx_count,
                              atomic_load_explicit(&_unreliable_rx_count, memory_order_relaxed) + 1u,
                              memory_order_release);
        break;

    case DONGLE_PID_CORE_RELIABLE:
        /* Ordered, loss-free: command replies / reliable input. */
        dongle_fifo_packet_push(&_reliable_rx_host_ss, pkt);
        break;

    case DONGLE_PID_WAKE:
        /* The gamepad's identity: the transport task adopts the session and brings the
         * console up as it. */
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
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* HOST APP TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

/* The transport task is the OUT lane's only producer; the radio task sends it on the link */
static void _host_transport_to_link(const dongle_pkt_s *pkt)
{
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
/* HOST API LINK (Bluetooth)                                                  */
/* -------------------------------------------------------------------------- */
/*
 * Received traffic goes to the transport task through _host_to_transport(), the link state
 * through _host_set_link(), and the OUT lane (console output reports, config traffic) is drained
 * here. The link itself delivers in order and resends, so there are no tokens or acks.
 */

static dongle_pkt_s _link_tx_pending; /* popped from the OUT lane, not yet sent */
static bool _link_tx_have;
static uint8_t _link_tx_peeked; /* dongle_link_msg_t handed out by peek, 0 when none */

/* Timing (see dongle_link.h): each report's delay, against the console's sends */
static uint16_t _link_interval_us; /* the pacing we asked for: arrivals repeat with this period */
static uint16_t _link_pc_interval_us = DONGLE_LINK_INTERVAL_PC_US;
static int32_t  _link_steer_delay[DONGLE_LINK_STEER_SAMPLES]; /* arrival - (first arrival + k periods) */
static uint32_t _link_steer_t0;    /* arrival of the window's first report */
static uint32_t _link_steer_k;     /* reports (sequence numbers) since then */
static uint16_t _link_steer_seq;   /* last sequence number */
static uint8_t  _link_steer_n;
static uint8_t  _link_steer_skip;
static bool     _link_steer_due;
static int16_t  _link_steer_shift;

/* The gamepad's radio queue (see dongle_link.h): each report's wait, against the quickest */
static bool     _link_queue_have_cur;
static bool     _link_queue_have_prev;
static uint32_t _link_queue_min_cur;  /* arrival - send time, gamepad and dongle clocks: modular */
static uint32_t _link_queue_min_prev;
static uint32_t _link_queue_window_us;
static uint8_t  _link_queue_run;

static void _host_link_steer_reset(void)
{
    _link_queue_have_cur = false;
    _link_queue_have_prev = false;
    _link_queue_run = 0;
    _link_steer_n = 0;
    _link_steer_skip = DONGLE_LINK_STEER_SETTLE;
    _link_steer_due = false;
}

/* One arrival, report sequence number seq. Every DONGLE_LINK_STEER_SAMPLES of them, place the
 * window's undelayed clock plus the delay that covers all but DONGLE_LINK_STEER_LATE_PCT percent
 * DONGLE_LINK_STEER_LEAD_US before a send, by moving the gamepad's clock. */
static void _host_link_steer_sample(uint64_t now_us, uint16_t seq)
{
    if (_link_steer_due || !_link_interval_us)
    {
        return;
    }
    if (_link_steer_skip)
    {
        _link_steer_skip--;
        return;
    }

    const uint32_t period = _link_interval_us;
    const uint32_t now = (uint32_t)now_us;
    if (_link_steer_n == 0)
    {
        _link_steer_t0 = now;
        _link_steer_k = 0;
    }
    else
    {
        _link_steer_k += (uint16_t)(seq - _link_steer_seq); /* lost reports still count */
    }
    _link_steer_seq = seq;
    _link_steer_delay[_link_steer_n++] = (int32_t)(now - _link_steer_t0 - _link_steer_k * period);
    if (_link_steer_n < DONGLE_LINK_STEER_SAMPLES)
    {
        return;
    }
    _link_steer_n = 0;

    dongle_host_mark_s mark;
    dongle_snapshot_mark_read(&_mark_host_ss, &mark);
    const uint64_t mark_us = mark.t_us;
    if (!mark_us || now_us < mark_us || (now_us - mark_us) > 64u * period)
    {
        return; /* the console is not taking reports */
    }

    /* Only when the console's sends repeat every whole number of our periods. Otherwise (a
     * report every slot pair against a computer's 2 ms) every send has a new report anyway. */
    const uint32_t usb_periods = (mark.period_us + period / 2u) / period;
    const int32_t usb_rest = (int32_t)mark.period_us - (int32_t)(usb_periods * period);
    if (!usb_periods || usb_rest > 100 || usb_rest < -100)
    {
        return;
    }

    /* Insertion sort: 64 values */
    for (uint8_t i = 1; i < DONGLE_LINK_STEER_SAMPLES; i++)
    {
        int32_t v = _link_steer_delay[i];
        uint8_t j = i;
        while (j > 0 && _link_steer_delay[j - 1] > v)
        {
            _link_steer_delay[j] = _link_steer_delay[j - 1];
            j--;
        }
        _link_steer_delay[j] = v;
    }
    const uint8_t rank = (uint8_t)(DONGLE_LINK_STEER_SAMPLES - 1u -
                                   (DONGLE_LINK_STEER_SAMPLES * DONGLE_LINK_STEER_LATE_PCT) / 100u);
    int32_t budget = _link_steer_delay[rank] - _link_steer_delay[0];
    const int32_t budget_max = (int32_t)period - 2 * (int32_t)DONGLE_LINK_STEER_LEAD_US;
    if (budget > budget_max)
    {
        budget = budget_max; /* more spread than a period: keep the fastest ones on time */
    }

    /* When the window's first report would have arrived that late, against the sends: sends
     * repeat every USB period, a whole number of our periods, so one comes up every period */
    const uint32_t planned = _link_steer_t0 + (uint32_t)_link_steer_delay[0] + (uint32_t)budget;
    const int32_t since = (int32_t)(planned - (uint32_t)mark_us);
    const int32_t phase = ((since % (int32_t)period) + (int32_t)period) % (int32_t)period;
    int32_t shift = (int32_t)period - phase - (int32_t)DONGLE_LINK_STEER_LEAD_US; /* > 0: could come later */
    if (shift > (int32_t)(period / 2u))
    {
        shift -= (int32_t)period;
    }
    if (shift < (int32_t)DONGLE_LINK_STEER_DEADBAND_US && shift > -(int32_t)DONGLE_LINK_STEER_DEADBAND_US)
    {
        return;
    }

    /* Big moves in steps: the delays are measured again after each one */
    const int32_t limit = (int32_t)(period / 4u);
    if (shift > limit) shift = limit;
    if (shift < -limit) shift = -limit;
    _link_steer_shift = (int16_t)shift;
    _link_steer_due = true;
}

/* One arrival and the gamepad's time handing it to its radio. A run of late ones means a report
 * is waiting in the gamepad's radio: skip one period. */
static void _host_link_queue_sample(uint32_t now, uint32_t t_pad_us)
{
    if (!_link_interval_us)
    {
        return;
    }

    /* The quickest wait over the current and the last window: the clocks drift apart slowly */
    const uint32_t offset = now - t_pad_us;
    if (_link_queue_have_cur && (uint32_t)(now - _link_queue_window_us) >= DONGLE_LINK_QUEUE_WINDOW_US)
    {
        _link_queue_min_prev = _link_queue_min_cur;
        _link_queue_have_prev = true;
        _link_queue_have_cur = false;
    }
    if (!_link_queue_have_cur)
    {
        _link_queue_min_cur = offset;
        _link_queue_have_cur = true;
        _link_queue_window_us = now;
    }
    else if ((int32_t)(offset - _link_queue_min_cur) < 0)
    {
        _link_queue_min_cur = offset;
    }
    uint32_t base = _link_queue_min_cur;
    if (_link_queue_have_prev && (int32_t)(_link_queue_min_prev - base) < 0)
    {
        base = _link_queue_min_prev;
    }

    if (_link_steer_due || _link_steer_skip)
    {
        return; /* a move is on its way or settling */
    }

    if ((offset - base) <= DONGLE_LINK_QUEUE_LATE_US)
    {
        _link_queue_run = 0;
        return;
    }
    if (++_link_queue_run < DONGLE_LINK_QUEUE_RUN)
    {
        return;
    }
    _link_queue_run = 0;
    _link_steer_shift = (int16_t)_link_interval_us; /* skip one: the waiting report goes */
    _link_steer_due = true;
}

void dongle_api_host_link_init(const dongle_cfg_host_s *cfg)
{
    _host_reset();
    _link_tx_have = false;
    _link_tx_peeked = 0;
    _link_interval_us = 0;
    _host_link_steer_reset();

    if (cfg != NULL)
    {
        _host_board = cfg->board;
        _host_fw_version = cfg->fw_version;
    }
}

void dongle_api_host_link_set_up(bool up)
{
    _host_set_link(up);
    if (!up)
    {
        _link_tx_have = false;
    }
    _link_tx_peeked = 0;
    _link_interval_us = 0;
    _host_link_steer_reset();
}

bool dongle_api_host_link_get_host(dongle_link_host_s *out)
{
    dongle_host_detect_s detect;
    dongle_snapshot_detect_read(&_detect_host_ss, &detect);

    if (!out || !detect.done)
    {
        return false;
    }

    out->info.protocol   = DONGLE_PROTOCOL_VERSION;
    out->info.board      = _host_board;
    out->info.host_kind  = detect.host_kind;
    out->info.host_mode  = (detect.host_kind == DONGLE_HOST_UNKNOWN) ? DONGLE_MODE_UNKNOWN : detect.host_mode;
    out->info.fw_version = _host_fw_version;
    out->report_interval_us = (detect.host_kind == DONGLE_HOST_PC) ? _link_pc_interval_us
                                                                     : DONGLE_LINK_INTERVAL_DEFAULT_US;

    /* The gamepad paces with this from now on: measure afresh */
    if (_link_interval_us != out->report_interval_us)
    {
        _link_interval_us = out->report_interval_us;
        _host_link_steer_reset();
    }
    return true;
}

void dongle_api_host_link_rx_report(const uint8_t *payload, uint16_t len, uint16_t seq, uint32_t t_pad_us)
{
    dongle_pkt_s pkt = {0};
    if (!payload || !dongle_link_untrim_report(payload, len, pkt.data, &pkt.len))
    {
        return;
    }
    pkt.id = DONGLE_PID_CORE_UNRELIABLE;
    _host_to_transport(&pkt);
    const uint64_t now = dongle_api_hook_get_time_us_u64();
    _host_link_queue_sample((uint32_t)now, t_pad_us);
    _host_link_steer_sample(now, seq);
}

void dongle_api_host_link_set_pc_interval(uint16_t interval_us)
{
    _link_pc_interval_us = interval_us ? interval_us : DONGLE_LINK_INTERVAL_PC_US;
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
    _host_to_transport(&pkt);
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
    _host_to_transport(&pkt);
}

bool dongle_api_host_link_peek_msg(uint8_t msg[DONGLE_LINK_MSG_MAX], uint16_t *len)
{
    if (!msg || !len)
    {
        return false;
    }
    if (_link_steer_due)
    {
        msg[0] = DONGLE_LINK_MSG_TIMING;
        memcpy(&msg[1], &_link_steer_shift, sizeof(_link_steer_shift));
        *len = (uint16_t)(1u + sizeof(_link_steer_shift));
        _link_tx_peeked = DONGLE_LINK_MSG_TIMING;
        return true;
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
    _link_tx_peeked = msg[0];
    memcpy(&msg[1], _link_tx_pending.data, n);
    *len = (uint16_t)(n + 1u);
    return true;
}

void dongle_api_host_link_msg_sent(void)
{
    if (_link_tx_peeked == DONGLE_LINK_MSG_TIMING)
    {
        /* Measure again once the move has taken effect */
        _link_steer_due = false;
        _link_steer_n = 0;
        _link_steer_skip = DONGLE_LINK_STEER_SETTLE;
    }
    else if (_link_tx_peeked)
    {
        _link_tx_have = false;
    }
    _link_tx_peeked = 0;
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

    /* React to the link. On the UP->DOWN edge the gamepad is
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

    dongle_pkt_s pkt = {0};
    memcpy(pkt.data, data, len);
    pkt.len = len;
    pkt.id = DONGLE_PID_CORE_RELIABLE;
    _host_transport_to_link(&pkt);
}

bool dongle_api_host_transport_send_config(const uint8_t *data, uint16_t len)
{
    if(!data || len == 0 || len > 64)
    {
        return false;
    }

    dongle_pkt_s pkt = {0};
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
        /* Count first: a report that lands during the read is new again next time */
        _unreliable_tx_count = atomic_load_explicit(&_unreliable_rx_count, memory_order_acquire);
        dongle_snapshot_packet_read(&_unreliable_rx_host_ss, pkt);
    }

    return pkt->len > 0;
}

bool dongle_api_host_transport_input_new(void)
{
    /* A reliable report waiting (this task is that queue's consumer), or a new live one */
    return atomic_load_explicit(&_reliable_rx_host_ss.head, memory_order_relaxed) !=
               atomic_load_explicit(&_reliable_rx_host_ss.tail, memory_order_acquire) ||
           atomic_load_explicit(&_unreliable_rx_count, memory_order_acquire) != _unreliable_tx_count;
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
    if (delta <= DONGLE_HOST_MARK_MIN_US)
    {
        return;
    }

    dongle_host_mark_s mark = {.t_us = this_time, .period_us = (uint32_t)delta};
    dongle_snapshot_mark_write(&_mark_host_ss, &mark);
    last_time = this_time;
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