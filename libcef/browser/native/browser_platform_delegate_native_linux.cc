// Copyright 2015 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libcef/browser/native/browser_platform_delegate_native_linux.h"

#include <sys/sysinfo.h>

#include "libcef/browser/browser_host_impl.h"
#include "libcef/browser/context.h"
#include "libcef/browser/native/menu_runner_linux.h"
#include "libcef/browser/native/window_delegate_view.h"
#include "libcef/browser/thread_util.h"

#include "base/no_destructor.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/native_web_keyboard_event.h"
#include "content/public/browser/render_view_host.h"
#include "third_party/blink/public/mojom/renderer_preferences.mojom.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion_x.h"
#include "ui/events/keycodes/keyboard_code_conversion_xkb.h"
#include "ui/events/keycodes/keysym_to_unicode.h"
#include "ui/gfx/font_render_params.h"
#include "ui/views/widget/widget.h"

#if defined(USE_X11)
#include "libcef/browser/native/window_x11.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host_x11.h"
#endif

namespace {

// Returns the number of seconds since system boot.
long GetSystemUptime() {
  struct sysinfo info;
  if (sysinfo(&info) == 0)
    return info.uptime;
  return 0;
}

}  // namespace

CefBrowserPlatformDelegateNativeLinux::CefBrowserPlatformDelegateNativeLinux(
    const CefWindowInfo& window_info,
    SkColor background_color,
    bool use_external_begin_frame)
    : CefBrowserPlatformDelegateNative(window_info,
                                       background_color,
                                       false,
                                       use_external_begin_frame),
      host_window_created_(false),
      window_widget_(nullptr) {}

void CefBrowserPlatformDelegateNativeLinux::BrowserDestroyed(
    CefBrowserHostImpl* browser) {
  CefBrowserPlatformDelegate::BrowserDestroyed(browser);

  if (host_window_created_) {
    // Release the reference added in CreateHostWindow().
    browser->Release();
  }
}

bool CefBrowserPlatformDelegateNativeLinux::CreateHostWindow() {
  DCHECK(!window_widget_);

  if (window_info_.width == 0)
    window_info_.width = 800;
  if (window_info_.height == 0)
    window_info_.height = 600;

  gfx::Rect rect(window_info_.x, window_info_.y, window_info_.width,
                 window_info_.height);

#if defined(USE_X11)
  DCHECK(!window_x11_);
  // Create a new window object. It will delete itself when the associated X11
  // window is destroyed.
  window_x11_ =
      new CefWindowX11(browser_, window_info_.parent_window, rect,
                       CefString(&window_info_.window_name).ToString());
  window_info_.window = window_x11_->xwindow();

  host_window_created_ = true;

  // Add a reference that will be released in BrowserDestroyed().
  browser_->AddRef();

  CefWindowDelegateView* delegate_view = new CefWindowDelegateView(
      GetBackgroundColor(), window_x11_->TopLevelAlwaysOnTop());
  delegate_view->Init(window_info_.window, browser_->web_contents(),
                      gfx::Rect(gfx::Point(), rect.size()));

  window_widget_ = delegate_view->GetWidget();
  window_widget_->Show();

  window_x11_->Show();
#endif  // defined(USE_X11)

  // As an additional requirement on Linux, we must set the colors for the
  // render widgets in webkit.
  blink::mojom::RendererPreferences* prefs =
      browser_->web_contents()->GetMutableRendererPrefs();
  prefs->focus_ring_color = SkColorSetARGB(255, 229, 151, 0);

  prefs->active_selection_bg_color = SkColorSetRGB(30, 144, 255);
  prefs->active_selection_fg_color = SK_ColorWHITE;
  prefs->inactive_selection_bg_color = SkColorSetRGB(200, 200, 200);
  prefs->inactive_selection_fg_color = SkColorSetRGB(50, 50, 50);

  // Set font-related attributes.
  static const base::NoDestructor<gfx::FontRenderParams> params(
      gfx::GetFontRenderParams(gfx::FontRenderParamsQuery(), nullptr));
  prefs->should_antialias_text = params->antialiasing;
  prefs->use_subpixel_positioning = params->subpixel_positioning;
  prefs->hinting = params->hinting;
  prefs->use_autohinter = params->autohinter;
  prefs->use_bitmaps = params->use_bitmaps;
  prefs->subpixel_rendering = params->subpixel_rendering;

  browser_->web_contents()->GetRenderViewHost()->SyncRendererPrefs();

  return true;
}

void CefBrowserPlatformDelegateNativeLinux::CloseHostWindow() {
#if defined(USE_X11)
  if (window_x11_)
    window_x11_->Close();
#endif
}

CefWindowHandle CefBrowserPlatformDelegateNativeLinux::GetHostWindowHandle()
    const {
  if (windowless_handler_)
    return windowless_handler_->GetParentWindowHandle();
  return window_info_.window;
}

views::Widget* CefBrowserPlatformDelegateNativeLinux::GetWindowWidget() const {
  return window_widget_;
}

void CefBrowserPlatformDelegateNativeLinux::SendFocusEvent(bool setFocus) {
  if (!setFocus) {
    if (window_x11_)
      window_x11_->Unfocus();
    return;
  }

  if (browser_->web_contents()) {
    // Give logical focus to the RenderWidgetHostViewAura in the views
    // hierarchy. This does not change the native keyboard focus.
    browser_->web_contents()->Focus();
  }

#if defined(USE_X11)
  if (window_x11_) {
    // Give native focus to the DesktopNativeWidgetAura for the root window.
    // Needs to be done via the ::Window so that keyboard focus is assigned
    // correctly.
    window_x11_->Focus();
  }
#endif  // defined(USE_X11)
}

void CefBrowserPlatformDelegateNativeLinux::NotifyMoveOrResizeStarted() {
  // Call the parent method to dismiss any existing popups.
  CefBrowserPlatformDelegate::NotifyMoveOrResizeStarted();

#if defined(USE_X11)
  if (!window_x11_)
    return;

  views::DesktopWindowTreeHostX11* tree_host = window_x11_->GetHost();
  if (!tree_host)
    return;

  // Explicitly set the screen bounds so that WindowTreeHost::*Screen()
  // methods return the correct results.
  const gfx::Rect& bounds = window_x11_->GetBoundsInScreen();
  tree_host->set_screen_bounds(bounds);

  // Send updated screen rectangle information to the renderer process so that
  // popups are displayed in the correct location.
  content::RenderWidgetHostImpl::From(
      browser_->web_contents()->GetRenderViewHost()->GetWidget())
      ->SendScreenRects();
#endif  // defined(USE_X11)
}

void CefBrowserPlatformDelegateNativeLinux::SizeTo(int width, int height) {
#if defined(USE_X11)
  if (window_x11_) {
    window_x11_->SetBounds(
        gfx::Rect(window_x11_->bounds().origin(), gfx::Size(width, height)));
  }
#endif  // defined(USE_X11)
}

gfx::Point CefBrowserPlatformDelegateNativeLinux::GetScreenPoint(
    const gfx::Point& view) const {
  if (windowless_handler_)
    return windowless_handler_->GetParentScreenPoint(view);

#if defined(USE_X11)
  if (!window_x11_)
    return view;

  // We can't use aura::Window::GetBoundsInScreen on Linux because it will
  // return bounds from DesktopWindowTreeHostX11 which in our case is relative
  // to the parent window instead of the root window (screen).
  const gfx::Rect& bounds_in_screen = window_x11_->GetBoundsInScreen();
  return gfx::Point(bounds_in_screen.x() + view.x(),
                    bounds_in_screen.y() + view.y());
#endif  // defined(USE_X11)
  return gfx::Point();
}

void CefBrowserPlatformDelegateNativeLinux::ViewText(const std::string& text) {
  char buff[] = "/tmp/CEFSourceXXXXXX";
  int fd = mkstemp(buff);

  if (fd == -1)
    return;

  FILE* srcOutput = fdopen(fd, "w+");
  if (!srcOutput)
    return;

  if (fputs(text.c_str(), srcOutput) < 0) {
    fclose(srcOutput);
    return;
  }

  fclose(srcOutput);

  std::string newName(buff);
  newName.append(".txt");
  if (rename(buff, newName.c_str()) != 0)
    return;

  std::string openCommand("xdg-open ");
  openCommand += newName;

  int result = system(openCommand.c_str());
  ALLOW_UNUSED_LOCAL(result);
}

bool CefBrowserPlatformDelegateNativeLinux::HandleKeyboardEvent(
    const content::NativeWebKeyboardEvent& event) {
  // TODO(cef): Is something required here to handle shortcut keys?
  return false;
}

// static
void CefBrowserPlatformDelegate::HandleExternalProtocol(const GURL& url) {}

void CefBrowserPlatformDelegateNativeLinux::TranslateKeyEvent(
    content::NativeWebKeyboardEvent& result,
    const CefKeyEvent& key_event) const {
  result.windows_key_code = key_event.windows_key_code;
  result.native_key_code = key_event.native_key_code;
  result.is_system_key = key_event.is_system_key ? 1 : 0;
  switch (key_event.type) {
    case KEYEVENT_RAWKEYDOWN:
    case KEYEVENT_KEYDOWN:
      result.SetType(blink::WebInputEvent::kRawKeyDown);
      break;
    case KEYEVENT_KEYUP:
      result.SetType(blink::WebInputEvent::kKeyUp);
      break;
    case KEYEVENT_CHAR:
      result.SetType(blink::WebInputEvent::kChar);
      break;
    default:
      NOTREACHED();
  }

#if defined(USE_X11)
  // Populate DOM values that will be passed to JavaScript handlers via
  // KeyboardEvent.
  result.dom_code = static_cast<int>(
      ui::KeycodeConverter::NativeKeycodeToDomCode(key_event.native_key_code));
  int keysym = ui::XKeysymForWindowsKeyCode(
      static_cast<ui::KeyboardCode>(key_event.windows_key_code),
      !!(key_event.modifiers & EVENTFLAG_SHIFT_DOWN));
  base::char16 ch = ui::GetUnicodeCharacterFromXKeySym(keysym);
  result.dom_key = static_cast<int>(ui::XKeySymToDomKey(keysym, ch));

  result.text[0] = key_event.character;
  result.unmodified_text[0] = key_event.unmodified_character;

  result.SetModifiers(result.GetModifiers() |
                      TranslateModifiers(key_event.modifiers));
#endif  // defined(USE_X11)
}

void CefBrowserPlatformDelegateNativeLinux::TranslateClickEvent(
    blink::WebMouseEvent& result,
    const CefMouseEvent& mouse_event,
    CefBrowserHost::MouseButtonType type,
    bool mouseUp,
    int clickCount) const {
  TranslateMouseEvent(result, mouse_event);

  switch (type) {
    case MBT_LEFT:
      result.SetType(mouseUp ? blink::WebInputEvent::kMouseUp
                             : blink::WebInputEvent::kMouseDown);
      result.button = blink::WebMouseEvent::Button::kLeft;
      break;
    case MBT_MIDDLE:
      result.SetType(mouseUp ? blink::WebInputEvent::kMouseUp
                             : blink::WebInputEvent::kMouseDown);
      result.button = blink::WebMouseEvent::Button::kMiddle;
      break;
    case MBT_RIGHT:
      result.SetType(mouseUp ? blink::WebInputEvent::kMouseUp
                             : blink::WebInputEvent::kMouseDown);
      result.button = blink::WebMouseEvent::Button::kRight;
      break;
    default:
      NOTREACHED();
  }

  result.click_count = clickCount;
}

void CefBrowserPlatformDelegateNativeLinux::TranslateMoveEvent(
    blink::WebMouseEvent& result,
    const CefMouseEvent& mouse_event,
    bool mouseLeave) const {
  TranslateMouseEvent(result, mouse_event);

  if (!mouseLeave) {
    result.SetType(blink::WebInputEvent::kMouseMove);
    if (mouse_event.modifiers & EVENTFLAG_LEFT_MOUSE_BUTTON)
      result.button = blink::WebMouseEvent::Button::kLeft;
    else if (mouse_event.modifiers & EVENTFLAG_MIDDLE_MOUSE_BUTTON)
      result.button = blink::WebMouseEvent::Button::kMiddle;
    else if (mouse_event.modifiers & EVENTFLAG_RIGHT_MOUSE_BUTTON)
      result.button = blink::WebMouseEvent::Button::kRight;
    else
      result.button = blink::WebMouseEvent::Button::kNoButton;
  } else {
    result.SetType(blink::WebInputEvent::kMouseLeave);
    result.button = blink::WebMouseEvent::Button::kNoButton;
  }

  result.click_count = 0;
}

void CefBrowserPlatformDelegateNativeLinux::TranslateWheelEvent(
    blink::WebMouseWheelEvent& result,
    const CefMouseEvent& mouse_event,
    int deltaX,
    int deltaY) const {
  result = blink::WebMouseWheelEvent();
  TranslateMouseEvent(result, mouse_event);

  result.SetType(blink::WebInputEvent::kMouseWheel);

  static const double scrollbarPixelsPerGtkTick = 40.0;
  result.delta_x = deltaX;
  result.delta_y = deltaY;
  result.wheel_ticks_x = deltaX / scrollbarPixelsPerGtkTick;
  result.wheel_ticks_y = deltaY / scrollbarPixelsPerGtkTick;
  result.has_precise_scrolling_deltas = true;

  if (mouse_event.modifiers & EVENTFLAG_LEFT_MOUSE_BUTTON)
    result.button = blink::WebMouseEvent::Button::kLeft;
  else if (mouse_event.modifiers & EVENTFLAG_MIDDLE_MOUSE_BUTTON)
    result.button = blink::WebMouseEvent::Button::kMiddle;
  else if (mouse_event.modifiers & EVENTFLAG_RIGHT_MOUSE_BUTTON)
    result.button = blink::WebMouseEvent::Button::kRight;
  else
    result.button = blink::WebMouseEvent::Button::kNoButton;
}

CefEventHandle CefBrowserPlatformDelegateNativeLinux::GetEventHandle(
    const content::NativeWebKeyboardEvent& event) const {
  if (!event.os_event)
    return NULL;
  return const_cast<CefEventHandle>(
      static_cast<CefEventHandle>(event.os_event->native_event()));
}

std::unique_ptr<CefMenuRunner>
CefBrowserPlatformDelegateNativeLinux::CreateMenuRunner() {
  return base::WrapUnique(new CefMenuRunnerLinux);
}

void CefBrowserPlatformDelegateNativeLinux::TranslateMouseEvent(
    blink::WebMouseEvent& result,
    const CefMouseEvent& mouse_event) const {
  // position
  result.SetPositionInWidget(mouse_event.x, mouse_event.y);

  const gfx::Point& screen_pt =
      GetScreenPoint(gfx::Point(mouse_event.x, mouse_event.y));
  result.SetPositionInScreen(screen_pt.x(), screen_pt.y());

  // modifiers
  result.SetModifiers(result.GetModifiers() |
                      TranslateModifiers(mouse_event.modifiers));

  // timestamp
  result.SetTimeStamp(base::TimeTicks() +
                      base::TimeDelta::FromSeconds(GetSystemUptime()));

  result.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
}
