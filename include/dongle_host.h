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

/**
 * @brief Pop the next input as a full packet (reliable-first, then unreliable).
 *
 * Uses the same selection logic as dongle_api_host_transport_get_inputreport()
 * but hands back the entire dongle_pkt_s so a core can inspect the id, session,
 * and ack alongside the payload. Consumes from the same queues, so a given
 * report is delivered by exactly one of the two getters.
 *
 * @param pkt Destination packet (filled only when true is returned).
 * @return true if a packet with a non-empty payload was available.
 */
bool dongle_api_host_transport_get_inputpacket(dongle_pkt_s *pkt);

void dongle_api_host_transport_set_rumble(uint8_t left, uint8_t right, uint8_t brake_left, uint8_t brake_right);

void dongle_api_host_transport_set_transport(bool connected);

void dongle_api_host_transport_set_player(uint8_t player_number);

void dongle_api_host_transport_mark_sent(void);

/**
 * @brief Read the latest published status (rumble / player / transport).
 *
 * Safe to call from the transport task's core (e.g. to drive status LEDs).
 *
 * @param out Destination status (left unchanged when @p out is NULL).
 */
void dongle_api_host_transport_get_status(dongle_status_s *out);

/**
 * @brief Read the current wireless link status.
 *
 * The link state is owned by the wlan task; this getter is the transport-side
 * consumer view (call it from the transport task's core).
 *
 * @return DONGLE_LINK_UP while a gamepad session is live, else DONGLE_LINK_DOWN.
 */
dongle_link_status_t dongle_api_host_transport_get_link_status(void);

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
