// MMFind.dll - one-button matchmaking for DoW2 Retribution / Elite (DOW2.exe 3.19).
//
// The stock "Multiplayer -> Find a Game" screen talks to Relic's ranked matchmaker. We take that screen over:
// "Start Searching" no longer starts Relic's search, it puts the player into our queue (dow2-ladder /ladder/api/mm)
// and the screen shows our status. When the server pairs two players, the DLL walks both clients through the
// normal custom-game menus by sending the same UI commands the buttons send (fscommand): the host creates a
// public game with a unique name MM-<match>, the guest finds that name in the Public Games list and joins,
// the host starts the game when the opponent is in. No screen coordinates are used.
//
// Hooks: Invoke 0x45DB6C (engine -> Flash, to read lists and to show our status), fscommand handler 0x681AB0
// (Flash -> engine, to catch the button and to replay menu commands), Shark.dll shark::update (main-thread tick).
#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <shlobj.h>

#define HOOK_ADDR   0x0045DB6C
#define HOOK_RESUME 0x0045DB71
static const unsigned char PROLOGUE[5] = {0xB8, 0x59, 0x70, 0xEF, 0x00};
#define FN_FSCMD    0x00681AB0
static const unsigned char FSCMD_PRO[7] = {0x51, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14};
#define IAT_SHARK_UPDATE 0x00F88068

#ifndef MM_HOST
#define MM_HOST L"rts.fintebtc.de;dow2.fintebtc.de;dow2ru.fintebtc.de"   // mirrors in order; dow2ru for players without Hetzner
#endif
#define MM_PATH L"/ladder/api/mm/"

typedef struct { int type; int pad; union { double num; unsigned char b; const wchar_t *ws; const char *s; } v; } GFxValue;
enum { GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_WSTRING = 5 };
typedef char (__fastcall *InvokeFn)(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count);
typedef void (__fastcall *FsCmdFn)(void *self, void *edx, void *movie, const char *cmd, const char *args);
typedef void (__stdcall *UpdFn)(float dt);
static InvokeFn g_invOrig; static FsCmdFn g_fsOrig; static UpdFn g_updOrig;

static wchar_t g_dir[MAX_PATH], g_server[128] = MM_HOST; static CRITICAL_SECTION g_lk; static int g_verbose, g_logInvoke;
static void logmsg(const char *fmt, ...) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsMMFind.log", g_dir);
    EnterCriticalSection(&g_lk);
    FILE *f = _wfopen(p, L"a"); if (f) {
        SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f); }
    LeaveCriticalSection(&g_lk);
}

// ---------------- UI tracking (filled from the hooks, read on the main thread) ----------------
enum { SC_OTHER, SC_MAIN, SC_MM, SC_LIST, SC_SETUP };
static volatile int g_screen;
static char g_panel[160];                 // path of the current full-screen panel, e.g. _level0.display_item.panel2_mc.panel_mc
static void *g_fsSelf, *g_fsMovie;        // fscommand handler + frontend movie, taken from real button presses
static volatile int g_heroShown;          // hero selection is on screen (needs Accept)
static volatile int g_listSeen;           // a Public Games list arrived since we opened the list
static volatile int g_rowFound = -1;      // row index of our game in the list
static volatile int g_humans, g_lobbyRace = -1;   // people in the lobby; my race as the lobby shows it (slot field 6)             // people in the lobby (from the player list)
static volatile int g_oppIn;              // the expected opponent is in the lobby
static wchar_t g_myName[64];              // our gamer tag
static void *g_mmSelf; static int g_mmFlags;  // Find a Game panel object (to push our status into it)
static int g_mapIndex = -1;               // (unused since 0.9: index comes from the map catalog)
static wchar_t g_mapName[64]; static char g_hostMap[96];   // g_hostMap: the map the host really set (UTF-8, for the ladder)             // map for this match (from the server), empty = host picks
// modes: Match Type list on Find a Game = Head to Head / Two on Two / Team Battle / Team Free For All
int g_mode;                               // 0 1v1, 1 2v2, 2 3v3, 3 watch (the Team FFA item is renamed to "Watch a match")
static const char *MODE_KEY[] = {"1v1", "2v2", "3v3", "watch"};
static const wchar_t *MODE_W[] = {L"1v1", L"2v2", L"3v3", L"Watch"};
static const wchar_t *MODE_PREFIX[] = {L"(2p)", L"(4p)", L"(6p)", L"(6p)"};
// map catalog: the lobby map list as the game sends it (all Elite maps, same order as the lobby drop-down)
#define MAX_BANS 3                // maps a player may uncheck per mode (ladder rule)
#define MAXMAPS 160
static wchar_t g_maps[MAXMAPS][64]; static int g_nmaps;
static unsigned char g_off[4][MAXMAPS];  // 1 = player unchecked this map for the mode
static int map_index(const wchar_t *n) { for (int i = 0; i < g_nmaps; i++) if (!wcscmp(g_maps[i], n)) return i; return -1; }
static int mode_map(int i, int mode) { return !wcsncmp(g_maps[i], MODE_PREFIX[mode], 4); }

// ---------------- ActionScript access (Scaleform 2.x inside DOW2.exe 3.19, as in LobbyStats 0.6) ----------------
#define FN_STR_NEW   0x00DB64D0
#define FN_STR_FREE  0x00DB62D0
#define FN_RESOLVE   0x00DF9520
#define FN_AS_CALL   0x00DD27E0
#define FN_STK_GROW  0x00DB7950
#define FN_STK_POP   0x00DB7BE0
#define FN_VAL_FREE  0x00DBA000
typedef struct { unsigned char b[16]; } ASValue;
typedef void *(__fastcall *StrNewFn)(void *mgr, void *edx, const char *s);
typedef void  (__fastcall *ThisFn)(void *self, void *edx);
typedef void *(__fastcall *EnvFn)(void *sprite, void *edx);
typedef char  (__fastcall *ResolveFn)(void *env, void *edx, void **node, ASValue *out, int, int, int, int);
typedef char  (__fastcall *AsCallFn)(void *sprite, void *edx, const char *path, ASValue *res, int n);
typedef void  (__fastcall *PopFn)(void *stk, void *edx, int n);
typedef char  (__fastcall *GetVarFn)(void *movie, void *edx, GFxValue *out, const char *path);              // GFxMovie vtbl+0x30
typedef char  (__fastcall *SetVarFn)(void *movie, void *edx, const char *path, const GFxValue *v, int type); // GFxMovie vtbl+0x2C
enum { AV_UNDEF, AV_NUM, AV_BOOL, AV_STR };
typedef struct { int kind; double num; const char *s; } Arg;
static void *as_str(void *movie, const char *s) {
    void *mgr = (char *)*(void **)((char *)movie + 0x5C4) + 0x238;
    int *n = ((StrNewFn)FN_STR_NEW)(mgr, NULL, s); n[3]++; return n;
}
static int as_call(void *movie, const char *method, const Arg *args, int n) {
    void *sprite = *(void **)((char *)movie + 0x24); if (!sprite) return 0;
    void *env = ((EnvFn)(*(void ***)sprite)[0x5C / 4])(sprite, NULL);
    char *stk = (char *)env + 8;
    for (int i = n - 1; i >= 0; i--) {
        *(char **)stk += 16;
        if (*(char **)stk >= *(char **)(stk + 8)) ((ThisFn)FN_STK_GROW)(stk, NULL);
        unsigned char *v = *(unsigned char **)stk; if (!v) continue;
        memset(v, 0, 16);
        if (args[i].kind == AV_NUM) { v[0] = 3; memcpy(v + 4, &args[i].num, 8); }
        else if (args[i].kind == AV_BOOL) { v[0] = 2; v[4] = args[i].num != 0; }
        else if (args[i].kind == AV_STR) { v[0] = 5; *(void **)(v + 4) = as_str(movie, args[i].s); }
    }
    ASValue res; memset(&res, 0, sizeof res);
    char ok = ((AsCallFn)FN_AS_CALL)(sprite, NULL, method, &res, n);
    ((PopFn)FN_STK_POP)(stk, NULL, n);
    if (res.b[0] >= 5) ((ThisFn)FN_VAL_FREE)(&res, NULL);
    return ok;
}
static int as_fnv(void *movie, const char *fn, const char *sig, ...) {      // n number, b bool, s string
    Arg a[12]; int n = 0; va_list ap; va_start(ap, sig);
    for (const char *c = sig; *c && n < 12; c++, n++) {
        a[n].kind = *c == 'n' ? AV_NUM : *c == 'b' ? AV_BOOL : AV_STR; a[n].num = 0; a[n].s = NULL;
        if (*c == 'n') a[n].num = va_arg(ap, double); else if (*c == 'b') a[n].num = va_arg(ap, int); else a[n].s = va_arg(ap, const char *);
    }
    va_end(ap); return as_call(movie, fn, a, n);
}
static int as_set(void *movie, const char *path, GFxValue *v) { return ((SetVarFn)(*(void ***)movie)[0x2C / 4])(movie, NULL, path, v, 1) != 0; }
static int as_get(void *movie, const char *path, GFxValue *v) { memset(v, 0, sizeof *v); return ((GetVarFn)(*(void ***)movie)[0x30 / 4])(movie, NULL, v, path) != 0; }
static void as_setw(void *movie, const char *path, const wchar_t *w) { GFxValue v; memset(&v, 0, sizeof v); v.type = GFX_WSTRING; v.v.ws = w; as_set(movie, path, &v); }
static void as_setn(void *movie, const char *path, double d) { GFxValue v; memset(&v, 0, sizeof v); v.type = GFX_NUMBER; v.v.num = d; as_set(movie, path, &v); }
static void as_setb(void *movie, const char *path, int b) { GFxValue v; memset(&v, 0, sizeof v); v.type = GFX_BOOL; v.v.b = b; as_set(movie, path, &v); }

// ---------------- matchmaking state ----------------
enum { S_IDLE, S_SEARCH, S_GO_MAIN, S_GO_LIST, S_HOST_DIALOG, S_HOST_NAME, S_SETUP_HERO, S_HOST_WAIT, S_GUEST_NAME, S_GUEST_FIND, S_GUEST_JOINED, S_PLACE, S_DONE, S_FAIL };
static const char *SN[] = {"idle","search","go_main","go_list","host_dialog","host_name","setup_hero","host_wait","guest_name","guest_find","guest_joined","place","done","fail"};
static volatile int g_state;
static volatile int g_role;               // 1 host, 2 guest
static DWORD g_stateTick, g_searchTick;
static char g_match[32];                  // server match id
static char g_gameName[48];               // MM-<match>
static wchar_t g_oppName[64];
static volatile int g_queue = -1;
// match roster from the server: who must be in which lobby slot; watchers go to observer slots; strangers get kicked
static unsigned long long g_rSteam[8], g_wSteam[8], g_rPid[8], g_wPid[8], g_myPid; static int g_rSlot[8], g_nR, g_nW, g_mySlot = -1, g_matchMode, g_bots;
static wchar_t g_rName[8][64], g_wName[8][64], g_slotName[8][64];
static int jstr(const char *j, const char *key, char *out, int cap);
extern unsigned long long g_mySteam;
static unsigned long long parse_u64(const char *p) { return _strtoui64(p, NULL, 10); }
static void parse_roster(const char *res) {
    int nr = 0, nw = 0; char tmp[200];
    const char *r = strstr(res, "\"roster\":["), *w = strstr(res, "\"watchers\":["), *wp = strstr(res, "\"watcher_pids\":["), *wn = strstr(res, "\"watcher_names\":[");
    if (r) { const char *e = strstr(r, "}]"); if (!e) e = r + strlen(r);
        for (const char *p = strstr(r, "\"steam\":\""); p && p < e && nr < 8; p = strstr(p + 1, "\"steam\":\"")) {
            const char *end = strchr(p, '}'); int len = end ? (int)(end - p) : 190; if (len > 190) len = 190;
            memcpy(tmp, p, len); tmp[len] = 0;
            g_rSteam[nr] = parse_u64(p + 9);
            const char *sl = strstr(tmp, "\"slot\":"); g_rSlot[nr] = sl ? atoi(sl + 7) : -1;
            const char *pi = strstr(tmp, "\"pid\":"); g_rPid[nr] = pi ? parse_u64(pi + 6) : 0;
            char nm[96] = ""; jstr(tmp, "name", nm, sizeof nm); MultiByteToWideChar(CP_UTF8, 0, nm, -1, g_rName[nr], 64);
            nr++; } }
    if (w) { const char *e = strchr(w, ']');
        for (const char *p = strchr(w + 12, '"'); p && p < e && nw < 8; ) { g_wSteam[nw++] = parse_u64(p + 1);
            const char *q = strchr(p + 1, '"'); if (!q) break; p = strchr(q + 1, '"'); } }
    for (int i = 0; i < nw; i++) { g_wPid[i] = 0; g_wName[i][0] = 0; }
    if (wp) { const char *p = wp + 16; for (int i = 0; i < nw && *p && *p != ']'; i++) { g_wPid[i] = parse_u64(p); p = strchr(p, ','); if (!p) break; p++; } }
    if (wn) { const char *p = strchr(wn + 17, '"'); const char *e = strchr(wn, ']');
        for (int i = 0; i < nw && p && p < e; i++) { const char *q = strchr(p + 1, '"'); if (!q) break; char nm[96]; int l = (int)(q - p - 1); if (l > 95) l = 95;
            memcpy(nm, p + 1, l); nm[l] = 0; MultiByteToWideChar(CP_UTF8, 0, nm, -1, g_wName[i], 64); p = strchr(q + 1, '"'); } }
    g_nR = nr; g_nW = nw;
    for (int i = 0; i < nr; i++) if (g_rSteam[i] == g_mySteam && g_rPid[i]) g_myPid = g_rPid[i];
    for (int i = 0; i < nw; i++) if (g_wSteam[i] == g_mySteam && g_wPid[i]) g_myPid = g_wPid[i];
    const char *ms = strstr(res, "\"slot\":"); if (ms && (!r || ms < r)) g_mySlot = atoi(ms + 7);
    char md[16]; if (jstr(res, "mode", md, sizeof md)) for (int i = 0; i < 3; i++) if (!strcmp(md, MODE_KEY[i])) g_matchMode = i;
    g_bots = strstr(res, "\"bots\":true") != NULL;
}
unsigned long long g_mySteam;
static volatile int g_wantJoin, g_wantLeave, g_wantHost, g_hostReported, g_wantStarted, g_wantEnded; static char g_ruined[48]; static volatile int g_loading;
static int g_maxDiff, g_testBots, g_aiDiff, g_manual, g_autoSwap = 1, g_autoSwapCfg = 1; static char g_group[32];   // Manual=1: no automation in the lobby (tests)   // TestBots: 1 = me vs computers, 2 = computers vs computers, I watch (AI tests)
static const char *RACES[] = {"chaos_space_marines","eldar","imperial_guard","ordo_malleus","orks","space_marines","tau","tyranids","necron"};
static HANDLE g_wake;
static volatile int g_coverWant; static wchar_t g_coverLine[160]; static int g_sending;

static void set_state(int s) { logmsg("state %s -> %s", SN[g_state], SN[s]); g_state = s; g_stateTick = GetTickCount(); if (s == S_FAIL) { g_coverWant = 0; g_wantLeave = 1; }
    if (s == S_IDLE || s == S_SEARCH || s == S_FAIL) { SetEnvironmentVariableA("DOW2_RELAY_USE", "0"); SetEnvironmentVariableA("DOW2_RELAY_HOST", NULL); } }   // no automatch lobby: Relic
static DWORD in_state(void) { return GetTickCount() - g_stateTick; }

// ---------------- ruined loading: MMFind_match.txt holds the ladder lobby while the match loads ----------------
static DWORD g_loadTick;
static void marker_set(const char *lobby) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsMMFind_match.txt", g_dir);
    if (!lobby) { DeleteFileW(p); return; }
    FILE *f = _wfopen(p, L"w"); if (f) { fputs(lobby, f); fclose(f); }
}
static void marker_check(void) {                    // at start: a marker left over = the game died while that match was loading
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsMMFind_match.txt", g_dir);
    FILE *f = _wfopen(p, L"r"); if (!f) return;
    char b[48] = ""; fgets(b, sizeof b, f); fclose(f); DeleteFileW(p); b[strcspn(b, "\r\n")] = 0;
    if (!strncmp(b, "MM-", 3)) { strcpy(g_ruined, b); logmsg("the last ladder match (%s) did not get past loading - reporting", b); }
}

// ---------------- "swap slots" requests: accepted without the Yes/No box ----------------
// PlayerListPanel event handler, swap request from another player (0x490FBF..): unless the hero panel is open it builds the box
// "Slot Swap Requested" (555015/555016) with the answer callback 0x4910BB(this = esi-0x4C; 2nd arg 1 = Yes, 2 = No, ret 8),
// storing the request id in esi+0xC. At 0x491026 ("is a box already up?") we jump to a stub that, when no box is up,
// stores the request id and calls the callback with Yes right away.
#define SWAP_AT   0x00491026
#define SWAP_BOX  0x0049102C      // original: create the box
#define SWAP_BUSY 0x00491076      // original: a box is already up - answer "busy"
#define SWAP_DONE 0x0049108C
#define SWAP_CB   0x004910BB
static const unsigned char SWAP_PRO[6] = {0x83, 0x7E, 0x08, 0xFF, 0x75, 0x4A};
static volatile LONG g_swapCount;
static void install_swap_hook(void) {
    unsigned char *t = (unsigned char *)SWAP_AT;
    if (IsBadReadPtr(t, 6) || memcmp(t, SWAP_PRO, 6)) { logmsg("swap request prologue mismatch - swaps are asked as usual"); return; }
    unsigned char *c = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); int k = 0;
#define B(x) c[k++] = (unsigned char)(x)
#define REL(target) do { DWORD r_ = (DWORD)(target) - (DWORD)(c + k + 4); memcpy(c + k, &r_, 4); k += 4; } while (0)
#define ABS(ptr) do { DWORD a_ = (DWORD)(ptr); memcpy(c + k, &a_, 4); k += 4; } while (0)
    B(0x83); B(0x7E); B(0x08); B(0xFF);                 // cmp dword [esi+8], -1
    B(0x0F); B(0x85); REL(SWAP_BUSY);                    // jne busy
    B(0x83); B(0x3D); ABS(&g_autoSwap); B(0x00);         // cmp dword [g_autoSwap], 0
    B(0x0F); B(0x84); REL(SWAP_BOX);                     // je  -> the usual box
    B(0xF0); B(0xFF); B(0x05); ABS(&g_swapCount);        // lock inc dword [g_swapCount]
    B(0x8B); B(0x47); B(0x20);                           // mov eax, [edi+0x20]   request id
    B(0x89); B(0x46); B(0x0C);                           // mov [esi+0xC], eax
    B(0x8D); B(0x4E); B(0xB4);                           // lea ecx, [esi-0x4C]
    B(0x6A); B(0x01); B(0x6A); B(0x00);                  // push 1 (Yes); push 0
    B(0xE8); REL(SWAP_CB);                               // call answer callback (ret 8)
    B(0xE9); REL(SWAP_DONE);                             // jmp done
#undef B
#undef REL
#undef ABS
    DWORD o; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &o); t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)c - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("hooked swap requests (auto-accept in ladder lobbies: %s)", g_autoSwapCfg ? "on" : "off");
}

// ---------------- game options of a ladder lobby: Victory Point Control, 500 points, random start positions, standard resources ----------------
// Random start (0.29): the game shuffles the start positions inside each team (checked on 6p maps), so only who is in which team
// matters, not the slot inside the team - see the lobby placement below.
// Lobby drop-downs (Receive_PopulateDropDownList: id, title, items...): 1 victory condition, 2 tickets, 3 start location, 4 resources.
// The host's game keeps the last custom game's options - the host sets these every time.
// the wanted item in each language the players use (English game, Gosudar's Russian translation)
static const wchar_t *OPT_WANT[5][3] = {{NULL}, {L"Victory Point Control", L"Контроль победных точек", NULL}, {L"500 points", L"500", NULL},
                                        {L"Random", L"Случайная", L"Случайно"}, {L"Standard", L"Стандартный", NULL}};
static volatile int g_optWant[5] = {-1, -1, -1, -1, -1}, g_optCur[5] = {-1, -1, -1, -1, -1}; static void *g_optSelf;

// ---------------- HTTP (worker thread) ----------------
// the ladder API through the mirrors (Server=host;host;...): stay on the one that answered, go to the next when it can't be reached
static wchar_t g_hosts[4][64]; static int g_nHosts, g_curHost;
static void parse_hosts(void) {
    static wchar_t b[128]; wcscpy(b, g_server); g_nHosts = 0;
    for (wchar_t *t = wcstok(b, L";, "); t && g_nHosts < 4; t = wcstok(NULL, L";, ")) wcsncpy(g_hosts[g_nHosts++], t, 63);
    if (!g_nHosts) { wcscpy(g_hosts[0], L"rts.fintebtc.de"); g_nHosts = 1; }
}
static int http_post1(const wchar_t *host, const wchar_t *path, const char *body, char *out, int cap, int tmo) {
    out[0] = 0; int got = 0, answered = 0;
    HINTERNET s = WinHttpOpen(L"MMFind/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0); if (!s) return 0;
    WinHttpSetTimeouts(s, tmo, tmo, tmo, tmo);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1, (void *)body, (DWORD)strlen(body), (DWORD)strlen(body), 0)
          && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, sz = sizeof code; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        answered = code && code < 500;                                // a 502 from a mirror: its way to the ladder is broken
        DWORD n; while (got < cap - 1 && WinHttpReadData(r, out + got, cap - 1 - got, &n) && n) got += n;
    }
    out[got] = 0;
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return answered && got > 0;
}
static int http_post(const wchar_t *ep, const char *body, char *out, int cap) {
    wchar_t path[128]; _snwprintf(path, 128, L"%ls%ls", MM_PATH, ep);
    static const int TMO[2] = {1000, 4000};                          // first round quick (1 s per step), then a patient one (4 s)
    for (int round = 0; round < 2; round++)
        for (int k = 0; k < g_nHosts; k++) {
            int h = (g_curHost + k) % g_nHosts;
            if (http_post1(g_hosts[h], path, body, out, cap, TMO[round])) {
                if (h != g_curHost) { logmsg("server: %ls is not reachable, now using %ls", g_hosts[g_curHost], g_hosts[h]); g_curHost = h; }
                return 1;
            }
        }
    out[0] = 0; return 0;
}
static int jstr(const char *j, const char *key, char *out, int cap) {   // "key":"value" or "key":null
    char k[64]; snprintf(k, sizeof k, "\"%s\":", key); const char *p = strstr(j, k); out[0] = 0; if (!p) return 0;
    p += strlen(k); while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++; int i = 0; while (*p && *p != '"' && i < cap - 1) out[i++] = *p++; out[i] = 0; return 1;
}
static int jint(const char *j, const char *key, int def) {
    char k[64]; snprintf(k, sizeof k, "\"%s\":", key); const char *p = strstr(j, k); if (!p) return def;
    return atoi(p + strlen(k));
}
static void name_utf8(char *o, int cap) { WideCharToMultiByte(CP_UTF8, 0, g_myName, -1, o, cap, NULL, NULL); }

static DWORD WINAPI net_worker(LPVOID _) {
    static char body[8192]; char res[4096], nm[128];
    for (;;) {
        WaitForSingleObject(g_wake, 2000);
        if (g_wantLeave) {
            g_wantLeave = 0; snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam);
            http_post(L"leave", body, res, sizeof res); logmsg("leave -> %s", res);
        }
        if (g_wantJoin) {
            g_wantJoin = 0; name_utf8(nm, sizeof nm);
            extern int g_race, g_hero, g_random; static char maps[6000]; int L = 0; maps[0] = 0;
            for (int i = 0; i < g_nmaps && L < 5800; i++) if (mode_map(i, g_mode) && !g_off[g_mode][i]) {
                char m8[160]; WideCharToMultiByte(CP_UTF8, 0, g_maps[i], -1, m8, sizeof m8, NULL, NULL);
                L += snprintf(maps + L, sizeof maps - L, "%s\"%s\"", L ? "," : "", m8); }
            int own = GetModuleHandleA("Relay.dll") != NULL;      // our own game server: Relay 7 plays only with players who have it,
            char rv[8] = ""; GetEnvironmentVariableA("DOW2_RELAY", rv, sizeof rv);   // Relay 8 "8c": compatible, any lobby (see ladder.py)
            int compat = own && strchr(rv, 'c') != NULL;
            // Relay 9 pings our servers: "ip:ms,ip:ms" (-1 = no answer) -> "relay_ping":{"ip":ms,...}, the matchmaker picks the best for all
            char pg[256] = "", rp[320] = ""; int rl = 0;
            if (own && GetEnvironmentVariableA("DOW2_RELAY_PING", pg, sizeof pg) && pg[0]) {
                rl = snprintf(rp, sizeof rp, ",\"relay_ping\":{");
                for (char *t = strtok(pg, ","), *first = t; t && rl < 280; t = strtok(NULL, ",")) {
                    char *c = strrchr(t, ':'); if (!c) continue; *c = 0;
                    rl += snprintf(rp + rl, sizeof rp - rl, "%s\"%s\":%d", t == first ? "" : ",", t, atoi(c + 1));
                }
                rl += snprintf(rp + rl, sizeof rp - rl, "}");
            }
            snprintf(body, sizeof body, "{\"steam\":\"%llu\",\"mode\":\"%s\",\"name\":\"%s\",\"race\":\"%s\",\"hero\":\"%d\",\"maps\":[%s],\"max_diff\":%d%s%s%s,\"group\":\"%s\"}",
                     g_mySteam, MODE_KEY[g_mode], nm, (g_random ? "random" : g_race >= 0 && g_race < 9 ? RACES[g_race] : ""), g_hero, maps, g_maxDiff, g_testBots ? ",\"bots\":true" : "",
                     own ? (compat ? ",\"own_server\":true,\"relay_compat\":true" : ",\"own_server\":true") : "", rp, g_group);
            http_post(L"join", body, res, sizeof res); logmsg("join -> %s", res);
            g_queue = jint(res, MODE_KEY[g_mode], -1);
        }
        if (g_wantHost && !g_hostReported) {
            snprintf(body, sizeof body, "{\"steam\":\"%llu\",\"lobby\":\"%s\",\"map\":\"%s\"}", g_mySteam, g_gameName, g_hostMap);
            if (http_post(L"host", body, res, sizeof res)) { g_hostReported = 1; logmsg("host %s -> %s", g_gameName, res); }
        }
        if (g_wantStarted) { g_wantStarted = 0; snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam); http_post(L"started", body, res, sizeof res); logmsg("started -> %s", res); }
        if (g_wantEnded) { g_wantEnded = 0; snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam); http_post(L"ended", body, res, sizeof res); logmsg("ended -> %s", res); }
        if (g_loading) {                                                  // in a match: "still playing" once a minute (the ladder closes a match nobody plays)
            static DWORD lastA; if (GetTickCount() - lastA > 60000) { lastA = GetTickCount();
                snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam); http_post(L"alive", body, res, sizeof res); }
        }
        if (g_ruined[0]) {                                                // the game died while a ladder match was loading (found at start)
            snprintf(body, sizeof body, "{\"steam\":\"%llu\",\"lobby\":\"%s\"}", g_mySteam, g_ruined);
            if (http_post(L"ruined", body, res, sizeof res)) { logmsg("ruined loading %s -> %s", g_ruined, res); g_ruined[0] = 0; }
        }
        int st = g_state;
        if (g_role == 1 && st >= S_GO_MAIN && st <= S_HOST_WAIT) {          // host keeps polling: watchers may be added while we set up
            static DWORD lastp; if (GetTickCount() - lastp > 2000) { lastp = GetTickCount();
                snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam);
                if (http_post(L"poll", body, res, sizeof res) && strstr(res, "\"matched\"")) parse_roster(res); }
            continue;
        }
        if (st == S_SEARCH || st == S_GUEST_NAME) {
            snprintf(body, sizeof body, "{\"steam\":\"%llu\"}", g_mySteam);
            if (!http_post(L"poll", body, res, sizeof res)) continue;
            char state[32]; jstr(res, "state", state, sizeof state);
            if (st == S_SEARCH && !strcmp(state, "searching")) { g_queue = jint(res, MODE_KEY[g_mode], g_queue); continue; }
            if (st == S_SEARCH && !strcmp(state, "idle")) { g_wantJoin = 1; continue; }        // server forgot us - rejoin
            if (!strcmp(state, "matched")) {
                char role[16], lobby[64], en[128] = "";
                jstr(res, "role", role, sizeof role); jstr(res, "match", g_match, sizeof g_match);
                { char mp[96]; g_mapName[0] = 0; if (jstr(res, "map", mp, sizeof mp) && mp[0]) MultiByteToWideChar(CP_UTF8, 0, mp, -1, g_mapName, 64); }
                const char *e = strstr(res, "\"enemies\""); if (e) jstr(e, "name", en, sizeof en);
                MultiByteToWideChar(CP_UTF8, 0, en, -1, g_oppName, 64);
                parse_roster(res);
                // Relay 8 in its compatible mode uses our game server only for this: an automatch where everyone has it
                SetEnvironmentVariableA("DOW2_RELAY_USE", strstr(res, "\"own_server\": true") || strstr(res, "\"own_server\":true") ? "1" : "0");
                { char rh[32] = ""; SetEnvironmentVariableA("DOW2_RELAY_HOST", jstr(res, "relay_host", rh, sizeof rh) && rh[0] ? rh : NULL); }   // best ping for all
                if (st == S_SEARCH) {
                    logmsg("matched: %s", res);
                    g_role = !strcmp(role, "host") ? 1 : !strcmp(role, "watcher") ? 3 : 2;
                    if (g_role == 1) {                       // unique room name: match id + random tail (match ids restart with the server)
                        static const char A[] = "abcdefghjkmnpqrstuvwxyz23456789"; char r[7]; unsigned x = GetTickCount() ^ (unsigned)(g_mySteam * 2654435761u);
                        for (int i = 0; i < 6; i++) { x = x * 1103515245u + 12345u; r[i] = A[(x >> 16) % (sizeof A - 1)]; } r[6] = 0;
                        snprintf(g_gameName, sizeof g_gameName, "MM-%s-%s", g_match, r);
                    }
                    _snwprintf(g_coverLine, 159, L"Opponent: %ls. Connecting", g_oppName[0] ? g_oppName : L"player"); g_coverWant = 1;
                    g_state = S_GO_MAIN; g_stateTick = GetTickCount();
                } else if (jstr(res, "lobby", lobby, sizeof lobby) && lobby[0]) {
                    strncpy(g_gameName, lobby, sizeof g_gameName - 1); logmsg("guest: host game is %s", g_gameName);
                    g_state = S_GO_LIST; g_stateTick = GetTickCount();
                }
            }
        }
    }
    return 0;
}

// ---------------- UI commands ----------------
static void fs(const char *rel, const char *args) {          // rel: ".OnClickHost" relative to the panel, or a full path
    if (!g_fsSelf || !g_fsMovie) { logmsg("fs %s: no handler yet", rel); return; }
    char cmd[300]; if (rel[0] == '.') snprintf(cmd, sizeof cmd, "%s%s", g_panel, rel); else snprintf(cmd, sizeof cmd, "%s", rel);
    logmsg("-> fscommand %s '%s'", cmd, args ? args : "");
    g_sending++; g_fsOrig(g_fsSelf, NULL, g_fsMovie, cmd, args ? args : ""); g_sending--;
}
static HWND game_wnd(void) {
    HWND w = FindWindowW(NULL, L"Dawn of War II"); if (w) return w;
    return GetForegroundWindow();
}
static void type_text(const char *s) {
    HWND w = game_wnd(); if (!w) { logmsg("type: no window"); return; }
    for (const char *p = s; *p; p++) PostMessageW(w, WM_CHAR, (WPARAM)(unsigned char)*p, 1);
    logmsg("typed '%s' into %p", s, w);
}
static void press_enter(void) {
    HWND w = game_wnd(); if (!w) return;
    PostMessageW(w, WM_KEYDOWN, VK_RETURN, 0x001C0001); PostMessageW(w, WM_CHAR, '\r', 0x001C0001); PostMessageW(w, WM_KEYUP, VK_RETURN, 0xC01C0001);
}

// ---------------- cover: while the DLL walks the menus, the player sees one calm "connecting" screen ----------------
static int g_coverOn; static volatile int g_loading; static wchar_t g_btnText[64], g_startTxt[64], g_aloneTxt[64]; static volatile int g_btnEnabled, g_readyMe, g_allReady, g_readyIdx = -1; static DWORD g_coverTextTick; static DWORD g_ffaNote, g_banNote; static double g_coverH = 1200;
static void cover_update(void) {
    void *m = g_fsMovie; if (!m) return;
    GFxValue v;
    int exists = as_get(m, "_level0.mmcover._visible", &v) && v.type == GFX_BOOL;
    if (g_coverWant && !exists) {
        as_fnv(m, "_level0.createEmptyMovieClip", "sn", "mmcover", 30000.0);
        double W = 1280, H = 800, d;
        if (as_get(m, "Stage.width", &v) && v.type == GFX_NUMBER && v.v.num > 100) W = v.v.num;
        if (as_get(m, "Stage.height", &v) && v.type == GFX_NUMBER && v.v.num > 100) H = v.v.num;
        as_fnv(m, "_level0.mmcover.beginFill", "nn", (double)0x07100d, 100.0);
        as_fnv(m, "_level0.mmcover.moveTo", "nn", -3000.0, -3000.0); as_fnv(m, "_level0.mmcover.lineTo", "nn", 6000.0, -3000.0);
        as_fnv(m, "_level0.mmcover.lineTo", "nn", 6000.0, 6000.0); as_fnv(m, "_level0.mmcover.lineTo", "nn", -3000.0, 6000.0);
        as_fnv(m, "_level0.mmcover.lineTo", "nn", -3000.0, -3000.0); as_fnv(m, "_level0.mmcover.endFill", "");
        d = H / 2 - H * 0.12;
        as_fnv(m, "_level0.mmcover.createTextField", "snnnnn", "t1", 1.0, W * 0.1, d, W * 0.8, H * 0.3);
        as_setb(m, "_level0.mmcover.t1.multiline", 1); as_setb(m, "_level0.mmcover.t1.wordWrap", 1);
        as_setb(m, "_level0.mmcover.t1.embedFonts", 1); as_setb(m, "_level0.mmcover.t1.html", 1); as_setb(m, "_level0.mmcover.t1.selectable", 0);
        g_coverH = H;
        logmsg("cover shown (stage %.0fx%.0f)", W, H); g_coverOn = 1; g_coverTextTick = 0;
    }
    if (!g_coverWant && exists) { as_fnv(m, "_level0.mmcover.removeMovieClip", ""); g_coverOn = 0; logmsg("cover removed"); }
    if (g_coverWant && exists && GetTickCount() - g_coverTextTick > 500) {
        g_coverTextTick = GetTickCount();
        static int dots; dots = (dots + 1) % 4; wchar_t dd[5] = L"...."; dd[dots] = 0;
        wchar_t h[700];
        int big = (int)(g_coverH * 0.07), small = (int)(g_coverH * 0.032);
        _snwprintf(h, 699, L"<p align='center'><font face='$TitleFont' size='%d' color='#E2C36B'>Match Found</font></p>"
                           L"<p align='center'><font face='$NormalFont' size='%d' color='#C9D6CF'>%ls%ls</font></p>", big, small, g_coverLine, dd);
        as_setw(m, "_level0.mmcover.t1.htmlText", h);
    }
}
static void cover(const wchar_t *line) { if (line) wcsncpy(g_coverLine, line, 159); g_coverWant = 1; }

// ---------------- saved choice: race / chapter / hero picked by the player ----------------
int g_race = -1, g_chapter = -1, g_hero = -1, g_random; static volatile int g_heroCount; static volatile int g_curRace = -1, g_curChapter = -1, g_curHero = -1; static void *g_heroSelf; static int g_heroFlags; static volatile DWORD g_autoSelTick, g_lastFsTick; static volatile int g_applyMM, g_mmApplied; static volatile DWORD g_heroAt;
static wchar_t g_ini[MAX_PATH];   // g_heroCount: heroes of the race on screen (from Receive_PopulateHeroList)
static void save_choice(void) {
    wchar_t b[16];
    _snwprintf(b, 16, L"%d", g_race); WritePrivateProfileStringW(L"Choice", L"Race", b, g_ini);
    _snwprintf(b, 16, L"%d", g_chapter); WritePrivateProfileStringW(L"Choice", L"Chapter", b, g_ini);
    _snwprintf(b, 16, L"%d", g_hero); WritePrivateProfileStringW(L"Choice", L"Hero", b, g_ini);
    _snwprintf(b, 16, L"%d", g_random); WritePrivateProfileStringW(L"Choice", L"Random", b, g_ini);
}
// unchecked maps per mode: [Maps] Off1v1=name|name|...  (new maps from Elite updates come in checked)
static void load_prefs(void) {
    for (int m = 0; m < 4; m++) {
        wchar_t key[16], buf[8192]; _snwprintf(key, 16, L"Off%ls", MODE_W[m]);
        memset(g_off[m], 0, sizeof g_off[m]);
        GetPrivateProfileStringW(L"Maps", key, L"", buf, 8192, g_ini);
        int bans = 0;                                          // older saves may hold more bans than the ladder allows now
        for (wchar_t *t = wcstok(buf, L"|"); t; t = wcstok(NULL, L"|")) { int i = map_index(t); if (i >= 0 && mode_map(i, m) && bans < MAX_BANS) { g_off[m][i] = 1; bans++; } }
    }
}
static void save_prefs(int m) {
    static wchar_t buf[8192]; int L = 0; buf[0] = 0; wchar_t key[16]; _snwprintf(key, 16, L"Off%ls", MODE_W[m]);
    for (int i = 0; i < g_nmaps && L < 8000; i++) if (g_off[m][i]) L += _snwprintf(buf + L, 8192 - L, L"%ls%ls", L ? L"|" : L"", g_maps[i]);
    WritePrivateProfileStringW(L"Maps", key, buf, g_ini);
}
static int apply_race(void) {                    // saved race / chapter into the hero panel of the current screen; 1 = changed something
    char a[16]; int ch = 0;
    if (GetPrivateProfileIntW(L"MMFind", L"DebugForceRace", 0, g_ini)) g_curRace = g_curChapter = -1;   // test: as if the shown race were unknown
    if (g_race >= 0 && g_race != g_curRace) { snprintf(a, sizeof a, "0;%d", g_race); fs(".hero_selection_mc.OnSelectDropDownItem", a); ch = 1; }
    if (g_chapter >= 0 && g_chapter != g_curChapter) { snprintf(a, sizeof a, "1;%d", g_chapter); fs(".hero_selection_mc.OnSelectDropDownItem", a); ch = 1; }
    return ch;
}
// the saved hero, as its own step some frames after the race change: picking it from inside the engine's hero-list refresh
// (nested call) killed Elite 3.2 in the lobby - "Assertion failed: i < count" in rlink vector.h
static void apply_hero(void) {
    if (g_hero < 0) return; char a[16]; snprintf(a, sizeof a, "%d", g_hero); fs(".hero_selection_mc.OnSelectHero", a);
    if (g_heroSelf) { GFxValue v; memset(&v, 0, sizeof v); v.type = GFX_NUMBER; v.v.num = g_hero; g_invOrig(g_heroSelf, NULL, "Receive_SelectHero", g_heroFlags, &v, 1); }
    logmsg("hero: set to %d", g_hero);
}

// ---------------- host without the "Game Name" box ----------------
// GameListPanel::OnClickHost (0x46CB44) only opens a text box and passes it a callback 0x46CC53(bool ok, LocString *name);
// the callback stores the name and calls the real "host a game" (0x46C2C9). While we host for the ladder we skip the box
// and call the callback ourselves, exactly as if the player typed the name and pressed OK.
#define FN_ONCLICKHOST 0x0046CB44
#define FN_HOSTNAME_CB 0x0046CC53
static const unsigned char HOST_PRO[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
typedef void (__fastcall *OnClickHostFn)(void *panel, void *edx, int arg);
typedef void (__fastcall *HostCbFn)(void *panel, void *edx, int ok, void *name);
typedef void (__fastcall *LocCtorFn)(void *self, void *edx, const wchar_t *s);
typedef void (__fastcall *LocDtorFn)(void *self, void *edx);
static OnClickHostFn g_hostOrig; static volatile int g_hostDirect; static volatile int g_hostDone;
static void __fastcall host_hook(void *panel, void *edx, int arg) {
    if (!g_hostDirect) { g_hostOrig(panel, edx, arg); return; }
    g_hostDirect = 0;
    HMODULE loc = GetModuleHandleA("Localizer.dll");
    LocCtorFn ctor = loc ? (LocCtorFn)GetProcAddress(loc, "??0LocString@@QAE@PB_W@Z") : NULL;
    LocDtorFn dtor = loc ? (LocDtorFn)GetProcAddress(loc, "??1LocString@@UAE@XZ") : NULL;
    if (!ctor || !dtor) { logmsg("host: Localizer exports not found - falling back to the box"); g_hostOrig(panel, edx, arg); return; }
    wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, g_gameName, -1, w, 64);
    static __attribute__((aligned(16))) unsigned char ls[64]; memset(ls, 0, sizeof ls);
    ctor(ls, NULL, w);
    logmsg("host: direct create '%s' (panel %p)", g_gameName, panel);
    ((HostCbFn)FN_HOSTNAME_CB)(panel, NULL, 1, ls);
    dtor(ls, NULL);
    g_hostDone = 1;
}
static void install_host_hook(void) {
    unsigned char *t = (unsigned char *)FN_ONCLICKHOST;
    if (IsBadReadPtr(t, 6) || memcmp(t, HOST_PRO, 6)) { logmsg("OnClickHost prologue mismatch - the name box will be used"); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, HOST_PRO, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (FN_ONCLICKHOST + 6) - (DWORD)(tr + 11); g_hostOrig = (OnClickHostFn)tr;
    DWORD o; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &o); t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)host_hook - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("hooked OnClickHost (direct host)");
}

// ---------------- the front-end message box movie (to keep "Connecting..." out of sight during our join) ----------------
#define FN_SFMOVIE_CTOR 0x00681B50          // __stdcall SFMovie(this, const char *path)
static const unsigned char SFM_PRO[7] = {0x6A, 0xFF, 0x68, 0x90, 0x5C, 0xEE, 0x00};
typedef void *(__stdcall *SfmCtorFn)(void *self, const char *path);
static SfmCtorFn g_sfmOrig; static void *g_msgWrap;
static void *__stdcall sfm_hook(void *self, const char *path) {
    void *r = g_sfmOrig(self, path);
    if (path && !IsBadReadPtr(path, 4) && strstr(path, "front_end\\message_box")) { g_msgWrap = self; logmsg("message box movie wrapper %p", self); }
    return r;
}
static void install_sfm_hook(void) {
    unsigned char *t = (unsigned char *)FN_SFMOVIE_CTOR;
    if (IsBadReadPtr(t, 7) || memcmp(t, SFM_PRO, 7)) { logmsg("SFMovie ctor prologue mismatch"); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, SFM_PRO, 7); tr[7] = 0xE9; *(DWORD *)(tr + 8) = (FN_SFMOVIE_CTOR + 7) - (DWORD)(tr + 12); g_sfmOrig = (SfmCtorFn)tr;
    DWORD o; VirtualProtect(t, 7, PAGE_EXECUTE_READWRITE, &o); t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)sfm_hook - (DWORD)(t + 5); t[5] = t[6] = 0x90;
    VirtualProtect(t, 7, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 7);
}

// ---------------- Map Preferences dialog: all Elite maps of the chosen mode, checked = may be played ----------------
// The stock dialog shows Relic's ranked pool; we fill it ourselves and answer its commands ourselves (the engine never sees them).
static void *g_mpSelf; static int g_mpFlags, g_mpMode = -1, g_mpIdx[MAXMAPS], g_mpN;
static GFxValue g_mpArgs[1 + 2 * MAXMAPS]; static wchar_t g_mpText[200]; static int g_relicN = 10;
static int build_maplist(void) {
    g_mpN = 0; int n = 1;
    for (int i = 0; i < g_nmaps; i++) if (mode_map(i, g_mode)) g_mpIdx[g_mpN++] = i;
    _snwprintf(g_mpText, 199, L"%ls: uncheck maps you do not want to play.", MODE_W[g_mode]);
    memset(g_mpArgs, 0, sizeof g_mpArgs);
    g_mpArgs[0].type = GFX_WSTRING; g_mpArgs[0].v.ws = g_mpText;
    for (int k = 0; k < g_mpN; k++) {
        g_mpArgs[n].type = GFX_WSTRING; g_mpArgs[n].v.ws = g_maps[g_mpIdx[k]]; n++;
        g_mpArgs[n].type = GFX_NUMBER; g_mpArgs[n].v.num = g_off[g_mode][g_mpIdx[k]] ? 2 : 0; n++;
    }
    g_mpMode = g_mode; return n;
}
static int parse_states(const char *a, unsigned char *st) {       // "0,2,0,..." -> st[k] = 1 if unchecked
    int k = 0; for (const char *p = a; *p && k < g_mpN; ) { st[k++] = atoi(p) == 2; p = strchr(p, ','); if (!p) break; p++; }
    return k;
}

// ---------------- lobby slots without the pop-up menu ----------------
// PlayerListPanel: a click on a slot (fscommand OnItemClick / OnObserverItemClick "<slot>") calls 0x491D51(panel, slot), which stores
// the slot in panel+0x50 and pops up a menu; each menu item is a __thiscall(panel, int, int) handler that reads panel+0x50.
// For our own actions we keep the menu away and call the handler right away.
#define FN_SLOTMENU   0x00491D51
#define CB_MOVE       0x00492E02      // "Move to this slot" (player slot)
#define CB_MOVE_OBS   0x00492EF1      // "Move to this slot" (observer slot)
#define CB_OPEN_OBS   0x00493396      // "Open All Observer Slots"
#define CB_KICK       0x00493183      // "Kick this player"
#define CB_AI_EASY    0x004933AE      // "Add Easy Computer" (Normal 0x4933CC, Hard 0x4933EA, Expert 0x493408)
static const DWORD CB_AI[4] = {0x004933AE, 0x004933CC, 0x004933EA, 0x00493408};
static const unsigned char SLOTMENU_PRO[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
typedef void (__stdcall *SlotMenuFn)(void *panel, int slot);
typedef void (__fastcall *SlotCbFn)(void *panel, void *edx, int a, int b);
static SlotMenuFn g_slotMenuOrig; static volatile DWORD g_slotCb;
static void __stdcall slotmenu_hook(void *panel, int slot) {
    if (!g_slotCb) { g_slotMenuOrig(panel, slot); return; }
    DWORD cb = g_slotCb; g_slotCb = 0;
    *(int *)((char *)panel + 0x50) = slot;
    logmsg("slot %d: action %08lX", slot, cb);
    ((SlotCbFn)cb)(panel, NULL, 0, 0);
}
static void install_slot_hook(void) {
    unsigned char *t = (unsigned char *)FN_SLOTMENU;
    if (IsBadReadPtr(t, 6) || memcmp(t, SLOTMENU_PRO, 6)) { logmsg("slot menu prologue mismatch - no slot control"); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, SLOTMENU_PRO, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (FN_SLOTMENU + 6) - (DWORD)(tr + 11); g_slotMenuOrig = (SlotMenuFn)tr;
    DWORD o; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &o); t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)slotmenu_hook - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("hooked slot menu");
}
static int team_n(void) { return g_matchMode + 1; }
static int is_obs_slot(int s) { int n = team_n(); return (s >= n && s < 4) || (s >= 4 + n && s < 8); }
static void slot_do(int slot, DWORD cb) {
    char a[8]; snprintf(a, sizeof a, "%d", slot); g_slotCb = cb;
    fs(is_obs_slot(slot) ? ".player_list_panel_mc.OnObserverItemClick" : ".player_list_panel_mc.OnItemClick", a);
    if (g_slotCb) { g_slotCb = 0; logmsg("slot %d: the engine did not open the slot handler", slot); }
}

// who sits where: GameInfo slot array (+0x70, stride 0xC8): +8 SteamID64, +0x98 position = lobby slot (as LobbyStats)
#define G_SESSION_ 0x01335014
#define FN_INFO_CTOR_ 0x0040172D
#define FN_INFO_DTOR_ 0x00401794
typedef void (__stdcall *Fn1_)(void *);
typedef void *(__fastcall *GetMatchFn_)(void *self, void *edx);
typedef char (__stdcall *GetInfoFn_)(void *self, void *info);
static int lobby_ids(unsigned long long *ids) {         // ids[8] by slot; returns number of occupied slots
    memset(ids, 0, 8 * sizeof *ids);
    void **mgr = *(void ***)G_SESSION_; if (!mgr || IsBadReadPtr(mgr, 4)) return -1;
    void **vt = *(void ***)mgr; if (IsBadReadPtr(vt, 0x28)) return -1;
    void *m = ((GetMatchFn_)vt[0x24 / 4])(mgr, NULL); if (!m || IsBadReadPtr(m, 8)) return -1;
    static __attribute__((aligned(16))) unsigned char info[0x2000];
    memset(info, 0, sizeof info); ((Fn1_)FN_INFO_CTOR_)(info);
    int found = 0;
    if (((GetInfoFn_)(*(void ***)m)[1])(m, info)) {
        unsigned char *base = *(unsigned char **)(info + 0x70); int n = *(int *)(info + 0x78);
        if (base && n > 0 && n <= 64 && !IsBadReadPtr(base, n * 0xC8))
            for (int i = 0; i < n; i++) {
                unsigned char *sl = base + i * 0xC8; unsigned long long id = *(unsigned long long *)(sl + 8); int pos = *(int *)(sl + 0x98);
                if (id != ~0ULL && id && pos >= 0 && pos < 8) { ids[pos] = id; found++; }
            }
    }
    ((Fn1_)FN_INFO_DTOR_)(info);
    return found;
}
// a lobby slot holds the Relic profile id (sometimes the SteamID) - match both; the name is a second check
static int is_me(unsigned long long id) { return id && (id == g_mySteam || id == g_myPid); }
static int in_roster(unsigned long long id) { for (int i = 0; i < g_nR; i++) if (id && (g_rSteam[i] == id || g_rPid[i] == id)) return i; return -1; }
static int is_watcher(unsigned long long id) { for (int i = 0; i < g_nW; i++) if (id && (g_wSteam[i] == id || g_wPid[i] == id)) return 1; return 0; }
static int name_known(const wchar_t *n) {
    if (!n || !n[0]) return 1;                         // no name seen yet - do not treat as a stranger
    for (int i = 0; i < g_nR; i++) if (!wcscmp(n, g_rName[i])) return 1;
    for (int i = 0; i < g_nW; i++) if (!wcscmp(n, g_wName[i])) return 1;
    return 0;
}

// ---------------- replay upload after a multiplayer match ----------------
// The game writes the last match to Documents\My Games\Dawn of War II - Retribution\Playback\temp.rec. Back in the menu after a match
// with other people we send it to the replay site (rts.fintebtc.de/replay, same mirrors as the ladder); its id is in MMFind.log.
static volatile int g_recHumans, g_recBots; static FILETIME g_recStart; static char g_recName[48];
static DWORD WINAPI replay_upload(LPVOID _) {
    Sleep(3000);                                                      // let the game finish writing the file
    wchar_t doc[MAX_PATH], path[MAX_PATH]; if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, doc) != S_OK) return 0;
    _snwprintf(path, MAX_PATH, L"%ls\\My Games\\Dawn of War II - Retribution\\Playback\\temp.rec", doc);
    WIN32_FILE_ATTRIBUTE_DATA fa; if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) { logmsg("replay: no temp.rec"); return 0; }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_recStart) < 0) { logmsg("replay: temp.rec is older than the match - not sent"); return 0; }
    DWORD size = fa.nFileSizeLow; if (fa.nFileSizeHigh || size < 64 || size > 30u << 20) { logmsg("replay: odd size %lu - not sent", size); return 0; }
    char *buf = malloc(size); if (!buf) return 0; DWORD got = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, buf, size, &got, NULL); CloseHandle(f); }
    if (got != size || memcmp(buf + 12, "DOW2_REC", 8)) { logmsg("replay: could not read temp.rec"); free(buf); return 0; }
    SYSTEMTIME t; GetLocalTime(&t); wchar_t hdr[300];            // file name on the site: MM-m2-xxxx_2026-10-27_12-31.rec / match_...
    _snwprintf(hdr, 300, L"Content-Type: application/octet-stream\r\nX-Filename: %hs_%04d-%02d-%02d_%02d-%02d.rec\r\nX-Lobby: %hs\r\n",
               g_recName[0] ? g_recName : "match", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, g_recName);   // lobby: the ladder finds its replay
    char out[512]; int ok = 0;
    for (int k = 0; k < g_nHosts && !ok; k++) {                       // the analysis runs while we wait: long receive timeout
        int h = (g_curHost + k) % g_nHosts; out[0] = 0; int n = 0; DWORD code = 0, sz = sizeof code;
        HINTERNET s = WinHttpOpen(L"MMFind/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0); if (!s) break;
        WinHttpSetTimeouts(s, 4000, 4000, 60000, 300000);
        HINTERNET c = WinHttpConnect(s, g_hosts[h], INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", L"/replay/api/upload", NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
        if (r && WinHttpSendRequest(r, hdr, (DWORD)-1, buf, size, size, 0) && WinHttpReceiveResponse(r, NULL)) {
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
            DWORD m; while (n < (int)sizeof out - 1 && WinHttpReadData(r, out + n, sizeof out - 1 - n, &m) && m) n += m;
            out[n] = 0; ok = code && code < 500;
        }
        if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
        logmsg("replay: %lu bytes -> %ls: HTTP %lu %s", size, g_hosts[h], code, out);
    }
    free(buf); return 0;
}
static void replay_after_match(void) {
    if ((g_recHumans < 2 && !g_recBots) || !GetPrivateProfileIntW(L"Replays", L"Upload", 1, g_ini)) return;   // skirmish vs AI: not needed (our AI tests: yes)
    CloseHandle(CreateThread(NULL, 0, replay_upload, NULL, 0, NULL));
}

// ---------------- hooks ----------------
static wchar_t g_st1[128], g_st2[128], g_st3[128], g_btn[64];
static void status_text(void) {
    DWORD s = (GetTickCount() - g_searchTick) / 1000;
    switch (g_state) {
    case S_SEARCH:
        _snwprintf(g_st1, 127, L"Searching for an opponent... %u:%02u", s / 60, s % 60);
        if (g_queue >= 0) _snwprintf(g_st2, 127, L"Players searching %ls: %d", MODE_W[g_mode], g_queue); else wcscpy(g_st2, L"Connecting to the ladder...");
        wcscpy(g_st3, L"Elite ladder automatch"); break;
    default:
        _snwprintf(g_st1, 127, L"Opponent found: %ls", g_oppName[0] ? g_oppName : L"player");
        wcscpy(g_st2, g_role == 1 ? L"Creating the game..." : L"Joining the game..."); g_st3[0] = 0;
    }
}
static int searching_ui(void) { return g_state == S_SEARCH || (g_state >= S_GO_MAIN && g_state <= S_GO_LIST); }

static void on_player_list(GFxValue *a, unsigned n) {
    if (n < 5 || (n - 5) % 20) return;
    int humans = 0, opp = 0; for (int i = 0; i < 8; i++) g_slotName[i][0] = 0;
    { static char last[4000]; char cur[4000]; int L = 0;                // log the slot fields when they change (ready flag position)
      for (unsigned i = 0; i < (n - 5) / 20 && L < 3800; i++) { GFxValue *q = a + 5 + i * 20; if (q[0].type != GFX_BOOL || !q[0].v.b) continue;
          L += snprintf(cur + L, sizeof cur - L, " |s%d", i);
          for (int k = 0; k < 20 && L < 3800; k++) { if (q[k].type == GFX_BOOL) L += snprintf(cur + L, sizeof cur - L, " %d:%c", k, q[k].v.b ? 'T' : 'F');
              else if (q[k].type == GFX_NUMBER) L += snprintf(cur + L, sizeof cur - L, " %d:%g", k, q[k].v.num); } }
      cur[L] = 0; if (strcmp(cur, last)) { strcpy(last, cur); logmsg("slots:%s", cur); } }
    if (g_testBots == 2) { static char last2[4000]; char cur[4000]; int L = 0;        // AI tests: every row (computers are not in the list above)
      for (unsigned i = 0; i < (n - 5) / 20 && L < 3800; i++) { GFxValue *q = a + 5 + i * 20;
          L += snprintf(cur + L, sizeof cur - L, " |r%u", i);
          for (int k = 0; k < 8 && L < 3800; k++) { if (q[k].type == GFX_BOOL) L += snprintf(cur + L, sizeof cur - L, " %d:%c", k, q[k].v.b ? 'T' : 'F');
              else if (q[k].type == GFX_NUMBER) L += snprintf(cur + L, sizeof cur - L, " %d:%g", k, q[k].v.num);
              else if (q[k].type == GFX_WSTRING && q[k].v.ws) { char u[64]; WideCharToMultiByte(CP_UTF8, 0, q[k].v.ws, -1, u, sizeof u, NULL, NULL); L += snprintf(cur + L, sizeof cur - L, " %d:'%s'", k, u); } } }
      cur[L] = 0; if (strcmp(cur, last2)) { strcpy(last2, cur); logmsg("rows:%s", cur); } }
    { int all = 1, cnt = 0; if (g_readyIdx >= 0) for (unsigned i = 0; i < (n - 5) / 20; i++) { GFxValue *q = a + 5 + i * 20;
          if (q[0].type != GFX_BOOL || !q[0].v.b || q[4].type != GFX_NUMBER || (int)q[4].v.num != 1) continue;   // other humans (the host itself has no ready mark)
          if (q[3].type == GFX_NUMBER && is_obs_slot((int)q[3].v.num)) continue;            // observers don't ready up
          cnt++; if (q[g_readyIdx].type != GFX_BOOL || !q[g_readyIdx].v.b) all = 0; }
      g_allReady = g_readyIdx >= 0 && all && (cnt >= 1 || g_bots); }
    for (unsigned i = 0; i < (n - 5) / 20; i++) {
        GFxValue *s = a + 5 + i * 20;
        if (s[0].type != GFX_BOOL || !s[0].v.b || s[4].type != GFX_NUMBER) continue;
        int st = (int)s[4].v.num;
        if (s[0].type == GFX_BOOL && s[0].v.b && s[3].type == GFX_NUMBER && s[5].type == GFX_WSTRING && s[5].v.ws) { int si = (int)s[3].v.num; if (si >= 0 && si < 8) { wcsncpy(g_slotName[si], s[5].v.ws, 63); g_slotName[si][63] = 0; } }
        if (st == 0 && s[6].type == GFX_NUMBER) g_lobbyRace = (int)s[6].v.num;
        if (st != 0 && st != 1) continue;       // 0 me, 1 other human (2.. AI / open / closed)
        humans++;
        if (st == 1 && s[5].type == GFX_WSTRING && s[5].v.ws && g_oppName[0] && !wcsncmp(s[5].v.ws, g_oppName, 63)) opp = 1;
    }
    if (humans != g_humans || opp != g_oppIn) logmsg("lobby: humans=%d opponent_in=%d", humans, opp);
    g_humans = humans; g_oppIn = opp;
}
static void on_game_list(GFxValue *a, unsigned n) {
    g_listSeen = 1;
    if (!g_gameName[0] || n < 2) return;
    wchar_t want[48]; MultiByteToWideChar(CP_UTF8, 0, g_gameName, -1, want, 48);
    int row = -1;
    for (unsigned i = 0; 2 + i * 11 < n; i++) {
        GFxValue *r = a + 2 + i * 11;
        if (r[0].type == GFX_WSTRING && r[0].v.ws && !wcscmp(r[0].v.ws, want)) { row = i; break; }
    }
    if (row != g_rowFound) logmsg("game list: %u rows, %s at row %d", (n - 2) / 11, g_gameName, row);
    g_rowFound = row;
}

static char __fastcall inv_hook(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count) {
    if (g_logInvoke && method && args && count < 64 && !IsBadReadPtr(args, count * sizeof(GFxValue))     // debug: engine -> Flash calls
        && strcmp(method, "Receive_ShowSearchingAnimation") && strcmp(method, "Receive_UpdateSearchStatus") && strncmp(method, "Receive_SetFrameRate", 20)) {
        char b[700]; int L = snprintf(b, sizeof b, "inv %s(%u):", method, count);
        for (unsigned i = 0; i < count && i < 12 && L < 600; i++) {
            if (args[i].type == GFX_NUMBER) L += snprintf(b + L, sizeof b - L, " %g", args[i].v.num);
            else if (args[i].type == GFX_BOOL) L += snprintf(b + L, sizeof b - L, " %s", args[i].v.b ? "T" : "F");
            else if (args[i].type == GFX_WSTRING && args[i].v.ws) { char u[80]; WideCharToMultiByte(CP_UTF8, 0, args[i].v.ws, -1, u, sizeof u, NULL, NULL); u[79] = 0; L += snprintf(b + L, sizeof b - L, " '%s'", u); }
            else if (args[i].type == GFX_STRING && args[i].v.s && !IsBadReadPtr(args[i].v.s, 1)) L += snprintf(b + L, sizeof b - L, " \"%.70s\"", args[i].v.s);
            else L += snprintf(b + L, sizeof b - L, " t%d", args[i].type);
        }
        static char last[700]; if (strcmp(b, last)) { strcpy(last, b); logmsg("%s", b); }
    }
    if (method && args && count < 4096 && !IsBadReadPtr(args, count * sizeof(GFxValue))) {
        if (!strcmp(method, "Receive_ShowSearchingAnimation")) { g_mmSelf = self; g_mmFlags = flags; }
        if (!strcmp(method, "Receive_PopulateDropDownListEx") && count > 2 && args[0].type == GFX_NUMBER && args[0].v.num == 0) {
            int n = 0;                                                  // map list: name, enabled, name, enabled...
            for (unsigned i = 0; 2 + i * 2 < count && n < MAXMAPS; i++)
                if (args[2 + i * 2].type == GFX_WSTRING && args[2 + i * 2].v.ws) { wcsncpy(g_maps[n], args[2 + i * 2].v.ws, 63); g_maps[n][63] = 0; n++; }
            if (n != g_nmaps) { g_nmaps = n; load_prefs(); logmsg("map catalog: %d maps", n); }
        }
        if (!strcmp(method, "Receive_PopulateDropDownList") && g_screen == SC_MM && count == 6 && args[0].type == GFX_NUMBER && args[0].v.num == 0
            && args[5].type == GFX_WSTRING) { static wchar_t w[] = L"Watch a match"; args[5].v.ws = w; }
        if (!strcmp(method, "Receive_CreateMapList") && g_nmaps) {
            g_relicN = count > 1 ? (int)(count - 1) / 2 : 10;
            g_mpSelf = self; g_mpFlags = flags; int n = build_maplist();
            logmsg("map preferences: %d maps for %s", g_mpN, MODE_KEY[g_mode]);
            return g_invOrig(self, edx, method, flags, g_mpArgs, n);
        }
        if (!strcmp(method, "Receive_UpdateButtonText") && count >= 1 && args[0].type == GFX_WSTRING && args[0].v.ws && wcscmp(g_btnText, args[0].v.ws)) {
            wcsncpy(g_btnText, args[0].v.ws, 63); g_btnText[63] = 0; if (g_state == S_SETUP_HERO || g_state == S_HOST_NAME) wcscpy(g_aloneTxt, g_btnText); /* host alone: "Start Skirmish" */ char b[96]; WideCharToMultiByte(CP_UTF8, 0, g_btnText, -1, b, sizeof b, NULL, NULL); logmsg("lobby button: %s", b); }
        if (!strcmp(method, "Receive_SetEnabledState") && count >= 1 && args[0].type == GFX_BOOL) g_btnEnabled = args[0].v.b;
        if (!strcmp(method, "Receive_SetReadyState") && count >= 1 && args[0].type == GFX_BOOL && g_readyMe != args[0].v.b) { g_readyMe = args[0].v.b; logmsg("my ready: %d", g_readyMe); }
        if (!strcmp(method, "Receive_SetGamerTag") && count >= 1 && args[0].type == GFX_WSTRING && args[0].v.ws) wcsncpy(g_myName, args[0].v.ws, 63);
        else if (!strcmp(method, "Receive_PopulatePlayerList")) on_player_list(args, count);
        else if (!strcmp(method, "Receive_PopulateDropDownList") && count >= 3 && args[0].type == GFX_NUMBER && g_screen == SC_SETUP) {
            int id = (int)args[0].v.num;
            if (id >= 1 && id <= 4) { g_optSelf = self; g_optWant[id] = -1;
                for (unsigned i = 2; i < count && g_optWant[id] < 0; i++) if (args[i].type == GFX_WSTRING && args[i].v.ws)
                    for (int w = 0; w < 3 && OPT_WANT[id][w]; w++) if (!wcscmp(args[i].v.ws, OPT_WANT[id][w])) { g_optWant[id] = (int)i - 2; break; } }
        }
        else if (!strcmp(method, "Receive_PopulateGameList")) on_game_list(args, count);
        else if (!strcmp(method, "Receive_SelectHero")) { g_autoSelTick = GetTickCount(); g_heroSelf = self; g_heroFlags = flags;
            if (count >= 1 && args[0].type == GFX_NUMBER) g_curHero = (int)args[0].v.num; }
        else if (!strcmp(method, "Receive_PopulateHeroList")) { g_heroSelf = self; g_heroFlags = flags;
            int imgs = 0;                                                 // every hero has two portraits from ...\heroes\ (abilities: other folders)
            for (unsigned i = 0; i < count; i++) {
                if (args[i].type == GFX_STRING && args[i].v.s && strstr(args[i].v.s, "\\heroes\\")) imgs++;
                else if (args[i].type == GFX_WSTRING && args[i].v.ws && wcsstr(args[i].v.ws, L"\\heroes\\")) imgs++;
            }
            if (imgs >= 2) g_heroCount = imgs / 2;
        }
        else if (!strcmp(method, "Receive_PopulateDropDownListWithImage") && count >= 4 &&      // the race list (race_*_x icons) arrives before
                 ((args[3].type == GFX_WSTRING && args[3].v.ws && wcsstr(args[3].v.ws, L"\\race_")) ||   // the race selection and the hero list
                  (args[3].type == GFX_STRING && args[3].v.s && strstr(args[3].v.s, "\\race_")))) { g_heroSelf = self; g_heroFlags = flags; }
        else if (!strcmp(method, "Receive_SetDropDownSelection") && count == 2 && args[0].type == GFX_NUMBER && args[1].type == GFX_NUMBER) {
            int dd = (int)args[0].v.num, v = (int)args[1].v.num;
            if (self == g_heroSelf) { if (dd == 0) { if (v != g_curRace) g_curChapter = -1; g_curRace = v; } else if (dd == 1) g_curChapter = v; }
            else if (self == g_optSelf && dd >= 1 && dd <= 4) g_optCur[dd] = v;
            else if (g_screen == SC_MM && dd == 0 && v != g_mode && v >= 0 && v < 4) {   // Match Type: the engine keeps resetting it - show the player's choice
                static int n; if (n++ < 20) logmsg("match type: engine set %d, keeping %d", v, g_mode);
                args[1].v.num = g_mode;
            }
        }
        else if (!strcmp(method, "Receive_ShowHeroSelect")) { g_heroShown = 1; if (g_screen == SC_MM && g_state == S_IDLE && !g_mmApplied) { g_applyMM = 1; g_mmApplied = 1; } }
        else if (!strcmp(method, "Receive_HideHeroSelect")) g_heroShown = 0;
        else if (searching_ui() && g_screen == SC_MM) {             // our search: the panel shows our state, not Relic's
            if (!strcmp(method, "Receive_ShowSearchingAnimation") && count == 1) { args[0].type = GFX_BOOL; args[0].v.b = 1; }
            else if (!strcmp(method, "Receive_SetMapPreferencesButtonVisible") && count == 1) { args[0].type = GFX_BOOL; args[0].v.b = 0; }
            else if (!strcmp(method, "Receive_SetStartButtonText") && count == 1) { wcscpy(g_btn, L"Cancel Searching"); args[0].type = GFX_WSTRING; args[0].v.ws = g_btn; }
            else if (!strcmp(method, "Receive_UpdateSearchStatus") && count == 3) {
                status_text(); args[0].type = args[1].type = args[2].type = GFX_WSTRING;
                args[0].v.ws = g_st1; args[1].v.ws = g_st2; args[2].v.ws = g_st3;
            }
        }
    }
    char rc = g_invOrig(self, edx, method, flags, args, count);
    // our status line: pushed right after the engine's own per-frame call into the Find a Game panel, on its thread
    if (method && !strcmp(method, "Receive_ShowSearchingAnimation") && g_screen == SC_MM && g_banNote && GetTickCount() - g_banNote < 8000 && !searching_ui()) {
        GFxValue v[3]; memset(v, 0, sizeof v); static wchar_t a1[] = L"Only 3 maps can be banned per mode.", a2[] = L"Можно убрать не больше 3 карт на режим.", a3[] = L"The first 3 unchecked maps are kept.";
        v[0].type = v[1].type = v[2].type = GFX_WSTRING; v[0].v.ws = a1; v[1].v.ws = a2; v[2].v.ws = a3;
        g_invOrig(self, edx, "Receive_UpdateSearchStatus", flags, v, 3);
    }
    if (method && !strcmp(method, "Receive_ShowSearchingAnimation") && g_screen == SC_MM && g_ffaNote && GetTickCount() - g_ffaNote < 6000 && !searching_ui()) {
        GFxValue v[3]; memset(v, 0, sizeof v); static wchar_t a1[] = L"Team Free For All is not in the ladder.", a2[] = L"Pick 1v1, 2v2 or Team Battle (3v3).", a3[] = L"";
        v[0].type = v[1].type = v[2].type = GFX_WSTRING; v[0].v.ws = a1; v[1].v.ws = a2; v[2].v.ws = a3;
        g_invOrig(self, edx, "Receive_UpdateSearchStatus", flags, v, 3);
    }
    if (method && !strcmp(method, "Receive_ShowSearchingAnimation") && searching_ui() && g_screen == SC_MM) {
        static DWORD last; DWORD now = GetTickCount();
        if (now - last > 1000) {
            last = now; GFxValue v[3]; memset(v, 0, sizeof v); status_text();
            v[0].type = v[1].type = v[2].type = GFX_WSTRING; v[0].v.ws = g_st1; v[1].v.ws = g_st2; v[2].v.ws = g_st3;
            g_invOrig(self, edx, "Receive_UpdateSearchStatus", flags, v, 3);
        }
    }
    return rc;
}
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static void __fastcall fs_hook(void *self, void *edx, void *movie, const char *cmd, const char *args) {
    if (cmd && !IsBadReadPtr(cmd, 1)) {
        if (g_verbose && strcmp(cmd, "OnPlaySound")) logmsg("fs '%s' '%s'", cmd, args && !IsBadReadPtr(args, 1) ? args : "");

        if (!strncmp(cmd, "_level0.display_item", 20) || (!strcmp(cmd, "OnLoadComplete") && args && strstr(args, "name:=_level0.display_item"))) {
            if (movie != g_fsMovie && (g_state == S_DONE || g_loading)) { marker_set(NULL); g_loadTick = 0; g_state = S_IDLE; g_coverWant = 0; g_coverOn = 0; g_loading = 0; g_wantEnded = 1; replay_after_match(); SetEvent(g_wake); logmsg("back in the menu after the match"); }
            g_fsSelf = self; g_fsMovie = movie;
        }
        if (movie == g_fsMovie) g_lastFsTick = GetTickCount();
        if (!strcmp(cmd, "OnLoadMovieComplete") && args && !strncmp(args, "/mp_mc", 6) && !g_loading) {
            g_loading = 1; g_recHumans = g_humans; g_recBots = g_bots && g_state == S_DONE; GetSystemTimeAsFileTime(&g_recStart);
            if (g_state == S_DONE && !strncmp(g_gameName, "MM-", 3) && !g_bots) { marker_set(g_gameName); g_loadTick = GetTickCount(); }
            if (g_gameName[0] && g_state == S_DONE) strcpy(g_recName, g_gameName); else g_recName[0] = 0;
            logmsg("loading screen - hands off the menu (%d people in the lobby)", g_humans); }
        if (!strcmp(cmd, "OnLoadComplete") && args) {                 // which full-screen panel is up
            const char *nm = strstr(args, "name:="), *url = strstr(args, "url:=");
            if (nm && url) {
                char path[160] = ""; const char *e = strchr(nm, ';'); int len = e ? (int)(e - nm - 6) : 0;
                if (len > 0 && len < 159) { memcpy(path, nm + 6, len); path[len] = 0; }
                int sc = -1;
                if (strstr(url, "main_menu.gfx")) sc = SC_MAIN; else if (strstr(url, "matchmaking.gfx")) sc = SC_MM;
                else if (strstr(url, "game_list_movie.gfx")) sc = SC_LIST; else if (strstr(url, "game_setup_movie.gfx")) sc = SC_SETUP;
                if (sc >= 0 && !strcmp(path + strlen(path) - 9, ".panel_mc")) {
                    strcpy(g_panel, path); g_screen = sc; if (sc != SC_MM) g_mmSelf = NULL; else g_mmApplied = 0; if (sc == SC_LIST) { g_listSeen = 0; g_rowFound = -1; }
                    logmsg("screen %d panel %s", sc, g_panel);
                }
            }
        }
        // the player's own pick - remember it (not while we set up the lobby ourselves: the panel answers our hero pick with its
        // own OnSelectHero, which turned the saved Random off after a random pick)
        if (!g_sending && g_state != S_SETUP_HERO && strstr(cmd, "hero_selection_mc.") && args) {
            int a = -1, b = -1;
            if (ends_with(cmd, ".OnClickRandom")) { if (!g_random) { g_random = 1; save_choice(); logmsg("choice: random race and hero"); } }
            else if (ends_with(cmd, ".OnSelectDropDownItem") && sscanf(args, "%d;%d", &a, &b) == 2) {
                if (g_random) { g_random = 0; save_choice(); logmsg("choice: random off"); }
                if (a == 0 && b != g_race) { g_race = b; g_chapter = -1; g_hero = -1; save_choice(); logmsg("choice: race %d", b); }
                else if (a == 1 && b != g_chapter) { g_chapter = b; save_choice(); logmsg("choice: chapter %d", b); }
            } else if (ends_with(cmd, ".OnSelectHero") && sscanf(args, "%d", &a) == 1 && GetTickCount() - g_autoSelTick > 400) {
                if (g_random) { g_random = 0; save_choice(); logmsg("choice: random off"); }
                g_curHero = a; if (a != g_hero) { g_hero = a; save_choice(); logmsg("choice: hero %d", a); }
            }
        }
        if (strstr(cmd, "map_preferences_mc.") && g_mpSelf && g_mpMode >= 0) {          // our dialog - the engine must not see it
            static unsigned char tmp[MAXMAPS]; GFxValue v; memset(&v, 0, sizeof v);
            if (ends_with(cmd, ".OnClickMap") && args) {
                int k = parse_states(args, tmp), on = 0; for (int i = 0; i < k; i++) on += !tmp[i];
                v.type = GFX_BOOL; v.v.b = on > 0; g_invOrig(g_mpSelf, NULL, "Receive_SetSaveButtonEnabled", g_mpFlags, &v, 1);
            } else if (ends_with(cmd, ".OnClickSave") && args) {
                int k = parse_states(args, tmp);
                { int bans = 0; for (int i = 0; i < k; i++) if (tmp[i] && ++bans > MAX_BANS) tmp[i] = 0;      // ladder rule: a few bans per mode
                  if (bans > MAX_BANS) { g_banNote = GetTickCount(); logmsg("map preferences: %d maps unchecked, only %d kept", bans, MAX_BANS); } } for (int i = 0; i < k; i++) g_off[g_mpMode][g_mpIdx[i]] = tmp[i];
                save_prefs(g_mpMode); int on = 0; for (int i = 0; i < k; i++) on += !tmp[i];
                logmsg("map preferences saved for %s: %d of %d maps", MODE_KEY[g_mpMode], on, k);
                char z[256] = ""; for (int i = 0; i < g_relicN && i < 100; i++) strcat(z, i ? ",0" : "0");     // the engine closes the dialog
                g_fsOrig(self, edx, movie, cmd, z);
            } else if (ends_with(cmd, ".OnClickCancel")) {
                g_fsOrig(self, edx, movie, cmd, args);                                   // the engine closes the dialog (and resets its own list)
                GFxValue st[MAXMAPS]; memset(st, 0, sizeof st);
                for (int i = 0; i < g_mpN; i++) { st[i].type = GFX_NUMBER; st[i].v.num = g_off[g_mpMode][g_mpIdx[i]] ? 2 : 0; }
                g_invOrig(g_mpSelf, NULL, "Receive_UpdateMapList", g_mpFlags, st, g_mpN);
            } else { g_fsOrig(self, edx, movie, cmd, args); }
            return;
        }
        if (g_screen == SC_MM && ends_with(cmd, ".OnClickMapPreferences") && g_mpSelf && g_mpMode != g_mode && g_nmaps) {
            g_fsOrig(self, edx, movie, cmd, args);                                      // mode changed - refill the list
            int n = build_maplist(); g_invOrig(g_mpSelf, NULL, "Receive_CreateMapList", g_mpFlags, g_mpArgs, n);
            logmsg("map preferences refilled for %s", MODE_KEY[g_mode]); return;
        }
        if (g_screen == SC_MM && !g_sending && args && !strncmp(cmd, g_panel, strlen(g_panel)) && !strcmp(cmd + strlen(g_panel), ".OnSelectDropDownItem")) {
            int a = -1, b = -1;
            if (sscanf(args, "%d;%d", &a, &b) == 2 && a == 0 && b >= 0 && b < 4 && b != g_mode) {
                g_mode = b; wchar_t w[8]; _snwprintf(w, 8, L"%d", b); WritePrivateProfileStringW(L"Choice", L"Mode", w, g_ini);
                logmsg("mode: %s", MODE_KEY[b]);
            }
        }
        // our button: "Start Searching" / "Cancel Searching" on the Find a Game screen
        if (g_screen == SC_MM && ends_with(cmd, ".OnClickStart")) {
            if (g_state == S_IDLE || g_state == S_DONE || g_state == S_FAIL) {
                if (g_curRace >= 0 && !g_random) { g_race = g_curRace; g_chapter = g_curChapter; if (g_curHero >= 0) g_hero = g_curHero; save_choice();
                    logmsg("search with race %d chapter %d hero %d (as shown on Find a Game)", g_race, g_chapter, g_hero); }
                g_searchTick = GetTickCount(); g_queue = -1; g_lobbyRace = -1; g_role = 0; g_gameName[0] = 0; g_hostMap[0] = 0; g_oppName[0] = 0; g_hostReported = 0; g_wantHost = 0;
                set_state(S_SEARCH); g_wantJoin = 1; SetEvent(g_wake);
            } else if (g_state == S_SEARCH) { set_state(S_IDLE); g_wantLeave = 1; SetEvent(g_wake); }
            return;                                                   // never start Relic's own search
        }
        if (g_screen == SC_MM && ends_with(cmd, ".OnClickBack") && g_state == S_SEARCH) { set_state(S_IDLE); g_wantLeave = 1; SetEvent(g_wake); }
    }
    g_fsOrig(self, edx, movie, cmd, args);
}

// ---------------- main-thread state machine ----------------
static void push_status(void) {
    if (!g_mmSelf || g_screen != SC_MM) return;
    GFxValue v[3]; memset(v, 0, sizeof v); status_text();
    v[0].type = v[1].type = v[2].type = GFX_WSTRING; v[0].v.ws = g_st1; v[1].v.ws = g_st2; v[2].v.ws = g_st3;
    g_invOrig(g_mmSelf, NULL, "Receive_UpdateSearchStatus", g_mmFlags, v, 3);
}
static void stage_line(void) {                 // what the cover says right now
    const wchar_t *opp = g_oppName[0] && wcscmp(g_oppName, L"Player") ? g_oppName : L"opponent";
    switch (g_state) {
    case S_GO_MAIN: case S_GO_LIST: case S_HOST_DIALOG: case S_HOST_NAME:
        if (g_role == 1) _snwprintf(g_coverLine, 159, L"Opponent: %ls. Creating the game", opp);
        else _snwprintf(g_coverLine, 159, L"Opponent: %ls. Looking for the host's game", opp); break;
    case S_GUEST_NAME: _snwprintf(g_coverLine, 159, L"Opponent: %ls. Waiting for the host to create the game", opp); break;
    case S_GUEST_FIND: case S_GUEST_JOINED: _snwprintf(g_coverLine, 159, L"Opponent: %ls. Joining the game", opp); break;
    case S_SETUP_HERO: wcscpy(g_coverLine, L"In the lobby. Setting up your army"); break;
    case S_HOST_WAIT: wcscpy(g_coverLine, L"Game created. Players are taking their slots"); break;
    case S_PLACE: wcscpy(g_coverLine, g_role == 3 ? L"Taking an observer slot" : L"Taking your slot"); break;
    default: break;
    }
}
// ---------------- lobby placement (0.29: by team) ----------------
// The ladder still sends a slot per player; only its half counts: slots 0..n-1 are one team, 4..4+n-1 the other (start positions are
// random, so the place inside the team does not matter). Every client works out the same plan from the same lobby:
//  - wait until every player of the match sits in a player slot;
//  - the top half belongs to the team that already has more players there (a tie: the team of slot 1) - the teams are equal, the
//    side doesn't matter, and this way 2v2 and 3v3 need one swap at most;
//  - players in the wrong half, top ones by slot and bottom ones by slot, make pairs; the top one of a pair asks the bottom one to
//    swap ("Move to this slot" on a taken slot asks its player), the bottom one's MMFind accepts it (auto-accept in a ladder lobby).
static int team_of(int slot) { return slot < 4 ? 0 : 1; }
static int my_team(void) { return g_mySlot < 0 ? -1 : team_of(g_mySlot); }
static int team_plan(const unsigned long long *ids, int pr[][2]) {     // pairs to swap (top slot, bottom slot); -1: not everyone in yet
    int n = team_n(), tm[8], seen = 0;
    for (int i = 0; i < 8; i++) {
        tm[i] = -1; if (!ids[i]) continue;
        int r = in_roster(ids[i]), t = r >= 0 ? team_of(g_rSlot[r]) : is_me(ids[i]) ? my_team() : -1;
        if (t < 0) continue;                                              // a watcher or a stranger
        if (is_obs_slot(i)) return -1;                                    // a player still in an observer slot
        tm[i] = t; seen++;
    }
    if (seen < g_nR) return -1;
    int a = 0; for (int i = 0; i < n; i++) if (tm[i] == 0) a++;
    int top = 2 * a > n ? 0 : 2 * a < n ? 1 : (tm[0] >= 0 ? tm[0] : 0);
    int bt[4], bb[4], nt = 0, nb = 0;
    for (int i = 0; i < n; i++) if (tm[i] >= 0 && tm[i] != top) bt[nt++] = i;
    for (int i = 4; i < 4 + n; i++) if (tm[i] >= 0 && tm[i] == top) bb[nb++] = i;
    int k = nt < nb ? nt : nb;
    for (int i = 0; i < k; i++) { pr[i][0] = bt[i]; pr[i][1] = bb[i]; }
    return k;
}
// every 0.5 s until the match loads; returns 1 when the teams are right (my ready mark can go on)
static int place_step(DWORD now) {
    static DWORD lastPoll, lastMove, lastAsk; static int inPlace, asks;
    if (now - lastPoll < 500) return inPlace;
    lastPoll = now;
    unsigned long long ids[8]; if (lobby_ids(ids) < 0) return inPlace;
    int cur = -1; for (int i = 0; i < 8; i++) if (is_me(ids[i])) cur = i;
    if (my_team() < 0 || cur < 0) return inPlace = 0;
    if (is_obs_slot(cur)) {                                               // came in as an observer: any free player slot (the plan sorts the teams)
        if (now - lastMove < 1200 + (unsigned)(g_mySteam % 7) * 100) return inPlace = 0;   // players don't move in lockstep
        for (int i = 0; i < 8; i++) if (!is_obs_slot(i) && !ids[i]) { lastMove = now; logmsg("place: observer slot %d -> free player slot %d", cur, i); slot_do(i, CB_MOVE); break; }
        return inPlace = 0;
    }
    int pr[4][2], k = team_plan(ids, pr);
    if (k < 0) return inPlace = 0;                                        // wait for everyone
    if (k == 0) { asks = 0; return inPlace = 1; }
    for (int i = 0; i < k; i++) {
        if (pr[i][1] == cur) return inPlace = 0;                          // the top one of my pair asks me
        if (pr[i][0] != cur) continue;
        if (now - lastAsk < 4000) return inPlace = 0;                    // the swap needs a moment to show
        lastAsk = now; asks++;
        logmsg("place: wrong team half, asking slot %d to swap with my slot %d (pair %d of %d, ask %d)", pr[i][1], cur, i + 1, k, asks);
        slot_do(pr[i][1], CB_MOVE);
        return inPlace = 0;
    }
    return inPlace = 0;                                                   // my team is right, the others are still swapping
}
// host: the ladder's game options, observer slots open, strangers out - until the match loads
static void host_step(DWORD now) {
    static DWORD lastOpt, lastObs, lastKick;
    if (now - lastOpt > 1000) { lastOpt = now;
        for (int id = 1; id <= 4; id++) if (g_optWant[id] >= 0 && g_optCur[id] >= 0 && g_optCur[id] != g_optWant[id]) {
            char a[16]; snprintf(a, sizeof a, "%d;%d", id, g_optWant[id]);
            logmsg("options: %ls (drop-down %d: %d -> %d)", OPT_WANT[id][0], id, g_optCur[id], g_optWant[id]);
            g_optCur[id] = -1; fs(".game_options_panel_mc.OnSelectDropDownItem", a); break; }      // one at a time, the lobby refreshes after each
    }
    unsigned long long ids[8]; if (lobby_ids(ids) < 0) return;
    int stranger = -1;
    for (int i = 0; i < 8; i++)
        if (ids[i] && !is_me(ids[i]) && in_roster(ids[i]) < 0 && !is_watcher(ids[i]) && !name_known(g_slotName[i])) stranger = i;
    if (stranger >= 0 && now - lastKick > 1500) { lastKick = now; logmsg("kick stranger %llu in slot %d", ids[stranger], stranger); slot_do(stranger, CB_KICK); }
    if (now - lastObs > 10000) { lastObs = now; slot_do(team_n(), CB_OPEN_OBS); }           // map changes close them again
}

static void msgbox_hide(int hide) {            // message boxes ("Connecting...") live in _level0.container of the message box movie
    static int cur = -1; static void *wrap; if (wrap != g_msgWrap) { wrap = g_msgWrap; cur = -1; }
    if (hide == cur || !g_msgWrap || (cur == -1 && !hide)) return;
    void *m = *(void **)((char *)g_msgWrap + 4); if (!m || IsBadReadPtr(m, 8)) return;
    as_setb(m, "_level0.container._visible", !hide); cur = hide; logmsg("message boxes %s", hide ? "hidden" : "shown");
}
static void tick(void) {
    static DWORD last, lastStatus; DWORD now = GetTickCount(); if (now - last < 250) return; last = now;
    g_autoSwap = g_autoSwapCfg && !(g_state == S_IDLE || g_state == S_SEARCH || g_state == S_FAIL);   // auto-accept slot swaps only inside a ladder/automatch lobby (not in a plain public game)
    if (g_coverWant && g_state != S_DONE) stage_line();
    { static DWORD hb; if (now - hb > 30000) { hb = now; logmsg("tick alive, state %s screen %d", SN[g_state], g_screen); } }
    (void)lastStatus;
    if (g_loadTick && now - g_loadTick > 180000) { g_loadTick = 0; marker_set(NULL); }      // past loading: a crash later is not a ruined loading
    { static LONG seen; if (g_swapCount != seen) { seen = g_swapCount; logmsg("swap request accepted (%ld so far)", seen); } }
    if (g_state == S_DONE && !g_loading && g_screen == SC_SETUP && in_state() < 180000) {
        static DWORD lastClick; if (in_state() < 300) lastClick = now;
        if (g_role == 1) {                                            // host: start as soon as everyone is ready, and keep pressing every 5 s
            static int wasReady; if (!g_allReady) wasReady = 0;
            // press only an enabled Start (any language: not the text it had while I was alone - "Start Skirmish");
            // a press starts a countdown ("Start Now!" / disabled) - pressing again then cancels it, so later presses need the same text
            if (in_state() < 300) g_startTxt[0] = 0;
            int canPress = g_btnEnabled && g_btnText[0] && (g_humans >= 2 || g_bots) && (g_bots || wcscmp(g_btnText, g_aloneTxt))
                           && (!g_startTxt[0] || !wcscmp(g_btnText, g_startTxt));
            int go = g_allReady && canPress && ((!wasReady && now - lastClick > 700) || now - lastClick > 15000);
            if (go) wasReady = 1;
            if (go) { lastClick = now; if (!g_startTxt[0]) wcscpy(g_startTxt, g_btnText); logmsg("start: pressing Start (all ready %d, button '%ls' enabled %d)", g_allReady, g_btnText, g_btnEnabled); fs(".OnClickReady", ""); }
        } else if (g_role == 2 && !g_readyMe && now - lastClick > 3000 && !g_manual && place_step(now)) {   // guest in his slot: the ready mark gets reset when someone moves - set it again
            lastClick = now; logmsg("ready was reset - setting ready again"); fs(".OnClickReady", "");
        }
        if (!g_manual) { if (g_role == 2) place_step(now); if (g_role == 1) { host_step(now); place_step(now); } }
    }
    if (!g_loading && g_coverWant && (g_state == S_HOST_WAIT || g_state == S_PLACE || g_state == S_DONE)) {
        static DWORD alone; static int maxH; int left = g_screen != SC_SETUP && in_state() > 3000;     // kicked / host gone: back in the list or menu
        if (g_state == S_PLACE || (g_state == S_HOST_WAIT && in_state() < 300)) maxH = 0;
        int inLobby = g_humans; { unsigned long long ids[8]; int n = lobby_ids(ids); if (n > inLobby) inLobby = n; }   // observer slots count too
        if (inLobby > maxH) maxH = inLobby;
        // everybody who came in has left again (a crash, a quit): nobody to play with
        if (g_screen == SC_SETUP && inLobby < 2 && maxH >= 2 && !g_bots) { if (!alone) alone = now; } else alone = 0;
        if (left || (alone && now - alone > 6000)) {
            logmsg("match fell apart (%s) - cover off", left ? "not in the lobby any more" : "the other players left");
            alone = 0; set_state(S_FAIL);
        }
    }
    if (g_state == S_DONE && g_coverWant && !g_loading && g_screen == SC_SETUP && in_state() > 60000 && now - g_lastFsTick < 3000) {
        logmsg("the game did not start - cover off"); g_coverWant = 0; set_state(S_FAIL);      // the menu movie is alive (it still sends commands)
    }
    switch (g_state) {
    case S_GO_MAIN:                                                   // leave Find a Game -> main menu
        { static DWORD since, lastBack, entered; if (entered != g_stateTick) { entered = g_stateTick; since = now; lastBack = now - 800; }
          if (g_screen == SC_MM && now - lastBack > 1500) { lastBack = now; fs(".OnClickBack", ""); }   // pressed again only if the screen did not change
          if (g_screen == SC_MAIN) set_state(g_role == 1 ? S_GO_LIST : S_GUEST_NAME);
          else if (now - since > 20000) { logmsg("could not leave Find a Game"); set_state(S_FAIL); } }
        break;
    case S_GUEST_NAME:                                                // guest waits in the main menu for the host's game name (net_worker)
        if (in_state() > 90000) set_state(S_FAIL);
        break;
    case S_GO_LIST:                                                   // main menu -> Public Games list
        if (g_screen == SC_MAIN && in_state() > 800) { fs(".OnClickPublicMatch", ""); g_stateTick = now; }
        if (g_screen == SC_LIST && (g_listSeen || g_role == 1)) set_state(g_role == 1 ? S_HOST_DIALOG : S_GUEST_FIND);
        if (in_state() > 20000 && g_screen != SC_LIST) set_state(S_FAIL);
        break;
    case S_HOST_DIALOG:
        if (in_state() > 200) { g_hostDirect = 1; g_hostDone = 0; fs(".OnClickHost", ""); set_state(S_HOST_NAME); }
        break;
    case S_HOST_NAME:                                                 // the "Game Name" box: type the name and press Enter
        { static int typed, entered; if (in_state() < 1000) typed = entered = 0;
          if (!g_hostDone && !typed && in_state() > 600) { typed = 1; type_text(g_gameName); }      // fallback: the box came up after all
          if (!g_hostDone && !entered && in_state() > 1000) { entered = 1; press_enter(); } }
        if (g_screen == SC_SETUP) set_state(S_SETUP_HERO);
        if (in_state() > 15000) set_state(S_FAIL);
        break;
    case S_GUEST_FIND:
        if (g_rowFound >= 0 && in_state() > 200) {
            char r[8]; snprintf(r, sizeof r, "%d", g_rowFound);
            fs(".OnEntryClick", r); fs(".OnClickJoin", ""); set_state(S_GUEST_JOINED);
        }
        if (in_state() > 60000) set_state(S_FAIL);
        break;
    case S_GUEST_JOINED: {
        static int tries; static DWORD lastTry; if (in_state() < 300) { tries = 0; lastTry = now; }
        if (g_screen == SC_SETUP) { set_state(S_SETUP_HERO); break; }
        if (g_screen == SC_LIST && now - lastTry > 8000 && tries < 3 && g_rowFound >= 0) {     // join did not go through - try again
            tries++; lastTry = now; char r[8]; snprintf(r, sizeof r, "%d", g_rowFound);
            logmsg("join retry %d", tries); fs(".OnEntryClick", r); fs(".OnClickJoin", "");
        }
        if (in_state() > 35000) set_state(S_FAIL);
        break; }
    case S_SETUP_HERO: {                                              // hero selection pops up in the lobby: accept the default once
        static int step; if (in_state() < 300) step = 0;
        static DWORD heroT;
        if (step == 0 && in_state() > 1500 && g_random) {             // Random on Find a Game = the same Random in the lobby
            step = 2; heroT = in_state(); logmsg("lobby: random race and hero"); fs(".hero_selection_mc.OnClickRandom", "");
        }
        if (step == 0 && in_state() > 1500) {                         // a fresh join needs a moment; the engine brings the race from Find a Game
            step = 1; if (g_lobbyRace >= 0) g_curRace = g_lobbyRace; int shown = g_curRace;
            int rc = apply_race(); heroT = in_state() + (rc ? 1000 : 300);
            if (!rc && g_curHero == g_hero) heroT = 0;                   // already as saved: don't touch the panel at all
            logmsg("lobby: race %d (saved %d), hero %d (saved %d)%s", shown, g_race, g_curHero, g_hero, rc ? " - switching race" : "");
        }
        if (step == 1 && in_state() > heroT) {
            step = 2;
            if (heroT) apply_hero(); else heroT = in_state();
        }
        if (step == 2 && in_state() > heroT + 600) { step = 3; fs(".hero_selection_mc.OnClickAccept", ""); }
        if (step == 3 && in_state() > heroT + 1200) {
            step = 4;
            if (g_role == 1) {                                        // host: 1v1 map right away (nobody else in yet - no swap question)
                int mi = g_mapName[0] ? map_index(g_mapName) : -1;
                if (mi < 0) {                                         // server gave no map - random one of mine for the mode
                    int c[MAXMAPS], k = 0; for (int i = 0; i < g_nmaps; i++) if (mode_map(i, g_mode) && !g_off[g_mode][i]) c[k++] = i;
                    if (!k) for (int i = 0; i < g_nmaps; i++) if (mode_map(i, g_mode)) c[k++] = i;
                    if (k) mi = c[GetTickCount() % k];
                }
                if (mi >= 0) { char a[16]; snprintf(a, sizeof a, "0;%d", mi); logmsg("map: %ls", g_maps[mi]); fs(".game_options_panel_mc.OnSelectDropDownItem", a);
                               WideCharToMultiByte(CP_UTF8, 0, g_maps[mi], -1, g_hostMap, sizeof g_hostMap, NULL, NULL); }
                else logmsg("no map for mode %s, keeping the current one", MODE_KEY[g_mode]);
                g_wantHost = 1; SetEvent(g_wake); cover(L"Waiting for the players to connect"); set_state(S_HOST_WAIT);
            } else { set_state(S_PLACE); }                            // guest / watcher: take the assigned slot, then ready
        }
        break; }
    case S_HOST_WAIT: {                                               // open observer slots, kick strangers, start when everyone is in place
        static int obsOpened, aiMask; static DWORD lastKick, allIn, lastLog, lastAi, wSince; if (in_state() < 300) { obsOpened = 0; allIn = 0; aiMask = 0; wSince = 0; }
        if (g_manual) { if (in_state() > 300000) set_state(S_FAIL); break; }        // test: the person runs the lobby
        if (in_state() > 2000 && !g_bots) host_step(now);
        if (!obsOpened && in_state() > 1500) { obsOpened = 1; slot_do(team_n(), CB_OPEN_OBS); }   // after the map change (it closes them again)
        unsigned long long ids[8]; if (lobby_ids(ids) < 0) break;
        int placed = 0, wIn = 0, stranger = -1, pr[4][2], plan = team_plan(ids, pr);
        if (!g_bots && in_state() > 2000) place_step(now);                // the host swaps too when its half is the wrong one
        for (int i = 0; i < 8; i++) if (ids[i]) {
            if (is_me(ids[i])) continue;
            int r = in_roster(ids[i]);
            if (r >= 0) { if (plan == 0 || (g_bots && g_rSlot[r] == i)) placed++; }
            else if (is_watcher(ids[i])) { if (is_obs_slot(i)) wIn++; }
            else if (!name_known(g_slotName[i])) stranger = i;
        }
        int aivai = g_bots && g_testBots == 2, mine = -1; for (int i = 0; i < 8; i++) if (ids[i] && is_me(ids[i])) mine = i;
        if (aivai && obsOpened && mine >= 0 && !is_obs_slot(mine) && in_state() > 2500 && now - lastAi > 2000) {   // AI test: I go watch
            for (int i = 0; i < 8; i++) if (is_obs_slot(i) && !ids[i]) { lastAi = now; logmsg("AI test: me %d -> observer slot %d", mine, i); slot_do(i, CB_MOVE_OBS); break; }
        }
        if (g_bots && obsOpened && in_state() > 3000 && now - lastAi > 800 && (!aivai || (mine >= 0 && is_obs_slot(mine)))) {   // test vs CPU: computers in the empty player slots
            int d = aivai ? (g_aiDiff < 0 ? 0 : g_aiDiff > 3 ? 3 : g_aiDiff) : 0;
            for (int i = 0; i < 8; i++) if (!is_obs_slot(i) && !ids[i] && !(aiMask & (1 << i)) && g_slotName[i][0] == 0) {
                aiMask |= 1 << i; lastAi = now; logmsg("add computer (difficulty %d) to slot %d", d, i); slot_do(i, CB_AI[d]); break; }
        }
        if (stranger >= 0 && now - lastKick > 1500) { lastKick = now; logmsg("kick stranger %llu in slot %d", ids[stranger], stranger); slot_do(stranger, CB_KICK); }
        int need = g_nR - 1;                                          // everyone but me in his slot
        if (now - lastLog > 3000) { lastLog = now; logmsg("lobby: placed %d/%d (swaps left %d), watchers %d/%d, stranger slot %d", placed, need, plan, wIn, g_nW, stranger); }
        int nPlayerSlots = 2 * team_n(), aiDone = 0; for (int i = 0; i < 8; i++) if (!is_obs_slot(i) && (aiMask & (1 << i))) aiDone++;
        int ready = placed >= need && stranger < 0 && (need > 0 || (g_bots && aiDone >= nPlayerSlots - (aivai ? 0 : 1) && now - lastAi > 1500));
        if (wIn >= g_nW && g_nW) { if (!wSince) wSince = now; } else wSince = 0;
        if (!ready) allIn = 0; else if (!allIn) allIn = now;
        if (ready && (wIn >= g_nW ? now - allIn > 2500 && (!g_nW || now - wSince > 2500) : now - allIn > 25000)) {
            cover(L"Everyone is in. Waiting for everyone to be ready"); g_wantStarted = 1; SetEvent(g_wake); set_state(S_DONE);
        }
        if (in_state() > 120000) { logmsg("players did not come - cover off"); set_state(S_FAIL); }
        break; }
    case S_PLACE: {                                                   // guest: take the assigned slot (place_step); watcher: a free observer slot
        if (g_manual && g_role != 3) { cover(L"Manual test: take your slot yourself"); set_state(S_DONE); break; }
        if (g_role != 3) {
            if (in_state() > 1200 && place_step(now)) { fs(".OnClickReady", ""); cover(L"In your slot. Waiting for the host to start"); set_state(S_DONE); }
            if (in_state() > 90000) set_state(S_FAIL);
            break;
        }
        static DWORD lastMove; if (in_state() < 300) lastMove = 0;
        unsigned long long ids[8]; if (lobby_ids(ids) < 0) break;
        int cur = -1; for (int i = 0; i < 8; i++) if (is_me(ids[i])) cur = i;
        int target = -1; if (cur >= 0 && is_obs_slot(cur)) target = cur; else for (int i = 0; i < 8; i++) if (is_obs_slot(i) && !ids[i]) { target = i; break; }
        if (cur >= 0 && cur == target) { cover(L"You are an observer. The game starts soon"); set_state(S_DONE); break; }
        if (target >= 0 && in_state() > 1200 && now - lastMove > 2000) { lastMove = now; logmsg("observer: %d -> %d", cur, target); slot_do(target, CB_MOVE_OBS); }
        if (in_state() > 60000) set_state(S_FAIL);
        break; }
    }
}
static void __stdcall upd_hook(float dt) {
    g_updOrig(dt);
    DWORD now = GetTickCount(); int alive = g_fsMovie && g_state != S_DONE;            // after Start the frontend goes away - hands off
    if (g_applyMM && alive) { static DWORD t; if (!t) t = now; if (now - t > 400) { g_applyMM = 0; t = 0;
        if (g_random) { g_autoSelTick = now; fs(".hero_selection_mc.OnClickRandom", ""); g_heroAt = 0; } else g_heroAt = now + (apply_race() ? 800 : 1);
        char a[8]; snprintf(a, sizeof a, "0;%d", g_mode); fs(".OnSelectDropDownItem", a); } }
    if (g_heroAt && now >= g_heroAt) { g_heroAt = 0; if (alive && g_screen == SC_MM) apply_hero(); }
    tick();
    { static DWORD t; if (now - t > 1000 && (g_state == S_IDLE || g_state == S_FAIL)) { t = now;        // debug: MMFind_cover.txt shows the cover in the menu
        wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsMMFind_cover.txt", g_dir);
        FILE *f = _wfopen(p, L"r"); if (f) { char b[150] = ""; fgets(b, sizeof b, f); fclose(f); b[strcspn(b, "\r\n")] = 0;
            MultiByteToWideChar(CP_UTF8, 0, b, -1, g_coverLine, 159); g_coverWant = 1; } else g_coverWant = 0; } }
    { static DWORD t2; if (now - t2 > 1000) { t2 = now; wchar_t p2[MAX_PATH]; _snwprintf(p2, MAX_PATH, L"%lsMMFind_nocover.txt", g_dir);
        if (GetFileAttributesW(p2) != INVALID_FILE_ATTRIBUTES && g_coverWant) { g_coverWant = 0; logmsg("debug: cover off"); } } }
    if (g_fsMovie && !g_loading && (alive || !g_coverWant) && (g_coverWant || g_coverOn)) cover_update();      // S_FAIL also passes here: removes the cover
    if (!g_loading) msgbox_hide(g_coverWant && g_state >= S_GO_MAIN && g_state <= S_GUEST_JOINED);
}

// ---------------- install ----------------
static int iat_hook(DWORD slot, const char *dll, void *fn, void **orig) {
    HMODULE m = GetModuleHandleA(dll); MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)slot, 4)) return 0;
    // one import-table write at a time across all our DLLs (LobbyStats hooks this same Shark slot from its own thread; two
    // threads opening and closing the page at once crashed the game at start): the shared mutex, the slot read again under
    // it (the other hook may be in by now - we chain onto it), the write through WriteProcessMemory (fails, never crashes)
    char n[64]; wsprintfA(n, "Local\\dow2patch_iat_%lu", GetCurrentProcessId());
    HANDLE mx = CreateMutexA(NULL, FALSE, n); if (mx) WaitForSingleObject(mx, 5000);
    int ok = 0; DWORD cur = *(volatile DWORD *)slot, old;
    if (VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) {
        SIZE_T w = 0;
        if (WriteProcessMemory(GetCurrentProcess(), (void *)slot, &fn, 4, &w) && w == 4) { *orig = (void *)cur; ok = 1; }
        VirtualProtect((void *)slot, 4, old, &old);
    }
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return ok;
}
static DWORD WINAPI installer(LPVOID _) {
    for (int i = 0; i < 180; i++) {
        if (GetModuleHandleA("Shark.dll") && iat_hook(IAT_SHARK_UPDATE, "Shark.dll", (void *)upd_hook, (void **)&g_updOrig)) {
            logmsg("frame hook installed"); return 0; }
        Sleep(1000);
    }
    logmsg("frame hook NOT installed"); return 0;
}
static void install(void) {
    unsigned char *t = (unsigned char *)HOOK_ADDR;
    int chained = !IsBadReadPtr(t, 5) && t[0] == 0xE9;                // another add-on (LobbyStats) already hooked Invoke: wrap it
    if (IsBadReadPtr(t, 5) || (memcmp(t, PROLOGUE, 5) && !chained)) { logmsg("Invoke prologue not recognised - not starting"); return; }
    unsigned char *f = (unsigned char *)FN_FSCMD;
    if (IsBadReadPtr(f, 7) || memcmp(f, FSCMD_PRO, 7)) { logmsg("fscommand prologue mismatch - not starting"); return; }
    unsigned char *tr = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (chained) { DWORD dest = (DWORD)(t + 5) + *(DWORD *)(t + 1); tr[0] = 0xE9; *(DWORD *)(tr + 1) = dest - (DWORD)(tr + 5); logmsg("Invoke already hooked by another add-on (-> %08lX), chaining", dest); }
    else { memcpy(tr, PROLOGUE, 5); tr[5] = 0xE9; *(DWORD *)(tr + 6) = HOOK_RESUME - (DWORD)(tr + 10); }
    g_invOrig = (InvokeFn)tr;
    memcpy(tr + 16, FSCMD_PRO, 7); tr[23] = 0xE9; *(DWORD *)(tr + 24) = (FN_FSCMD + 7) - (DWORD)(tr + 28); g_fsOrig = (FsCmdFn)(tr + 16);
    DWORD o;
    VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &o); t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)inv_hook - (DWORD)(t + 5); VirtualProtect(t, 5, o, &o);
    VirtualProtect(f, 7, PAGE_EXECUTE_READWRITE, &o); f[0] = 0xE9; *(DWORD *)(f + 1) = (DWORD)fs_hook - (DWORD)(f + 5); f[5] = f[6] = 0x90; VirtualProtect(f, 7, o, &o);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    logmsg("hooked Invoke + fscommand");
    install_host_hook(); install_sfm_hook(); install_slot_hook(); install_swap_hook(); marker_check();
    CloseHandle(CreateThread(NULL, 0, installer, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, net_worker, NULL, 0, NULL));
}
static unsigned long long my_steam(void) {                         // Steam writes the active account id here
    DWORD v = 0, sz = sizeof v; HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"ActiveUser", NULL, NULL, (BYTE *)&v, &sz); RegCloseKey(k); }
    return v ? 76561197960265728ULL + v : 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID _) {
    if (r != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h); InitializeCriticalSection(&g_lk); g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t *s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0;
    wchar_t ini[MAX_PATH]; _snwprintf(ini, MAX_PATH, L"%lsMMFind.ini", g_dir);
    GetPrivateProfileStringW(L"MMFind", L"Server", MM_HOST, g_server, 128, ini); if (!wcscmp(g_server, L"rts.fintebtc.de")) wcscpy(g_server, MM_HOST);   // old single-host ini -> all mirrors
    parse_hosts();
    g_mode = GetPrivateProfileIntW(L"Choice", L"Mode", 0, ini); if (g_mode < 0 || g_mode > 3) g_mode = 0;
    g_maxDiff = GetPrivateProfileIntW(L"MMFind", L"MaxRatingDiff", 0, ini);
    g_testBots = GetPrivateProfileIntW(L"MMFind", L"TestBots", 0, ini);
    g_aiDiff = GetPrivateProfileIntW(L"MMFind", L"AIDifficulty", 1, ini);    // 0 Easy, 1 Normal, 2 Hard, 3 Expert
    g_readyIdx = GetPrivateProfileIntW(L"MMFind", L"ReadyField", 12, ini);     // PopulatePlayerList slot field 12 = "ready" (seen 26.10)
    g_verbose = GetPrivateProfileIntW(L"MMFind", L"Verbose", 0, ini);
    g_logInvoke = GetPrivateProfileIntW(L"MMFind", L"LogInvoke", 0, ini);   // debug: every engine -> Flash call
    g_manual = GetPrivateProfileIntW(L"MMFind", L"Manual", 0, ini);          // test: menus are walked, the lobby is left to the person
    g_autoSwapCfg = GetPrivateProfileIntW(L"MMFind", L"AutoSwap", 1, ini);   // master: auto-accept "swap slots" (only in a ladder lobby); 0 = always ask
    { wchar_t g[32]; GetPrivateProfileStringW(L"MMFind", L"TestGroup", L"", g, 32, ini); WideCharToMultiByte(CP_UTF8, 0, g, -1, g_group, sizeof g_group, NULL, NULL); }
    wcscpy(g_ini, ini);
    g_race = GetPrivateProfileIntW(L"Choice", L"Race", -1, ini); g_chapter = GetPrivateProfileIntW(L"Choice", L"Chapter", -1, ini); g_hero = GetPrivateProfileIntW(L"Choice", L"Hero", -1, ini); g_random = GetPrivateProfileIntW(L"Choice", L"Random", 0, ini);
    g_mySteam = my_steam();
    { wchar_t p[MAX_PATH], o[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsMMFind.log", g_dir); _snwprintf(o, MAX_PATH, L"%lsMMFind.log.old", g_dir);
      WIN32_FILE_ATTRIBUTE_DATA a; if (GetFileAttributesExW(p, GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > (1u << 20))) MoveFileExW(p, o, MOVEFILE_REPLACE_EXISTING); }
    logmsg("MMFind 0.30 start, steam=%llu server=%ls", g_mySteam, g_server);
    { char n[64]; wsprintfA(n, "Local\\dow2patch_MMFind_%lu", GetCurrentProcessId());          // one copy per game (launcher + dow2-patches)
      HANDLE mx = CreateMutexA(NULL, FALSE, n); if (mx && GetLastError() == ERROR_ALREADY_EXISTS) { logmsg("another MMFind copy is active, this one stays idle"); return TRUE; } }
    install(); return TRUE;
}
