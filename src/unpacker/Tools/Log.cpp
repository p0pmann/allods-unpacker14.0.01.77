#include "../Header.h"
#include <cstdarg>
#include <cstdio>

namespace {
char g_path[MAX_PATH] = {0};
CRITICAL_SECTION g_cs;
bool g_ready = false;
}

namespace Log {

void init(const char* dir)
{
    wsprintfA(g_path, "%s\\AllodsUnpacker14.log", dir);
    InitializeCriticalSection(&g_cs);
    g_ready = true;
}

void write(const char* fmt, ...)
{
    if (!g_ready) return;
    char body[2048], line[2200];
    va_list ap; va_start(ap, fmt);
    wvsprintfA(body, fmt, ap);          // no CRT dependency
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    wsprintfA(line, "[%02d:%02d:%02d.%03d] %s\r\n",
              t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
    EnterCriticalSection(&g_cs);
    HANDLE h = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD wr; WriteFile(h, line, lstrlenA(line), &wr, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace Log
