#include "gui_window.hpp"

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_opengl2.h"

#include "IconsFontAwesome4.h"
#include "fontawesome_data.hpp"

// Thread-local ImGui context definition (see imconfig.h): each plugin
// instance renders on its own thread with its own context.
thread_local ImGuiContext* g_sclImGuiTls = nullptr;

namespace {

// A plugin can't let Xlib's default handler kill the host over a transient
// BadWindow (e.g. if the parent disappears before we do). Installed once for
// the whole process.
int swallowXError(Display*, XErrorEvent* e) {
    fprintf(stderr, "[NeoLooper] X11 error ignored: code=%d\n",
            (int)e->error_code);
    return 0;
}

void installErrorHandlerOnce() {
    static bool done = false;
    if (!done) {
        done = true;
        XSetErrorHandler(swallowXError);
    }
}

ImGuiKey keysymToImGuiKey(KeySym k) {
    switch (k) {
        case XK_Tab: return ImGuiKey_Tab;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Page_Up: return ImGuiKey_PageUp;
        case XK_Page_Down: return ImGuiKey_PageDown;
        case XK_Home: return ImGuiKey_Home;
        case XK_End: return ImGuiKey_End;
        case XK_Insert: return ImGuiKey_Insert;
        case XK_Delete: case XK_KP_Delete: return ImGuiKey_Delete;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_space: return ImGuiKey_Space;
        case XK_Return: return ImGuiKey_Enter;
        case XK_KP_Enter: return ImGuiKey_KeypadEnter;
        case XK_Escape: return ImGuiKey_Escape;
        case XK_apostrophe: return ImGuiKey_Apostrophe;
        case XK_comma: return ImGuiKey_Comma;
        case XK_minus: return ImGuiKey_Minus;
        case XK_period: return ImGuiKey_Period;
        case XK_slash: return ImGuiKey_Slash;
        case XK_semicolon: return ImGuiKey_Semicolon;
        case XK_equal: return ImGuiKey_Equal;
        case XK_bracketleft: return ImGuiKey_LeftBracket;
        case XK_backslash: return ImGuiKey_Backslash;
        case XK_bracketright: return ImGuiKey_RightBracket;
        case XK_grave: return ImGuiKey_GraveAccent;
        case XK_Caps_Lock: return ImGuiKey_CapsLock;
        case XK_Shift_L: return ImGuiKey_LeftShift;
        case XK_Shift_R: return ImGuiKey_RightShift;
        case XK_Control_L: return ImGuiKey_LeftCtrl;
        case XK_Control_R: return ImGuiKey_RightCtrl;
        case XK_Alt_L: return ImGuiKey_LeftAlt;
        case XK_Alt_R: return ImGuiKey_RightAlt;
        case XK_Super_L: return ImGuiKey_LeftSuper;
        case XK_Super_R: return ImGuiKey_RightSuper;
        default: break;
    }
    if (k >= XK_0 && k <= XK_9)
        return (ImGuiKey)(ImGuiKey_0 + (k - XK_0));
    if (k >= XK_a && k <= XK_z)
        return (ImGuiKey)(ImGuiKey_A + (k - XK_a));
    if (k >= XK_A && k <= XK_Z)
        return (ImGuiKey)(ImGuiKey_A + (k - XK_A));
    if (k >= XK_F1 && k <= XK_F12)
        return (ImGuiKey)(ImGuiKey_F1 + (k - XK_F1));
    if (k >= XK_KP_0 && k <= XK_KP_9)
        return (ImGuiKey)(ImGuiKey_Keypad0 + (k - XK_KP_0));
    return ImGuiKey_None;
}

void latin1ToUtf8(const char* in, int n, char* out) {
    int o = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

// Decodes a file:// URI (including percent-encoding).
std::string uriToPath(const std::string& uri) {
    std::string u = uri;
    if (u.rfind("file://", 0) == 0) {
        u = u.substr(7);
        size_t slash = u.find('/');
        if (slash != std::string::npos && slash > 0)
            u = u.substr(slash); // strips the optional hostname
    }
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size()) {
            char hex[3] = {u[i + 1], u[i + 2], 0};
            out.push_back((char)strtol(hex, nullptr, 16));
            i += 2;
        } else {
            out.push_back(u[i]);
        }
    }
    return out;
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
    if (thread_.joinable()) thread_.join();
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
    installErrorHandlerOnce();

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "[NeoLooper] could not open the X11 display\n");
        return;
    }
    const int screen = DefaultScreen(dpy);

    int visAttrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                      GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 0,
                      None};
    XVisualInfo* vi = glXChooseVisual(dpy, screen, visAttrs);
    if (!vi) {
        fprintf(stderr, "[NeoLooper] no GLX visual available\n");
        XCloseDisplay(dpy);
        return;
    }

    Colormap cmap =
        XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.border_pixel = 0;
    swa.background_pixel = BlackPixel(dpy, screen);
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                     FocusChangeMask | PropertyChangeMask;

    uint32_t w = width_.load(), h = height_.load();
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, w, h, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBorderPixel | CWBackPixel |
                                   CWEventMask,
                               &swa);
    XStoreName(dpy, win, "NeoLooper");

    // -------- XDND atoms --------
    Atom aXdndAware = XInternAtom(dpy, "XdndAware", False);
    Atom aXdndEnter = XInternAtom(dpy, "XdndEnter", False);
    Atom aXdndPosition = XInternAtom(dpy, "XdndPosition", False);
    Atom aXdndStatus = XInternAtom(dpy, "XdndStatus", False);
    Atom aXdndLeave = XInternAtom(dpy, "XdndLeave", False);
    Atom aXdndDrop = XInternAtom(dpy, "XdndDrop", False);
    Atom aXdndFinished = XInternAtom(dpy, "XdndFinished", False);
    Atom aXdndSelection = XInternAtom(dpy, "XdndSelection", False);
    Atom aXdndActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    Atom aXdndTypeList = XInternAtom(dpy, "XdndTypeList", False);
    Atom aUriList = XInternAtom(dpy, "text/uri-list", False);
    Atom aDndProp = XInternAtom(dpy, "SCHOOLLOOPER_DND", False);
    {
        Atom dndVersion = 5;
        XChangeProperty(dpy, win, aXdndAware, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&dndVersion, 1);
    }
    Window dndSource = 0;
    bool dndUriOk = false;

    GLXContext glc = glXCreateContext(dpy, vi, nullptr, GL_TRUE);
    glXMakeCurrent(dpy, win, glc);

    // no vsync: our own clock sets the pace (the swap must not block)
    using SwapIntervalFn = void (*)(Display*, GLXDrawable, int);
    if (auto f = (SwapIntervalFn)glXGetProcAddressARB(
            (const GLubyte*)"glXSwapIntervalEXT"))
        f(dpy, win, 0);

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
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

    ImGui_ImplOpenGL2_Init();

    bool mapped = false;
    auto lastFrame = std::chrono::steady_clock::now();

    while (!quit_.load()) {
        // -------- host commands --------
        {
            std::lock_guard<std::mutex> l(cmdMutex_);
            if (pendingParent_) {
                XReparentWindow(dpy, win, (Window)pendingParent_, 0, 0);
                pendingParent_ = 0;
            }
            if (hasPendingSize_) {
                XResizeWindow(dpy, win, pendingW_, pendingH_);
                width_.store(pendingW_);
                height_.store(pendingH_);
                hasPendingSize_ = false;
            }
            if (pendingVisible_ == 1 && !mapped) {
                XMapWindow(dpy, win);
                mapped = true;
            } else if (pendingVisible_ == 0 && mapped) {
                XUnmapWindow(dpy, win);
                mapped = false;
            }
            pendingVisible_ = -1;
        }

        // -------- X11 events --------
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case ConfigureNotify:
                    width_.store((uint32_t)ev.xconfigure.width);
                    height_.store((uint32_t)ev.xconfigure.height);
                    io.DisplaySize = ImVec2((float)ev.xconfigure.width,
                                            (float)ev.xconfigure.height);
                    break;
                case MotionNotify:
                    io.AddMousePosEvent((float)ev.xmotion.x,
                                        (float)ev.xmotion.y);
                    break;
                case EnterNotify:
                    io.AddMousePosEvent((float)ev.xcrossing.x,
                                        (float)ev.xcrossing.y);
                    break;
                case LeaveNotify:
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                    break;
                case ButtonPress:
                case ButtonRelease: {
                    bool down = ev.type == ButtonPress;
                    unsigned b = ev.xbutton.button;
                    if (down) // so keyboard shortcuts get delivered
                        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
                    if (b == Button1) io.AddMouseButtonEvent(0, down);
                    else if (b == Button2) io.AddMouseButtonEvent(2, down);
                    else if (b == Button3) io.AddMouseButtonEvent(1, down);
                    else if (down && b == Button4) io.AddMouseWheelEvent(0, 1);
                    else if (down && b == Button5) io.AddMouseWheelEvent(0, -1);
                    else if (down && b == 6) io.AddMouseWheelEvent(1, 0);
                    else if (down && b == 7) io.AddMouseWheelEvent(-1, 0);
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    // filters the auto-repeat Release/Press pair
                    if (ev.type == KeyRelease && XPending(dpy)) {
                        XEvent next;
                        XPeekEvent(dpy, &next);
                        if (next.type == KeyPress &&
                            next.xkey.keycode == ev.xkey.keycode &&
                            next.xkey.time == ev.xkey.time)
                            break; // the following KeyPress re-emits it
                    }
                    bool down = ev.type == KeyPress;
                    char buf[8];
                    KeySym ks = NoSymbol;
                    int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks,
                                          nullptr);
                    unsigned st = ev.xkey.state;
                    io.AddKeyEvent(ImGuiMod_Ctrl, (st & ControlMask) != 0);
                    io.AddKeyEvent(ImGuiMod_Shift, (st & ShiftMask) != 0);
                    io.AddKeyEvent(ImGuiMod_Alt, (st & Mod1Mask) != 0);
                    KeySym lower, upper;
                    XConvertCase(ks, &lower, &upper);
                    ImGuiKey k = keysymToImGuiKey(lower);
                    if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
                    if (down && n > 0 && !(st & ControlMask) &&
                        (unsigned char)buf[0] >= 32) {
                        char utf8[16];
                        latin1ToUtf8(buf, n, utf8);
                        io.AddInputCharactersUTF8(utf8);
                    }
                    break;
                }
                case FocusIn:
                    io.AddFocusEvent(true);
                    break;
                case FocusOut:
                    io.AddFocusEvent(false);
                    break;
                case ClientMessage: {
                    Atom t = ev.xclient.message_type;
                    if (t == aXdndEnter) {
                        dndSource = (Window)ev.xclient.data.l[0];
                        dndUriOk = false;
                        if (ev.xclient.data.l[1] & 1) {
                            // more than 3 types: read XdndTypeList
                            Atom type;
                            int fmt;
                            unsigned long count, after;
                            unsigned char* data = nullptr;
                            if (XGetWindowProperty(dpy, dndSource, aXdndTypeList,
                                                   0, 1024, False, XA_ATOM,
                                                   &type, &fmt, &count, &after,
                                                   &data) == Success && data) {
                                Atom* atoms = (Atom*)data;
                                for (unsigned long i = 0; i < count; ++i)
                                    if (atoms[i] == aUriList) dndUriOk = true;
                                XFree(data);
                            }
                        } else {
                            for (int i = 2; i <= 4; ++i)
                                if ((Atom)ev.xclient.data.l[i] == aUriList)
                                    dndUriOk = true;
                        }
                    } else if (t == aXdndPosition && dndSource) {
                        XEvent st{};
                        st.xclient.type = ClientMessage;
                        st.xclient.display = dpy;
                        st.xclient.window = dndSource;
                        st.xclient.message_type = aXdndStatus;
                        st.xclient.format = 32;
                        st.xclient.data.l[0] = (long)win;
                        st.xclient.data.l[1] = dndUriOk ? 1 : 0;
                        st.xclient.data.l[4] = (long)aXdndActionCopy;
                        XSendEvent(dpy, dndSource, False, NoEventMask, &st);
                    } else if (t == aXdndDrop && dndSource) {
                        if (dndUriOk)
                            XConvertSelection(dpy, aXdndSelection, aUriList,
                                              aDndProp, win,
                                              (Time)ev.xclient.data.l[2]);
                    } else if (t == aXdndLeave) {
                        dndSource = 0;
                    }
                    break;
                }
                case SelectionNotify: {
                    if (ev.xselection.property != aDndProp) break;
                    Atom type;
                    int fmt;
                    unsigned long count, after;
                    unsigned char* data = nullptr;
                    if (XGetWindowProperty(dpy, win, aDndProp, 0, 1 << 20,
                                           True, AnyPropertyType, &type, &fmt,
                                           &count, &after, &data) == Success &&
                        data) {
                        std::string list((char*)data, count);
                        XFree(data);
                        // first line of the uri-list
                        size_t pos = 0;
                        while (pos < list.size()) {
                            size_t eol = list.find_first_of("\r\n", pos);
                            std::string line = list.substr(
                                pos, eol == std::string::npos ? eol
                                                              : eol - pos);
                            if (!line.empty() && line[0] != '#') {
                                std::string path = uriToPath(line);
                                if (!path.empty() && onFileDrop_)
                                    onFileDrop_(path);
                                break;
                            }
                            if (eol == std::string::npos) break;
                            pos = list.find_first_not_of("\r\n", eol);
                        }
                    }
                    if (dndSource) {
                        XEvent fin{};
                        fin.xclient.type = ClientMessage;
                        fin.xclient.display = dpy;
                        fin.xclient.window = dndSource;
                        fin.xclient.message_type = aXdndFinished;
                        fin.xclient.format = 32;
                        fin.xclient.data.l[0] = (long)win;
                        fin.xclient.data.l[1] = 1;
                        fin.xclient.data.l[2] = (long)aXdndActionCopy;
                        XSendEvent(dpy, dndSource, False, NoEventMask, &fin);
                        dndSource = 0;
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // -------- ImGui frame --------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        io.DeltaTime = dt > 0.f ? dt : 1.f / 60.f;

        ImGui_ImplOpenGL2_NewFrame();
        ImGui::NewFrame();
        if (drawUI_) drawUI_();
        ImGui::Render();

        glViewport(0, 0, (int)width_.load(), (int)height_.load());
        glClearColor(0.09f, 0.09f, 0.10f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glXSwapBuffers(dpy, win);

        // ~60 fps
        auto frameEnd = std::chrono::steady_clock::now();
        auto spent = frameEnd - now;
        auto budget = std::chrono::microseconds(16600);
        if (spent < budget)
            std::this_thread::sleep_for(budget - spent);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui::DestroyContext(ctx);
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, glc);
    XDestroyWindow(dpy, win);
    XFreeColormap(dpy, cmap);
    XFree(vi);
    XCloseDisplay(dpy);
}
