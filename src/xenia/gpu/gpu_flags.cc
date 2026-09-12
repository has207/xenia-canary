/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/gpu_flags.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/ui/renderdoc_api.h"

// Unified-memory hosts (Apple Silicon, Windows on ARM) benefit from aliasing
// guest RAM directly. x86-64 hosts are overwhelmingly discrete, where it is
// slow, so default off there.
#if XE_ARCH_ARM64
#define XE_GPU_ZERO_COPY_DEFAULT true
#else
#define XE_GPU_ZERO_COPY_DEFAULT false
#endif
DEFINE_bool(
    shared_memory_zero_copy, XE_GPU_ZERO_COPY_DEFAULT,
    "Alias guest RAM directly as the GPU shared-memory buffer instead "
    "of uploading dirty pages each frame. Removes upload copies and "
    "keeps memexport and resolve output coherent with the CPU for free. "
    "Default on for ARM64 (unified-memory) builds, off for x86-64. "
    "Turn off on discrete GPUs, where shared-memory fetches cross PCIe "
    "and are slow.",
    "GPU");
#undef XE_GPU_ZERO_COPY_DEFAULT

DEFINE_bool(
    enable_host_buffer, true,
    "Import guest RAM as a second GPU buffer, so memexport output and resolve "
    "readback reach the CPU in place rather than through a staging copy. With "
    "it off, or where the driver can't import guest RAM, both fall back to "
    "copying through a staging buffer, where a resolve readback_resolve_sync "
    "would have run asynchronously is instead a frame behind. Ignored under "
    "shared_memory_zero_copy, where the only buffer already aliases guest RAM. "
    "Applies at title launch.",
    "GPU");

DEFINE_bool(use_50Hz_mode, false, "Enables usage of PAL-50 mode.", "Console");

DEFINE_path(trace_gpu_prefix, "scratch/gpu/",
            "Prefix path for GPU trace files.", "GPU.Debug");
DEFINE_bool(trace_gpu_stream, false, "Trace all GPU packets.", "GPU.Debug");

DEFINE_path(
    dump_shaders, "",
    "For shader debugging, path to dump GPU shaders to as they are compiled.",
    "GPU.Debug");

DEFINE_bool(guest_display_refresh_cap, true,
            "Control guest vblank timing.\n"
            "  true: Fixed rate vblanks (50Hz PAL, 60Hz NTSC based on "
            "use_50Hz_mode).\n"
            "  false: Unlimited vblanks, allows the guest to run as fast as "
            "possible.",
            "GPU");

DEFINE_uint32(
    framerate_limit, 0,
    "Host frame rate limit in FPS. 0 = unlimited.\n"
    "Throttles presentation without affecting guest vblank timing.\n"
    "Guest vblanks are controlled by use_50Hz_mode (50Hz PAL, 60Hz NTSC).",
    "GPU");

void SetGuestDisplayRefreshCap(bool value) {
  OVERRIDE_bool(guest_display_refresh_cap, value);
}

void SetFramerateLimit(uint32_t value) {
  OVERRIDE_uint32(framerate_limit, value);
}

DEFINE_bool(
    gpu_allow_invalid_fetch_constants, true,
    "Allow texture and vertex fetch constants with invalid type - generally "
    "unsafe because the constant may contain completely invalid values, but "
    "may be used to bypass fetch constant type errors in certain games until "
    "the real reason why they're invalid is found.",
    "GPU");
// TODO(has207): the range check this skips predates the handler that backs
// unallocated guest pages on fault, so reading past a guest allocation is
// now harmless. Texture mip extents routinely overrun the allocation, and
// the check left those pages invalid forever. Drop it and this cvar if
// nothing regresses.
DEFINE_bool(
    gpu_allow_invalid_upload_range, true,
    "Allows games to read data from pages that are marked as no access.",
    "GPU");
UPDATE_from_bool(gpu_allow_invalid_upload_range, 2026, 9, 12, 12, false);

DEFINE_bool(
    non_seamless_cube_map, true,
    "Disable filtering between cube map faces near edges where possible "
    "(Vulkan with VK_EXT_non_seamless_cube_map) to reproduce the Direct3D 9 "
    "behavior.",
    "GPU.Debug");

// Extremely bright screen borders in 4D5307E6.
// Reading between texels with half-pixel offset in 58410954.
DEFINE_bool(
    half_pixel_offset, true,
    "Enable support of vertex half-pixel offset (D3D9 PA_SU_VTX_CNTL "
    "PIX_CENTER). Generally games are aware of the half-pixel offset, and "
    "having this enabled is the correct behavior (disabling this may "
    "significantly break post-processing in some games), but in certain games "
    "it might have been ignored, resulting in slight blurriness of UI "
    "textures, for instance, when they are read between texels rather than "
    "at texel centers, or the leftmost/topmost pixels may not be fully covered "
    "when MSAA is used with fullscreen passes.",
    "GPU.Debug");

DEFINE_int32(occlusion_query_fake_lower_threshold, 80,
             "Lower end of the fake sample count value written on "
             "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
             "-1 writes nothing, resulting in some games that sit and hang.\n"
             "0 means the fake result stays fully occluded.",
             "GPU");
DEFINE_int32(occlusion_query_fake_upper_threshold, 100,
             "Upper end of the fake sample count value written on "
             "EVENT_WRITE_ZPD when real occlusion queries are disabled.\n"
             "Keep this higher than occlusion_query_fake_lower_threshold.\n"
             "Ignored if occlusion_query_fake_lower_threshold is -1.",
             "GPU");
DEFINE_bool(occlusion_query_log, false,
            "Log occlusion query lifetime and summary stats.", "GPU");
DEFINE_int32(occlusion_query_querybatch_range, 0,
             "Range of fake sample count values to walk for titles using the\n"
             "D3D QueryBatch standard before wrapping back to\n"
             "occlusion_query_fake_lower_threshold. This shouldn't be changed\n"
             "from the default value of 0 (disabled) unless necessary for a\n"
             "specific title.",
             "GPU");
DEFINE_double(
    occlusion_query_saturation, 1.0,
    "Compress higher occlusion query sample counts before guest writeback.\n"
    "This can be useful if effects such as lens flares appear too bright\n"
    "or too strong.\n"
    "1.0 = default behavior\n"
    "0.0 = collapse all nonzero sample counts to 1\n"
    "Values around 0.90 are a good starting point for subtle tuning.",
    "GPU");

uint32_t GetGuestVblankRateHz() { return cvars::use_50Hz_mode ? 50 : 60; }

DEFINE_bool(
    gpu_debug_markers, false,
    "Insert debug markers into GPU command streams for tools like RenderDoc. "
    "Annotates draw calls with Xbox 360 GPU context (primitive type, shader "
    "hashes, vertex count, etc). Automatically enabled when RenderDoc is "
    "detected. Has minimal overhead when disabled.",
    "GPU");

bool IsGpuDebugMarkersEnabled() {
  // Cache the result - RenderDoc detection only needs to happen once.
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (cvars::gpu_debug_markers) {
      result = true;
      XELOGI("GPU debug markers enabled via CVAR");
    } else {
      auto renderdoc_api = xe::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        XELOGI("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }
  }
  return result;
}

// TODO(Triang3l): Make accuracy (ROV/FSI) the default when it's optimized
// better (for instance, using static shader modifications to pass render
// target parameters).
DEFINE_string(
    render_target_path, "performance",
    "Render target emulation path to use across all GPU backends.\n"
    "Use: [performance, accuracy]\n"
    " performance:\n"
    "  Host render targets and fixed-function blending and depth/stencil "
    "testing, copying between render targets when needed.\n"
    "  Lower accuracy (limited pixel format support).\n"
    "  Performance limited primarily by render target layout changes requiring "
    "copying, but generally higher.\n"
    "  Maps to 'fbo' on Vulkan and 'rtv' on D3D12.\n"
    " accuracy:\n"
    "  Manual pixel packing, blending and depth/stencil testing, with free "
    "render target layout changes.\n"
    "  Requires GPU supporting fragment shader interlock (Vulkan) or "
    "rasterizer-ordered views (D3D12).\n"
    "  Highest accuracy (all pixel formats handled in software).\n"
    "  Performance limited primarily by overdraw.\n"
    "  Maps to 'fsi' on Vulkan and 'rov' on D3D12.",
    "GPU");

DEFINE_bool(submit_on_primary_buffer_end, true,
            "Submit the command buffer when a PM4 primary buffer ends if it's "
            "possible to submit immediately to try to reduce frame latency.",
            "GPU");

DEFINE_bool(no_discard_stencil_in_transfer_pipelines, false,
            "Skip stencil bit discard in render target transfer pipelines. "
            "May improve performance on some GPUs.",
            "GPU.Debug");

DEFINE_bool(
    async_shader_compilation, true,
    "Compile shaders and create pipelines asynchronously in background "
    "threads. "
    "Eliminates shader compilation stutter but may cause brief rendering "
    "artifacts while pipelines are being created. When disabled, pipelines are "
    "created synchronously which causes stutter but no visual artifacts.",
    "GPU");

DEFINE_bool(async_shader_vs_interpreter, true,
            "Render new vertex shaders with the ucode interpreter while they "
            "translate and compile in the background, instead of stalling on "
            "translation. Requires async_shader_compilation.",
            "GPU");
DEFINE_bool(
    async_shader_vs_interpreter_debug_color, false,
    "Draw ucode interpreter VS placeholders with a flat grey pixel "
    "shader so the interim geometry is visible (host render target path "
    "only). Requires async_shader_vs_interpreter.",
    "GPU");
DEFINE_bool(
    async_shader_skip_draws, true,
    "Skip draws whose shaders can't render immediately via a placeholder "
    "(no interpreter stand-in, e.g. tessellation or textured/memexport/loop "
    "vertex shaders) until their real pipeline compiles in the background, "
    "instead of translating them on the draw thread. Avoids stutter but the "
    "geometry pops in a few frames later.",
    "GPU");

DEFINE_bool(
    shader_profiling, false,
    "Log shader translation and host pipeline (PSO) creation timings, tagged "
    "with 'shader_profiling:'. Off by default because it logs per shader and "
    "per pipeline.",
    "GPU");

DEFINE_bool(
    readback_resolve_half_pixel_offset, false,
    "When resolution scaling is active, sample from the center of each scaled "
    "pixel block during readback resolve instead of the top-left corner. May "
    "improve image quality in some cases but can break games that rely on "
    "reading back specific pixel values (e.g., for gamma detection).",
    "GPU");

DEFINE_bool(readback_resolve_sync, false,
            "Stall the GPU after each readback_resolve copy so guest RAM is "
            "coherent in the same frame, instead of copying asynchronously.\n"
            "The copies a guest read actually waits on stall on their own, so "
            "this only adds a stall for the rest.",
            "GPU");
UPDATE_from_bool(readback_resolve_sync, 2026, 8, 14, 12, true);

DEFINE_bool(gpu_3d_to_2d_texture, true,
            "Handle shaders that sample 3D textures as 2D by creating a 2D "
            "texture from slice 0 of the guest memory.",
            "GPU");

DEFINE_int32(anisotropic_override, -1,
             "Forces anisotropic filtering (AF) for eligible textures.\n"
             "Higher values keep textures sharper at oblique angles at the "
             "cost of GPU bandwidth, though most GPUs handle up to 16x fine.\n"
             "In rare cases, forcing AF can introduce visual artifacts.\n"
             " -1 = No override\n"
             "  0 = Disable anisotropic filtering\n"
             "  1 = Force 1x anisotropic filtering\n"
             "  2 = Force 2x anisotropic filtering\n"
             "  3 = Force 4x anisotropic filtering\n"
             "  4 = Force 8x anisotropic filtering\n"
             "  5 = Force 16x anisotropic filtering",
             "GPU");

DEFINE_bool(use_fuzzy_alpha_epsilon, false,
            "Use approximate compare for alpha values to prevent flickering on "
            "NVIDIA graphics cards",
            "GPU");

DEFINE_bool(
    force_depth_clamp, false,
    "Use host depth clamping instead of near and far plane clipping when "
    "guest clipping is enabled. X/Y/W clipping is unaffected. On Vulkan, "
    "this requires depthClamp support.",
    "GPU");
