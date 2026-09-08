/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_PERFORMANCE_DIALOG_H_
#define XENIA_UI_IMGUI_PERFORMANCE_DIALOG_H_

#include <functional>
#include <string>

#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace app {
class EmulatorWindow;
}  // namespace app
}  // namespace xe

namespace xe {
namespace ui {

// ImGui-based performance tuning dialog with gamepad support.
class ImGuiPerformanceDialog : public ImGuiGamepadDialog {
 public:
  ImGuiPerformanceDialog(ImGuiDrawer* drawer,
                         app::EmulatorWindow* emulator_window,
                         hid::InputSystem* input_system);

  void CloseDialog() { Close(); }

  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  void LoadCurrentSettings();
  void ShowNotification(const std::string& title,
                        const std::string& description);

  // Setting change handlers
  void OnReadbackResolveChanged(int value);
  void OnReadbackResolveSyncChanged(bool enabled);
  void OnMemexportEnableChanged(bool enabled);
  void OnMemexportAwaitFencesChanged(bool enabled);
  void OnOcclusionQueryChanged(int value);
  void OnEmulatedDisplayUncappedChanged(bool uncapped);
  void OnFramerateLimitChanged(int value);

  app::EmulatorWindow* emulator_window_;
  std::function<void()> on_close_callback_;

  // Current settings state (selected values)
  int readback_resolve_mode_ = 1;  // 0=None, 1=Fast, 2=All
  bool readback_resolve_sync_ = true;
  bool memexport_enable_ = true;
  bool memexport_await_fences_ = true;
  int occlusion_query_mode_ = 0;  // 0=Fake, 1=Fast, 2=Fast-Alt, 3=Strict
  bool display_uncapped_ = false;
  int framerate_limit_ = 0;  // FPS, 0 = unlimited

  // Highlight positions for navigation
  int resolve_highlight_ = 1;
  int occlusion_query_highlight_ = 0;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_PERFORMANCE_DIALOG_H_
