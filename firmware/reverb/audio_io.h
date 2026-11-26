// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
// PCM1808 + PCM5102A on one I2S bus: one PIO SM does TX and RX (rvb_io.pio), SCKI on a second SM.
// Both directions run through endless DMA rings of CFG_RING_FRAMES frames ([2n] = L, [2n+1] = R,
// 24-bit data left-justified in 32-bit words).
#pragma once
#include <stdint.h>
#include "config.h"

#define RING_MASK (CFG_RING_FRAMES - 1)

extern volatile int32_t rx_ring[2 * CFG_RING_FRAMES];
extern volatile int32_t tx_ring[2 * CFG_RING_FRAMES];

void audio_io_start();
float audio_fs();
uint32_t rx_wr_frame();   // next frame the RX DMA writes
uint32_t tx_rd_frame();   // next frame the TX DMA reads
