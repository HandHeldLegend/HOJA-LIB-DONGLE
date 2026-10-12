#ifndef HOJA_LIB_DONGLE_GAMEPAD_H
#define HOJA_LIB_DONGLE_GAMEPAD_H

#include <dongle.h>
#include <dongle_link.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* GAMEPAD API                                                                */
/*                                                                            */
/* Platform-agnostic implementation of the HOJA dongle gamepad protocol (the  */
/* "respond-only" Switch/SInput/etc. transport described in                   */
/* docs/GAMEPAD_IMPLEMENTATION.md).                                           */
/*                                                                            */
/* This library owns the protocol state machine ONLY. All networking          */
/* (Wi-Fi association, UDP send/recv, static IP) and input/output report      */
/* generation are delegated to the platform through the                       */
/* dongle_api_gamepad_hook_* callbacks below.                                 */
/*                                                                            */
/* THE GOLDEN RULE: the gamepad never transmits proactively. Every TX is one  */
/* reply to one RX. The library enforces this; the platform must only call    */
/* dongle_api_gamepad_udp_rx() when a datagram actually arrives.              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the gamepad protocol state machine.
 *
 * Stores the USB identity / personality from @p cfg (mode, vid, pid, name,
 * manufacturer) and picks a fresh random session id so the dongle
 * re-enumerates the console core.
 * The platform is responsible for radio bring-up and UDP bind BEFORE driving
 * the task loop (this library does not touch the radio directly).
 *
 * @param cfg Gamepad configuration (mode/vid/pid/name/manufacturer/pin used).
 */
void dongle_api_gamepad_wlan_init(const dongle_cfg_gamepad_s *cfg);

/**
 * @brief Feed one received datagram into the protocol engine.
 *
 * Call this from the platform UDP receive path with a full, fixed-size
 * dongle_pkt_s. The platform is responsible for source/length filtering (only
 * accept sizeof(dongle_pkt_s) datagrams from the dongle endpoint). At most one
 * reply is emitted via dongle_api_gamepad_hook_udp_tx().
 *
 * @param pkt The received packet (must be a complete dongle_pkt_s).
 */
void dongle_api_gamepad_udp_rx(const dongle_pkt_s *pkt);

/**
 * @brief Periodic task: drives connection phase, retries, and optional stats.
 *
 * Call this regularly from the platform main loop. It scans for dongles, joins
 * them in turn (the paired one first unless pairing, then the strongest) and
 * moves on from any that is gone or busy, and (when
 * DONGLE_LIB_DEBUG_GAMEPADSTATS is enabled) prints a periodic throughput line.
 */
void dongle_api_gamepad_wlan_task(void);

/**
 * @brief Report one network seen by the scan started with hook_scan_start().
 *
 * Call it for every result, duplicates included; only HOJA_WLAN_ networks count.
 */
void dongle_api_gamepad_scan_result(const char *ssid, uint8_t ssid_len, int16_t rssi);

/**
 * @brief Change the identity sent in WAKE replies, after hook_mode_request() said yes.
 *
 * Starts a new session so the dongle re-enumerates as the new mode.
 */
void dongle_api_gamepad_set_identity(dongle_mode_t mode, uint16_t vid, uint16_t pid,
                                     const uint8_t name[DONGLE_WAKE_NAME_LEN],
                                     const uint8_t manufacturer[DONGLE_WAKE_MANUFACTURER_LEN]);

/**
 * @brief Queue a WebUSB reply for the config app (sent reliably, in order).
 * @return false when the queue is full.
 */
bool dongle_api_gamepad_config_send(const uint8_t *data, uint16_t len);

/** @brief Offer the latest live WebUSB input report for the config app (only the newest is sent). */
void dongle_api_gamepad_bulk_send(const uint8_t *data, uint16_t len);

/** True while config replies are still waiting to go out (start the app's next command after them). */
bool dongle_api_gamepad_config_pending(void);

/** True while a dongle has adopted us (it sends us STATUS). */
bool dongle_api_gamepad_adopted(void);

/**
 * @brief The latest beacon of the dongle being joined or connected.
 * @return false until one has been heard.
 */
bool dongle_api_gamepad_get_dongle(dongle_beacon_s *out);

/* -------------------------------------------------------------------------- */
/* GAMEPAD API LINK (Bluetooth)                                               */
/* -------------------------------------------------------------------------- */
/*
 * The gamepad side of a Bluetooth dongle link (see dongle_link.h), in place of the WLAN
 * protocol. The platform runs the link: it connects to the dongle, opens the reliable
 * channel and paces the live reports. These calls carry the same session, identity,
 * status, mode following and config traffic as the WLAN protocol, through the same hooks
 * (get_inputreport, set_outputreport, set_player, set_transport, set_rumble, config_rx,
 * mode_request). dongle_api_gamepad_set_identity(), _config_send(), _bulk_send(),
 * _adopted() and _get_dongle() work for both.
 *
 * Order on a link: the dongle sends its host info (HOST); unless that changes our mode
 * (hook_mode_request), we send our identity (WAKE), and from then on we are adopted:
 * live reports flow and the dongle's console comes up as us.
 */

/** Initialize the link side with the gamepad's identity (as dongle_api_gamepad_wlan_init()). */
void dongle_api_gamepad_link_init(const dongle_cfg_gamepad_s *cfg);

/** The reliable channel to a dongle opened (up) or the link went away. Each link is a new session. */
void dongle_api_gamepad_link_set_up(bool up);

/** A reliable channel message arrived from the dongle: [type][payload]. */
void dongle_api_gamepad_link_rx_msg(const uint8_t *msg, uint16_t len);

/** The dongle's status arrived (link OUTPUT DONGLE_LINK_OUT_STATUS). */
void dongle_api_gamepad_link_rx_status(const uint8_t *data, uint16_t len);

/**
 * The next reliable message for the dongle ([type][payload]: WAKE, input reports that must
 * arrive, config replies). The same message comes back until dongle_api_gamepad_link_msg_sent().
 */
bool dongle_api_gamepad_link_peek_msg(uint8_t msg[DONGLE_LINK_MSG_MAX], uint16_t *len);

/** The message from dongle_api_gamepad_link_peek_msg() went out. */
void dongle_api_gamepad_link_msg_sent(void);

/**
 * Take the next input report (hook_get_inputreport), once adopted. A live one comes back
 * trimmed for the air (DONGLE_LINK_IN_REPORT payload) and returns true; one that must
 * arrive is queued for the reliable channel instead and returns false.
 */
bool dongle_api_gamepad_link_next_report(uint8_t out[DONGLE_LINK_REPORT_MAX], uint16_t *len);

/** The newest live WebUSB report for the config app, once (DONGLE_LINK_IN_BULK). */
bool dongle_api_gamepad_link_take_bulk(uint8_t out[64], uint16_t *len);

/** Report pacing the dongle asked for (us), 0 until it said. */
uint16_t dongle_api_gamepad_link_interval_us(void);

/* -------------------------------------------------------------------------- */
/* GAMEPAD HOOKS (platform implements the strong versions)                    */
/*                                                                            */
/* Each hook ships a weak default so the library links even before a platform */
/* provides an implementation. Provide a strong definition for every hook you */
/* need in your platform adapter.                                             */
/* -------------------------------------------------------------------------- */

/** Return DONGLE_LINK_UP once associated with the dongle and given an address by its DHCP. */
dongle_link_status_t dongle_api_gamepad_hook_link_up(void);

/** Start a non-blocking scan; feed each result to dongle_api_gamepad_scan_result(). */
bool dongle_api_gamepad_hook_scan_start(void);

/** True while the scan is running. */
bool dongle_api_gamepad_hook_scan_active(void);

/** Begin a non-blocking association with the dongle AP. */
void dongle_api_gamepad_hook_connect_async(const char *ssid, const char *pw);

/** Leave the dongle network joined with hook_connect_async(). */
void dongle_api_gamepad_hook_disconnect(void);

/** Pairing found a dongle and it adopted us: save @p pin as the paired one. */
void dongle_api_gamepad_hook_paired(const uint8_t pin[4]);

/** The dongle's host wants @p mode and ours was not forced: return true to switch to it, then
 *  call dongle_api_gamepad_set_identity(). Return false to keep the current mode. */
bool dongle_api_gamepad_hook_mode_request(dongle_mode_t mode);

/** Transmit exactly one datagram to @p ip:@p port. The only TX path. */
void dongle_api_gamepad_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port);

/** Fill @p data/@p len with the latest input report.
 *  @param reliable set true to send it on the reliable (CORE_RELIABLE) lane.
 *  @return true if a report is available; false to reply empty (ack-only). */
bool dongle_api_gamepad_hook_get_inputreport(uint8_t data[64], uint16_t *len, bool *reliable);

/** Link: move the live report clock by @p delta_us (+ = later), as the dongle asks (DONGLE_LINK_MSG_TIMING). */
void dongle_api_gamepad_hook_shift_reports(int16_t delta_us);

/** Deliver a host OUT report (Switch command / config) to the upper layer. */
void dongle_api_gamepad_hook_set_outputreport(const uint8_t data[64], uint16_t len);

/** Notify of an assigned player index change (drive player LEDs). */
void dongle_api_gamepad_hook_set_player(uint8_t player_number);

/** Notify whether the console side is actually enumerated/connected. */
void dongle_api_gamepad_hook_set_transport(bool connected);

/** Notify of a rumble/brake amplitude change (drive haptics). */
void dongle_api_gamepad_hook_set_rumble(uint8_t left, uint8_t right, uint8_t left_brake, uint8_t right_brake);

/** A WebUSB packet from the config app, through the dongle. */
void dongle_api_gamepad_hook_config_rx(const uint8_t data[64], uint16_t len);

/** Notify that the protocol state was reset (link lost) so the platform can
 *  tear down / re-init its network stack as needed. */
void dongle_api_gamepad_hook_reset_network(void);

#ifdef __cplusplus
}
#endif

#endif /* HOJA_LIB_DONGLE_GAMEPAD_H */
