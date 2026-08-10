// Unpack/EngineWriter.cpp -- drive the CLIENT'S OWN serializer to emit .xdb XML.
//
// Why this and not a byte-walker: the engine's serializer already knows every
// field's type, element stride, enum name and nesting. Re-deriving that
// externally produced systematically wrong values (bools as huge ints, floats as
// ints, arrays never expanded). Here the output is correct by construction
// because it is the same code that wrote the data.
//
// Recipe (each step was a separate bug until it wasn't):
//   arc = sub_5AACD0(&sink, 2 /*write*/, &resourcePath)
//   ++arc[+4]                       ; take a ref, else the release is a no-op
//   arc->vtable[38](arc, 1)         ; init -- ONE arg; two unbalances the stack
//   arc->vtable[7](arc, 0, 0)       ; root scope (unnamed; renamed on the way out)
//   obj->vtable[slot](obj, arc)     ; THE serializer (__thiscall, arc on stack)
//   arc->vtable[8](arc)             ; end scope
//   sub_5AB860(arc)                 ; flush: renders the DOM -> sink->vtable[0]
//   sub_5872D0(arc, 1, 0xFFFFF)     ; release
//
// The document is then finished in memory: <Header><resourceId> from the pack's
// id map, and the root element name if the engine ignored ours. That replaces
// the whole offline finalize stage.
#include "../Header.h"
#include "../Containers/PackReader.h"
#include "../Containers/Types.h"

#include <cctype>
#include <cstring>

namespace {

const char* XML_DECL = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>";

// The three material template databases render to more than 8 MiB. Match the
// sink ceiling so finalization cannot truncate a document the serializer built.
char   g_doc[64u * 1024 * 1024];
UINT32 g_docLen = 0;
UINT32 g_docEmpty = 0;      // unresolved hrefs in the document just built
char   g_curPath[512];
bool   g_traceFirst = true;      // detail-log the first few of the first container
char   g_onlyMap[64];            // OnlyMap=<substring>: mount just that database
char   g_scope[512];              // normalized resource directory prefix, ending '/'
char   g_containerLabel[64];      // client calls do not preserve register-held arguments
UINT32 g_limit = 0;
MapLoader::Database g_currentMap;
const UINT32 MAX_MAPS = 1024;

void setScopeFilter(const char* scope)
{
    g_scope[0] = 0;
    if (!scope) return;

    while (*scope == '/' || *scope == '\\') ++scope;
    UINT32 n = 0;
    while (*scope && n < sizeof(g_scope) - 2) {
        char c = *scope++;
        if (c == '\\') c = '/';
        if (c == '/' && (n == 0 || g_scope[n - 1] == '/')) continue;
        g_scope[n++] = c;
    }
    while (n && g_scope[n - 1] == '/') --n;
    if (n) g_scope[n++] = '/';
    g_scope[n] = 0;
}

bool pathMatchesScope(const char* path)
{
    if (!g_scope[0]) return true;
    while (*path == '/' || *path == '\\') ++path;
    for (UINT32 i = 0; g_scope[i]; ++i) {
        char c = path[i];
        if (!c) return false;
        if (c == '\\') c = '/';
        if (tolower((unsigned char)c) != tolower((unsigned char)g_scope[i])) return false;
    }
    return true;
}

// The same resource is embedded in many databases, and the copies differ: one
// container may record a cross-container reference that another leaves out. The
// last writer must therefore not clobber a copy that resolved more.
UINT32 emptyHrefsInFile(const char* relPath)
{
    static char probe[MAX_PATH * 2];
    static char buf[1u << 20];
    if (!Fs::fullPath(relPath, probe, sizeof(probe))) return 0xFFFFFFFF;
    HANDLE h = CreateFileA(probe, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0xFFFFFFFF;     // absent: anything wins
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    buf[n] = 0;
    UINT32 c = 0;
    for (const char* s = buf; (s = strstr(s, "href=\"\"")) != nullptr; s += 7) ++c;
    return c;
}

BYTE* imageBase() { return (BYTE*)GetModuleHandleA(nullptr); }
void* rva(DWORD va) { return imageBase() + (va - Off::IMAGE_BASE); }

// Serialize one live object; the rendered XML ends up in Sink.
bool serialize(UINT32 objAddr, int serSlot, const char* rootName, const char* resPath)
{
    auto MakeArchive = (MakeArchiveFn)rva(Off::FN_MAKE_ARCHIVE);

    UINT32 ovt = 0, ser = 0;
    __try {
        ovt = *(UINT32*)objAddr;
        if (serSlot < 0 || serSlot > Off::VTABLE_MAX_SLOT) serSlot = Off::SER_SLOT_DEFAULT;
        ser = *(UINT32*)(ovt + 4 * serSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (ser < Off::CODE_LO || ser >= Off::CODE_HI) return false;

    Sink::begin();

    char pathBuf[512];
    lstrcpynA(pathBuf, resPath, sizeof(pathBuf));
    GameStr ps; ps.set(pathBuf, lstrlenA(pathBuf));

    void* arc = nullptr;
    __try { arc = MakeArchive(Sink::object(), 2, &ps); }
    __except (EXCEPTION_EXECUTE_HANDLER) { arc = nullptr; }
    if (!arc) return false;

    UINT32 avt = *(UINT32*)arc;
    __try {
        // Take a reference exactly as sub_60B5C0 does. Without it sub_5872D0
        // frees nothing, archives + their DOMs accumulate, and the run dies at
        // ~190k resources.
        ++*(UINT32*)((BYTE*)arc + Off::ARCHIVE_REFCNT);
        ((ThisCallI1)(*(UINT32*)(avt + Off::ARC_INIT)))(arc, nullptr, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // The root scope stays UNNAMED. Naming it makes the engine emit the name as
    // an element inside its own document root, so the type ends up nested twice;
    // finish() renames the document root instead.
    int ok = 0;
    __try {
        ok = ((ThisCallI2)(*(UINT32*)(avt + Off::ARC_BEGIN_SCOPE)))(arc, nullptr, nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }

    if (ok) {
        __try { ((ThisCall1)ser)((void*)objAddr, nullptr, arc); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        // endScope must run even if the serializer faulted; leaving a scope open
        // makes every LATER document nest one level deeper (3 GB and a stall).
        __try { ((ThisCall0)(*(UINT32*)(avt + Off::ARC_END_SCOPE)))(arc, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    __try { ((ThisCall0)rva(Off::FN_ARCHIVE_FLUSH))(arc, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { ((ThisCallRel)rva(Off::FN_RELEASE))(arc, nullptr, 1, 0xFFFFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok != 0;
}

void put(const char* s, UINT32 n)
{
    if (g_docLen + n >= sizeof(g_doc)) return;
    memcpy(g_doc + g_docLen, s, n);
    g_docLen += n;
}
void puts_(const char* s) { put(s, (UINT32)strlen(s)); }

void putUint(UINT32 v)
{
    char t[16]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put(&t[--n], 1);
}

// Fold same-directory .txt references down to a bare file name. The engine's
// attribute writer always emits an absolute resource path, but the dialect
// writes a neighbouring text file as just its name (<Name href="BardClassName
// .txt" />). Everything else stays absolute.
void putRelativised(const char* s, UINT32 n, const char* curDir, UINT32 curDirLen)
{
    for (UINT32 i = 0; i < n;) {
        if (i + 7 < n && !memcmp(s + i, "href=\"/", 7)) {
            UINT32 v = i + 7, e = v;
            while (e < n && s[e] != '"') ++e;
            UINT32 len = e - v;
            if (e < n && len > 4 &&
                (s[e - 4] == '.') && (s[e - 3] | 32) == 't' &&
                (s[e - 2] | 32) == 'x' && (s[e - 1] | 32) == 't') {
                UINT32 dir = 0;
                for (UINT32 k = 0; k < len; ++k) if (s[v + k] == '/') dir = k;
                if (dir == curDirLen && (!dir || !memcmp(s + v, curDir, dir))) {
                    put("href=\"", 6);
                    put(s + v + (dir ? dir + 1 : 0), len - (dir ? dir + 1 : 0));
                    put("\"", 1);
                    i = e + 1;
                    continue;
                }
            }
        }
        put(s + i, 1);
        ++i;
    }
}

// A reference the engine could not resolve comes out as href="/" -- it prepends
// the slash to an empty path. Two cases, both measured against the 7.0 oracle:
//   binaryFile / binaryFile2 -- not stored in the object at all; they are a
//     DERIVED self-reference to the resource's own payload file, "<name>.bin"
//     and "<name>.hi.bin" (2955/2955 agreement).
//   everything else -- a NULL reference, which the dialect writes as href="".
//     The element must stay: the engine emitting it at all is what tells us the
//     field exists. Dropping these cost 2,851 elements in 20,000 files
//     (bumpTexture, shadowSettings, mainHandItem, visualItemClass, ...).
// Returns KEEP (not an empty href), WROTE (rewritten), or OMIT.
//
// OMIT applies to ONE case only: a derived payload reference whose file is not
// installed. The oracle does not write <binaryFile2 href="" /> -- it leaves the
// element out entirely (absent in 19,205 of 20,000 files), so an empty one would
// be just as invented as a bogus path. Every other unresolved reference is still
// kept, because there the engine emitting the element is what proves the field.
enum { KEEP, WROTE, OMIT };

// Fields that reference the resource's OWN payload file rather than another
// resource. The engine derives them from the resource path at load time and
// stores nothing, so they always arrive unresolved. Every rule below was
// validated against the whole 7.0 oracle: 3,705 + 1,988 + 3,750 + 3,402
// instances, zero name mismatches. `absolute` follows whichever form the oracle
// uses for that field (terrain dumps are written absolute, the rest bare).
struct Payload { const char* field; const char* suffix; bool cutAtUnderscore; bool absolute; };

const Payload kPayload[] = {
    { "binaryFile",        ".bin",               false, false },
    { "binaryFile2",       ".hi.bin",            false, false },
    { "BinaryFile",        "terrain.bin",        true,  false },
    { "BinaryFileDown",    "terrainDown.bin",    true,  false },
    { "compressedTerrain", "terrainDump.bin",    true,  true  },
    { "extraOcclusion",    "terrainDumpOcc.bin", true,  true  },
};

const Payload* payloadRule(const char* name, UINT32 len)
{
    for (const Payload& p : kPayload)
        if (strlen(p.field) == len && !memcmp(name, p.field, len)) return &p;
    return nullptr;
}

// "<client>\data\" -- payloads sit there under the same relative path as the
// resource that names them.
const char* dataRoot()
{
    static char root[MAX_PATH] = { 0 };
    static bool tried = false;
    if (!tried) {
        tried = true;
        MapLoader::dataRoot(root, MAX_PATH);
    }
    return root;
}

// A derived name is only legitimate if the payload is really there: "the engine
// has the field" is NOT the condition, "the file exists" is. Verified against
// the client's own payload files, not against the 7.0 dump -- of the 79,197
// .hi.bin it ships, all 68,058 whose descriptor we extract are referenced
// correctly, with zero missing and zero wrong; and of the 69,023 payload refs we
// emit, zero point at a file that is not there.
bool payloadExists(const char* derivedName)
{
    const char* root = dataRoot();
    if (!root[0]) return true;             // cannot check -- do not suppress
    char probe[MAX_PATH * 2];
    const char* base = g_curPath;
    for (const char* p = g_curPath; *p; ++p) if (*p == '/') base = p + 1;
    int n = wsprintfA(probe, "\\\\?\\%s", root);
    for (const char* p = g_curPath; p < base; ++p) probe[n++] = (*p == '/') ? '\\' : *p;
    lstrcpynA(probe + n, derivedName, (int)(sizeof(probe) - n));
    for (char* p = probe + n; *p; ++p) if (*p == '/') *p = '\\';
    return GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES;
}

int putEmptyHref(const char* line, UINT32 len)
{
    UINT32 i = 0;
    while (i < len && (unsigned char)line[i] <= ' ') ++i;
    if (i >= len || line[i] != '<') return KEEP;
    UINT32 nameStart = ++i;
    while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '.')) ++i;
    UINT32 nameLen = i - nameStart;
    if (!nameLen) return KEEP;
    const char* tail = " href=\"/\" />";
    UINT32 tailLen = 12;
    if (len - i != tailLen || memcmp(line + i, tail, tailLen)) return KEEP;

    const char* name = line + nameStart;
    const Payload* d = payloadRule(name, nameLen);
    if (!d) {                                        // null reference
        put(line, nameStart);
        put(name, nameLen);
        puts_(" href=\"\" />");
        ++g_docEmpty;
        return WROTE;
    }

    // Stem of the resource's own file name: everything before ".xdb", or -- for
    // the terrain fields -- up to and including the last underscore, so
    // "0_6_MapRegion.xdb" yields "0_6_".
    const char* base = g_curPath;
    for (const char* p = g_curPath; *p; ++p) if (*p == '/') base = p + 1;
    UINT32 stem = (UINT32)strlen(base);
    if (stem > 4 && !memcmp(base + stem - 4, ".xdb", 4)) stem -= 4;
    if (d->cutAtUnderscore) {
        UINT32 cut = stem;
        while (cut && base[cut - 1] != '_') --cut;
        stem = cut;
    }

    char derived[512];
    if (stem >= sizeof(derived) - 32) return KEEP;
    memcpy(derived, base, stem);
    lstrcpynA(derived + stem, d->suffix, (int)(sizeof(derived) - stem));

    if (!payloadExists(derived)) return OMIT;

    put(line, nameStart);                            // indent + '<'
    put(name, nameLen);
    put(" href=\"", 7);
    if (d->absolute) {
        put("/", 1);
        put(g_curPath, (UINT32)(base - g_curPath));  // directory, trailing '/'
    }
    puts_(derived);
    puts_("\" />");
    return WROTE;
}

void putBody(const char* s, UINT32 n)
{
    const char* curDir = g_curPath;
    const char* slash = nullptr;
    for (const char* p = g_curPath; *p; ++p) if (*p == '/') slash = p;
    UINT32 curDirLen = slash ? (UINT32)(slash - curDir) : 0;

    for (UINT32 i = 0; i < n;) {
        UINT32 e = i;
        while (e < n && s[e] != '\n') ++e;
        UINT32 len = e - i;
        UINT32 trimmed = len;
        while (trimmed && (unsigned char)s[i + trimmed - 1] <= ' ') --trimmed;

        int what = putEmptyHref(s + i, trimmed);
        if (what == KEEP) putRelativised(s + i, len, curDir, curDirLen);
        // OMIT swallows the newline too, so no blank line is left behind.
        if (what != OMIT && e < n) put("\n", 1);
        i = (e < n) ? e + 1 : e;
    }
}

// Skip whitespace and return the offset of the first '<' of the root element,
// stepping over an <?xml ?> declaration if the engine emitted one.
UINT32 rootTagAt(const char* s, UINT32 n)
{
    UINT32 i = 0;
    while (i < n && (unsigned char)s[i] <= ' ') ++i;
    if (i + 2 < n && s[i] == '<' && s[i + 1] == '?') {
        while (i < n && s[i] != '>') ++i;
        if (i < n) ++i;
        while (i < n && (unsigned char)s[i] <= ' ') ++i;
    }
    return i;
}

// Assemble the final document: declaration, root element named after the type,
// the <Header> the engine has no way to know about, then the engine's body.
void finish(const char* body, UINT32 n, const char* root, UINT32 resourceId)
{
    g_docLen = 0;
    g_docEmpty = 0;
    UINT32 open = rootTagAt(body, n);
    if (open >= n || body[open] != '<') { put(body, n); return; }

    UINT32 nameEnd = open + 1;
    while (nameEnd < n && body[nameEnd] != '>' && body[nameEnd] != ' ' &&
           body[nameEnd] != '/' && body[nameEnd] != '\r' && body[nameEnd] != '\n') ++nameEnd;
    UINT32 tagEnd = nameEnd;
    while (tagEnd < n && body[tagEnd] != '>') ++tagEnd;
    if (tagEnd >= n) { put(body, n); return; }

    // An empty root ("<Base />") has no body and no closing tag to rename.
    bool selfClosing = (body[tagEnd - 1] == '/');

    puts_(XML_DECL); put("\n", 1);
    put("<", 1); puts_(root);
    if (selfClosing && !resourceId) { puts_(" />\n"); return; }
    put(">", 1);

    if (resourceId) {
        puts_("\n\t<Header>\n\t\t<resourceId>");
        putUint(resourceId);
        puts_("</resourceId>\n\t</Header>");
    }

    if (!selfClosing) {
        // Body between the root's '>' and its closing tag.
        UINT32 bodyStart = tagEnd + 1;
        UINT32 bodyEnd = n;
        while (bodyEnd > bodyStart && (unsigned char)body[bodyEnd - 1] <= ' ') --bodyEnd;
        if (bodyEnd > bodyStart && body[bodyEnd - 1] == '>') {
            UINT32 close = bodyEnd - 1;
            while (close > bodyStart && body[close] != '<') --close;
            if (close > bodyStart && body[close + 1] == '/') bodyEnd = close;
        }
        while (bodyEnd > bodyStart && (unsigned char)body[bodyEnd - 1] <= ' ') --bodyEnd;
        putBody(body + bodyStart, bodyEnd - bodyStart);
    }
    puts_("\n</"); puts_(root); puts_(">\n");
}

} // namespace

namespace EngineWriter {

const char* currentPath() { return g_curPath; }

// Serialize every resource of the container PackReader currently has open.
UINT32 runContainer(const char* label)
{
    lstrcpynA(g_containerLabel, label, sizeof(g_containerLabel));
    if (!Pack::count()) { Log::write("EngineWriter: %s is empty", g_containerLabel); return 0; }
    // Hrefs are best-effort; a container still extracts without them.
    HrefMap::addCurrentContainer();

    // volatile, deliberately: on x86 an SEH __except does NOT restore the
    // non-volatile registers the faulting callee had, so any loop state the
    // compiler parks in EBX/ESI/EDI is garbage after a serializer faults. That
    // turned the loop counter into a random small number and made the pass run
    // forever. Keeping the state in memory costs nothing measurable here.
    volatile UINT32 total = Pack::count();
    Log::write("EngineWriter: %s -> %u resources", g_containerLabel, total);

    volatile UINT32 selected = 0, written = 0, noType = 0, empty = 0, failed = 0, kept = 0;
    for (volatile UINT32 i = 0; i < total; ++i) {
        const Pack::Res* r = Pack::at(i);
        if (!r) break;                               // index can never run past the table
        const char* path = Pack::path(*r);
        if (!pathMatchesScope(path)) continue;
        if (g_limit && selected >= g_limit) break;
        bool trace = (selected < 5) && g_traceFirst;
        ++selected;
        UINT32 objAddr = Pack::blobBase() + r->blobOff;

        UINT32 vt = 0;
        __try { vt = *(UINT32*)objAddr; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = 0; }
        const char* shortName = vt ? Types::shortName(vt) : nullptr;
        if (!shortName) { ++noType; continue; }      // not an NDb object (raw blob)

        lstrcpynA(g_curPath, path, sizeof(g_curPath));
        const char* root = Types::rootName(shortName);
        int slot = SlotMap::slotFor(vt);
        if (trace) Log::write("  [%u] %s vt=%08X %s -> %s slot=%d id=%u",
                              i, path, vt, shortName, root, slot, r->resourceId);

        if (!serialize(objAddr, slot, root, path)) { ++failed; continue; }
        if (trace) Log::write("  [%u] serialized %u bytes", i, Sink::size());
        if (Sink::size() < 8) { ++empty; continue; }

        finish(Sink::data(), Sink::size(), root, r->resourceId);
        if (g_docEmpty && emptyHrefsInFile(path) < g_docEmpty) { ++kept; continue; }
        bool ok = Fs::write(path, g_doc, g_docLen);
        if (trace) Log::write("  [%u] write %s (%u bytes)", i, ok ? "OK" : "FAILED", g_docLen);
        if (ok) ++written; else ++failed;

        if (written && (written % 25000) == 0)
            Log::write("EngineWriter: %u written (at %u/%u)", written, i, total);
    }
    Log::write("EngineWriter: %s done selected=%u written=%u noType=%u empty=%u failed=%u keptBetter=%u",
               g_containerLabel, (UINT32)selected, (UINT32)written, (UINT32)noType,
               (UINT32)empty, (UINT32)failed, (UINT32)kept);
    g_traceFirst = false;
    return written;
}

void runAll(const char* outDir, UINT32 limit, const char* onlyMap, const char* scope)
{
    g_limit = limit;
    lstrcpynA(g_onlyMap, onlyMap ? onlyMap : "", sizeof(g_onlyMap));
    if (g_onlyMap[0]) Log::write("EngineWriter: OnlyMap=%s", g_onlyMap);
    setScopeFilter(scope);
    if (g_scope[0]) Log::write("EngineWriter: Scope=%s", g_scope);
    if (!Pack::open() || !Types::init() || !SlotMap::init() || !Sink::init()) {
        Log::write("EngineWriter: prerequisites missing, nothing written");
        return;
    }

    if (!Fs::setRoot(outDir)) { Log::write("EngineWriter: cannot create %s", outDir); return; }

    HrefMap::setContainerBase(Pack::blobBase(), true);
    volatile UINT32 total = runContainer("pack.bin");

    // Per-map databases are not mounted at the freeze -- mount each in turn and
    // run the same pipeline over it. Installing one only drops a single
    // reference on the previous container, so the pack stays mapped and hrefs
    // that point into it keep resolving.
    // Enumerate FIRST, then mount. A find handle must not be held across the
    // serializer: an x86 __except does not restore the non-volatile registers
    // the faulting callee used, so a handle the compiler parked in one comes
    // back as garbage and the walk dies silently after the first database.
    static MapLoader::Database mapsFound[MAX_MAPS];
    volatile UINT32 nNames = MapLoader::enumerate(mapsFound, MAX_MAPS, g_onlyMap);
    Log::write("EngineWriter: %u map databases to mount", nNames);

    volatile UINT32 maps = 0, mapFail = 0;
    for (volatile UINT32 i = 0; i < nNames; ++i) {
        // The client loader does not preserve every x86 non-volatile register.
        // Copy this iteration's state to globals before crossing that boundary.
        memcpy(&g_currentMap, &mapsFound[i], sizeof(g_currentMap));
        if (!MapLoader::load(g_currentMap.path)) { ++mapFail; continue; }
        Pack::close();
        if (!Pack::open()) { ++mapFail; continue; }
        HrefMap::setContainerBase(Pack::blobBase(), false);
        // The loader drops the fixup stream, so read this stored archive entry
        // in place to recover references into pack.bin that were never bound.
        Fixups::loadSlice(g_currentMap.archive, g_currentMap.offset, g_currentMap.size);
        total += runContainer(g_currentMap.path + 4); // drop "Bin/"
        ++maps;
    }
    Log::write("EngineWriter: all databases processed");

    // Copy out of the volatiles before logging: reading them through varargs was
    // the last thing this function did, and it never got there.
    UINT32 f = total, m = maps, mf = mapFail;
    Log::write("EngineWriter: ALL DONE files=%u maps=%u mapFail=%u", f, m, mf);
    Log::write("EngineWriter: hrefs=%u refFaults=%u unresolved=%u external=%u",
               HrefWriter::written(), HrefWriter::faults(),
               HrefWriter::unresolved(), HrefWriter::external());
}

} // namespace EngineWriter
