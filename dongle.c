#include "dongle.h"

__attribute__((weak)) void dongle_api_gp_hook_udp_tx(const dongle_pkt_s *out, uint8_t ip[4], uint16_t port)
{
        
}

__attribute__((weak)) bool dongle_api_gp_hook_get_inputreport(const uint8_t *data, uint16_t *len, bool *reliable)
{
    return false;
}

__attribute__((weak)) void dongle_api_gp_hook_set_outputreport(const uint8_t *data, uint16_t len)
{

}

__attribute__((weak)) void dongle_api_gp_hook_connect(const char *ssid, const char *pw)
{

}

__attribute__((weak)) uint64_t dongle_api_hook_time_us(void)
{
    return 0;
}

__attribute__((weak)) void dongle_api_gp_hook_set_rumble(uint8_t rumble_left, uint8_t rumble_right, uint8_t brake_left, uint8_t brake_right)
{
    (void)rumble_left;
    (void)rumble_right;
    (void)brake_left;
    (void)brake_right;
}

__attribute__((weak)) void dongle_api_gp_hook_set_transport(bool connected)
{
    (void)connected;
}

__attribute__((weak)) void dongle_api_gp_hook_set_player(uint8_t player_number)
{
    (void)player_number;
}

__attribute__((weak)) void dongle_api_hook_set_ip(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4])
{

}

__attribute__((weak)) void dongle_api_hook_port_bind(uint16_t port)
{

}

__attribute__((weak)) void dongle_api_hook_port_unbind(void)
{

}

__attribute__((weak)) void dongle_api_hook_bringup(void)
{

}

__attribute__((weak)) void dongle_api_hook_teardown(void)
{

}

void dongle_api_gp_task(void)
{
    uint64_t now_us = dongle_api_hook_time_us();
}
