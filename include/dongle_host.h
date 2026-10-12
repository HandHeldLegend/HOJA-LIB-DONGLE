#ifndef HOJA_LIB_DONGLE_HOST_H
#define HOJA_LIB_DONGLE_HOST_H

#include <dongle.h>
#include <dongle_link.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* HOST API WLAN                                                              */
/* -------------------------------------------------------------------------- */

void dongle_api_host_wlan_init(const dongle_cfg_host_s *cfg);

void dongle_api_host_wlan_task(void); 

/** Feed one received datagram, with the address it came from. */
void dongle_api_host_wlan_udp_rx(const dongle_pkt_s *pkt, const uint8_t ip[4]);

/* -------------------------------------------------------------------------- */
/* HOST API LINK (Bluetooth)                                                  */
/* -------------------------------------------------------------------------- */
/*
 * The radio side of a Bluetooth dongle. The platform runs the link (see dongle_link.h)
 * and calls these from its radio core, in place of the WLAN task: they feed the same
 * queues the transport task and the console cores read.
 */

/** Reset the link side. cfg gives the board and firmware for the host info (may be NULL). */
void dongle_api_host_link_init(const dongle_cfg_host_s *cfg);

/** A gamepad connected (reliable channel open) or went away. */
void dongle_api_host_link_set_up(bool up);

/** The host info to send the gamepad (DONGLE_LINK_MSG_HOST). False until host detection finished. */
bool dongle_api_host_link_get_host(dongle_link_host_s *out);

/** A live input report arrived (DONGLE_LINK_IN_REPORT payload, trimmed), with the link's sequence number. */
void dongle_api_host_link_rx_report(const uint8_t *payload, uint16_t len, uint16_t seq);

/** The gamepad's live WebUSB report for the config app arrived (DONGLE_LINK_IN_BULK). */
void dongle_api_host_link_rx_bulk(const uint8_t *data, uint16_t len);

/** A reliable channel message arrived: [type][payload]. */
void dongle_api_host_link_rx_msg(const uint8_t *msg, uint16_t len);

/**
 * The next reliable message for the gamepad ([type][payload]: console output reports
 * and config app traffic). The same message comes back until dongle_api_host_link_msg_sent().
 */
bool dongle_api_host_link_peek_msg(uint8_t msg[DONGLE_LINK_MSG_MAX], uint16_t *len);

/** The message from dongle_api_host_link_peek_msg() went out. */
void dongle_api_host_link_msg_sent(void);

/** The latest status for the gamepad (DONGLE_LINK_OUT_STATUS). */
void dongle_api_host_link_get_status(dongle_status_s *out);

/* -------------------------------------------------------------------------- */
/* HOST API TRANSPORT                                                         */
/* -------------------------------------------------------------------------- */

void dongle_api_host_transport_task(void);

void dongle_api_host_transport_set_outputreport(const uint8_t data[64], uint16_t len);

bool dongle_api_host_transport_get_inputreport(uint8_t data[64], uint16_t *len);

/** True when the gamepad sent an input report the console has not taken yet. */
bool dongle_api_host_transport_input_new(void);

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

/** Queue a WebUSB packet from the config app for the gamepad (reliable). False if the queue is full. */
bool dongle_api_host_transport_send_config(const uint8_t *data, uint16_t len);

/** The next WebUSB packet from the gamepad for the config app: replies first, then its latest
 *  live input report. False when there is nothing new. */
bool dongle_api_host_transport_get_config(uint8_t data[64], uint16_t *len);

void dongle_api_host_transport_set_rumble(uint8_t left, uint8_t right, uint8_t brake_left, uint8_t brake_right);

void dongle_api_host_transport_set_transport(bool connected);

void dongle_api_host_transport_set_player(uint8_t player_number);

void dongle_api_host_transport_mark_sent(void);

/**
 * @brief Publish what the dongle is plugged into, for the beacon.
 *
 * Gamepads that follow the dongle switch to @p host_mode. No beacon goes out until this is
 * called; pass DONGLE_HOST_UNKNOWN when nothing was found and gamepads keep their own mode.
 */
void dongle_api_host_transport_set_detected(dongle_host_kind_t host_kind, dongle_mode_t host_mode);

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

/** The connected gamepad's identity (its WAKE reply). False while no gamepad is connected. */
bool dongle_api_host_transport_get_gamepad(dongle_wake_s *out);

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
