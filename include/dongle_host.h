#ifndef HOJA_LIB_DONGLE_HOST_H
#define HOJA_LIB_DONGLE_HOST_H

#include <dongle.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* HOST API WLAN                                                              */
/* -------------------------------------------------------------------------- */

void dongle_api_host_wlan_init(const dongle_cfg_host_s *cfg);

void dongle_api_host_wlan_task(void); 

void dongle_api_host_wlan_udp_rx(const dongle_pkt_s *pkt);

/* -------------------------------------------------------------------------- */
/* HOST API TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

void dongle_api_host_transport_task(void);

void dongle_api_host_transport_set_outputreport(const uint8_t data[64], uint16_t len);

bool dongle_api_host_transport_get_inputreport(uint8_t data[64], uint16_t *len);

void dongle_api_host_transport_set_rumble(uint8_t left, uint8_t right, uint8_t brake_left, uint8_t brake_right);

void dongle_api_host_transport_set_transport(bool connected);

void dongle_api_host_transport_set_player(uint8_t player_number);

void dongle_api_host_transport_mark_sent(void);

/* -------------------------------------------------------------------------- */
/* HOST HOOKS (platform implements the strong versions)                       */
/* -------------------------------------------------------------------------- */

void dongle_api_host_wlan_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port);
bool dongle_api_host_wlan_hook_ap_bringup(const char *ssid, const char *password, uint8_t ip[4], uint8_t mask[4]);

void dongle_api_host_wlan_hook_reset_network(void);

void dongle_api_host_transport_hook_transport_bringup(const dongle_wake_s *wake);
void dongle_api_host_transport_hook_transport_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* HOJA_LIB_DONGLE_HOST_H */
