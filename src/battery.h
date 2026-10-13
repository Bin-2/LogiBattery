// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Pure HID++ decoding: no device I/O or UI state. Tests can use captured payloads.
namespace logibattery {
enum class Level { unknown, empty, critical, low, good, full };
enum class Charge { unknown, discharging, charging, full, error };
struct Battery {
    Level level = Level::unknown;
    Charge charge = Charge::unknown;
    int percent = -1; // -1 means unavailable, never an invented coarse percentage.
    int millivolts = -1;
    std::array<std::uint8_t, 7> raw{};
};
enum class ReplyKind { unrelated, battery, error };
struct Reply {
    ReplyKind kind = ReplyKind::unrelated;
    Battery battery;
    std::uint8_t error = 0;
};
std::array<std::uint8_t, 7> battery_request(std::uint8_t index);
Reply parse_reply(const std::uint8_t* data, std::size_t length, std::uint8_t index);
// Payload parsers return false for malformed/out-of-range values.
bool decode_feature(std::uint16_t feature, const std::uint8_t* payload,
                    std::size_t length, bool reports_percent, Battery& battery);
bool decode_mileage(const std::uint8_t* payload, std::size_t length, Battery& battery);
// Match both short and long responses, including echoed error headers.
int match_response(const std::uint8_t* data, std::size_t length,
                   std::uint8_t index, std::uint8_t command, std::uint8_t function);
int receiver_slots(std::uint16_t product);
// Descriptor evidence, not product ID/usage numbering, identifies HID++ I/O.
constexpr unsigned short_report = 1, long_report = 2;
std::uint8_t hidpp_report(std::uint16_t vendor, std::uint16_t page,
    std::uint16_t input_length, std::uint16_t output_length,
    unsigned input_reports, unsigned output_reports);
const wchar_t* level_name(Level level);
const wchar_t* charge_name(Charge charge);
const wchar_t* error_name(std::uint8_t error);
std::wstring hex(const std::uint8_t* data, std::size_t length);
std::wstring describe(const Battery& battery);
}
