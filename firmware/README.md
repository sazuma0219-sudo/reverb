# REVERB firmware

Firmware for the RP2350 (Waveshare RP2350-Zero) on the REVERB board. It runs the four reverb modes (Plate / Hall / Room / Shimmer), reads the knobs, CVs and the MODE button, and has a USB serial console for testing.

- Core 1 runs only the audio: PIO + DMA I2S to the PCM1808 ADC and PCM5102A DAC, 32-frame blocks, fs = 48 828.125 Hz.
- Core 0 runs the knobs (CD4051), CVs, MODE button, LEDs, mode memory (flash) and the USB console.

See [`../spec/reverb_spec.md`](../spec/reverb_spec.md) for the design and [`../hardware/`](../hardware/) for the schematic.

## Files

| File | Contents |
|---|---|
| `reverb.ino` | Start-up, core 0 / core 1 loops |
| `config.h` | Build configuration: board revision, levels, clocks, pins |
| `engine.cpp` | Audio engine: pre-delay, mode switching, WET gain, limiter, soft clip |
| `audio_io.cpp`, `rvb_io.pio` | I2S (PIO) and DMA |
| `dsp.h` | DSP building blocks (delays, allpasses, filters, random wobble, fast math) |
| `mode_plate.cpp`, `mode_fdn.cpp` (Hall), `mode_room.cpp`, `mode_shimmer.cpp` | The four modes |
| `control.cpp` | Knobs, CVs, MODE button, LEDs |
| `mode_store.cpp` | Saves the selected mode in flash |
| `console.cpp` | USB serial console |

`rvb_io.pio.h` is generated from `rvb_io.pio` by `pioasm` (arduino-pico runs it automatically when the `.pio` changes). It is checked in.

## Requirements

- [Arduino CLI](https://arduino.github.io/arduino-cli/) or Arduino IDE 2
- The [arduino-pico](https://github.com/earlephilhower/arduino-pico) core **6.1.0** (`rp2040:rp2040`).
  Board manager URL: `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
- Board: **Waveshare RP2350 Zero**, with optimization **-O2** (`opt=Optimize2`). The default size optimization is too slow for the Shimmer mode.
- No extra libraries.

## Build and flash

```sh
arduino-cli core install rp2040:rp2040@6.1.0
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2350_zero:opt=Optimize2 firmware/reverb --output-dir build/reverb
```

- **First flash:** hold BOOT while you plug in USB. A `RP2350` drive appears; copy `build/reverb/reverb.ino.uf2` to it.
- **Later updates:** `arduino-cli upload -p <serial port> --fqbn rp2040:rp2040:waveshare_rp2350_zero:opt=Optimize2 firmware/reverb`

A Schottky diode between the AE bus 5 V and the RP2350-Zero's 5 V pin keeps the bus and USB VBUS apart.

### Board revision

`config.h` has `CFG_E003`:

- `0` (default): board **v1.0.1** and later.
- `1`: board **v1.0**, which lacks the series 10 µF capacitors (C34 / C35) in the WET output stage. The firmware then adds a DC offset to centre the WET output; the WET swing is limited to about 0.95 Vpp.

## Console

Open the USB serial port at 115200 baud. Send one command per line; every reply ends with `ok`.

| Command | Does |
|---|---|
| `help` | List the commands |
| `info` | Version, mode, knob and CV values, RT60, delay-memory use |
| `load [reset]` | Processing load per block, underruns, limiter activity, WET peak, input and reverb RMS (`reset` clears the statistics) |
| `mode <0-3\|plate\|hall\|room\|shimmer>` / `next` | Select a mode |
| `set <wet\|pre\|decay\|tone\|char> <0..1>` | Override a knob (CVs are ignored for it) |
| `default` | Clear all knob overrides. **Always send this after `set`**, or the knobs stay dead |
| `imp` → `tail` / `ir` | Impulse response: empties the reverb, fires an impulse, then `tail` prints the decay curve and RT60, `ir` prints the first 40 000 samples (int16, 32767 = 0.25 FS) |
| `tone <Hz> <ms>` | Like `imp`, with a sine burst |
| `dbg mod <0..1>` | Scale all modulation depths (0 = no wobble: the impulse response becomes repeatable) |
| `dbg lim <0/1>` | Limiter off / on |
