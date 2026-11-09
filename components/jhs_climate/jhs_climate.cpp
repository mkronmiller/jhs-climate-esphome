#include "jhs_climate.h"

#include "esphome/core/log.h"
#include "esp32-hal-rmt.h"

#include "jhs_recv_task.h"
#include "esp32-hal.h"

#include <sstream>
#include <iomanip>


static const char *TAG = "JHSClimate";

#include <cmath>
static inline float f_to_c(int f) { return (f - 32) * 5.0f / 9.0f; }
static inline int c_to_f(float c) { return (int) lroundf(c * 9.0f / 5.0f + 32.0f); }

namespace esphome
{
namespace JHSClimate
{
static std::string bytes_to_hex2(std::vector<uint8_t> bytes)
{
    std::stringstream ss;
    for (auto b : bytes)
    {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    }
    return ss.str();
}

void JHSClimate::setup()
{
    ESP_LOGI(TAG, "Setting up JHSClimate...");
    this->setup_rmt();
    jhs_recv_task_config recv_config = {
        .ac_rx_pin = this->ac_rx_pin_->get_pin(),
        .panel_rx_pin = this->panel_rx_pin_->get_pin()};
    start_jhs_climate_recv_task(recv_config);
    ESP_LOGI(TAG, "JHSClimate setup complete");

    // send hello packet to panel
    JHSAcPacket hello_packet;
    hello_packet.beep_amount = 3;
    hello_packet.beep_length = 1;
    hello_packet.set_display("dd");
    this->send_rmt_data(this->rmt_panel_tx, hello_packet.to_wire_format());
    // auto ota = esphome::App.get_component<ota::OTAComponent>("ota");
    // OTAComponent->add_on_state_callback([this](esphome::ota::OTAState state, float progress, uint8_t error) {
    //   if (state == esphome::ota::OTA_IN_PROGRESS) {
    //     ESP_LOGD(TAG, "OTA in progress %f", progress);
    //     JHSAcPacket ota_progress_packet;    
    //     // Display progress on panel
    //     ota_progress_packet.set_temp(int(progress));
    //     ota_progress_packet.wifi = 1;
    //     ota_progress_packet.unused_above_timer = 1;
    //     this->send_rmt_data(this->rmt_panel_tx, ota_progress_packet.to_wire_format());
    //   }
    // });

}

void JHSClimate::setup_rmt()
{
    this->rmt_panel_tx = this->panel_tx_pin_->get_pin();
    rmtInit(this->rmt_panel_tx, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_3, 400000);
    rmtSetEOT(this->rmt_panel_tx, 1);
    this->rmt_panel_tx_tick = 2500;
    ESP_LOGI(TAG, "RMT panel tx tick: %f", this->rmt_panel_tx_tick);

    this->rmt_ac_tx = this->ac_tx_pin_->get_pin();
    rmtInit(this->rmt_ac_tx, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_3, 400000);
    rmtSetEOT(this->rmt_ac_tx, 1);
    this->rmt_ac_tx_tick = 2500;
    ESP_LOGI(TAG, "RMT ac tx tick: %f", this->rmt_ac_tx_tick);

    ESP_LOGI(TAG, "RMT initialized");
}

void JHSClimate::control(const esphome::climate::ClimateCall &call)
{
    if (call.get_target_temperature().has_value())
    {
        this->target_temperature = call.get_target_temperature().value();
        this->steps_left_to_adjust_temp = 24;
    }
    if (call.get_mode().has_value())
    {
        this->mode = call.get_mode().value();
        this->steps_left_to_adjust_mode = 8;
    }
    if (call.get_fan_mode().has_value())
    {
        this->fan_mode = call.get_fan_mode().value();
        // A retry budget, not a single-shot attempt: each retry is spaced
        // FAN_ADJUSTMENT_INTERVAL apart (see recv_from_ac()) and only fires
        // while latched_fan_mode still disagrees, so this is a worst-case
        // ceiling (~3s at the current interval), not something normally
        // fully spent.
        this->steps_left_to_adjust_fan = 6;
    }
    if (call.get_preset().has_value())
    {
        this->preset = call.get_preset().value();
        this->adjust_preset = true;
    }
    this->publish_state();
}

esphome::climate::ClimateTraits JHSClimate::traits()
{
    // The capabilities of the climate device
    auto traits = esphome::climate::ClimateTraits();
    traits.set_supported_modes({esphome::climate::CLIMATE_MODE_OFF,
                                esphome::climate::CLIMATE_MODE_DRY,
                                esphome::climate::CLIMATE_MODE_COOL,
                                esphome::climate::CLIMATE_MODE_FAN_ONLY});
    traits.set_supported_fan_modes({esphome::climate::CLIMATE_FAN_LOW,
                                    esphome::climate::CLIMATE_FAN_HIGH});
    traits.set_supported_presets({esphome::climate::CLIMATE_PRESET_NONE,
                                  esphome::climate::CLIMATE_PRESET_SLEEP});
    traits.set_visual_min_temperature(16);    // ~61F
    traits.set_visual_max_temperature(30);    // ~86F
    traits.set_visual_temperature_step(0.5555556);
    return traits;
}

void JHSClimate::dump_config()
{
    ESP_LOGCONFIG(TAG, "JHSClimate:");
    LOG_PIN("  AC TX Pin: ", this->ac_tx_pin_);
    LOG_PIN("  AC RX Pin: ", this->ac_rx_pin_);
    LOG_PIN("  Panel TX Pin: ", this->panel_tx_pin_);
    LOG_PIN("  Panel RX Pin: ", this->panel_rx_pin_);
    ESP_LOGCONFIG(TAG, "  RMT panel tx tick: %f", this->rmt_panel_tx_tick);
    ESP_LOGCONFIG(TAG, "  RMT ac tx tick: %f", this->rmt_ac_tx_tick);
}

void JHSClimate::loop()
{
    this->recv_from_ac();
    this->recv_from_panel();
    this->check_link_watchdogs();
}

// Panel -> AC frames are 0x30, code, checksum (0x5A + sum of preceding bytes).
static bool is_valid_panel_packet(const uint8_t *p)
{
    return p[0] == 0x30 && (uint8_t)(0x5A + p[0] + p[1]) == p[2];
}

void JHSClimate::check_link_watchdogs()
{
    uint32_t now = esphome::millis();
    if (this->have_last_ac_packet && now - this->last_sent_to_panel > AC_TO_PANEL_WATCHDOG)
    {
        // Repeat the last good state so the panel doesn't time out. Never
        // repeat a beep — it already sounded the first time.
        JHSAcPacket repeat = this->last_ac_packet;
        repeat.beep_amount = 0;
        repeat.beep_length = 0;
        ESP_LOGW(TAG, "No valid AC packet for %ums, repeating last one to panel", now - this->last_sent_to_panel);
        this->send_rmt_data(this->rmt_panel_tx, repeat.to_wire_format());
        this->last_sent_to_panel = now;
    }
    if (this->last_sent_to_ac != 0 && now - this->last_sent_to_ac > PANEL_TO_AC_WATCHDOG)
    {
        ESP_LOGW(TAG, "No valid panel packet for %ums, sending keepalive to AC", now - this->last_sent_to_ac);
        std::vector<uint8_t> keepalive(KEEPALIVE_PACKET.begin(), KEEPALIVE_PACKET.end());
        this->send_rmt_data(this->rmt_ac_tx, keepalive);
        this->last_sent_to_ac = now;
    }
}


void JHSClimate::recv_from_panel()
{
    uint8_t packet[JHS_PANEL_PACKET_SIZE];
    while (xQueueReceive(panel_rx_queue, &packet, 0))
    {
        std::vector<uint8_t> packet_vector(packet, packet + JHS_PANEL_PACKET_SIZE);

        if (!is_valid_panel_packet(packet))
        {
            // A bit got flipped on the wire (e.g. 30 00 92 / 31 00 8a, one or
            // two bits off a keepalive). Forwarding it gives the AC a frame
            // that fails its checksum. Nearly every panel frame is a
            // keepalive, so send a clean one in its place. If it really was a
            // button press, that press is lost and the user presses again.
            ESP_LOGW(TAG, "Corrupt packet from panel: %s, sending keepalive instead", bytes_to_hex2(packet_vector).c_str());
            packet_vector.assign(KEEPALIVE_PACKET.begin(), KEEPALIVE_PACKET.end());
            this->send_rmt_data(this->rmt_ac_tx, packet_vector);
            this->last_sent_to_ac = esphome::millis();
            continue;
        }

        if (memcmp(packet, &KEEPALIVE_PACKET, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGVV(TAG, "Received keepalive packet from panel");
        }
        else if (memcmp(packet, &BUTTON_ON, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_ON from panel");
        }
        else if (memcmp(packet, &BUTTON_LOWER_TEMP, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_LOWER_TEMP from panel");
        }
        else if (memcmp(packet, &BUTTON_HIGHER_TEMP, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_HIGHER_TEMP from panel");
        }
        else if (memcmp(packet, &BUTTON_MODE, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_MODE from panel");
        }
        else if (memcmp(packet, &BUTTON_FAN, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_FAN from panel");
        }
        else if (memcmp(packet, &BUTTON_SLEEP, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_SLEEP from panel");
        }
        else if (memcmp(packet, &BUTTON_TIMER, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_TIMER from panel");
        }
        else if (memcmp(packet, &BUTTON_UNIT_CHANGE, JHS_PANEL_PACKET_SIZE) == 0)
        {
            ESP_LOGI(TAG, "Received BUTTON_UNIT_CHANGE from panel, ignoring");
            JHSAcPacket hello_packet;
            hello_packet.beep_amount = 3;
            hello_packet.beep_length = 2;
            hello_packet.power = 0;
            hello_packet.cool = 1;
            hello_packet.set_display("dd");
            this->send_rmt_data(this->rmt_panel_tx, hello_packet.to_wire_format());
            continue;
        }
        else
        {
            ESP_LOGI(TAG, "Received unknown packet from panel: %s", bytes_to_hex2(packet_vector).c_str());
        }
        this->send_rmt_data(this->rmt_ac_tx, packet_vector);
        this->last_sent_to_ac = esphome::millis();
    }
}

bool JHSClimate::is_adjusting()
{
    return this->steps_left_to_adjust_fan > 0 || this->steps_left_to_adjust_temp > 0 || this->steps_left_to_adjust_mode > 0 || this->adjust_preset;
}

void JHSClimate::recv_from_ac()
{
    uint8_t packet[JHS_AC_PACKET_SIZE];

    while (xQueueReceive(ac_rx_queue, &packet, 0))
    {
        std::vector<uint8_t> packet_vector(packet, packet + JHS_AC_PACKET_SIZE);
        esphome::optional<JHSAcPacket> packet_optional = JHSAcPacket::parse(packet_vector);
        if (!packet_optional)
        {
            // Logged at W (was V) so a dropped frame shows up next to any E3.
            ESP_LOGW(TAG, "Invalid packet from AC: %s", bytes_to_hex2(packet_vector).c_str());
            continue;
        }
        JHSAcPacket packet = *packet_optional;
        ESP_LOGVV(TAG, "Received new packet from AC: %s", packet.to_string().c_str());
        // Prints ~10x/sec and contributes to "took a long time for an operation"
        // warnings. Comment out again once done capturing (see
        // "Adapting to a different unit" in README.md).
        ESP_LOGD(TAG, "AC packet: %s", bytes_to_hex2(packet_vector).c_str());

        // Captured before the is_adjusting() mutation below can zero it out.
        // A real state change (confirmed power-off, mode change, the display
        // waking back up) always arrives with a beep; the display blanking
        // itself after its own idle timeout (a power-saving feature, nothing
        // to do with the AC actually turning off) does not. See "Display
        // sleep vs. real power-off" in handoff.md.
        bool packet_confirmed_change = packet.beep_amount > 0 && packet.beep_length > 0;

        // Modify the packet
        packet.wifi = !wifi::global_wifi_component->is_connected();
        if (is_adjusting()){
            packet.beep_amount = 0;
            packet.beep_length = 0;
        }

        esphome::climate::ClimateMode mode_from_packet = esphome::climate::CLIMATE_MODE_OFF;
        if (packet.cool)
        {
            mode_from_packet = esphome::climate::CLIMATE_MODE_COOL;
        }
        else if (packet.heat)
        {
            mode_from_packet = esphome::climate::CLIMATE_MODE_HEAT;
        }
        else if (packet.fan)
        {
            mode_from_packet = esphome::climate::CLIMATE_MODE_FAN_ONLY;
        }
        else if (packet.dehum)
        {
            mode_from_packet = esphome::climate::CLIMATE_MODE_DRY;
        }
        // An all-zero packet is ambiguous: it's what a genuine power-off looks
        // like, but it's also what this unit sends while its display is
        // merely dimmed/asleep and the compressor is still running unchanged.
        // The two are only distinguishable by the beep: a real off arrives
        // with one, a display timeout doesn't. Don't downgrade to OFF on the
        // silent version — just leave the last known mode alone.
        bool display_asleep = mode_from_packet == esphome::climate::CLIMATE_MODE_OFF && !packet_confirmed_change;
        // Tracked before the ADJUSTMENT_INTERVAL throttle below can skip this
        // packet: the one beep-carrying packet that confirms a power-off is
        // easy to miss otherwise, and every packet after it is ambiguous again.
        if (!display_asleep)
            this->last_confirmed_mode = mode_from_packet;
        // Best knowledge of the AC's real mode right now: what the packet
        // says, unless the display is asleep and the packet can't be trusted.
        esphome::climate::ClimateMode real_mode = display_asleep ? this->last_confirmed_mode : mode_from_packet;
        // This unit never sets the fan_low/fan_high status bits. Fan speed is
        // only ever shown on the display as "F1"/"F2" during the menu flash,
        // so latch it whenever we see it.
        if (packet.first_digit == 0x71) // 'F'
        {
            if (packet.second_digit == 0x06) // '1'
                this->latched_fan_mode = esphome::climate::CLIMATE_FAN_LOW;
            else if (packet.second_digit == 0x5B) // '2'
                this->latched_fan_mode = esphome::climate::CLIMATE_FAN_HIGH;
        }
        esphome::climate::ClimateFanMode fan_from_packet = this->latched_fan_mode;
        esphome::climate::ClimatePreset preset_from_packet = esphome::climate::CLIMATE_PRESET_NONE;
        if (packet.sleep) preset_from_packet = esphome::climate::CLIMATE_PRESET_SLEEP;
        if (!display_asleep)
            this->last_confirmed_preset = preset_from_packet;
        esphome::climate::ClimatePreset real_preset = display_asleep ? this->last_confirmed_preset : preset_from_packet;

        if (!this->is_adjusting())
        {
            // if we are not adjusting anything we can copy the state from the packet to the climate

            bool did_change = false;
            // While the timer's duration-setting screen is up, the AC repurposes
            // these same two digit bytes to show the selected hour count (1, 2,
            // 3, ...) instead of the temperature — confirmed by capture: setting
            // a 4-hour timer read back as target temperature dropping to
            // f_to_c(1)..f_to_c(4) in sequence, purely from the digits, with no
            // real temperature request involved. packet.timer stays set for the
            // whole armed duration (not just the setup screen), so this also
            // means target/current temperature won't track a real digit change
            // while a timer is armed — an acceptable tradeoff against guaranteed
            // corruption every time a timer is set.
            if (!packet.timer)
            {
                if (packet.get_temp() > 0 && c_to_f(this->target_temperature) != packet.get_temp() && packet.cool)
                {
                    this->target_temperature = f_to_c(packet.get_temp());
                    did_change = true;
                }
                if (this->current_temperature != f_to_c(packet.get_temp()))
                {
                    this->current_temperature = f_to_c(packet.get_temp());
                    did_change = true;
                }
            }
            if (!display_asleep && this->mode != mode_from_packet)
            {
                this->mode = mode_from_packet;
                did_change = true;
            }
            if (this->fan_mode != fan_from_packet)
            {
                this->fan_mode = fan_from_packet;
                did_change = true;
            }
            if (!display_asleep && this->preset != preset_from_packet)
            {
                this->preset = preset_from_packet;
                did_change = true;
            }
            
            if (did_change)
            {
                this->publish_state();
            }
            if (this->water_full != packet.water_full)
            {
                if (packet.water_full){
                    last_water_full = esphome::millis();
                    this->water_full = true;
                }else{
                    if (esphome::millis() - last_water_full > WATER_FULL_INTERVAL){
                        this->water_full = false;
                    }else{
                        this->water_full = true;
                    }
                    last_water_full = esphome::millis();
                }
                this->water_full_sensor->publish_state(this->water_full);
            }
        }
        else
        {
            if (esphome::millis() - last_adjustment < ADJUSTMENT_INTERVAL)
            {
                continue;
            }
            last_adjustment = esphome::millis();
            // we are adjusting
            if (this->steps_left_to_adjust_temp > 0)
            {
                if (packet.timer)
                {
                    // The digit bytes are showing the timer's duration right now,
                    // not the real setpoint (confirmed by capture), so there's
                    // nothing valid to compare against — and pressing a temp
                    // button while the timer's own menu might still be up risks
                    // corrupting that instead. Defer rather than guess; this can
                    // stall a pending request for as long as the timer stays
                    // armed (packet.timer stays set for the whole duration, not
                    // just its setup screen), which is an accepted tradeoff for
                    // now over sending a button based on garbage digits.
                    ESP_LOGD(TAG, "Timer active, deferring temperature adjustment");
                }
                else if (mode_from_packet == esphome::climate::CLIMATE_MODE_COOL || display_asleep)
                {
                    // display_asleep means we can't actually see the current
                    // setpoint digits (packet.get_temp() reads -1), so this
                    // naturally always picks BUTTON_HIGHER_TEMP below — that's
                    // fine, it's only being sent to wake the display/AC back
                    // up so the *next* packet reveals the real digits to
                    // converge against. Confirmed non-cool modes (below) are
                    // the only case with genuinely no setpoint to aim for.
                    if (c_to_f(this->target_temperature) != packet.get_temp())
                    {
                        auto packet_to_send = BUTTON_LOWER_TEMP;
                        if (c_to_f(this->target_temperature) > packet.get_temp())
                        {
                            packet_to_send = BUTTON_HIGHER_TEMP;
                            ESP_LOGD(TAG, "Sending BUTTON_HIGHER_TEMP packet to AC");
                        }
                        else
                        {
                            ESP_LOGD(TAG, "Sending BUTTON_LOWER_TEMP packet to AC");
                        }
                        // create a vector from BUTTON_UP, which is an std::array
                        std::vector<uint8_t> packet_vector(packet_to_send.begin(), packet_to_send.end());

                        this->send_rmt_data(this->rmt_ac_tx, packet_vector);
                        this->steps_left_to_adjust_temp--;
                    }
                    else
                    {
                        this->steps_left_to_adjust_temp = 0;
                    }
                }
                else
                {
                    // No setpoint is displayed outside cool mode, so there is nothing
                    // for the button presses to converge on. Give up immediately.
                    ESP_LOGD(TAG, "Not in cool mode, skipping temperature adjustment");
                    this->steps_left_to_adjust_temp = 0;
                }
            }
            if (this->steps_left_to_adjust_fan > 0)
            {
                if (this->fan_mode != fan_from_packet)
                {
                    // Only one press per FAN_ADJUSTMENT_INTERVAL — see the
                    // member declaration for why. If it's not time yet, just
                    // wait for a later iteration rather than sending early.
                    if (esphome::millis() - this->last_fan_adjustment >= (uint32_t)FAN_ADJUSTMENT_INTERVAL)
                    {
                        auto packet_to_send = BUTTON_FAN;

                        // create a vector from BUTTON_FAN, which is an std::array
                        std::vector<uint8_t> packet_vector(packet_to_send.begin(), packet_to_send.end());
                        ESP_LOGD(TAG, "Sending BUTTON_FAN packet to AC");
                        this->send_rmt_data(this->rmt_ac_tx, packet_vector);
                        this->last_fan_adjustment = esphome::millis();
                        this->steps_left_to_adjust_fan--;
                    }
                }
                else
                {
                    this->steps_left_to_adjust_fan = 0;
                }
            }
            // Only when a preset change was actually requested, and compared
            // against real_preset: an asleep display reads sleep=0, so the old
            // unconditional `preset != preset_from_packet` check pressed SLEEP
            // during *any* adjustment while in the SLEEP preset (confirmed
            // 2026-11-08: OFF from HA sent BUTTON_SLEEP, which only woke the
            // display).
            if (this->adjust_preset)
            {
                if (this->preset != real_preset)
                {
                    auto packet_to_send = BUTTON_SLEEP;
                    // create a vector from BUTTON_SLEEP, which is an std::array
                    std::vector<uint8_t> packet_vector(packet_to_send.begin(), packet_to_send.end());
                    ESP_LOGD(TAG, "Sending BUTTON_SLEEP packet to AC");
                    this->send_rmt_data(this->rmt_ac_tx, packet_vector);
                    // BUTTON_SLEEP toggles, so it's single-shot once the
                    // display is awake. From asleep, the first press only
                    // wakes it, so keep the request open and re-check against
                    // the real reading that follows.
                    if (!display_asleep)
                        this->adjust_preset = false;
                }
                else
                {
                    this->adjust_preset = false;
                }
            }
            if (this->steps_left_to_adjust_mode > 0)
            {
                // Compare against real_mode, not mode_from_packet: an asleep
                // display reads as OFF, so a requested OFF would otherwise
                // "match" a unit that's still running and never get sent
                // (confirmed 2026-11-08: cool on → display sleep → off from
                // HA showed off but the AC kept cooling).
                if (this->mode != real_mode)
                {
                    auto packet_to_send = BUTTON_MODE;
                    // BUTTON_ON is a power toggle, not a plain "turn on" — sending
                    // it to a unit that's actually already running would turn it
                    // off. A genuinely-off unit and one that's merely asleep look
                    // identical here (both silent, all-zero, no beep — the beep
                    // only marks the transition moment, not the ongoing state),
                    // so real_mode falls back on the last confirmed reading.
                    //
                    // Turning off from asleep takes two presses: the first only
                    // wakes the display (revealing e.g. COOL, still != OFF), the
                    // second toggles power, and its beep-carrying all-zero packet
                    // confirms OFF and ends the loop.
                    if (this->mode == esphome::climate::ClimateMode::CLIMATE_MODE_OFF ||
                        real_mode == esphome::climate::ClimateMode::CLIMATE_MODE_OFF)
                    {
                        packet_to_send = BUTTON_ON;
                        ESP_LOGD(TAG, "Sending BUTTON_ON packet to AC");
                    }
                    else
                    {
                        ESP_LOGD(TAG, "Sending BUTTON_MODE packet to AC");
                    }
                    // create a vector from BUTTON_MODE, which is an std::array
                    std::vector<uint8_t> packet_vector(packet_to_send.begin(), packet_to_send.end());

                    this->send_rmt_data(this->rmt_ac_tx, packet_vector);
                    this->steps_left_to_adjust_mode--;
                }
                else
                {
                    this->steps_left_to_adjust_mode = 0;
                }
            }

        }
        this->send_rmt_data(this->rmt_panel_tx, packet.to_wire_format());
        this->last_sent_to_panel = esphome::millis();
        this->last_ac_packet = packet;
        this->have_last_ac_packet = true;
    }
}


void JHSClimate::send_rmt_data(int rmt, std::vector<uint8_t> data)
{
    ESP_LOGVV(TAG, "Sending RMT data: %s", bytes_to_hex2(data).c_str());

    std::vector<rmt_data_t> rmt_data_to_send = {};
    rmt_data_to_send.reserve((data.size() * 8) + 2); // 8 bits per byte + 2 bits for start/stop
    rmt_data_t leadin;
    leadin.level0 = 0;
    leadin.duration0 = 1800;
    leadin.level1 = 1;
    leadin.duration1 = 900;
    rmt_data_to_send.push_back(leadin);
    for (size_t i = 0; i < data.size() * 8; i++)
    {
        uint8_t bit = (data[i / 8] >> (7 - (i % 8))) & 1;

        if (bit)
        {
            rmt_data_t bit1;
            bit1.level0 = 0;
            bit1.duration0 = 100;
            bit1.level1 = 1;
            bit1.duration1 = 300;
            rmt_data_to_send.push_back(bit1);
        }
        else
        {
            rmt_data_t bit0;
            bit0.level0 = 0;
            bit0.duration0 = 100;
            bit0.level1 = 1;
            bit0.duration1 = 100;
            rmt_data_to_send.push_back(bit0);
        }
    }

    rmt_data_t leadout;
    leadout.level0 = 0;
    leadout.duration0 = 100;
    leadout.level1 = 1;
    leadout.duration1 = 100;
    rmt_data_to_send.push_back(leadout);
    rmt_data_t end;
    end.level0 = 0;
    end.duration0 = 200;
    end.level1 = 1;
    end.duration1 = 200;
    rmt_data_to_send.push_back(end);
    rmtWrite(rmt, rmt_data_to_send.data(), rmt_data_to_send.size(), RMT_WAIT_FOR_EVER);
}


} // namespace jhs
} // namespace esphome
