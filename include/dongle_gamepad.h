#ifndef HOJA_LIB_DONGLE_GAMEPAD_H
#define HOJA_LIB_DONGLE_GAMEPAD_H

#include <dongle.h>

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
 * @param cfg Gamepad configuration (mode/vid/pid/name/manufacturer used; pin reserved).
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
 * Call this regularly from the platform main loop. It polls the link state via
 * dongle_api_gamepad_hook_link_up(), drives (re)connection through
 * dongle_api_gamepad_hook_connect_async() / _apply_static_ip(), and (when
 * DONGLE_LIB_DEBUG_GAMEPADSTATS is enabled) prints a periodic throughput line.
 */
void dongle_api_gamepad_wlan_task(void);

/* -------------------------------------------------------------------------- */
/* GAMEPAD HOOKS (platform implements the strong versions)                    */
/*                                                                            */
/* Each hook ships a weak default so the library links even before a platform */
/* provides an implementation. Provide a strong definition for every hook you */
/* need in your platform adapter.                                             */
/* -------------------------------------------------------------------------- */

/** Return the current wireless link status (associated or not). */
dongle_link_status_t dongle_api_gamepad_hook_link_up(void);

/** Pin the station to the gamepad address the dongle filters on.
 *  @return true once the interface holds @p addr; false to force a retry. */
bool dongle_api_gamepad_hook_apply_static_ip(uint8_t addr[4], uint8_t mask[4], uint8_t gateway[4]);

/** Begin a non-blocking association with the dongle AP. */
void dongle_api_gamepad_hook_connect_async(const char *ssid, const char *pw);

/** Transmit exactly one datagram to @p ip:@p port. The only TX path. */
void dongle_api_gamepad_hook_udp_tx(const dongle_pkt_s *pkt, uint8_t ip[4], uint16_t port);

/** Fill @p data/@p len with the latest input report.
 *  @param reliable set true to send it on the reliable (CORE_RELIABLE) lane.
 *  @return true if a report is available; false to reply empty (ack-only). */
bool dongle_api_gamepad_hook_get_inputreport(uint8_t data[64], uint16_t *len, bool *reliable);

/** Deliver a host OUT report (Switch command / config) to the upper layer. */
void dongle_api_gamepad_hook_set_outputreport(const uint8_t data[64], uint16_t len);

/** Notify of an assigned player index change (drive player LEDs). */
void dongle_api_gamepad_hook_set_player(uint8_t player_number);

/** Notify whether the console side is actually enumerated/connected. */
void dongle_api_gamepad_hook_set_transport(bool connected);

/** Notify of a rumble/brake amplitude change (drive haptics). */
void dongle_api_gamepad_hook_set_rumble(uint8_t left, uint8_t right, uint8_t left_brake, uint8_t right_brake);

/** Notify that the protocol state was reset (link lost) so the platform can
 *  tear down / re-init its network stack as needed. */
void dongle_api_gamepad_hook_reset_network(void);

#ifdef __cplusplus
}
#endif

#endif /* HOJA_LIB_DONGLE_GAMEPAD_H */
