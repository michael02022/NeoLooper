#include "gui_window.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>

#include <chrono>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl2.h"

#include "IconsFontAwesome4.h"
#include "fontawesome_data.hpp"

#include <sndfile.h>
#include <rubberband/RubberBandStretcher.h>

// Declared in imgui_impl_win32.h only inside a '#if 0' block (to avoid
// dragging <windows.h> into that header) -- forward-declared here per its
// own instructions.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                              WPARAM wParam, LPARAM lParam);

// Thread-local ImGui context definition (see imconfig.h): each plugin
// instance renders on its own thread with its own context.
thread_local ImGuiContext* g_sclImGuiTls = nullptr;

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);

namespace {

const wchar_t* const kClassName = L"NeoLooperGuiWindow";

// Per-window data the WndProc needs that isn't reachable through Win32 APIs
// alone. Lives on threadMain's stack for the lifetime of the window.
struct WndCtx {
    std::function<void(const std::string&)>* onFileDrop;
};

std::string wideToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n - 1, '\0'); // n includes the null terminator
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* ctx = (WndCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            // so keyboard shortcuts get delivered when embedded as a child
            SetFocus(hwnd);
            break;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            wchar_t path[MAX_PATH];
            if (ctx && ctx->onFileDrop && *ctx->onFileDrop &&
                DragQueryFileW(drop, 0, path, MAX_PATH))
                (*ctx->onFileDrop)(wideToUtf8(path));
            DragFinish(drop);
            return 0;
        }
        case WM_CLOSE:
            // embedded plugin window: the host (via clap gui->destroy) owns
            // its lifecycle, not the window manager
            return 0;
        case WM_ERASEBKGND:
            return 1; // avoids a flash before the first GL frame is drawn
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerClassOnce(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC; // required by ImGui_ImplWin32_InitForOpenGL
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = nullptr; // ImGui_ImplWin32 drives the cursor shape itself
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

// sndfile.dll/rubberband-3.dll are /DELAYLOAD-ed (see CMakeLists.txt and
// win_delayload.cpp) so their own LoadLibrary only happens on first actual
// use. If that first use happens deep inside a WndProc callback -- e.g. the
// user's first "import audio" click, handled via DispatchMessageWorker ->
// wglWndProc -> our WndProc -> ImGui -> sf_open -- the delay-load helper's
// LoadLibraryExW call runs re-entrantly inside Win32's message dispatch,
// which can collide with the OS loader's own parallel/worker-thread module
// resolution and surface as a bogus "ordinal not found" hard error instead
// of loading cleanly. Touching both libraries once here, before the window
// (and therefore any WndProc) exists at all, forces that one-time
// LoadLibrary+bind to happen on a plain, non-reentrant thread instead --
// by the time a real WndProc-driven call happens, delay-load's cache makes
// it a no-op.
void primeDelayLoadedLibs() {
    char buf[256];
    sf_command(nullptr, SFC_GET_LIB_VERSION, buf, sizeof(buf));
    RubberBand::RubberBandStretcher rb(44100, 1,
                                       RubberBand::RubberBandStretcher::OptionProcessOffline);
    (void)rb;
}

} // namespace

GuiWindow::GuiWindow(std::function<void()> drawUI,
                     std::function<void(const std::string&)> onFileDrop)
    : drawUI_(std::move(drawUI)), onFileDrop_(std::move(onFileDrop)) {}

GuiWindow::~GuiWindow() { destroy(); }

bool GuiWindow::create(uint32_t w, uint32_t h) {
    if (running_.load()) return true;
    if (w > 0 && h > 0) {
        width_.store(w);
        height_.store(h);
    }
    quit_.store(false);
    thread_ = std::thread([this] { threadMain(); });
    running_.store(true);
    return true;
}

void GuiWindow::destroy() {
    if (!running_.load()) return;
    quit_.store(true);
    if (thread_.joinable()) {
        // A plain thread_.join() here can deadlock: our hwnd is WS_CHILD of
        // a window owned by the HOST's thread (the one calling destroy(),
        // e.g. REAPER's main thread), and Win32 child-window teardown
        // (DestroyWindow -> WM_PARENTNOTIFY to the parent, cross-thread
        // SetParent, etc.) needs that thread to keep pumping its own
        // message queue to complete. Blocking it in a bare join() while our
        // GUI thread's DestroyWindow() call waits on exactly that message
        // being processed is a classic cross-thread window deadlock. Pump
        // the calling thread's queue while we wait instead.
        HANDLE h = thread_.native_handle();
        while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) ==
               WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        thread_.join();
    }
    running_.store(false);
}

void GuiWindow::setParent(intptr_t nativeParent) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingParent_ = nativeParent;
}

void GuiWindow::setVisible(bool visible) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingVisible_ = visible ? 1 : 0;
}

void GuiWindow::setSize(uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> l(cmdMutex_);
    pendingW_ = w;
    pendingH_ = h;
    hasPendingSize_ = true;
}

void GuiWindow::threadMain() {
    primeDelayLoadedLibs();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    registerClassOnce(hInst);

    WndCtx ctx{&onFileDrop_};

    uint32_t w = width_.load(), h = height_.load();
    // WS_POPUP (no parent yet): the host reparents us via setParent() once
    // it has a window to embed into, same two-step flow as the X11 backend
    // (create, then XReparentWindow/SetParent on demand).
    HWND hwnd = CreateWindowExW(0, kClassName, L"NeoLooper",
                                WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                0, 0, (int)w, (int)h,
                                nullptr, nullptr, hInst, &ctx);
    if (!hwnd) {
        fprintf(stderr, "[NeoLooper] could not create the Win32 window\n");
        return;
    }
    DragAcceptFiles(hwnd, TRUE);

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) {
        fprintf(stderr, "[NeoLooper] no compatible WGL pixel format\n");
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        return;
    }
    HGLRC glrc = wglCreateContext(hdc);
    if (!glrc) {
        fprintf(stderr, "[NeoLooper] wglCreateContext failed\n");
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        return;
    }
    wglMakeCurrent(hdc, glrc);

    // no vsync: our own clock sets the pace (the swap must not block)
    if (auto f = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT"))
        f(0);

    IMGUI_CHECKVERSION();
    ImGuiContext* imCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(imCtx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini in the DAW's CWD
    io.DisplaySize = ImVec2((float)w, (float)h);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScrollbarSize = 16.f;

    // Icon font (Font Awesome 4, embedded -- see
    // third_party/fontawesome/VENDORED_FROM.txt): merged ON TOP of the
    // default font (same ImFont, same size) so a button can combine icon +
    // text in a single string (ICON_FA_PLAY " Play") without any
    // PushFont/PopFont calls.
    io.Fonts->AddFontDefault();
    {
        ImFontConfig iconCfg;
        iconCfg.MergeMode = true;
        iconCfg.PixelSnapH = true;
        // The buffer is a global const array embedded in the binary (see
        // fontawesome_data.hpp), not memory allocated by ImGui -- it must
        // not try to free it when destroying the atlas.
        iconCfg.FontDataOwnedByAtlas = false;
        static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
        io.Fonts->AddFontFromMemoryTTF(fontawesome_webfont_ttf,
                                       (int)fontawesome_webfont_ttf_len, 13.f,
                                       &iconCfg, iconRanges);
    }

    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL2_Init();

    bool mapped = false;
    auto lastFrame = std::chrono::steady_clock::now();

    while (!quit_.load()) {
        // -------- host commands --------
        {
            std::lock_guard<std::mutex> l(cmdMutex_);
            if (pendingParent_) {
                HWND parent = (HWND)pendingParent_;
                LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
                style = (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME |
                                   WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) |
                       WS_CHILD;
                SetWindowLongPtrW(hwnd, GWL_STYLE, style);
                SetParent(hwnd, parent);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                SWP_FRAMECHANGED);
                pendingParent_ = 0;
            }
            if (hasPendingSize_) {
                SetWindowPos(hwnd, nullptr, 0, 0, (int)pendingW_, (int)pendingH_,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                width_.store(pendingW_);
                height_.store(pendingH_);
                hasPendingSize_ = false;
            }
            if (pendingVisible_ == 1 && !mapped) {
                ShowWindow(hwnd, SW_SHOWNA);
                mapped = true;
            } else if (pendingVisible_ == 0 && mapped) {
                ShowWindow(hwnd, SW_HIDE);
                mapped = false;
            }
            pendingVisible_ = -1;
        }

        // -------- Win32 messages (non-blocking: our own clock paces frames) --------
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // -------- ImGui frame --------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        io.DeltaTime = dt > 0.f ? dt : 1.f / 60.f;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (drawUI_) drawUI_();
        ImGui::Render();

        width_.store((uint32_t)io.DisplaySize.x);
        height_.store((uint32_t)io.DisplaySize.y);
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.09f, 0.09f, 0.10f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(hdc);

        // ~60 fps
        auto frameEnd = std::chrono::steady_clock::now();
        auto spent = frameEnd - now;
        auto budget = std::chrono::microseconds(16600);
        if (spent < budget)
            std::this_thread::sleep_for(budget - spent);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(imCtx);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glrc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
}
