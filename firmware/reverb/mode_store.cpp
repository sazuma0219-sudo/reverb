// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// Mode memory in flash (EEPROM emulation, last 4 KB sector). Layout: [0] = magic, [1] = mode.
// EEPROM.commit() pauses core0 (rp2040.idleOtherCore) for the erase + program (~50 ms), so it is only
// called from core1 while the engine holds the wet output silent (engine SW_SAVE state).
#include "mode_store.h"

#include <EEPROM.h>
#include "config.h"
#include "shared.h"

#define STORE_MAGIC 0xA5

int mode_store_load() {
    EEPROM.begin(256);
    if (EEPROM.read(0) == STORE_MAGIC && EEPROM.read(1) < MODE_COUNT) return EEPROM.read(1);
    return -1;
}

// core1, called often
void mode_store_poll() {
    if (!g_sh.save_ready || g_sh.save_done) return;
    int m = g_sh.save_mode;
    EEPROM.write(0, STORE_MAGIC);
    EEPROM.write(1, (uint8_t)m);
    EEPROM.commit();
    g_sh.mode_saved = m;
    g_sh.saves = g_sh.saves + 1;
    g_sh.save_done = 1;
}
