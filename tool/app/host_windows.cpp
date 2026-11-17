// Windows host: Tela's normal top-level window (tela::WindowsView) drawn with
// Tela's Pictor surface. Tela feeds button presses to the runtime itself; this
// host subclasses the window to also observe the pointer positions, the wheel
// and keyboard shortcuts for the drawn controls (pointer_router.hpp), after
// Tela has handled each message (Tela owns the gesture).
//
// The only Sonus file besides macOS's that is compiled with exceptions: Tela's
// constructors and synchronize() throw, and everything is caught here.
//
// Shortcuts: Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo, F2 mixer, F3 profiler,
// Esc closes (Tela), F11 full screen (Tela).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <exception>

#include <tela/pictor_surface.hpp>
#include <tela/runtime.hpp>
#include <tela/windows_view.hpp>

#include "host.hpp"

namespace snd::tool::app {
namespace {

struct Subclass {
  ui::App* app = nullptr;
  WNDPROC previous = nullptr;
  std::uint64_t sequence = 0, gesture = 0;
};
Subclass g_subclass;  // one tool window per process

std::uint32_t Modifiers(WPARAM keys) {
  std::uint32_t m = 0;
  if ((keys & MK_SHIFT) != 0u) m |= ui::kModShift;
  if ((keys & MK_CONTROL) != 0u) m |= ui::kModControl;
  return m;
}

void Observe(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  ui::App& app = *g_subclass.app;
  tela::HostPointerEvent e;
  e.sequence = ++g_subclass.sequence;
  e.viewport_revision = app.runtime().viewport().revision;
  POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
      ClientToScreen(window, &p);  // client coordinates -> desktop physical pixels (as Tela)
      if (message == WM_LBUTTONDOWN) ++g_subclass.gesture;
      e.phase = message == WM_LBUTTONDOWN ? tela::PointerPhase::down
                : message == WM_LBUTTONUP ? tela::PointerPhase::up
                                          : tela::PointerPhase::move;
      e.button = message == WM_MOUSEMOVE ? tela::PointerButton::none : tela::PointerButton::primary;
      e.modifiers = Modifiers(wp);
      break;
    case WM_MOUSEWHEEL:  // already in screen coordinates
      e.phase = tela::PointerPhase::wheel;
      e.wheel_y = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
      e.modifiers = Modifiers(GET_KEYSTATE_WPARAM(wp));
      break;
    default: return;
  }
  e.gesture_id = g_subclass.gesture;
  e.desktop_x = p.x;
  e.desktop_y = p.y;
  app.Pointer(e, GetTickCount64(), false);
}

void Key(WPARAM key) {
  ui::App& app = *g_subclass.app;
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (ctrl && key == 'Z') {
    if (shift) {
      app.OnRedo();
    } else {
      app.OnUndo();
    }
  } else if (ctrl && key == 'Y') {
    app.OnRedo();
  } else if (key == VK_F2) {
    app.OnScreen(ui::Screen::Mixer);
  } else if (key == VK_F3) {
    app.OnScreen(ui::Screen::Profiler);
  }
}

LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  // Tela first: it decides who owns a press before the router looks at it.
  const LRESULT r = CallWindowProcW(g_subclass.previous, window, message, wp, lp);
  if (g_subclass.app == nullptr) return r;
  switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
      Observe(window, message, wp, lp);
      break;
    case WM_MOUSEWHEEL:
      Observe(window, message, wp, lp);
      return 0;
    case WM_KEYDOWN:
      Key(wp);
      break;
    default: break;
  }
  return r;
}

}  // namespace

int RunWindows(ui::App& app, const Options& o) {
  if (o.font.empty()) {
    std::fprintf(stderr, "sonus-tool: --font <file.ttf> is required (Tela does not bundle a font)\n");
    return 2;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    tela::PictorSurface surface(o.font);
    tela::WindowsView view(app.runtime(), surface, "Sonus Tool", o.width, o.height, false);
    HWND window = reinterpret_cast<HWND>(view.native_window());
    g_subclass.app = &app;
    g_subclass.previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Procedure)));
    while (view.pump()) {
      app.Frame(NowNs());
      view.synchronize();
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    g_subclass.app = nullptr;
    return 0;
  } catch (const std::exception& e) {
    g_subclass.app = nullptr;
    std::fprintf(stderr, "sonus-tool: %s\n", e.what());
    return 1;
  } catch (...) {
    g_subclass.app = nullptr;
    std::fprintf(stderr, "sonus-tool: unknown error from the Tela host\n");
    return 1;
  }
}

}  // namespace snd::tool::app
