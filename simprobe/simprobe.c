// SimProbe.dll - which game functions DOW2.exe calls, and how often (diagnostics, nothing else).
//
// Why: MatchTimer and FrameProbe count simulation ticks on World::SimulatePre (IAT 0x00F88154), but DOW2.exe calls
// that import only once per match. SimProbe looks for a call that really comes every tick, and for calls that mark
// the start and the end of a match.
// How: every import DOW2.exe takes from the game's own DLLs (the ones in the game folder: SimEngine, Shark, ...;
// Windows DLLs are left alone) gets a thunk "lock inc [counter]; jmp [original]". The thunk doesn't touch the stack
// or registers, so it works with any calling convention. Imports of data (not code) are skipped.
// Every ReportSec seconds SimProbe.log gets: functions called for the first time, functions that stopped being
// called or came back, and the most called ones with calls per second.
// Read-only: the game does exactly what it did before, so no desync.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define MAX_SLOTS 4096
#define TOP       20

static wchar_t g_log[MAX_PATH];
static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static int g_n;
static volatile LONG g_cnt[MAX_SLOTS];
static void *g_orig[MAX_SLOTS];
static LONG g_prev[MAX_SLOTS], g_lastD[MAX_SLOTS];
static char g_seen[MAX_SLOTS];
static char *g_name[MAX_SLOTS];                 // "SimEngine.dll World::SimulatePre"
static DWORD g_slot[MAX_SLOTS];

typedef DWORD (WINAPI *UnDecorate_t)(PCSTR, PSTR, DWORD, DWORD);
#define UNDNAME_NAME_ONLY 0x1000

static int is_code(void *p) {
    MEMORY_BASIC_INFORMATION mb;
    return VirtualQuery(p, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
           (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static int install(void) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) { logmsg("DOW2.exe has no import table?"); return 0; }

    char game[MAX_PATH]; GetModuleFileNameA(NULL, game, MAX_PATH);   // game folder = folder of the exe
    char *sl = strrchr(game, '\\'); if (sl) sl[1] = 0;
    size_t glen = strlen(game);

    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    UnDecorate_t undec = dbg ? (UnDecorate_t)GetProcAddress(dbg, "UnDecorateSymbolName") : NULL;

    BYTE *thunks = VirtualAlloc(NULL, MAX_SLOTS * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunks) { logmsg("VirtualAlloc for thunks failed"); return 0; }

    int skippedData = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        HMODULE m = GetModuleHandleA(dll); char path[MAX_PATH];
        if (!m || !GetModuleFileNameA(m, path, MAX_PATH) || _strnicmp(path, game, glen)) continue;   // not a game DLL
        DWORD *iat = (DWORD *)(base + d->FirstThunk);
        DWORD *ilt = d->OriginalFirstThunk ? (DWORD *)(base + d->OriginalFirstThunk) : NULL;
        int before = g_n;
        for (int i = 0; iat[i] && g_n < MAX_SLOTS; i++) {
            if (!is_code((void *)iat[i])) { skippedData++; continue; }
            char fn[512];
            if (ilt && !(ilt[i] & IMAGE_ORDINAL_FLAG32)) {
                const char *raw = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + ilt[i]))->Name;
                if (!undec || !undec(raw, fn, sizeof fn, UNDNAME_NAME_ONLY)) lstrcpynA(fn, raw, sizeof fn);
            } else if (ilt) wsprintfA(fn, "#%lu", ilt[i] & 0xFFFF);
            else wsprintfA(fn, "slot %08lX", (DWORD)&iat[i]);
            char full[600]; wsprintfA(full, "%s %s", dll, fn);
            int k = g_n;
            g_name[k] = _strdup(full); g_slot[k] = (DWORD)&iat[i]; g_orig[k] = (void *)iat[i];
            BYTE *t = thunks + k * 16;                               // lock inc dword [g_cnt+k]; jmp dword [g_orig+k]
            t[0] = 0xF0; t[1] = 0xFF; t[2] = 0x05; *(DWORD *)(t + 3) = (DWORD)&g_cnt[k];
            t[7] = 0xFF; t[8] = 0x25;              *(DWORD *)(t + 9) = (DWORD)&g_orig[k];
            DWORD old;
            if (!VirtualProtect(&iat[i], 4, PAGE_READWRITE, &old)) continue;
            iat[i] = (DWORD)t;
            VirtualProtect(&iat[i], 4, old, &old);
            g_n++;
        }
        logmsg("%s: %d functions hooked", dll, g_n - before);
    }
    FlushInstructionCache(GetCurrentProcess(), thunks, MAX_SLOTS * 16);
    logmsg("%d functions hooked in total (%d data imports skipped)", g_n, skippedData);
    return g_n > 0;
}

static void report(DWORD ms) {
    static int idx[MAX_SLOTS];
    int m = 0;
    logmsg("---- %.1f s", ms / 1000.0);
    for (int i = 0; i < g_n; i++) {
        LONG c = g_cnt[i], d = c - g_prev[i];
        g_prev[i] = c;
        if (d && !g_seen[i]) { g_seen[i] = 1; logmsg("  first call   %s  (%ld)", g_name[i], d); }
        else if (d && !g_lastD[i]) logmsg("  called again %s  (%ld)", g_name[i], d);
        else if (!d && g_lastD[i]) logmsg("  stopped      %s", g_name[i]);
        g_lastD[i] = d;
        if (d) idx[m++] = i;
    }
    for (int a = 0; a < m && a < TOP; a++) {                          // the TOP most called, by calls per second
        int best = a;
        for (int b = a + 1; b < m; b++) if (g_lastD[idx[b]] > g_lastD[idx[best]]) best = b;
        int t = idx[a]; idx[a] = idx[best]; idx[best] = t;
        logmsg("  %9.1f/s  %s", g_lastD[idx[a]] * 1000.0 / ms, g_name[idx[a]]);
    }
    if (m > TOP) logmsg("  ... and %d more called less often", m - TOP);
}

static DWORD WINAPI worker(LPVOID _) {
    Sleep(2000);                                                     // let the other patches put their hooks in first
    if (!install()) return 0;
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_log); wcscpy(wcsrchr(ini, L'\\') + 1, L"SimProbe.ini");
    DWORD period = GetPrivateProfileIntW(L"SimProbe", L"ReportSec", 5, ini) * 1000;
    if (period < 1000) period = 1000;
    DWORD last = GetTickCount();
    for (;;) {
        Sleep(period);
        DWORD now = GetTickCount();
        report(now - last);
        last = now;
    }
}

static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wcscat(g_log, L"SimProbe.log");
    logmsg("SimProbe v1");
    if (already_loaded("SimProbe")) { logmsg("another SimProbe copy is already active in this game, this one stays idle"); return TRUE; }
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
