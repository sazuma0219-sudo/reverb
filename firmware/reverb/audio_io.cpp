// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shota Azuma
#include "audio_io.h"

#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "rvb_io.pio.h"

#define RING_BYTES (CFG_RING_FRAMES * 8)
static constexpr unsigned ring_bits(unsigned b) { return b <= 1 ? 0 : 1 + ring_bits(b >> 1); }
static_assert((RING_BYTES & (RING_BYTES - 1)) == 0, "ring must be a power of 2");
static_assert(PIN_BCK == PIN_LRCK + 1, "side-set needs LRCK, BCK on consecutive pins");

alignas(RING_BYTES) volatile int32_t rx_ring[2 * CFG_RING_FRAMES];
alignas(RING_BYTES) volatile int32_t tx_ring[2 * CFG_RING_FRAMES];

static PIO s_pio = pio0;
static int s_dma_tx = -1, s_dma_rx = -1;

float audio_fs() { return (float)clock_get_hz(clk_sys) / (CFG_FS_DIV * 256.0f); }

uint32_t rx_wr_frame() {
    return ((dma_hw->ch[s_dma_rx].write_addr - (uint32_t)(uintptr_t)rx_ring) >> 3) & RING_MASK;
}
uint32_t tx_rd_frame() {
    return ((dma_hw->ch[s_dma_tx].read_addr - (uint32_t)(uintptr_t)tx_ring) >> 3) & RING_MASK;
}

void audio_io_start() {
    for (int i = 0; i < 2 * CFG_RING_FRAMES; i++) { rx_ring[i] = 0; tx_ring[i] = 0; }
    uint off_i2s = pio_add_program(s_pio, &i2s_duplex_program);
    uint off_scki = pio_add_program(s_pio, &scki_program);
    uint sm_i2s = pio_claim_unused_sm(s_pio, true);
    uint sm_scki = pio_claim_unused_sm(s_pio, true);
    s_dma_tx = dma_claim_unused_channel(true);
    s_dma_rx = dma_claim_unused_channel(true);

    pio_sm_config c = scki_program_get_default_config(off_scki);
    sm_config_set_set_pins(&c, PIN_SCKI, 1);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    pio_gpio_init(s_pio, PIN_SCKI);
    pio_sm_set_consecutive_pindirs(s_pio, sm_scki, PIN_SCKI, 1, true);
    pio_sm_init(s_pio, sm_scki, off_scki, &c);

    c = i2s_duplex_program_get_default_config(off_i2s);
    sm_config_set_out_pins(&c, PIN_DIN, 1);
    sm_config_set_in_pins(&c, PIN_DOUT);
    sm_config_set_sideset_pins(&c, PIN_LRCK);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_NONE);
    sm_config_set_clkdiv_int_frac8(&c, CFG_FS_DIV, 0);
    pio_gpio_init(s_pio, PIN_DIN);
    pio_gpio_init(s_pio, PIN_LRCK);
    pio_gpio_init(s_pio, PIN_BCK);
    gpio_init(PIN_DOUT);
    gpio_set_dir(PIN_DOUT, false);
    gpio_disable_pulls(PIN_DOUT);
    pio_sm_set_consecutive_pindirs(s_pio, sm_i2s, PIN_LRCK, 2, true);
    pio_sm_set_consecutive_pindirs(s_pio, sm_i2s, PIN_DIN, 1, true);
    pio_sm_set_consecutive_pindirs(s_pio, sm_i2s, PIN_DOUT, 1, false);
    pio_sm_init(s_pio, sm_i2s, off_i2s + i2s_duplex_offset_entry_point, &c);

    dma_channel_config d = dma_channel_get_default_config(s_dma_tx);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_ring(&d, false, ring_bits(RING_BYTES));
    channel_config_set_dreq(&d, pio_get_dreq(s_pio, sm_i2s, true));
    dma_channel_configure(s_dma_tx, &d, &s_pio->txf[sm_i2s], (const void *)tx_ring,
                          dma_encode_endless_transfer_count(), true);

    d = dma_channel_get_default_config(s_dma_rx);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, ring_bits(RING_BYTES));
    channel_config_set_dreq(&d, pio_get_dreq(s_pio, sm_i2s, false));
    dma_channel_configure(s_dma_rx, &d, (void *)rx_ring, &s_pio->rxf[sm_i2s],
                          dma_encode_endless_transfer_count(), true);

    busy_wait_us(10);
    pio_enable_sm_mask_in_sync(s_pio, (1u << sm_i2s) | (1u << sm_scki));
}
