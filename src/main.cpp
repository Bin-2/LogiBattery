// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid_reader.h"
#include "tray_view.h"
#include <shellapi.h>
#include <dbt.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <map>
#include <thread>

namespace {
constexpr wchar_t window_class[] = L"LogiBattery.Native.Tray.1";
constexpr wchar_t settings_key[] = L"Software\\LogiBattery";
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT reading_message = WM_APP + 2;
constexpr UINT retry_timer = 1;
constexpr UINT refresh_timer = 2;
constexpr UINT refresh_command = 100;
constexpr UINT details_command = 101;
constexpr UINT startup_command = 102;
constexpr UINT alerts_command = 103;
constexpr UINT exit_command = 104;
constexpr UINT separate_command = 106;
constexpr UINT offline_command = 107;
constexpr UINT interval_command = 200;
constexpr DWORD intervals[] = {30, 60, 120, 300};

DWORD read_setting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key, name, RRF_RT_REG_DWORD,
        nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return value;
}
LSTATUS save_setting(const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, name, REG_DWORD, &value, sizeof(value));
}
std::wstring executable_path() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return path;
}
std::wstring startup_value() {
    return L"\"" + executable_path() + L"\"";
}
bool startup_enabled() {
    wchar_t buffer[32768]{};
    DWORD bytes = sizeof(buffer);
    return RegGetValueW(HKEY_CURRENT_USER, run_key, L"LogiBattery", RRF_RT_REG_SZ,
        nullptr, buffer, &bytes) == ERROR_SUCCESS && startup_value() == buffer;
}
LSTATUS set_startup(bool enabled) {
    if (enabled) {
        const auto command = startup_value();
        return RegSetKeyValueW(HKEY_CURRENT_USER, run_key, L"LogiBattery", REG_SZ,
            command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, L"LogiBattery");
    return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}
void report_error(HWND window, const wchar_t* action, DWORD code) {
    const auto text = std::wstring(action) + L"\r\n" + logibattery::windows_error(code);
    MessageBoxW(window, text.c_str(), L"LogiBattery", MB_OK | MB_ICONERROR);
}

// Draw directly into a 32-bit ARGB bitmap: no image assets or graphics framework.
HICON make_icon(const logibattery::Reading& reading) {
    const int size = std::max(32, GetSystemMetrics(SM_CXSMICON));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!mask) { DeleteObject(color); return nullptr; }
    // Alpha controls transparency; initialize the legacy AND mask as well.
    const HDC dc = CreateCompatibleDC(nullptr);
    const auto old = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, size, size, BLACKNESS);
    SelectObject(dc, old);
    DeleteDC(dc);
    const auto level = reading.battery.level;
    const auto charge = reading.battery.charge;
    const bool charging = reading.available && (charge == logibattery::Charge::charging || charge == logibattery::Charge::full);
    int bars = 0;
    DWORD ink = 0xff9da7b1;
    if (reading.available) {
        ink = 0xff48c98a;
        switch (level) {
        case logibattery::Level::empty: ink = 0xfff06666; break;
        case logibattery::Level::critical: bars = 1; ink = 0xfff06666; break;
        case logibattery::Level::low: bars = 1; ink = 0xffffc34d; break;
        case logibattery::Level::good: bars = 2; break;
        case logibattery::Level::full: bars = 3; break;
        default: ink = 0xff9da7b1; break;
        }
        if (charging) { ink = 0xff64baff; if (charge == logibattery::Charge::full) bars = 3; }
        if (charge == logibattery::Charge::error) ink = 0xfff06666;
    }
    auto pixels = static_cast<DWORD*>(bits);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const int u = x * 32 / size;
        const int v = y * 32 / size;
        DWORD pixel = 0;
        if (u >= 11 && u < 21 && v >= 2 && v < 5) pixel = ink;
        if (u >= 5 && u < 27 && v >= 5 && v < 30
            && (u < 8 || u >= 24 || v < 8 || v >= 27)) pixel = ink;
        for (int bar = 0; bar < bars; ++bar)
            if (u >= 10 && u < 22 && v >= 22 - bar * 6 && v < 26 - bar * 6) pixel = ink;
        if (!reading.available || level == logibattery::Level::unknown) {
            if (u >= 11 && u < 21 && v >= 11 && v < 24 && std::abs((u - 11) - (23 - v)) <= 1) pixel = ink;
        }
        if (charging && ((u >= 22 && u < 25 && v >= 0 && v < 11)
            || (u >= 18 && u < 29 && v >= 4 && v < 7))) pixel = 0xffe8f5ff;
        pixels[y * size + x] = pixel;
    }
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    const auto icon = CreateIconIndirect(&icon_info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Window, tray icons and snapshots are owned by the UI thread. The reader
// worker publishes only pending under mutex, then posts reading_message.
// Cancellation events stop I/O before shutdown joins the worker.
struct App {
    HWND window = nullptr;
    logibattery::Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    logibattery::Handle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    logibattery::Handle refresh{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<DWORD> interval{60};
    std::atomic<bool> suspended{false};
    std::mutex mutex;
    logibattery::Snapshot pending;
    logibattery::Snapshot snapshot;
    struct Tray {
        UINT id = 0;
        HICON icon = nullptr;
        bool added = false;
    };
    std::map<std::wstring, Tray> trays;
    UINT next_id = 2;
    UINT selected_id = 1;
    std::thread worker;
    HDEVNOTIFY device_notification = nullptr;
    UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    bool menu_open = false;
    bool alerts = true;
    bool separate_icons = false;
    bool show_offline = false;
    logibattery::AlertHistory alert_history;

    NOTIFYICONDATAW tray_data(UINT id) const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = id;
        return data;
    }
    void select_tray(UINT id) {
        for (const auto& pair : trays) if (pair.second.id == id) {
            selected_id = id; return;
        }
    }
    void update_tray() {
        bool retry = false;
        const auto views = logibattery::tray_views(snapshot, separate_icons, show_offline);
        for (auto it = trays.begin(); it != trays.end();) {
            const bool exists = std::any_of(views.begin(), views.end(), [&](const auto& r) { return r.key == it->first; });
            if (!exists) {
                auto data = tray_data(it->second.id); Shell_NotifyIconW(NIM_DELETE, &data);
                if (it->second.icon) DestroyIcon(it->second.icon);
                it = trays.erase(it);
            } else ++it;
        }
        for (const auto& view : views) {
            const auto& reading = view.reading;
            auto& tray = trays[view.key];
            if (!tray.id) tray.id = view.key.empty() ? 1 : next_id++;
            HICON next = make_icon(reading);
            if (next) { if (tray.icon) DestroyIcon(tray.icon); tray.icon = next; }
            auto data = tray_data(tray.id);
            data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
            data.uCallbackMessage = tray_message;
            data.hIcon = tray.icon;
            wcsncpy_s(data.szTip, view.tooltip.c_str(), _TRUNCATE);
            if (!tray.added) {
                tray.added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
                if (tray.added) {
                    data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data);
                }
            } else if (!Shell_NotifyIconW(NIM_MODIFY, &data)) tray.added = false;
            retry = retry || !tray.added;
        }
        const auto selected = std::find_if(trays.begin(), trays.end(), [&](const auto& p) { return p.second.id == selected_id; });
        if (selected == trays.end()) selected_id = trays.begin()->second.id;
        if (retry) SetTimer(window, retry_timer, 3000, nullptr);
        else KillTimer(window, retry_timer);
    }
    void alert_if_needed() {
        for (const auto& reading : snapshot.devices) {
            const auto warning = alert_history.pending(reading);
            if (!alerts || warning == logibattery::Level::unknown) continue;
            const auto icon = trays.find(separate_icons ? reading.key : L"");
            if (icon == trays.end() || !icon->second.added) continue;
            auto data = tray_data(icon->second.id);
            data.uFlags = NIF_INFO;
            data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
            wcscpy_s(data.szInfoTitle, L"Device battery running low");
            const auto text = logibattery::device_summary(reading)
                + L". Recharge or replace its battery when convenient.";
            wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
            if (Shell_NotifyIconW(NIM_MODIFY, &data)) alert_history.mark_sent(reading, warning);
        }
    }
    void request_refresh() { SetEvent(refresh.get()); }
    void start_worker() {
        worker = std::thread([this] {
            for (;;) {
                if (WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0) break;
                if (!suspended.load()) {
                    logibattery::Snapshot reading;
                    try { reading = logibattery::read_batteries(cancel.get()); }
                    catch (...) { /* Publish an empty snapshot after a failed poll. */ }
                    if (!suspended.load() && WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0) {
                        { std::lock_guard<std::mutex> guard(mutex); pending = std::move(reading); }
                        PostMessageW(window, reading_message, 0, 0);
                    }
                }
                HANDLE events[] = {stop.get(), refresh.get()};
                const DWORD timeout = suspended.load() ? INFINITE : interval.load() * 1000;
                if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0) break;
            }
        });
    }
    void shutdown() {
        SetEvent(stop.get());
        SetEvent(cancel.get());
        if (worker.joinable()) worker.join();
        if (device_notification) { UnregisterDeviceNotification(device_notification); device_notification = nullptr; }
        for (auto& pair : trays) {
            auto data = tray_data(pair.second.id); Shell_NotifyIconW(NIM_DELETE, &data);
            if (pair.second.icon) DestroyIcon(pair.second.icon);
        }
        trays.clear();
    }
    void command(UINT id) {
        if (id == refresh_command) request_refresh();
        else if (id == details_command) {
            std::wstring text = LOGIBATTERY_BUILD_LABEL L"\r\n\r\n";
            const auto devices = logibattery::visible_devices(snapshot, show_offline);
            if (suspended.load()) text += L"Suspended; readings will refresh after resume.\r\n\r\n";
            if (devices.empty()) text += L"No online devices; wake a device and refresh.\r\n\r\n";
            for (const auto& r : devices) text += logibattery::reading_text(r) + L"\r\n";
            text += L"Use the tray menu to choose unified or separate icons.";
            MessageBoxW(window, text.c_str(), L"LogiBattery - details", MB_OK | MB_ICONINFORMATION);
        }
        else if (id == startup_command) {
            const auto status = set_startup(!startup_enabled());
            if (status != ERROR_SUCCESS) report_error(window, L"Could not change startup setting", status);
        } else if (id == alerts_command) {
            const auto status = save_setting(L"Alerts", alerts ? 0 : 1);
            if (status == ERROR_SUCCESS) { alerts = !alerts; if (alerts) alert_history.clear(); }
            else report_error(window, L"Could not save alert setting", status);
        } else if (id == separate_command || id == offline_command) {
            bool& option = id == separate_command ? separate_icons : show_offline;
            const auto status = save_setting(id == separate_command ? L"SeparateIcons" : L"ShowOffline", option ? 0 : 1);
            if (status == ERROR_SUCCESS) { option = !option; update_tray(); }
            else report_error(window, L"Could not save tray setting", status);
        } else if (id >= interval_command && id < interval_command + std::size(intervals)) {
            const DWORD seconds = intervals[id - interval_command];
            const auto status = save_setting(L"IntervalSeconds", seconds);
            if (status == ERROR_SUCCESS) { interval = seconds; request_refresh(); }
            else report_error(window, L"Could not save polling interval", status);
        } else if (id == exit_command) DestroyWindow(window);
    }
    void show_menu() {
        if (menu_open) return;
        menu_open = true;
        const auto menu = CreatePopupMenu();
        const auto polling = CreatePopupMenu();
        bool listed = false;
        for (const auto& reading : logibattery::visible_devices(snapshot, show_offline)) {
            auto summary = logibattery::device_summary(reading);
            // Escape menu accelerator markers in names supplied by devices.
            for (std::size_t pos = 0; (pos = summary.find(L'&', pos)) != std::wstring::npos; pos += 2)
                summary.insert(pos, 1, L'&');
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, summary.c_str());
            listed = true;
        }
        if (!listed) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No online devices");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, refresh_command, L"&Refresh now");
        for (UINT i = 0; i < std::size(intervals); ++i) {
            const auto label = std::to_wstring(intervals[i]) + L" seconds";
            AppendMenuW(polling, MF_STRING | (interval == intervals[i] ? MF_CHECKED : 0), interval_command + i, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(polling), L"Polling &interval");
        AppendMenuW(menu, MF_STRING | (separate_icons ? MF_CHECKED : 0), separate_command, L"Separate icons for each &device");
        AppendMenuW(menu, MF_STRING | (show_offline ? MF_CHECKED : 0), offline_command, L"Show &offline devices");
        AppendMenuW(menu, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), startup_command, L"Launch at &sign-in");
        AppendMenuW(menu, MF_STRING | (alerts ? MF_CHECKED : 0), alerts_command, L"&Low battery notifications");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, details_command, L"&Details...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exit_command, L"&Quit LogiBattery");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window);
        const UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, window, nullptr);
        DestroyMenu(menu);
        PostMessageW(window, WM_NULL, 0, 0);
        menu_open = false;
        if (selected) command(selected);
        auto data = tray_data(selected_id);
        if (IsWindow(window)) Shell_NotifyIconW(NIM_SETFOCUS, &data);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    if (message == app->taskbar_created) { for (auto& pair : app->trays) pair.second.added = false; app->update_tray(); return 0; }
    switch (message) {
    case tray_message:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT)
        {
            app->select_tray(HIWORD(lparam));
            app->show_menu();
        }
        return 0;
    case reading_message:
        if (app->suspended.load()) return 0;
        { std::lock_guard<std::mutex> guard(app->mutex); app->snapshot = app->pending; }
        app->update_tray();
        app->alert_if_needed();
        return 0;
    case WM_COMMAND: app->command(LOWORD(wparam)); return 0;
    case WM_TIMER:
        if (wparam == retry_timer) app->update_tray();
        else if (wparam == refresh_timer) { KillTimer(window, refresh_timer); app->request_refresh(); }
        return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE)
            SetTimer(window, refresh_timer, 1500, nullptr);
        return TRUE;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            app->suspended = true;
            SetEvent(app->cancel.get());
            for (auto& r : app->snapshot.devices) { r.available = false; r.error = L"Suspended"; }
            app->update_tray();
        } else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            ResetEvent(app->cancel.get());
            app->suspended = false;
            SetTimer(window, refresh_timer, 2000, nullptr);
        }
        return TRUE;
    case WM_SETTINGCHANGE: case WM_DPICHANGED: case WM_DISPLAYCHANGE:
        app->update_tray(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wparam) DestroyWindow(window); return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        KillTimer(window, retry_timer);
        KillTimer(window, refresh_timer);
        app->shutdown();
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    LocalFree(arguments);
    if (count > 1) {
        MessageBoxW(nullptr, L"Run without arguments for the tray app.",
            L"LogiBattery - usage", MB_OK);
        return 2;
    }
    logibattery::Handle singleton(CreateMutexW(nullptr, FALSE, L"Local\\LogiBattery.Native.Tray.1"));
    if (!singleton.valid()) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(window_class, nullptr)) PostMessageW(existing, WM_COMMAND, refresh_command, 0);
        return 0;
    }
    App app;
    if (!app.stop.valid() || !app.cancel.valid() || !app.refresh.valid()) return 2;
    const auto saved_interval = read_setting(L"IntervalSeconds", 60);
    if (std::find(std::begin(intervals), std::end(intervals), saved_interval) != std::end(intervals)) app.interval = saved_interval;
    app.alerts = read_setting(L"Alerts", 1) != 0;
    app.separate_icons = read_setting(L"SeparateIcons", 0) != 0;
    app.show_offline = read_setting(L"ShowOffline", 0) != 0;
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.lpszClassName = window_class;
    if (!RegisterClassW(&type)) return 2;
    // A hidden top-level window, rather than HWND_MESSAGE, receives Explorer's
    // TaskbarCreated broadcast and Windows power notifications.
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, window_class, L"LogiBattery", WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, instance, &app)) return 2;
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    app.device_notification = RegisterDeviceNotificationW(app.window, &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    app.update_tray();
    try { app.start_worker(); }
    catch (...) {
        MessageBoxW(app.window, L"Could not start the battery reader.", L"LogiBattery", MB_OK | MB_ICONERROR);
        DestroyWindow(app.window);
        return 2;
    }
    MSG message{};
    BOOL status;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status == -1) { if (IsWindow(app.window)) DestroyWindow(app.window); return 2; }
    return static_cast<int>(message.wParam);
}
