// SPDX-License-Identifier: GPL-3.0-or-later
// Register decoding informed by Solaar (Daniel Pavel and contributors),
// with explicit charging-state values from Linux's hid-logitech-hidpp driver.
#include "battery.h"
#include <algorithm>

namespace logibattery {
namespace {
Level percent_level(int value) {
    return value == 0 ? Level::empty : value <= 10 ? Level::critical
        : value <= 30 ? Level::low : value < 90 ? Level::good : Level::full;
}
Charge status_charge(std::uint8_t value) {
    switch (value) {
    case 0: return Charge::discharging;
    case 1: case 2: case 4: return Charge::charging;
    case 3: return Charge::full;
    case 5: case 6: return Charge::error;
    default: return Charge::unknown;
    }
}
}
int match_response(const std::uint8_t* d, std::size_t n,
                   std::uint8_t index, std::uint8_t command, std::uint8_t function) {
    if (n < 7 || (d[0] != 0x10 && d[0] != 0x11) || d[1] != index) return 0;
    if (d[0] == 0x11 && n < 20) return 0;
    if ((d[2] == 0x8f || d[2] == 0xff) && d[3] == command && d[4] == function)
        return -1;
    return d[2] == command && d[3] == function ? 1 : 0;
}
bool decode_mileage(const std::uint8_t* p, std::size_t n, Battery& b) {
    if (n < 3 || p[0] > 100) return false;
    b = {};
    b.percent = p[0]; b.level = percent_level(b.percent);
    switch (p[2] & 0xf0) {
    case 0x30: b.charge = Charge::discharging; break;
    case 0x50: b.charge = Charge::charging; break;
    case 0x90: b.charge = Charge::full; break;
    }
    return true;
}
bool decode_feature(std::uint16_t feature, const std::uint8_t* p,
                    std::size_t n, bool reports_percent, Battery& b) {
    if (n < 3) return false;
    b = {};
    if (feature == 0x1000 || feature == 0x1004) {
        if (feature == 0x1004 && n < 4) return false;
        if (reports_percent && p[0] > 100) return false;
        b.charge = status_charge(p[2]);
        if (feature == 0x1004) {
            // Unified battery uses a different charging enum: 4 is an error.
            b.charge = p[2] == 4 ? Charge::error : p[2] > 4 ? Charge::unknown : b.charge;
            switch (p[1]) {
            case 1: b.level = Level::critical; break;
            case 2: b.level = Level::low; break;
            case 4: b.level = Level::good; break;
            case 8: b.level = Level::full; break;
            }
        }
        // 1000 uses zero for unknown. 1004 has an explicit capability bit.
        if (reports_percent && (feature == 0x1004 || p[0] != 0)) {
            b.percent = p[0];
            if (b.level == Level::unknown) b.level = percent_level(b.percent);
        }
        return true;
    }
    if (feature == 0x1f20) {
        // Solaar / LGSTrayEx ADC measurement: an invalid or buffered all-zero
        // frame is an unavailable device, never a genuine empty battery.
        const int mv = (p[0] << 8) | p[1];
        if (!(p[2] & 1) || mv < 2000 || mv > 5000) return false;
        b.millivolts = mv;
        b.charge = (p[2] & 2) ? Charge::charging : Charge::discharging;
        return true; // Preserve voltage; do not guess the battery chemistry.
    }
    if (feature == 0x1001) {
        b.millivolts = (p[0] << 8) | p[1];
        if (b.millivolts == 0 || b.millivolts > 20000) return false;
        b.charge = Charge::discharging;
        if (p[2] & 0x80) {
            switch (p[2] & 3) {
            case 0: b.charge = Charge::charging; break;
            case 1: b.charge = Charge::full; b.level = Level::full; break;
            case 2: b.charge = Charge::error; break;
            default: b.charge = Charge::unknown; break;
            }
        }
        if (p[2] & 0x20) b.level = Level::critical;
        return true; // Show voltage; no universal curve for different chemistries.
    }
    return false;
}
int receiver_slots(std::uint16_t product) {
    // Receiver IDs from Solaar base_usb.py. A receiver PID is not a model ID.
    switch (product) {
    case 0xc52b: case 0xc532: case 0xc548: return 6;
    case 0xc534: return 2;
    case 0xc518: case 0xc51a: case 0xc51b: case 0xc521: case 0xc525:
    case 0xc526: case 0xc52e: case 0xc52f: case 0xc531: case 0xc535:
    case 0xc537: case 0xc539: case 0xc53a: case 0xc53d: case 0xc53f:
    case 0xc541: case 0xc545: case 0xc547: case 0xc54d: return 1;
    default: return 0;
    }
}
std::uint8_t hidpp_report(std::uint16_t vendor, std::uint16_t page,
    std::uint16_t input_length, std::uint16_t output_length,
    unsigned input_reports, unsigned output_reports) {
    if (vendor != 0x046d || page < 0xff00) return 0;
    const auto both = input_reports & output_reports;
    if ((both & long_report) && input_length == 20 && output_length == 20) return 0x11;
    if ((both & short_report) && input_length == 7 && output_length == 7) return 0x10;
    return 0;
}
std::array<std::uint8_t, 7> battery_request(std::uint8_t index) {
    return {0x10, index, 0x81, 0x07, 0, 0, 0};
}

Reply parse_reply(const std::uint8_t* data, std::size_t length, std::uint8_t index) {
    Reply result;
    if (length < 7 || data[0] != 0x10 || data[1] != index) return result;
    if (data[2] == 0x8f && data[3] == 0x81 && data[4] == 0x07) {
        result.kind = ReplyKind::error;
        result.error = data[5];
        return result;
    }
    if (data[2] != 0x81 || data[3] != 0x07) return result;
    result.kind = ReplyKind::battery;
    auto& battery = result.battery;
    std::copy_n(data, 7, battery.raw.begin());
    switch (data[4]) {
    case 0: battery.level = data[5] == 0 ? Level::empty : Level::unknown; break;
    case 1: case 2: battery.level = Level::critical; break;
    case 3: case 4: battery.level = Level::low; break;
    case 5: case 6: battery.level = Level::good; break;
    case 7: battery.level = Level::full; break;
    default: break;
    }
    switch (data[5]) {
    case 0x00: battery.charge = Charge::discharging; break;
    case 0x21: case 0x24: case 0x25: battery.charge = Charge::charging; break;
    case 0x22: case 0x26: battery.charge = Charge::full; break;
    case 0x23: battery.charge = Charge::error; break;
    default: break;
    }
    return result;
}

const wchar_t* level_name(Level level) {
    switch (level) {
    case Level::empty: return L"Empty";
    case Level::critical: return L"Critical";
    case Level::low: return L"Low";
    case Level::good: return L"Good";
    case Level::full: return L"Full";
    default: return L"Unknown";
    }
}
const wchar_t* charge_name(Charge charge) {
    switch (charge) {
    case Charge::discharging: return L"Discharging";
    case Charge::charging: return L"Charging";
    case Charge::full: return L"Fully charged";
    case Charge::error: return L"Charging error";
    default: return L"Charging state unknown";
    }
}
const wchar_t* error_name(std::uint8_t error) {
    switch (error) {
    case 1: return L"Unsupported command";
    case 2: return L"Unsupported register";
    case 7: return L"Receiver busy; will retry";
    case 8: return L"No device paired at this slot";
    case 9: return L"Device asleep or disconnected";
    case 10: return L"Battery request unavailable";
    default: return L"Device rejected the battery request";
    }
}
std::wstring hex(const std::uint8_t* data, std::size_t length) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    for (std::size_t i = 0; i < length; ++i) {
        if (i) result += L' ';
        result += digits[data[i] >> 4];
        result += digits[data[i] & 15];
    }
    return result;
}
std::wstring describe(const Battery& battery) {
    auto text = battery.percent >= 0 ? std::to_wstring(battery.percent) + L"%"
        : std::wstring(level_name(battery.level));
    if (battery.millivolts >= 0) text += L" (" + std::to_wstring(battery.millivolts) + L" mV)";
    return text + L" / " + charge_name(battery.charge);
}
}
