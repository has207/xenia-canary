/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/spirv_fsi_system_constants.h"

#include <algorithm>
#include <cstring>

#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

void WriteFragmentShaderInterlockSystemConstants(
    SpirvShaderTranslator::SystemConstants& system_constants, uint32_t& flags,
    bool& dirty, const RegisterFile& regs, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, uint32_t zpd_fsi_counter_index) {
  using SpirvTranslator = SpirvShaderTranslator;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf =
      regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();

  // Per render target clamp ranges and write keep masks (two UINT32_MAX when no
  // components actually existing in the RT are written).
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  float rt_clamp[4][4];
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    RenderTargetCache::GetPSIColorFormatInfo(
        color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111,
        rt_clamp[i][0], rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3],
        rt_keep_masks[i][0], rt_keep_masks[i][1]);
  }

  // Disable depth and stencil if it aliases a written color render target.
  // Don't exclude fully overlapping render targets - two with the same base are
  // used in the lighting pass of 4D5307E6, picked with dynamic control flow.
  bool depth_stencil_enabled = normalized_depth_control.stencil_enable ||
                               normalized_depth_control.z_enable;
  if (depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX ||
           rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  // Depth / stencil flag bits.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  if (depth_stencil_enabled) {
    flags |= SpirvTranslator::kSysFlag_FSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << SpirvTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SpirvTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and don't
      // modify the stored depth.
      flags |= SpirvTranslator::kSysFlag_FSIDepthPassIfLess |
               SpirvTranslator::kSysFlag_FSIDepthPassIfEqual |
               SpirvTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SpirvTranslator::kSysFlag_FSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SpirvTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }

  dirty |= system_constants.zpd_fsi_counter_index != zpd_fsi_counter_index;
  system_constants.zpd_fsi_counter_index = zpd_fsi_counter_index;

  uint32_t edram_tile_dwords_scaled =
      xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples *
      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for FSI render target writing. Align, then multiply by the
  // 32bpp tile size in dwords.
  uint32_t edram_32bpp_tile_pitch_dwords_scaled =
      ((rb_surface_info.surface_pitch *
        (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
       (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
  dirty |= system_constants.edram_32bpp_tile_pitch_dwords_scaled !=
           edram_32bpp_tile_pitch_dwords_scaled;
  system_constants.edram_32bpp_tile_pitch_dwords_scaled =
      edram_32bpp_tile_pitch_dwords_scaled;

  // Per render target FSI write state.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    dirty |= system_constants.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
    system_constants.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
    dirty |= system_constants.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
    system_constants.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
    if (rt_keep_masks[i][0] != UINT32_MAX ||
        rt_keep_masks[i][1] != UINT32_MAX) {
      uint32_t rt_base_dwords_scaled =
          color_info.color_base * edram_tile_dwords_scaled;
      dirty |= system_constants.edram_rt_base_dwords_scaled[i] !=
               rt_base_dwords_scaled;
      system_constants.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
      uint32_t blend_factors_ops =
          regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
      dirty |=
          system_constants.edram_rt_blend_factors_ops[i] != blend_factors_ops;
      system_constants.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      // Can't do float comparisons here because NaNs would result in always
      // setting the dirty flag.
      dirty |= std::memcmp(system_constants.edram_rt_clamp[i], rt_clamp[i],
                           4 * sizeof(float)) != 0;
      std::memcpy(system_constants.edram_rt_clamp[i], rt_clamp[i],
                  4 * sizeof(float));
    }
  }

  uint32_t depth_base_dwords_scaled =
      rb_depth_info.depth_base * edram_tile_dwords_scaled;
  dirty |= system_constants.edram_depth_base_dwords_scaled !=
           depth_base_dwords_scaled;
  system_constants.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

  // For non-polygons, front polygon offset is used, enabled if
  // POLY_OFFSET_PARA_ENABLED is set. For polygons, front and back are separate.
  float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
  float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
  if (primitive_polygonal) {
    if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
      poly_offset_front_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      poly_offset_front_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
      poly_offset_back_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      poly_offset_back_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else {
    if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
      poly_offset_front_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      poly_offset_front_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      poly_offset_back_scale = poly_offset_front_scale;
      poly_offset_back_offset = poly_offset_front_offset;
    }
  }
  // With non-square resolution scaling, make sure the worst-case impact is
  // reverted (slope only along the scaled axis), thus max. More bias is better
  // than less bias, because less bias means Z fighting with the background is
  // more likely.
  float poly_offset_scale_factor =
      xenos::kPolygonOffsetScaleSubpixelUnit *
      std::max(draw_resolution_scale_x, draw_resolution_scale_y);
  poly_offset_front_scale *= poly_offset_scale_factor;
  poly_offset_back_scale *= poly_offset_scale_factor;
  dirty |=
      system_constants.edram_poly_offset_front_scale != poly_offset_front_scale;
  system_constants.edram_poly_offset_front_scale = poly_offset_front_scale;
  dirty |= system_constants.edram_poly_offset_front_offset !=
           poly_offset_front_offset;
  system_constants.edram_poly_offset_front_offset = poly_offset_front_offset;
  dirty |=
      system_constants.edram_poly_offset_back_scale != poly_offset_back_scale;
  system_constants.edram_poly_offset_back_scale = poly_offset_back_scale;
  dirty |=
      system_constants.edram_poly_offset_back_offset != poly_offset_back_offset;
  system_constants.edram_poly_offset_back_offset = poly_offset_back_offset;

  if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
    uint32_t stencil_front_reference_masks = rb_stencilrefmask.value & 0xFFFFFF;
    dirty |= system_constants.edram_stencil_front_reference_masks !=
             stencil_front_reference_masks;
    system_constants.edram_stencil_front_reference_masks =
        stencil_front_reference_masks;
    uint32_t stencil_func_ops =
        (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
    dirty |= system_constants.edram_stencil_front_func_ops != stencil_func_ops;
    system_constants.edram_stencil_front_func_ops = stencil_func_ops;

    if (primitive_polygonal && normalized_depth_control.backface_enable) {
      uint32_t stencil_back_reference_masks =
          rb_stencilrefmask_bf.value & 0xFFFFFF;
      dirty |= system_constants.edram_stencil_back_reference_masks !=
               stencil_back_reference_masks;
      system_constants.edram_stencil_back_reference_masks =
          stencil_back_reference_masks;
      uint32_t stencil_func_ops_bf =
          (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
      dirty |=
          system_constants.edram_stencil_back_func_ops != stencil_func_ops_bf;
      system_constants.edram_stencil_back_func_ops = stencil_func_ops_bf;
    } else {
      dirty |= std::memcmp(system_constants.edram_stencil_back,
                           system_constants.edram_stencil_front,
                           2 * sizeof(uint32_t)) != 0;
      std::memcpy(system_constants.edram_stencil_back,
                  system_constants.edram_stencil_front, 2 * sizeof(uint32_t));
    }
  }

  dirty |= system_constants.edram_blend_constant[0] !=
           regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  system_constants.edram_blend_constant[0] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  dirty |= system_constants.edram_blend_constant[1] !=
           regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  system_constants.edram_blend_constant[1] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  dirty |= system_constants.edram_blend_constant[2] !=
           regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  system_constants.edram_blend_constant[2] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  dirty |= system_constants.edram_blend_constant[3] !=
           regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  system_constants.edram_blend_constant[3] =
      regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
}

}  // namespace gpu
}  // namespace xe
