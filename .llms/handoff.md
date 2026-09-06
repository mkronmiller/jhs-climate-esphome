# jhs-climate-esphome — GE/JHS-A019 port: context handoff

Fork of `alufers/jhs-climate-esphome`, adapted for a **GE-branded portable AC**
built by JHS. Display board silkscreen: `JHS-A019-LED-D13`. Panel MCU is an
ABOV MC96F62xx. Running on an **ESP32 DevKit V1** (dual-core, Arduino framework)
under ESPHome 2026.8.2.

Status: **working**. Mode, target temperature, and fan speed are all controllable
from Home Assistant, and state reads back correctly.

---

## Protocol summary

Not UART. Pulse-width encoded on two single wires, decoded by a falling-edge ISR
measuring gaps in ~250µs units (`jhs_recv_task.cpp`), transmitted via the ESP32
RMT peripheral at a 2500ns tick (400kHz).

- **AC → panel**: 9 bytes. `addr(0x90)`, two seven-segment digit bytes, two zero
  bytes, two status-bit bytes, a beep byte, then checksum.
- **Panel → AC**: 3 bytes. `0x30`, button code, checksum.
- **Checksum** (both directions): `0x5A + sum of preceding bytes`, truncated to 8 bits.
- Panel sends a keepalive every ~3.9s. If the mainboard stops receiving it, the
  unit drops to a blank/off state.
- When the AC is off, packets slow from ~10/sec to ~1 per 10 seconds and read
  `9000000000000000ea` (all fields zero).

## Wiring

Both ribbon conductors are **cut**, with the ESP in the middle of each.

| ESP pin | Config name | Wire | Interface |
|---|---|---|---|
| GPIO25 | `ac_rx_pin` | display line, mainboard side | BSS138 level shifter |
| GPIO32 | `panel_tx_pin` | display line, panel side | BSS138 level shifter |
| GPIO33 | `panel_rx_pin` | key line, panel side | **1K/2K divider, NOT a shifter** |
| GPIO26 | `ac_tx_pin` | key line, mainboard side | BSS138 level shifter |

Power: ribbon 5V → DevKit VIN, common ground.

**The divider on GPIO33 is important.** The README's suggested 2K pulldown fights
a level shifter's internal pull-ups; that combination caused each key press to be
transmitted ~10 times, which the mainboard acted on. A `1K` from the line to the
pin and `2K` from the pin to ground gives 3.33V logic levels *and* serves as the
pulldown, with nothing to fight. Do not put a shifter channel on this line.

---

## Unit-specific deviations from upstream

### Button codes (`jhs_packets.h`)
This panel uses a contiguous `0x0A`–`0x10` range, not upstream's scattered `0x01`–`0x09`.

```
KEEPALIVE   0x30 0x00 0x8a
FAN         0x30 0x0a 0x94
UP          0x30 0x0b 0x95
MODE        0x30 0x0c 0x96
SLEEP       0x30 0x0d 0x97
DOWN        0x30 0x0e 0x98
TIMER       0x30 0x0f 0x99
POWER       0x30 0x10 0x9a
UNIT_CHANGE 0x30 0x09 0x93   (unverified, inherited from upstream)
```

`0x30 0x05 0x8f` has been observed once with a valid checksum — an unmapped
panel event, purpose unknown.

### Status bits
`fan` and `dehum` are **swapped** relative to upstream and have been exchanged in
the struct.

`water_full` and `timer` were also swapped relative to upstream. Confirmed
2026-09-04 in two modes: shorting/opening the float switch in dehumidify mode
produced a bursty on/off toggle of upstream's `timer` bit (byte 5 bit 7, i.e.
`0x80`) — consistent with a float switch physically bouncing — while no TIMER
button was pressed and upstream's `water_full` bit (byte 5 bit 5, `0x20`)
never moved. Repeated in cool mode after swapping the struct: the same bit
came with a simultaneous beep packet (a real full-tank alert), and the
`water_full` binary_sensor published ON correctly. Exchanged in the struct
to match; considered resolved.

`fan_low` / `fan_high` are **never set** on this unit. Fan speed appears only as
display digits `"F1"` / `"F2"` during the menu flash, so it is latched into
`latched_fan_mode` whenever `first_digit == 0x71` ('F'). Consequence: fan speed is
unknown at boot until the speed is displayed once.

Byte 6 bit 3 (upstream calls it `wifi`) is set in cool mode and clear in fan mode.
Actual meaning unknown.

### Display sleep vs. real power-off
This panel dims/blanks its display after roughly 40–60s of no button
interaction (a power-saving feature — quiet at night in a bedroom — not
related to this mod). While asleep, the AC broadcasts the exact same all-zero
packet (`9000000000000000ea`) documented above for "AC off", **including a
cleared `power` bit** — indistinguishable at the byte level from a genuine
power-off.

Confirmed 2026-09-05: waking the display (one physical POWER press while
asleep) produced a packet showing the AC's true, unchanged state (still
`COOL`, same target temp, as if nothing had happened) — proof the compressor
had kept running the whole time the display was dark. A second press actually
turned the AC off, and *that* packet — otherwise identical, all-zero — carried
a beep (`beep_amount`/`beep_length` both nonzero). Every "just sleeping"
capture across three separate logs had `beep_amount == 0`.

So the beep is the only distinguishing signal: a real state transition (power
off, mode change, the display waking up) always arrives with one; the display
timing out on its own never does. Fixed in `recv_from_ac()` by capturing
`packet.beep_amount`/`beep_length` before the `is_adjusting()` mutation can
zero them, and refusing to downgrade `mode` (and `preset`, which has the same
issue via `packet.sleep`) to what the all-zero packet implies unless that
beep is present. Not yet re-tested after the fix (needs another display-sleep
cycle to confirm the climate entity now holds its last real mode/preset
through the blank period, and still flips to OFF/NONE the moment a real
beep-confirmed off packet arrives).

Also observed a physical POWER button code this fork hadn't seen before —
`0x30 0x03 0x8d` — sent for *both* the wake press and the actual toggle-off
press while the display was asleep, distinct from the `0x30 0x10 0x9a` this
fork already maps to `POWER`/`BUTTON_ON` (which was established with the
display already awake). The panel's own MCU appears to decide whether a given
POWER press is a "wake" or a "toggle" internally — the wire code alone doesn't
distinguish them. Not currently handled specially; both arrive as "unknown
packet from panel" today and are forwarded to the AC unmodified, which is
harmless since the AC's own logic (not ours) is what decides what the press
means.

**Confirmed 2026-09-05 (same day) the display-sleep ambiguity also broke the
adjustment loop**, not just state readback. Requesting a new target
temperature landed on a silent all-zero packet: `mode_from_packet` read `OFF`,
the `steps_left_to_adjust_temp` block (TODO #1's cool-mode lockout) mistook
that for "genuinely not in cool mode" and cancelled the whole adjustment
before sending a single button — confirmed in the log by the target
snapping back to the old value within ~200ms with zero
`Sending BUTTON_HIGHER_TEMP`/`LOWER_TEMP` lines anywhere nearby. A retry after
the display had woken back up worked immediately.

Fixed by also allowing the send when `display_asleep` is true (only a
*confirmed* non-cool mode reading now cancels the adjustment). Since
`packet.get_temp()` reads `-1` while asleep, the existing higher/lower
comparison naturally always picks `BUTTON_HIGHER_TEMP` in that case — that's
fine, it's just a wake nudge; the real digits (and correct direction) show up
on the next packet once the AC responds.

The mode-adjustment block had a related but more dangerous version of the
same bug: it picks `BUTTON_ON` (a power *toggle*, not "turn on") whenever
`mode_from_packet == OFF`, which an asleep-but-still-running unit also
satisfies — meaning a mode change requested at the wrong moment could have
sent a power-off to a unit that was actually on. First fix attempt (requiring
`!display_asleep` before trusting the OFF reading) overcorrected and broke
turning the unit **on** from a genuinely off state: a real off, once past its
one beep-carrying transition packet, is *just as silent and ambiguous* as a
sleeping display — `display_asleep` can't tell them apart from a single
packet alone, since the beep only marks the transition, not the ongoing
state. Confirmed 2026-09-05: turning on from OFF looped `Sending BUTTON_MODE`
every ~10s forever (MODE presumably no-ops while genuinely off), never
sending `BUTTON_ON`.

Properly fixed by adding `mode_before_adjustment` (`jhs_climate.h`/`.cpp`):
`control()` now snapshots `this->mode` into it *before* overwriting `mode`
with the requested target. That's the last mode actually confirmed before
this adjustment began, and it's what resolves the ambiguity: if it was OFF,
an ambiguous reading during the adjustment is still assumed OFF (nothing
since has un-confirmed it) and `BUTTON_ON` is sent; if it was a real running
mode, ambiguous stays non-committal and falls through to `BUTTON_MODE`
instead. Both directions confirmed by log evidence; not yet tested together
in one session (turn on from real off, then request a mode change while
genuinely asleep-but-running, back to back).

**Not yet fixed**, and lower priority since it needs `adjust_preset` to be
in-flight at the exact moment the display is asleep: the sleep-preset block
reads `packet.sleep` the same way and could send a spurious/wrong-direction
`BUTTON_SLEEP` when ambiguous. Unlike the temp/mode fixes, there's no safe
default here — `BUTTON_SLEEP` toggles, so guessing wrong is just as bad as
not sending anything, and not sending anything risks stalling forever if
nothing else happens to wake the display. Needs a real capture before
deciding how to handle it.

### Temperature units
The unit displays Fahrenheit; ESPHome climate is Celsius internally. `f_to_c()` /
`c_to_f()` helpers convert on read, and all comparisons in the adjustment loop are
done in Fahrenheit. Visual range 16–30°C, step 0.5.

### Fan speed requires a double press
One press only *displays* the current speed; a second press while the menu is open
changes it. Currently handled with two `send_rmt_data` calls separated by a
blocking `delay(150)`.

---

## Migration notes (upstream was ~3 years stale)

- `climate.CLIMATE_SCHEMA` → `climate.climate_schema()`; `new_Pvariable` +
  `register_climate` → `climate.new_climate()`
- Class must be declared with `climate.Climate` in `__init__.py` to match the C++
- Arduino-ESP32 3.x RMT API: `rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_3, 400000)`,
  `rmtSetEOT(pin, 1)` replacing the `RMT.conf_ch[i].conf1.idle_out_lv` register write,
  and pin-based `rmtWrite`. Channels are `int` pin numbers, not `rmt_obj_t*`.
- Removed: `#include "esphome.h"`, `#include ".../ota_component.h"`,
  `traits.set_supports_current_temperature(true)`
- Framework **must** be `arduino` — the code uses `pinMode`, `attachInterrupt`,
  `micros`, and `esp32-hal-rmt.h`.
- Set `refresh: 0s` on the `external_components` source or edits will not be picked up.

---

## TODO

1. **Lock out temperature adjustment outside cool mode.** In the
   `steps_left_to_adjust_temp` block (the one containing `BUTTON_LOWER_TEMP`), add
   `&& mode_from_packet == CLIMATE_MODE_COOL`, with an `else` that zeroes the
   counter. Without this, requesting a temperature in fan mode fires all 24 button
   presses and never converges, because there is no setpoint on the display to
   match. HA's climate card does not reliably hide the control.
2. **Replace the `delay(150)` fan double-press** with a non-blocking state machine.
3. **Ignore digits while the timer is displayed** — gate the temperature read on
   `!packet.timer`, or a timer countdown is read as a setpoint.
4. ~~**Water-full bit is unverified.**~~ Resolved 2026-09-04 — see "Status bits"
   above: it was upstream's `timer` bit all along (fan mode showed no change
   because the wrong bit was being watched). Confirmed in both dehumidify and
   cool mode, the latter with a correlated beep packet and the `water_full`
   binary_sensor publishing ON. Struct updated.
5. **Identify panel code `0x05`** and byte 6 bit 3.
6. ~~**Comment out** the raw `AC packet:` debug log~~ Done 2026-09-04 — commented
   out in `recv_from_ac()`; uncomment when capturing packets for debugging.
7. **README rewrite** for the repo: model, board revision, wiring diagram, and
   instructions for capturing button codes on other units, since they vary.

## Debugging technique that works

Log raw packets as hex in `recv_from_ac()`, then diff steady-state captures before
and after changing one thing on the unit. Every finding above came from that —
including the fan-speed-in-the-digits discovery, which was invisible in the status
bits.