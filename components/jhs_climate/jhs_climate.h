#pragma once
#include <esp32-hal-log.h>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/gpio.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/wifi/wifi_component.h"

#include "esp32-hal-rmt.h"
#include "soc/rmt_struct.h"
#include "jhs_packets.h"
#include <vector>


namespace esphome
{
namespace JHSClimate
{

class JHSClimate : public esphome::Component, public esphome::climate::Climate
{
public:
    // pin setters
    void set_ac_tx_pin(esphome::InternalGPIOPin *ac_tx_pin) { ac_tx_pin_ = ac_tx_pin; }
    void set_ac_rx_pin(esphome::InternalGPIOPin *ac_rx_pin) { ac_rx_pin_ = ac_rx_pin; }
    void set_panel_tx_pin(esphome::InternalGPIOPin *panel_tx_pin) { panel_tx_pin_ = panel_tx_pin; }
    void set_panel_rx_pin(esphome::InternalGPIOPin *panel_rx_pin) { panel_rx_pin_ = panel_rx_pin; }

    void set_water_full_sensor(esphome::binary_sensor::BinarySensor *water_full_sensor_) { water_full_sensor  = water_full_sensor_; }

    // esphome handlers
    void setup() override;
    void dump_config() override;
    esphome::climate::ClimateTraits traits() override;
    void control(const esphome::climate::ClimateCall &call) override;
    void loop() override;

protected:
    esphome::InternalGPIOPin *ac_tx_pin_;
    esphome::InternalGPIOPin *ac_rx_pin_;
    esphome::InternalGPIOPin *panel_tx_pin_;
    esphome::InternalGPIOPin *panel_rx_pin_;
    esphome::binary_sensor::BinarySensor *water_full_sensor;
    // esphome::ota::OTAComponent *OTAComponent =

    int rmt_ac_tx;
    int rmt_panel_tx;

    uint32_t last_adjustment = 0;
    const int ADJUSTMENT_INTERVAL = 100;
    int steps_left_to_adjust_mode = 0;
    int steps_left_to_adjust_temp = 0;
    int steps_left_to_adjust_fan = 0;
    // FAN needs a slower retry cadence than the other adjustments: a press
    // spaced only ADJUSTMENT_INTERVAL (100ms) after the previous one doesn't
    // reliably register as a second, distinct press to the AC (confirmed via
    // capture — a 150ms gap wasn't enough either). Retried independently of
    // last_adjustment/ADJUSTMENT_INTERVAL until latched_fan_mode confirms
    // convergence or the budget below runs out.
    uint32_t last_fan_adjustment = 0;
    const int FAN_ADJUSTMENT_INTERVAL = 500;
    bool adjust_preset = false;
    // Mode from the most recent non-ambiguous AC packet (a lit display, or an
    // all-zero packet that carried the power-off beep). Updated on every such
    // packet, including mid-adjustment, so it always reflects the AC's real
    // state as of the last time we could actually see it. Used in place of
    // the packet's own reading while the display is asleep.
    esphome::climate::ClimateMode last_confirmed_mode = esphome::climate::CLIMATE_MODE_OFF;
    // Same idea for the sleep preset: packet.sleep reads 0 while asleep.
    esphome::climate::ClimatePreset last_confirmed_preset = esphome::climate::CLIMATE_PRESET_NONE;
    bool water_full = false;
    uint32_t last_water_full = 0;
    const int WATER_FULL_INTERVAL = 3000;
    esphome::climate::ClimateFanMode latched_fan_mode = esphome::climate::CLIMATE_FAN_LOW;

    // Link watchdogs. Since both wires are cut with the ESP in the middle, a
    // packet we fail to decode is a packet the other side never receives.
    // While the AC is off it only talks every ~10s, so losing one or two in a
    // row leaves the panel silent long enough to raise E3. If a direction goes
    // quiet longer than its normal cadence, re-send the last good frame
    // (AC -> panel) or a keepalive (panel -> AC) to fill the gap.
    uint32_t last_sent_to_panel = 0;
    uint32_t last_sent_to_ac = 0;
    bool have_last_ac_packet = false;
    JHSAcPacket last_ac_packet;
    const uint32_t AC_TO_PANEL_WATCHDOG = 11000;  // AC cadence when off: ~10s
    const uint32_t PANEL_TO_AC_WATCHDOG = 5000;   // panel keepalive: ~3.9s
    void check_link_watchdogs();

    // is_adjusting is set to true when a change was made externally (e.g. homeassistant) and we are in the process of pressing button
    bool is_adjusting();

    float rmt_panel_tx_tick;
    float rmt_ac_tx_tick;

private:
    // setup helpers
    void setup_rmt();

    void send_rmt_data(int rmt, std::vector<uint8_t> data);

    void recv_from_panel();

    void recv_from_ac();

    void update_screen_if_needed();
};
}
}