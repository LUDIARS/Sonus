// Windows host: Tela's normal top-level window (tela::WindowsView) drawn with
// Tela's Pictor surface. Tela feeds button presses to the runtime itself; this
// host subclasses the window to also observe the pointer positions, the wheel
// and keyboard shortcuts for the drawn controls (pointer_router.hpp), after
// Tela has handled each message (Tela owns the gesture).
//
// The only Sonus file besides macOS's that is compiled with exceptions: Tela's
// constructors and synchronize() throw, and everything is caught here.
//
// Keys go to App::Key: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, Ctrl+S, F2..F10 screens, Ctrl+PageUp /
// PageDown, and the tool's text entry (WM_CHAR). Tela sees every message first (F11 full screen),
// except Esc: the tool takes it and Tela never closes the window with it.
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

std::uint32_t KeyModifiers() {
  std::uint32_t m = 0;
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) m |= ui::kModControl;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) m |= ui::kModShift;
  return m;
}

// WM_KEYDOWN: the key (App::Key: shortcuts, screens, the text entry's Enter / Escape / Backspace).
void Key(WPARAM key) {
  ui::KeyEvent k;
  k.modifiers = KeyModifiers();
  if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
    k.key = static_cast<std::uint32_t>(key);
  } else if (key >= VK_F1 && key <= VK_F12) {
    k.key = ui::kKeyF1 + static_cast<std::uint32_t>(key - VK_F1);
  } else {
    switch (key) {
      case VK_RETURN: k.key = ui::kKeyEnter; break;
      case VK_ESCAPE: k.key = ui::kKeyEscape; break;
      case VK_BACK: k.key = ui::kKeyBackspace; break;
      case VK_DELETE: k.key = ui::kKeyDelete; break;
      case VK_TAB: k.key = ui::kKeyTab; break;
      case VK_LEFT: k.key = ui::kKeyLeft; break;
      case VK_RIGHT: k.key = ui::kKeyRight; break;
      case VK_UP: k.key = ui::kKeyUp; break;
      case VK_DOWN: k.key = ui::kKeyDown; break;
      case VK_HOME: k.key = ui::kKeyHome; break;
      case VK_END: k.key = ui::kKeyEnd; break;
      case VK_PRIOR: k.key = ui::kKeyPageUp; break;
      case VK_NEXT: k.key = ui::kKeyPageDown; break;
      default: return;
    }
  }
  g_subclass.app->Key(k);
}

// WM_CHAR: the typed character (UTF-16; a surrogate pair arrives as two messages).
void Char(WPARAM ch) {
  static std::uint32_t high = 0;
  std::uint32_t c = static_cast<std::uint32_t>(ch);
  if (c >= 0xD800u && c <= 0xDBFFu) {
    high = c;
    return;
  }
  if (c >= 0xDC00u && c <= 0xDFFFu) {
    if (high == 0u) return;
    c = 0x10000u + ((high - 0xD800u) << 10) + (c - 0xDC00u);
  }
  high = 0;
  if (c < 0x20u || c == 0x7Fu) return;  // control characters come as keys
  ui::KeyEvent k;
  k.text = c;
  k.modifiers = KeyModifiers() & ~ui::kModControl;  // AltGr reports Ctrl: a typed character is text
  g_subclass.app->Key(k);
}

LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  // Esc is the tool's (D-186: it cancels a text entry, a list, a drag or a pending connection, else
  // nothing); Tela would close the window, so it never gets it.
  if (message == WM_KEYDOWN && wp == VK_ESCAPE && g_subclass.app != nullptr) {
    Key(wp);
    return 0;
  }
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
    case WM_CHAR:
      Char(wp);
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
