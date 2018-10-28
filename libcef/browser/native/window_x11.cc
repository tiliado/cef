// Copyright 2014 The Chromium Embedded Framework Authors.
// Portions copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libcef/browser/native/window_x11.h"
#include "libcef/browser/thread_util.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>

#include "ui/base/x/x11_util.h"
#include "ui/events/platform/platform_event_source.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host_x11.h"
#include "ui/views/widget/desktop_aura/x11_topmost_window_finder.h"

namespace {

const char kAtom[] = "ATOM";
const char kWMDeleteWindow[] = "WM_DELETE_WINDOW";
const char kWMProtocols[] = "WM_PROTOCOLS";
const char kNetWMPid[] = "_NET_WM_PID";
const char kNetWMPing[] = "_NET_WM_PING";
const char kNetWMState[] = "_NET_WM_STATE";
const char kXdndProxy[] = "XdndProxy";

::Window FindEventTarget(const ui::PlatformEvent& xev) {
  ::Window target = xev->xany.window;
  if (xev->type == GenericEvent)
    target = static_cast<XIDeviceEvent*>(xev->xcookie.data)->event;
  return target;
}

::Window FindChild(::Display* display, ::Window window) {
  ::Window root;
  ::Window parent;
  ::Window* children;
  ::Window child_window = x11::None;
  unsigned int nchildren;
  if (XQueryTree(display, window, &root, &parent, &children, &nchildren)) {
    DCHECK_EQ(1U, nchildren);
    child_window = children[0];
    XFree(children);
  }
  return child_window;
}

::Window FindToplevelParent(::Display* display, ::Window window) {
  ::Window top_level_window = window;
  ::Window root = x11::None;
  ::Window parent = x11::None;
  ::Window* children = NULL;
  unsigned int nchildren = 0;
  // Enumerate all parents of "window" to find the highest level window
  // that has a parent that is the root window.
  while (XQueryTree(display, window, &root, &parent, &children, &nchildren)) {
    if (children) {
      XFree(children);
    }

    top_level_window = window;
    if (parent == root) {
      break;
    }
    window = parent;
  }
  return top_level_window;
}

bool IsParentOfChildWindow(::Display* display, ::Window parent, ::Window child) {
  if (parent == child)
    return false;
  ::Window root = x11::None;
  ::Window real_parent = x11::None;
  ::Window* children = NULL;
  unsigned int nchildren = 0;
  while (XQueryTree(display, child, &root, &real_parent, &children, &nchildren)) {
    if (children) {
      XFree(children);
    }
    if (real_parent == root) {
      return real_parent == parent;
    }
    if (real_parent == parent)
        return true;
    child = real_parent;
  }
  return false;
}

}  // namespace

CEF_EXPORT XDisplay* cef_get_xdisplay() {
  if (!CEF_CURRENTLY_ON(CEF_UIT))
    return NULL;
  return gfx::GetXDisplay();
}

CefWindowX11::CefWindowX11(CefRefPtr<CefBrowserHostImpl> browser,
                           ::Window parent_xwindow,
                           const gfx::Rect& bounds)
    : browser_(browser),
      xdisplay_(gfx::GetXDisplay()),
      parent_xwindow_(parent_xwindow),
      xwindow_(0),
      previously_focused_(0),
      window_mapped_(false),
      bounds_(bounds),
      focus_pending_(false),
      weak_ptr_factory_(this) {
  if (parent_xwindow_ == x11::None)
    parent_xwindow_ = DefaultRootWindow(xdisplay_);

  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof(swa));
  swa.background_pixmap = x11::None;
  swa.override_redirect = false;
  xwindow_ = XCreateWindow(xdisplay_, parent_xwindow_, bounds.x(), bounds.y(),
                           bounds.width(), bounds.height(),
                           0,               // border width
                           CopyFromParent,  // depth
                           InputOutput,
                           CopyFromParent,  // visual
                           CWBackPixmap | CWOverrideRedirect, &swa);
  CHECK(xwindow_);

  if (ui::PlatformEventSource::GetInstance())
    ui::PlatformEventSource::GetInstance()->AddPlatformEventDispatcher(this);

  long event_mask = FocusChangeMask | StructureNotifyMask | PropertyChangeMask;
  XSelectInput(xdisplay_, xwindow_, event_mask);
  XFlush(xdisplay_);

  // TODO(erg): We currently only request window deletion events. We also
  // should listen for activation events and anything else that GTK+ listens
  // for, and do something useful.
  ::Atom protocols[2];
  protocols[0] = gfx::GetAtom(kWMDeleteWindow);
  protocols[1] = gfx::GetAtom(kNetWMPing);
  XSetWMProtocols(xdisplay_, xwindow_, protocols, 2);

  // We need a WM_CLIENT_MACHINE and WM_LOCALE_NAME value so we integrate with
  // the desktop environment.
  XSetWMProperties(xdisplay_, xwindow_, NULL, NULL, NULL, 0, NULL, NULL, NULL);

  // Likewise, the X server needs to know this window's pid so it knows which
  // program to kill if the window hangs.
  // XChangeProperty() expects "pid" to be long.
  static_assert(sizeof(long) >= sizeof(pid_t),
                "pid_t should not be larger than long");
  long pid = getpid();
  XChangeProperty(xdisplay_, xwindow_, gfx::GetAtom(kNetWMPid), XA_CARDINAL, 32,
                  PropModeReplace, reinterpret_cast<unsigned char*>(&pid), 1);
}

CefWindowX11::~CefWindowX11() {
  DCHECK(!xwindow_);
  if (ui::PlatformEventSource::GetInstance())
    ui::PlatformEventSource::GetInstance()->RemovePlatformEventDispatcher(this);
}

void CefWindowX11::Close() {
  XEvent ev = {0};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = xwindow_;
  ev.xclient.message_type = gfx::GetAtom(kWMProtocols);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = gfx::GetAtom(kWMDeleteWindow);
  ev.xclient.data.l[1] = x11::CurrentTime;
  XSendEvent(xdisplay_, xwindow_, false, NoEventMask, &ev);
}

void CefWindowX11::Show() {
  if (xwindow_ == x11::None)
    return;

  if (!window_mapped_) {
    // Before we map the window, set size hints. Otherwise, some window managers
    // will ignore toplevel XMoveWindow commands.
    XSizeHints size_hints;
    size_hints.flags = PPosition | PWinGravity;
    size_hints.x = bounds_.x();
    size_hints.y = bounds_.y();
    // Set StaticGravity so that the window position is not affected by the
    // frame width when running with window manager.
    size_hints.win_gravity = StaticGravity;
    XSetWMNormalHints(xdisplay_, xwindow_, &size_hints);

    XMapWindow(xdisplay_, xwindow_);

    // TODO(thomasanderson): Find out why this flush is necessary.
    XFlush(xdisplay_);
    window_mapped_ = true;

    // Setup the drag and drop proxy on the top level window of the application
    // to be the child of this window.
    ::Window child = FindChild(xdisplay_, xwindow_);
    ::Window toplevel_window = FindToplevelParent(xdisplay_, xwindow_);
    DCHECK(toplevel_window);
    if (child && toplevel_window) {
      // Configure the drag&drop proxy property for the top-most window so
      // that all drag&drop-related messages will be sent to the child
      // DesktopWindowTreeHostX11. The proxy property is referenced by
      // DesktopDragDropClientAuraX11::FindWindowFor.
      ::Window proxy_target = gfx::kNullAcceleratedWidget;
      ui::GetXIDProperty(toplevel_window, kXdndProxy, &proxy_target);

      if (proxy_target != child) {
        // Set the proxy target for the top-most window.
        XChangeProperty(xdisplay_, toplevel_window, gfx::GetAtom(kXdndProxy),
                        XA_WINDOW, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&child), 1);
        // Do the same for the proxy target per the spec.
        XChangeProperty(xdisplay_, child, gfx::GetAtom(kXdndProxy), XA_WINDOW,
                        32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&child), 1);
      }
    }
  }
}

void CefWindowX11::Hide() {
  if (xwindow_ == x11::None)
    return;

  if (window_mapped_) {
    XWithdrawWindow(xdisplay_, xwindow_, 0);
    window_mapped_ = false;
  }
}

void CefWindowX11::Focus() {
  if (xwindow_ == x11::None || !window_mapped_)
    return;

  ::Window focused = x11::None;
  int revert_to = 0;
  XGetInputFocus(xdisplay_, &focused, &revert_to);
  
  if (browser_.get()) {
    ::Window child = FindChild(xdisplay_, xwindow_);
    if (child && focused != child && ui::IsWindowVisible(child)) {
      // Give focus to the child DesktopWindowTreeHostX11.
      XSetInputFocus(xdisplay_, child, RevertToParent, x11::CurrentTime);
      if (focused != xwindow_) {
          // Store the focused window to restore the original state precisely.
          previously_focused_ = focused;  
      }
    }
  } else if (focused != xwindow_) {
    XSetInputFocus(xdisplay_, xwindow_, RevertToParent, x11::CurrentTime);
    // Store the focused window to restore the original state precisely.
    previously_focused_ = focused;
  }
}

void CefWindowX11::Unfocus() {
  if (xwindow_ == x11::None || !window_mapped_)
    return;
  
  ::Window focused = x11::None;
  int revert_to = 0;
  XGetInputFocus(xdisplay_, &focused, &revert_to);
  if (!focused)
    return;

  ::Window toplevel = FindToplevelParent(xdisplay_, xwindow_);
  if (toplevel == xwindow_)
    return;
  
  ::Window child = x11::None;
  if (browser_.get()) {
    child = FindChild(xdisplay_, xwindow_);
  }
  if (focused == xwindow_ || focused == child) {
    // Our window or child window  still has keyboard focus. Return it back to
    // the original window so that GUI toolkits can receive keyboard events again.
    if (previously_focused_ && IsParentOfChildWindow(xdisplay_, toplevel, previously_focused_)) {
      // GTK+ may have a special "focus window" for keyboard events. It must be a child of the toplevel though.
      XSetInputFocus(xdisplay_, previously_focused_, RevertToParent, x11::CurrentTime);
    } else {
      // Otherwise, the tolevel window is the best focus candidate we have.
      XSetInputFocus(xdisplay_, toplevel, RevertToParent, x11::CurrentTime);
    }
  }
}

void CefWindowX11::SetBounds(const gfx::Rect& bounds) {
  if (xwindow_ == x11::None)
    return;

  bool origin_changed = bounds_.origin() != bounds.origin();
  bool size_changed = bounds_.size() != bounds.size();
  XWindowChanges changes = {0};
  unsigned value_mask = 0;

  if (size_changed) {
    changes.width = bounds.width();
    changes.height = bounds.height();
    value_mask = CWHeight | CWWidth;
  }

  if (origin_changed) {
    changes.x = bounds.x();
    changes.y = bounds.y();
    value_mask |= CWX | CWY;
  }

  if (value_mask)
    XConfigureWindow(xdisplay_, xwindow_, value_mask, &changes);
}

gfx::Rect CefWindowX11::GetBoundsInScreen() {
  int x, y;
  Window child;
  if (XTranslateCoordinates(xdisplay_, xwindow_, DefaultRootWindow(xdisplay_),
                            0, 0, &x, &y, &child)) {
    return gfx::Rect(gfx::Point(x, y), bounds_.size());
  }
  return gfx::Rect();
}

views::DesktopWindowTreeHostX11* CefWindowX11::GetHost() {
  if (browser_.get()) {
    ::Window child = FindChild(xdisplay_, xwindow_);
    if (child)
      return views::DesktopWindowTreeHostX11::GetHostForXID(child);
  }
  return NULL;
}

bool CefWindowX11::CanDispatchEvent(const ui::PlatformEvent& event) {
  ::Window target = FindEventTarget(event);
  return target == xwindow_;
}

uint32_t CefWindowX11::DispatchEvent(const ui::PlatformEvent& event) {
  XEvent* xev = event;
  switch (xev->type) {
    case ConfigureNotify: {
      DCHECK_EQ(xwindow_, xev->xconfigure.event);
      DCHECK_EQ(xwindow_, xev->xconfigure.window);
      // It's possible that the X window may be resized by some other means
      // than from within Aura (e.g. the X window manager can change the
      // size). Make sure the root window size is maintained properly.
      gfx::Rect bounds(xev->xconfigure.x, xev->xconfigure.y,
                       xev->xconfigure.width, xev->xconfigure.height);
      bounds_ = bounds;

      if (browser_.get()) {
        ::Window child = FindChild(xdisplay_, xwindow_);
        if (child) {
          // Resize the child DesktopWindowTreeHostX11 to match this window.
          XWindowChanges changes = {0};
          changes.width = bounds.width();
          changes.height = bounds.height();
          XConfigureWindow(xdisplay_, child, CWHeight | CWWidth, &changes);

          browser_->NotifyMoveOrResizeStarted();
        }
      }
      break;
    }
    case ClientMessage: {
      Atom message_type = xev->xclient.message_type;
      if (message_type == gfx::GetAtom(kWMProtocols)) {
        Atom protocol = static_cast<Atom>(xev->xclient.data.l[0]);
        if (protocol == gfx::GetAtom(kWMDeleteWindow)) {
          // We have received a close message from the window manager.
          if (!browser_ || browser_->TryCloseBrowser()) {
            // Allow the close.
            XDestroyWindow(xdisplay_, xwindow_);

            xwindow_ = x11::None;

            if (browser_.get()) {
              // Force the browser to be destroyed and release the reference
              // added in PlatformCreateWindow().
              browser_->WindowDestroyed();
            }

            delete this;
          }
        } else if (protocol == gfx::GetAtom(kNetWMPing)) {
          XEvent reply_event = *xev;
          reply_event.xclient.window = parent_xwindow_;

          XSendEvent(xdisplay_, reply_event.xclient.window, false,
                     SubstructureRedirectMask | SubstructureNotifyMask,
                     &reply_event);
          XFlush(xdisplay_);
        }
      }
      break;
    }
    case x11::FocusIn:
      // This message is received first followed by a "_NET_ACTIVE_WINDOW"
      // message sent to the root window. When X11DesktopHandler handles the
      // "_NET_ACTIVE_WINDOW" message it will erroneously mark the WebView
      // (hosted in a DesktopWindowTreeHostX11) as unfocused. Use a delayed
      // task here to restore the WebView's focus state.
      if (!focus_pending_) {
        focus_pending_ = true;
        CEF_POST_DELAYED_TASK(CEF_UIT,
                              base::Bind(&CefWindowX11::ContinueFocus,
                                         weak_ptr_factory_.GetWeakPtr()),
                              100);
      }
      break;
    case x11::FocusOut:
      // Cancel the pending focus change if some other window has gained focus
      // while waiting for the async task to run. Otherwise we can get stuck in
      // a focus change loop.
      if (focus_pending_)
        focus_pending_ = false;
      break;
    case PropertyNotify: {
      ::Atom changed_atom = xev->xproperty.atom;
      if (changed_atom == gfx::GetAtom(kNetWMState)) {
        // State change event like minimize/maximize.
        if (browser_.get()) {
          ::Window child = FindChild(xdisplay_, xwindow_);
          if (child) {
            // Forward the state change to the child DesktopWindowTreeHostX11
            // window so that resource usage will be reduced while the window is
            // minimized.
            std::vector<::Atom> atom_list;
            if (ui::GetAtomArrayProperty(xwindow_, kNetWMState, &atom_list) &&
                !atom_list.empty()) {
              ui::SetAtomArrayProperty(child, kNetWMState, "ATOM", atom_list);
            } else {
              // Set an empty list of property values to pass the check in
              // DesktopWindowTreeHostX11::OnWMStateUpdated().
              XChangeProperty(xdisplay_, child,
                              gfx::GetAtom(kNetWMState),  // name
                              gfx::GetAtom(kAtom),        // type
                              32,  // size in bits of items in 'value'
                              PropModeReplace, NULL,
                              0);  // num items
            }
          }
        }
      }
      break;
    }
  }

  return ui::POST_DISPATCH_STOP_PROPAGATION;
}

void CefWindowX11::ContinueFocus() {
  if (!focus_pending_)
    return;
  if (browser_.get())
    browser_->SetFocus(true);
  focus_pending_ = false;
}
