// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "hid_reader.h"
#include <map>

namespace logibattery {
// Presentation policy shared by menus, Details and tray icons. Hiding an
// unavailable pairing is a view choice and never changes receiver pairings.
struct TrayView {
    std::wstring key; // Empty means the application icon, including the empty state.
    Reading reading;
    std::wstring tooltip;
};
std::wstring device_summary(const Reading& reading);
std::vector<Reading> visible_devices(const Snapshot& snapshot, bool show_offline);
std::vector<TrayView> tray_views(const Snapshot& snapshot, bool separate, bool show_offline);

// Notification history belongs to devices, independently of visible icons.
class AlertHistory {
    struct State { std::wstring name; Level sent = Level::unknown; };
    std::map<std::wstring, State> devices;
public:
    Level pending(const Reading& reading);
    void mark_sent(const Reading& reading, Level level);
    void clear() { devices.clear(); }
};
}
