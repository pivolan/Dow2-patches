// Cooldowns.dll - seconds left on recharging abilities, an add-on of its own with its own launcher tick (owner 30.10.2026:
// LobbyStats was carrying everything and games crashed). cooldowns.c + the match timer's match state and Direct3D hook (no clock).
#include <windows.h>
#include <stdio.h>
#include <d3d9.h>
extern void (*mt_extra_draw)(IDirect3DDevice9 *);
extern void (*mt_extra_lost)(IDirect3DDevice9 *);
extern void (*mt_extra_reset)(IDirect3DDevice9 *, HRESULT);
void mt_start(const wchar_t *dir, const wchar_t *ini);
void cd_start(const wchar_t *dir, const wchar_t *ini); void cd_draw(IDirect3DDevice9 *dev);
void cd_lost(IDirect3DDevice9 *dev); void cd_reset(IDirect3DDevice9 *dev, HRESULT hr);

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    static wchar_t dir[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameW(h, dir, MAX_PATH); wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0; else dir[0] = 0;
    _snwprintf(ini, MAX_PATH, L"%lsCooldowns.ini", dir);
    char n[64]; wsprintfA(n, "Local\\dow2patch_Cooldowns_%lu", GetCurrentProcessId());   // one copy per game
    HANDLE m = CreateMutexA(NULL, FALSE, n); if (m && GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
    mt_extra_draw = cd_draw; mt_extra_lost = cd_lost; mt_extra_reset = cd_reset;
    mt_start(dir, ini);
    cd_start(dir, ini);
    return TRUE;
}
