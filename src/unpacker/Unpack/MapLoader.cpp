// Unpack/MapLoader.cpp -- discover resource databases in installed .pak files.
//
// The installed client keeps its per-map databases as stored ZIP entries under
// data/Packs. Archive names are discovered at runtime. The client VFS can mount
// entries by their virtual Bin/... name. We parse only the ZIP directories so
// Fixups can read the same bytes in place and EngineWriter can validate derived
// payload references without extracting them first; nothing is copied into
// data/Bin.
#include "../Header.h"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

typedef char (__cdecl* LoadDbFn)(GameStr*, GameStr*);

std::vector<std::string> g_payloadEntries;

UINT16 u16(const BYTE* p) { return (UINT16)(p[0] | (p[1] << 8)); }
UINT32 u32(const BYTE* p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) |
                                  ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }

bool readAt(HANDLE h, UINT32 at, void* dst, UINT32 size)
{
    if (SetFilePointer(h, at, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR) return false;
    DWORD got = 0, n = 0;
    while (got < size && ReadFile(h, (BYTE*)dst + got, size - got, &n, nullptr) && n) got += n;
    return got == size;
}

bool startsWithI(const char* s, const char* prefix)
{
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        ++s; ++prefix;
    }
    return true;
}

bool endsWithI(const char* s, const char* suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && startsWithI(s + a - b, suffix);
}

bool bytesEndWithI(const char* s, size_t length, const char* suffix)
{
    size_t suffixLength = strlen(suffix);
    if (length < suffixLength) return false;
    s += length - suffixLength;
    for (size_t i = 0; i < suffixLength; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    return true;
}

bool equalsI(const char* a, const char* b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return *a == *b;
}

std::string normalizedPath(const char* path, size_t length)
{
    while (length && (*path == '/' || *path == '\\')) { ++path; --length; }
    std::string result(path, length);
    for (char& c : result) {
        if (c == '\\') c = '/';
        else c = (char)tolower((unsigned char)c);
    }
    return result;
}

} // namespace

namespace MapLoader {

bool dataRoot(char* out, UINT32 cch)
{
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    char* cut = nullptr;
    for (char* p = exe; *p; ++p) if (*p == '\\') cut = p;
    if (!cut) return false;
    *cut = 0;                              // <client>\bin
    cut = nullptr;
    for (char* p = exe; *p; ++p) if (*p == '\\') cut = p;
    if (!cut) return false;
    *cut = 0;                              // <client>

    static const char* kData[] = { "data", "Data" };
    for (int i = 0; i < 2; ++i) {
        char probe[MAX_PATH];
        wsprintfA(probe, "%s\\%s\\Packs", exe, kData[i]);
        DWORD attrs = GetFileAttributesA(probe);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        wsprintfA(probe, "%s\\%s\\", exe, kData[i]);
        lstrcpynA(out, probe, (int)cch);
        return true;
    }
    Log::write("MapLoader: no data\\Packs directory next to %s", exe);
    return false;
}

UINT32 enumerateArchive(const char* archive, Database* pack, Database* maps,
                        UINT32 mapCap, const char* mapNameFilter)
{
    HANDLE h = CreateFileA(archive, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    UINT32 fileSize = GetFileSize(h, nullptr);
    UINT32 tailSize = fileSize < 65557 ? fileSize : 65557;
    BYTE* tail = (BYTE*)VirtualAlloc(nullptr, tailSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tail || !readAt(h, fileSize - tailSize, tail, tailSize)) {
        if (tail) VirtualFree(tail, 0, MEM_RELEASE);
        CloseHandle(h); return 0;
    }

    const BYTE* eocd = nullptr;
    for (UINT32 i = tailSize - 22; ; --i) {
        if (u32(tail + i) == 0x06054B50) { eocd = tail + i; break; }
        if (!i) break;
    }
    if (!eocd) { VirtualFree(tail, 0, MEM_RELEASE); CloseHandle(h); return 0; }
    UINT32 count = u16(eocd + 10), cdSize = u32(eocd + 12), cdAt = u32(eocd + 16);
    VirtualFree(tail, 0, MEM_RELEASE);
    if (!count || cdAt > fileSize || cdSize > fileSize - cdAt) { CloseHandle(h); return 0; }

    BYTE* cd = (BYTE*)VirtualAlloc(nullptr, cdSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!cd || !readAt(h, cdAt, cd, cdSize)) {
        if (cd) VirtualFree(cd, 0, MEM_RELEASE);
        CloseHandle(h); return 0;
    }

    UINT32 found = 0, pos = 0;
    while (pos + 46 <= cdSize) {
        const BYTE* c = cd + pos;
        if (u32(c) != 0x02014B50) break;
        UINT32 compSize = u32(c + 20), rawSize = u32(c + 24), localAt = u32(c + 42);
        UINT16 method = u16(c + 10), nameLen = u16(c + 28);
        UINT16 extraLen = u16(c + 30), commentLen = u16(c + 32), disk = u16(c + 34);
        UINT32 next = pos + 46u + nameLen + extraLen + commentLen;
        if (next > cdSize) break;

        char name[128];
        UINT32 copy = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
        memcpy(name, c + 46, copy); name[copy] = 0;
        for (char* p = name; *p; ++p) if (*p == '\\') *p = '/';
        bool isPack = nameLen < sizeof(name) && equalsI(name, "Bin/pack.bin");
        bool isMap = nameLen < sizeof(name) && startsWithI(name, "Bin/Maps_") &&
                     endsWithI(name, ".bin") &&
                     (!mapNameFilter || !*mapNameFilter || strstr(name, mapNameFilter));
        if (bytesEndWithI((const char*)c + 46, nameLen, ".bin"))
            g_payloadEntries.push_back(normalizedPath((const char*)c + 46, nameLen));
        if (method == 0 && disk == 0 && compSize == rawSize && (isPack || isMap)) {
            BYTE local[30];
            if (localAt <= fileSize - sizeof(local) && readAt(h, localAt, local, sizeof(local)) &&
                u32(local) == 0x04034B50) {
                UINT32 dataAt = localAt + 30u + u16(local + 26) + u16(local + 28);
                if (dataAt <= fileSize && compSize <= fileSize - dataAt) {
                    Database* dst = nullptr;
                    if (isPack && !pack->path[0]) dst = pack;
                    else if (isMap && found < mapCap) dst = &maps[found++];
                    if (dst) {
                        lstrcpynA(dst->archive, archive, MAX_PATH);
                        lstrcpynA(dst->path, name, sizeof(dst->path));
                        dst->offset = dataAt;
                        dst->size = compSize;
                    }
                }
            }
        }
        pos = next;
    }
    VirtualFree(cd, 0, MEM_RELEASE);
    CloseHandle(h);
    return found;
}

UINT32 enumerate(Database* pack, Database* maps, UINT32 mapCap,
                 const char* mapNameFilter)
{
    memset(pack, 0, sizeof(*pack));
    g_payloadEntries.clear();
    char root[MAX_PATH];
    if (!dataRoot(root, MAX_PATH)) return 0;

    char pattern[MAX_PATH];
    wsprintfA(pattern, "%sPacks\\*.pak", root);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        Log::write("MapLoader: no .pak archives found in %sPacks", root);
        return 0;
    }

    UINT32 found = 0, archives = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char archive[MAX_PATH];
        wsprintfA(archive, "%sPacks\\%s", root, fd.cFileName);
        ++archives;
        found += enumerateArchive(archive, pack, maps + found,
                                  mapCap - found, mapNameFilter);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    std::sort(g_payloadEntries.begin(), g_payloadEntries.end());
    g_payloadEntries.erase(std::unique(g_payloadEntries.begin(), g_payloadEntries.end()),
                           g_payloadEntries.end());
    Log::write("MapLoader: searched %u archives, pack=%s, payloads=%u", archives,
               pack->path[0] ? pack->path : "not found",
               (UINT32)g_payloadEntries.size());
    return found;
}

bool hasPayload(const char* relativePath)
{
    std::string path = normalizedPath(relativePath, strlen(relativePath));
    return std::binary_search(g_payloadEntries.begin(), g_payloadEntries.end(), path);
}

bool load(const char* virtualPath)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    LoadDbFn LoadDb = (LoadDbFn)(base + (Off::FN_LOAD_DB - Off::IMAGE_BASE));
    char buf[MAX_PATH];
    lstrcpynA(buf, virtualPath, MAX_PATH);
    GameStr path; path.set(buf, lstrlenA(buf));
    char empty[2] = { 0, 0 };
    GameStr alt; alt.set(empty, 0);
    char ok = 0;
    __try { ok = LoadDb(&path, &alt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) Log::write("MapLoader: mount failed (%s)", virtualPath);
    return ok != 0;
}

} // namespace MapLoader
