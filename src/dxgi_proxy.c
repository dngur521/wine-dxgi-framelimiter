// dxgi_proxy — a Windows-side frame limiter for DXMT games under Wine.
//
// Installed as system32\dxgi.dll in front of DXMT's real dxgi (renamed dxgi_orig.dll).
// Every export forwards to the real DLL. In the target process only (FL_EXE; any
// process when unset) with FL_FPS > 0, the factory's CreateSwapChain* methods are patched
// so that every swapchain's Present/Present1 sleeps *after* presenting, on the game's
// own render thread, until the next frame slot.
//
// Why here and not DXMT's d3d11.preferredMaxFrameRate: DXMT paces at the very end of
// the pipeline (presentDrawable:afterMinimumDuration:), so the game keeps producing
// frames and the 3-frame DXMT queue plus the drawable pool fill up — ~50-80 ms of
// input-to-photon latency at 60 fps. Waiting in Present on the calling thread keeps
// the queue empty: the game wakes, samples input, renders, and that frame is shown
// immediately. The macOS cursor is drawn by WindowServer and is never affected.
//
// Environment (inherited from the Wine launch script through Steam):
//   FL_FPS        target fps; unset/0 = pure passthrough
//   FL_EXE        process basename(s) to limit, comma-separated (unset or "*" = any)
//   FL_SPIN_US    busy-wait tail before the deadline in µs (default 0: measured no gain)
//   FL_SYNC       -1 keep the game's SyncInterval (default), 0/1 force it
//   FL_LOG        Windows path of a log file (optional)
//   FL_REAL_DXGI  path of the real dxgi (default <system32>\dxgi_orig.dll)

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID  // define the IIDs here: linking libdxgi would import ourselves
#include <windows.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

typedef HRESULT (WINAPI *PFN_CreateFactory)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateFactory2)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_GetDebug1)(UINT, REFIID, void **);
typedef LONG    (NTAPI  *PFN_NtDelayExecution)(BOOLEAN, PLARGE_INTEGER);

typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1 *, UINT, UINT,
                                                   const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *,
                                                          DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND,
        const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
        IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForCoreWindow)(IDXGIFactory2 *, IUnknown *,
        IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateSwapChainForComposition)(IDXGIFactory2 *, IUnknown *,
        const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *, IDXGISwapChain1 **);

// vtable slots (IUnknown 0-2, IDXGIObject 3-6, ...)
enum {
    SLOT_FACTORY_CREATE_SWAPCHAIN            = 10,
    SLOT_FACTORY2_CREATE_SWAPCHAIN_HWND      = 15,
    SLOT_FACTORY2_CREATE_SWAPCHAIN_COREWIN   = 16,
    SLOT_FACTORY2_CREATE_SWAPCHAIN_COMPOSE   = 24,
    SLOT_SWAPCHAIN_PRESENT                   = 8,
    SLOT_SWAPCHAIN1_PRESENT1                 = 22,
};

static HMODULE g_real;
static PFN_CreateFactory  g_CreateDXGIFactory;
static PFN_CreateFactory  g_CreateDXGIFactory1;
static PFN_CreateFactory2 g_CreateDXGIFactory2;
static PFN_GetDebug1      g_DXGIGetDebugInterface1;

static int  g_active;          // this process is the target and FL_FPS > 0
static int  g_fps;
static int  g_spin_us = 0;
static int  g_sync = -1;
static char g_log_path[MAX_PATH];

static PFN_NtDelayExecution g_NtDelayExecution;
static LARGE_INTEGER g_qpf;
static LONGLONG g_next_deadline;   // QPC ticks
static LONGLONG g_stat_start;
static unsigned g_stat_frames;
static volatile DWORD g_present_tid;  // thread inside Present (DXMT's Present may call Present1)

static CRITICAL_SECTION g_hook_lock;

// Original methods, keyed by vtable (one swapchain class in practice; a few for safety).
#define MAX_VTBL 8
static struct { void **vtbl; PFN_Present present; PFN_Present1 present1; } g_sc[MAX_VTBL];
static int g_sc_count;
static struct {
    void **vtbl;
    PFN_CreateSwapChain cs;
    PFN_CreateSwapChainForHwnd hwnd;
    PFN_CreateSwapChainForCoreWindow corewin;
    PFN_CreateSwapChainForComposition compose;
} g_fac[MAX_VTBL];
static int g_fac_count;

static void log_line(const char *fmt, ...) {
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int env_int(const char *name, int def) {
    char buf[32];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return (n > 0 && n < sizeof buf) ? atoi(buf) : def;
}

static int process_matches(void) {
    char want[1024];
    DWORD n = GetEnvironmentVariableA("FL_EXE", want, sizeof want);
    if (n == 0 || n >= sizeof want || strcmp(want, "*") == 0) return 1;
    char path[MAX_PATH];
    n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path) return 0;
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    for (char *tok = strtok(want, ",;"); tok; tok = strtok(NULL, ",;")) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ') *--end = '\0';
        if (*tok && _stricmp(base, tok) == 0) return 1;
    }
    return 0;
}

static BOOL patch_slot(void **vtbl, int slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &old)) return FALSE;
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void *));
    return TRUE;
}

// ---- pacing ----
// Called after the real Present returns: the next frame starts (and samples input)
// right when we wake, so the wait adds no latency to what ends up on screen.
static void wait_until(LONGLONG deadline) {
    LONGLONG spin = g_qpf.QuadPart * g_spin_us / 1000000;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG remain = deadline - now.QuadPart;
    if (remain > spin && g_NtDelayExecution) {
        LARGE_INTEGER rel;   // negative = relative, in 100 ns units
        rel.QuadPart = -((remain - spin) * 10000000 / g_qpf.QuadPart);
        if (rel.QuadPart < 0) g_NtDelayExecution(FALSE, &rel);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < deadline);
}

static void pace_after_present(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG period = g_qpf.QuadPart / g_fps;

    if (g_next_deadline == 0 || now.QuadPart > g_next_deadline + period) {
        // first frame or a long stall (loading, alt-tab): resync, never pay back debt
        g_next_deadline = now.QuadPart + period;
    } else {
        if (g_next_deadline > now.QuadPart) wait_until(g_next_deadline);
        g_next_deadline += period;
    }

    g_stat_frames++;
    QueryPerformanceCounter(&now);
    if (g_stat_start == 0) g_stat_start = now.QuadPart;
    LONGLONG el = now.QuadPart - g_stat_start;
    if (el >= g_qpf.QuadPart * 5) {
        log_line("fps=%.2f (target %d)", (double)g_stat_frames * g_qpf.QuadPart / el, g_fps);
        g_stat_frames = 0;
        g_stat_start = now.QuadPart;
    }
}

static int find_sc(void **vtbl) {
    for (int i = 0; i < g_sc_count; i++) if (g_sc[i].vtbl == vtbl) return i;
    return -1;
}

static HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    int i = find_sc(*(void ***)sc);
    PFN_Present orig = g_sc[i < 0 ? 0 : i].present;
    if (g_present_tid == GetCurrentThreadId() || (flags & DXGI_PRESENT_TEST)) return orig(sc, sync, flags);
    if (g_sync >= 0) sync = (UINT)g_sync;
    g_present_tid = GetCurrentThreadId();
    HRESULT hr = orig(sc, sync, flags);
    g_present_tid = 0;
    pace_after_present();
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *p) {
    int i = find_sc(*(void ***)sc);
    PFN_Present1 orig = g_sc[i < 0 ? 0 : i].present1;
    if (g_present_tid == GetCurrentThreadId() || (flags & DXGI_PRESENT_TEST)) return orig(sc, sync, flags, p);
    if (g_sync >= 0) sync = (UINT)g_sync;
    g_present_tid = GetCurrentThreadId();
    HRESULT hr = orig(sc, sync, flags, p);
    g_present_tid = 0;
    pace_after_present();
    return hr;
}

static void hook_swapchain(IUnknown *obj) {
    if (!obj) return;
    void **vtbl = *(void ***)obj;
    EnterCriticalSection(&g_hook_lock);
    if (find_sc(vtbl) < 0 && g_sc_count < MAX_VTBL) {
        int i = g_sc_count++;
        g_sc[i].vtbl = vtbl;
        g_sc[i].present = (PFN_Present)vtbl[SLOT_SWAPCHAIN_PRESENT];
        patch_slot(vtbl, SLOT_SWAPCHAIN_PRESENT, (void *)hook_Present);

        IDXGISwapChain1 *sc1 = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IDXGISwapChain1, (void **)&sc1)) && sc1) {
            // Present1 is only patched when it lives in the same vtable we just hooked.
            if (*(void ***)sc1 == vtbl) {
                g_sc[i].present1 = (PFN_Present1)vtbl[SLOT_SWAPCHAIN1_PRESENT1];
                patch_slot(vtbl, SLOT_SWAPCHAIN1_PRESENT1, (void *)hook_Present1);
            }
            IDXGISwapChain1_Release(sc1);
        }
        log_line("hooked swapchain vtbl=%p present1=%s", (void *)vtbl,
                 g_sc[i].present1 ? "yes" : "no");
    }
    LeaveCriticalSection(&g_hook_lock);
}

// ---- factory hooks ----
static int find_fac(void **vtbl) {
    for (int i = 0; i < g_fac_count; i++) if (g_fac[i].vtbl == vtbl) return i;
    return -1;
}
#define FAC(self) g_fac[find_fac(*(void ***)(self))]

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChain(IDXGIFactory *f, IUnknown *dev,
        DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    HRESULT hr = FAC(f).cs(f, dev, d, out);
    if (SUCCEEDED(hr) && out) hook_swapchain((IUnknown *)*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(IDXGIFactory2 *f, IUnknown *dev,
        HWND w, const DXGI_SWAP_CHAIN_DESC1 *d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
        IDXGIOutput *o, IDXGISwapChain1 **out) {
    HRESULT hr = FAC(f).hwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) hook_swapchain((IUnknown *)*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForCoreWindow(IDXGIFactory2 *f, IUnknown *dev,
        IUnknown *win, const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *o, IDXGISwapChain1 **out) {
    HRESULT hr = FAC(f).corewin(f, dev, win, d, o, out);
    if (SUCCEEDED(hr) && out) hook_swapchain((IUnknown *)*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForComposition(IDXGIFactory2 *f, IUnknown *dev,
        const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *o, IDXGISwapChain1 **out) {
    HRESULT hr = FAC(f).compose(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out) hook_swapchain((IUnknown *)*out);
    return hr;
}

static void hook_factory(void *obj) {
    if (!g_active || !obj) return;
    IUnknown *unk = (IUnknown *)obj;
    void **vtbl = *(void ***)unk;
    EnterCriticalSection(&g_hook_lock);
    if (find_fac(vtbl) < 0 && g_fac_count < MAX_VTBL) {
        int i = g_fac_count++;
        g_fac[i].vtbl = vtbl;
        g_fac[i].cs = (PFN_CreateSwapChain)vtbl[SLOT_FACTORY_CREATE_SWAPCHAIN];
        patch_slot(vtbl, SLOT_FACTORY_CREATE_SWAPCHAIN, (void *)hook_CreateSwapChain);

        // Factory2 slots are only touched when that interface shares this vtable,
        // so a factory class that stops at IDXGIFactory1 is never written past its end.
        IDXGIFactory2 *f2 = NULL;
        int has2 = 0;
        if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IDXGIFactory2, (void **)&f2)) && f2) {
            if (*(void ***)f2 == vtbl) {
                has2 = 1;
                g_fac[i].hwnd    = (PFN_CreateSwapChainForHwnd)vtbl[SLOT_FACTORY2_CREATE_SWAPCHAIN_HWND];
                g_fac[i].corewin = (PFN_CreateSwapChainForCoreWindow)vtbl[SLOT_FACTORY2_CREATE_SWAPCHAIN_COREWIN];
                g_fac[i].compose = (PFN_CreateSwapChainForComposition)vtbl[SLOT_FACTORY2_CREATE_SWAPCHAIN_COMPOSE];
                patch_slot(vtbl, SLOT_FACTORY2_CREATE_SWAPCHAIN_HWND,    (void *)hook_CreateSwapChainForHwnd);
                patch_slot(vtbl, SLOT_FACTORY2_CREATE_SWAPCHAIN_COREWIN, (void *)hook_CreateSwapChainForCoreWindow);
                patch_slot(vtbl, SLOT_FACTORY2_CREATE_SWAPCHAIN_COMPOSE, (void *)hook_CreateSwapChainForComposition);
            }
            IDXGIFactory2_Release(f2);
        }
        log_line("hooked factory vtbl=%p factory2=%d", (void *)vtbl, has2);
    }
    LeaveCriticalSection(&g_hook_lock);
}

// ---- real DLL ----
static BOOL load_real(void) {
    if (g_real) return TRUE;
    WCHAR path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"FL_REAL_DXGI", path, MAX_PATH)) {
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 20) return FALSE;
        wcscat(path, L"\\dxgi_orig.dll");
    }
    g_real = LoadLibraryW(path);
    if (!g_real) { log_line("cannot load real dxgi (err=%lu)", GetLastError()); return FALSE; }
    g_CreateDXGIFactory      = (PFN_CreateFactory)GetProcAddress(g_real, "CreateDXGIFactory");
    g_CreateDXGIFactory1     = (PFN_CreateFactory)GetProcAddress(g_real, "CreateDXGIFactory1");
    g_CreateDXGIFactory2     = (PFN_CreateFactory2)GetProcAddress(g_real, "CreateDXGIFactory2");
    g_DXGIGetDebugInterface1 = (PFN_GetDebug1)GetProcAddress(g_real, "DXGIGetDebugInterface1");
    return TRUE;
}

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **out) {
    if (!load_real() || !g_CreateDXGIFactory) return E_FAIL;
    HRESULT hr = g_CreateDXGIFactory(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out) {
    if (!load_real() || !g_CreateDXGIFactory1) return E_FAIL;
    HRESULT hr = g_CreateDXGIFactory1(riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}
HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **out) {
    if (!load_real() || !g_CreateDXGIFactory2) return E_FAIL;
    HRESULT hr = g_CreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out) hook_factory(*out);
    return hr;
}
HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **out) {
    if (!load_real() || !g_DXGIGetDebugInterface1) return E_NOINTERFACE;
    return g_DXGIGetDebugInterface1(flags, riid, out);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_hook_lock);
    GetEnvironmentVariableA("FL_LOG", g_log_path, sizeof g_log_path);

    g_fps = env_int("FL_FPS", 0);
    if (g_fps > 1000) g_fps = 1000;
    g_spin_us = env_int("FL_SPIN_US", 0);
    if (g_spin_us < 0) g_spin_us = 0;
    g_sync = env_int("FL_SYNC", -1);
    if (g_sync > 4) g_sync = 4;

    if (g_fps > 0 && process_matches()) {
        g_active = 1;
        QueryPerformanceFrequency(&g_qpf);
        g_NtDelayExecution = (PFN_NtDelayExecution)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
        log_line("active: fps=%d spin_us=%d sync=%d", g_fps, g_spin_us, g_sync);
    }
    return TRUE;
}
