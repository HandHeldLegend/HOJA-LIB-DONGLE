#include <dongle_gamepad.h>
#include <dongle_crosscore.h>

#define GP_TIMEOUT_US 5000000

static uint64_t _gp_last_time_us = 0;
static bool _gp_link_up = false;

// Current working ack
static dongle_session_s _gp_session;
static uint16_t _gp_ack = 0;
static uint8_t _gp_ip[4];
static uint16_t _gp_port;
static dongle_status_s _gp_status;

static dongle_cfg_gp_s _gp_cfg;

void dongle_gp_set_status(dongle_status_s *status)
{
    if(_gp_cfg.evt.rumble)
    {
        if(_gp_status.rumble_value != status->rumble_value)
        {
            dongle_api_gp_hook_set_rumble(
                status->rumble.left,
                status->rumble.right,
                status->brake.left,
                status->brake.right
            );
            _gp_status.rumble_value = status->rumble_value;
        }
    }

    if(_gp_cfg.evt.player_number)
    {
        if(_gp_status.player_number != status->player_number)
        {
            dongle_api_gp_hook_set_player(status->player_number);
            _gp_status.player_number = status->player_number;
        }
    }

    if(_gp_cfg.evt.transport_status)
    {
        if(_gp_status.transport_status != status->transport_status)
        {
            dongle_api_gp_hook_set_transport(
                status->transport_status == DONGLE_TRANSPORT_CONNECTED ? true : false
            );
            _gp_status.transport_status = status->transport_status;
        }
    }
}

static void _gp_reset_status(void)
{
    dongle_status_s blank = {0};
    dongle_gp_set_status(&blank);
    // We may not be subscribed to all events
    // so we reset after manually
    _gp_status = blank;
}

void dongle_gp_onconnect(void)
{
    
}

void dongle_gp_send_std(void)
{
    dongle_pkt_s pkt;
    // Set outbound ACK
    pkt.ack = _gp_ack;
    pkt.session = dongle_session_pack(&_gp_session);

    bool reliable;
    if(dongle_api_gp_hook_get_inputreport(pkt.data, &pkt.len, &reliable))
    {
        pkt.id = reliable ? DONGLE_PID_CORE_RELIABLE : DONGLE_PID_CORE_UNRELIABLE;
        dongle_api_gp_hook_udp_tx(&pkt, _gp_ip, _gp_port);
    }
}

void dongle_gp_send_wake(void)
{
    dongle_pkt_s pkt;
    // Create new session ID
    _gp_session.id= dongle_api_hook_rand_16u() & 0xFFF;

    pkt.ack = 0;
    pkt.len = 0;
    pkt.session = dongle_session_pack(&_gp_session);
    pkt.id = DONGLE_PID_WAKE;
    dongle_api_gp_hook_udp_tx(&pkt, _gp_ip, _gp_port);
}

void dongle_gp_udp_rx(const dongle_pkt_s *pkt)
{
    _gp_last_time_us = dongle_api_hook_time_us();
    if(!_gp_link_up)
    {
        _gp_link_up = true;
        dongle_api_gp_hook_set_link(true);
    }

    // If we have a new non-zero ack, set it, otherwise keep the same
    if (pkt->ack>0) 
    {
        _gp_ack = pkt->ack;
    }

    static uint16_t last_reliable_ack = 0;
    static bool wake_lockout = false;

    switch(pkt->id)
    {
        case DONGLE_PID_CORE_RELIABLE:
        if(pkt->ack != last_reliable_ack)
        {
            last_reliable_ack = pkt->ack;
            dongle_api_gp_hook_set_outputreport(pkt->data, pkt->len);
        }
        dongle_gp_send_std();
        wake_lockout = false;
        break;

        case DONGLE_PID_CORE_UNRELIABLE:
        dongle_gp_send_std();
        wake_lockout = false;
        break;

        case DONGLE_PID_STATUS:
        dongle_gp_send_std();
        wake_lockout = false;
        break;

        case DONGLE_PID_WAKE:
        if(!wake_lockout)
        {
            dongle_gp_send_wake();
            wake_lockout = true;
        }
        break;

        case DONGLE_PID_BULK_UNRELIABLE:
        case DONGLE_PID_CONFIG_RELIABLE:
        default:
        dongle_gp_send_std();
        wake_lockout = false;
        break;
    }
}

void dongle_gp_task(void)
{
    uint64_t now_us = dongle_api_hook_time_us();

    if(_gp_link_up)
    {
        if(now_us > _gp_last_time_us + GP_TIMEOUT_US)
        {
            _gp_link_up = false;
            dongle_api_gp_hook_set_link(false);
        }
    }
}
