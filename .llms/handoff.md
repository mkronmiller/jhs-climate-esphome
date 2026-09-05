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