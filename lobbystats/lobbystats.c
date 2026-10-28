// LobbyStats.dll - Relic rating right in the DoW2 Retribution / Elite lobby.
//
// How it works: we hook the engine's "call a Flash function" call (FUN_0045db6c, __thiscall
// Invoke(method, flags, GFxValue* args, count)). When the engine sends Receive_PopulatePlayerList
// to the lobby movie, we append the rating to the name in each player slot and replace
// the slot tooltip (m_tooltip field) with "rating / wins / games / hours".
// The main menu gets the own rating after the name too (Receive_SetGamerTag), and the stats tooltip on hover, which
// we draw ourselves through Direct3D9: the menu has no tooltip there.
// Data is fetched by a background thread from the stats service (<Server>/stats/api/lobby), which reads the public Relic API.
// A name is first shown without numbers; they appear on the next lobby update (join, ready,
// race change) and immediately in later lobbies (disk cache next to the DLL).
// Players are matched by SteamID64 from the slot data (name is only a fallback).
// It only reads the lobby: no kicking, no chat commands, no extra menus.
//
// Works only with DOW2.exe 3.19 (Steam): the prologue bytes are checked before installing.
#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>
#define COBJMACROS
#include <d3d9.h>

#define HOOK_ADDR   0x0045DB6C
#define HOOK_RESUME 0x0045DB71
static const unsigned char PROLOGUE[5] = {0xB8, 0x59, 0x70, 0xEF, 0x00};   // mov eax, 0x00EF7059

// stats service address: at build time -DSTATS_HOST=L\"host\" or in ini [LobbyStats] Server=host
#ifndef STATS_HOST
#define STATS_HOST L""
#endif
static wchar_t g_server[128] = STATS_HOST;
#define SERVER_PATH "/stats/api/lobby"

// GFxValue (Scaleform 2.x in the game): 16 bytes. Type in the first word, value at +8.
typedef struct { int type; int pad; union { double num; unsigned char b; const wchar_t *ws; const char *s; } v; } GFxValue;
enum { GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_WSTRING = 5 };
enum { HEADER_ARGS = 5, SLOT_ARGS = 20, A_HASUSER = 0, A_SLOTINDEX = 3, A_SLOTTYPE = 4, A_NAME = 5, A_TOOLTIP = 19 };
enum { SLOT_LOCAL = 0, SLOT_REMOTE = 1 };

typedef struct { wchar_t name[64]; int known, absent; int rating, wins, losses, hours; DWORD fetched; } Entry;  // absent: server said "no Relic profile"
#define MAX_ENTRIES 256
static Entry g_cache[MAX_ENTRIES]; static int g_ncache;
static CRITICAL_SECTION g_lock; static HANDLE g_wake;
static wchar_t g_pending[16][64]; static int g_npending;
static wchar_t g_dir[MAX_PATH];
static int g_showInName = 1, g_stats = 1;                  // g_stats=0: leave names and tooltips alone

static void logmsg(const char *fmt, ...) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsLobbyStats.log", g_dir);
    FILE *f = _wfopen(p, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static Entry *find(const wchar_t *name) {
    for (int i = 0; i < g_ncache; i++) if (!wcscmp(g_cache[i].name, name)) return &g_cache[i];
    return NULL;
}
static Entry *find_or_add(const wchar_t *name) {
    Entry *e = find(name); if (e) return e;
    if (g_ncache == MAX_ENTRIES) { memmove(g_cache, g_cache + 1, sizeof(Entry) * (MAX_ENTRIES - 1)); g_ncache--; }
    e = &g_cache[g_ncache++]; memset(e, 0, sizeof *e); wcsncpy(e->name, name, 63); return e;
}

// ---------- disk cache: lines "name\trating\twins\tlosses\thours" in UTF-8 ----------
static void cache_path(wchar_t *p) { _snwprintf(p, MAX_PATH, L"%lsLobbyStats.cache", g_dir); }
static void parse_lines(char *buf, int from_disk) {
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[6] = {0}; int n = 0; char *p = line;
        while (n < 6) { f[n++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (n < 2 || !*f[0]) continue;
        wchar_t name[64]; if (!MultiByteToWideChar(CP_UTF8, 0, f[0], -1, name, 64)) continue;
        Entry *e = find_or_add(name);
        e->fetched = from_disk ? 0 : GetTickCount();
        if (n >= 4 && strcmp(f[1], "-")) {
            e->known = 1; e->absent = 0; e->rating = atoi(f[1]); e->wins = atoi(f[2]); e->losses = atoi(f[3]);
            e->hours = (n >= 5 && strcmp(f[4], "-")) ? atoi(f[4]) : -1;
        } else if (!from_disk) { e->known = 0; e->absent = 1; }
    }
}
static void load_cache(void) {
    wchar_t p[MAX_PATH]; cache_path(p); FILE *f = _wfopen(p, L"rb"); if (!f) return;
    static char buf[64 * 1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    parse_lines(buf, 1);
}
static void save_cache(void) {
    wchar_t p[MAX_PATH]; cache_path(p); FILE *f = _wfopen(p, L"wb"); if (!f) return;
    for (int i = 0; i < g_ncache; i++) if (g_cache[i].known) {
        char name[256]; WideCharToMultiByte(CP_UTF8, 0, g_cache[i].name, -1, name, sizeof name, NULL, NULL);
        fprintf(f, "%s\t%d\t%d\t%d\t%d\n", name, g_cache[i].rating, g_cache[i].wins, g_cache[i].losses, g_cache[i].hours);
    }
    fclose(f);
}

// ---------- network ----------
static void urlenc(const wchar_t *w, char *out, int cap) {
    char u[256]; WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    int o = 0;
    for (unsigned char *c = (unsigned char *)u; *c && o < cap - 4; c++) {
        if (isalnum(*c) || strchr("-_.~", *c)) out[o++] = *c;
        else o += sprintf(out + o, "%%%02X", *c);
    }
    out[o] = 0;
}
static int http_get(const char *pathq, char *out, int cap) {
    int got = 0; wchar_t wpath[4096]; MultiByteToWideChar(CP_UTF8, 0, pathq, -1, wpath, 4096);
    HINTERNET s = WinHttpOpen(L"LobbyStats/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 5000, 5000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, g_server, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", wpath, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD n;
        while (got < cap - 1 && WinHttpReadData(r, out + got, cap - 1 - got, &n) && n) got += n;
    } else got = -1;
    if (got >= 0) out[got] = 0;
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return got;
}
static DWORD WINAPI worker(LPVOID _) {
    static char resp[64 * 1024], q[4096];
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Sleep(300);                                  // collect all names from one lobby update into a single request
        wchar_t names[16][64]; int n;
        EnterCriticalSection(&g_lock);
        n = g_npending; memcpy(names, g_pending, sizeof(wchar_t) * 64 * n); g_npending = 0;
        LeaveCriticalSection(&g_lock);
        if (!n) continue;
        int o = sprintf(q, "%s?", SERVER_PATH);
        for (int i = 0; i < n; i++) { char e[400]; urlenc(names[i], e, sizeof e); o += snprintf(q + o, sizeof q - o, "%sn=%s", i ? "&" : "", e); }
        int got = http_get(q, resp, sizeof resp);
        logmsg("lookup %d names -> %d bytes", n, got);
        EnterCriticalSection(&g_lock);
        if (got > 0) { parse_lines(resp, 0); save_cache(); }
        else for (int i = 0; i < n; i++) find_or_add(names[i])->fetched = GetTickCount();   // don't hammer the server on error
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// ---------- game functions (DOW2.exe 3.19, fixed addresses, no ASLR) ----------
#define G_SESSION      0x01335014     // global network manager; vtbl+0x24 -> match/lobby object
#define FN_INFO_CTOR   0x0040172D     // __stdcall(GameInfo*)
#define FN_INFO_DTOR   0x00401794
#define FN_ST_CTOR     0x0041F1E5     // __stdcall(StationInfo*)
#define FN_ST_DTOR     0x00401A14
#define INFO_SLOTS     0x70           // GameInfo: start of the slot array
#define INFO_NSLOTS    0x78           //           number of slots
#define SLOT_SIZE      0xC8           // slot: +8/+0xC - player's SteamID64
typedef void  (__stdcall *Fn1)(void *);
typedef void *(__fastcall *GetMatchFn)(void *self, void *edx);
typedef char  (__stdcall *GetInfoFn)(void *self, void *info);

static int g_hooked;
static void *match_obj(void) {
    void **mgr = *(void ***)G_SESSION; if (!mgr) return NULL;
    return ((GetMatchFn)(*(void ***)mgr)[0x24 / 4])(mgr, NULL);
}
// player IDs by slot POSITION - the way the game itself builds the lobby list (InvokePopulatePlayerList 0x4911C4):
// GameInfo (match vtbl+4): slots *(+0x70), count *(+0x78), stride 0xC8; in the slot +8 = player ID (profile ID, not SteamID; -1 = empty),
// +0x98 = position (the UI receives it as the slot number). Own ID - same as the game: 0x401607 + 0x53A400, ID in the first 8 bytes.
static int ids_by_position(unsigned long long *ids, int cap) {
    memset(ids, 0, sizeof(*ids) * cap);
    if (!g_hooked) return 0;
    void *m = match_obj(); if (!m) return 0;
    static __attribute__((aligned(16))) unsigned char info[0x2000];
    memset(info, 0, sizeof info); ((Fn1)FN_INFO_CTOR)(info);
    int n = 0, found = 0;
    if (((GetInfoFn)(*(void ***)m)[4 / 4])(m, info)) {
        unsigned char *base = *(unsigned char **)(info + INFO_SLOTS);
        n = *(int *)(info + INFO_NSLOTS);
        if (base && n > 0 && n <= 64 && !IsBadReadPtr(base, n * SLOT_SIZE))
            for (int i = 0; i < n; i++) {
                unsigned char *sl = base + i * SLOT_SIZE;
                unsigned long long id = *(unsigned long long *)(sl + 8); int pos = *(int *)(sl + 0x98);
                if (id != ~0ULL && id && pos >= 0 && pos < cap) { ids[pos] = id; found++; }
            }
    }
    ((Fn1)FN_INFO_DTOR)(info);
    return found;
}

typedef struct { wchar_t key[64], name[64]; unsigned long long id; int st, pos, obs; } Seen;
static Seen g_seen[16]; static int g_nseen;
static unsigned long long g_localId;                        // own ID (from the slot the game marked as own)
static int g_maxPlayers;                                    // player seats; positions beyond are observers

// ---------- hook ----------
typedef char (__fastcall *InvokeFn)(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count);
static InvokeFn g_orig;
static wchar_t g_nameBuf[16][128], g_tipBuf[16][320];

// the stats tooltip (320 wchar_t): lobby slots and the main menu name
static void format_tip(wchar_t *out, const wchar_t *name, const Entry *e, int wr, int games) {
    wchar_t hours[48] = L"";
    if (e->hours >= 0) _snwprintf(hours, 47, L"\nSteam hours: %d", e->hours);
    _snwprintf(out, 319, L"%ls\nRelic rating: %d\nWin rate: %d%%  (%d W / %d L)\nGames: %d%ls",
               name, e->rating, wr, e->wins, e->losses, games, hours);
    out[319] = 0;
}

static void decorate(GFxValue *args, unsigned count) {
    if (count < HEADER_ARGS + SLOT_ARGS || (count - HEADER_ARGS) % SLOT_ARGS) {
        static int warned; if (!warned++) logmsg("PopulatePlayerList: unexpected arg count %u, not touching", count);
        return;
    }
    int isHost = args[1].type == GFX_BOOL && args[1].v.b;
    g_nseen = 0;
    unsigned long long ids[32] = {0}; int nids = ids_by_position(ids, 32) ? 32 : 0;
    // own ID - from the slot the game marked as "own" (type 0); calling local_user_id() from 0.6.8 crashed the game for a user
    g_localId = 0;
    for (unsigned i = 0; i < (count - HEADER_ARGS) / SLOT_ARGS && i < 16; i++) {
        GFxValue *a0 = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a0[A_SLOTTYPE].type == GFX_NUMBER && (int)a0[A_SLOTTYPE].v.num == SLOT_LOCAL && a0[A_SLOTINDEX].type == GFX_NUMBER) {
            int p0 = (int)a0[A_SLOTINDEX].v.num; if (p0 >= 0 && p0 < nids && ids[p0]) g_localId = ids[p0];
        }
    }
    unsigned slots = (count - HEADER_ARGS) / SLOT_ARGS; int want = 0;
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < slots && i < 16; i++) {
        GFxValue *a = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a[A_HASUSER].type != GFX_BOOL || !a[A_HASUSER].v.b) continue;
        if (a[A_SLOTTYPE].type != GFX_NUMBER) continue;
        int st = (int)a[A_SLOTTYPE].v.num;
        // slot type: 0 own, 1 other player; observers may have another - rating is shown to everyone
        if (a[A_NAME].type != GFX_WSTRING || !a[A_NAME].v.ws || !a[A_NAME].v.ws[0]) continue;
        if (a[A_TOOLTIP].type != GFX_WSTRING) continue;          // layout isn't what we expect - leave it alone
        const wchar_t *name = a[A_NAME].v.ws;
        unsigned long long id = 0;
        int si = a[A_SLOTINDEX].type == GFX_NUMBER ? (int)a[A_SLOTINDEX].v.num : -1;
        if (si >= 0 && si < nids && ids[si]) id = ids[si];       // player ID by slot position
        int observer = g_maxPlayers > 0 && si >= g_maxPlayers;  // observer: position beyond the player seats
        wchar_t key[64];
        if ((id >> 32) == 0x01100001) _snwprintf(key, 63, L"/steam/%llu", id); else wcsncpy(key, name, 63);   // stats are looked up by SteamID or by name
        key[63] = 0;
        if (g_nseen < 16) {
            Seen *sn = &g_seen[g_nseen++]; wcsncpy(sn->key, key, 63); sn->key[63] = 0; wcsncpy(sn->name, name, 63); sn->name[63] = 0;
            sn->id = id;
            sn->st = st; sn->pos = si; sn->obs = observer;
        }
        Entry *e = find(key);
        DWORD now = GetTickCount();
        if (!e || !e->fetched || now - e->fetched > 10 * 60 * 1000) {          // no data or older than 10 minutes
            int dup = 0;
            for (int k = 0; k < g_npending; k++) if (!wcscmp(g_pending[k], key)) dup = 1;
            if (!dup && g_npending < 16) { wcsncpy(g_pending[g_npending++], key, 63); want = 1; }
            if (!e) { e = find_or_add(key); e->fetched = 0; }
        }
        if (!e->known || !g_stats) continue;
        int games = e->wins + e->losses, wr = games ? (e->wins * 100 + games / 2) / games : 0;
        if (g_showInName) {
            _snwprintf(g_nameBuf[i], 127, L"%ls  [%d]", name, e->rating); g_nameBuf[i][127] = 0;
            a[A_NAME].v.ws = g_nameBuf[i];
        }
        format_tip(g_tipBuf[i], name, e, wr, games);
        a[A_TOOLTIP].type = GFX_WSTRING; a[A_TOOLTIP].v.ws = g_tipBuf[i];
    }
    {   // lobby roster changed - log who is there
        static unsigned long long lastSig; unsigned long long sig = nids * 1315423911ULL + g_nseen;
        for (int k = 0; k < g_nseen; k++) sig = sig * 31 + g_seen[k].id;
        if (sig != lastSig) {
            lastSig = sig;
            logmsg("lobby: host=%d, max players=%d, my id=%llu, people=%d", isHost, g_maxPlayers, g_localId, g_nseen);
            for (int k = 0; k < g_nseen; k++) {
                char n8[200]; WideCharToMultiByte(CP_UTF8, 0, g_seen[k].name, -1, n8, sizeof n8, NULL, NULL);
                logmsg("lobby:   %s type=%d pos=%d%s id=%llu%s", n8, g_seen[k].st, g_seen[k].pos, g_seen[k].obs ? " OBSERVER" : "",
                       g_seen[k].id, g_seen[k].id && g_seen[k].id == g_localId ? " (me)" : "");
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (want) SetEvent(g_wake);
}

// ---------- UI probe ([Debug] LogInvoke=1): which Flash functions the game calls, with what arguments ----------
// LobbyStats_invoke.log gets the first 3 calls of every method with all arguments (as the game sent them, before
// decorate), and once a minute how many times each method was called. Used to find the loading-screen and HUD
// functions. "movie" is the engine object Invoke is called on: one per Flash screen.
typedef struct { char name[64]; void *movie; LONG calls, lastCalls; int otherMovie; } ProbeEntry;
#define PROBE_SLOTS 1024
static int g_probe;
static wchar_t g_probeLog[MAX_PATH];
static CRITICAL_SECTION g_probeLock;
static ProbeEntry g_probeTab[PROBE_SLOTS]; static int g_probeN;
static DWORD g_probeLast;

static FILE *probe_open(void) {
    FILE *f = _wfopen(g_probeLog, L"a"); if (!f) return NULL;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    return f;
}
static void probe_args(FILE *f, GFxValue *args, unsigned count) {
    for (unsigned i = 0; i < count && i < 400; i++) {
        GFxValue *a = &args[i];
        fprintf(f, "    [%u] ", i);
        if (a->type == GFX_BOOL) fprintf(f, "bool %d\n", a->v.b);
        else if (a->type == GFX_NUMBER) fprintf(f, "num %g\n", a->v.num);
        else if (a->type == GFX_STRING) fprintf(f, "str \"%.150s\"\n", a->v.s && !IsBadReadPtr(a->v.s, 1) ? a->v.s : "?");
        else if (a->type == GFX_WSTRING) {
            char u[512] = "?"; int n = 0;
            if (a->v.ws && !IsBadReadPtr(a->v.ws, 2)) {
                while (n < 150 && a->v.ws[n]) n++;
                int k = WideCharToMultiByte(CP_UTF8, 0, a->v.ws, n, u, sizeof u - 1, NULL, NULL); u[k > 0 ? k : 0] = 0;
            }
            fprintf(f, "wstr \"%s\"\n", u);
        } else fprintf(f, "type %d\n", a->type);
    }
    if (count > 400) fprintf(f, "    ... %u more\n", count - 400);
}
static void probe(void *movie, const char *method, GFxValue *args, unsigned count) {
    if (IsBadReadPtr(method, 1)) return;
    unsigned h = 2166136261u;
    for (int i = 0; i < 63 && method[i]; i++) h = (h ^ (unsigned char)method[i]) * 16777619u;
    EnterCriticalSection(&g_probeLock);
    ProbeEntry *e = NULL;
    for (unsigned i = 0; i < PROBE_SLOTS; i++) {
        ProbeEntry *s = &g_probeTab[(h + i) % PROBE_SLOTS];
        if (!s->name[0]) {
            if (g_probeN < PROBE_SLOTS - 1) { strncpy(s->name, method, 63); s->movie = movie; g_probeN++; e = s; }
            break;
        }
        if (!strncmp(s->name, method, 63)) { e = s; break; }
    }
    if (e && ++e->calls <= 3) {
        FILE *f = probe_open();
        if (f) {
            DWORD vt = !IsBadReadPtr(movie, 4) ? *(DWORD *)movie : 0;
            fprintf(f, "%s  movie=%p vtbl=%08lX  call #%ld, %u args\n", e->name, movie, vt, e->calls, count);
            if (args && count && count < 4096 && !IsBadReadPtr(args, count * sizeof(GFxValue))) probe_args(f, args, count);
            fclose(f);
        }
    } else if (e && movie != e->movie && !e->otherMovie) {
        e->otherMovie = 1;
        FILE *f = probe_open(); if (f) { fprintf(f, "%s  also called on movie=%p\n", e->name, movie); fclose(f); }
    }
    DWORD now = GetTickCount();
    if (now - g_probeLast >= 60000) {
        g_probeLast = now;
        FILE *f = probe_open();
        if (f) {
            fprintf(f, "calls in the last minute:\n");
            for (int i = 0; i < PROBE_SLOTS; i++) {
                ProbeEntry *s = &g_probeTab[i];
                if (s->name[0] && s->calls != s->lastCalls) fprintf(f, "  %7ld  %s\n", s->calls - s->lastCalls, s->name);
                s->lastCalls = s->calls;
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_probeLock);
}

// main menu: own name in the top right corner, Receive_SetGamerTag(wstr name, num). Looked up by name, as the lobby
// does for the own slot. Numbers come from the cache; a name not cached yet is fetched and shows up the next time.
static wchar_t g_tagBuf[128];
static volatile int g_front;          // the menu name bar is on screen: SetGamerTag seen and no match since
static wchar_t g_menuName[64];        // own name from SetGamerTag (under g_lock), for the main menu tooltip
static void decorate_gamertag(GFxValue *args) {
    if (args[0].type != GFX_WSTRING || !args[0].v.ws || !args[0].v.ws[0] || args[0].v.ws == g_tagBuf) return;
    const wchar_t *name = args[0].v.ws; wchar_t key[64]; wcsncpy(key, name, 63); key[63] = 0;
    int want = 0;
    EnterCriticalSection(&g_lock);
    Entry *e = find(key);
    if (!e || !e->fetched || GetTickCount() - e->fetched > 10 * 60 * 1000) {
        int dup = 0;
        for (int k = 0; k < g_npending; k++) if (!wcscmp(g_pending[k], key)) dup = 1;
        if (!dup && g_npending < 16) { wcsncpy(g_pending[g_npending++], key, 63); want = 1; }
        if (!e) { e = find_or_add(key); e->fetched = 0; }
    }
    wcscpy(g_menuName, key); g_front = 1;
    if (e->known && g_stats && g_showInName) {
        _snwprintf(g_tagBuf, 127, L"%ls  [%d]", name, e->rating); g_tagBuf[127] = 0;
        args[0].v.ws = g_tagBuf;
    }
    LeaveCriticalSection(&g_lock);
    if (want) SetEvent(g_wake);
}

static char __fastcall hook(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count) {
    if (g_probe && method) probe(self, method, args, count);
    if (method && args && count >= 1 && !strcmp(method, "Receive_SetGamerTag") && !IsBadReadPtr(args, sizeof(GFxValue)))
        decorate_gamertag(args);
    if (method && args && !strcmp(method, "Receive_PopulatePlayerList") && count < 4096 && !IsBadReadPtr(args, count * sizeof(GFxValue)))
        decorate(args, count);
    if (method && args && count >= 1 && !strcmp(method, "Receive_SetMaxPlayers") && !IsBadReadPtr(args, sizeof(GFxValue)) && args[0].type == GFX_NUMBER)
        g_maxPlayers = (int)args[0].v.num;
    return g_orig(self, edx, method, flags, args, count);
}

static void install(void) {
    unsigned char *target = (unsigned char *)HOOK_ADDR;
    if (IsBadReadPtr(target, 5) || memcmp(target, PROLOGUE, 5)) { logmsg("DOW2.exe version mismatch at %08X, not hooking", HOOK_ADDR); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, PROLOGUE, 5); tr[5] = 0xE9; *(DWORD *)(tr + 6) = HOOK_RESUME - (DWORD)(tr + 10);
    g_orig = (InvokeFn)tr;
    DWORD old; VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9; *(DWORD *)(target + 1) = (DWORD)hook - (DWORD)(target + 5);
    VirtualProtect(target, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), target, 5);
    g_hooked = 1;
    logmsg("hooked Invoke at %08X", HOOK_ADDR);
}

// ---------- main menu: stats tooltip on hover over the own name ----------
// The menu's Flash has no tooltip there and the game sends only the name, so we draw it ourselves through Direct3D9
// (EndScene through the device vtable + ID3DXFont, as MatchTimer does), in a box like the game's own.
#define IAT_SIM_PRE 0x00F88154        // World::SimulatePre (__thiscall, ecx = World): a match starts, the menu is gone

typedef struct ID3DXSprite ID3DXSprite;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(ID3DXSprite *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(ID3DXSprite *);
    ULONG   (WINAPI *Release)(ID3DXSprite *);
    void *GetDevice, *GetTransform, *SetTransform, *SetWorldViewRH, *SetWorldViewLH;
    HRESULT (WINAPI *Begin)(ID3DXSprite *, DWORD flags);
    HRESULT (WINAPI *Draw)(ID3DXSprite *, IDirect3DTexture9 *, const RECT *src, const float *center, const float *pos, D3DCOLOR);
    void *Flush;
    HRESULT (WINAPI *End)(ID3DXSprite *);
    HRESULT (WINAPI *OnLostDevice)(ID3DXSprite *);
    HRESULT (WINAPI *OnResetDevice)(ID3DXSprite *);
} ID3DXSpriteVtbl;
struct ID3DXSprite { ID3DXSpriteVtbl *lpVtbl; };
typedef struct ID3DXFont ID3DXFont;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(ID3DXFont *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(ID3DXFont *);
    ULONG   (WINAPI *Release)(ID3DXFont *);
    void *GetDevice, *GetDescA, *GetDescW, *GetTextMetricsA, *GetTextMetricsW, *GetDC, *GetGlyphData;
    void *PreloadCharacters, *PreloadGlyphs, *PreloadTextA, *PreloadTextW;
    void *DrawTextA;
    INT     (WINAPI *DrawTextW)(ID3DXFont *, ID3DXSprite *, const wchar_t *, INT, RECT *, DWORD format, D3DCOLOR);
    HRESULT (WINAPI *OnLostDevice)(ID3DXFont *);
    HRESULT (WINAPI *OnResetDevice)(ID3DXFont *);
} ID3DXFontVtbl;
struct ID3DXFont { ID3DXFontVtbl *lpVtbl; };
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(IDirect3DDevice9 *, INT height, UINT width, UINT weight, UINT miplevels, BOOL italic,
    DWORD charset, DWORD prec, DWORD quality, DWORD pitchfam, const char *face, ID3DXFont **out);
typedef HRESULT (WINAPI *D3DXCreateSprite_t)(IDirect3DDevice9 *, ID3DXSprite **out);
static D3DXCreateFontA_t p_D3DXCreateFontA;
static D3DXCreateSprite_t p_D3DXCreateSprite;
#define D3DXSPRITE_ALPHABLEND 0x10

typedef void (__fastcall *ThisFn)(void *, void *);
static ThisFn o_pre;
static void __fastcall hk_pre(void *self, void *edx) { g_front = 0; o_pre(self, edx); }
// slot already hooked by another DLL (MatchTimer, FrameProbe) - chain onto it if it points to executable code
static int iat_hook(DWORD slot, const char *dll, void *fn, void **orig) {
    HMODULE m = GetModuleHandleA(dll); MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)slot, 4)) return 0;
    DWORD cur = *(DWORD *)slot;
    if (cur < (DWORD)mi.lpBaseOfDll || cur >= (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) {
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((void *)cur, &mb, sizeof mb) || !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
    }
    DWORD old; if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return 0;
    *orig = (void *)cur; *(DWORD *)slot = (DWORD)fn;
    VirtualProtect((void *)slot, 4, old, &old);
    return 1;
}

typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9 *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static EndScene_t o_EndScene; static Reset_t o_Reset;
static ID3DXFont *g_font; static ID3DXSprite *g_sprite;
static IDirect3DDevice9 *g_fontDev; static int g_fontPx, g_lost;
static IDirect3DTexture9 *g_box; static IDirect3DDevice9 *g_boxDev; static int g_boxW, g_boxH;

static int ensure_font(IDirect3DDevice9 *dev, int px) {
    if (g_fontDev == dev && g_fontPx == px) return g_font && g_sprite;   // made already (or failed: no retry every frame)
    if (g_font) { g_font->lpVtbl->Release(g_font); g_font = NULL; }
    if (g_sprite) { g_sprite->lpVtbl->Release(g_sprite); g_sprite = NULL; }
    g_fontDev = dev; g_fontPx = px;
    HRESULT hf = p_D3DXCreateFontA(dev, px, 0, FW_NORMAL, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial", &g_font);
    HRESULT hs = p_D3DXCreateSprite(dev, &g_sprite);
    logmsg("tooltip font %dpx -> hr=0x%08lx, sprite -> hr=0x%08lx", px, (unsigned long)hf, (unsigned long)hs);
    if (FAILED(hf)) g_font = NULL;
    if (FAILED(hs)) g_sprite = NULL;
    return g_font && g_sprite;
}
// box like the game's own (the "Fog of War Off" box of the replay panel): dark frame with cut corners, steel line,
// slate-blue fill. k = line thickness scale (1 at 1440p).
static DWORD box_pixel(int x, int y, int w, int h, float k) {
    float c = 5 * k, px = x + 0.5f, py = y + 0.5f, R = 0.7071f;
    float d = px; if (py < d) d = py; if (w - px < d) d = w - px; if (h - py < d) d = h - py;
    float e[4] = { (px + py - c) * R, (w - px + py - c) * R, (px + h - py - c) * R, (w - px + h - py - c) * R };
    for (int i = 0; i < 4; i++) if (e[i] < d) d = e[i];
    if (d <= 0) return 0;
    DWORD rgb;
    if (d < 1) rgb = 0x5A6A6C;
    else if (d < 3.5f * k) rgb = 0x0E1D29;
    else if (d < 4.5f * k) rgb = 0x48606E;
    else {
        float t = py / h; rgb = 0;
        for (int s = 0; s < 24; s += 8)
            rgb |= (DWORD)(((0x2D4556 >> s) & 255) + (((int)((0x22384A >> s) & 255) - (int)((0x2D4556 >> s) & 255)) * t)) << s;
    }
    return (d < 1 ? (DWORD)(d * 255) : 255) << 24 | rgb;
}
static int ensure_box(IDirect3DDevice9 *dev, int w, int h, float k) {
    if (g_box && g_boxDev == dev && g_boxW == w && g_boxH == h) return 1;
    if (g_box) { IDirect3DTexture9_Release(g_box); g_box = NULL; }
    g_boxDev = dev; g_boxW = w; g_boxH = h;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_box, NULL))) { g_box = NULL; return 0; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(g_box, 0, &lr, NULL, 0))) { IDirect3DTexture9_Release(g_box); g_box = NULL; return 0; }
    for (int y = 0; y < h; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) row[x] = box_pixel(x, y, w, h, k);
    }
    IDirect3DTexture9_UnlockRect(g_box, 0);
    return 1;
}
static int on_back_buffer(IDirect3DDevice9 *dev, int *w, int *h) {
    IDirect3DSurface9 *bb = NULL, *rt = NULL; D3DSURFACE_DESC d; int ok = 0;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) && rt) {
            if (rt == bb && SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d))) { *w = d.Width; *h = d.Height; ok = 1; }
            IDirect3DSurface9_Release(rt);
        }
        IDirect3DSurface9_Release(bb);
    }
    return ok;
}
static void draw_tip(IDirect3DDevice9 *dev) {
    static wchar_t tip[320];
    int W, H;
    if (!g_stats || g_lost || !on_back_buffer(dev, &W, &H)) return;
    D3DDEVICE_CREATION_PARAMETERS cp; RECT cr; POINT p;
    if (FAILED(IDirect3DDevice9_GetCreationParameters(dev, &cp)) || !cp.hFocusWindow || GetForegroundWindow() != cp.hFocusWindow) return;
    if (!GetCursorPos(&p) || !ScreenToClient(cp.hFocusWindow, &p) || !GetClientRect(cp.hFocusWindow, &cr) || cr.right <= 0 || cr.bottom <= 0) return;
    int mx = p.x * W / cr.right, my = p.y * H / cr.bottom;
    // the name bar in the top right corner, in 1/1000 of the screen height from the right edge (measured at 2560x1440)
    int L = W - 533 * H / 1000, R = W - 142 * H / 1000, T = 20 * H / 1000, B = 70 * H / 1000;
    if (mx < L || mx > R || my < T || my > B) return;
    tip[0] = 0;                                                      // numbers fetched after SetGamerTag count too
    EnterCriticalSection(&g_lock);
    Entry *e = g_menuName[0] ? find(g_menuName) : NULL;
    if (e && e->known) {
        int games = e->wins + e->losses, wr = games ? (e->wins * 100 + games / 2) / games : 0;
        format_tip(tip, g_menuName, e, wr, games);
    }
    LeaveCriticalSection(&g_lock);
    if (!tip[0]) return;
    int px = 18 * H / 1080; if (px < 10) px = 10;
    if (!ensure_font(dev, px)) return;
    RECT m = {0, 0, 0, 0};
    g_font->lpVtbl->DrawTextW(g_font, NULL, tip, -1, &m, DT_CALCRECT | DT_LEFT, 0);
    int pad = px * 7 / 10, bw = m.right - m.left + 2 * pad, bh = m.bottom - m.top + 2 * pad;
    RECT box = { R - bw, B + 6 * H / 1000, R, B + 6 * H / 1000 + bh };
    int haveBox = ensure_box(dev, bw, bh, H / 1440.0f < 1 ? 1 : H / 1440.0f);
    RECT r = { box.left + pad, box.top + pad, box.right - pad, box.bottom - pad }, s = r;
    OffsetRect(&s, 1, 1);
    D3DVIEWPORT9 vp, full = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
    int haveVp = SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp));
    IDirect3DDevice9_SetViewport(dev, &full);
    if (SUCCEEDED(g_sprite->lpVtbl->Begin(g_sprite, D3DXSPRITE_ALPHABLEND))) {   // no sorting: box first, then the text
        if (haveBox) { float pos[3] = { (float)box.left, (float)box.top, 0.0f }; g_sprite->lpVtbl->Draw(g_sprite, g_box, NULL, NULL, pos, 0xFFFFFFFF); }
        g_font->lpVtbl->DrawTextW(g_font, g_sprite, tip, -1, &s, DT_LEFT | DT_NOCLIP, 0xB0000000);
        g_font->lpVtbl->DrawTextW(g_font, g_sprite, tip, -1, &r, DT_LEFT | DT_NOCLIP, 0xFFF0EEE4);
        g_sprite->lpVtbl->End(g_sprite);
    }
    if (haveVp) IDirect3DDevice9_SetViewport(dev, &vp);
}
static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    if (g_front) draw_tip(dev);
    return o_EndScene(dev);
}
// ID3DXFont/ID3DXSprite hold state blocks and video memory: they must let go before Reset or Reset fails
static HRESULT WINAPI hk_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    if (dev == g_fontDev && !g_lost && (g_font || g_sprite)) {
        if (g_font) g_font->lpVtbl->OnLostDevice(g_font);
        if (g_sprite) g_sprite->lpVtbl->OnLostDevice(g_sprite);
        g_lost = 1;
    }
    HRESULT hr = o_Reset(dev, pp);
    if (g_lost && SUCCEEDED(hr)) {
        if (g_font) g_font->lpVtbl->OnResetDevice(g_font);
        if (g_sprite) g_sprite->lpVtbl->OnResetDevice(g_sprite);
        g_lost = 0;
    }
    return hr;
}
static void *vt_hook(void **vtbl, int index, void *fn) {
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    void *prev = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    return prev;
}
// create a throwaway device to reach the device vtable (shared with the game's device), then hook it
static int install_d3d_hooks(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    IDirect3D9 *(WINAPI *pCreate)(UINT) = d3d9 ? (void *)GetProcAddress(d3d9, "Direct3DCreate9") : NULL;
    if (!pCreate) return 0;
    IDirect3D9 *d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) return 0;
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "lsprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("lsprobe", "ls", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr) || !dev) {
        logmsg("tooltip: temp CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        IDirect3D9_Release(d3d); if (hwnd) DestroyWindow(hwnd); return 0;
    }
    void **vtbl = *(void ***)dev;
    o_Reset    = (Reset_t)    vt_hook(vtbl, 16, (void *)hk_Reset);
    o_EndScene = (EndScene_t) vt_hook(vtbl, 42, (void *)hk_EndScene);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    if (hwnd) DestroyWindow(hwnd);
    return 1;
}
static DWORD WINAPI tip_worker(LPVOID _) {
    const char *vers[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_38.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_31.dll", "d3dx9_30.dll", NULL };
    for (int i = 0; vers[i]; i++) {
        HMODULE h = LoadLibraryA(vers[i]); if (!h) continue;
        p_D3DXCreateFontA = (D3DXCreateFontA_t)GetProcAddress(h, "D3DXCreateFontA");
        p_D3DXCreateSprite = (D3DXCreateSprite_t)GetProcAddress(h, "D3DXCreateSprite");
        if (p_D3DXCreateFontA && p_D3DXCreateSprite) break;
    }
    if (!p_D3DXCreateFontA || !p_D3DXCreateSprite) { logmsg("tooltip: no d3dx9_*.dll, no main menu tooltip"); return 0; }
    for (int i = 0, pre = 0; i < 120; i++) {
        if (!pre) pre = iat_hook(IAT_SIM_PRE, "SimEngine.dll", hk_pre, (void **)&o_pre);
        if (pre && GetModuleHandleA("d3d9.dll") && install_d3d_hooks()) { logmsg("main menu tooltip hooks installed"); return 0; }
        Sleep(1000);
    }
    logmsg("tooltip: hooks not installed (no World::SimulatePre import or no d3d9.dll), no main menu tooltip");
    return 0;
}

// one copy per game process: a patch can come both from Elite_DLLs (launcher) and from dow2-patches\ (winmm loader)
static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t *sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    wchar_t ini[MAX_PATH]; _snwprintf(ini, MAX_PATH, L"%lsLobbyStats.ini", g_dir);
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);
    g_stats       = GetPrivateProfileIntW(L"LobbyStats", L"Stats", 1, ini);
    GetPrivateProfileStringW(L"LobbyStats", L"Server", STATS_HOST, g_server, 128, ini);
    if (!g_server[0]) { g_stats = 0; }                        // stats service not set - there will be no numbers
    g_probe = GetPrivateProfileIntW(L"Debug", L"LogInvoke", 0, ini);
    _snwprintf(g_probeLog, MAX_PATH, L"%lsLobbyStats_invoke.log", g_dir);
    InitializeCriticalSection(&g_lock); InitializeCriticalSection(&g_probeLock); g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_probeLast = GetTickCount();
    logmsg("LobbyStats v0.7.3%s", g_probe ? ", UI probe on (LobbyStats_invoke.log)" : "");
    if (already_loaded("LobbyStats")) { logmsg("another LobbyStats copy is already active in this game, this one stays idle"); return TRUE; }
    load_cache(); install();
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    if (g_stats) CloseHandle(CreateThread(NULL, 0, tip_worker, NULL, 0, NULL));
    return TRUE;
}

// for testing outside the game: run the substitution on a fake argument set
__declspec(dllexport) int LobbyStats_Test(GFxValue *args, unsigned count) { decorate(args, count); return g_ncache; }
