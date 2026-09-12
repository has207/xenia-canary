/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/trace_dump.h"

#include <cstdio>

#include "third_party/stb/stb_image_write.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/memory.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/xbox.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#undef _CRT_SECURE_NO_WARNINGS
#undef _CRT_NONSTDC_NO_DEPRECATE
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

DEFINE_path(target_trace_file, "", "Specifies the trace file to load.",
            "GPU.Debug");
DEFINE_path(trace_dump_path, "", "Output path for dumped files.", "GPU.Debug");

DEFINE_uint32(trace_dump_interval, 0,
              "Also dump an image every N replayed frames, named with the "
              "frame index. 0 dumps only the final frame.",
              "GPU.Debug");

DEFINE_uint32(trace_dump_from, 0,
              "First frame index trace_dump_interval applies to.", "GPU.Debug");

DEFINE_uint32(trace_dump_to, 0,
              "Last frame index trace_dump_interval applies to. 0 means "
              "no upper bound.",
              "GPU.Debug");

DEFINE_int32(trace_dump_stop_command, -1,
             "Stop the frame named by trace_dump_edram_frame after this "
             "command index instead of playing it whole. -1 plays it whole.",
             "GPU.Debug");

DEFINE_int32(trace_dump_edram_frame, -1,
             "Write the EDRAM contents to <output>_edram.bin once this "
             "replayed frame has played, honouring trace_dump_stop_command. "
             "-1 disables.",
             "GPU.Debug");

DEFINE_int32(trace_dump_trace_frame, -1,
             "Write a single-frame trace of this replayed frame, which "
             "carries an EDRAM snapshot taken before it. -1 disables.",
             "GPU.Debug");

DEFINE_int32(trace_dump_capture_frame, -1,
             "Wrap the RenderDoc host capture around just this frame "
             "instead of the whole replay. -1 keeps the whole replay.",
             "GPU.Debug");

DEFINE_int32(trace_dump_memory_frame, -1,
             "After replaying this frame, write guest physical memory to "
             "<output>_mem.bin for byte comparison. -1 disables.",
             "GPU.Debug");

DEFINE_uint32(trace_dump_memory_size, 0x20000000,
              "Bytes of guest physical memory to write out.", "GPU.Debug");

DEFINE_uint32(trace_dump_memory_base, 0,
              "Guest physical address the memory dump starts at.", "GPU.Debug");

DEFINE_bool(trace_dump_memory_series, false,
            "Also write guest memory alongside every image dumped by "
            "trace_dump_interval.",
            "GPU.Debug");

namespace xe {
namespace gpu {

using namespace xe::gpu::xenos;

TraceDump::TraceDump() = default;

TraceDump::~TraceDump() = default;

int TraceDump::Main(const std::vector<std::string>& args) {
  // Grab path from the flag or unnamed argument.
  std::filesystem::path path;
  std::filesystem::path output_path;
  if (!cvars::target_trace_file.empty()) {
    // Passed as a named argument.
    // TODO(benvanik): find something better than gflags that supports
    // unicode.
    path = cvars::target_trace_file;
  } else if (args.size() >= 2) {
    // Passed as an unnamed argument.
    path = xe::to_path(args[1]);

    if (args.size() >= 3) {
      output_path = xe::to_path(args[2]);
    }
  }

  if (path.empty()) {
    XELOGE("No trace file specified");
    return 5;
  }

  // Normalize the path and make absolute.
  auto abs_path = std::filesystem::absolute(path);
  XELOGI("Loading trace file {}...", abs_path);

  if (!Setup()) {
    XELOGE("Unable to setup trace dump tool");
    return 4;
  }
  if (!Load(std::move(abs_path))) {
    XELOGE("Unable to load trace file; not found?");
    return 5;
  }

  // Root file name for outputs.
  if (output_path.empty()) {
    base_output_path_ = cvars::trace_dump_path;
    auto output_name = path.filename().replace_extension();

    base_output_path_ = base_output_path_ / output_name;
  } else {
    base_output_path_ = output_path;
  }

  // Ensure output path exists.
  xe::filesystem::CreateParentFolder(base_output_path_);

  return Run();
}

bool TraceDump::Setup() {
  // Create the emulator but don't initialize so we can setup the window.
  emulator_ = std::make_unique<Emulator>("", "", "", "");
  X_STATUS result = emulator_->Setup(
      nullptr, nullptr, false, nullptr,
      [this]() { return CreateGraphicsSystem(); }, nullptr);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    return false;
  }
  // Setup only stores the factories; the graphics system is created here.
  result = emulator_->SetupSubsystems();
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator subsystems: {:08X}", result);
    return false;
  }
  graphics_system_ = emulator_->graphics_system();
  player_ = std::make_unique<TracePlayer>(graphics_system_);
  return true;
}

bool TraceDump::Load(const std::filesystem::path& trace_file_path) {
  trace_file_path_ = trace_file_path;

  if (!player_->Open(xe::path_to_utf8(trace_file_path_))) {
    XELOGE("Could not load trace file");
    return false;
  }

  return true;
}

bool TraceDump::CaptureToPng(const std::filesystem::path& png_path) {
  ui::Presenter* presenter = graphics_system_->presenter();
  ui::RawImage raw_image;
  if (!presenter || !presenter->CaptureGuestOutput(raw_image)) {
    return false;
  }
  auto handle = filesystem::OpenFile(png_path, "wb");
  if (!handle) {
    return false;
  }
  auto callback = [](void* context, void* data, int size) {
    fwrite(data, 1, size, (FILE*)context);
  };
  stbi_write_png_to_func(callback, handle, static_cast<int>(raw_image.width),
                         static_cast<int>(raw_image.height), 4,
                         raw_image.data.data(),
                         static_cast<int>(raw_image.stride));
  fclose(handle);
  return true;
}

int TraceDump::Run() {
  int capture_frame = cvars::trace_dump_capture_frame;
  if (capture_frame < 0) {
    BeginHostCapture();
  }
  // State accumulates across frames, so every frame has to run in order.
  int frame_count = player_->frame_count();
  XELOGI("TraceDump: replaying {} frames", frame_count);
  for (int i = 0; i < frame_count; ++i) {
    // Seeking an empty frame starts no playback, so the wait never ends.
    int last_command = static_cast<int>(player_->frame(i)->commands.size()) - 1;
    if (last_command < 0) {
      continue;
    }
    XELOGI("TraceDump: frame {}/{} ({} commands)", i, frame_count,
           last_command + 1);
    if (i == capture_frame) {
      BeginHostCapture();
    }
    if (i == cvars::trace_dump_trace_frame) {
      graphics_system_->RequestFrameTrace();
    }
    bool stop_early = i == cvars::trace_dump_edram_frame &&
                      cvars::trace_dump_stop_command >= 0 &&
                      cvars::trace_dump_stop_command < last_command;
    if (stop_early) {
      player_->PlayFramePrefix(i, cvars::trace_dump_stop_command);
    } else {
      player_->SeekFrame(i);
      player_->SeekCommand(last_command);
    }
    player_->WaitOnPlayback();
    if (i == cvars::trace_dump_edram_frame) {
      std::filesystem::path edram_path = base_output_path_;
      edram_path.replace_filename(edram_path.stem().string() + "_edram.bin");
      // The dump submits GPU work, so it has to run on the GPU thread.
      CommandProcessor* command_processor =
          graphics_system_->command_processor();
      bool edram_written = false;
      threading::Fence edram_fence;
      command_processor->CallInThread([&]() {
        edram_written = command_processor->DumpEdramSnapshotToFile(edram_path);
        edram_fence.Signal();
      });
      edram_fence.Wait();
      if (edram_written) {
        XELOGI("TraceDump: wrote the EDRAM snapshot after frame {} command {}",
               i, stop_early ? cvars::trace_dump_stop_command : last_command);
      }
    }
    if (i == capture_frame) {
      EndHostCapture();
    }
    if (i == cvars::trace_dump_memory_frame) {
      std::filesystem::path mem_path = base_output_path_;
      mem_path.replace_filename(mem_path.stem().string() + "_mem.bin");
      auto mem_handle = filesystem::OpenFile(mem_path, "wb");
      if (mem_handle) {
        fwrite(emulator_->memory()->physical_membase() +
                   cvars::trace_dump_memory_base,
               1, cvars::trace_dump_memory_size, mem_handle);
        fclose(mem_handle);
        XELOGI("TraceDump: wrote guest memory at frame {}", i);
      }
    }
    if (cvars::trace_dump_interval && i >= int(cvars::trace_dump_from) &&
        (!cvars::trace_dump_to || i <= int(cvars::trace_dump_to)) &&
        !((i - int(cvars::trace_dump_from)) %
          int(cvars::trace_dump_interval))) {
      std::filesystem::path frame_path = base_output_path_;
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), "_f%05d.png", i);
      frame_path.replace_filename(frame_path.stem().string() + suffix);
      CaptureToPng(frame_path);
      if (cvars::trace_dump_memory_series) {
        std::filesystem::path mem_path = base_output_path_;
        char mem_suffix[40];
        std::snprintf(mem_suffix, sizeof(mem_suffix), "_f%05d_mem.bin", i);
        mem_path.replace_filename(mem_path.stem().string() + mem_suffix);
        auto h = filesystem::OpenFile(mem_path, "wb");
        if (h) {
          fwrite(emulator_->memory()->physical_membase() +
                     cvars::trace_dump_memory_base,
                 1, cvars::trace_dump_memory_size, h);
          fclose(h);
        }
      }
    }
  }
  if (capture_frame < 0) {
    EndHostCapture();
  }

  // Capture.
  int result =
      CaptureToPng(base_output_path_.replace_extension(".png")) ? 0 : 1;

  player_.reset();
  emulator_.reset();
  return result;
}

}  //  namespace gpu
}  //  namespace xe
