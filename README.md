# JHS Climate for ESPHome

> **This fork exists to make the original project work with one specific
> air conditioner:** a **GE-branded portable AC built by JHS**, with a control
> panel silkscreened `JHS-A019-LED-D13` (ABOV MC96F62xx panel MCU). If you have
> a different JHS/"Lifetime Air" unit, the wiring and general approach still
> apply, but button codes and status bits **will differ** — see
> [Adapting to a different unit](#adapting-to-a-different-unit) below.
>
> The changes in this fork (packet decoding fixes, migration to a current
> ESPHome release, new logic to work around this panel's quirks) were made
> **with the help of [Claude Code](https://claude.com/claude-code)**, an AI
> coding assistant. Review the code yourself before flashing it to your own
> unit.

Integration with [ESPHome](https://esphome.io) for portable air conditioners
made by [JHS (Dongguan Jinhongsheng Electric Co., Ltd.)](https://www.jhs8.com/).
The original upstream project ([alufers/jhs-climate-esphome](https://github.com/alufers/jhs-climate-esphome))
targets units sold by the Action chain in Poland under the "Lifetime Air"
brand. This fork adapts it for a GE-branded unit sold in the US that uses the
same panel-to-mainboard protocol with a different button/status layout.

Status: **working** on the GE/JHS-A019 unit described above. Mode, target
temperature, and fan speed are all controllable from Home Assistant, and
state (current temperature, mode, fan speed, sleep preset, water-full) reads
back correctly.

This component works only on the dual-core ESP32 (it does not work on the
ESP8266), and requires the **Arduino framework** (not ESP-IDF) — the code uses
`pinMode`, `attachInterrupt`, `micros`, and the `esp32-hal-rmt.h` RMT API.

## How it works

The ESP32 sits electrically **in the middle** of the two wires that already
run between the AC's mainboard and its control panel — it does not need any
extra power, since the panel harness already carries 5V. The integration
intercepts and re-transmits both directions of communication, reading the
mainboard's state to drive an ESPHome `climate` entity and translating
Home Assistant actions into panel button presses.

The protocol is **not UART**. It's pulse-width encoded on two single wires,
decoded by a falling-edge interrupt handler and transmitted via the ESP32's
RMT peripheral. Both wires are cut and the ESP32 is spliced into the middle
of each — see [Wiring](#wiring) below.

![An image of the AC control panel](./docs/control_panel.jpg)

## Wiring

Both ribbon conductors between the mainboard and the control panel are
**cut**, with the ESP32 in the middle of each.

| ESP32 pin | Config name    | Wire                        | Interface                        |
| --------- | -------------- | ---------------------------- | --------------------------------- |
| GPIO25    | `ac_rx_pin`     | display line, mainboard side | BSS138 level shifter              |
| GPIO32    | `panel_tx_pin`  | display line, panel side     | BSS138 level shifter              |
| GPIO33    | `panel_rx_pin`  | key line, panel side         | **1K/2K divider, NOT a shifter**  |
| GPIO26    | `ac_tx_pin`     | key line, mainboard side     | BSS138 level shifter              |

Power: ribbon 5V → ESP32 DevKit VIN, common ground.

**The divider on `panel_rx_pin` matters — read this before wiring it up.**
A plain 2K pulldown (as suggested by older revisions of this doc) fights a
level shifter's internal pull-ups; on the GE/JHS-A019 panel that combination
caused every key press to be transmitted about 10 times, and the mainboard
acted on all of them. Instead, use a resistor divider with no shifter chip on
this line at all: **1K from the panel's key line to the GPIO pin, and 2K from
the pin to ground.** That gives ~3.33V logic levels into the ESP32's input
*and* serves as the pulldown, with nothing left to fight it.

## Example configuration

```yaml
esphome:
  name: my-ac
  friendly_name: My AC

esp32:
  variant: esp32
  framework:
    type: arduino # required — the code uses pinMode/attachInterrupt/micros/RMT

external_components:
  - source:
      type: git
      url: https://github.com/mkronmiller/jhs-climate-esphome
      ref: master
    refresh: 0s # re-fetch this ref on every build instead of caching it

# jhs_climate registers itself as its own top-level component, not as a
# "climate:" platform — but it still needs these two domains present
# (even empty) so their base support gets pulled in.
climate:
binary_sensor:

jhs_climate:
  id: jhsclimate
  name: "JHS Climate"
  ac_tx_pin: 26 # data going from the ESP to the AC mainboard (key line)
  ac_rx_pin: 25 # data coming from the AC mainboard to the ESP (display line)
  panel_rx_pin: 33 # data coming from the control panel to the ESP (key line — 1K/2K divider, NOT a level shifter, see Wiring above)
  panel_tx_pin: 32 # data going from the ESP to the control panel (display line)
  water_full_sensor:
    name: "Water full"
```

If you're developing against a checkout of this repo rather than flashing
from GitHub directly, ESPHome also accepts a local source (no `ref`/`refresh`,
and no push needed before each build — but the path has to be reachable from
wherever ESPHome actually compiles, e.g. not from a separate HAOS/ESPHome
Builder add-on host):

```yaml
external_components:
  - source:
      type: local
      path: /path/to/jhs-climate-esphome/components
```

## Protocol summary

- **AC → panel**: 9 bytes: `addr (0x90)`, two seven-segment digit bytes, two
  zero bytes, two status-bit bytes, a beep byte, then a checksum.
- **Panel → AC**: 3 bytes: `0x30`, a button code, then a checksum.
- **Checksum** (both directions): `0x5A + sum of preceding bytes`, truncated
  to 8 bits.
- The panel sends a keepalive roughly every 3.9s. If the mainboard stops
  receiving it, the unit drops to a blank/off display.
- When the AC is off, packets slow from ~10/sec to about 1 per 10 seconds and
  read as all-zero fields.

### Button codes on the GE/JHS-A019 panel

This panel uses a contiguous `0x0A`–`0x10` range, unlike upstream's scattered
`0x01`–`0x09` — **button codes are not portable between panel revisions**,
see [Adapting to a different unit](#adapting-to-a-different-unit).

```
KEEPALIVE   0x30 0x00 0x8a
FAN         0x30 0x0a 0x94
UP          0x30 0x0b 0x95
MODE        0x30 0x0c 0x96
SLEEP       0x30 0x0d 0x97
DOWN        0x30 0x0e 0x98
TIMER       0x30 0x0f 0x99
POWER       0x30 0x10 0x9a
```

### Known quirks of this unit

- **Fan speed isn't in the status bits.** The `fan_low`/`fan_high` bits are
  never set; fan speed only ever appears as the display digits `"F1"`/`"F2"`
  during the menu flash after pressing the FAN button. The component latches
  this into its internal fan-speed state whenever it sees those digits, which
  means fan speed reads as unknown/stale until the speed has been displayed
  at least once after boot.
- Changing fan speed from Home Assistant requires **two** button presses
  (the first only opens/displays the menu), currently done with a blocking
  `delay(150)` between them.
- Requesting a target temperature only makes sense in cool mode — there's no
  setpoint on the display to converge on otherwise — so temperature
  adjustment is a no-op outside cool mode.
- The status bits are not all where upstream put them: `fan`/`dehum` and
  `water_full`/`timer` are each swapped relative to upstream's layout on this
  panel. Both swaps were confirmed by diffing captures (see
  [Adapting to a different unit](#adapting-to-a-different-unit)) — the
  water-full swap in particular by triggering the float switch in two
  different modes and confirming the `water_full` binary_sensor published
  correctly, with a real full-tank beep packet arriving at the same moment.
- **The display sleeps after ~40–60s idle** (dims/blanks, presumably so it's
  not glaring in a bedroom overnight) and broadcasts the exact same all-zero
  packet as a genuine power-off while asleep — compressor included, it just
  keeps running. The only reliable tell is a beep: a real power-off (or any
  other real state change) arrives with one, the display timing out on its
  own never does. The climate entity holds its last known mode/preset through
  a silent all-zero packet and only follows it to OFF/NONE once a beep
  confirms the change is real.

## Adapting to a different unit

JHS/"Lifetime Air"/GE-rebadged portable ACs appear to share this general
protocol but not necessarily the same button codes or status-bit layout.
Panels have different silkscreens and MCUs across revisions, so **capture
your own unit's traffic rather than trusting the tables above.**

The technique that works: enable verbose logging on `recv_from_ac()` /
`recv_from_panel()` (see `components/jhs_climate/jhs_climate.cpp`) so every
raw packet is logged as hex, then diff steady-state captures from your own
unit before and after changing exactly one thing (pressing one button,
switching one mode, etc.). Every unit-specific fix in this fork — including
the fan-speed-in-the-digits discovery, which is invisible in the status bits
alone — came from that process.

## Credits

Forked from [alufers/jhs-climate-esphome](https://github.com/alufers/jhs-climate-esphome).
Adapted for the GE/JHS-A019 unit, with the help of [Claude Code](https://claude.com/claude-code).
