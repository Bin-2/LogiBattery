// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid_reader.h"
#include <hidsdi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace logibattery {
namespace {
struct Endpoint {
    std::wstring path, group;
    USHORT product = 0, input_length = 0, output_length = 0;
    BYTE report_id = 0;
};
class DeviceList {
public:
    HDEVINFO value;
    explicit DeviceList(const GUID& guid) : value(SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)) {}
    ~DeviceList() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
unsigned report_ids(PHIDP_PREPARSED_DATA data, const HIDP_CAPS& caps, HIDP_REPORT_TYPE type) {
    unsigned result = 0;
    const auto add = [&](BYTE id) {
        if (id == 0x10) result |= short_report;
        if (id == 0x11) result |= long_report;
    };
    USHORT count = type == HidP_Input ? caps.NumberInputValueCaps : caps.NumberOutputValueCaps;
    if (count) {
        std::vector<HIDP_VALUE_CAPS> values(count);
        if (HidP_GetValueCaps(type, values.data(), &count, data) != HIDP_STATUS_SUCCESS) return 0;
        for (USHORT i = 0; i < count; ++i) add(values[i].ReportID);
    }
    count = type == HidP_Input ? caps.NumberInputButtonCaps : caps.NumberOutputButtonCaps;
    if (count) {
        std::vector<HIDP_BUTTON_CAPS> buttons(count);
        if (HidP_GetButtonCaps(type, buttons.data(), &count, data) != HIDP_STATUS_SUCCESS) return 0;
        for (USHORT i = 0; i < count; ++i) add(buttons[i].ReportID);
    }
    return result;
}
std::vector<Endpoint> enumerate(HANDLE cancel) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    DeviceList list(guid);
    std::vector<Endpoint> endpoints;
    if (list.value == INVALID_HANDLE_VALUE) {
        return endpoints;
    }
    for (DWORD i = 0; WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0; ++i) {
        SP_DEVICE_INTERFACE_DATA item{};
        item.cbSize = sizeof(item);
        if (!SetupDiEnumDeviceInterfaces(list.value, nullptr, &guid, i, &item)) break;
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(list.value, &item, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(bytes);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(list.value, &item, detail, bytes, nullptr, &info)) continue;
        Handle metadata(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (!metadata.valid()) continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(metadata.get(), &attributes) || attributes.VendorID != 0x046d) continue;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!HidD_GetPreparsedData(metadata.get(), &preparsed)) continue;
        HIDP_CAPS caps{};
        const auto status = HidP_GetCaps(preparsed, &caps);
        BYTE report = 0;
        if (status == HIDP_STATUS_SUCCESS) {
            report = hidpp_report(attributes.VendorID, caps.UsagePage,
                caps.InputReportByteLength, caps.OutputReportByteLength,
                report_ids(preparsed, caps, HidP_Input), report_ids(preparsed, caps, HidP_Output));
        }
        HidD_FreePreparsedData(preparsed);
        if (!report) continue;
        // Gaming devices can put short and long reports on different USB
        // interfaces. Container ID joins those without merging identical dongles.
        GUID container{};
        DEVPROPTYPE property_type = 0;
        DWORD property_size = 0;
        std::wstring group;
        if (SetupDiGetDevicePropertyW(list.value, &info, &DEVPKEY_Device_ContainerId,
            &property_type, reinterpret_cast<BYTE*>(&container), sizeof(container), &property_size, 0)
            && property_type == DEVPROP_TYPE_GUID && property_size == sizeof(container)
            && container != GUID{}) {
            group = hex(reinterpret_cast<const BYTE*>(&container), sizeof(container));
        } else {
            DEVINST parent;
            wchar_t parent_id[MAX_DEVICE_ID_LEN]{};
            if (CM_Get_Parent(&parent, info.DevInst, 0) != CR_SUCCESS
                || CM_Get_Device_IDW(parent, parent_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) continue;
            group = parent_id;
        }
        group += L"/" + std::to_wstring(attributes.ProductID);
        endpoints.push_back({detail->DevicePath, group, attributes.ProductID,
            caps.InputReportByteLength, caps.OutputReportByteLength, report});
    }
    return endpoints;
}
// Wait for every cancelled OVERLAPPED operation to finish before its buffer or
// event goes out of scope. The UI never waits for a HID request on its thread.
bool transfer(HANDLE device, HANDLE cancel, bool writing, BYTE* buffer,
              DWORD length, DWORD timeout, DWORD& transferred, std::wstring& error) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) { error = windows_error(GetLastError()); return false; }
    if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { error = L"Cancelled"; return false; }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    const BOOL done = writing ? WriteFile(device, buffer, length, nullptr, &operation)
                             : ReadFile(device, buffer, length, nullptr, &operation);
    if (!done) {
        const DWORD code = GetLastError();
        if (code != ERROR_IO_PENDING) { error = windows_error(code); return false; }
        HANDLE events[] = {cancel, event.get()};
        const auto wait = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (wait != WAIT_OBJECT_0 + 1) {
            const DWORD wait_error = GetLastError();
            CancelIoEx(device, &operation);
            GetOverlappedResult(device, &operation, &transferred, TRUE);
            error = wait == WAIT_TIMEOUT ? L"Mouse did not reply; move it and refresh"
                : wait == WAIT_OBJECT_0 ? L"Cancelled" : windows_error(wait_error);
            return false;
        }
    }
    if (!GetOverlappedResult(device, &operation, &transferred, FALSE)) {
        error = windows_error(GetLastError());
        return false;
    }
    return true;
}

// Every pending read owns stable storage and drains cancellation on destruction.
struct Port {
    Endpoint endpoint;
    Handle device;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED operation{};
    std::vector<BYTE> buffer;
    bool pending = false;
    explicit Port(const Endpoint& e) : endpoint(e), device(CreateFileW(e.path.c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)), buffer(e.input_length) {
        operation.hEvent = event.get();
    }
    ~Port() { finish(); }
    void finish() {
        if (!pending) return;
        CancelIoEx(device.get(), &operation);
        DWORD count = 0;
        GetOverlappedResult(device.get(), &operation, &count, TRUE);
        pending = false;
    }
    bool start() {
        ResetEvent(event.get());
        operation = {}; operation.hEvent = event.get();
        const BOOL done = ReadFile(device.get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &operation);
        pending = done || GetLastError() == ERROR_IO_PENDING;
        if (done) SetEvent(event.get());
        return pending;
    }
};
// A session owns every channel belonging to one receiver/device. Requests
// listen on all channels and validate reply addressing before decoding payloads.
class Session {
    HANDLE cancel;
    std::vector<std::unique_ptr<Port>> ports;
    BYTE software = 0;
public:
    std::wstring error;
    BYTE error_code = 0;
    bool protocol_error = false;
    BYTE error_report = 0;
    bool prefer_long = false;
    explicit Session(const std::vector<Endpoint>& endpoints, HANDLE stop) : cancel(stop) {
        for (const auto& e : endpoints) {
            auto p = std::make_unique<Port>(e);
            if (p->device.valid() && p->event.valid() && ports.size() < MAXIMUM_WAIT_OBJECTS - 1) ports.push_back(std::move(p));
        }
    }
    std::vector<BYTE> request(BYTE index, BYTE command, BYTE function,
                             BYTE a = 0, BYTE b = 0, BYTE c = 0, bool feature = true,
                             bool force_long = false, DWORD timeout = 1500) {
        error.clear(); error_code = 0; protocol_error = false; error_report = 0;
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { error = L"Cancelled"; return {}; }
        if (ports.empty()) { error = L"Cannot open HID++ channel"; return {}; }
        if (feature) { software = software % 15 + 1; function = (function & 0xf0) | software; }
        Port* writer = ports.front().get();
        const BYTE wanted = force_long || (feature && prefer_long) ? 0x11 : 0x10;
        for (auto& p : ports) if (p->endpoint.report_id == wanted) writer = p.get();
        if (force_long && writer->endpoint.report_id != 0x11) { error = L"No HID++ long output channel"; return {}; }
        std::vector<BYTE> output(writer->endpoint.output_length, 0);
        output[0] = writer->endpoint.report_id;
        output[1] = index; output[2] = command; output[3] = function;
        output[4] = a; output[5] = b; output[6] = c;
        struct Drain { std::vector<std::unique_ptr<Port>>& ports;
            ~Drain() { for (auto& p : ports) p->finish(); } } drain{ports};
        std::vector<HANDLE> events{cancel};
        for (auto& p : ports) {
            if (!HidD_FlushQueue(p->device.get()) || !p->start()) {
                error = L"Cannot read HID++ channel: " + windows_error(GetLastError()); return {};
            }
            events.push_back(p->event.get());
        }
        DWORD count = 0;
        if (!transfer(writer->device.get(), cancel, true, output.data(), static_cast<DWORD>(output.size()),
            1000, count, error)) return {};
        if (count != output.size()) { error = L"Incomplete HID request"; return {}; }
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            const auto wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                static_cast<DWORD>(deadline - now));
            if (wait == WAIT_OBJECT_0) { error = L"Cancelled"; return {}; }
            if (wait == WAIT_TIMEOUT) break;
            if (wait < WAIT_OBJECT_0 + 1 || wait >= WAIT_OBJECT_0 + events.size()) {
                error = windows_error(GetLastError()); return {};
            }
            auto& p = *ports[wait - WAIT_OBJECT_0 - 1];
            if (!GetOverlappedResult(p.device.get(), &p.operation, &count, FALSE)) {
                error = windows_error(GetLastError()); return {};
            }
            p.pending = false;
            const int match = match_response(p.buffer.data(), count, index, command, function);
            if (match < 0) {
                error_code = p.buffer[5]; protocol_error = true; error_report = p.buffer[2];
                error = p.buffer[2] == 0x8f ? error_name(error_code)
                    : L"HID++ 2.0 error " + hex(&error_code, 1);
                return {};
            }
            if (match > 0) {
                if (!feature && command == 0x83 && function == 0xb5 && p.buffer[4] != a) {
                    if (!p.start()) break;
                    continue;
                }
                // Ping's challenge prevents accepting another process's ping.
                if (command == 0 && (function & 0xf0) == 0x10 && p.buffer[6] != c) {
                    if (!p.start()) break;
                    continue;
                }
                const auto length = p.buffer[0] == 0x10 ? 7u : 20u;
                return {p.buffer.begin(), p.buffer.begin() + length};
            }
            if (!p.start()) { error = windows_error(GetLastError()); return {}; }
        }
        error = L"Device did not reply; wake it and refresh";
        return {};
    }
    std::vector<BYTE> ping(BYTE index, DWORD timeout = 1000) {
        prefer_long = false;
        const BYTE challenge = static_cast<BYTE>(GetTickCount64());
        auto reply = request(index, 0, 0x10, 0, 0, challenge, true, false, timeout);
        if (!reply.empty()) return reply;
        const bool legacy_rejection = error_report == 0x8f && error_code == 1;
        const bool has_long = std::any_of(ports.begin(), ports.end(), [](const auto& p) { return p->endpoint.report_id == 0x11; });
        const bool has_short = std::any_of(ports.begin(), ports.end(), [](const auto& p) { return p->endpoint.report_id == 0x10; });
        if (has_long && has_short) {
            prefer_long = true;
            reply = request(index, 0, 0x10, 0, 0, challenge, true, true, timeout);
            if (reply.empty()) {
                prefer_long = false;
                // A short-channel rejection alone does not prove HID++ 1.0:
                // some headsets only implement the long request channel.
                // Preserve the legacy result only if neither channel answers.
                if (legacy_rejection) {
                    error_report = 0x8f; error_code = 1; protocol_error = true;
                    error = error_name(error_code);
                }
            }
        }
        return reply;
    }
    BYTE feature_index(BYTE index, USHORT feature) {
        const auto r = request(index, 0, 0, static_cast<BYTE>(feature >> 8), static_cast<BYTE>(feature));
        return r.empty() ? 0 : r[4];
    }
};
std::wstring utf8_name(const std::string& value) {
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n);
    for (auto& ch : result) if (ch < 32) ch = L' ';
    return result;
}
void query(Session& s, BYTE index, Reading& r, bool legacy) {
    if (!legacy) {
        const auto ping = s.ping(index);
        if (ping.empty()) {
            // HID++ 1.0 devices reject the HID++ 2.0 ping with invalid sub-ID.
            if (s.protocol_error && s.error_report == 0x8f && s.error_code == 1) legacy = true;
            else { r.error = s.error; return; }
        } else if (ping[4] < 2) legacy = true;
    }
    if (legacy) {
        auto packet = s.request(index, 0x81, 7, 0, 0, 0, false);
        if (!packet.empty()) {
            const auto decoded = parse_reply(packet.data(), packet.size(), index);
            if (decoded.kind == ReplyKind::battery) { r.battery = decoded.battery; r.available = true; }
        } else if (s.protocol_error && (s.error_code == 1 || s.error_code == 2)) {
            packet = s.request(index, 0x81, 0x0d, 0, 0, 0, false);
            if (!packet.empty()) r.available = decode_mileage(packet.data() + 4, packet.size() - 4, r.battery);
        }
        r.error = r.available ? L"" : s.error.empty() ? L"Invalid battery response" : s.error;
        return;
    }
    const auto name_feature = s.feature_index(index, 5);
    if (name_feature) {
        const auto size = s.request(index, name_feature, 0);
        if (!size.empty() && size[4] > 0 && size[4] <= 128) {
            std::string name;
            while (name.size() < size[4]) {
                const auto fragment = s.request(index, name_feature, 0x10, static_cast<BYTE>(name.size()));
                if (fragment.empty()) break;
                const auto count = std::min<std::size_t>(fragment.size() - 4, size[4] - name.size());
                name.append(reinterpret_cast<const char*>(fragment.data() + 4), count);
            }
            if (name.size() == size[4]) {
                const auto decoded = utf8_name(name);
                if (!decoded.empty()) r.name = decoded;
            }
        }
    }
    for (const auto id : std::array<USHORT, 4>{0x1004, 0x1000, 0x1001, 0x1f20}) {
        const auto feature = s.feature_index(index, id);
        if (!feature) continue;
        bool percentage = id != 0x1004;
        BYTE levels = 0xff;
        if (id == 0x1004) {
            const auto capabilities = s.request(index, feature, 0);
            if (capabilities.empty()) continue;
            percentage = (capabilities[5] & 2) != 0;
            levels = capabilities[4];
        }
        auto packet = s.request(index, feature, id == 0x1004 ? 0x10 : 0, 0, 0, 0, true, id == 0x1f20);
        if (packet.empty()) continue;
        if (id == 0x1004) packet[5] &= levels;
        if (decode_feature(id, packet.data() + 4, packet.size() - 4, percentage, r.battery)) {
            r.available = true; r.error.clear(); return;
        }
        s.error = id == 0x1f20 ? L"ADC battery measurement unavailable or invalid" : L"Invalid battery payload";
    }
    r.error = s.error.empty() ? L"No supported battery feature (1004, 1000, 1001, 1F20)" : s.error;
}
}

std::wstring windows_error(DWORD code) {
    wchar_t buffer[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring text(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text + L" (Windows " + std::to_wstring(code) + L")";
}
Snapshot read_batteries(HANDLE cancel) {
    Snapshot result;
    const auto endpoints = enumerate(cancel);
    std::map<std::wstring, std::vector<Endpoint>> groups;
    for (const auto& e : endpoints) groups[e.group].push_back(e);
    // Remember only names, never stale battery values. Re-discover features each
    // time so re-pairing a different device at the same receiver slot is safe.
    static std::map<std::wstring, std::wstring> known_names;
    std::map<std::wstring, std::wstring> next_names;
    for (const auto& entry : groups) {
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) break;
        Session session(entry.second, cancel);
        const auto product = entry.second.front().product;
        int slots = receiver_slots(product);
        bool classic_receiver = slots != 0;
        if (!slots && product != 0xc07c && product != 0xc06b) {
            const auto receiver = session.request(0xff, 0x81, 2, 0, 0, 0, false, false, 500);
            if (!receiver.empty()) { slots = 6; classic_receiver = true; }
            else {
                const auto direct = session.ping(0xff, 500);
                if (direct.empty() || direct[4] < 2) slots = 6; // HID++ 2.0 bridge, such as G733.
            }
        }
        if (classic_receiver) {
            const auto info = session.request(0xff, 0x83, 0xb5, 3, 0, 0, false);
            if (info.size() >= 11 && info[10] >= 1 && info[10] <= 6) slots = info[10];
        }
        for (int slot = slots ? 1 : 0xff; slot <= (slots ? slots : 0xff); ++slot) {
            if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) break;
            Reading reading;
            reading.key = entry.first + L"/" + std::to_wstring(slot);
            reading.name = product == 0xc531 ? L"G700 / G700s" : product == 0xc07c ? L"G700s"
                : product == 0xc06b ? L"G700" : L"Logitech device";
            const auto known = known_names.find(reading.key);
            if (known != known_names.end()) reading.name = known->second;
            reading.connection = slots ? (classic_receiver ? L"Wireless receiver, slot " : L"Wireless bridge, slot ")
                + std::to_wstring(slot) : L"Direct HID++";
            if (classic_receiver) {
                const bool bolt = product == 0xc548;
                const auto selector = static_cast<BYTE>(bolt ? 0x60 + slot : 0x40 + slot - 1);
                const auto name = session.request(0xff, 0x83, 0xb5, selector, bolt ? 1 : 0, 0, false);
                const std::size_t length_offset = bolt ? 6 : 5;
                const std::size_t text_offset = length_offset + 1;
                if (name.size() > text_offset && name[length_offset] > 0
                    && name[length_offset] <= name.size() - text_offset) {
                    const auto decoded = utf8_name(std::string(
                        reinterpret_cast<const char*>(name.data() + text_offset), name[length_offset]));
                    if (!decoded.empty()) reading.name = decoded;
                }
            }
            query(session, static_cast<BYTE>(slot), reading,
                product == 0xc07c || product == 0xc06b);
            GetLocalTime(&reading.checked);
            if (reading.available || known != known_names.end() || product == 0xc531
                || product == 0xc07c || product == 0xc06b || reading.name != L"Logitech device") {
                next_names[reading.key] = reading.name;
                result.devices.push_back(std::move(reading));
            }
        }
    }
    known_names = std::move(next_names);
    return result;
}
std::wstring reading_text(const Reading& reading) {
    std::wstring text = reading.name + L" / " + reading.connection + L"\r\n";
    text += reading.available ? describe(reading.battery) : L"Battery unavailable: " + reading.error;
    wchar_t timestamp[64]{};
    swprintf_s(timestamp, L"\r\nChecked: %04u-%02u-%02u %02u:%02u:%02u (local time)\r\n",
        reading.checked.wYear, reading.checked.wMonth, reading.checked.wDay,
        reading.checked.wHour, reading.checked.wMinute, reading.checked.wSecond);
    text += timestamp;
    if (reading.available && reading.battery.percent < 0)
        text += L"The device reports a coarse level or voltage, not a percentage.\r\n";
    return text;
}
}
