#ifndef HOJA_LIB_DONGLE_LINK_H
#define HOJA_LIB_DONGLE_LINK_H

/*
 * Bluetooth link between a gamepad and its dongle.
 *
 * Copyright (c) 2026 Hand Held Legend, LLC
 * Author: Mitchell Cairns
 *
 * SPDX-License-Identifier: MIT-0
 */

/**
 * @file dongle_link.h
 * @brief What a gamepad and its dongle say to each other over a Bluetooth Classic link.
 *
 * The link itself (pairing, reconnecting, pacing, timing) is the platform's: on HOJA
 * hardware the FLOSS link profile, with the gamepad connecting and the dongle storing
 * nothing. This header defines the traffic on top of it, in three lanes:
 *
 *   lane                       direction          carried by
 *   -------------------------  -----------------  -------------------------------------
 *   live input report          gamepad -> dongle  link INPUT, id DONGLE_LINK_IN_REPORT
 *   live WebUSB report (app)   gamepad -> dongle  link INPUT, id DONGLE_LINK_IN_BULK
 *   status (rumble, player)    dongle -> gamepad  link OUTPUT, id DONGLE_LINK_OUT_STATUS
 *   everything that must       both               a second L2CAP channel on
 *   arrive, in order                              DONGLE_LINK_PSM_RELIABLE
 *
 * The link INPUT/OUTPUT lanes are latest wins: a newer report replaces one that has not
 * gone out yet. The reliable channel carries one message per SDU, [type][payload]
 * (dongle_link_msg_t): the dongle's host info, the gamepad's identity (WAKE), input
 * reports that must not be dropped (command replies), output reports from the console,
 * and the config app's traffic.
 *
 * Live input reports drop their trailing zero bytes on the air (a report's unused
 * fields are zero, so most of a 64-byte report is often empty). The first byte carries
 * the full length so the dongle hands the console exactly the report the gamepad made:
 *
 *   DONGLE_LINK_IN_REPORT payload: [full length][report bytes up to the last non-zero one]
 */

#include <dongle.h>

#ifdef __cplusplus
extern "C" {
#endif

/** L2CAP PSM of the reliable channel (dynamic range, odd). The gamepad opens it. */
#define DONGLE_LINK_PSM_RELIABLE 0x1003u

/** Name the dongle answers searches with; gamepads look for it as a prefix. */
#define DONGLE_LINK_NAME "HOJA Dongle"

/* Link INPUT report ids (gamepad -> dongle) */
#define DONGLE_LINK_IN_REPORT 1u /**< live input report of the current mode, trimmed (see above) */
#define DONGLE_LINK_IN_BULK   2u /**< the gamepad's live WebUSB report for the config app */

/* Link OUTPUT report ids (dongle -> gamepad) */
#define DONGLE_LINK_OUT_STATUS 1u /**< dongle_status_s */

/** Largest live report payload: the length byte plus a full report. */
#define DONGLE_LINK_REPORT_MAX (1u + 64u)

/** Reliable channel message types (first byte of each SDU). */
typedef enum
{
    DONGLE_LINK_MSG_HOST   = 1, /**< dongle -> gamepad: dongle_link_host_s, on connect and on change */
    DONGLE_LINK_MSG_WAKE   = 2, /**< gamepad -> dongle: dongle_wake_s, the gamepad's identity and mode */
    DONGLE_LINK_MSG_REPORT = 3, /**< gamepad -> dongle: an input report that must arrive (up to 64 bytes) */
    DONGLE_LINK_MSG_OUTPUT = 4, /**< dongle -> gamepad: an output report from the console (up to 64 bytes) */
    DONGLE_LINK_MSG_CONFIG = 5, /**< both: config app traffic (up to 64 bytes) */
    DONGLE_LINK_MSG_TIMING = 6, /**< dongle -> gamepad: int16 (little endian), move the report clock by this many us (+ = later) */
} dongle_link_msg_t;

/** Largest reliable message: type byte plus the largest payload. */
#define DONGLE_LINK_MSG_MAX (1u + 64u)

/** What the dongle is plugged into, and how often it wants input reports. */
#pragma pack(push, 1)
typedef struct
{
    dongle_beacon_s beacon;    /**< protocol, board, host kind, mode for that host, firmware */
    uint16_t report_interval_us; /**< live input pacing the dongle asks for (0: the gamepad's own) */
} dongle_link_host_s;
#pragma pack(pop)

/*
 * Timing: the dongle sends the console a report on its own clock (every USB poll period it
 * picks up the newest one), and the console should get one new report at every send.
 * The gamepad's reports leave on a steady clock but arrive with a spread: its own task
 * loop, and radio resends a slot pair (1.25 ms) or more late. The link's sequence numbers
 * say when each report would have arrived with no delay at all; the dongle measures every
 * report's delay over that, and moves the gamepad's report clock (DONGLE_LINK_MSG_TIMING)
 * so that a report delayed as much as all but the slowest DONGLE_LINK_STEER_LATE_PCT
 * percent still arrives DONGLE_LINK_STEER_LEAD_US before its send. That is a small jitter
 * buffer: every report waits for its send, so the console sees one per period, evenly.
 * A report later than that (rare) goes out at the host's next poll if it polls faster
 * than the send period (see the dongle's USB transport).
 */
#define DONGLE_LINK_STEER_LEAD_US     150u /* margin before the send */
#define DONGLE_LINK_STEER_LATE_PCT    3u   /* reports allowed to miss their send */
#define DONGLE_LINK_STEER_SAMPLES     64u  /* arrivals per measurement */
#define DONGLE_LINK_STEER_SETTLE      8u   /* arrivals ignored after a move */
#define DONGLE_LINK_STEER_DEADBAND_US 50u  /* smaller errors are left alone */

/*
 * The gamepad's radio queue. At one report per slot pair the gamepad hands its radio reports
 * exactly as fast as they can leave, so a report the radio had to send again (or two handed
 * over within one slot pair) leaves the next one waiting, and every report after it a slot
 * pair late, for good. Each report carries the time it was handed to the radio; the dongle
 * sees the wait, and when DONGLE_LINK_QUEUE_RUN reports in a row arrive more than
 * DONGLE_LINK_QUEUE_LATE_US later than the link's quickest, it moves the gamepad's clock a
 * whole period later: one report is skipped and the waiting one goes out on time.
 */
#define DONGLE_LINK_QUEUE_LATE_US   900u    /* most of a slot pair: a report was waiting ahead */
#define DONGLE_LINK_QUEUE_RUN       3u
#define DONGLE_LINK_QUEUE_WINDOW_US 500000u /* the quickest wait is taken over this long, twice */

/** Report pacing the dongle asks for, per host. */
/* PC: every slot pair (Bluetooth Classic's 1.25 ms grid). Reports cross the air on that
 * grid whatever the gamepad's clock, so at 2 ms they arrive 1.25 and 2.5 ms apart and some of
 * the computer's 2 ms polls would get two and some none. One per slot pair puts a new report
 * in every poll, at most a slot pair old. */
#define DONGLE_LINK_INTERVAL_PC_US      1250u
#define DONGLE_LINK_INTERVAL_DEFAULT_US 4000u /* Switch and consoles: 250 Hz */

/**
 * Trim a live input report for the air: writes [full length][bytes up to the last
 * non-zero one] into out and returns the payload length (at least 1).
 */
static inline uint16_t dongle_link_trim_report(const uint8_t *report, uint16_t len, uint8_t out[DONGLE_LINK_REPORT_MAX])
{
    if (len > 64u)
    {
        len = 64u;
    }
    uint16_t used = len;
    while (used > 0u && report[used - 1u] == 0u)
    {
        used--;
    }
    out[0] = (uint8_t)len;
    if (used)
    {
        memcpy(&out[1], report, used);
    }
    return (uint16_t)(used + 1u);
}

/**
 * Restore a trimmed live input report: fills report (64 bytes, zero padded) and its
 * full length. False when the payload is malformed.
 */
static inline bool dongle_link_untrim_report(const uint8_t *payload, uint16_t plen, uint8_t report[64], uint16_t *len)
{
    if (plen < 1u || payload[0] > 64u || (uint16_t)(plen - 1u) > payload[0])
    {
        return false;
    }
    memset(report, 0, 64);
    memcpy(report, &payload[1], (size_t)(plen - 1u));
    *len = payload[0];
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* HOJA_LIB_DONGLE_LINK_H */
