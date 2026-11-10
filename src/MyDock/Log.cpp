#include "Log.h"

#include <Windows.h>

#include <cstdio>

namespace MyDock::Log {
namespace {

constexpr LONGLONG kMaxLogBytes = 256 * 1024;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    }
    return result;
}

void Append(const wchar_t* fileName, bool& checkedSize, const std::wstring& message) {
    wchar_t path[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, path);
    if (tempLength == 0 || tempLength + wcslen(fileName) + 1 >= MAX_PATH) {
        return;
    }
    wcscat_s(path, fileName);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    if (!checkedSize) {
        checkedSize = true;
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(file, &size) && size.QuadPart > kMaxLogBytes) {
            CloseHandle(file);
            file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                return;
            }
        }
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char stamp[32] = {};
    sprintf_s(stamp, "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    const std::string line = stamp + Utf8(message) + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

} // namespace

void Write(const std::wstring& message) {
    static bool checkedSize = false;
    Append(L"MyDock.log", checkedSize, message);
}

#ifdef MYDOCK_DEBUG
void Debug(const std::wstring& message) {
    static bool checkedSize = false;
    Append(L"MyDock-debug.log", checkedSize, message);
}
#endif

} // namespace MyDock::Log
