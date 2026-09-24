# Flipdot display hardware

Reverse-engineered hardware documentation for the **F15** flip-dot display module by Shenzhen Xinqidian Optoelectronics (XQD): the TM1824-driven dot modules, the board architecture, the measured byte semantics and the closed bench questions. The coil stage runs the pulse-then-coast firmware in this repo (see [README.md](README.md)).

## Hardware model

The display's block drivers are **Titan Micro TM1824** chips (SOP8, four-channel constant-current PWM drivers, datasheet V1.2), connected in series on a single data line — each chip's DOUT feeds the next chip's DIN. A module is 8x8 dots; modules are chained together the same way as the chips inside them (a module's last chip feeds the next module's first chip).

- A module holds **16 chips x 4 channels x 8 bits = 64 bytes**: exactly one byte per dot; a chain of N modules carries N x 64 bytes. Pixel-perfect addressing verified with a walking pixel (see "Byte semantics and power" for what a byte drives).
- The controller drives DIN directly with 3.3 V (e.g. an ESP32 GPIO); the datasheet Vih is 3.5 V at VDD=5 V. It works within the tolerance of real chips but is out of spec — first suspect for any unexplained glitches.
- Layout inside a module: pixels are wired as a mirrored Z — every row is scanned **right-to-left**, pixel 0 is the block's top-right corner, the last pixel is the bottom-left corner.

Bit timings, the frame structure and the protocol's electrical details are TM1824 datasheet territory; any transmitter should implement the datasheet windows.

## Board architecture (as traced)

Per-dot drive is fully individual — there is no row/column matrix. A module is built from **16 identical quads**, each owning 4 dots (16 × 4 = 64):

```
RS-485 A/B (daisy to every module) ──> TM512AC0 ──DO──(R_O?)──> DIN ─> TM1824#1 ─DOUT─> ... ─DOUT─> next module's DIN
                                                        │ 4x duty-PWM state outputs (R,G,B,W)
                                                        v
                                                      STC8G ── IN1/IN2 + nSLEEP ──> 4x AT8837 ──> 4 coils
```

- **TM1824 = chain demux, not a coil driver.** Its RGBW outputs go to the paired STC8G's input pins (traced), carrying the latched duty as a ~1 kHz PWM state signal. This is why the chain chips exist at all: single-wire position addressing is decoded in hardware, and an 8051-class MCU cannot cheaply decode an 800 kbps RZ stream in software. The whole byte-semantics model measured on this display (0x00 = off, 0xFF = full, mid = locked-antiphase chatter, see "Byte semantics and power") lives in this duty signal.
- **STC8G (8051-class MCU, one per quad)** reads the 4 state pins and drives its 4 bridges. It does **not** touch the single-wire chain (traced: confirmed). Full pin map (identical on all 16 quads of a module): TM1824 R/G/B/W → P3.6/P3.7/P5.4/P5.5 (open-drain duty outputs, read through the STC8G's internal 4 kΩ pull-ups); AT8837 IN1/IN2 → P1.0–P1.7 in bridge order; AT8837 nSLEEP (active high) → P3.2–P3.5 in bridge order; 16 of 18 I/O used, the remaining two are P3.0/P3.1 (the ISP UART).
- **AT8837 (single H-bridge per dot)** provides the coil polarity: forward / reverse / brake (coil shorted) / coast (outputs Hi-Z). Every control input has an internal pull-down: with the MCU off or in reset the bridges sit at IN1=IN2=0 = coast, coils float, dots hold magnetically — hardware fail-safe for free. Coil wiring per dot is untraced: a single bipolar winding between OUT1/OUT2 is what the part is made for; two unipolar coils (one per output) is electrically possible — open question.
- The STC8G's interpretation (polarity choice, how a black dot is driven back to white, the boot behavior) is its firmware's business and is invisible from outside. The measured boot quirk (field white *and* drawing current until the first frame arrives, see the appendix) is an active white drive state, not a passive release.

### DMX decoder: TM512AC0 (one per module, datasheet V1.5)

The single SOP8 next to the connector is a Titan Micro **TM512AC0** — a DMX512 decoder with decode-and-forward: it receives the differential bus and re-emits the captured data as a single-wire RZ stream on its DO pin, feeding the module's chain.

Addressing: the modules sit on the RS-485 bus as a daisy chain (multidrop per the RS-485 spec) — A/B runs module to module and every decoder hears every frame. The E2 address is a 0-based window start over the valid (post-start-code) fields; each decoder captures the **64-field window at its own address** and drives its module's chain with it, one byte per dot. With the factory addresses the windows are the dense 64-slot blocks (**1, 65, 129, ...** — see the vendor bus rules below).

Address writing rides the same A/B lines with a proprietary pattern (dedicated writer tool only; the datasheet does not document it). After a successful write the decoder drives the forwarded chain at 25% white — the display flashes white on re-addressing. A controller that only sends normal DMX frames with start code 0x00 cannot trigger the write mode.

Bus level detail worth keeping: the differential inputs bias a **floating bus to MARK** (measured: the negative floating-bus probe). DO → DIN link: per the TM512AC0 reference design the DO output reaches the chain input through a series resistor **R_O = 300–500 Ω** (same net as the connector's DIN pin). Accepted as the working explanation, not verified — and the reason a continuity check between DO and DIN fails: measure ohms, not beep.

Vendor bus rules (the vendor doc is thin, treat with care): one module occupies **64 universe channels, one channel per dot**; 8 modules max on one line (8 x 64 = 512 slots — a full universe offset), so factory base addresses are dense: **1, 65, 129, ...**.

## Byte semantics and power

The TM1824 chip itself only sets a duty per channel (open-drain output, 1 kHz PWM, "all zeros = channel off"); the coil polarity / bridge logic lives in the STC8G + AT8837 stage (see "Board architecture") and was mapped empirically. On this board the net behavior is that of an H-bridge in locked-antiphase PWM: the byte is a duty `d = byte/255`, the coil is driven toward **black during `d` of each PWM period and toward white during the rest** (net force toward black = `2d-1`), and it keeps doing this for as long as the value is latched:

- `0x00` — special-cased "all off": coils released, detent holds the dots, no significant current. A field of any other value costs current (measured: 0x01 eats nearly as much as 0xFF; mid-range values eat MORE than 0xFF — reversal spikes), so free constant-white is only the exact-0x00 latch.
- `0x01`–`0x65`: the net force is fully toward white — the dots flip white, but the drive continues (~full current, faint hum/chatter toward the top of this zone).
- Mid-range (`~0x80`–`0xC0`): net force hovers at zero; every dot is battered in both directions each period and flips black↔white asynchronously, forever. Maximum current draw. It never settles, is independent of the value's history (no hysteresis) and is not affected by repeated identical frames.
- `0xFE`–`0xFF`: net force fully toward black — the dots flip black and hold; constant current, faint magnetostriction hum.

There is **no sub-threshold / "park" byte** (hysteresis scans, repeat-spam probes and long idle all tested, all negative), and the wire idle level is irrelevant. Every transmitted frame drives every dot to the value's end of the scale first (dots flip to whichever end the duty is closer to) and then the drive continues per the rule above.

Power consequences, settled by measurement:

- **The only free state is an all-0x00 (white) frame.** Every nonzero byte keeps the coil stage energized. Current is paid for by every dot latched at a nonzero byte, and mid-range bytes cost the most.
- Between frames the wire is silent (the driver transmits only on change), but the latched coil drive keeps running — the current draw after a frame is set by that frame's contents, not by the wire.
- The accepted palette (classic look): dark background latched at 0xFF (paying), light content at the release byte — white background with black content would be free, but the light-on-dark look is the wanted one. Measurement caveat: all current readings were taken on the module's shared PSU, which also fed the ESP32 and the buck converters — "zero current" means "no significant coil-side draw".

## Appendix: closed questions

Investigated on the bench and settled; kept here so nobody re-opens them. Unless noted, "current" means coil-side draw (see the measurement caveat in "Byte semantics and power").

- **No "hold without driving" byte.** Any nonzero value drives the coil continuously; pulse-then-release and parking schemes were tested and closed (descending scans from black, repeated identical frames, an hour of idle — the driver never sleeps). The bistability the dots physically have is used by the STC8G firmware (pulse-then-coast), not by the wire protocol.
- **DMX512 mode adds no bistability.** Whatever the decoder outputs ends up latching duty in the TM1824 chain the same way as the single-wire protocol; a black field held through DMX draws the same as through DIN. Implemented and verified working.
- **No release-on-signal-loss.** The decoder holds its data indefinitely, no fail-safe release. Probes, all negative (current unchanged, picture unchanged): plain idle for minutes, forced breaks far beyond spec (up to 1 s), floating bus (transceiver released — the closest emulation of the cable pulled).
- **Power switching (module rail cut) rejected.** With the rail off the magnetic picture holds indefinitely, but every power-up resets the chips to the all-zero (release) state, letting the field drift to white during rail settle — each cycle produced a white→black→image flash, and flashes of any frequency were unacceptable. The rail stays on permanently.
- **Boot quirk, accepted:** with the board powered but no frame ever received, the field shows white and draws current (chip power-on default differs from a released-coils state); the first transmitted frame clears it.
- **Debug probes removed from the display driver** (the former bench API: uniform frames, long breaks, bus off/on, pin parking). The git history keeps them for any future bench work.
