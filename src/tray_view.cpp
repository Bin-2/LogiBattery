// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray_view.h"
#include <algorithm>

namespace logibattery {
std::wstring device_summary(const Reading& r) {
    return r.name + L": " + (r.available ? describe(r.battery) : L"Unavailable");
}
namespace {
int attention(const Reading& r) {
    if (!r.available) return 100;
    if (r.battery.charge == Charge::error) return 0;
    if (r.battery.charge == Charge::charging || r.battery.charge == Charge::full) return 20;
    switch (r.battery.level) {
    case Level::empty: return 1;
    case Level::critical: return 2;
    case Level::low: return 3;
    case Level::good: return 4;
    case Level::full: return 5;
    default: return 10;
    }
}
std::wstring summary_tooltip(const std::vector<Reading>& readings) {
    // Windows' standard tray tooltip holds 127 UTF-16 code units. Keep whole
    // lines and reserve room for an explicit overflow hint instead of silently
    // cutting a device's name or battery value in half.
    std::wstring text = L"LogiBattery";
    for (std::size_t i = 0; i < readings.size(); ++i) {
        auto line = device_summary(readings[i]);
        if (line.size() > 82) line = line.substr(0, 79) + L"...";
        const auto remaining = readings.size() - i - 1;
        const auto suffix = remaining ? L"\n+" + std::to_wstring(remaining) + L" more - click to view" : L"";
        if (text.size() + 1 + line.size() + suffix.size() > 127) {
            text += L"\n+" + std::to_wstring(readings.size() - i) + L" more - click to view";
            break;
        }
        text += L'\n' + line;
    }
    return text;
}
}
std::vector<Reading> visible_devices(const Snapshot& snapshot, bool show_offline) {
    std::vector<Reading> visible;
    for (const auto& r : snapshot.devices) if (r.available || show_offline) visible.push_back(r);
    return visible;
}
std::vector<TrayView> tray_views(const Snapshot& snapshot, bool separate, bool show_offline) {
    const auto visible = visible_devices(snapshot, show_offline);
    if (separate && !visible.empty()) {
        std::vector<TrayView> views;
        for (const auto& r : visible) views.push_back({r.key, r, device_summary(r)});
        return views;
    }
    Reading representative;
    representative.error = L"No online devices; wake a device and refresh";
    const auto best = std::min_element(visible.begin(), visible.end(), [](const auto& a, const auto& b) {
        if (attention(a) != attention(b)) return attention(a) < attention(b);
        const int a_percent = a.battery.percent >= 0 ? a.battery.percent : 101;
        const int b_percent = b.battery.percent >= 0 ? b.battery.percent : 101;
        return a_percent < b_percent;
    });
    if (best != visible.end() && best->available) representative = *best;
    const auto tip = visible.empty() ? L"LogiBattery\n" + representative.error : summary_tooltip(visible);
    return {{L"", representative, tip}};
}
Level AlertHistory::pending(const Reading& r) {
    // A sleeping device may have only its shorter receiver pairing name.
    // That must not look like a replacement device and rearm notifications.
    if (!r.available) return Level::unknown;
    auto& state = devices[r.key];
    if (state.name != r.name) { state.name = r.name; state.sent = Level::unknown; }
    const auto& b = r.battery;
    if (b.charge == Charge::charging || b.charge == Charge::full || b.level == Level::good || b.level == Level::full) {
        state.sent = Level::unknown;
        return Level::unknown;
    }
    if (b.charge != Charge::discharging) return Level::unknown;
    const auto warning = b.level == Level::empty ? Level::critical : b.level;
    if ((warning != Level::low && warning != Level::critical)
        || state.sent == warning || state.sent == Level::critical) return Level::unknown;
    return warning;
}
void AlertHistory::mark_sent(const Reading& r, Level level) { devices[r.key] = {r.name, level}; }
}
