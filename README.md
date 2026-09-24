# flipdot-stc8g

Pulse-then-coast firmware for the [F15 flip-dot display's](F15.pdf) coil stage: replaces the stock loader on the per-quad **STC8G1K08** MCU (one per quad, 16 per module). The firmware reads the four TM1824 duty-state outputs and drives the four AT8837 H-bridges with short pulses on confirmed state changes, then brakes, coasts and sleeps the bridges — the dots are magnetically bistable, so the steady current is ~zero in any image (the stock firmware holds the coils energized continuously). Firmware behavior, pin map and the tuning console are documented in the `src/main.c` header comment. The display hardware itself — modules, board architecture, measured byte semantics, closed bench questions — is documented in [HARDWARE.md](HARDWARE.md).

## MCU facts

- **STC8G1K08-20/16PIN**, TSSOP20 (stcgal: Magic F754, BSL 7.3.13U, mfg 2024-01), 18 I/O variant; 8 KB code / 1 KB xram / 256 B iram.
- **A factory ISP header (VDD / GND / RX / TX) is exposed per MCU** — no soldering. P3.0/P3.1 (TSSOP20 pins 11/12) are dedicated to ISP; the console UART shares them (the ROM bootloader owns the pins at boot, so ISP still works).
- ISP lives in ROM, starts at every power-up and waits for the host handshake. **The ROM bootloader cannot be bricked** — a bad flash is fixed by reflashing. During ISP all I/O is Hi-Z, so the bridges coast via their 100 kΩ pull-downs.
- **Read-back is impossible** on STC8 series (the BSL dropped it; see stcgal issue #7): the stock firmware cannot be dumped, and the first Erase destroys it for good. Accepted — the required behavior is fully known from the display's measurements.
- **Never set the lock/encrypt ISP options.**
- A half-flashed quad = 4 dead dots until reflashed.
- The chip's 4 KB IAP EEPROM is a candidate slot for per-quad calibration data (pulse width, bridge polarity) if quads ever need individualization.

## TSSOP20 physical pin map

Logical names are in `src/main.c`; the physical numbering for probing:

| pins | signals |
|------|---------|
| 1–4 | P1.2–P1.5 (bridges 2, 3 IN) |
| 5–6 | P1.6 / P1.7 (bridge 4 IN) |
| 7 | P5.4 (TM1824 B duty input) |
| 8 | VCC |
| 9 | P5.5 (TM1824 W duty input) |
| 10 | GND |
| 11 / 12 | P3.0 / P3.1 (ISP UART) |
| 13–18 | P3.2–P3.7 (nSLEEP 1–4, duty inputs R/G) |
| 19–20 | P1.0 / P1.1 (bridge 1 IN) |

## Toolchain and build

Proven end to end on real hardware:

- **sdcc 4.5.0** (sdcc/sdar/packihx/sdranlib)
- **FwLib_STC8** ([IOsetting/FwLib_STC8](https://github.com/IOsetting/FwLib_STC8), explicitly supports STC8G1K08; only the used modules are compiled — the full library does not fit in 8 KB)
- **stcgal** ([grigorig/stcgal](https://github.com/grigorig/stcgal), dedicated `stc8g` protocol with RC trim during the handshake)

```sh
make                        # build/quad.hex (DEPLOY_DIR=/path make also copies the hex there)
make flash PORT=/dev/ttyX   # or run stcgal on any host:
                            # stcgal -p PORT -P stc8g flipdot-stc8g-quad.hex
```

Flash procedure: **power off the module → start stcgal (it polls the header) → power on.** stcgal also reports the per-chip **UID** (usable as quad identity in logs) and the measured IRC frequency.

## Clock decision

No `SYS_SetClock()` / no IRC re-trim — the chip runs on its power-on IRC default (~24 MHz). The FwLib trim constants are per-chip; using them would be a gamble. 24 MHz → 115200 baud error ~0.2%, well inside UART tolerance.

## Bench findings baked into the firmware

- **Pulse width**: 2 ms = the dot twitches without reaching the detent; 10 ms = flips work but are marginal/weak; 40 ms = already noticeably worse; >50 ms = no further gain. An A/B (two modules running 100 ms and 50 ms side by side) showed no observable difference, so the build default stays at 100 ms.
- **Burst mode** (`N<n>`/`G<n>`) was tested as an automated fix for the failing dots and rejected: bursts don't make those dots reliable either. The knobs stay in the console, default N=1.
- **Sampling window** is hardware-timed (Timer0, exactly 4 ms = 4 PWM periods): a counted sampling loop compiled with `--model-large` took ~50 ms and phase-locked the state machine to the frame rate, starving the middle dots of each quad.
- Bench rule: test with a static frame on the ESP side ("DMX all 0x00") — a running animation re-pulses dots by duty edges right after every manual test and garbles the observations (or toggle auto mode off with `M`).
