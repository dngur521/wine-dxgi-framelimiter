// fltest — D3D11 present-loop harness for the dxgi proxy.
// Creates a window and a flip swapchain via IDXGIFactory2::CreateSwapChainForHwnd (as
// Unity does), presents with SyncInterval 0 for N seconds, and prints the measured fps
// and frame-interval jitter.  Usage: fltest.exe [seconds]

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 5;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"fltest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"fltest", L"fltest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 640, 360, NULL, NULL, wc.hInstance, NULL);

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%lx\n", hr); return 1; }

    IDXGIFactory2 *fac = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&fac);
    if (FAILED(hr)) { printf("CreateDXGIFactory1 failed 0x%lx\n", hr); return 1; }

    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Width = 640; d.Height = 360;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc = NULL;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev, hwnd, &d, NULL, NULL, &sc);
    if (FAILED(hr)) { printf("CreateSwapChainForHwnd failed 0x%lx\n", hr); return 1; }

    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

    LARGE_INTEGER f, t0, prev, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    prev = t0;
    double sum = 0, sum2 = 0, maxdt = 0;
    long frames = 0, n = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        float c[4] = { (frames % 120) / 120.0f, 0.2f, 0.4f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        IDXGISwapChain1_Present(sc, 0, 0);
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart;
        prev = now;
        if (now.QuadPart - t0.QuadPart > f.QuadPart) {   // skip the 1 s warm-up
            n++; sum += dt; sum2 += dt * dt; if (dt > maxdt) maxdt = dt; }
        frames++;
        if (now.QuadPart - t0.QuadPart >= f.QuadPart * seconds) break;
    }
    double mean = sum / n;
    printf("frames=%ld fps=%.2f mean=%.3fms sd=%.3fms max=%.3fms\n", frames,
           1000.0 / mean, mean, sqrt(sum2 / n - mean * mean), maxdt);
    return 0;
}
