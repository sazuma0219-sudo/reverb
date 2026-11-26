# REVERB

A stereo multi-reverb module for [AE Modular](https://www.tangiblewaves.com/) (5 V, 0–5 V CV).

- Four reverb modes, selected with the MODE button and shown on two LEDs: **Plate** (Dattorro tank), **Hall** (8-line FDN), **Room** (early reflections + small tank) and **Shimmer** (Hall with octave and fifth shifted feedback).
- The DRY signal passes through in analog at unity gain; only the WET is computed (RP2350 + PCM1808 ADC + PCM5102A DAC, fs = 48.8 kHz) and added to it.
- Outputs: L / R MIX (DRY + WET) and L / R WET.
- Knobs: WET, PRE-DELAY, DECAY, TONE, CHARACTER. CV inputs for WET, DECAY and TONE.
- A USB serial console for testing (impulse responses, load, levels).

The files are for board **v1.0.1**. The PCB layout is not published.

## Repository layout

| Folder | Contents |
|---|---|
| [`hardware/`](hardware/) | Schematic (PDF, SVG), BOM (CSV) and the EasyEDA Pro project (`.epro`) |
| [`firmware/`](firmware/) | RP2350 firmware (Arduino, arduino-pico). See [`firmware/README.md`](firmware/README.md) for build, flashing and the console. |
| [`spec/`](spec/) | Technical specification: design, design rationale and verification results. [English](spec/reverb_spec.md) / [Japanese](spec/reverb_spec.ja.md) |

## License

| Folder | License |
|---|---|
| `hardware/` | [CERN-OHL-P v2](hardware/LICENSE) |
| `firmware/` | [MIT](firmware/LICENSE) |
| `spec/` | [CC BY 4.0](spec/LICENSE) |

Copyright (c) 2026 Shota Azuma.

You may use, modify, build and sell this design, including commercially, under these licenses.
Please keep the copyright notice and credit the original.

### Brand name: Tsumiki Modular

REVERB is designed and sold by **Tsumiki Modular**.

These licenses cover the design files only. **They grant no right to use the name "Tsumiki Modular" or its logo.**
Do not sell or promote products made from this design under that name or logo, or in a way that suggests Tsumiki Modular made or endorses them.
You are free to use the name "REVERB".
