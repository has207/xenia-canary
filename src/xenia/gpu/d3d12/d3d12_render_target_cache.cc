/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"

#include <cstdint>
#include <cstring>
#include <tuple>

#include "third_party/fmt/include/fmt/xchar.h"

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_texture_cache.h"
#include "xenia/gpu/d3d12/deferred_command_list.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/spirv_to_dxil_compiler.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(
    native_stencil_value_output_d3d12_intel, false,
    "Allow stencil reference output usage on Direct3D 12 on Intel GPUs - not "
    "working on UHD Graphics 630 as of March 2021 (driver 27.20.0100.8336).",
    "GPU.Debug");

namespace xe {
namespace gpu {
namespace d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/clear_uint2_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/fullscreen_cw_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/passthrough_position_xy_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_clear_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_clear_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_clear_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_clear_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

static constexpr D3D12RenderTargetCache::ResolveCopyShaderCode
    kResolveCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs,
         sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs,
         sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs,
         sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs,
         sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs,
         sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs,
         sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

D3D12RenderTargetCache::~D3D12RenderTargetCache() { Shutdown(true); }

bool D3D12RenderTargetCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (cvars::render_target_path == "performance") {
    path_ = Path::kHostRenderTargets;
  } else if (cvars::render_target_path == "accuracy") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    // As of April 2021 (driver version 27.20.0100.9316), on Intel (tested on
    // UHD Graphics 630), the "always" stencil comparison function isn't working
    // properly, so clears in the Xbox 360's Direct3D 9 don't work. Forcing ROV
    // there.
    // As of December 2025, Intel pre-Arc still suffers from this issue.
    // Intel Arc (Alchemist and newer) does not have this issue so an
    // exception is made so they default to RTV.
#if 1
    // The ROV path is currently much slower generally.
    // TODO(Triang3l): Make ROV the default when it's optimized better (for
    // instance, using static shader modifications to pass render target
    // parameters).

    path_ = Path::kHostRenderTargets;
    if (provider.GetAdapterVendorID() ==
            ui::GraphicsProvider::GpuVendorID::kIntel &&
        !provider.IsIntelArcGpu()) {
      path_ = Path::kPixelShaderInterlock;
    }
#else
    // The AMD shader compiler crashes very often with Xenia's custom
    // output-merger code as of March 2021.
    path_ =
        provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kAMD
            ? Path::kHostRenderTargets
            : Path::kPixelShaderInterlock;
#endif
  }
  if (path_ == Path::kPixelShaderInterlock &&
      !provider.AreRasterizerOrderedViewsSupported()) {
    path_ = Path::kHostRenderTargets;
  }

  // Create the buffer for reinterpreting EDRAM contents.
  uint32_t edram_buffer_size =
      xenos::kEdramSizeBytes *
      (draw_resolution_scale_x() * draw_resolution_scale_y());
  D3D12_RESOURCE_DESC edram_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      edram_buffer_desc, edram_buffer_size,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // The first operation will likely be depth self-comparison with host render
  // targets or drawing with ROV.
  edram_buffer_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Creating zeroed for stable initial value with ROV (though on a real
  // console it has to be cleared anyway probably) and not to leak irrelevant
  // data to trace dumps when not covered by host render targets entirely.
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &edram_buffer_desc, edram_buffer_state_, nullptr,
          IID_PPV_ARGS(&edram_buffer_)))) {
    XELOGE("D3D12RenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  edram_buffer_->SetName(L"EDRAM Buffer");
  edram_buffer_gpu_address_ = edram_buffer_->GetGPUVirtualAddress();
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;

  // Create non-shader-visible descriptors of the EDRAM buffer for copying.
  D3D12_DESCRIPTOR_HEAP_DESC edram_buffer_descriptor_heap_desc;
  edram_buffer_descriptor_heap_desc.Type =
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  edram_buffer_descriptor_heap_desc.NumDescriptors =
      uint32_t(EdramBufferDescriptorIndex::kCount);
  edram_buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  edram_buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(
          &edram_buffer_descriptor_heap_desc,
          IID_PPV_ARGS(&edram_buffer_descriptor_heap_)))) {
    XELOGE(
        "D3D12RenderTargetCache: Failed to create the descriptor heap for "
        "EDRAM buffer views");
    Shutdown();
    return false;
  }
  edram_buffer_descriptor_heap_start_ =
      edram_buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32G32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32G32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Create the resolve copying root signature.
  std::array<D3D12_ROOT_PARAMETER, 3> resolve_copy_root_parameters;
  // Parameter 0 is constants.
  resolve_copy_root_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  resolve_copy_root_parameters[0].Constants.ShaderRegister = 0;
  resolve_copy_root_parameters[0].Constants.RegisterSpace = 0;
  // Binding all of the shared memory at 1x resolution, portions with scaled
  // resolution.
  resolve_copy_root_parameters[0].Constants.Num32BitValues =
      (draw_resolution_scaled
           ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
           : sizeof(draw_util::ResolveCopyShaderConstants)) /
      sizeof(uint32_t);
  resolve_copy_root_parameters[0].ShaderVisibility =
      D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the destination (shared memory).
  resolve_copy_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  resolve_copy_root_parameters[1].Descriptor.ShaderRegister = 0;
  resolve_copy_root_parameters[1].Descriptor.RegisterSpace = 0;
  resolve_copy_root_parameters[1].ShaderVisibility =
      D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the source (EDRAM).
  resolve_copy_root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  resolve_copy_root_parameters[2].Descriptor.ShaderRegister = 0;
  resolve_copy_root_parameters[2].Descriptor.RegisterSpace = 0;
  resolve_copy_root_parameters[2].ShaderVisibility =
      D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC resolve_copy_root_signature_desc;
  resolve_copy_root_signature_desc.NumParameters =
      UINT(resolve_copy_root_parameters.size());
  resolve_copy_root_signature_desc.pParameters =
      resolve_copy_root_parameters.data();
  resolve_copy_root_signature_desc.NumStaticSamplers = 0;
  resolve_copy_root_signature_desc.pStaticSamplers = nullptr;
  resolve_copy_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  resolve_copy_root_signature_ = ui::d3d12::util::CreateRootSignature(
      provider, resolve_copy_root_signature_desc);
  if (resolve_copy_root_signature_ == nullptr) {
    XELOGE(
        "D3D12RenderTargetCache: Failed to create the resolve copy root "
        "signature");
    Shutdown();
    return false;
  }
  if (draw_resolution_scaled) {
    // Second root signature for fully native resolve copies, full constants
    // including the dest base, like without scaling.
    resolve_copy_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_util::ResolveCopyShaderConstants) / sizeof(uint32_t);
    resolve_copy_native_root_signature_ = ui::d3d12::util::CreateRootSignature(
        provider, resolve_copy_root_signature_desc);
    if (resolve_copy_native_root_signature_ == nullptr) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the native resolve copy "
          "root signature");
      Shutdown();
      return false;
    }
  }

  // Create the resolve copying pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount);
       ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code =
        kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled &&
                resolve_copy_shader_code.unscaled_size &&
                resolve_copy_shader_code.scaled &&
                resolve_copy_shader_code.scaled_size);
    ID3D12PipelineState* resolve_copy_pipeline =
        ui::d3d12::util::CreateComputePipeline(
            device,
            draw_resolution_scaled ? resolve_copy_shader_code.scaled
                                   : resolve_copy_shader_code.unscaled,
            draw_resolution_scaled ? resolve_copy_shader_code.scaled_size
                                   : resolve_copy_shader_code.unscaled_size,
            resolve_copy_root_signature_);
    if (resolve_copy_pipeline == nullptr) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create {} resolve copy pipeline",
          resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    std::u16string resolve_copy_pipeline_name =
        xe::to_utf16(resolve_copy_shader_info.debug_name);
    resolve_copy_pipeline->SetName(
        reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
    if (draw_resolution_scaled) {
      // Unscaled variant for fully native resolves.
      ID3D12PipelineState* resolve_copy_native_pipeline =
          ui::d3d12::util::CreateComputePipeline(
              device, resolve_copy_shader_code.unscaled,
              resolve_copy_shader_code.unscaled_size,
              resolve_copy_native_root_signature_);
      if (resolve_copy_native_pipeline == nullptr) {
        XELOGE(
            "D3D12RenderTargetCache: Failed to create {} native resolve copy "
            "pipeline",
            resolve_copy_shader_info.debug_name);
        Shutdown();
        return false;
      }
      resolve_copy_native_pipeline->SetName(
          reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
      resolve_copy_native_pipelines_[i] = resolve_copy_native_pipeline;
    }
  }

  // Using the cvar on emulator initialization so used pipelines are consistent
  // across different titles launched in one emulator instance.
  use_stencil_reference_output_ =
      cvars::native_stencil_value_output &&
      provider.IsPSSpecifiedStencilReferenceSupported() &&
      (provider.IsIntelArcGpu() ||
       cvars::native_stencil_value_output_d3d12_intel ||
       provider.GetAdapterVendorID() !=
           ui::GraphicsProvider::GpuVendorID::kIntel);

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    gamma_render_target_as_unorm16_ = cvars::gamma_render_target_as_unorm16;

    depth_float24_round_ = cvars::depth_float24_round;
    depth_float24_convert_in_pixel_shader_ =
        cvars::depth_float24_convert_in_pixel_shader;

    // Check if 2x MSAA is supported or needs to be emulated with 4x MSAA
    // instead.
    if (!cvars::debug_msaa_2x_as_4x) {
      msaa_2x_supported_ = true;
      static constexpr DXGI_FORMAT kRenderTargetDXGIFormats[] = {
          DXGI_FORMAT_R16G16B16A16_FLOAT,
          DXGI_FORMAT_R16G16B16A16_SNORM,
          DXGI_FORMAT_R32G32_FLOAT,
          DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
          DXGI_FORMAT_R10G10B10A2_UNORM,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          DXGI_FORMAT_R16G16_FLOAT,
          DXGI_FORMAT_R16G16_SNORM,
          DXGI_FORMAT_R32_FLOAT,
          DXGI_FORMAT_D24_UNORM_S8_UINT,
          // For ownership transfer.
          DXGI_FORMAT_R16G16B16A16_UINT,
          DXGI_FORMAT_R32G32_UINT,
          DXGI_FORMAT_R16G16_UINT,
          DXGI_FORMAT_R32_UINT,
      };
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags =
          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      for (size_t i = 0; i < xe::countof(kRenderTargetDXGIFormats); ++i) {
        multisample_quality_levels.Format = kRenderTargetDXGIFormats[i];
        multisample_quality_levels.NumQualityLevels = 0;
        if (FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &multisample_quality_levels,
                sizeof(multisample_quality_levels))) ||
            !multisample_quality_levels.NumQualityLevels) {
          msaa_2x_supported_ = false;
          break;
        }
      }
      if (msaa_2x_supported_ && gamma_render_target_as_unorm16_) {
        multisample_quality_levels.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        multisample_quality_levels.NumQualityLevels = 0;
        if (FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &multisample_quality_levels,
                sizeof(multisample_quality_levels))) ||
            !multisample_quality_levels.NumQualityLevels) {
          msaa_2x_supported_ = false;
        }
      }
    } else {
      msaa_2x_supported_ = false;
    }
    if (!msaa_2x_supported_) {
      XELOGW(
          "2x MSAA is not supported, emulated via top-left and bottom-right "
          "samples of 4x MSAA");
    }

    descriptor_pool_color_ =
        std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
            provider, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 11);
    descriptor_pool_depth_ =
        std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
            provider, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 11);
    descriptor_pool_srv_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11);

    // Create null render target descriptors for gaps, must be fully typed
    // (though in pipeline states, DXGI_FORMAT_UNKNOWN must be used instead -
    // this would also cause a mismatching format error in the debug layer, but
    // it's a bug in the debug layer itself - needs to be suppressed, and
    // already fixed in some version of Windows).
    null_rtv_descriptor_ss_ = descriptor_pool_color_->AllocateDescriptor();
    null_rtv_descriptor_ms_ = descriptor_pool_color_->AllocateDescriptor();
    if (!null_rtv_descriptor_ss_ || !null_rtv_descriptor_ms_) {
      Shutdown();
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv_desc;
    // The format doesn't matter, but it must be bindable as a render target,
    // not DXGI_FORMAT_UNKNOWN.
    null_rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    null_rtv_desc.Texture2D.MipSlice = 0;
    null_rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc,
                                   null_rtv_descriptor_ss_.GetHandle());
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc,
                                   null_rtv_descriptor_ms_.GetHandle());

    // For host depth -> same depth transfers, host depth storing root signature
    // and pipelines.
    D3D12_ROOT_PARAMETER
    host_depth_store_root_parameters[kHostDepthStoreRootParameterCount];
    // Constants.
    D3D12_ROOT_PARAMETER& host_depth_store_root_constants =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterConstants];
    host_depth_store_root_constants.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    host_depth_store_root_constants.Constants.ShaderRegister = 0;
    host_depth_store_root_constants.Constants.RegisterSpace = 0;
    host_depth_store_root_constants.Constants.Num32BitValues =
        sizeof(HostDepthStoreConstants) / sizeof(uint32_t);
    host_depth_store_root_constants.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
    // Source.
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_source_range;
    host_depth_store_root_source_range.RangeType =
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    host_depth_store_root_source_range.NumDescriptors = 1;
    host_depth_store_root_source_range.BaseShaderRegister = 0;
    host_depth_store_root_source_range.RegisterSpace = 0;
    host_depth_store_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_source =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterSource];
    host_depth_store_root_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_source.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_source.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_source_range;
    host_depth_store_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Destination.
    D3D12_ROOT_PARAMETER& host_depth_store_root_dest =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterDest];
    host_depth_store_root_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    host_depth_store_root_dest.Descriptor.ShaderRegister = 0;
    host_depth_store_root_dest.Descriptor.RegisterSpace = 0;
    host_depth_store_root_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Root signature.
    D3D12_ROOT_SIGNATURE_DESC host_depth_store_root_desc;
    host_depth_store_root_desc.NumParameters =
        UINT(xe::countof(host_depth_store_root_parameters));
    host_depth_store_root_desc.pParameters = host_depth_store_root_parameters;
    host_depth_store_root_desc.NumStaticSamplers = 0;
    host_depth_store_root_desc.pStaticSamplers = nullptr;
    host_depth_store_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    host_depth_store_root_signature_ = ui::d3d12::util::CreateRootSignature(
        provider, host_depth_store_root_desc);
    if (!host_depth_store_root_signature_) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the host depth storing "
          "root signature");
      Shutdown();
      return false;
    }
    // Pipelines.
    // 1 sample.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
        ui::d3d12::util::CreateComputePipeline(
            device, shaders::host_depth_store_1xmsaa_cs,
            sizeof(shaders::host_depth_store_1xmsaa_cs),
            host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the 1-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]->SetName(
        L"Host Depth Store 1xMSAA");
    // 2 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
        ui::d3d12::util::CreateComputePipeline(
            device, shaders::host_depth_store_2xmsaa_cs,
            sizeof(shaders::host_depth_store_2xmsaa_cs),
            host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the 2-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]->SetName(
        L"Host Depth Store 2xMSAA");
    // 4 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
        ui::d3d12::util::CreateComputePipeline(
            device, shaders::host_depth_store_4xmsaa_cs,
            sizeof(shaders::host_depth_store_4xmsaa_cs),
            host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the 4-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]->SetName(
        L"Host Depth Store 4xMSAA");

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ =
        std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
            provider,
            std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                     sizeof(float) * 2 * 6 *
                         Transfer::kMaxCutoutBorderRectangles *
                         xenos::kEdramTileCount));

    // Transfer root signatures - one per mode, since the mode is what decides
    // which of the emitter's descriptor sets the shader declares, and which
    // register space Mesa gives each of them.
    D3D12_ROOT_PARAMETER
    transfer_root_parameters[kTransferUsedRootParameterCount];
    D3D12_DESCRIPTOR_RANGE
    transfer_root_ranges[kTransferUsedRootParameterCount];
    D3D12_ROOT_SIGNATURE_DESC transfer_root_desc;
    transfer_root_desc.pParameters = transfer_root_parameters;
    transfer_root_desc.NumStaticSamplers = 0;
    transfer_root_desc.pStaticSamplers = nullptr;
    transfer_root_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    for (size_t i = 0; i < size_t(EdramTransferMode::kCount); ++i) {
      TransferRootSignatureInfo transfer_root_info =
          GetTransferRootSignatureInfo(EdramTransferMode(i));
      uint32_t transfer_root_mask = transfer_root_info.used_root_parameters;
      auto transfer_root_index = [transfer_root_mask](uint32_t parameter_bit) {
        return xe::bit_count(transfer_root_mask & (parameter_bit - 1));
      };
      // A one-descriptor table per texture: the command processor can't
      // contiguously allocate multiple descriptors with bindless, so the
      // depth source's stencil gets a table of its own rather than sharing
      // the depth one's, even though Mesa puts them in the same space.
      auto set_up_transfer_srv = [&](uint32_t parameter_bit,
                                     uint32_t shader_register,
                                     uint32_t register_space) {
        uint32_t parameter_index = transfer_root_index(parameter_bit);
        D3D12_DESCRIPTOR_RANGE& range = transfer_root_ranges[parameter_index];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = shader_register;
        range.RegisterSpace = register_space;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER& parameter =
            transfer_root_parameters[parameter_index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      };
      // Push constants.
      {
        D3D12_ROOT_PARAMETER& parameter =
            transfer_root_parameters[transfer_root_index(
                kTransferUsedRootParameterPushConstantsBit)];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister =
            SpirvToDxilCompiler::kPushConstantShaderRegister;
        parameter.Constants.RegisterSpace =
            SpirvToDxilCompiler::kPushConstantRegisterSpace;
        parameter.Constants.Num32BitValues = kTransferRootPushConstantDwords;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      if (transfer_root_mask & kTransferUsedRootParameterColorSRVBit) {
        set_up_transfer_srv(kTransferUsedRootParameterColorSRVBit, 0,
                            transfer_root_info.space_source);
      }
      if (transfer_root_mask & kTransferUsedRootParameterDepthSRVBit) {
        set_up_transfer_srv(kTransferUsedRootParameterDepthSRVBit, 0,
                            transfer_root_info.space_source);
      }
      if (transfer_root_mask & kTransferUsedRootParameterStencilSRVBit) {
        set_up_transfer_srv(kTransferUsedRootParameterStencilSRVBit, 1,
                            transfer_root_info.space_source);
      }
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthSRVBit) {
        set_up_transfer_srv(kTransferUsedRootParameterHostDepthSRVBit, 0,
                            transfer_root_info.space_host_depth);
      }
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthBufferBit) {
        D3D12_ROOT_PARAMETER& parameter =
            transfer_root_parameters[transfer_root_index(
                kTransferUsedRootParameterHostDepthBufferBit)];
        // A root descriptor is a raw buffer view, which is the kind Mesa
        // declares for the storage buffer.
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = 0;
        parameter.Descriptor.RegisterSpace =
            transfer_root_info.space_host_depth;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      transfer_root_desc.NumParameters = xe::bit_count(transfer_root_mask);
      assert_true(transfer_root_desc.NumParameters <=
                  kTransferUsedRootParameterCount);
      transfer_root_signatures_[i] =
          ui::d3d12::util::CreateRootSignature(provider, transfer_root_desc);
      if (!transfer_root_signatures_[i]) {
        XELOGE(
            "D3D12RenderTargetCache: Failed to create the render target "
            "ownership transfer root signature for mode {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dumping root signatures.
    D3D12_DESCRIPTOR_RANGE dump_root_source_range;
    dump_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_source_range.NumDescriptors = 1;
    dump_root_source_range.BaseShaderRegister = 0;
    dump_root_source_range.RegisterSpace = kDumpDescriptorSetSource;
    dump_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_stencil_range;
    dump_root_stencil_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_stencil_range.NumDescriptors = 1;
    dump_root_stencil_range.BaseShaderRegister = 1;
    dump_root_stencil_range.RegisterSpace = kDumpDescriptorSetSource;
    dump_root_stencil_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    dump_root_color_parameters[kDumpRootParameterColorCount];
    D3D12_ROOT_PARAMETER
    dump_root_depth_parameters[kDumpRootParameterDepthCount];
    for (uint32_t i = 0; i < 2; ++i) {
      // Push constants.
      D3D12_ROOT_PARAMETER& dump_root_push_constants =
          i ? dump_root_depth_parameters[kDumpRootParameterPushConstants]
            : dump_root_color_parameters[kDumpRootParameterPushConstants];
      dump_root_push_constants.ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_push_constants.Constants.ShaderRegister =
          SpirvToDxilCompiler::kPushConstantShaderRegister;
      dump_root_push_constants.Constants.RegisterSpace =
          SpirvToDxilCompiler::kPushConstantRegisterSpace;
      dump_root_push_constants.Constants.Num32BitValues =
          kDumpRootPushConstantDwords;
      dump_root_push_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Source.
      D3D12_ROOT_PARAMETER& dump_root_source =
          i ? dump_root_depth_parameters[kDumpRootParameterSource]
            : dump_root_color_parameters[kDumpRootParameterSource];
      dump_root_source.ParameterType =
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_source.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_source.DescriptorTable.pDescriptorRanges =
          &dump_root_source_range;
      dump_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Stencil.
      if (i) {
        D3D12_ROOT_PARAMETER& dump_root_stencil =
            dump_root_depth_parameters[kDumpRootParameterDepthStencil];
        dump_root_stencil.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dump_root_stencil.DescriptorTable.NumDescriptorRanges = 1;
        dump_root_stencil.DescriptorTable.pDescriptorRanges =
            &dump_root_stencil_range;
        dump_root_stencil.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      // EDRAM.
      D3D12_ROOT_PARAMETER& dump_root_edram =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthEdram]
            : dump_root_color_parameters[kDumpRootParameterColorEdram];
      dump_root_edram.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      dump_root_edram.Descriptor.ShaderRegister = 0;
      dump_root_edram.Descriptor.RegisterSpace = kDumpDescriptorSetEdram;
      dump_root_edram.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC dump_root_desc;
    dump_root_desc.NumParameters =
        UINT(xe::countof(dump_root_color_parameters));
    dump_root_desc.pParameters = dump_root_color_parameters;
    dump_root_desc.NumStaticSamplers = 0;
    dump_root_desc.pStaticSamplers = nullptr;
    dump_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    dump_root_signature_color_ =
        ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_color_) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the color render target "
          "dumping root signature");
      Shutdown();
      return false;
    }
    dump_root_desc.NumParameters =
        UINT(xe::countof(dump_root_depth_parameters));
    dump_root_desc.pParameters = dump_root_depth_parameters;
    dump_root_signature_depth_ =
        ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_depth_) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the depth render target "
          "dumping root signature");
      Shutdown();
      return false;
    }

    // k_32_FLOAT and k_32_32_FLOAT clear root signature and pipelines.
    D3D12_ROOT_PARAMETER uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_constants.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    uint32_rtv_clear_root_constants.Constants.ShaderRegister = 0;
    uint32_rtv_clear_root_constants.Constants.RegisterSpace = 0;
    uint32_rtv_clear_root_constants.Constants.Num32BitValues = 2;
    uint32_rtv_clear_root_constants.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC uint32_rtv_clear_root_desc;
    uint32_rtv_clear_root_desc.NumParameters = 1;
    uint32_rtv_clear_root_desc.pParameters = &uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_desc.NumStaticSamplers = 0;
    uint32_rtv_clear_root_desc.pStaticSamplers = nullptr;
    uint32_rtv_clear_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    uint32_rtv_clear_root_signature_ = ui::d3d12::util::CreateRootSignature(
        provider, uint32_rtv_clear_root_desc);
    if (!uint32_rtv_clear_root_signature_) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the k_32_FLOAT / "
          "k_32_32_FLOAT render target clearing root signature");
      Shutdown();
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC uint32_rtv_clear_pipeline_desc = {};
    uint32_rtv_clear_pipeline_desc.pRootSignature =
        uint32_rtv_clear_root_signature_;
    uint32_rtv_clear_pipeline_desc.VS.pShaderBytecode =
        shaders::fullscreen_cw_vs;
    uint32_rtv_clear_pipeline_desc.VS.BytecodeLength =
        sizeof(shaders::fullscreen_cw_vs);
    uint32_rtv_clear_pipeline_desc.PS.pShaderBytecode = shaders::clear_uint2_ps;
    uint32_rtv_clear_pipeline_desc.PS.BytecodeLength =
        sizeof(shaders::clear_uint2_ps);
    uint32_rtv_clear_pipeline_desc.BlendState.RenderTarget[0]
        .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    uint32_rtv_clear_pipeline_desc.RasterizerState.FillMode =
        D3D12_FILL_MODE_SOLID;
    uint32_rtv_clear_pipeline_desc.RasterizerState.CullMode =
        D3D12_CULL_MODE_NONE;
    uint32_rtv_clear_pipeline_desc.RasterizerState.DepthClipEnable = true;
    uint32_rtv_clear_pipeline_desc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    uint32_rtv_clear_pipeline_desc.NumRenderTargets = 1;
    for (size_t i = 0; i < 2; ++i) {
      uint32_rtv_clear_pipeline_desc.RTVFormats[0] =
          GetColorOwnershipTransferDXGIFormat(
              i ? xenos::ColorRenderTargetFormat::k_32_32_FLOAT
                : xenos::ColorRenderTargetFormat::k_32_FLOAT);
      for (size_t j = size_t(xenos::MsaaSamples::k1X);
           j <= size_t(xenos::MsaaSamples::k4X); ++j) {
        if (xenos::MsaaSamples(j) == xenos::MsaaSamples::k2X &&
            !msaa_2x_supported_) {
          // Using sample 0 as 0 and 3 as 1 for 2x instead.
          uint32_rtv_clear_pipeline_desc.SampleMask = 0b1001;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 4;
        } else {
          uint32_rtv_clear_pipeline_desc.SampleMask = UINT_MAX;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 1 << j;
        }
        ID3D12PipelineState* uint32_rtv_clear_pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(
                &uint32_rtv_clear_pipeline_desc,
                IID_PPV_ARGS(&uint32_rtv_clear_pipeline)))) {
          XELOGE(
              "D3D12RenderTargetCache: Failed to create the {} {}-sample "
              "render target clearing pipeline",
              i ? "k_32_32_FLOAT" : "k_32_FLOAT", uint32_t(1) << j);
          Shutdown();
          return false;
        }
        uint32_rtv_clear_pipelines_[i][j] = uint32_rtv_clear_pipeline;
        std::wstring uint32_rtv_clear_pipeline_name =
            fmt::format(L"Resolve Clear {} {}xMSAA",
                        i ? L"k_32_32_FLOAT" : L"k_32_FLOAT", uint32_t(1) << j);
        uint32_rtv_clear_pipeline->SetName(
            reinterpret_cast<LPCWSTR>(uint32_rtv_clear_pipeline_name.c_str()));
      }
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel shader interlock (rasterizer-ordered view).

    // Piecewise linear gamma is 8-bit with programmable blending.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // Only ForcedSampleCount, which doesn't support 2x.
    msaa_2x_supported_ = false;

    // Create the resolve EDRAM buffer clearing root signature.
    std::array<D3D12_ROOT_PARAMETER, 2> resolve_rov_clear_root_parameters;
    // Parameter 0 is constants.
    resolve_rov_clear_root_parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_rov_clear_root_parameters[0].Constants.ShaderRegister = 0;
    resolve_rov_clear_root_parameters[0].Constants.RegisterSpace = 0;
    // Binding all of the shared memory at 1x resolution, portions with scaled
    // resolution.
    resolve_rov_clear_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_util::ResolveClearShaderConstants) / sizeof(uint32_t);
    resolve_rov_clear_root_parameters[0].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
    // Parameter 1 is the destination (EDRAM).
    resolve_rov_clear_root_parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_UAV;
    resolve_rov_clear_root_parameters[1].Descriptor.ShaderRegister = 0;
    resolve_rov_clear_root_parameters[1].Descriptor.RegisterSpace = 0;
    resolve_rov_clear_root_parameters[1].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolve_rov_clear_root_signature_desc;
    resolve_rov_clear_root_signature_desc.NumParameters =
        UINT(resolve_rov_clear_root_parameters.size());
    resolve_rov_clear_root_signature_desc.pParameters =
        resolve_rov_clear_root_parameters.data();
    resolve_rov_clear_root_signature_desc.NumStaticSamplers = 0;
    resolve_rov_clear_root_signature_desc.pStaticSamplers = nullptr;
    resolve_rov_clear_root_signature_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_NONE;
    resolve_rov_clear_root_signature_ = ui::d3d12::util::CreateRootSignature(
        provider, resolve_rov_clear_root_signature_desc);
    if (resolve_rov_clear_root_signature_ == nullptr) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear root signature");
      Shutdown();
      return false;
    }

    // Create the resolve EDRAM buffer clearing pipelines.
    const void* resolve_clear_32bpp_cs =
        draw_resolution_scaled
            ? static_cast<const void*>(shaders::resolve_clear_32bpp_scaled_cs)
            : static_cast<const void*>(shaders::resolve_clear_32bpp_cs);
    size_t resolve_clear_32bpp_cs_size =
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs);
    resolve_rov_clear_32bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, resolve_clear_32bpp_cs, resolve_clear_32bpp_cs_size,
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_32bpp_pipeline_ == nullptr) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_32bpp_pipeline_->SetName(L"Resolve Clear 32bpp");
    const void* resolve_clear_64bpp_cs =
        draw_resolution_scaled
            ? static_cast<const void*>(shaders::resolve_clear_64bpp_scaled_cs)
            : static_cast<const void*>(shaders::resolve_clear_64bpp_cs);
    size_t resolve_clear_64bpp_cs_size =
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs);
    resolve_rov_clear_64bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device, resolve_clear_64bpp_cs, resolve_clear_64bpp_cs_size,
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_64bpp_pipeline_ == nullptr) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_64bpp_pipeline_->SetName(L"Resolve Clear 64bpp");
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  InitializeCommon();

  return true;
}

void D3D12RenderTargetCache::Shutdown(bool from_destructor) {
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_64bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_32bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_root_signature_);

  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = size_t(xenos::MsaaSamples::k1X);
         j <= size_t(xenos::MsaaSamples::k4X); ++j) {
      ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_pipelines_[i][j]);
    }
  }
  ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_root_signature_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->Release();
    }
  }
  dump_pipelines_.clear();
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_depth_);
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_color_);

  for (const auto& transfer_pipeline_array_pair :
       transfer_stencil_bit_pipelines_) {
    for (ID3D12PipelineState* transfer_pipeline :
         transfer_pipeline_array_pair.second) {
      if (transfer_pipeline) {
        transfer_pipeline->Release();
      }
    }
  }
  transfer_stencil_bit_pipelines_.clear();
  for (const auto& transfer_pipeline_pair : transfer_pipelines_) {
    if (transfer_pipeline_pair.second) {
      transfer_pipeline_pair.second->Release();
    }
  }
  transfer_pipelines_.clear();
  for (size_t i = 0; i < xe::countof(transfer_root_signatures_); ++i) {
    ui::d3d12::util::ReleaseAndNull(transfer_root_signatures_[i]);
  }

  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(host_depth_store_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(host_depth_store_root_signature_);

  null_rtv_descriptor_ms_.Free();
  null_rtv_descriptor_ss_.Free();
  descriptor_pool_srv_.reset();
  descriptor_pool_depth_.reset();
  descriptor_pool_color_.reset();

  for (size_t i = 0; i < xe::countof(resolve_copy_native_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(resolve_copy_native_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(resolve_copy_native_root_signature_);
  for (size_t i = 0; i < xe::countof(resolve_copy_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(resolve_copy_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(resolve_copy_root_signature_);

  edram_snapshot_restore_pool_.reset();
  ui::d3d12::util::ReleaseAndNull(edram_snapshot_download_buffer_);

  ui::d3d12::util::ReleaseAndNull(edram_buffer_descriptor_heap_);
  ui::d3d12::util::ReleaseAndNull(edram_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12RenderTargetCache::CompletedSubmissionUpdated() {
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->Reclaim(
        command_processor_.GetCompletedSubmission());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(
        command_processor_.GetCompletedSubmission());
  }
}

void D3D12RenderTargetCache::BeginSubmission() {
  // New command list - render targets not bound.
  InvalidateCommandListRenderTargets();
  // ExecuteCommandLists is a full UAV barrier.
  if (edram_buffer_modification_status_ !=
      EdramBufferModificationStatus::kUnmodified) {
    assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    edram_buffer_modification_status_ =
        EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
  }
}

bool D3D12RenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");
  if (!RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }
  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       depth_and_color_render_targets,
                                       last_update_transfers());
      SetCommandListRenderTargets(depth_and_color_render_targets);
    } break;
    case Path::kPixelShaderInterlock: {
      // For ROV, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for ROV drawing now.
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // Commit preceding UAV (but not ROV) writes like clears as they aren't
      // synchronized with ROV accesses.
      CommitEdramBufferUAVWrites(EdramBufferModificationStatus::kAsUAV);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kAsROV);
    } break;
    default:
      assert_unhandled_case(GetPath());
      return false;
  }
  return true;
}

void D3D12RenderTargetCache::WriteEdramRawSRVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramRawUAVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_,
          uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2SRVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2UAVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12RenderTargetCache::Resolve(const Memory& memory,
                                     D3D12SharedMemory& shared_memory,
                                     D3D12TextureCache& texture_cache,
                                     uint32_t& written_address_out,
                                     uint32_t& written_length_out,
                                     reg::RB_COPY_DEST_INFO* copy_dest_info_out,
                                     bool* written_scaled_out) {
  SCOPE_profile_cpu_f("gpu");
  written_address_out = 0;
  written_length_out = 0;
  if (written_scaled_out) {
    *written_scaled_out = false;
  }

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  bool fixed_16_truncated_to_minus_1_to_1 = IsFixed16TruncatedToMinus1To1();
  if (!draw_util::GetResolveInfo(
          register_file(), memory, trace_writer_, draw_resolution_scale_x(),
          draw_resolution_scale_y(), fixed_16_truncated_to_minus_1_to_1,
          fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
    return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    if (command_processor_.debug_markers_enabled() || cvars::log_resolves) {
      char label[draw_util::kDebugMarkerLabelMaxLength];
      draw_util::FormatResolveCopyDebugMarker(label, sizeof(label),
                                              resolve_info);
      command_processor_.PushDebugMarker("%s", label);
    }
    // If everything owning the source is native, copy at 1x1 into shared
    // memory.
    bool copy_native = false;
    uint32_t dump_base = 0;
    uint32_t dump_row_length_used = 0;
    uint32_t dump_rows = 0;
    uint32_t dump_pitch = 0;
    if (GetPath() == Path::kHostRenderTargets) {
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
      copy_native = IsResolveSourceNativeOnly(dump_base, dump_row_length_used,
                                              dump_rows, dump_pitch);
      if (copy_native) {
        // Redo the resolve info at 1x1 so the scale-dependent fields match
        // what the unscaled copy shaders expect.
        if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_,
                                       1, 1, fixed_16_truncated_to_minus_1_to_1,
                                       fixed_16_truncated_to_minus_1_to_1,
                                       resolve_info)) {
          return false;
        }
      }
    }
    bool copy_dest_scaled = draw_resolution_scaled && !copy_native;

    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
        copy_native ? 1 : draw_resolution_scale_x(),
        copy_native ? 1 : draw_resolution_scale_y(), copy_shader_constants,
        copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);

    bool resolved_directly = false;
    if (GetPath() == Path::kHostRenderTargets) {
      // Read the render targets straight into shared memory where the copy
      // wouldn't have converted anything, otherwise dump the current contents
      // of the ones owning the affected range to edram_buffer_ for it.
      if (cvars::direct_host_resolve &&
          GetDirectResolveEligibility(resolve_info, copy_shader) ==
              DirectResolveEligibility::kEligible) {
        resolved_directly = DirectResolveRenderTargets(
            resolve_info, copy_shader_constants, dump_base,
            dump_row_length_used, dump_rows, dump_pitch, copy_dest_scaled,
            shared_memory, texture_cache);
      }
      if (!resolved_directly) {
        DumpRenderTargets(dump_base, dump_row_length_used, dump_rows,
                          dump_pitch, copy_native);
      }
    }

    if (resolved_directly) {
      // Invalidate textures and mark the range as scaled if needed.
      texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                        resolve_info.copy_dest_extent_length,
                                        copy_dest_scaled);
      written_address_out = resolve_info.copy_dest_extent_start;
      written_length_out = resolve_info.copy_dest_extent_length;
      if (copy_dest_info_out) {
        *copy_dest_info_out = resolve_info.copy_dest_info;
      }
      if (written_scaled_out) {
        *written_scaled_out = copy_dest_scaled;
      }
      copied = true;
    } else if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (copy_dest_scaled) {
        // Committing starting with the beginning of the potentially written
        // extent, but making the buffer containing the base current as the
        // beginning of the bound buffer is the base.
        copy_dest_committed = texture_cache.EnsureScaledResolveMemoryCommitted(
                                  resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length) &&
                              texture_cache.MakeScaledResolveRangeCurrent(
                                  resolve_info.copy_dest_base,
                                  resolve_info.copy_dest_extent_start -
                                      resolve_info.copy_dest_base +
                                      resolve_info.copy_dest_extent_length);
      } else {
        copy_dest_committed =
            shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                       resolve_info.copy_dest_extent_length);
      }
      if (copy_dest_committed) {
        command_list.D3DSetComputeRootSignature(
            copy_native ? resolve_copy_native_root_signature_
                        : resolve_copy_root_signature_);

        // Source.
        TransitionEdramBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_list.D3DSetComputeRootShaderResourceView(
            2, edram_buffer_gpu_address_);

        // Destination and constants.
        if (copy_dest_scaled) {
          texture_cache.TransitionCurrentScaledResolveRange(
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          command_list.D3DSetComputeRootUnorderedAccessView(
              1, texture_cache.GetCurrentScaledResolveRangeGPUAddress());

          command_list.D3DSetComputeRoot32BitConstants(
              0, sizeof(copy_shader_constants.dest_relative) / sizeof(uint32_t),
              &copy_shader_constants.dest_relative, 0);
        } else {
          shared_memory.UseForWriting();
          command_list.D3DSetComputeRootUnorderedAccessView(
              1, shared_memory.GetGPUAddress());

          command_list.D3DSetComputeRoot32BitConstants(
              0, sizeof(copy_shader_constants) / sizeof(uint32_t),
              &copy_shader_constants, 0);
        }

        // Dispatch the resolve.
        command_processor_.SetExternalPipeline(
            copy_native ? resolve_copy_native_pipelines_[size_t(copy_shader)]
                        : resolve_copy_pipelines_[size_t(copy_shader)]);
        command_processor_.SubmitBarriers();
        command_list.D3DDispatch(copy_group_count_x, copy_group_count_y, 1);

        // Order the resolve with other work using the destination as a UAV.
        if (copy_dest_scaled) {
          texture_cache.MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
        } else {
          shared_memory.MarkUAVWritesCommitNeeded();
        }

        // Invalidate textures and mark the range as scaled if needed.
        texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                          resolve_info.copy_dest_extent_length,
                                          copy_dest_scaled);
        written_address_out = resolve_info.copy_dest_extent_start;
        written_length_out = resolve_info.copy_dest_extent_length;
        if (copy_dest_info_out) {
          // Normalized copy format (depth format for depth resolves) - the
          // texel size the readback downscale expects for the extent.
          *copy_dest_info_out = resolve_info.copy_dest_info;
        }
        if (written_scaled_out) {
          *written_scaled_out = copy_dest_scaled;
        }
        copied = true;
      } else {
        XELOGE(
            "D3D12RenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      }
    }
    command_processor_.PopDebugMarker();
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    if (command_processor_.debug_markers_enabled() || cvars::log_resolves) {
      char label[draw_util::kDebugMarkerLabelMaxLength];
      draw_util::FormatResolveClearDebugMarker(
          label, sizeof(label), resolve_info, clear_depth, clear_color);
      command_processor_.PushDebugMarker("%s", label);
    }
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(
                resolve_info, clear_rectangle, clear_render_targets[0],
                clear_transfers_[0], clear_render_targets[1],
                clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          // For 64bpp formats, RB_COLOR_CLEAR_LO is the lower 32 bits of the
          // packed clear value. RB_COLOR_CLEAR is the upper 32 bits and, for
          // 32bpp formats, the whole value.
          clear_values[1] =
              resolve_info.color_edram_info.format_is_64bpp
                  ? resolve_info.rb_color_clear_lo |
                        (uint64_t(resolve_info.rb_color_clear) << 32)
                  : resolve_info.rb_color_clear;
          PerformTransfersAndResolveClears(2, clear_render_targets,
                                           clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Should be safe to only commit once (if was UAV / ROV previously - if
        // there was nothing to copy, only to clear, for some reason, for
        // instance), overlap of the depth and the color ranges is highly
        // unlikely.
        CommitEdramBufferUAVWrites();
        command_list.D3DSetComputeRootSignature(
            resolve_rov_clear_root_signature_);
        command_list.D3DSetComputeRootUnorderedAccessView(
            1, edram_buffer_gpu_address_);
        std::pair<uint32_t, uint32_t> clear_group_count =
            resolve_info.GetClearShaderGroupCount(draw_resolution_scale_x(),
                                                  draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          draw_util::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_list.D3DSetComputeRoot32BitConstants(
              0, sizeof(depth_clear_constants) / sizeof(uint32_t),
              &depth_clear_constants, 0);
          command_processor_.SetExternalPipeline(
              resolve_rov_clear_32bpp_pipeline_);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(clear_group_count.first,
                                   clear_group_count.second, 1);
        }
        if (clear_color) {
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(color_clear_constants.rt_specific) / sizeof(uint32_t),
                &color_clear_constants.rt_specific,
                offsetof(draw_util::ResolveClearShaderConstants, rt_specific) /
                    sizeof(uint32_t));
          } else {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(color_clear_constants) / sizeof(uint32_t),
                &color_clear_constants, 0);
          }
          command_processor_.SetExternalPipeline(
              resolve_info.color_edram_info.format_is_64bpp
                  ? resolve_rov_clear_64bpp_pipeline_
                  : resolve_rov_clear_32bpp_pipeline_);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(clear_group_count.first,
                                   clear_group_count.second, 1);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
    command_processor_.PopDebugMarker();
  } else {
    cleared = true;
  }

  return copied && cleared;
}

void D3D12RenderTargetCache::DumpAllRenderTargetsToEdram() {
  DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount,
                    false);
}

bool D3D12RenderTargetCache::BeginEdramSnapshotReadback() {
  if (!edram_snapshot_download_buffer_) {
    D3D12_RESOURCE_DESC edram_snapshot_download_buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(edram_snapshot_download_buffer_desc,
                                            xenos::kEdramSizeBytes,
                                            D3D12_RESOURCE_FLAG_NONE);
    const ui::d3d12::D3D12Provider& provider =
        command_processor_.GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback,
            provider.GetHeapFlagCreateNotZeroed(),
            &edram_snapshot_download_buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&edram_snapshot_download_buffer_)))) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create a EDRAM snapshot download "
          "buffer");
      return false;
    }
  }
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      edram_snapshot_download_buffer_, 0, edram_buffer_, 0,
      xenos::kEdramSizeBytes);
  return true;
}

const void* D3D12RenderTargetCache::MapEdramSnapshotReadback() {
  if (!edram_snapshot_download_buffer_) {
    return nullptr;
  }
  void* download_mapping;
  if (FAILED(edram_snapshot_download_buffer_->Map(0, nullptr,
                                                  &download_mapping))) {
    return nullptr;
  }
  edram_snapshot_download_mapped_ = true;
  return download_mapping;
}

void D3D12RenderTargetCache::EndEdramSnapshotReadback() {
  if (!edram_snapshot_download_buffer_) {
    return;
  }
  if (edram_snapshot_download_mapped_) {
    D3D12_RANGE download_write_range = {};
    edram_snapshot_download_buffer_->Unmap(0, &download_write_range);
    edram_snapshot_download_mapped_ = false;
  }
  edram_snapshot_download_buffer_->Release();
  edram_snapshot_download_buffer_ = nullptr;
}

void D3D12RenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return;
  }

  // Create the buffer - will be used for copying to either a 32-bit 1280x2048
  // render target or the EDRAM buffer.
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  if (!edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_ =
        std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
            provider, xenos::kEdramSizeBytes);
  }
  ID3D12Resource* upload_buffer;
  size_t upload_buffer_offset;
  void* upload_buffer_mapping = edram_snapshot_restore_pool_->Request(
      command_processor_.GetCurrentSubmission(), xenos::kEdramSizeBytes, 1,
      &upload_buffer, &upload_buffer_offset, nullptr);
  if (!upload_buffer_mapping) {
    XELOGE(
        "D3D12RenderTargetCache: Failed to get a buffer for restoring a EDRAM "
        "snapshot");
    return;
  }

  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      // k_32_FLOAT because it's unambiguous (not effected by something like
      // DXGI_FORMAT_R8G8B8A8 vs. DXGI_FORMAT_B8G8R8A8).
      D3D12RenderTarget* full_edram_render_target =
          static_cast<D3D12RenderTarget*>(
              PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
                  xenos::ColorRenderTargetFormat::k_32_FLOAT));
      if (!full_edram_render_target) {
        return;
      }
      D3D12_TEXTURE_COPY_LOCATION copy_source_location;
      copy_source_location.pResource = upload_buffer;
      copy_source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      UINT64 copy_total_bytes;
      D3D12_RESOURCE_DESC full_edram_render_target_desc =
          full_edram_render_target->resource()->GetDesc();
      provider.GetDevice()->GetCopyableFootprints(
          &full_edram_render_target_desc, 0, 1, 0,
          &copy_source_location.PlacedFootprint, nullptr, nullptr,
          &copy_total_bytes);
      // 1280 width * sizeof(uint32_t) is aligned to
      // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
      assert_true(copy_total_bytes <= xenos::kEdramSizeBytes);
      assert_false(full_edram_render_target->key().Is64bpp());
      uint32_t pitch_tiles =
          full_edram_render_target->key().pitch_tiles_at_32bpp;
      uint32_t tile_rows = xenos::kEdramTileCount / pitch_tiles;
      assert_true(pitch_tiles * tile_rows == xenos::kEdramTileCount);
      const uint8_t* snapshot_sample_row =
          reinterpret_cast<const uint8_t*>(snapshot);
      for (uint32_t y_tile = 0; y_tile < tile_rows; ++y_tile) {
        uint8_t* upload_buffer_tile_row_origin =
            reinterpret_cast<uint8_t*>(upload_buffer_mapping) +
            copy_source_location.PlacedFootprint.Offset +
            xenos::kEdramTileHeightSamples * y_tile *
                copy_source_location.PlacedFootprint.Footprint.RowPitch;
        for (uint32_t x_tile = 0; x_tile < pitch_tiles; ++x_tile) {
          uint8_t* upload_buffer_sample_row =
              upload_buffer_tile_row_origin +
              sizeof(uint32_t) * xenos::kEdramTileWidthSamples * x_tile;
          for (uint32_t sample_row = 0;
               sample_row < xenos::kEdramTileHeightSamples; ++sample_row) {
            std::memcpy(upload_buffer_sample_row, snapshot_sample_row,
                        sizeof(uint32_t) * xenos::kEdramTileWidthSamples);
            snapshot_sample_row +=
                sizeof(uint32_t) * xenos::kEdramTileWidthSamples;
            upload_buffer_sample_row +=
                copy_source_location.PlacedFootprint.Footprint.RowPitch;
          }
        }
      }
      command_processor_.PushTransitionBarrier(
          full_edram_render_target->resource(),
          full_edram_render_target->SetResourceState(
              D3D12_RESOURCE_STATE_COPY_DEST),
          D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      D3D12_TEXTURE_COPY_LOCATION copy_dest_location;
      copy_dest_location.pResource = full_edram_render_target->resource();
      copy_dest_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      copy_dest_location.SubresourceIndex = 0;
      command_list.D3DCopyTextureRegion(&copy_dest_location, 0, 0, 0,
                                        &copy_source_location, nullptr);
    } break;

    case Path::kPixelShaderInterlock: {
      std::memcpy(upload_buffer_mapping, snapshot, xenos::kEdramSizeBytes);
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      command_list.D3DCopyBufferRegion(edram_buffer_, 0, upload_buffer,
                                       UINT64(upload_buffer_offset),
                                       xenos::kEdramSizeBytes);
    } break;

    default:
      assert_unhandled_case(GetPath());
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorResourceDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  // Typed should be preferred over typeless so there are more opportunities for
  // compression.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? DXGI_FORMAT_R16G16B16A16_UNORM
                                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SNORM has two representations of -1.
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // Floating-point - ensure NaN propagation during ownership transfer for
    // unmodified data.
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // TODO(Triang3l): Check if NaN propagation defined in the D3D11.3
    // specification can be relied on for 32-bit float render targets.
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorDrawDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return GetColorResourceDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorOwnershipTransferDXGIFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorDrawDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthResourceDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthDSVDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

bool D3D12RenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

RenderTargetCache::RenderTarget* D3D12RenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  D3D12_RESOURCE_DESC resource_desc;
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Alignment = 0;
  resource_desc.Width = key.GetWidth() * GetKeyScaleX(key);
  resource_desc.Height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) *
      GetKeyScaleY(key);
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  if (key.is_depth) {
    resource_desc.Format = GetDepthResourceDXGIFormat(key.GetDepthFormat());
  } else {
    resource_desc.Format = GetColorResourceDXGIFormat(key.GetColorFormat());
  }
  assert_true(resource_desc.Format != DXGI_FORMAT_UNKNOWN);
  if (resource_desc.Format == DXGI_FORMAT_UNKNOWN) {
    XELOGE("D3D12RenderTargetCache: Unknown {} render target format {}",
           key.is_depth ? "depth" : "color", key.resource_format);
    return nullptr;
  }
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported()) {
    resource_desc.SampleDesc.Count = 4;
  } else {
    resource_desc.SampleDesc.Count = UINT(1) << UINT(key.msaa_samples);
  }
  resource_desc.SampleDesc.Quality = 0;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resource_desc.Flags = key.is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // The first access will be ownership transfer into this render target or
  // starting to draw directly.
  D3D12_RESOURCE_STATES resource_state =
      key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_CLEAR_VALUE optimized_clear_value;
  if (key.is_depth) {
    optimized_clear_value.Format = GetDepthDSVDXGIFormat(key.GetDepthFormat());
    // Fixed-point depth is generally direct (1 being the farthest),
    // floating-point is used for more uniform precision across the range (0
    // being the farthest).
    optimized_clear_value.DepthStencil.Depth =
        key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f
                                                                       : 0.0f;
    optimized_clear_value.DepthStencil.Stencil = 0;
  } else {
    optimized_clear_value.Format = GetColorDrawDXGIFormat(key.GetColorFormat());
    optimized_clear_value.Color[0] = 0.0f;
    optimized_clear_value.Color[1] = 0.0f;
    optimized_clear_value.Color[2] = 0.0f;
    optimized_clear_value.Color[3] = 0.0f;
  }
  // Create zeroed for more determinism, primarily with respect to compression
  // and depth float24 / float32 mirroring.
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &resource_desc, resource_state, &optimized_clear_value,
          IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  {
    std::u16string resource_name = xe::to_utf16(key.GetDebugName());
    resource->SetName(reinterpret_cast<LPCWSTR>(resource_name.c_str()));
  }

  ui::d3d12::D3D12CpuDescriptorPool& descriptor_pool =
      key.is_depth ? *descriptor_pool_depth_ : *descriptor_pool_color_;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_draw =
      descriptor_pool.AllocateDescriptor();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv =
      descriptor_pool_srv_->AllocateDescriptor();
  if (!descriptor_draw.IsValid() || !descriptor_srv.IsValid()) {
    return nullptr;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor_draw_handle =
      descriptor_draw.GetHandle();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_load_separate;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_stencil;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (resource_desc.SampleDesc.Count > 1) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
  }
  if (key.is_depth) {
    // DSV and stencil SRV.
    descriptor_srv_stencil = descriptor_pool_srv_->AllocateDescriptor();
    if (!descriptor_srv_stencil.IsValid()) {
      return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Format = optimized_clear_value.Format;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    D3D12_SHADER_RESOURCE_VIEW_DESC stencil_srv_desc;
    stencil_srv_desc.Format =
        GetDepthSRVStencilDXGIFormat(key.GetDepthFormat());
    // X24_TYPELESS_G8_UINT puts stencil in G, but the shared SPIR-V emitters
    // read component 0 like a Vulkan stencil aspect view, so route G there.
    stencil_srv_desc.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 1, 1, 1);
    if (resource_desc.SampleDesc.Count > 1) {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dsv_desc.Texture2D.MipSlice = 0;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      stencil_srv_desc.Texture2D.MostDetailedMip = 0;
      stencil_srv_desc.Texture2D.MipLevels = 1;
      stencil_srv_desc.Texture2D.PlaneSlice = 1;
      stencil_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    device->CreateDepthStencilView(resource.Get(), &dsv_desc,
                                   descriptor_draw_handle);
    device->CreateShaderResourceView(resource.Get(), &stencil_srv_desc,
                                     descriptor_srv_stencil.GetHandle());
    // Depth SRV.
    srv_desc.Format = GetDepthSRVDepthDXGIFormat(key.GetDepthFormat());
  } else {
    // Drawing RTV.
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    rtv_desc.Format = optimized_clear_value.Format;
    if (resource_desc.SampleDesc.Count > 1) {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv_desc.Texture2D.MipSlice = 0;
      rtv_desc.Texture2D.PlaneSlice = 0;
    }
    device->CreateRenderTargetView(resource.Get(), &rtv_desc,
                                   descriptor_draw_handle);
    // Ownership transfer RTV.
    DXGI_FORMAT load_format =
        GetColorOwnershipTransferDXGIFormat(key.GetColorFormat());
    if (rtv_desc.Format != load_format) {
      descriptor_load_separate = descriptor_pool.AllocateDescriptor();
      if (!descriptor_load_separate.IsValid()) {
        return nullptr;
      }
      rtv_desc.Format = load_format;
      device->CreateRenderTargetView(resource.Get(), &rtv_desc,
                                     descriptor_load_separate.GetHandle());
    }
    // SRV for ownership transfer and dumping.
    srv_desc.Format = load_format;
  }
  device->CreateShaderResourceView(resource.Get(), &srv_desc,
                                   descriptor_srv.GetHandle());

  return new D3D12RenderTarget(
      key, resource.Get(), std::move(descriptor_draw),
      std::move(descriptor_load_separate), std::move(descriptor_srv),
      std::move(descriptor_srv_stencil), resource_state);
}

bool D3D12RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

void D3D12RenderTargetCache::RequestPixelShaderInterlockBarrier() {
  CommitEdramBufferUAVWrites();
}

void D3D12RenderTargetCache::TransitionEdramBuffer(
    D3D12_RESOURCE_STATES new_state) {
  if (command_processor_.PushTransitionBarrier(
          edram_buffer_, edram_buffer_state_, new_state)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted - in particular, not resetting it for UAV > UAV as
    // barriers are dropped if the state hasn't been changed.
    edram_buffer_modification_status_ =
        EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_state_ = new_state;
}

void D3D12RenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status !=
              EdramBufferModificationStatus::kUnmodified);
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    return;
  }
  // max because being modified as a UAV requires stricter synchronization than
  // as ROV.
  edram_buffer_modification_status_ =
      std::max(edram_buffer_modification_status_, modification_status);
}

void D3D12RenderTargetCache::CommitEdramBufferUAVWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    command_processor_.PushUAVBarrier(edram_buffer_);
  }
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

D3D12RenderTargetCache::TransferRootSignatureInfo
D3D12RenderTargetCache::GetTransferRootSignatureInfo(
    EdramTransferMode mode) const {
  const EdramTransferModeInfo& mode_info = kEdramTransferModes[size_t(mode)];
  uint32_t used_sets =
      kEdramTransferPipelineLayoutInfos[size_t(mode_info.pipeline_layout)]
          .used_descriptor_sets;
  // The emitter numbers the sets it uses densely, in the order of their bits,
  // and Mesa gives each set the register space of the same number.
  auto space_of = [used_sets](uint32_t set_bit) {
    return xe::bit_count(used_sets & (set_bit - 1));
  };
  TransferRootSignatureInfo info = {};
  // Every mode reads at least the address.
  info.used_root_parameters = kTransferUsedRootParameterPushConstantsBit;
  if (used_sets & kEdramTransferUsedDescriptorSetColorTextureBit) {
    info.used_root_parameters |= kTransferUsedRootParameterColorSRVBit;
    info.space_source =
        space_of(kEdramTransferUsedDescriptorSetColorTextureBit);
  }
  if (used_sets & kEdramTransferUsedDescriptorSetDepthStencilTexturesBit) {
    info.space_source =
        space_of(kEdramTransferUsedDescriptorSetDepthStencilTexturesBit);
    // The emitter leaves out the depth texture when only a stencil bit is
    // written, and the stencil texture when only depth is - unless the stencil
    // rides along in SV_StencilRef.
    if (mode_info.output != EdramTransferOutput::kStencilBit) {
      info.used_root_parameters |= kTransferUsedRootParameterDepthSRVBit;
    }
    if (mode_info.output != EdramTransferOutput::kDepth ||
        use_stencil_reference_output_) {
      info.used_root_parameters |= kTransferUsedRootParameterStencilSRVBit;
    }
  }
  if (used_sets & kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    info.used_root_parameters |= kTransferUsedRootParameterHostDepthSRVBit;
    info.space_host_depth =
        space_of(kEdramTransferUsedDescriptorSetHostDepthStencilTexturesBit);
  }
  if (used_sets & kEdramTransferUsedDescriptorSetHostDepthBufferBit) {
    info.used_root_parameters |= kTransferUsedRootParameterHostDepthBufferBit;
    info.space_host_depth =
        space_of(kEdramTransferUsedDescriptorSetHostDepthBufferBit);
  }
  return info;
}

ID3D12PipelineState* const*
D3D12RenderTargetCache::GetOrCreateTransferPipelines(
    EdramTransferShaderKey key) {
  const EdramTransferModeInfo& mode = kEdramTransferModes[size_t(key.mode)];
  bool dest_is_stencil_bit = (mode.output == EdramTransferOutput::kStencilBit);

  if (dest_is_stencil_bit) {
    auto pipelines_it = transfer_stencil_bit_pipelines_.find(key);
    if (pipelines_it != transfer_stencil_bit_pipelines_.end()) {
      return pipelines_it->second[0] ? pipelines_it->second.data() : nullptr;
    }
  } else {
    auto pipeline_it = transfer_pipelines_.find(key);
    if (pipeline_it != transfer_pipelines_.end()) {
      return pipeline_it->second ? &pipeline_it->second : nullptr;
    }
  }

  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  bool source_is_color = EdramTransferSourceIsColor(key.mode);
  bool dest_is_color = (mode.output == EdramTransferOutput::kColor);

  EdramTransferShaderOptions options;
  // The host depth buffer is declared with the pre-1.3 BufferBlock and Uniform
  // forms, like the dump shader's EDRAM buffer.
  options.spirv_version = 0x00010000;
  options.resolution_scale_x = draw_resolution_scale_x();
  options.resolution_scale_y = draw_resolution_scale_y();
  options.msaa_2x_attachments_supported = msaa_2x_supported_;
  // The emitter only needs to know whether each side is an integer texture,
  // which is this backend's own format policy.
  if (source_is_color) {
    GetColorOwnershipTransferDXGIFormat(source_color_format,
                                        &options.source_color_is_uint);
  }
  if (dest_is_color) {
    GetColorOwnershipTransferDXGIFormat(dest_color_format,
                                        &options.dest_color_is_uint);
  }
  options.stencil_reference_output_supported = use_stencil_reference_output_;
  // A pixel shader reading SV_SampleIndex runs per sample on Direct3D 12, and
  // the sample mask is pipeline state, so neither of the per-draw-sample
  // fallbacks is needed.
  options.sample_rate_shading_supported = true;
  options.depth_float24_round = depth_float24_round();
  options.depth_float24_convert_in_pixel_shader =
      depth_float24_convert_in_pixel_shader();
  options.no_discard_stencil = cvars::no_discard_stencil_in_transfer_pipelines;

  std::vector<uint8_t> pixel_shader_dxil;
  {
    std::vector<uint32_t> spirv = BuildEdramTransferShaderSpirv(key, options);
    if (!spirv.empty()) {
      pixel_shader_dxil = SpirvToDxilCompiler::Translate(
          spirv.data(), spirv.size(), SpirvToDxilCompiler::Stage::kPixel);
      if (pixel_shader_dxil.empty()) {
        XELOGE(
            "D3D12RenderTargetCache: Failed to translate the render target "
            "ownership transfer shader 0x{:08X}",
            key.key);
      }
    } else {
      XELOGE(
          "D3D12RenderTargetCache: Failed to emit the render target ownership "
          "transfer shader 0x{:08X}",
          key.key);
    }
    if (pixel_shader_dxil.empty()) {
      // Store the null pointers not to try to build it again.
      if (dest_is_stencil_bit) {
        transfer_stencil_bit_pipelines_.emplace(
            std::piecewise_construct, std::make_tuple(key), std::make_tuple());
      } else {
        transfer_pipelines_.emplace(key, nullptr);
      }
      return nullptr;
    }
  }

  ID3D12PipelineState* const* pipelines;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_INPUT_ELEMENT_DESC pipeline_input_element_desc;
  pipeline_input_element_desc.SemanticName = "POSITION";
  pipeline_input_element_desc.SemanticIndex = 0;
  pipeline_input_element_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
  pipeline_input_element_desc.InputSlot = 0;
  pipeline_input_element_desc.AlignedByteOffset = 0;
  pipeline_input_element_desc.InputSlotClass =
      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  pipeline_input_element_desc.InstanceDataStepRate = 0;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};
  pipeline_desc.pRootSignature = transfer_root_signatures_[size_t(key.mode)];
  pipeline_desc.VS.pShaderBytecode = shaders::passthrough_position_xy_vs;
  pipeline_desc.VS.BytecodeLength = sizeof(shaders::passthrough_position_xy_vs);
  pipeline_desc.PS.pShaderBytecode = pixel_shader_dxil.data();
  pipeline_desc.PS.BytecodeLength = UINT(pixel_shader_dxil.size());
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead.
    pipeline_desc.SampleMask = 0b1001;
    pipeline_desc.SampleDesc.Count = 4;
  } else {
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.SampleDesc.Count = UINT(1) << UINT(key.dest_msaa_samples);
  }
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthClipEnable = true;
  pipeline_desc.InputLayout.pInputElementDescs = &pipeline_input_element_desc;
  pipeline_desc.InputLayout.NumElements = 1;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (dest_is_stencil_bit) {
    pipeline_desc.DepthStencilState.StencilEnable = true;
    pipeline_desc.DepthStencilState.FrontFace.StencilFailOp =
        D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
        D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilPassOp =
        D3D12_STENCIL_OP_REPLACE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline_desc.DepthStencilState.BackFace =
        pipeline_desc.DepthStencilState.FrontFace;
    pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    // Even if creation fails, still store the null pointers not to try to
    // create again.
    std::array<ID3D12PipelineState*, 8>& stencil_bit_pipelines =
        transfer_stencil_bit_pipelines_
            .emplace(std::piecewise_construct, std::make_tuple(key),
                     std::make_tuple())
            .first->second;
    bool stencil_pipelines_created = true;
    for (uint32_t i = 0; i < 8; ++i) {
      pipeline_desc.DepthStencilState.StencilWriteMask = UINT8(1) << i;
      if (SUCCEEDED(device->CreateGraphicsPipelineState(
              &pipeline_desc, IID_PPV_ARGS(&stencil_bit_pipelines[i])))) {
        continue;
      }
      stencil_pipelines_created = false;
      for (uint32_t j = 0; j < i; ++j) {
        stencil_bit_pipelines[j]->Release();
        stencil_bit_pipelines[j] = nullptr;
      }
      break;
    }
    pipelines =
        stencil_pipelines_created ? stencil_bit_pipelines.data() : nullptr;
  } else {
    if (dest_is_color) {
      pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
          D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline_desc.NumRenderTargets = 1;
      pipeline_desc.RTVFormats[0] =
          GetColorOwnershipTransferDXGIFormat(dest_color_format);
    } else {
      pipeline_desc.DepthStencilState.DepthEnable = true;
      pipeline_desc.DepthStencilState.DepthWriteMask =
          D3D12_DEPTH_WRITE_MASK_ALL;
      pipeline_desc.DepthStencilState.DepthFunc =
          cvars::depth_transfer_not_equal_test ? D3D12_COMPARISON_FUNC_NOT_EQUAL
                                               : D3D12_COMPARISON_FUNC_ALWAYS;
      if (use_stencil_reference_output_) {
        pipeline_desc.DepthStencilState.StencilEnable = true;
        pipeline_desc.DepthStencilState.StencilWriteMask = UINT8_MAX;
        pipeline_desc.DepthStencilState.FrontFace.StencilFailOp =
            D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
            cvars::depth_transfer_not_equal_test ? D3D12_STENCIL_OP_REPLACE
                                                 : D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilPassOp =
            D3D12_STENCIL_OP_REPLACE;
        // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by
        // stencil being different.
        pipeline_desc.DepthStencilState.FrontFace.StencilFunc =
            D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline_desc.DepthStencilState.BackFace =
            pipeline_desc.DepthStencilState.FrontFace;
      }
      pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    }
    ID3D12PipelineState* pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline_desc,
                                                   IID_PPV_ARGS(&pipeline)))) {
      pipeline = nullptr;
    }
    // Even if creation fails, still store the null pointer not to try to create
    // again.
    // Return a pointer to the persistent location.
    ID3D12PipelineState*& inserted_pipeline =
        transfer_pipelines_.emplace(key, pipeline).first->second;
    pipelines = inserted_pipeline ? &inserted_pipeline : nullptr;
  }
  // TODO(Triang3l): Pipeline state name debug names (lots of variables - but
  // not very important since everything can be derived from the bindings and
  // outputs in a debugger).

  if (!pipelines) {
    const char* source_format_name =
        source_is_color
            ? xenos::GetColorRenderTargetFormatName(source_color_format)
            : xenos::GetDepthRenderTargetFormatName(source_depth_format);
    const char* dest_format_name =
        dest_is_color
            ? xenos::GetColorRenderTargetFormatName(dest_color_format)
            : xenos::GetDepthRenderTargetFormatName(dest_depth_format);
    if (EdramTransferUsesHostDepth(key.mode)) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} + {}-sample host depth{} -> "
          "{}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.host_depth_source_msaa_samples),
          EdramTransferHostDepthIsCopy(key.mode) ? " copy" : "",
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name,
          uint32_t(key.mode));
    } else {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} -> {}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name,
          uint32_t(key.mode));
    }
  }
  return pipelines;
}

void D3D12RenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  SCOPE_profile_cpu_f("gpu");
  assert_true(GetPath() == Path::kHostRenderTargets);

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;

  // Check if there's any actual work to do before pushing debug marker.
  bool has_transfers = false;
  for (uint32_t i = 0; i < render_target_count && !has_transfers; ++i) {
    if (render_targets[i] &&
        (!render_target_transfers[i].empty() || resolve_clear_needed)) {
      has_transfers = true;
    }
  }
  if (!has_transfers) {
    return;
  }

  LogTransfers(render_target_count, render_targets, render_target_transfers,
               resolve_clear_needed ? resolve_clear_rectangle : nullptr);

  command_processor_.PushDebugMarker("PerformTransfersAndResolveClears");

  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();

  D3D12_RECT clear_rect;
  if (resolve_clear_needed) {
    // All render targets of a clear share the pitch and the scale class.
    // Take the scale from whichever is there.
    uint32_t resolve_clear_scale_x = draw_resolution_scale_x();
    uint32_t resolve_clear_scale_y = draw_resolution_scale_y();
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (render_targets[i]) {
        resolve_clear_scale_x = GetKeyScaleX(render_targets[i]->key());
        resolve_clear_scale_y = GetKeyScaleY(render_targets[i]->key());
        break;
      }
    }
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    clear_rect.left =
        LONG(resolve_clear_rectangle->x_pixels * resolve_clear_scale_x);
    clear_rect.top =
        LONG(resolve_clear_rectangle->y_pixels * resolve_clear_scale_y);
    clear_rect.right = LONG((resolve_clear_rectangle->x_pixels +
                             resolve_clear_rectangle->width_pixels) *
                            resolve_clear_scale_x);
    clear_rect.bottom = LONG((resolve_clear_rectangle->y_pixels +
                              resolve_clear_rectangle->height_pixels) *
                             resolve_clear_scale_y);
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      assert_false(dest_rt_key.scale_native);
      if (!host_depth_store_set_up) {
        // Source descriptor.
        ui::d3d12::util::DescriptorCpuGpuHandlePair
            host_depth_store_descriptor_source;
        if (!command_processor_.RequestOneUseSingleViewDescriptors(
                1, &host_depth_store_descriptor_source)) {
          continue;
        }
        command_list.D3DSetComputeRootSignature(
            host_depth_store_root_signature_);
        // Destination (EDRAM buffer).
        command_list.D3DSetComputeRootUnorderedAccessView(
            kHostDepthStoreRootParameterDest, edram_buffer_gpu_address_);
        // Depth source texture.
        device->CopyDescriptorsSimple(
            1, host_depth_store_descriptor_source.first,
            dest_d3d12_rt.descriptor_srv().GetHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        command_list.D3DSetComputeRootDescriptorTable(
            kHostDepthStoreRootParameterSource,
            host_depth_store_descriptor_source.second);
        // Render target constant.
        HostDepthStoreRenderTargetConstant
            host_depth_store_render_target_constant =
                GetHostDepthStoreRenderTargetConstant(
                    dest_rt_key.pitch_tiles_at_32bpp, msaa_2x_supported_);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_render_target_constant) / sizeof(uint32_t),
            &host_depth_store_render_target_constant,
            offsetof(HostDepthStoreConstants, render_target) /
                sizeof(uint32_t));
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, NON_PIXEL_SHADER_RESOURCE -> DEPTH_WRITE will be done
        // anyway even in the best case, so it's not possible to have all the
        // barriers in one place here.
        TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Pipeline.
        command_processor_.SetExternalPipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle
          transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp,
          dest_rt_key.msaa_samples, false, transfer_rectangles,
          resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(
            transfer_rectangles[j], dest_rt_key.msaa_samples,
            host_depth_store_rectangle_constant, group_count_x, group_count_y);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_rectangle_constant) / sizeof(uint32_t),
            &host_depth_store_rectangle_constant,
            offsetof(HostDepthStoreConstants, rectangle) / sizeof(uint32_t));
        command_processor_.SubmitBarriers();
        command_list.D3DDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : render_target_transfers[i]) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(transfer.source);
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
        command_processor_.PushTransitionBarrier(
            host_depth_source_d3d12_rt.resource(),
            host_depth_source_d3d12_rt.SetResourceState(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt ||
            previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      D3D12_RESOURCE_STATES dest_state =
          dest_d3d12_rt.key().is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                       : D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_processor_.PushTransitionBarrier(
          dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state),
          dest_state);
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // Copy source descriptors to the shader-visible heap.
  // Clear previously set shader-visible descriptor indices.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      source_d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
      source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      if (host_depth_source_d3d12_rt) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(UINT32_MAX);
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndexStencil(
            UINT32_MAX);
      }
    }
  }
  current_temporary_descriptors_cpu_.clear();
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    bool dest_is_depth = dest_rt->key().is_depth;
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      if (source_d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.descriptor_srv().GetHandle());
      }
      if (source_d3d12_rt.key().is_depth &&
          source_d3d12_rt.temporary_srv_descriptor_index_stencil() ==
              UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.descriptor_srv_stencil().GetHandle());
      }
      bool source_is_depth = source_d3d12_rt.key().is_depth;
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      // The host_depth_source_d3d12_rt == dest_rt case would use the EDRAM
      // buffer instead.
      if (host_depth_source_d3d12_rt && host_depth_source_d3d12_rt != dest_rt &&
          host_depth_source_d3d12_rt->temporary_srv_descriptor_index() ==
              UINT32_MAX) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            host_depth_source_d3d12_rt->descriptor_srv().GetHandle());
      }
    }
  }
  uint32_t descriptor_count =
      uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return;
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1,
                                  current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  // Perform the transfers and clears.

  bool transfer_viewport_set = false;
  float pixels_to_ndc_unscaled =
      2.0f / float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);

  EdramTransferMode last_transfer_mode = EdramTransferMode::kCount;
  uint32_t transfer_root_parameters_set = 0;
  uint32_t last_descriptor_index_color = UINT32_MAX;
  uint32_t last_descriptor_index_depth = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  uint32_t last_descriptor_index_host_depth = UINT32_MAX;
  // The address and the host depth address are dwords of one root constants
  // parameter, so they're tracked apart from the descriptor tables' mask.
  bool address_constant_set = false;
  bool host_depth_address_constant_set = false;
  EdramTransferAddressConstant last_address_constant;
  EdramTransferAddressConstant last_host_depth_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();

    // Late barrier in case there was cross-copying that prevented merging of
    // barriers.
    D3D12_RESOURCE_STATES dest_state = dest_rt_key.is_depth
                                           ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_processor_.PushTransitionBarrier(
        dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state),
        dest_state);

    if (!current_transfers.empty()) {
      are_current_command_list_render_targets_valid_ = false;
      if (dest_rt_key.is_depth) {
        auto handle = dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(0, nullptr, false, &handle);
        if (!use_stencil_reference_output_) {
          command_processor_.SetStencilReference(UINT8_MAX);
        }
      } else {
        auto handle = dest_d3d12_rt.descriptor_load_separate().IsValid()
                          ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                          : dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(1, &handle, false, nullptr);
      }

      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();
      // GetRectangles returns guest pixels.
      // Scale to the destination.
      float pixels_to_ndc_x =
          pixels_to_ndc_unscaled * GetKeyScaleX(dest_rt_key);
      float pixels_to_ndc_y =
          pixels_to_ndc_unscaled * GetKeyScaleY(dest_rt_key);

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws =
          dest_rt_key.is_depth && !use_stencil_reference_output_;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(
          current_transfers.size() << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      EdramTransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format =
          dest_rt_key.resource_format;
      new_transfer_shader_key.dest_scale_native = dest_rt_key.scale_native;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto* host_depth_source_d3d12_rt =
              static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_d3d12_rt =
              *static_cast<D3D12RenderTarget*>(transfer.source);
          source_d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_d3d12_rt =
              *static_cast<D3D12RenderTarget*>(transfer.source);
          D3D12RenderTarget* host_depth_source_d3d12_rt =
              j ? nullptr
                : static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt &&
              host_depth_source_d3d12_rt->temporary_sort_index() ==
                  UINT32_MAX) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_d3d12_rt.temporary_sort_index() == UINT32_MAX) {
            source_d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          new_transfer_shader_key.source_msaa_samples =
              source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format =
              source_rt_key.resource_format;
          new_transfer_shader_key.value_convert =
              IsTransferValueConverted7e3And8888(source_rt_key, dest_rt_key);
          new_transfer_shader_key.source_scale_native =
              source_rt_key.scale_native;
          assert_true(!host_depth_source_d3d12_rt ||
                      host_depth_source_d3d12_rt->key().scale_native ==
                          dest_rt_key.scale_native);
          bool host_depth_source_is_copy =
              host_depth_source_d3d12_rt == &dest_d3d12_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_d3d12_rt && !host_depth_source_is_copy)
                  ? host_depth_source_d3d12_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode =
                source_rt_key.is_depth ? EdramTransferMode::kDepthToStencilBit
                                       : EdramTransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count +=
                transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles,
                                       dest_rt_key.msaa_samples, dest_is_64bpp,
                                       nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_d3d12_rt) {
                // Reading the host depth back out of the EDRAM buffer is a
                // mode of its own rather than a flag, so the shader declares a
                // buffer instead of a texture for it.
                if (host_depth_source_is_copy) {
                  new_transfer_shader_key.mode =
                      source_rt_key.is_depth
                          ? EdramTransferMode::kDepthAndHostDepthCopyToDepth
                          : EdramTransferMode::kColorAndHostDepthCopyToDepth;
                } else {
                  new_transfer_shader_key.mode =
                      source_rt_key.is_depth
                          ? EdramTransferMode::kDepthAndHostDepthToDepth
                          : EdramTransferMode::kColorAndHostDepthToDepth;
                }
              } else {
                new_transfer_shader_key.mode =
                    source_rt_key.is_depth ? EdramTransferMode::kDepthToDepth
                                           : EdramTransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode =
                  source_rt_key.is_depth ? EdramTransferMode::kDepthToColor
                                         : EdramTransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer,
                                                     new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source =
                nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(),
                current_transfer_invocations_.end());

      // Clear the stencil to 0 where it will be loaded - will be setting the
      // bits that need to be 1 by discarding samples. Clearing everything here
      // to reduce context switches internally in the driver if clear causes
      // them.
      if (stencil_clear_rectangle_count) {
        command_processor_.SubmitBarriers();
        D3D12_RECT* stencil_clear_rect_write_ptr =
            command_list.ClearDepthStencilViewAllocatedRects(
                dest_d3d12_rt.descriptor_draw().GetHandle(),
                D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
                stencil_clear_rectangle_count);
        assert_not_null(stencil_clear_rect_write_ptr);
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count =
              transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles,
                                     dest_rt_key.msaa_samples, dest_is_64bpp,
                                     transfer_stencil_clear_rectangles,
                                     resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count;
               ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->left = LONG(
                stencil_clear_rectangle.x_pixels * GetKeyScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->top = LONG(
                stencil_clear_rectangle.y_pixels * GetKeyScaleY(dest_rt_key));
            stencil_clear_rect_write_ptr->right =
                LONG((stencil_clear_rectangle.x_pixels +
                      stencil_clear_rectangle.width_pixels) *
                     GetKeyScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->bottom =
                LONG((stencil_clear_rectangle.y_pixels +
                      stencil_clear_rectangle.height_pixels) *
                     GetKeyScaleY(dest_rt_key));
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Perform the transfers for the render target.

      if (!transfer_viewport_set) {
        transfer_viewport_set = true;
        // Will be passing NDC directly, set the viewport to the maximum host
        // render target size for simplicity. Using a power-of-two scale for
        // exact pixel coordinates.
        D3D12_VIEWPORT transfer_viewport;
        transfer_viewport.TopLeftX = 0.0f;
        transfer_viewport.TopLeftY = 0.0f;
        transfer_viewport.Width = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.Height = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.MinDepth = 0.0f;
        transfer_viewport.MaxDepth = 1.0f;
        command_processor_.SetViewport(transfer_viewport);
        // TODO(Triang3l): Reduce scissor to the smallest transfer region for
        // more tiling friendliness.
        D3D12_RECT transfer_scissor;
        transfer_scissor.left = 0;
        transfer_scissor.top = 0;
        transfer_scissor.right = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_scissor.bottom = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        command_processor_.SetScissorRect(transfer_scissor);
      }

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count =
            transfer_invocation_first.transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles,
                dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
                resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles,
              dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
              resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(it->transfer.source);
        auto* host_depth_source_d3d12_rt =
            static_cast<D3D12RenderTarget*>(it->transfer.host_depth_source);
        EdramTransferShaderKey transfer_shader_key = it->shader_key;
        EdramTransferMode transfer_mode = transfer_shader_key.mode;
        const EdramTransferModeInfo& transfer_mode_info =
            kEdramTransferModes[size_t(transfer_mode)];
        TransferRootSignatureInfo transfer_root_info =
            GetTransferRootSignatureInfo(transfer_mode);
        uint32_t transfer_root_parameters_used =
            transfer_root_info.used_root_parameters;
        bool is_stencil_bit =
            transfer_mode_info.output == EdramTransferOutput::kStencilBit;
        // The emitter packs a layout's push constants densely, so a dword's
        // index depends on which of the others the layout uses.
        uint32_t transfer_push_constants_used =
            kEdramTransferPipelineLayoutInfos[size_t(transfer_mode_info
                                                         .pipeline_layout)]
                .used_push_constant_dwords;
        auto transfer_push_constant_dword =
            [transfer_push_constants_used](uint32_t push_constant_bit) {
              return xe::bit_count(transfer_push_constants_used &
                                   (push_constant_bit - 1));
            };

        // Late barriers in case there was cross-copying that prevented merging
        // of barriers.
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (host_depth_source_d3d12_rt) {
          if (EdramTransferHostDepthIsCopy(transfer_mode)) {
            // Reading copied host depth from the EDRAM buffer.
            TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushTransitionBarrier(
                host_depth_source_d3d12_rt->resource(),
                host_depth_source_d3d12_rt->SetResourceState(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          }
        }

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        D3D12_VERTEX_BUFFER_VIEW transfer_rectangle_buffer_view;
        transfer_rectangle_buffer_view.StrideInBytes = sizeof(float) * 2;
        transfer_rectangle_buffer_view.SizeInBytes =
            transfer_rectangle_buffer_view.StrideInBytes *
            transfer_vertex_count;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, transfer_rectangle_buffer_view.SizeInBytes,
                sizeof(float), nullptr, nullptr,
                &transfer_rectangle_buffer_view.BufferLocation));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last;
             ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count =
              it_merged->transfer.GetRectangles(
                  dest_rt_key.base_tiles, dest_pitch_tiles,
                  dest_rt_key.msaa_samples, dest_is_64bpp,
                  transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle =
                transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 =
                -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 =
                1.0f - transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 +
                transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 -
                transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_processor_.SetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list.D3DIASetVertexBuffers(0, 1,
                                           &transfer_rectangle_buffer_view);

        ID3D12PipelineState* const* transfer_pipelines =
            GetOrCreateTransferPipelines(transfer_shader_key);
        if (!transfer_pipelines) {
          continue;
        }
        if (last_transfer_mode != transfer_mode) {
          last_transfer_mode = transfer_mode;
          command_processor_.SetExternalGraphicsRootSignature(
              transfer_root_signatures_[size_t(transfer_mode)]);
          transfer_root_parameters_set = 0;
          address_constant_set = false;
          host_depth_address_constant_set = false;
          // The host depth copy's buffer never changes, so bind it with the
          // root signature.
          if (transfer_root_parameters_used &
              kTransferUsedRootParameterHostDepthBufferBit) {
            command_list.D3DSetGraphicsRootShaderResourceView(
                xe::bit_count(
                    transfer_root_parameters_used &
                    (kTransferUsedRootParameterHostDepthBufferBit - 1)),
                edram_buffer_gpu_address_);
          }
        }

        // Invalidate outdated bindings.
        if (transfer_root_parameters_used &
            kTransferUsedRootParameterColorSRVBit) {
          uint32_t descriptor_index_color =
              source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_color != UINT32_MAX);
          if (last_descriptor_index_color != descriptor_index_color) {
            last_descriptor_index_color = descriptor_index_color;
            transfer_root_parameters_set &=
                ~kTransferUsedRootParameterColorSRVBit;
          }
        }
        if (transfer_root_parameters_used &
            kTransferUsedRootParameterDepthSRVBit) {
          uint32_t descriptor_index_depth =
              source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_depth != UINT32_MAX);
          if (last_descriptor_index_depth != descriptor_index_depth) {
            last_descriptor_index_depth = descriptor_index_depth;
            transfer_root_parameters_set &=
                ~kTransferUsedRootParameterDepthSRVBit;
          }
        }
        if (transfer_root_parameters_used &
            kTransferUsedRootParameterStencilSRVBit) {
          uint32_t descriptor_index_stencil =
              source_d3d12_rt.temporary_srv_descriptor_index_stencil();
          assert_true(descriptor_index_stencil != UINT32_MAX);
          if (last_descriptor_index_stencil != descriptor_index_stencil) {
            last_descriptor_index_stencil = descriptor_index_stencil;
            transfer_root_parameters_set &=
                ~kTransferUsedRootParameterStencilSRVBit;
          }
        }
        if (transfer_root_parameters_used &
            kTransferUsedRootParameterHostDepthSRVBit) {
          assert_not_null(host_depth_source_d3d12_rt);
          uint32_t descriptor_index_host_depth =
              host_depth_source_d3d12_rt->temporary_srv_descriptor_index();
          assert_true(descriptor_index_host_depth != UINT32_MAX);
          if (last_descriptor_index_host_depth != descriptor_index_host_depth) {
            last_descriptor_index_host_depth = descriptor_index_host_depth;
            transfer_root_parameters_set &=
                ~kTransferUsedRootParameterHostDepthSRVBit;
          }
        }
        {
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          EdramTransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest = int32_t(dest_rt_key.base_tiles) -
                                            int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            address_constant_set = false;
          }
        }
        if (transfer_push_constants_used &
            kEdramTransferUsedPushConstantDwordHostDepthAddressBit) {
          assert_not_null(host_depth_source_d3d12_rt);
          RenderTargetKey host_depth_source_rt_key =
              host_depth_source_d3d12_rt->key();
          EdramTransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch =
              host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) -
              int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            host_depth_address_constant_set = false;
          }
        }

        // Apply the new bindings.
        uint32_t transfer_root_parameters_unset =
            transfer_root_parameters_used & ~transfer_root_parameters_set;
        if (!host_depth_address_constant_set &&
            (transfer_push_constants_used &
             kEdramTransferUsedPushConstantDwordHostDepthAddressBit)) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterPushConstantsBit - 1)),
              sizeof(last_host_depth_address_constant) / sizeof(uint32_t),
              &last_host_depth_address_constant,
              transfer_push_constant_dword(
                  kEdramTransferUsedPushConstantDwordHostDepthAddressBit));
          host_depth_address_constant_set = true;
        }
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterHostDepthSRVBit) {
          assert_true(last_descriptor_index_host_depth != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterHostDepthSRVBit - 1)),
              current_temporary_descriptors_gpu_
                  [last_descriptor_index_host_depth]
                      .second);
          transfer_root_parameters_set |=
              kTransferUsedRootParameterHostDepthSRVBit;
        }
        if (!address_constant_set) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterPushConstantsBit - 1)),
              sizeof(last_address_constant) / sizeof(uint32_t),
              &last_address_constant,
              transfer_push_constant_dword(
                  kEdramTransferUsedPushConstantDwordAddressBit));
          address_constant_set = true;
        }
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterStencilSRVBit) {
          assert_true(last_descriptor_index_stencil != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterStencilSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_stencil]
                  .second);
          transfer_root_parameters_set |=
              kTransferUsedRootParameterStencilSRVBit;
        }
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterDepthSRVBit) {
          assert_true(last_descriptor_index_depth != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterDepthSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_depth]
                  .second);
          transfer_root_parameters_set |= kTransferUsedRootParameterDepthSRVBit;
        }
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterColorSRVBit) {
          assert_true(last_descriptor_index_color != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              xe::bit_count(transfer_root_parameters_used &
                            (kTransferUsedRootParameterColorSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_color]
                  .second);
          transfer_root_parameters_set |= kTransferUsedRootParameterColorSRVBit;
        }

        // Draw the transfer rectangles.
        command_processor_.SubmitBarriers();
        for (uint32_t j = 0; j <= uint32_t(is_stencil_bit) * 7; ++j) {
          if (is_stencil_bit) {
            uint32_t transfer_stencil_bit = uint32_t(1) << j;
            command_list.D3DSetGraphicsRoot32BitConstants(
                xe::bit_count(transfer_root_parameters_used &
                              (kTransferUsedRootParameterPushConstantsBit - 1)),
                sizeof(transfer_stencil_bit) / sizeof(uint32_t),
                &transfer_stencil_bit,
                transfer_push_constant_dword(
                    kEdramTransferUsedPushConstantDwordStencilMaskBit));
          }
          command_processor_.SetExternalPipeline(transfer_pipelines[j]);
          command_list.D3DDrawInstanced(transfer_vertex_count, 1, 0, 0);
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            depth_host_clear_value =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        command_processor_.SubmitBarriers();
        command_list.D3DClearDepthStencilView(
            dest_d3d12_rt.descriptor_draw().GetHandle(),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            depth_host_clear_value, UINT(clear_value) & 0xFF, 1, &clear_rect);
      } else {
        float color_clear_value[4] = {};
        bool clear_via_drawing = false;
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] =
                  xenos::PWLGammaToLinear(color_clear_value[j]);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            color_clear_value[1] = float(uint32_t(clear_value >> 32));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value) ||
                uint64_t(color_clear_value[1]) != uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (clear_via_drawing) {
          auto handle =
              (dest_d3d12_rt.descriptor_load_separate().IsValid()
                   ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                   : dest_d3d12_rt.descriptor_draw().GetHandle());

          command_list.D3DOMSetRenderTargets(1, &handle, false, nullptr);
          are_current_command_list_render_targets_valid_ = true;
          D3D12_VIEWPORT clear_viewport;
          clear_viewport.TopLeftX = float(clear_rect.left);
          clear_viewport.TopLeftY = float(clear_rect.top);
          clear_viewport.Width = float(clear_rect.right - clear_rect.left);
          clear_viewport.Height = float(clear_rect.bottom - clear_rect.top);
          clear_viewport.MinDepth = 0.0f;
          clear_viewport.MaxDepth = 1.0f;
          command_processor_.SetViewport(clear_viewport);
          command_processor_.SetScissorRect(clear_rect);
          command_processor_.SetExternalGraphicsRootSignature(
              uint32_rtv_clear_root_signature_);
          uint32_t clear_via_drawing_value[2] = {uint32_t(clear_value),
                                                 uint32_t(clear_value >> 32)};
          command_list.D3DSetGraphicsRoot32BitConstants(
              0, 2, clear_via_drawing_value, 0);
          command_processor_.SetExternalPipeline(
              uint32_rtv_clear_pipelines_[size_t(
                  dest_rt_key.GetColorFormat() ==
                  xenos::ColorRenderTargetFormat::k_32_32_FLOAT)]
                                         [size_t(dest_rt_key.msaa_samples)]);
          command_processor_.SetPrimitiveTopology(
              D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          command_list.D3DDrawInstanced(3, 1, 0, 0);
        } else {
          command_processor_.SubmitBarriers();
          command_list.D3DClearRenderTargetView(
              dest_d3d12_rt.descriptor_load_separate().IsValid()
                  ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                  : dest_d3d12_rt.descriptor_draw().GetHandle(),
              color_clear_value, 1, &clear_rect);
        }
      }
    }
  }

  command_processor_.PopDebugMarker();
}

void D3D12RenderTargetCache::SetCommandListRenderTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  // Ensure the render targets are in the needed resource state.
  if (depth_and_color_render_targets[0]) {
    auto& d3d12_rt =
        *static_cast<D3D12RenderTarget*>(depth_and_color_render_targets[0]);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* render_target = depth_and_color_render_targets[1 + i];
    if (!render_target) {
      continue;
    }
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // Bind the render targets.
  if (are_current_command_list_render_targets_valid_) {
    // chrispy: the small memcmp doesnt get optimized by msvc

    for (unsigned i = 0;
         i < sizeof(current_command_list_render_targets_) /
                 sizeof(current_command_list_render_targets_[0]);
         ++i) {
      if ((const void*)current_command_list_render_targets_[i] !=
          (const void*)depth_and_color_render_targets[i]) {
        are_current_command_list_render_targets_valid_ = false;
        break;
      }
    }
  }
  if (!are_current_command_list_render_targets_valid_) {
    std::memcpy(current_command_list_render_targets_,
                depth_and_color_render_targets,
                sizeof(current_command_list_render_targets_));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    if (depth_and_color_render_targets[0]) {
      dsv_handle = static_cast<const D3D12RenderTarget*>(
                       depth_and_color_render_targets[0])
                       ->descriptor_draw()
                       .GetHandle();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[xenos::kMaxColorRenderTargets];
    uint32_t rtv_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const RenderTarget* render_target = depth_and_color_render_targets[1 + i];
      if (!render_target) {
        continue;
      }
      // Fill the gaps with a null descriptor.
      while (rtv_count < i) {
        rtv_handles[rtv_count++] =
            render_target->key().msaa_samples != xenos::MsaaSamples::k1X
                ? null_rtv_descriptor_ms_.GetHandle()
                : null_rtv_descriptor_ss_.GetHandle();
      }
      auto& d3d12_rt = *static_cast<const D3D12RenderTarget*>(render_target);
      rtv_handles[rtv_count++] = d3d12_rt.descriptor_draw().GetHandle();
    }
    command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(
        rtv_count, rtv_handles, false,
        depth_and_color_render_targets[0] ? &dsv_handle : nullptr);
    are_current_command_list_render_targets_valid_ = true;
  }
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDumpPipeline(
    EdramDumpShaderKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  EdramDumpShaderOptions shader_options;
  // The emitter declares the EDRAM buffer with the pre-1.3 BufferBlock and
  // Uniform forms, so it has to be emitted as SPIR-V 1.0.
  shader_options.spirv_version = 0x00010000;
  shader_options.descriptor_set_dest = kDumpDescriptorSetEdram;
  shader_options.descriptor_set_source = kDumpDescriptorSetSource;
  shader_options.resolution_scale_x = draw_resolution_scale_x();
  shader_options.resolution_scale_y = draw_resolution_scale_y();
  shader_options.msaa_2x_attachments_supported = msaa_2x_supported_;
  if (!key.is_depth) {
    GetColorOwnershipTransferDXGIFormat(key.GetColorFormat(),
                                        &shader_options.source_is_uint);
  }
  shader_options.depth_float24_round = depth_float24_round();
  shader_options.depth_float24_convert_in_pixel_shader =
      depth_float24_convert_in_pixel_shader();

  const char* format_name =
      key.is_depth
          ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
          : xenos::GetColorRenderTargetFormatName(key.GetColorFormat());
  uint32_t sample_count = uint32_t(1) << uint32_t(key.msaa_samples);
  ID3D12PipelineState* pipeline = nullptr;
  std::vector<uint32_t> spirv = BuildEdramDumpShaderSpirv(key, shader_options);
  if (spirv.empty()) {
    XELOGE(
        "D3D12RenderTargetCache: Failed to emit the render target dumping "
        "shader for {}-sample render targets with format {}",
        sample_count, format_name);
  } else {
    std::vector<uint8_t> dxil = SpirvToDxilCompiler::Translate(
        spirv.data(), spirv.size(), SpirvToDxilCompiler::Stage::kCompute);
    if (dxil.empty()) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to translate the render target "
          "dumping shader for {}-sample render targets with format {}",
          sample_count, format_name);
    } else {
      pipeline = ui::d3d12::util::CreateComputePipeline(
          command_processor_.GetD3D12Provider().GetDevice(), dxil.data(),
          dxil.size(),
          key.is_depth ? dump_root_signature_depth_
                       : dump_root_signature_color_);
      if (pipeline) {
        std::u16string pipeline_name = xe::to_utf16(
            fmt::format("RT Dump {} {}xMSAA", format_name, sample_count));
        pipeline->SetName(reinterpret_cast<LPCWSTR>(pipeline_name.c_str()));
      } else {
        XELOGE(
            "D3D12RenderTargetCache: Failed to create a render target dumping "
            "pipeline for {}-sample render targets with format {}",
            sample_count, format_name);
      }
    }
  }
  // Even if creation fails, still store the null pointer not to try to create
  // again.
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool D3D12RenderTargetCache::PrepareDumpSourceDescriptors() {
  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
  }
  current_temporary_descriptors_cpu_.clear();
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    if (d3d12_rt.temporary_sort_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    if (d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndex(
          uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(
          d3d12_rt.descriptor_srv().GetHandle());
    }
    if (d3d12_rt.key().is_depth &&
        d3d12_rt.temporary_srv_descriptor_index_stencil() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
          uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(
          d3d12_rt.descriptor_srv_stencil().GetHandle());
    }
  }

  // Copy source descriptors to a shader-visible heap.
  uint32_t descriptor_count =
      uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return false;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1,
                                  current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  return true;
}

bool D3D12RenderTargetCache::DirectResolveRenderTargets(
    const draw_util::ResolveInfo& resolve_info,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, bool copy_dest_scaled,
    D3D12SharedMemory& shared_memory, D3D12TextureCache& texture_cache) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return false;
  }

  // Everything that can fail has to be settled before the first dispatch is
  // encoded - falling back after that would resolve part of the range twice.
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    RenderTargetKey rt_key =
        static_cast<D3D12RenderTarget*>(rectangle.render_target)->key();
    EdramDumpShaderKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    pipeline_key.source_scale_native = rt_key.scale_native;
    pipeline_key.native_layout = uint32_t(!copy_dest_scaled);
    pipeline_key.direct_resolve = 1;
    if (!GetOrCreateDumpPipeline(pipeline_key)) {
      return false;
    }
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Committing starting with the beginning of the potentially written extent,
  // but making the buffer containing the base current - the beginning of the
  // bound buffer is what the tiled addresses are relative to.
  bool dest_committed =
      copy_dest_scaled
          ? (texture_cache.EnsureScaledResolveMemoryCommitted(
                 resolve_info.copy_dest_extent_start,
                 resolve_info.copy_dest_extent_length) &&
             texture_cache.MakeScaledResolveRangeCurrent(
                 resolve_info.copy_dest_base,
                 resolve_info.copy_dest_extent_start -
                     resolve_info.copy_dest_base +
                     resolve_info.copy_dest_extent_length))
          : shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                       resolve_info.copy_dest_extent_length);
  if (!dest_committed) {
    XELOGE(
        "D3D12RenderTargetCache: Failed to obtain the direct resolve "
        "destination memory region");
    return false;
  }

  if (!PrepareDumpSourceDescriptors()) {
    return false;
  }

  // Committed to the direct path from here on.
  command_processor_.PushDebugMarker("DirectResolveRenderTargets: base tile %u",
                                     dump_base);

  if (copy_dest_scaled) {
    texture_cache.TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    shared_memory.UseForWriting();
  }
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // One destination and one rectangle for the whole resolve.
  uint32_t resolve_constants[kEdramDumpShaderPushConstantCount] = {};
  resolve_constants[kEdramDumpShaderPushConstantResolveEdramInfo] =
      copy_shader_constants.dest_relative.edram_info.packed;
  resolve_constants[kEdramDumpShaderPushConstantResolveCoordinateInfo] =
      copy_shader_constants.dest_relative.coordinate_info.packed;
  resolve_constants[kEdramDumpShaderPushConstantResolveDestInfo] =
      copy_shader_constants.dest_relative.dest_info.value;
  resolve_constants[kEdramDumpShaderPushConstantResolveDestCoordinateInfo] =
      copy_shader_constants.dest_relative.dest_coordinate_info.packed;
  // The scaled destination's binding already starts at the base.
  resolve_constants[kEdramDumpShaderPushConstantResolveDestBase] =
      copy_dest_scaled ? 0 : copy_shader_constants.dest_base;
  resolve_constants[kEdramDumpShaderPushConstantResolveHeightDiv8] =
      resolve_info.height_div_8;

  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();
  ID3D12RootSignature* last_root_signature = nullptr;
  // `root_parameters_set` doesn't include the destination, which is never
  // changed.
  uint32_t root_parameters_set = 0;
  uint32_t last_descriptor_index_source = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  bool pitches_set = false;
  bool resolve_constants_set = false;
  EdramDumpShaderPitches last_pitches;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d12_rt.key();
    EdramDumpShaderKey pipeline_key = invocation.pipeline_key;
    ID3D12PipelineState* pipeline = GetOrCreateDumpPipeline(pipeline_key);
    assert_not_null(pipeline);
    command_processor_.SetExternalPipeline(pipeline);

    ID3D12RootSignature* root_signature = pipeline_key.is_depth
                                              ? dump_root_signature_depth_
                                              : dump_root_signature_color_;
    if (last_root_signature != root_signature) {
      last_root_signature = root_signature;
      command_list.D3DSetComputeRootSignature(root_signature);
      root_parameters_set = 0;
      pitches_set = false;
      resolve_constants_set = false;
      // Unscaled, the whole buffer is bound, which is what lets the
      // destination base stay the absolute byte offset the shader adds to the
      // tiled address. A scaled destination is a window starting at the
      // destination base instead, so the shader adds nothing.
      command_list.D3DSetComputeRootUnorderedAccessView(
          pipeline_key.is_depth ? kDumpRootParameterDepthEdram
                                : kDumpRootParameterColorEdram,
          copy_dest_scaled
              ? texture_cache.GetCurrentScaledResolveRangeGPUAddress()
              : shared_memory.GetGPUAddress());
    }

    if (!resolve_constants_set) {
      resolve_constants_set = true;
      command_list.D3DSetComputeRoot32BitConstants(
          kDumpRootParameterPushConstants,
          kEdramDumpShaderPushConstantCount -
              kEdramDumpShaderPushConstantResolveEdramInfo,
          &resolve_constants[kEdramDumpShaderPushConstantResolveEdramInfo],
          kEdramDumpShaderPushConstantResolveEdramInfo);
    }

    EdramDumpShaderPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_set = false;
    }
    if (!pitches_set) {
      pitches_set = true;
      command_list.D3DSetComputeRoot32BitConstants(
          kDumpRootParameterPushConstants,
          sizeof(last_pitches) / sizeof(uint32_t), &last_pitches,
          kEdramDumpShaderPushConstantPitches);
    }

    if (pipeline_key.is_depth) {
      constexpr uint32_t kDumpRootParameterDepthStencilBit =
          uint32_t(1) << kDumpRootParameterDepthStencil;
      uint32_t descriptor_index_stencil =
          d3d12_rt.temporary_srv_descriptor_index_stencil();
      assert_true(descriptor_index_stencil != UINT32_MAX);
      if (last_descriptor_index_stencil != descriptor_index_stencil) {
        last_descriptor_index_stencil = descriptor_index_stencil;
        root_parameters_set &= ~kDumpRootParameterDepthStencilBit;
      }
      if (!(root_parameters_set & kDumpRootParameterDepthStencilBit)) {
        command_list.D3DSetComputeRootDescriptorTable(
            kDumpRootParameterDepthStencil,
            current_temporary_descriptors_gpu_[last_descriptor_index_stencil]
                .second);
        root_parameters_set |= kDumpRootParameterDepthStencilBit;
      }
    }

    constexpr uint32_t kDumpRootParameterSourceBit =
        uint32_t(1) << kDumpRootParameterSource;
    uint32_t descriptor_index_source =
        d3d12_rt.temporary_srv_descriptor_index();
    assert_true(descriptor_index_source != UINT32_MAX);
    if (last_descriptor_index_source != descriptor_index_source) {
      last_descriptor_index_source = descriptor_index_source;
      root_parameters_set &= ~kDumpRootParameterSourceBit;
    }
    if (!(root_parameters_set & kDumpRootParameterSourceBit)) {
      command_list.D3DSetComputeRootDescriptorTable(
          kDumpRootParameterSource,
          current_temporary_descriptors_gpu_[last_descriptor_index_source]
              .second);
      root_parameters_set |= kDumpRootParameterSourceBit;
    }

    // A direct resolve dispatches over destination pixels rather than EDRAM
    // samples, so the runs of pixels the guest layout stores contiguously line
    // up with whole stores. Tiles cover this many of them - host pixels, so
    // scaled along with the destination.
    bool format_is_64bpp = rt_key.Is64bpp();
    uint32_t tile_pixels_x =
        ((xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) >>
         uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k4X)) *
        (copy_dest_scaled ? draw_resolution_scale_x() : 1);
    uint32_t tile_pixels_y =
        (xenos::kEdramTileHeightSamples >>
         uint32_t(rt_key.msaa_samples >= xenos::MsaaSamples::k2X)) *
        (copy_dest_scaled ? draw_resolution_scale_y() : 1);
    uint32_t pixels_per_thread =
        GetEdramDumpShaderResolvePixelsPerThread(format_is_64bpp);

    EdramDumpShaderOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      command_list.D3DSetComputeRoot32BitConstants(
          kDumpRootParameterPushConstants, sizeof(offsets) / sizeof(uint32_t),
          &offsets, kEdramDumpShaderPushConstantOffsets);

      // Where this dispatch starts in the resolve's tile grid, which the
      // threads place themselves against.
      uint32_t dispatch_tile_relative =
          offsets.dispatch_first_tile -
          copy_shader_constants.dest_relative.edram_info.base_tiles;
      EdramDumpShaderResolveDispatchTile dispatch_tile;
      dispatch_tile.tile_x = dispatch_tile_relative % dump_pitch;
      dispatch_tile.tile_y = dispatch_tile_relative / dump_pitch;
      command_list.D3DSetComputeRoot32BitConstants(
          kDumpRootParameterPushConstants,
          sizeof(dispatch_tile) / sizeof(uint32_t), &dispatch_tile,
          kEdramDumpShaderPushConstantResolveDispatchTile);

      command_processor_.SubmitBarriers();
      uint32_t threads_x =
          (dispatch.width_tiles * tile_pixels_x + (pixels_per_thread - 1)) /
          pixels_per_thread;
      command_list.D3DDispatch(
          (threads_x + (kEdramDumpShaderResolveThreadsPerGroupX - 1)) /
              kEdramDumpShaderResolveThreadsPerGroupX,
          (dispatch.height_tiles * tile_pixels_y +
           (kEdramDumpShaderResolveThreadsPerGroupY - 1)) /
              kEdramDumpShaderResolveThreadsPerGroupY,
          1);
    }
  }

  if (copy_dest_scaled) {
    texture_cache.MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
  } else {
    shared_memory.MarkUAVWritesCommitNeeded();
  }

  command_processor_.PopDebugMarker();
  return true;
}

void D3D12RenderTargetCache::DumpRenderTargets(uint32_t dump_base,
                                               uint32_t dump_row_length_used,
                                               uint32_t dump_rows,
                                               uint32_t dump_pitch,
                                               bool native_layout) {
  SCOPE_profile_cpu_f("gpu");
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return;
  }

  command_processor_.PushDebugMarker(
      "DumpRenderTargets (EDRAM Write): base tile %u", dump_base);

  if (!PrepareDumpSourceDescriptors()) {
    command_processor_.PopDebugMarker();
    return;
  }

  TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RenderTargetKey rt_key = d3d12_rt.key();
    // Native layout is only for resolves with ALL native sources.
    assert_true(!native_layout || rt_key.scale_native);
    EdramDumpShaderKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    pipeline_key.source_scale_native = rt_key.scale_native;
    pipeline_key.native_layout = uint32_t(native_layout);
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandList& command_list =
      command_processor_.GetDeferredCommandList();
  ID3D12RootSignature* last_root_signature = nullptr;
  // `root_parameters_set` doesn't include the EDRAM buffer, which is never
  // changed.
  uint32_t root_parameters_set = 0;
  uint32_t last_descriptor_index_source = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  // The pitches and the offsets are two dwords of one root constants
  // parameter, so they're tracked apart from the descriptor tables' mask.
  bool pitches_set = false;
  bool offsets_set = false;
  EdramDumpShaderOffsets last_offsets;
  EdramDumpShaderPitches last_pitches;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d12_rt.key();
    EdramDumpShaderKey pipeline_key = invocation.pipeline_key;
    ID3D12PipelineState* pipeline = GetOrCreateDumpPipeline(pipeline_key);
    if (!pipeline) {
      continue;
    }
    command_processor_.SetExternalPipeline(pipeline);

    ID3D12RootSignature* root_signature = pipeline_key.is_depth
                                              ? dump_root_signature_depth_
                                              : dump_root_signature_color_;
    if (last_root_signature != root_signature) {
      last_root_signature = root_signature;
      command_list.D3DSetComputeRootSignature(root_signature);
      root_parameters_set = 0;
      pitches_set = false;
      offsets_set = false;
      command_list.D3DSetComputeRootUnorderedAccessView(
          pipeline_key.is_depth ? kDumpRootParameterDepthEdram
                                : kDumpRootParameterColorEdram,
          edram_buffer_gpu_address_);
    }

    EdramDumpShaderPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_set = false;
    }
    if (!pitches_set) {
      command_list.D3DSetComputeRoot32BitConstants(
          kDumpRootParameterPushConstants,
          sizeof(last_pitches) / sizeof(uint32_t), &last_pitches,
          kEdramDumpShaderPushConstantPitches);
      pitches_set = true;
    }

    if (pipeline_key.is_depth) {
      constexpr uint32_t kDumpRootParameterDepthStencilBit =
          uint32_t(1) << kDumpRootParameterDepthStencil;
      uint32_t descriptor_index_stencil =
          d3d12_rt.temporary_srv_descriptor_index_stencil();
      assert_true(descriptor_index_stencil != UINT32_MAX);
      if (last_descriptor_index_stencil != descriptor_index_stencil) {
        last_descriptor_index_stencil = descriptor_index_stencil;
        root_parameters_set &= ~kDumpRootParameterDepthStencilBit;
      }
      if (!(root_parameters_set & kDumpRootParameterDepthStencilBit)) {
        command_list.D3DSetComputeRootDescriptorTable(
            kDumpRootParameterDepthStencil,
            current_temporary_descriptors_gpu_[last_descriptor_index_stencil]
                .second);
        root_parameters_set |= kDumpRootParameterDepthStencilBit;
      }
    }

    constexpr uint32_t kDumpRootParameterSourceBit =
        uint32_t(1) << kDumpRootParameterSource;
    uint32_t descriptor_index_source =
        d3d12_rt.temporary_srv_descriptor_index();
    assert_true(descriptor_index_source != UINT32_MAX);
    if (last_descriptor_index_source != descriptor_index_source) {
      last_descriptor_index_source = descriptor_index_source;
      root_parameters_set &= ~kDumpRootParameterSourceBit;
    }
    if (!(root_parameters_set & kDumpRootParameterSourceBit)) {
      command_list.D3DSetComputeRootDescriptorTable(
          kDumpRootParameterSource,
          current_temporary_descriptors_gpu_[last_descriptor_index_source]
              .second);
      root_parameters_set |= kDumpRootParameterSourceBit;
    }

    EdramDumpShaderOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    bool format_is_64bpp = rt_key.Is64bpp();
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_set = false;
      }
      if (!offsets_set) {
        command_list.D3DSetComputeRoot32BitConstants(
            kDumpRootParameterPushConstants,
            sizeof(last_offsets) / sizeof(uint32_t), &last_offsets,
            kEdramDumpShaderPushConstantOffsets);
        offsets_set = true;
      }
      command_processor_.SubmitBarriers();
      // The emitter's group covers 8 x 16 samples. The native layout has a
      // 1x1 footprint.
      command_list.D3DDispatch(
          ((native_layout ? 1 : draw_resolution_scale_x()) *
               (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
               dispatch.width_tiles +
           (kEdramDumpShaderSamplesPerGroupX - 1)) /
              kEdramDumpShaderSamplesPerGroupX,
          ((native_layout ? 1 : draw_resolution_scale_y()) *
               xenos::kEdramTileHeightSamples * dispatch.height_tiles +
           (kEdramDumpShaderSamplesPerGroupY - 1)) /
              kEdramDumpShaderSamplesPerGroupY,
          1);
    }
    MarkEdramBufferModified();
  }

  command_processor_.PopDebugMarker();
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
