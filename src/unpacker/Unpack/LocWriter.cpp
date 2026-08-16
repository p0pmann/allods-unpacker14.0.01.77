// Unpack/LocWriter.cpp -- Bin/pack.loc -> sibling localized .txt resources.
//
// pack.loc is a zlib stream containing a key table, a 1:1 value directory,
// and a deduplicated UTF-16LE value pool. Its keys are the exact resource paths
// used by XDB hrefs. Files on disk are UTF-16LE with a BOM and no added NUL or
// newline; authored-empty values are therefore two-byte, BOM-only files.
#include "../Header.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace {

typedef int (__cdecl* UncompressFn)(BYTE*, unsigned long*, const BYTE*, unsigned long);

const int Z_OK = 0;
const int Z_BUF_ERROR = -5;

bool readAt(HANDLE h, UINT32 at, void* dst, UINT32 size)
{
    if (SetFilePointer(h, at, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR) return false;
    DWORD got = 0, n = 0;
    while (got < size && ReadFile(h, (BYTE*)dst + got, size - got, &n, nullptr) && n) got += n;
    return got == size;
}

bool rd(const BYTE* p, UINT32 size, UINT32 at, UINT32* value)
{
    if (at > size || size - at < 4) return false;
    *value = (UINT32)p[at] | ((UINT32)p[at + 1] << 8) |
             ((UINT32)p[at + 2] << 16) | ((UINT32)p[at + 3] << 24);
    return true;
}

bool add(UINT32 a, UINT32 b, UINT32* result)
{
    if (a > 0xFFFFFFFFu - b) return false;
    *result = a + b;
    return true;
}

bool mul(UINT32 a, UINT32 b, UINT32* result)
{
    if (a && b > 0xFFFFFFFFu / a) return false;
    *result = a * b;
    return true;
}

bool endsWithTxt(const std::string& path)
{
    size_t n = path.size();
    return n > 4 && path[n - 4] == '.' &&
           (path[n - 3] | 32) == 't' && (path[n - 2] | 32) == 'x' &&
           (path[n - 1] | 32) == 't';
}

bool safePath(const std::string& path)
{
    if (path.empty() || path[0] == '/' || path[0] == '\\' || !endsWithTxt(path)) return false;
    size_t part = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        char c = (i < path.size()) ? path[i] : '/';
        if (c == ':' || (unsigned char)c < 0x20) return false;
        if (c != '/' && c != '\\') continue;
        size_t n = i - part;
        if (!n || (n == 1 && path[part] == '.') ||
            (n == 2 && path[part] == '.' && path[part + 1] == '.')) return false;
        part = i + 1;
    }
    return true;
}

bool pathMatchesScope(const std::string& path, const char* scope)
{
    if (!scope || !*scope) return true;
    // The five root LocFileList_*.txt resources do not belong to a directory.
    // Full extraction is split into directory-scoped client runs, so publish
    // these tiny root entries on a scoped pass too; later passes overwrite
    // them byte-for-byte.
    if (path.find('/') == std::string::npos &&
        path.find('\\') == std::string::npos) return true;
    size_t n = strlen(scope);
    if (path.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = path[i] == '\\' ? '/' : path[i];
        char b = scope[i] == '\\' ? '/' : scope[i];
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return false;
    }
    return true;
}

BYTE* inflate(const BYTE* packed, UINT32 packedSize, UINT32* rawSize)
{
    HMODULE zlib = GetModuleHandleA("zlib1.dll");
    if (!zlib) zlib = LoadLibraryA("zlib1.dll");
    UncompressFn uncompress = zlib ? (UncompressFn)GetProcAddress(zlib, "uncompress") : nullptr;
    if (!uncompress) {
        Log::write("LocWriter: zlib1.dll!uncompress is unavailable");
        return nullptr;
    }

    UINT32 cap = packedSize <= 64u * 1024 * 1024
        ? packedSize * 8 : 512u * 1024 * 1024;
    if (cap < 64u * 1024 * 1024) cap = 64u * 1024 * 1024;
    while (cap <= 512u * 1024 * 1024) {
        BYTE* raw = (BYTE*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!raw) return nullptr;
        unsigned long actual = cap;
        int result = uncompress(raw, &actual, packed, packedSize);
        if (result == Z_OK) {
            *rawSize = actual;
            return raw;
        }
        VirtualFree(raw, 0, MEM_RELEASE);
        if (result != Z_BUF_ERROR || cap == 512u * 1024 * 1024) {
            Log::write("LocWriter: zlib inflate failed (%d)", result);
            return nullptr;
        }
        cap *= 2;
    }
    return nullptr;
}

} // namespace

namespace LocWriter {

UINT32 run(const MapLoader::Database& loc, UINT32 limit, const char* scope)
{
    HANDLE h = CreateFileA(loc.archive, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::write("LocWriter: cannot open %s", loc.archive);
        return 0;
    }
    BYTE* packed = (BYTE*)VirtualAlloc(nullptr, loc.size,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool readOk = packed && readAt(h, loc.offset, packed, loc.size);
    CloseHandle(h);
    if (!readOk) {
        if (packed) VirtualFree(packed, 0, MEM_RELEASE);
        Log::write("LocWriter: cannot read %s from %s", loc.path, loc.archive);
        return 0;
    }

    UINT32 size = 0;
    BYTE* raw = inflate(packed, loc.size, &size);
    VirtualFree(packed, 0, MEM_RELEASE);
    if (!raw) return 0;

    UINT32 zero = 0, keySeg = 0, cell = 0, count = 0;
    UINT32 keyBytes = 0, keyEnd = 0, sec2 = 0, tag = 0, dirWords = 0, dirBytes = 0;
    UINT32 dir = 0, vsec = 0, poolBytes = 0, pool = 0, poolEnd = 0;
    bool valid = rd(raw, size, 0x00, &zero) && zero == 0 &&
                 rd(raw, size, 0x04, &keySeg) && rd(raw, size, 0x08, &cell) && cell == 8 &&
                 rd(raw, size, 0x0C, &count) && mul(count, 12, &keyBytes) &&
                 add(0x10, keyBytes, &keyEnd) && keyEnd <= size &&
                 add(0x08, keySeg, &sec2) && rd(raw, size, sec2, &tag) && tag == 1 &&
                 rd(raw, size, sec2 + 4, &dirWords) && dirWords == count * 2 &&
                 add(sec2, 8, &dir) && mul(count, 8, &dirBytes) && add(dir, dirBytes, &vsec) &&
                 rd(raw, size, vsec, &tag) && tag == 2 && rd(raw, size, vsec + 4, &poolBytes) &&
                 add(vsec, 8, &pool) && add(pool, poolBytes, &poolEnd) && poolEnd == size;
    if (!valid) {
        Log::write("LocWriter: invalid pack.loc structure (inflated=%u)", size);
        VirtualFree(raw, 0, MEM_RELEASE);
        return 0;
    }

    Log::write("LocWriter: %u entries, %uK inflated, %uK value pool",
               count, size / 1024, poolBytes / 1024);
    UINT32 selected = 0, written = 0, empty = 0, unsafe = 0, invalid = 0, failed = 0;
    std::vector<BYTE> body;
    for (UINT32 j = 0; j < count; ++j) {
        UINT32 rowOffset = 0, row = 0, off = 0, len = 0, id = 0, pathAt = 0;
        if (!mul(j, 12, &rowOffset) || !add(0x10, rowOffset, &row) ||
            !rd(raw, size, row, &off) || !rd(raw, size, row + 4, &len) ||
            !rd(raw, size, row + 8, &id) || id != j ||
            !add(0x10, off, &pathAt) || !add(pathAt, rowOffset, &pathAt) ||
            !len || pathAt > size || len > size - pathAt) { ++invalid; continue; }
        const BYTE* nul = (const BYTE*)memchr(raw + pathAt, 0, len);
        if (!nul) { ++invalid; continue; }
        bool ascii = true;
        for (const BYTE* p = raw + pathAt; p < nul; ++p)
            if (*p < 0x20 || *p >= 0x7F) { ascii = false; break; }
        if (!ascii) { ++invalid; continue; }
        std::string path((const char*)raw + pathAt, (const char*)nul);
        if (!safePath(path)) { ++unsafe; continue; }
        if (!pathMatchesScope(path, scope)) continue;
        if (limit && selected >= limit) break;
        ++selected;

        UINT32 dOffset = 0, d = 0, chars = 0, valueOff = 0, valueBytes = 0, valueAt = 0;
        if (!mul(j, 8, &dOffset) || !add(dir, dOffset, &d) || !rd(raw, size, d, &chars) ||
            !rd(raw, size, d + 4, &valueOff) || !mul(chars, 2, &valueBytes) ||
            !add(pool, valueOff, &valueAt) || valueAt > size || valueBytes > size - valueAt) {
            ++invalid; continue;
        }
        body.resize((size_t)valueBytes + 2);
        body[0] = 0xFF; body[1] = 0xFE;
        if (valueBytes) memcpy(&body[2], raw + valueAt, valueBytes); else ++empty;
        if (Fs::write(path.c_str(), &body[0], (UINT32)body.size())) ++written; else ++failed;
        if (written && written % 50000 == 0) Log::write("LocWriter: %u texts written", written);
    }

    Log::write("LocWriter: done selected=%u written=%u empty=%u unsafe=%u invalid=%u failed=%u",
               selected, written, empty, unsafe, invalid, failed);
    VirtualFree(raw, 0, MEM_RELEASE);
    return written;
}

} // namespace LocWriter
