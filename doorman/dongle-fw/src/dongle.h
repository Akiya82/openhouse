#pragma once
#include "nrf.h"

#define DONGLE_VID         0x1209   /* pid.codes test VID/PID: dev only, replace before release */
#define DONGLE_PID         0x0001
#define DONGLE_FW_VERSION  0x0004
#define STREAM_KIND        0x80   /* IN report: [0x80][slot][n][n bytes of [len][frame]... stream] */
#define DONGLE_PRODUCT     "openhouse dongle"

/* host <-> dongle, 64-byte HID reports (no report ID)
   OUT: [cmd][seq][args...]      IN: [cmd][seq][status][data...]   status 0 = ok */
enum {
    CMD_INFO = 0x01, CMD_STATS = 0x02, CMD_ECHO = 0x03,
    CMD_DFU_UF2 = 0x10, CMD_DFU_SERIAL = 0x11, CMD_RESET = 0x12, CMD_CRASH_TEST = 0x13, CMD_CLEAR_CRASH = 0x14,
    CMD_LINK = 0x20, CMD_DOWN = 0x21, CMD_RADIO_RSSI_SCAN = 0x22, CMD_LINKSTATS = 0x23,
};
enum { ST_OK = 0, ST_BAD_CMD = 1, ST_BAD_ARG = 2, ST_TIMEOUT = 3 };

/* Adafruit nRF52 bootloader magic values in NRF_POWER->GPREGRET */
#define GPREGRET_UF2_DFU     0x57
#define GPREGRET_SERIAL_DFU  0x4E
