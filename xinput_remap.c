/*
 * xinput9_1_0.dll proxy - XInput slot remapper for Dragon Age: Inquisition
 *
 * Problem: the game only reads XInput "user index" 0 (Player 1). Windows
 * hands out XInput slots on a first-come basis, so if anything has held
 * slot 0 when your controller is plugged in (a previous controller, a
 * controller that re-enumerates while switching modes such as the GameSir
 * G7 SE, a virtual pad from Razer/AMD/Steam/DS4Windows, ...), your pad
 * lands in slot 1+ and the game sees it but never reads its input.
 *
 * Fix: this DLL sits in the game folder and is loaded instead of
 * System32\xinput9_1_0.dll. Every call is forwarded to the real DLL, but
 * the connected controllers are presented to the game as slots 0..n-1
 * (in physical slot order), so the first connected pad is always "Player 1".
 *
 * Optional xinput_remap.ini next to the DLL:
 *   [xinput_remap]
 *   Remap=1   ; 0 = pass slots through unchanged (diagnostics only)
 *   Log=1     ; 0 = don't write xinput_remap.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } XSTATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVIB;
typedef struct { BYTE Type, SubType; WORD Flags; PAD Gamepad; XVIB Vibration; } XCAPS;

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XSTATE *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, XVIB *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, XCAPS *);
typedef DWORD (WINAPI *PFN_GetDSGuids)(DWORD, GUID *, GUID *);

static HMODULE g_self, g_real;
static PFN_GetState rGetState;
static PFN_SetState rSetState;
static PFN_GetCaps rGetCaps;
static PFN_GetDSGuids rGetDSGuids;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_map = 0x03020100;   /* byte i = physical slot for virtual slot i */
static BOOL g_remap = TRUE, g_log = TRUE;
static volatile LONG g_calls[4];
static volatile LONG g_inputLogs;
static DWORD g_lastPacket[4];

static HANDLE g_logf = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logcs;

static void logline(const char *fmt, ...)
{
    char buf[1200];
    SYSTEMTIME st;
    va_list ap;
    int n;
    DWORD w;
    if (g_logf == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&st);
    n = wsprintfA(buf, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_logcs);
    WriteFile(g_logf, buf, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_logcs);
}

static DWORD phys(DWORD virt)
{
    LONG m = g_map;
    return virt < 4 ? (DWORD)((m >> (virt * 8)) & 0xFF) : virt;
}

static void scan(BOOL force_log)
{
    XCAPS c;
    BYTE conn[4], used[4] = {0, 0, 0, 0};
    int n = 0, v, p;
    LONG m = 0, old;
    char list[32];
    int ln = 0;

    for (p = 0; p < 4; p++)
        if (rGetCaps((DWORD)p, 0, &c) == ERROR_SUCCESS)
            conn[n++] = (BYTE)p;

    /* connected pads first (virtual 0..n-1), then the empty slots, so it stays a 1:1 permutation */
    for (v = 0; v < n; v++) { m |= (LONG)conn[v] << (v * 8); used[conn[v]] = 1; }
    for (p = 0; p < 4; p++) if (!used[p]) { m |= (LONG)p << (v * 8); v++; }
    if (!g_remap) m = 0x03020100;

    old = InterlockedExchange(&g_map, m);
    if (old != m || force_log) {
        for (v = 0; v < n; v++) ln += wsprintfA(list + ln, v ? ",%d" : "%d", conn[v]);
        if (!n) lstrcpyA(list, "none");
        logline("controllers connected in physical XInput slot(s): %s  ->  game slot 0..3 = physical %d %d %d %d%s",
                list, m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF, (m >> 24) & 0xFF, g_remap ? "" : "  (Remap=0, pass-through)");
    }
}

static DWORD WINAPI watcher(LPVOID unused)
{
    LONG seen[4] = {0, 0, 0, 0}, last[4] = {0, 0, 0, 0};
    int tick = 0, summaries = 0, i;
    (void)unused;
    for (;;) {
        Sleep(1000);
        scan(FALSE);
        for (i = 0; i < 4; i++) {
            LONG c = g_calls[i];
            if (c && !seen[i]) {
                seen[i] = 1;
                logline("game started polling XInputGetState(slot %d)  [-> physical slot %d]", i, (int)phys((DWORD)i));
            }
        }
        if (++tick % 30 == 0 && summaries < 20) {
            LONG now[4];
            for (i = 0; i < 4; i++) now[i] = g_calls[i];
            logline("XInputGetState calls in last 30s per game slot: 0:%ld 1:%ld 2:%ld 3:%ld",
                    now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3]);
            for (i = 0; i < 4; i++) last[i] = now[i];
            summaries++;
        }
    }
}

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], exe[MAX_PATH];
    WCHAR *slash;
    char exeA[MAX_PATH * 2], realA[MAX_PATH * 2];
    HANDLE th;
    (void)once; (void)param; (void)ctx;

    GetModuleFileNameW(g_self, dir, MAX_PATH);
    slash = dir; { WCHAR *s; for (s = dir; *s; s++) if (*s == L'\\') slash = s; } *slash = 0;

    lstrcpyW(path, dir); lstrcatW(path, L"\\xinput_remap.ini");
    g_remap = GetPrivateProfileIntW(L"xinput_remap", L"Remap", 1, path) != 0;
    g_log = GetPrivateProfileIntW(L"xinput_remap", L"Log", 1, path) != 0;

    if (g_log) {
        lstrcpyW(path, dir); lstrcatW(path, L"\\xinput_remap.log");
        g_logf = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\xinput9_1_0.dll");
    g_real = LoadLibraryW(path);
    if (g_real) {
        rGetState = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
        rSetState = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
        rGetCaps = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
        rGetDSGuids = (PFN_GetDSGuids)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    }

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeA, sizeof exeA, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, realA, sizeof realA, NULL, NULL);
    logline("xinput_remap 1.0 loaded into %s", exeA);
    logline("real XInput: %s %s", realA, (rGetState && rSetState && rGetCaps) ? "(ok)" : "(FAILED TO LOAD)");

    if (rGetState && rSetState && rGetCaps) {
        scan(TRUE);
        th = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return TRUE;
}

static void ensure_init(void) { InitOnceExecuteOnce(&g_once, init_once, NULL, NULL); }

DWORD WINAPI P_XInputGetState(DWORD i, XSTATE *s)
{
    DWORD p, r;
    ensure_init();
    if (!rGetState) return ERROR_DEVICE_NOT_CONNECTED;
    p = phys(i);
    if (i < 4) InterlockedIncrement(&g_calls[i]);
    r = rGetState(p, s);
    if (r == ERROR_SUCCESS && i < 4 && s && s->dwPacketNumber != g_lastPacket[i]) {
        g_lastPacket[i] = s->dwPacketNumber;
        if (s->Gamepad.wButtons && g_inputLogs < 8 && InterlockedIncrement(&g_inputLogs) <= 8)
            logline("game received button input on slot %lu (physical slot %lu): buttons=0x%04x", i, p, s->Gamepad.wButtons);
    }
    return r;
}

DWORD WINAPI P_XInputSetState(DWORD i, XVIB *v)
{
    ensure_init();
    return rSetState ? rSetState(phys(i), v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI P_XInputGetCapabilities(DWORD i, DWORD flags, XCAPS *c)
{
    ensure_init();
    return rGetCaps ? rGetCaps(phys(i), flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI P_XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *render, GUID *capture)
{
    ensure_init();
    return rGetDSGuids ? rGetDSGuids(phys(i), render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logcs);
    }
    return TRUE;
}
