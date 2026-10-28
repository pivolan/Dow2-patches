// EliteAI.dll - new computer AI for DoW2 Retribution / Elite without touching the game archives.
// Every Lua chunk goes through lua_load (LuaConfig.dll, stdcall build). When the engine loads an AI script and
// Elite_DLLs\EliteAI\<same path> exists, the engine gets our text instead of the one from the archive.
// No file in the folder = the original Elite AI; the archives and the lobby data (datacrc) stay as they are.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

typedef const char *(__stdcall *LuaReader)(void *L, void *data, size_t *size);
typedef int (__stdcall *LuaLoadFn)(void *L, LuaReader reader, void *data, const char *chunkname);
static LuaLoadFn g_loadOrig;
// a few Lua 5.1 calls from LuaConfig.dll (stdcall build): to give the AI scripts a log - aitrace writes nothing without -dev
typedef int (__stdcall *LuaCFn)(void *L);
static void (__stdcall *p_pushcclosure)(void *L, LuaCFn f, int n);
static void (__stdcall *p_setfield)(void *L, int idx, const char *k);
static const char *(__stdcall *p_tolstring)(void *L, int idx, size_t *len);
static int (__stdcall *p_gettop)(void *L);
static void (__stdcall *p_pushstring)(void *L, const char *s);
static void (__stdcall *p_pushnil)(void *L);
static void (__stdcall *p_getfield)(void *L, int idx, const char *k);
static void (__stdcall *p_settop)(void *L, int idx);
static double (__stdcall *p_tonumber)(void *L, int idx);
static wchar_t g_ini[MAX_PATH];
#define LUA_GLOBALSINDEX (-10002)
static wchar_t g_dir[MAX_PATH];                 // ...\Elite_DLLs\ (where this DLL lives)
static CRITICAL_SECTION g_lk;
static int g_verbose;

static void logmsg(const char *fmt, ...) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsEliteAI.log", g_dir);
    EnterCriticalSection(&g_lk);
    FILE *f = _wfopen(p, L"a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_lk);
}

// "data:simulation/ai/default.ai", "@Simulation\AI\Personality\orks\expert.ai" ... -> "simulation\ai\default.ai" (lower case)
static int ai_rel(const char *chunk, char *out, int cap) {
    if (!chunk) return 0;
    char b[512]; int n = 0;
    for (const char *s = chunk; *s && n < (int)sizeof b - 1; s++) b[n++] = (*s == '/') ? '\\' : (char)tolower((unsigned char)*s);
    b[n] = 0;
    const char *p = strstr(b, "simulation\\ai\\");
    if (!p || strstr(p, "..")) return 0;
    snprintf(out, cap, "%s", p); return 1;
}

static int __stdcall lua_log(void *L) {          // eliteai_log(a, b, ...) -> one line in EliteAI.log
    char line[1024]; int n = 0, top = p_gettop(L);
    for (int i = 1; i <= top && n < (int)sizeof line - 2; i++) {
        size_t len = 0; const char *s = p_tolstring(L, i, &len);
        n += snprintf(line + n, sizeof line - n, "%s%s", i > 1 ? " " : "", s ? s : "nil");
    }
    line[sizeof line - 1] = 0; logmsg("AI: %s", line); return 0;
}
static int __stdcall lua_cfg(void *L) {          // eliteai_cfg("Key") -> string from EliteAI.ini [Lua], or nil (quick tuning, A/B tests)
    const char *k = p_tolstring(L, 1, NULL); char v[256] = "";
    char ini[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, g_ini, -1, ini, sizeof ini, NULL, NULL);
    if (k) GetPrivateProfileStringA("Lua", k, "", v, sizeof v, ini);
    if (v[0]) p_pushstring(L, v); else p_pushnil(L);
    return 1;
}
// combat director: the AI state can't give orders (no Command_* there); it queues them here and a SCAR rule
// (EliteAI\append\scar\elite\elite.scar) takes them once a second and issues them with Cmd_*.
typedef struct { double id, x, y, z, mode; } Order;
static Order g_orders[1024]; static int g_nOrders;
static int __stdcall lua_order(void *L) {          // eliteai_order(squad_world_id, x, y, z, mode)  mode 1 move, 2 attack-move, 3 retreat
    EnterCriticalSection(&g_lk);
    if (g_nOrders < 1024) { Order *o = &g_orders[g_nOrders++];
        o->id = p_tonumber(L, 1); o->x = p_tonumber(L, 2); o->y = p_tonumber(L, 3); o->z = p_tonumber(L, 4); o->mode = p_tonumber(L, 5); }
    LeaveCriticalSection(&g_lk); return 0;
}
static int __stdcall lua_take(void *L) {           // eliteai_take_orders() -> "id x y z mode;..." (empty string = none)
    static char buf[1024 * 64]; int n = 0; buf[0] = 0;
    EnterCriticalSection(&g_lk);
    for (int i = 0; i < g_nOrders && n < (int)sizeof buf - 128; i++)
        n += snprintf(buf + n, sizeof buf - n, "%.0f %.2f %.2f %.2f %.0f;", g_orders[i].id, g_orders[i].x, g_orders[i].y, g_orders[i].z, g_orders[i].mode);
    g_nOrders = 0; LeaveCriticalSection(&g_lk);
    p_pushstring(L, buf); return 1;
}
static void give_log(void *L) {
    if (!p_pushcclosure || !p_setfield) return;
    p_pushcclosure(L, lua_log, 0); p_setfield(L, LUA_GLOBALSINDEX, "eliteai_log");
    p_pushcclosure(L, lua_cfg, 0); p_setfield(L, LUA_GLOBALSINDEX, "eliteai_cfg");
    if (p_tonumber) {
        p_pushcclosure(L, lua_order, 0); p_setfield(L, LUA_GLOBALSINDEX, "eliteai_order");
        p_pushcclosure(L, lua_take, 0); p_setfield(L, LUA_GLOBALSINDEX, "eliteai_take_orders");
    }
    if (p_getfield && p_settop) {                 // AI scripts run in their own environment: also via the shared string table
        p_getfield(L, LUA_GLOBALSINDEX, "string");
        p_pushcclosure(L, lua_log, 0); p_setfield(L, -2, "eliteai_log");
        p_pushcclosure(L, lua_cfg, 0); p_setfield(L, -2, "eliteai_cfg");
        if (p_tonumber) { p_pushcclosure(L, lua_order, 0); p_setfield(L, -2, "eliteai_order");
                          p_pushcclosure(L, lua_take, 0); p_setfield(L, -2, "eliteai_take_orders"); }
        p_settop(L, -2);
    }
}

typedef struct { const char *buf; size_t len; int done; } BufState;
static const char *__stdcall buf_reader(void *L, void *data, size_t *size) {
    BufState *s = (BufState *)data; (void)L;
    if (s->done) { *size = 0; return NULL; }
    s->done = 1; *size = s->len; return s->buf;
}

static char *read_file(const wchar_t *p, size_t *len) {
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(f, NULL), got = 0; char *b = NULL;
    if (sz != INVALID_FILE_SIZE && sz < (8u << 20) && (b = malloc(sz + 1))) {
        if (!ReadFile(f, b, sz, &got, NULL) || got != sz) { free(b); b = NULL; } else { b[sz] = 0; *len = sz; }
    }
    CloseHandle(f); return b;
}

static int append_load(void *L, LuaReader reader, void *data, const char *chunkname, const char *add, size_t alen) {
    size_t cap = 1 << 16, n = 0; char *all = malloc(cap + alen + 64);
    for (;;) { size_t sz = 0; const char *part = reader(L, data, &sz); if (!part || !sz) break;
        if (n + sz + alen + 64 > cap) { cap = (n + sz) * 2; all = realloc(all, cap + alen + 64); }
        memcpy(all + n, part, sz); n += sz; }
    all[n++] = '\n'; memcpy(all + n, add, alen); n += alen;
    BufState st = { all, n, 0 }; give_log(L);
    int r = g_loadOrig(L, buf_reader, &st, chunkname); free(all); return r;
}
// Online the new AI must stay out: the AI and SCAR scripts run on every machine of the match, so one player with it
// would desync the game for everyone. The game's own test for "live multiplayer" (see docs/OBSERVER_LAG.md): the
// network session's flags & 0x558. Skirmish, campaign and replays have no online session.
#define G_SESSION   0x01335678
#define ONLINE_MASK 0x558
static int online(void) {
    DWORD *g = (DWORD *)G_SESSION;
    if (IsBadReadPtr(g, 4) || !*g || IsBadReadPtr((void *)(*g + 4), 4)) return 0;
    return (*(DWORD *)(*g + 4) & ONLINE_MASK) != 0;
}
static int __stdcall load_hook(void *L, LuaReader reader, void *data, const char *chunkname) {
    char rel[400];
    if (online()) {                                    // live multiplayer: Elite's own scripts, untouched
        static DWORD said; if (GetTickCount() - said > 60000) { said = GetTickCount(); logmsg("online game: the new AI stays out (Elite's own AI)"); }
        return g_loadOrig(L, reader, data, chunkname);
    }
    if (chunkname) {                                   // SCAR scripts: EliteAI\append\scar\<name> is appended (the order queue of the new AI)
        char b[400]; int n = 0;
        for (const char *s = chunkname; *s && n < 398; s++) b[n++] = (*s == '/') ? '\\' : (char)tolower((unsigned char)*s);
        b[n] = 0; const char *p = strstr(b, "simulation\\scar\\");
        if (p && !strstr(p, "..")) {
            wchar_t f[MAX_PATH]; _snwprintf(f, MAX_PATH, L"%lsEliteAI\\append\\%hs", g_dir, p + 11);
            size_t alen = 0; char *add = read_file(f, &alen);
            if (add) { int r = append_load(L, reader, data, chunkname, add, alen); logmsg("%s + append (%u bytes), load %d", chunkname, (unsigned)alen, r); free(add); return r; }
        }
    }
    if (ai_rel(chunkname, rel, sizeof rel)) {
        wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsEliteAI\\%hs", g_dir, rel);
        size_t len = 0; char *buf = read_file(p, &len);
        if (buf) {
            BufState st = { buf, len, 0 }; give_log(L);
            int r = g_loadOrig(L, buf_reader, &st, chunkname);
            logmsg("ai script %s -> ours (%u bytes), load %d", chunkname, (unsigned)len, r);
            free(buf); return r;
        }
        if (!strcmp(rel, "simulation\\ai\\default.ai")) {           // Elite's default.ai + our module at its end
            _snwprintf(p, MAX_PATH, L"%lsEliteAI\\main.ai", g_dir);
            size_t mlen = 0; char *mod = read_file(p, &mlen);
            if (mod) {
                size_t cap = 1 << 16, n = 0; char *all = malloc(cap + mlen + 64);
                for (;;) {                                           // the original chunk, as the engine's reader gives it
                    size_t sz = 0; const char *part = reader(L, data, &sz);
                    if (!part || !sz) break;
                    if (n + sz + mlen + 64 > cap) { cap = (n + sz) * 2; all = realloc(all, cap + mlen + 64); }
                    memcpy(all + n, part, sz); n += sz;
                }
                // Elite's default.ai ends with a top-level "InitializeAI()" call: our module goes right before it,
                // so it can wrap InitializeAI / adjust what the AI is built from; no such call -> at the end
                all[n] = 0; size_t at = n; char *q = all, *hit;
                while ((hit = strstr(q, "InitializeAI()"))) {
                    if ((hit == all || hit[-1] == '\n') ) at = hit - all;     // line start = the call, not "function InitializeAI()"
                    q = hit + 1;
                }
                static const char sep[] = "\n-- ===== EliteAI main.ai =====\n", sep2[] = "\n-- ===== /EliteAI =====\n";
                size_t tail = n - at; char *rest = malloc(tail + 1); memcpy(rest, all + at, tail);
                n = at; memcpy(all + n, sep, sizeof sep - 1); n += sizeof sep - 1; memcpy(all + n, mod, mlen); n += mlen;
                if (n + sizeof sep2 + tail > cap + mlen + 64) all = realloc(all, n + sizeof sep2 + tail + 1);
                memcpy(all + n, sep2, sizeof sep2 - 1); n += sizeof sep2 - 1; memcpy(all + n, rest, tail); n += tail; free(rest);
                BufState st = { all, n, 0 }; give_log(L);
                int r = g_loadOrig(L, buf_reader, &st, chunkname);
                logmsg("default.ai + main.ai (%u + %u bytes), load %d", (unsigned)(n - mlen), (unsigned)mlen, r);
                free(all); free(mod); return r;
            }
        }
        if (g_verbose) logmsg("ai script %s -> Elite's", chunkname);
    } else if (g_verbose > 1) logmsg("lua chunk %s", chunkname ? chunkname : "(null)");
    return g_loadOrig(L, reader, data, chunkname);
}

static const unsigned char LOAD_PRO[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};   // push ebp; mov ebp,esp; sub esp,14h
static int install(void) {
    HMODULE lc = GetModuleHandleW(L"LuaConfig.dll");
    unsigned char *t = lc ? (unsigned char *)GetProcAddress(lc, "_lua_load@16") : NULL;
    if (!t) { logmsg("LuaConfig.dll / lua_load not found"); return 0; }
    p_pushcclosure = (void *)GetProcAddress(lc, "_lua_pushcclosure@12"); p_setfield = (void *)GetProcAddress(lc, "_lua_setfield@12");
    p_tolstring = (void *)GetProcAddress(lc, "_lua_tolstring@12"); p_gettop = (void *)GetProcAddress(lc, "_lua_gettop@4");
    p_pushstring = (void *)GetProcAddress(lc, "_lua_pushstring@8"); p_pushnil = (void *)GetProcAddress(lc, "_lua_pushnil@4");
    p_getfield = (void *)GetProcAddress(lc, "_lua_getfield@12"); p_settop = (void *)GetProcAddress(lc, "_lua_settop@8");
    p_tonumber = (void *)GetProcAddress(lc, "_lua_tonumber@8");
    if (!p_pushcclosure || !p_setfield || !p_tolstring || !p_gettop || !p_pushstring || !p_pushnil) { p_pushcclosure = NULL; logmsg("no Lua API for eliteai_log"); }
    if (memcmp(t, LOAD_PRO, 6)) { logmsg("lua_load prologue differs (%02X %02X %02X ...) - not hooking", t[0], t[1], t[2]); return 0; }
    unsigned char *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, t, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (DWORD)(t + 6) - (DWORD)(tr + 11);
    g_loadOrig = (LuaLoadFn)tr;
    DWORD o; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &o);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)load_hook - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 6);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h); InitializeCriticalSection(&g_lk);
    wchar_t mx[64]; _snwprintf(mx, 64, L"Local\\EliteAI_%lu", GetCurrentProcessId());   // one copy per game (Elite's injector + dow2-patches)
    CreateMutexW(NULL, FALSE, mx); if (GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
    // data always in <game>\Elite_DLLs\ - also when loaded from dow2-patches (-dev runs: Elite's injector skips its DLLs there)
    GetModuleFileNameW(NULL, g_dir, MAX_PATH); wchar_t *s = wcsrchr(g_dir, L'\\'); if (s) wcscpy(s + 1, L"Elite_DLLs\\");
    _snwprintf(g_ini, MAX_PATH, L"%lsEliteAI.ini", g_dir);
    g_verbose = GetPrivateProfileIntW(L"EliteAI", L"Verbose", 0, g_ini);
    logmsg("EliteAI 0.5 start (skirmish only, never online), hook %s", install() ? "installed" : "NOT installed");
    return TRUE;
}
