// doa4 - choice of renderer (see renderer_select.h).

#include "renderer_select.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#if HAJINMON_PLUME_RENDERER
#include "plume_graphics_system.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif
REXCVAR_DECLARE(bool, plume_precompile_shaders);
#endif

REXCVAR_DEFINE_STRING(renderer, "xenia", "GPU",
                      "Renderer: xenia (Xenos emulation) or plume (experimental native renderer)")
    .allowed({"xenia", "plume"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace doa4 {

#if HAJINMON_PLUME_RENDERER && defined(_WIN32)
namespace {

// The shader precompilation (started by the launcher) needs no window, but the
// SDK always opens one: right before it is shown, the game window is made
// invisible (layered, never given an opacity), without a taskbar button and
// without taking the focus.
LRESULT CALLBACK HideWindowHook(int code, WPARAM wparam, LPARAM lparam) {
  const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
  if (code == HC_ACTION && message->message == WM_SHOWWINDOW && message->wParam &&
      !(GetWindowLongW(message->hwnd, GWL_STYLE) & WS_CHILD)) {
    SetWindowLongW(message->hwnd, GWL_EXSTYLE,
                   GetWindowLongW(message->hwnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                       WS_EX_NOACTIVATE);
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

}  // namespace
#endif

std::unique_ptr<rex::system::IGraphicsSystem> CreateSelectedGraphicsSystem() {
  if (REXCVAR_GET(renderer) != "plume") {
    return nullptr;
  }
#if HAJINMON_PLUME_RENDERER
#if defined(_WIN32)
  // Called on the UI thread, before the window is created.
  if (REXCVAR_GET(plume_precompile_shaders)) {
    SetWindowsHookExW(WH_CALLWNDPROC, HideWindowHook, nullptr, GetCurrentThreadId());
  }
#endif
  return std::make_unique<hajinmon::renderer::PlumeGraphicsSystem>();
#else
  REXLOG_ERROR("doa4: this build has no native renderer, using Xenos emulation");
  return nullptr;
#endif
}

void AttachWindowToRenderer(rex::system::IGraphicsSystem* graphics, rex::ui::Window* window) {
#if HAJINMON_PLUME_RENDERER
  auto* plume = dynamic_cast<hajinmon::renderer::PlumeGraphicsSystem*>(graphics);
  if (plume && window) {
#if defined(_WIN32)
    plume->AttachWindow(window->GetNativeWindowHandle());
#else
    // The SDK gives no native handle outside Windows: the renderer takes the
    // game's SDL window (its only window) and finds the X11 window from it.
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows && count > 0) {
      plume->AttachWindow(windows[0]);
    } else {
      REXLOG_ERROR("doa4: no SDL window for the native renderer");
    }
    SDL_free(windows);
#endif
    // The SDK's F3 debug overlay needs its presenter, which the native renderer
    // replaces: F3 takes screenshots instead.
    rex::ui::UnregisterBind("bind_debug_overlay");
    rex::ui::RegisterBind("bind_screenshot", "F3", "Save a screenshot", [plume] {
      plume->renderer().RequestScreenshot();
    });
  }
#else
  (void)graphics;
  (void)window;
#endif
}

}  // namespace doa4
