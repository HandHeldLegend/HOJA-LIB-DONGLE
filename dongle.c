#include <dongle.h>
#include <dongle_gamepad.h>
#include <dongle_host.h>

#include <dongle_crosscore.h>

/* -------------------------------------------------------------------------- */
/* Generic utility hooks                                                      */
/*                                                                            */
/* These are the only platform primitives the core library needs directly.    */
/* Provide strong definitions in your platform adapter. The connection         */
/* lifecycle (radio init / bind / connect) is owned by the platform and        */
/* orchestrated through the dongle_api_gamepad_hook_* callbacks instead.        */
/* -------------------------------------------------------------------------- */

__attribute__((weak)) uint64_t dongle_api_hook_get_time_us_u64(void)
{
    return 0;
}

__attribute__((weak)) uint16_t dongle_api_hook_get_rand_u16(void)
{
    return 0;
}

uint16_t dongle_api_generate_ack(void)
{
    static uint16_t last_ack = 0;
    uint16_t ack = dongle_api_hook_get_rand_u16() & 0xFFFu;
    if((ack==0) || (ack==last_ack))
    {
        ack+=1;
    }
    return ack;
}
