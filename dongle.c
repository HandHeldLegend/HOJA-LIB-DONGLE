#include <dongle.h>
#include <dongle_gamepad.h>
#include <dongle_host.h>

#include <dongle_crosscore.h>

__attribute__((weak)) uint64_t dongle_api_hook_get_time_us_u64(void)
{
    return 0;
}

__attribute__((weak)) uint16_t dongle_api_hook_get_rand_u16(void)
{
    return 0;
}

__attribute__((weak)) bool dongle_api_wlan_hook_bringup(void)
{

}

__attribute__((weak)) void dongle_api_wlan_hook_teardown(void)
{

}

__attribute__((weak)) dongle_link_status_t dongle_api_wlan_hook_poll_link(void)
{
    return DONGLE_LINK_DOWN;
} 

__attribute__((weak)) void dongle_api_wlan_hook_bind(uint16_t port)
{

}

__attribute__((weak)) void dongle_api_wlan_hook_unbind(void)
{

}

