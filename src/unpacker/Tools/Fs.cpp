// Tools/Fs.cpp -- write one output file, creating its directories.
//
// Resource paths nest deep enough to pass MAX_PATH, so every path goes through
// the "\\?\" form; the common case (directory already there) costs one
// CreateFile call, directories are only built after it reports one missing.
#include "../Header.h"

#include <cstring>

namespace {

char g_root[MAX_PATH];      // "\\?\E:\...\xdb"

void makeDirs(char* full)
{
    // full is "\\?\<root>\a\b\c.xdb"; create every component under the root.
    size_t rootLen = strlen(g_root);
    for (char* p = full + rootLen + 1; *p; ++p) {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryA(full, nullptr);
        *p = '\\';
    }
}

} // namespace

namespace Fs {

bool setRoot(const char* dir)
{
    wsprintfA(g_root, "\\\\?\\%s", dir);
    CreateDirectoryA(g_root, nullptr);
    return GetFileAttributesA(g_root) != INVALID_FILE_ATTRIBUTES;
}

// The long-path form of an output file, for callers that need to read it back.
bool fullPath(const char* relPath, char* out, UINT32 cch)
{
    if (!g_root[0]) return false;
    int len = wsprintfA(out, "%s\\%s", g_root, relPath);
    if (len <= 0 || (UINT32)len >= cch) return false;
    for (int i = 0; i < len; ++i) if (out[i] == '/') out[i] = '\\';
    return true;
}

bool write(const char* relPath, const void* data, UINT32 n)
{
    char full[MAX_PATH * 2];
    if (!fullPath(relPath, full, sizeof(full))) return false;

    HANDLE h = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        makeDirs(full);
        h = CreateFileA(full, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
    }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, n, &wr, nullptr);
    CloseHandle(h);
    return ok && wr == n;
}

} // namespace Fs
