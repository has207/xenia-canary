/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_GNULINUX_H_
#define XENIA_UI_SURFACE_GNULINUX_H_

#include <wayland-client.h>
#include <xcb/xcb.h>
#include <memory>

#include "xenia/ui/surface.h"

namespace xe {
namespace ui {

class XcbWindowSurface final : public Surface {
 public:
  // Takes ownership of `connection`, which GtkSurfaceFactory::Create opens
  // exclusively for this surface - see the comment there.
  explicit XcbWindowSurface(xcb_connection_t* connection, xcb_window_t window)
      : connection_(connection), window_(window) {}
  ~XcbWindowSurface() override;
  TypeIndex GetType() const override { return kTypeIndex_XcbWindow; }
  xcb_connection_t* connection() const { return connection_; }
  xcb_window_t window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  xcb_connection_t* connection_;
  xcb_window_t window_;
};

class GtkSurfaceFactory;

class WaylandWindowSurface final : public Surface {
 public:
  explicit WaylandWindowSurface(wl_display* display, wl_surface* surface,
                                uint32_t width, uint32_t height)
      : display_(display), surface_(surface), width_(width), height_(height) {}
  ~WaylandWindowSurface() override;
  TypeIndex GetType() const override { return kTypeIndex_WaylandWindow; }
  wl_display* display() const { return display_; }
  wl_surface* surface() const { return surface_; }

  void SetSize(uint32_t width, uint32_t height) override {
    width_ = width;
    height_ = height;
  }

  // Two-way back-pointer to the factory so the GTK size-allocate signal can
  // push physical-pixel updates here. Nulled by either side's destructor to
  // tolerate either destruction order.
  void set_factory(GtkSurfaceFactory* f) { factory_ = f; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  wl_display* display_;
  wl_surface* surface_;
  uint32_t width_;
  uint32_t height_;
  GtkSurfaceFactory* factory_ = nullptr;
};

// Creates the appropriate Surface (XCB or Wayland-subsurface) for a GTK
// widget. Owns any Wayland resources that need to outlive the call.
class GtkSurfaceFactory {
 public:
  GtkSurfaceFactory() = default;
  ~GtkSurfaceFactory();
  GtkSurfaceFactory(const GtkSurfaceFactory&) = delete;
  GtkSurfaceFactory& operator=(const GtkSurfaceFactory&) = delete;

  // gtk_widget is a GtkWidget*. Caller must have realized the widget already.
  std::unique_ptr<Surface> Create(void* gtk_widget,
                                  Surface::TypeFlags allowed_types);
  void OnResize(void* gtk_widget);

  void set_wayland_surface(WaylandWindowSurface* s) { wayland_surface_ = s; }

 private:
  wl_surface* wl_render_surface_ = nullptr;
  wl_subsurface* wl_render_subsurface_ = nullptr;
  void* size_allocate_widget_ = nullptr;
  unsigned long size_allocate_handler_id_ = 0;
  WaylandWindowSurface* wayland_surface_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_LINUX_H_
