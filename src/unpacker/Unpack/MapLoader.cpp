// Unpack/MapLoader.cpp -- extraction beyond pack.bin.
//
// pack.bin is only ONE resource database. Per-map content (terrain, regions,
// server objects, lightmaps) lives in ~300 sibling `Maps_<name>.bin` containers
// that the client mounts on world entry -- which never happens at the freeze,
// so none of it is in memory.
//
// The client's own loader is `sub_619850(path, altPath)`: it constructs a
// GameMain::InplaceLoader (the same class as the pack, RTTI-confirmed), and on
// success installs it at resourceManager+0x84 -- exactly the field PackReader
// reads. So mounting a map database makes the whole existing pipeline point at
// it with no other change.
//
// Installing releases the previous container by one reference. The pack is held
// twice, so it survives every swap and cross-container hrefs keep resolving.
#include "../Header.h"

#include <cstring>

namespace {

typedef char (__cdecl* LoadDbFn)(GameStr*, GameStr*);

bool fileExists(const char* p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

namespace MapLoader {

// The databases sit next to pack.bin, under the client's data root -- one level
// up from the executable. Derived rather than configured so nothing is tied to
// one installation.
bool databaseDir(char* out, UINT32 cch)
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
        wsprintfA(probe, "%s\\%s\\Bin\\pack.bin", exe, kData[i]);
        if (!fileExists(probe)) continue;
        wsprintfA(probe, "%s\\%s\\Bin\\", exe, kData[i]);
        lstrcpynA(out, probe, (int)cch);
        return true;
    }
    Log::write("MapLoader: no data\\Bin next to %s", exe);
    return false;
}

// Mount a database. The engine resolves its own paths against the data root, so
// the absolute form is tried first and a couple of relative spellings after it;
// whichever works is remembered for the remaining databases.
bool load(const char* absPath)
{
    static int form = -1;
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    LoadDbFn LoadDb = (LoadDbFn)(base + (Off::FN_LOAD_DB - Off::IMAGE_BASE));

    const char* name = absPath;
    for (const char* p = absPath; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;

    char cand[3][MAX_PATH];
    lstrcpynA(cand[0], absPath, MAX_PATH);
    wsprintfA(cand[1], "../data/Bin/%s", name);
    wsprintfA(cand[2], "Bin/%s", name);

    for (int i = 0; i < 3; ++i) {
        if (form >= 0 && i != form) continue;

        char buf[MAX_PATH];
        lstrcpynA(buf, cand[i], MAX_PATH);
        GameStr path; path.set(buf, lstrlenA(buf));
        char empty[2] = { 0, 0 };
        GameStr alt;  alt.set(empty, 0);

        char ok = 0;
        __try { ok = LoadDb(&path, &alt); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }

        if (ok) {
            if (form != i) { form = i; Log::write("MapLoader: path form %d works (%s)", i, buf); }
            return true;
        }
        if (form >= 0) break;               // the known-good form failed
    }
    return false;
}

} // namespace MapLoader
