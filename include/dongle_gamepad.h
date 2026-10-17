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
/* The gamepad side of the dongle link (see dongle_link.h). The platform runs */
/* the link: it connects to the dongle, opens the reliable channel and paces  */
/* the live reports. These calls carry the session, identity, status, mode    */
/* following and config traffic, through the hooks below.                     */
/* Order on a link: the dongle sends its host info (HOST); unless that        */
/* changes our mode (hook_mode_request), we send our identity (WAKE), and     */
/* from then on we are adopted: live reports flow and the console comes up    */
/* as us.                                                                     */
/* -------------------------------------------------------------------------- */

/** Initialize with the gamepad's identity, and pick a fresh session id. */
void dongle_api_gamepad_link_init(const dongle_cfg_gamepad_s *cfg);

/**
 * @brief Change the identity sent to the dongle (WAKE), after hook_mode_request() said yes.
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

/** True once the dongle adopted us (our identity went out). */
bool dongle_api_gamepad_adopted(void);

/**
 * @brief The connected dongle's host info.
 * @return false until it has sent it.
 */
bool dongle_api_gamepad_get_dongle(dongle_host_info_s *out);

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

/** The dongle's host wants @p mode and ours was not forced: return true to switch to it, then
 *  call dongle_api_gamepad_set_identity(). Return false to keep the current mode. */
bool dongle_api_gamepad_hook_mode_request(dongle_mode_t mode);

/** Fill @p data/@p len with the latest input report.
 *  @param reliable set true when it must arrive (it goes on the reliable channel).
 *  @return true if a report is available. */
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

#ifdef __cplusplus
}
#endif

#endif /* HOJA_LIB_DONGLE_GAMEPAD_H */
