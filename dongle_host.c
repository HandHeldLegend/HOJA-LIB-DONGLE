#include <dongle.h>
#include <dongle_host.h>
#include <dongle_crosscore.h>

#define DONGLE_HOST_TIMEOUT_MS 5000
#define DONGLE_HOST_PUMP_MAX_WAIT_MS 1000
#define DONGLE_HOST_PUMP_MAX_WAIT_US (DONGLE_HOST_PUMP_MAX_WAIT_MS*1000)

#define DONGLE_HOST_PUMP_MIN_ALLOWED_US 1900

#define DONGLE_HOST_MAILBOX_LEN 64

DONGLE_CROSSCORE_SNAPSHOT_TYPE(session, dongle_session_s);
dongle_snapshot_session_t _session_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(status, dongle_status_s);
dongle_snapshot_status_t _status_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(pump, uint64_t);
dongle_snapshot_pump_t _pump_host_ss;

DONGLE_CROSSCORE_SNAPSHOT_TYPE(packet, dongle_pkt_s);
dongle_snapshot_packet_t _unreliable_rx_host_ss;

DONGLE_CROSSCORE_FIFO_TYPE(packet, dongle_pkt_s, DONGLE_HOST_MAILBOX_LEN);
dongle_fifo_packet_t _reliable_rx_host_ss;
dongle_fifo_packet_t _reliable_tx_host_ss;

typedef struct
{
    uint64_t timeout_us;
    uint64_t last_pump_us;
    uint16_t inflight_ack;
    dongle_pkt_s inflight_pkt;
} dongle_host_sm;

static dongle_host_sm _hsm;

/* -------------------------------------------------------------------------- */
/* HOST APP WLAN                                                              */
/* -------------------------------------------------------------------------- */

static bool _host_wlan_is_link_up(void)
{

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

static void _host_wlan_reset(void)
{

}

static void _host_wlan_arm_inflight_reliable(void)
{
    if(_hsm.inflight_ack != 0)
    {
        return;
    }

    dongle_pkt_s pkt;
    if(dongle_fifo_packet_pop(&_reliable_tx_host_ss, &pkt))
    {
        _hsm.inflight_ack = pkt.ack;
        _hsm.inflight_pkt = pkt;
    }
}

void _host_wlan_send_to_transport(const dongle_pkt_s *pkt)
{

}

static bool _host_wlan_send_reliable(void)
{

}

static void _host_wlan_send_status(void)
{

}

static void _host_wlan_send_wake(void)
{

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

}

/* -------------------------------------------------------------------------- */
/* HOST API WLAN                                                              */
/* -------------------------------------------------------------------------- */

void dongle_api_host_wlan_init(const dongle_cfg_host_s *cfg)
{
    _host_wlan_reset();
}

void dongle_api_host_wlan_task(void)
{
    uint64_t now_us = dongle_api_hook_get_time_us_u64();

    if(_timeout_reset)
    {
        _host_wlan_link_timeout_reset(now_us);
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

void dongle_api_host_wlan_udp_rx(const dongle_pkt_s *pkt)
{
    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);
    uint16_t sessionu16 = dongle_session_pack(&session);

    if((pkt->id == DONGLE_PID_WAKE) || (pkt->session == sessionu16))
    {
        _host_wlan_schedule_timeout_reset();
    }

    if((_hsm.inflight_ack != 0) && (pkt->ack == _hsm.inflight_ack))
    {
        _hsm.inflight_ack = 0;
        _host_wlan_arm_inflight_reliable();
    }

    _host_wlan_send_to_transport(pkt);
}

/* -------------------------------------------------------------------------- */
/* HOST API TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

void dongle_api_host_transport_task(void)
{
    uint64_t now_us = dongle_api_hook_get_time_us_u64();


}

void dongle_api_host_transport_set_outputreport(const uint8_t data[64], uint16_t len)
{
    if(!data || len==0) return;
    if(len>64) len = 64;

    dongle_pkt_s pkt;
    dongle_session_s session;
    dongle_snapshot_session_read(&_session_host_ss, &session);

    pkt.ack = dongle_api_generate_ack();
    memcpy(pkt.data, data, len);
    pkt.len = len;
    pkt.id = DONGLE_PID_CORE_RELIABLE;
    pkt.session = dongle_session_pack(&session);
    _host_transport_send_to_wlan(&pkt);
}

bool dongle_api_host_transport_get_inputreport(uint8_t data[64], uint16_t *len)
{
    dongle_pkt_s pkt;
    if(dongle_fifo_packet_pop(&_reliable_rx_host_ss, &pkt))
    {

    }
    else
    {
        dongle_snapshot_packet_read(&_unreliable_rx_host_ss, &pkt);
    }

    if(pkt.len>0)
    {
        memcpy(data, pkt.data, pkt.len);
        *len = pkt.len;
        return true;
    }

    *len = 0;
    return false;
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