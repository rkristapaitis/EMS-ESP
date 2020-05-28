/*
 * EMS-ESP - https://github.com/proddy/EMS-ESP
 * Copyright 2019  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mixing.h"

namespace emsesp {

REGISTER_FACTORY(Mixing, EMSdevice::DeviceType::MIXING);
MAKE_PSTR(logger_name, "mixing")
uuid::log::Logger Mixing::logger_{F_(logger_name), uuid::log::Facility::CONSOLE};

Mixing::Mixing(uint8_t device_type, uint8_t device_id, uint8_t product_id, const std::string & version, const std::string & name, uint8_t flags, uint8_t brand)
    : EMSdevice(device_type, device_id, product_id, version, name, flags, brand) {
    LOG_DEBUG(F("Registering new Mixing module with device ID 0x%02X"), device_id);

    if (flags == EMSdevice::EMS_DEVICE_FLAG_MMPLUS) {
        if (device_id < 0x28) {
            // telegram handlers 0x20 - 0x27 for HC
            register_telegram_type(device_id -0x20 + 0x02D7, F("MMPLUSStatusMessage_HC"), true, std::bind(&Mixing::process_MMPLUSStatusMessage_HC, this, _1));
        } else {
            // telegram handlers for warm water/DHW 0x28, 0x29
            register_telegram_type(device_id - 0x28 + 0x0331, F("MMPLUSStatusMessage_WWC"), true, std::bind(&Mixing::process_MMPLUSStatusMessage_WWC, this, _1));
        }
    }
    // EMS 1.0
    if (flags == EMSdevice::EMS_DEVICE_FLAG_MM10) {
        register_telegram_type(0x00AA, F("MMConfigMessage"), false, nullptr);
        register_telegram_type(0x00AB, F("MMStatusMessage"), true, std::bind(&Mixing::process_MMStatusMessage, this, _1));
        register_telegram_type(0x00AC, F("MMSetMessage"), false, nullptr);
    }
    Settings settings;
    mqtt_format_ = settings.mqtt_format(); // single, nested or ha

    // MQTT callbacks
    // register_mqtt_topic("cmd", std::bind(&Mixing::cmd, this, _1));
}

// add context submenu
void Mixing::add_context_menu() {
}

// display all values into the shell console
void Mixing::show_values(uuid::console::Shell & shell) {
    EMSdevice::show_values(shell); // always call this to show header

    if (type_ == Type::NONE) {
        return; // don't have any values yet
    }

    char buffer[10]; // used for formatting

    if (type_ == Type::WWC) {
        shell.printfln(F("  Warm Water Circuit #: %d"), hc_);

    } else {
        shell.printfln(F("  Heating Circuit #: %d"), hc_);
    }
    print_value(shell, 2, F("Current flow temperature"), F_(degrees), Helpers::render_value(buffer, flowTemp_, 10));
    print_value(shell, 2, F("Setpoint flow temperature"), F_(degrees), Helpers::render_value(buffer, flowSetTemp_, 1));
    print_value(shell, 2, F("Current pump modulation"), Helpers::render_value(buffer, pumpMod_, 1));
    print_value(shell, 2, F("Current valve status"), Helpers::render_value(buffer, status_, 1));
}

// publish values via MQTT
// ideally we should group up all the mixing units together into a nested JSON but for now we'll send them individually
void Mixing::publish_values() {
    DynamicJsonDocument doc(EMSESP_MAX_JSON_SIZE_SMALL);
    JsonObject          rootMixing = doc.to<JsonObject>();
    JsonObject          dataMixing;

    if (mqtt_format_ == Settings::MQTT_format::SINGLE) {
        switch (type_) {
        case Type::HC:
            rootMixing["type"] = F("hc");
            break;
        case Type::WWC:
            rootMixing["type"] = F("wwc");
            break;
        case Type::NONE:
        default:
            return;
        }
        dataMixing = rootMixing;
    } else {
        char hc_name[10]; // hc{1-4}
        if(type_ == Type::HC) {
            strlcpy(hc_name, "hc", 10);
        } else {
            strlcpy(hc_name, "wwc", 10);
        }
        char s[3]; // for formatting strings
        strlcat(hc_name, Helpers::itoa(s, hc_), 10);
        dataMixing = rootMixing.createNestedObject(hc_name);
    }

    if (flowTemp_ != EMS_VALUE_USHORT_NOTSET) {
        dataMixing["flowTemp"] = (float)flowTemp_ / 10;
    }

    if (pumpMod_ != EMS_VALUE_UINT_NOTSET) {
        dataMixing["pumpMod"] = pumpMod_;
    }

    if (status_ != EMS_VALUE_UINT_NOTSET) {
        dataMixing["status"] = status_;
    }

    if (flowSetTemp_ != EMS_VALUE_UINT_NOTSET) {
        dataMixing["flowSetTemp"] = flowSetTemp_;
    }

#ifdef EMSESP_DEBUG
    LOG_DEBUG(F("[DEBUG] Performing a mixing module publish"));
#endif
    // if format is single, send immediately and quit
    if (mqtt_format_ == Settings::MQTT_format::SINGLE) {
        char topic[30];
        char s[3]; // for formatting strings
        strlcpy(topic, "mixing_data", 30);
        strlcat(topic, Helpers::itoa(s, hc_), 30); // append hc to topic
        Mqtt::publish(topic, doc);
        return;
    }
    char topic[30];
    strlcpy(topic, "mixing_data", 30);
    Mqtt::publish(topic, doc);
}

// check to see if values have been updated
bool Mixing::updated_values() {
    return false;
}

// add console commands
void Mixing::console_commands() {
}

//  heating circuits 0x02D7, 0x02D8 etc...
void Mixing::process_MMPLUSStatusMessage_HC(std::shared_ptr<const Telegram> telegram) {
    type_ = Type::HC;
    hc_   = telegram->type_id - 0x02D7 + 1; // determine which circuit this is
    telegram->read_value(flowTemp_, 3);     // is * 10
    telegram->read_value(pumpMod_, 5);
    telegram->read_value(status_, 2); // valve status
}

// Mixing module warm water loading/DHW - 0x0331, 0x0332
// e.g. A9 00 FF 00 02 32 02 6C 00 3C 00 3C 3C 46 02 03 03 00 3C // on 0x28
//      A8 00 FF 00 02 31 02 35 00 3C 00 3C 3C 46 02 03 03 00 3C // in 0x29
void Mixing::process_MMPLUSStatusMessage_WWC(std::shared_ptr<const Telegram> telegram) {
    type_ = Type::WWC;
    hc_   = telegram->type_id - 0x0331 + 1; // determine which circuit this is. There are max 2.
    telegram->read_value(flowTemp_, 0);     // is * 10
    telegram->read_value(pumpMod_, 2);
    telegram->read_value(status_, 11); // temp status
}

// Mixing on a MM10 - 0xAB
// e.g. Mixing Module -> All, type 0xAB, telegram: 21 00 AB 00 2D 01 BE 64 04 01 00 (CRC=15) #data=7
// see also https://github.com/proddy/EMS-ESP/issues/386
void Mixing::process_MMStatusMessage(std::shared_ptr<const Telegram> telegram) {
    type_ = Type::HC;

    // the heating circuit is determine by which device_id it is, 0x20 - 0x23
    // 0x21 is position 2. 0x20 is typically reserved for the WM10 switch module
    // see https://github.com/proddy/EMS-ESP/issues/270 and https://github.com/proddy/EMS-ESP/issues/386#issuecomment-629610918
    hc_ = device_id() - 0x20 + 1;
    telegram->read_value(flowTemp_, 1); // is * 10
    telegram->read_value(pumpMod_, 3);
    telegram->read_value(flowSetTemp_, 0);
}

} // namespace emsesp