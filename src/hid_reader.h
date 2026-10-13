// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "battery.h"
#include "build_config.h"
#include <windows.h>
#include <string>
#include <vector>

namespace logibattery {
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
// One observation from this poll; available=false never reuses old battery data.
struct Reading {
    std::wstring key;
    std::wstring name = L"LogiBattery";
    bool available = false;
    Battery battery;
    std::wstring connection;
    std::wstring error = L"Waiting for the first reading";
    SYSTEMTIME checked{};
};
struct Snapshot {
    std::vector<Reading> devices;
};
// Read-only HID++ discovery and battery queries. cancel is a manual-reset event.
// Call serially from one reader thread: the name cache is intentionally local
// to the reader implementation and is not synchronized for concurrent clients.
Snapshot read_batteries(HANDLE cancel);
std::wstring windows_error(DWORD code);
std::wstring reading_text(const Reading& reading);
}
