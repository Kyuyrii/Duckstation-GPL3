// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_hw.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "gpu_hw_shadergen.h"
#include "gpu_sw_backend.h"
#include "host.h"
#include "settings.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsFontAwesome5.h"
#include "imgui.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

Log_SetChannel(GPU_HW);

// TODO: instead of full state restore, only restore what changed

static constexpr GPUTexture::Format VRAM_RT_FORMAT = GPUTexture::Format::RGBA8;
static constexpr GPUTexture::Format VRAM_DS_FORMAT = GPUTexture::Format::D16;
static constexpr GPUTexture::Format VRAM_DS_DEPTH_FORMAT = GPUTexture::Format::D32F;
static constexpr GPUTexture::Format VRAM_DS_COLOR_FORMAT = GPUTexture::Format::R32F;

#ifdef _DEBUG

static u32 s_draw_number = 0;

static constexpr const std::array s_transparency_modes = {
  "HalfBackgroundPlusHalfForeground",
  "BackgroundPlusForeground",
  "BackgroundMinusForeground",
  "BackgroundPlusQuarterForeground",
  "Disabled",
};

static constexpr const std::array s_batch_texture_modes = {
  "Palette4Bit",       "Palette8Bit",       "Direct16Bit",       "Disabled",
  "SpritePalette4Bit", "SpritePalette8Bit", "SpriteDirect16Bit",
};

static constexpr const std::array s_batch_render_modes = {
  "TransparencyDisabled", "TransparentAndOpaque", "OnlyOpaque", "OnlyTransparent", "ShaderBlend",
};

#endif

/// Returns the distance between two rectangles.
ALWAYS_INLINE static float RectDistance(const GSVector4i lhs, const GSVector4i rhs)
{
  const s32 lcx = (lhs.left + ((lhs.right - lhs.left) / 2));
  const s32 lcy = (lhs.top + ((lhs.bottom - lhs.top) / 2));
  const s32 rcx = (rhs.left + ((rhs.right - rhs.left) / 2));
  const s32 rcy = (rhs.top + ((rhs.bottom - rhs.top) / 2));
  const s32 dx = (lcx - rcx);
  const s32 dy = (lcy - rcy);
  const s32 distsq = (dx * dx) + (dy * dy);
  return std::sqrt(static_cast<float>(distsq));
}

ALWAYS_INLINE static u32 GetMaxResolutionScale()
{
  return g_gpu_device->GetMaxTextureSize() / VRAM_WIDTH;
}

ALWAYS_INLINE_RELEASE static u32 GetBoxDownsampleScale(u32 resolution_scale)
{
  u32 scale = std::min<u32>(resolution_scale, g_settings.gpu_downsample_scale);
  while ((resolution_scale % scale) != 0)
    scale--;
  return scale;
}

ALWAYS_INLINE static bool ShouldClampUVs(GPUTextureFilter texture_filter)
{
  // We only need UV limits if PGXP is enabled, or texture filtering is enabled.
  return g_settings.gpu_pgxp_enable || texture_filter != GPUTextureFilter::Nearest;
}

ALWAYS_INLINE static bool ShouldAllowSpriteMode(u8 resolution_scale, GPUTextureFilter texture_filter,
                                                GPUTextureFilter sprite_texture_filter)
{
  // Use sprite shaders/mode when texcoord rounding is forced, or if the filters are different.
  return (sprite_texture_filter != texture_filter || (resolution_scale > 1 && g_settings.gpu_force_round_texcoords));
}

ALWAYS_INLINE static bool ShouldDisableColorPerspective()
{
  return g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction && !g_settings.gpu_pgxp_color_correction;
}

/// Returns true if the specified texture filtering mode requires dual-source blending.
ALWAYS_INLINE static bool IsBlendedTextureFiltering(GPUTextureFilter filter)
{
  // return (filter == GPUTextureFilter::Bilinear || filter == GPUTextureFilter::JINC2 || filter ==
  // GPUTextureFilter::xBR);
  static_assert(((static_cast<u8>(GPUTextureFilter::Nearest) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::Bilinear) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::BilinearBinAlpha) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::JINC2) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::JINC2BinAlpha) & 1u) == 0u) &&
                ((static_cast<u8>(GPUTextureFilter::xBR) & 1u) == 1u) &&
                ((static_cast<u8>(GPUTextureFilter::xBRBinAlpha) & 1u) == 0u));
  return ((static_cast<u8>(filter) & 1u) == 1u);
}

/// Computes the area affected by a VRAM transfer, including wrap-around of X.
ALWAYS_INLINE_RELEASE static GSVector4i GetVRAMTransferBounds(u32 x, u32 y, u32 width, u32 height)
{
  GSVector4i ret;
  ret.left = x % VRAM_WIDTH;
  ret.top = y % VRAM_HEIGHT;
  ret.right = ret.left + width;
  ret.bottom = ret.top + height;
  if (ret.right > static_cast<s32>(VRAM_WIDTH))
  {
    ret.left = 0;
    ret.right = static_cast<s32>(VRAM_WIDTH);
  }
  if (ret.bottom > static_cast<s32>(VRAM_HEIGHT))
  {
    ret.top = 0;
    ret.bottom = static_cast<s32>(VRAM_HEIGHT);
  }
  return ret;
}

namespace {
class ShaderCompileProgressTracker
{
public:
  ShaderCompileProgressTracker(std::string title, u32 total)
    : m_title(std::move(title)), m_min_time(Common::Timer::ConvertSecondsToValue(1.0)),
      m_update_interval(Common::Timer::ConvertSecondsToValue(0.1)), m_start_time(Common::Timer::GetCurrentValue()),
      m_last_update_time(0), m_progress(0), m_total(total)
  {
  }
  ~ShaderCompileProgressTracker() = default;

  void Increment(u32 progress = 1)
  {
    m_progress += progress;

    const u64 tv = Common::Timer::GetCurrentValue();
    if ((tv - m_start_time) >= m_min_time && (tv - m_last_update_time) >= m_update_interval)
    {
      Host::DisplayLoadingScreen(m_title.c_str(), 0, static_cast<int>(m_total), static_cast<int>(m_progress));
      m_last_update_time = tv;
    }
  }

private:
  std::string m_title;
  u64 m_min_time;
  u64 m_update_interval;
  u64 m_start_time;
  u64 m_last_update_time;
  u32 m_progress;
  u32 m_total;
};
} // namespace

GPU_HW::GPU_HW() : GPU()
{
#ifdef _DEBUG
  s_draw_number = 0;
#endif
}

GPU_HW::~GPU_HW()
{
  if (m_sw_renderer)
  {
    m_sw_renderer->Shutdown();
    m_sw_renderer.reset();
  }
}

ALWAYS_INLINE void GPU_HW::BatchVertex::Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_,
                                            u16 packed_texcoord, u32 uv_limits_)
{
  Set(x_, y_, z_, w_, color_, texpage_, packed_texcoord & 0xFF, (packed_texcoord >> 8), uv_limits_);
}

ALWAYS_INLINE void GPU_HW::BatchVertex::Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 u_,
                                            u16 v_, u32 uv_limits_)
{
  x = x_;
  y = y_;
  z = z_;
  w = w_;
  color = color_;
  texpage = texpage_;
  u = u_;
  v = v_;
  uv_limits = uv_limits_;
}

ALWAYS_INLINE u32 GPU_HW::BatchVertex::PackUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
{
  return min_u | (min_v << 8) | (max_u << 16) | (max_v << 24);
}

ALWAYS_INLINE void GPU_HW::BatchVertex::SetUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
{
  uv_limits = PackUVLimits(min_u, max_u, min_v, max_v);
}

const Threading::Thread* GPU_HW::GetSWThread() const
{
  return m_sw_renderer ? m_sw_renderer->GetThread() : nullptr;
}

bool GPU_HW::IsHardwareRenderer() const
{
  return true;
}

bool GPU_HW::Initialize()
{
  if (!GPU::Initialize())
    return false;

  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  m_resolution_scale = Truncate8(CalculateResolutionScale());
  m_multisamples = Truncate8(std::min<u32>(g_settings.gpu_multisamples, g_gpu_device->GetMaxMultisamples()));
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_sprite_texture_filtering = g_settings.gpu_sprite_texture_filter;
  m_line_detect_mode = (m_resolution_scale > 1) ? g_settings.gpu_line_detect_mode : GPULineDetectMode::Disabled;
  m_downsample_mode = GetDownsampleMode(m_resolution_scale);
  m_wireframe_mode = g_settings.gpu_wireframe_mode;
  m_supports_dual_source_blend = features.dual_source_blend;
  m_supports_framebuffer_fetch = features.framebuffer_fetch;
  m_true_color = g_settings.gpu_true_color;
  m_pgxp_depth_buffer = g_settings.UsingPGXPDepthBuffer();
  m_clamp_uvs = ShouldClampUVs(m_texture_filtering) || ShouldClampUVs(m_sprite_texture_filtering);
  m_compute_uv_range = m_clamp_uvs;
  m_allow_sprite_mode = ShouldAllowSpriteMode(m_resolution_scale, m_texture_filtering, m_sprite_texture_filtering);

  CheckSettings();

  UpdateSoftwareRenderer(false);

  PrintSettingsToLog();

  Error error;
  if (!CompilePipelines(&error))
  {
    ERROR_LOG("Failed to compile pipelines: {}", error.GetDescription());
    return false;
  }

  if (!CreateBuffers())
  {
    ERROR_LOG("Failed to create framebuffer");
    return false;
  }

  UpdateDownsamplingLevels();
  RestoreDeviceContext();
  return true;
}

void GPU_HW::Reset(bool clear_vram)
{
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(0, 0);

  GPU::Reset(clear_vram);

  if (m_sw_renderer)
    m_sw_renderer->Reset();

  m_batch = {};
  m_batch_ubo_data = {};
  m_batch_ubo_dirty = true;
  m_current_depth = 1;
  SetClampedDrawingArea();

  if (clear_vram)
    ClearFramebuffer();
}

bool GPU_HW::DoState(StateWrapper& sw, GPUTexture** host_texture, bool update_display)
{
  // Need to download local VRAM copy before calling the base class, because it serializes this.
  if (m_sw_renderer)
  {
    m_sw_renderer->Sync(true);
  }
  else if (sw.IsWriting() && !host_texture)
  {
    // If SW renderer readbacks aren't enabled, the CLUT won't be populated, which means it'll be invalid if the user
    // loads this state with software instead of hardware renderers. So force-update the CLUT.
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    if (IsCLUTValid())
      GPU::ReadCLUT(g_gpu_clut, GPUTexturePaletteReg{Truncate16(m_current_clut_reg_bits)}, m_current_clut_is_8bit);
  }

  if (!GPU::DoState(sw, host_texture, update_display))
    return false;

  if (host_texture)
  {
    GPUTexture* tex = *host_texture;
    if (sw.IsReading())
    {
      if (tex->GetWidth() != m_vram_texture->GetWidth() || tex->GetHeight() != m_vram_texture->GetHeight() ||
          tex->GetSamples() != m_vram_texture->GetSamples())
      {
        return false;
      }

      g_gpu_device->CopyTextureRegion(m_vram_texture.get(), 0, 0, 0, 0, tex, 0, 0, 0, 0, tex->GetWidth(),
                                      tex->GetHeight());
    }
    else
    {
      if (!tex || tex->GetWidth() != m_vram_texture->GetWidth() || tex->GetHeight() != m_vram_texture->GetHeight() ||
          tex->GetSamples() != m_vram_texture->GetSamples())
      {
        delete tex;

        // We copy to/from the save state texture, but we can't have multisampled non-RTs.
        tex = g_gpu_device
                ->FetchTexture(
                  m_vram_texture->GetWidth(), m_vram_texture->GetHeight(), 1, 1, m_vram_texture->GetSamples(),
                  m_vram_texture->IsMultisampled() ? GPUTexture::Type::RenderTarget : GPUTexture::Type::Texture,
                  GPUTexture::Format::RGBA8, nullptr, 0)
                .release();
        *host_texture = tex;
        if (!tex)
          return false;
      }

      g_gpu_device->CopyTextureRegion(tex, 0, 0, 0, 0, m_vram_texture.get(), 0, 0, 0, 0, tex->GetWidth(),
                                      tex->GetHeight());
    }
  }
  else if (sw.IsReading())
  {
    // Need to update the VRAM copy on the GPU with the state data.
    UpdateVRAMOnGPU(0, 0, VRAM_WIDTH, VRAM_HEIGHT, g_vram, VRAM_WIDTH * sizeof(u16), false, false, VRAM_SIZE_RECT);
  }

  // invalidate the whole VRAM read texture when loading state
  if (sw.IsReading())
  {
    DebugAssert(!m_batch_vertex_ptr && !m_batch_index_ptr);
    ClearVRAMDirtyRectangle();
    SetFullVRAMDirtyRectangle();
    ResetBatchVertexDepth();
  }

  return true;
}

void GPU_HW::RestoreDeviceContext()
{
  g_gpu_device->SetTextureSampler(0, m_vram_read_texture.get(), g_gpu_device->GetNearestSampler());
  SetVRAMRenderTarget();
  g_gpu_device->SetViewport(m_vram_texture->GetRect());
  SetScissor();
  m_batch_ubo_dirty = true;
}

void GPU_HW::UpdateSettings(const Settings& old_settings)
{
  GPU::UpdateSettings(old_settings);

  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  const u8 resolution_scale = Truncate8(CalculateResolutionScale());
  const u8 multisamples = Truncate8(std::min<u32>(g_settings.gpu_multisamples, g_gpu_device->GetMaxMultisamples()));
  const bool clamp_uvs = ShouldClampUVs(m_texture_filtering) || ShouldClampUVs(m_sprite_texture_filtering);
  const bool framebuffer_changed = (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
                                    g_settings.IsUsingAccurateBlending() != old_settings.IsUsingAccurateBlending() ||
                                    m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer());
  const bool shaders_changed =
    (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
     m_true_color != g_settings.gpu_true_color || g_settings.gpu_debanding != old_settings.gpu_debanding ||
     (multisamples > 0 && g_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading) ||
     (resolution_scale > 1 && g_settings.gpu_scaled_dithering != old_settings.gpu_scaled_dithering) ||
     (resolution_scale > 1 && g_settings.gpu_texture_filter == GPUTextureFilter::Nearest &&
      g_settings.gpu_force_round_texcoords != old_settings.gpu_force_round_texcoords) ||
     g_settings.IsUsingAccurateBlending() != old_settings.IsUsingAccurateBlending() ||
     m_texture_filtering != g_settings.gpu_texture_filter ||
     m_sprite_texture_filtering != g_settings.gpu_sprite_texture_filter || m_clamp_uvs != clamp_uvs ||
     (resolution_scale > 1 && (g_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
                               (m_downsample_mode == GPUDownsampleMode::Box &&
                                g_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale))) ||
     (features.geometry_shaders && g_settings.gpu_wireframe_mode != old_settings.gpu_wireframe_mode) ||
     m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer() ||
     (features.noperspective_interpolation &&
      ShouldDisableColorPerspective() != old_settings.gpu_pgxp_color_correction) ||
     m_allow_sprite_mode !=
       ShouldAllowSpriteMode(m_resolution_scale, g_settings.gpu_texture_filter, g_settings.gpu_sprite_texture_filter));

  if (m_resolution_scale != resolution_scale)
  {
    Host::AddIconOSDMessage(
      "ResolutionScaleChanged", ICON_FA_PAINT_BRUSH,
      fmt::format(TRANSLATE_FS("GPU_HW", "Resolution scale set to {0}x (display {1}x{2}, VRAM {3}x{4})"),
                  resolution_scale, m_crtc_state.display_vram_width * resolution_scale,
                  resolution_scale * m_crtc_state.display_vram_height, VRAM_WIDTH * resolution_scale,
                  VRAM_HEIGHT * resolution_scale),
      Host::OSD_INFO_DURATION);
  }

  if (m_multisamples != multisamples || g_settings.gpu_per_sample_shading != old_settings.gpu_per_sample_shading)
  {
    if (g_settings.gpu_per_sample_shading && features.per_sample_shading)
    {
      Host::AddIconOSDMessage(
        "MultisamplingChanged", ICON_FA_PAINT_BRUSH,
        fmt::format(TRANSLATE_FS("GPU_HW", "Multisample anti-aliasing set to {}x (SSAA)."), multisamples),
        Host::OSD_INFO_DURATION);
    }
    else
    {
      Host::AddIconOSDMessage(
        "MultisamplingChanged", ICON_FA_PAINT_BRUSH,
        fmt::format(TRANSLATE_FS("GPU_HW", "Multisample anti-aliasing set to {}x."), multisamples),
        Host::OSD_INFO_DURATION);
    }
  }

  // Back up VRAM if we're recreating the framebuffer.
  if (framebuffer_changed)
  {
    RestoreDeviceContext();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    DestroyBuffers();
  }

  m_resolution_scale = resolution_scale;
  m_multisamples = multisamples;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_sprite_texture_filtering = g_settings.gpu_sprite_texture_filter;
  m_line_detect_mode = (m_resolution_scale > 1) ? g_settings.gpu_line_detect_mode : GPULineDetectMode::Disabled;
  m_downsample_mode = GetDownsampleMode(resolution_scale);
  m_wireframe_mode = g_settings.gpu_wireframe_mode;
  m_true_color = g_settings.gpu_true_color;
  m_clamp_uvs = clamp_uvs;
  m_compute_uv_range = m_clamp_uvs;
  m_allow_sprite_mode = ShouldAllowSpriteMode(resolution_scale, m_texture_filtering, m_sprite_texture_filtering);
  m_batch.sprite_mode = (m_allow_sprite_mode && m_batch.sprite_mode);

  const bool depth_buffer_changed = (m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer());
  if (depth_buffer_changed)
  {
    m_pgxp_depth_buffer = g_settings.UsingPGXPDepthBuffer();
    m_batch.use_depth_buffer = false;
    m_depth_was_copied = false;
  }

  CheckSettings();

  UpdateSoftwareRenderer(true);

  PrintSettingsToLog();

  if (shaders_changed)
  {
    DestroyPipelines();

    Error error;
    if (!CompilePipelines(&error))
    {
      ERROR_LOG("Failed to recompile pipelines: {}", error.GetDescription());
      Panic("Failed to recompile pipelines.");
    }
  }

  if (framebuffer_changed)
  {
    // When using very high upscaling, it's possible that we don't have enough VRAM for two sets of buffers.
    // Purge the pool, and idle the GPU so that all video memory is freed prior to creating the new buffers.
    g_gpu_device->PurgeTexturePool();
    g_gpu_device->ExecuteAndWaitForGPUIdle();

    if (!CreateBuffers())
      Panic("Failed to recreate buffers.");

    UpdateDownsamplingLevels();
    RestoreDeviceContext();
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, g_vram, false, false);
    if (m_write_mask_as_depth)
      UpdateDepthBufferFromMaskBit();
    UpdateDisplay();
  }
  else if (m_vram_depth_texture && depth_buffer_changed)
  {
    if (m_pgxp_depth_buffer)
      ClearDepthBuffer();
    else if (m_write_mask_as_depth)
      UpdateDepthBufferFromMaskBit();
  }

  if (g_settings.gpu_downsample_mode != old_settings.gpu_downsample_mode ||
      (g_settings.gpu_downsample_mode == GPUDownsampleMode::Box &&
       g_settings.gpu_downsample_scale != old_settings.gpu_downsample_scale))
  {
    UpdateDownsamplingLevels();
  }
}

void GPU_HW::CheckSettings()
{
  const GPUDevice::Features features = g_gpu_device->GetFeatures();

  if (m_multisamples != g_settings.gpu_multisamples)
  {
    Host::AddIconOSDMessage("MSAAUnsupported", ICON_FA_EXCLAMATION_TRIANGLE,
                            fmt::format(TRANSLATE_FS("GPU_HW", "{}x MSAA is not supported, using {}x instead."),
                                        g_settings.gpu_multisamples, m_multisamples),
                            Host::OSD_CRITICAL_ERROR_DURATION);
  }
  else
  {
    Host::RemoveKeyedOSDMessage("MSAAUnsupported");
  }

  if (g_settings.gpu_per_sample_shading && !features.per_sample_shading)
  {
    Host::AddIconOSDMessage("SSAAUnsupported", ICON_FA_EXCLAMATION_TRIANGLE,
                            TRANSLATE_STR("GPU_HW", "SSAA is not supported, using MSAA instead."),
                            Host::OSD_ERROR_DURATION);
  }
  if (!features.dual_source_blend && !features.framebuffer_fetch &&
      (IsBlendedTextureFiltering(m_texture_filtering) || IsBlendedTextureFiltering(m_sprite_texture_filtering)))
  {
    Host::AddIconOSDMessage(
      "TextureFilterUnsupported", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("GPU_HW", "Texture filter '{}/{}' is not supported with the current renderer."),
                  Settings::GetTextureFilterDisplayName(m_texture_filtering),
                  Settings::GetTextureFilterName(m_sprite_texture_filtering), Host::OSD_ERROR_DURATION));
    m_texture_filtering = GPUTextureFilter::Nearest;
    m_sprite_texture_filtering = GPUTextureFilter::Nearest;
    m_allow_sprite_mode = ShouldAllowSpriteMode(m_resolution_scale, m_texture_filtering, m_sprite_texture_filtering);
  }

  if (g_settings.IsUsingAccurateBlending() && !m_supports_framebuffer_fetch && !features.feedback_loops &&
      !features.raster_order_views)
  {
    // m_allow_shader_blend/m_prefer_shader_blend will be cleared in pipeline compile.
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_FA_PAINT_BRUSH,
      TRANSLATE_STR("GPU_HW", "Accurate blending is not supported by your current GPU.\nIt requires framebuffer fetch, "
                              "feedback loops, or rasterizer order views."),
      Host::OSD_WARNING_DURATION);
  }
  else if (IsUsingMultisampling() && !features.framebuffer_fetch &&
           ((g_settings.IsUsingAccurateBlending() && features.raster_order_views) ||
            (m_pgxp_depth_buffer && features.raster_order_views && !features.feedback_loops)))
  {
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_FA_PAINT_BRUSH,
      TRANSLATE_STR("GPU_HW", "Multisample anti-aliasing is not supported when using ROV blending."),
      Host::OSD_WARNING_DURATION);
    m_multisamples = 1;
  }

  if (m_pgxp_depth_buffer && !features.feedback_loops && !features.framebuffer_fetch && !features.raster_order_views)
  {
    Host::AddIconOSDMessage(
      "AccurateBlendingUnsupported", ICON_FA_PAINT_BRUSH,
      TRANSLATE_STR("GPU_HW", "PGXP depth buffer is not supported by your current GPU or renderer.\nIt requires "
                              "framebuffer fetch, feedback loops, or rasterizer order views."),
      Host::OSD_WARNING_DURATION);
    m_pgxp_depth_buffer = false;
  }

  if (!features.noperspective_interpolation && !ShouldDisableColorPerspective())
    WARNING_LOG("Disable color perspective not supported, but should be used.");

  if (!features.geometry_shaders && m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    Host::AddIconOSDMessage(
      "GeometryShadersUnsupported", ICON_FA_EXCLAMATION_TRIANGLE,
      TRANSLATE("GPU_HW", "Geometry shaders are not supported by your GPU, and are required for wireframe rendering."),
      Host::OSD_CRITICAL_ERROR_DURATION);
    m_wireframe_mode = GPUWireframeMode::Disabled;
  }

  if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    const u32 resolution_scale = CalculateResolutionScale();
    const u32 box_downscale = GetBoxDownsampleScale(resolution_scale);
    if (box_downscale != g_settings.gpu_downsample_scale || box_downscale == resolution_scale)
    {
      Host::AddIconOSDMessage(
        "BoxDownsampleUnsupported", ICON_FA_PAINT_BRUSH,
        fmt::format(TRANSLATE_FS(
                      "GPU_HW", "Resolution scale {0}x is not divisible by downsample scale {1}x, using {2}x instead."),
                    resolution_scale, g_settings.gpu_downsample_scale, box_downscale),
        Host::OSD_WARNING_DURATION);
    }
    else
    {
      Host::RemoveKeyedOSDMessage("BoxDownsampleUnsupported");
    }

    if (box_downscale == g_settings.gpu_resolution_scale)
      m_downsample_mode = GPUDownsampleMode::Disabled;
  }
}

u32 GPU_HW::CalculateResolutionScale() const
{
  const u32 max_resolution_scale = GetMaxResolutionScale();

  u32 scale;
  if (g_settings.gpu_resolution_scale != 0)
  {
    scale = std::clamp<u32>(g_settings.gpu_resolution_scale, 1, max_resolution_scale);
  }
  else
  {
    // Auto scaling. When the system is starting and all borders crop is enabled, the registers are zero, and
    // display_height therefore is also zero. Use the default size from the region in this case.
    const s32 height = (m_crtc_state.display_height != 0) ?
                         static_cast<s32>(m_crtc_state.display_height) :
                         (m_console_is_pal ? (PAL_VERTICAL_ACTIVE_END - PAL_VERTICAL_ACTIVE_START) :
                                             (NTSC_VERTICAL_ACTIVE_END - NTSC_VERTICAL_ACTIVE_START));

    float widescreen_multiplier = 1.0f;
    if (g_settings.gpu_widescreen_hack)
    {
      // Multiply scale factor by aspect ratio relative to 4:3, so that widescreen resolution is as close as possible to
      // native screen resolution. Otherwise, anamorphic stretching would result in increasingly less horizontal
      // resolution (relative to native screen resolution) as the aspect ratio gets wider.
      widescreen_multiplier = std::max(1.0f, (static_cast<float>(g_gpu_device->GetWindowWidth()) /
                                              static_cast<float>(g_gpu_device->GetWindowHeight())) /
                                               (4.0f / 3.0f));
    }

    const s32 preferred_scale =
      static_cast<s32>(std::ceil(static_cast<float>(g_gpu_device->GetWindowHeight() * widescreen_multiplier) / height));
    VERBOSE_LOG("Height = {}, preferred scale = {}", height, preferred_scale);

    scale = static_cast<u32>(std::clamp<s32>(preferred_scale, 1, max_resolution_scale));
  }

  if (g_settings.gpu_downsample_mode == GPUDownsampleMode::Adaptive && scale > 1 && !Common::IsPow2(scale))
  {
    const u32 new_scale = Common::PreviousPow2(scale);
    WARNING_LOG("Resolution scale {}x not supported for adaptive downsampling, using {}x", scale, new_scale);

    if (g_settings.gpu_resolution_scale != 0)
    {
      Host::AddIconOSDMessage(
        "ResolutionNotPow2", ICON_FA_PAINT_BRUSH,
        fmt::format(
          TRANSLATE_FS("GPU_HW", "Resolution scale {0}x not supported for adaptive downsampling, using {1}x."), scale,
          new_scale),
        Host::OSD_WARNING_DURATION);
    }

    scale = new_scale;
  }

  return scale;
}

void GPU_HW::UpdateResolutionScale()
{
  GPU::UpdateResolutionScale();

  if (CalculateResolutionScale() != m_resolution_scale)
    UpdateSettings(g_settings);
}

GPUDownsampleMode GPU_HW::GetDownsampleMode(u32 resolution_scale) const
{
  return (resolution_scale == 1) ? GPUDownsampleMode::Disabled : g_settings.gpu_downsample_mode;
}

bool GPU_HW::IsUsingMultisampling() const
{
  return m_multisamples > 1;
}

bool GPU_HW::IsUsingDownsampling() const
{
  return (m_downsample_mode != GPUDownsampleMode::Disabled && !m_GPUSTAT.display_area_color_depth_24);
}

void GPU_HW::SetFullVRAMDirtyRectangle()
{
  m_vram_dirty_draw_rect = VRAM_SIZE_RECT;
  m_draw_mode.SetTexturePageChanged();
}

void GPU_HW::ClearVRAMDirtyRectangle()
{
  m_vram_dirty_draw_rect = INVALID_RECT;
  m_vram_dirty_write_rect = INVALID_RECT;
}

void GPU_HW::AddWrittenRectangle(const GSVector4i rect)
{
  m_vram_dirty_write_rect = m_vram_dirty_write_rect.runion(rect);
  SetTexPageChangedOnOverlap(m_vram_dirty_write_rect);
}

void GPU_HW::AddDrawnRectangle(const GSVector4i rect)
{
  // Normally, we would check for overlap here. But the GPU's texture cache won't actually reload until the page
  // changes, or it samples a larger region, so we can get away without doing so. This reduces copies considerably in
  // games like Mega Man Legends 2.
  m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(rect);
}

void GPU_HW::AddUnclampedDrawnRectangle(const GSVector4i rect)
{
  m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(rect);
  SetTexPageChangedOnOverlap(m_vram_dirty_draw_rect);
}

void GPU_HW::SetTexPageChangedOnOverlap(const GSVector4i update_rect)
{
  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (!m_draw_mode.IsTexturePageChanged() && m_batch.texture_mode != BatchTextureMode::Disabled &&
      (m_draw_mode.mode_reg.GetTexturePageRectangle().rintersects(update_rect) ||
       (m_draw_mode.mode_reg.IsUsingPalette() &&
        m_draw_mode.palette_reg.GetRectangle(m_draw_mode.mode_reg.texture_mode).rintersects(update_rect))))
  {
    m_draw_mode.SetTexturePageChanged();
  }
}

std::tuple<u32, u32> GPU_HW::GetEffectiveDisplayResolution(bool scaled /* = true */)
{
  const u32 scale = scaled ? m_resolution_scale : 1u;
  return std::make_tuple(m_crtc_state.display_vram_width * scale, m_crtc_state.display_vram_height * scale);
}

std::tuple<u32, u32> GPU_HW::GetFullDisplayResolution(bool scaled /* = true */)
{
  const u32 scale = scaled ? m_resolution_scale : 1u;
  return std::make_tuple(m_crtc_state.display_width * scale, m_crtc_state.display_height * scale);
}

void GPU_HW::PrintSettingsToLog()
{
  INFO_LOG("Resolution Scale: {} ({}x{}), maximum {}", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
           VRAM_HEIGHT * m_resolution_scale, GetMaxResolutionScale());
  INFO_LOG("Multisampling: {}x{}", m_multisamples,
           (g_settings.gpu_per_sample_shading && g_gpu_device->GetFeatures().per_sample_shading) ?
             " (per sample shading)" :
             "");
  INFO_LOG("Dithering: {}{}", m_true_color ? "Disabled" : "Enabled",
           (!m_true_color && g_settings.gpu_scaled_dithering) ?
             " (Scaled)" :
             ((m_true_color && g_settings.gpu_debanding) ? " (Debanding)" : ""));
  INFO_LOG("Force round texture coordinates: {}",
           (m_resolution_scale > 1 && g_settings.gpu_force_round_texcoords) ? "Enabled" : "Disabled");
  INFO_LOG("Texture Filtering: {}/{}", Settings::GetTextureFilterDisplayName(m_texture_filtering),
           Settings::GetTextureFilterDisplayName(m_sprite_texture_filtering));
  INFO_LOG("Dual-source blending: {}", m_supports_dual_source_blend ? "Supported" : "Not supported");
  INFO_LOG("Clamping UVs: {}", m_clamp_uvs ? "YES" : "NO");
  INFO_LOG("Depth buffer: {}", m_pgxp_depth_buffer ? "YES" : "NO");
  INFO_LOG("Downsampling: {}", Settings::GetDownsampleModeDisplayName(m_downsample_mode));
  INFO_LOG("Wireframe rendering: {}", Settings::GetGPUWireframeModeDisplayName(m_wireframe_mode));
  INFO_LOG("Line detection: {}", Settings::GetLineDetectModeDisplayName(m_line_detect_mode));
  INFO_LOG("Using software renderer for readbacks: {}", m_sw_renderer ? "YES" : "NO");
  INFO_LOG("Separate sprite shaders: {}", m_allow_sprite_mode ? "YES" : "NO");
}

GPUTexture::Format GPU_HW::GetDepthBufferFormat() const
{
  // Use 32-bit depth for PGXP depth buffer, otherwise 16-bit for mask bit.
  return m_pgxp_depth_buffer ? (m_use_rov_for_shader_blend ? VRAM_DS_COLOR_FORMAT : VRAM_DS_DEPTH_FORMAT) :
                               VRAM_DS_FORMAT;
}

bool GPU_HW::CreateBuffers()
{
  DestroyBuffers();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const u8 samples = static_cast<u8>(m_multisamples);
  const bool needs_depth_buffer = m_write_mask_as_depth || m_pgxp_depth_buffer;

  // Needed for Metal resolve.
  const GPUTexture::Type read_texture_type = (g_gpu_device->GetRenderAPI() == RenderAPI::Metal && m_multisamples > 1) ?
                                               GPUTexture::Type::RWTexture :
                                               GPUTexture::Type::Texture;
  const GPUTexture::Type vram_texture_type =
    m_use_rov_for_shader_blend ? GPUTexture::Type::RWTexture : GPUTexture::Type::RenderTarget;
  const GPUTexture::Type depth_texture_type =
    m_use_rov_for_shader_blend ? GPUTexture::Type::RWTexture : GPUTexture::Type::DepthStencil;

  if (!(m_vram_texture = g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, samples, vram_texture_type,
                                                    VRAM_RT_FORMAT)) ||
      (needs_depth_buffer &&
       !(m_vram_depth_texture = g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, samples,
                                                           depth_texture_type, GetDepthBufferFormat()))) ||
      (m_pgxp_depth_buffer && !(m_vram_depth_copy_texture =
                                  g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, samples,
                                                             GPUTexture::Type::RenderTarget, VRAM_DS_COLOR_FORMAT))) ||
      !(m_vram_read_texture =
          g_gpu_device->FetchTexture(texture_width, texture_height, 1, 1, 1, read_texture_type, VRAM_RT_FORMAT)) ||
      !(m_vram_readback_texture = g_gpu_device->FetchTexture(VRAM_WIDTH / 2, VRAM_HEIGHT, 1, 1, 1,
                                                             GPUTexture::Type::RenderTarget, VRAM_RT_FORMAT)))
  {
    return false;
  }

  GL_OBJECT_NAME(m_vram_texture, "VRAM Texture");
  if (m_vram_depth_texture)
    GL_OBJECT_NAME(m_vram_depth_texture, "VRAM Depth Texture");
  GL_OBJECT_NAME(m_vram_read_texture, "VRAM Read Texture");
  GL_OBJECT_NAME(m_vram_readback_texture, "VRAM Readback Texture");

  if (g_gpu_device->GetFeatures().memory_import)
  {
    DEV_LOG("Trying to import guest VRAM buffer for downloads...");
    m_vram_readback_download_texture = g_gpu_device->CreateDownloadTexture(
      m_vram_readback_texture->GetWidth(), m_vram_readback_texture->GetHeight(), m_vram_readback_texture->GetFormat(),
      g_vram, sizeof(g_vram), VRAM_WIDTH * sizeof(u16));
    if (!m_vram_readback_download_texture)
      ERROR_LOG("Failed to create imported readback buffer");
  }
  if (!m_vram_readback_download_texture)
  {
    m_vram_readback_download_texture = g_gpu_device->CreateDownloadTexture(
      m_vram_readback_texture->GetWidth(), m_vram_readback_texture->GetHeight(), m_vram_readback_texture->GetFormat());
    if (!m_vram_readback_download_texture)
    {
      ERROR_LOG("Failed to create readback download texture");
      return false;
    }
  }

  if (g_gpu_device->GetFeatures().supports_texture_buffers)
  {
    if (!(m_vram_upload_buffer =
            g_gpu_device->CreateTextureBuffer(GPUTextureBuffer::Format::R16UI, GPUDevice::MIN_TEXEL_BUFFER_ELEMENTS)))
    {
      return false;
    }

    GL_OBJECT_NAME(m_vram_upload_buffer, "VRAM Upload Buffer");
  }

  INFO_LOG("Created HW framebuffer of {}x{}", texture_width, texture_height);

  SetVRAMRenderTarget();
  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW::ClearFramebuffer()
{
  g_gpu_device->ClearRenderTarget(m_vram_texture.get(), 0);
  if (m_vram_depth_texture)
  {
    if (m_use_rov_for_shader_blend)
      g_gpu_device->ClearRenderTarget(m_vram_depth_texture.get(), 0xFF);
    else
      g_gpu_device->ClearDepth(m_vram_depth_texture.get(), m_pgxp_depth_buffer ? 1.0f : 0.0f);
  }
  ClearVRAMDirtyRectangle();
  m_last_depth_z = 1.0f;
}

void GPU_HW::SetVRAMRenderTarget()
{
  if (m_use_rov_for_shader_blend)
  {
    GPUTexture* rts[2] = {m_vram_texture.get(), m_vram_depth_texture.get()};
    const u32 num_rts = m_pgxp_depth_buffer ? 2 : 1;
    g_gpu_device->SetRenderTargets(
      rts, num_rts, nullptr, m_rov_active ? GPUPipeline::BindRenderTargetsAsImages : GPUPipeline::NoRenderPassFlags);
  }
  else
  {
    g_gpu_device->SetRenderTarget(m_vram_texture.get(), m_vram_depth_texture.get(),
                                  ((m_allow_shader_blend && !m_use_rov_for_shader_blend) ?
                                     GPUPipeline::ColorFeedbackLoop :
                                     GPUPipeline::NoRenderPassFlags));
  }
}

void GPU_HW::DeactivateROV()
{
  if (!m_rov_active)
    return;

  GL_INS("Deactivating ROV.");
  m_rov_active = false;
  SetVRAMRenderTarget();
}

void GPU_HW::DestroyBuffers()
{
  ClearDisplayTexture();

  DebugAssert((m_batch_vertex_ptr != nullptr) == (m_batch_index_ptr != nullptr));
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(0, 0);

  m_vram_upload_buffer.reset();
  m_vram_readback_download_texture.reset();
  g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_extract_depth_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_extract_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_read_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_depth_copy_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_depth_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_texture));
  g_gpu_device->RecycleTexture(std::move(m_vram_readback_texture));
}

bool GPU_HW::CompilePipelines(Error* error)
{
  const GPUDevice::Features features = g_gpu_device->GetFeatures();
  const bool per_sample_shading = g_settings.gpu_per_sample_shading && features.per_sample_shading;
  const bool force_round_texcoords = (m_resolution_scale > 1 && m_texture_filtering == GPUTextureFilter::Nearest &&
                                      g_settings.gpu_force_round_texcoords);

  // Determine when to use shader blending.
  // FBFetch is free, we need it for filtering without DSB, or when accurate blending is forced.
  // But, don't bother with accurate blending if true colour is on. The result will be the same.
  // Prefer ROV over barriers/feedback loops without FBFetch, it'll be faster.
  // Abuse the depth buffer for the mask bit when it's free (FBFetch), or PGXP depth buffering is enabled.
  m_allow_shader_blend = features.framebuffer_fetch ||
                         ((features.feedback_loops || features.raster_order_views) &&
                          (m_pgxp_depth_buffer || g_settings.gpu_accurate_blending ||
                           (!m_supports_dual_source_blend && (IsBlendedTextureFiltering(m_texture_filtering) ||
                                                              IsBlendedTextureFiltering(m_sprite_texture_filtering)))));
  m_prefer_shader_blend = (m_allow_shader_blend && g_settings.gpu_accurate_blending && !g_settings.gpu_true_color);
  m_use_rov_for_shader_blend = (m_allow_shader_blend && !features.framebuffer_fetch && features.raster_order_views &&
                                (m_prefer_shader_blend || !features.feedback_loops));
  m_write_mask_as_depth = (!m_pgxp_depth_buffer && !features.framebuffer_fetch && !m_prefer_shader_blend);

  // ROV doesn't support MSAA in DirectX.
  Assert(!m_use_rov_for_shader_blend || !IsUsingMultisampling());

  const bool needs_depth_buffer = (m_pgxp_depth_buffer || m_write_mask_as_depth);
  const bool needs_rov_depth = (m_pgxp_depth_buffer && m_use_rov_for_shader_blend);
  const bool needs_real_depth_buffer = (needs_depth_buffer && !needs_rov_depth);
  const bool needs_feedback_loop = (m_allow_shader_blend && features.feedback_loops && !m_use_rov_for_shader_blend);
  const GPUTexture::Format depth_buffer_format =
    needs_depth_buffer ? GetDepthBufferFormat() : GPUTexture::Format::Unknown;

  // Logging in case something goes wrong.
  INFO_LOG("Shader blending allowed: {}", m_allow_shader_blend ? "YES" : "NO");
  INFO_LOG("Shader blending preferred: {}", m_prefer_shader_blend ? "YES" : "NO");
  INFO_LOG("Use ROV for shader blending: {}", m_use_rov_for_shader_blend ? "YES" : "NO");
  INFO_LOG("Write mask as depth: {}", m_write_mask_as_depth ? "YES" : "NO");
  INFO_LOG("Depth buffer is {}needed in {}.", needs_depth_buffer ? "" : "NOT ",
           GPUTexture::GetFormatName(GetDepthBufferFormat()));
  INFO_LOG("Using ROV depth: {}", needs_rov_depth ? "YES" : "NO");
  INFO_LOG("Using real depth buffer: {}", needs_real_depth_buffer ? "YES" : "NO");
  INFO_LOG("Using feedback loops: {}", needs_feedback_loop ? "YES" : "NO");

  // Start generating shaders.
  GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), m_resolution_scale, m_multisamples, per_sample_shading,
                             m_true_color, (m_resolution_scale > 1 && g_settings.gpu_scaled_dithering),
                             m_write_mask_as_depth, ShouldDisableColorPerspective(), m_supports_dual_source_blend,
                             m_supports_framebuffer_fetch, g_settings.gpu_true_color && g_settings.gpu_debanding);

  const u32 active_texture_modes =
    m_allow_sprite_mode ? NUM_TEXTURE_MODES :
                          (NUM_TEXTURE_MODES - (NUM_TEXTURE_MODES - static_cast<u32>(BatchTextureMode::SpriteStart)));
  const u32 total_pipelines =
    (m_allow_sprite_mode ? 5 : 3) +                                                    // vertex shaders
    (active_texture_modes * 5 * 9 * 2 * 2 * 2 * (1 + BoolToUInt32(needs_rov_depth))) + // fragment shaders
    ((m_pgxp_depth_buffer ? 2 : 1) * 5 * 5 * active_texture_modes * 2 * 2 * 2) +       // batch pipelines
    ((m_wireframe_mode != GPUWireframeMode::Disabled) ? 1 : 0) +                       // wireframe
    1 +                                                                                // fullscreen quad VS
    (2 * 2) +                                                                          // vram fill
    (1 + BoolToUInt32(m_write_mask_as_depth)) +                                        // vram copy
    (1 + BoolToUInt32(m_write_mask_as_depth)) +                                        // vram write
    1 +                                                                                // vram write replacement
    (m_write_mask_as_depth ? 1 : 0) +                                                  // mask -> depth
    1 +                                                                                // vram read
    2 +                                                                                // extract/display
    ((m_downsample_mode != GPUDownsampleMode::Disabled) ? 1 : 0);                      // downsample

  ShaderCompileProgressTracker progress("Compiling Pipelines", total_pipelines);

  // vertex shaders - [textured/palette/sprite]
  // fragment shaders - [depth_test][render_mode][transparency_mode][texture_mode][check_mask][dithering][interlacing]
  static constexpr auto destroy_shader = [](std::unique_ptr<GPUShader>& s) { s.reset(); };
  DimensionalArray<std::unique_ptr<GPUShader>, 2, 2, 2> batch_vertex_shaders{};
  DimensionalArray<std::unique_ptr<GPUShader>, 2, 2, 2, NUM_TEXTURE_MODES, 5, 5, 2> batch_fragment_shaders{};
  ScopedGuard batch_shader_guard([&batch_vertex_shaders, &batch_fragment_shaders]() {
    batch_vertex_shaders.enumerate(destroy_shader);
    batch_fragment_shaders.enumerate(destroy_shader);
  });

  for (u8 textured = 0; textured < 2; textured++)
  {
    for (u8 palette = 0; palette < (textured ? 2 : 1); palette++)
    {
      for (u8 sprite = 0; sprite < (textured ? 2 : 1); sprite++)
      {
        const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
        const std::string vs = shadergen.GenerateBatchVertexShader(
          textured != 0, palette != 0, uv_limits, !sprite && force_round_texcoords, m_pgxp_depth_buffer);
        if (!(batch_vertex_shaders[textured][palette][sprite] =
                g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(), vs, error)))
        {
          return false;
        }

        progress.Increment();
      }
    }
  }

  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    if (depth_test && !needs_rov_depth)
    {
      // Don't need to do depth testing in the shader.
      continue;
    }

    for (u8 render_mode = 0; render_mode < 5; render_mode++)
    {
      for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
      {
        if (
          // Can't generate shader blending.
          ((render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && !m_allow_shader_blend) ||
           (render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend) &&
            transparency_mode != static_cast<u8>(GPUTransparencyMode::Disabled))) ||
          // Don't need multipass shaders if we're preferring shader blend or have (free) FBFetch.
          ((m_supports_framebuffer_fetch || m_prefer_shader_blend) &&
           (render_mode == static_cast<u8>(BatchRenderMode::OnlyOpaque) ||
            render_mode == static_cast<u8>(BatchRenderMode::OnlyTransparent))) ||
          // If using ROV depth, we only draw with shader blending.
          (needs_rov_depth && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend)))
        {
          progress.Increment(active_texture_modes * 2 * 2 * 2);
          continue;
        }

        for (u8 texture_mode = 0; texture_mode < active_texture_modes; texture_mode++)
        {
          for (u8 check_mask = 0; check_mask < 2; check_mask++)
          {
            if (check_mask && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend))
            {
              // mask bit testing is only valid with shader blending.
              progress.Increment(2 * 2);
              continue;
            }

            for (u8 dithering = 0; dithering < 2; dithering++)
            {
              for (u8 interlacing = 0; interlacing < 2; interlacing++)
              {
                const bool sprite = (static_cast<BatchTextureMode>(texture_mode) >= BatchTextureMode::SpriteStart);
                const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
                const BatchTextureMode shader_texmode = static_cast<BatchTextureMode>(
                  texture_mode - (sprite ? static_cast<u8>(BatchTextureMode::SpriteStart) : 0));
                const bool use_rov =
                  (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && m_use_rov_for_shader_blend);
                const std::string fs = shadergen.GenerateBatchFragmentShader(
                  static_cast<BatchRenderMode>(render_mode), static_cast<GPUTransparencyMode>(transparency_mode),
                  shader_texmode, sprite ? m_sprite_texture_filtering : m_texture_filtering, uv_limits,
                  !sprite && force_round_texcoords, ConvertToBoolUnchecked(dithering),
                  ConvertToBoolUnchecked(interlacing), ConvertToBoolUnchecked(check_mask), use_rov, needs_rov_depth,
                  (depth_test != 0));

                if (!(batch_fragment_shaders[depth_test][render_mode][transparency_mode][texture_mode][check_mask]
                                            [dithering][interlacing] = g_gpu_device->CreateShader(
                                              GPUShaderStage::Fragment, shadergen.GetLanguage(), fs, error)))
                {
                  return false;
                }

                progress.Increment();
              }
            }
          }
        }
      }
    }
  }

  static constexpr GPUPipeline::VertexAttribute vertex_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 4, OFFSETOF(BatchVertex, x)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::Color, 0,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(BatchVertex, color)),
    GPUPipeline::VertexAttribute::Make(2, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::UInt32, 1, OFFSETOF(BatchVertex, u)),
    GPUPipeline::VertexAttribute::Make(3, GPUPipeline::VertexAttribute::Semantic::TexCoord, 1,
                                       GPUPipeline::VertexAttribute::Type::UInt32, 1, OFFSETOF(BatchVertex, texpage)),
    GPUPipeline::VertexAttribute::Make(4, GPUPipeline::VertexAttribute::Semantic::TexCoord, 2,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(BatchVertex, uv_limits)),
  };
  static constexpr u32 NUM_BATCH_VERTEX_ATTRIBUTES = 2;
  static constexpr u32 NUM_BATCH_TEXTURED_VERTEX_ATTRIBUTES = 4;
  static constexpr u32 NUM_BATCH_TEXTURED_LIMITS_VERTEX_ATTRIBUTES = 5;

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndUBO;
  plconfig.input_layout.vertex_stride = sizeof(BatchVertex);
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.geometry_shader = nullptr;
  plconfig.samples = m_multisamples;
  plconfig.per_sample_shading = per_sample_shading;
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();

  // [depth_test][transparency_mode][render_mode][texture_mode][dithering][interlacing][check_mask]
  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    if (depth_test && !m_pgxp_depth_buffer)
    {
      // Not used.
      continue;
    }

    for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
    {
      for (u8 render_mode = 0; render_mode < 5; render_mode++)
      {
        if (
          // Can't generate shader blending.
          (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend) && !m_allow_shader_blend) ||
          // Don't need multipass shaders.
          ((m_supports_framebuffer_fetch || m_prefer_shader_blend) &&
           (render_mode == static_cast<u8>(BatchRenderMode::OnlyOpaque) ||
            render_mode == static_cast<u8>(BatchRenderMode::OnlyTransparent))) ||
          // If using ROV depth, we only draw with shader blending.
          (needs_rov_depth && render_mode != static_cast<u8>(BatchRenderMode::ShaderBlend)))
        {
          progress.Increment(9 * 2 * 2 * 2);
          continue;
        }

        for (u8 texture_mode = 0; texture_mode < active_texture_modes; texture_mode++)
        {
          for (u8 dithering = 0; dithering < 2; dithering++)
          {
            for (u8 interlacing = 0; interlacing < 2; interlacing++)
            {
              for (u8 check_mask = 0; check_mask < 2; check_mask++)
              {
                const bool textured = (static_cast<BatchTextureMode>(texture_mode) != BatchTextureMode::Disabled);
                const bool palette =
                  (static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::Palette4Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::Palette8Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::SpritePalette4Bit ||
                   static_cast<BatchTextureMode>(texture_mode) == BatchTextureMode::SpritePalette8Bit);
                const bool sprite = (static_cast<BatchTextureMode>(texture_mode) >= BatchTextureMode::SpriteStart);
                const bool uv_limits = ShouldClampUVs(sprite ? m_sprite_texture_filtering : m_texture_filtering);
                const bool use_shader_blending = (render_mode == static_cast<u8>(BatchRenderMode::ShaderBlend));
                const bool use_rov = (use_shader_blending && m_use_rov_for_shader_blend);
                plconfig.input_layout.vertex_attributes =
                  textured ?
                    (uv_limits ? std::span<const GPUPipeline::VertexAttribute>(
                                   vertex_attributes, NUM_BATCH_TEXTURED_LIMITS_VERTEX_ATTRIBUTES) :
                                 std::span<const GPUPipeline::VertexAttribute>(vertex_attributes,
                                                                               NUM_BATCH_TEXTURED_VERTEX_ATTRIBUTES)) :
                    std::span<const GPUPipeline::VertexAttribute>(vertex_attributes, NUM_BATCH_VERTEX_ATTRIBUTES);

                plconfig.vertex_shader =
                  batch_vertex_shaders[BoolToUInt8(textured)][BoolToUInt8(palette)][BoolToUInt8(sprite)].get();
                plconfig.fragment_shader =
                  batch_fragment_shaders[BoolToUInt8(depth_test && needs_rov_depth)][render_mode]
                                        [use_shader_blending ? transparency_mode :
                                                               static_cast<u8>(GPUTransparencyMode::Disabled)]
                                        [texture_mode][use_shader_blending ? check_mask : 0][dithering][interlacing]
                                          .get();
                Assert(plconfig.vertex_shader && plconfig.fragment_shader);

                if (needs_real_depth_buffer)
                {
                  plconfig.depth.depth_test =
                    m_pgxp_depth_buffer ?
                      (depth_test ? GPUPipeline::DepthFunc::LessEqual : GPUPipeline::DepthFunc::Always) :
                      (check_mask ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always);

                  // Don't write for transparent, but still test.
                  plconfig.depth.depth_write =
                    !m_pgxp_depth_buffer ||
                    (depth_test && transparency_mode == static_cast<u8>(GPUTransparencyMode::Disabled));
                }

                plconfig.SetTargetFormats(use_rov ? GPUTexture::Format::Unknown : VRAM_RT_FORMAT,
                                          needs_rov_depth ? GPUTexture::Format::Unknown : depth_buffer_format);
                plconfig.color_formats[1] = needs_rov_depth ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;
                plconfig.render_pass_flags =
                  use_rov ? GPUPipeline::BindRenderTargetsAsImages :
                            (needs_feedback_loop ? GPUPipeline::ColorFeedbackLoop : GPUPipeline::NoRenderPassFlags);

                plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();

                if (use_rov)
                {
                  plconfig.blend.write_mask = 0;
                }
                else if (!use_shader_blending &&
                         ((static_cast<GPUTransparencyMode>(transparency_mode) != GPUTransparencyMode::Disabled &&
                           (static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                            static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque)) ||
                          (textured &&
                           IsBlendedTextureFiltering(sprite ? m_sprite_texture_filtering : m_texture_filtering))))
                {
                  plconfig.blend.enable = true;
                  plconfig.blend.src_alpha_blend = GPUPipeline::BlendFunc::One;
                  plconfig.blend.dst_alpha_blend = GPUPipeline::BlendFunc::Zero;
                  plconfig.blend.alpha_blend_op = GPUPipeline::BlendOp::Add;

                  if (m_supports_dual_source_blend)
                  {
                    plconfig.blend.src_blend = GPUPipeline::BlendFunc::One;
                    plconfig.blend.dst_blend = GPUPipeline::BlendFunc::SrcAlpha1;
                    plconfig.blend.blend_op =
                      (static_cast<GPUTransparencyMode>(transparency_mode) ==
                         GPUTransparencyMode::BackgroundMinusForeground &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                        GPUPipeline::BlendOp::ReverseSubtract :
                        GPUPipeline::BlendOp::Add;
                  }
                  else
                  {
                    // TODO: This isn't entirely accurate, 127.5 versus 128.
                    // But if we use fbfetch on Mali, it doesn't matter.
                    plconfig.blend.src_blend = GPUPipeline::BlendFunc::One;
                    plconfig.blend.dst_blend = GPUPipeline::BlendFunc::One;
                    if (static_cast<GPUTransparencyMode>(transparency_mode) ==
                        GPUTransparencyMode::HalfBackgroundPlusHalfForeground)
                    {
                      plconfig.blend.dst_blend = GPUPipeline::BlendFunc::ConstantColor;
                      plconfig.blend.dst_alpha_blend = GPUPipeline::BlendFunc::ConstantColor;
                      plconfig.blend.constant = 0x00808080u;
                    }

                    plconfig.blend.blend_op =
                      (static_cast<GPUTransparencyMode>(transparency_mode) ==
                         GPUTransparencyMode::BackgroundMinusForeground &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                       static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                        GPUPipeline::BlendOp::ReverseSubtract :
                        GPUPipeline::BlendOp::Add;
                  }
                }

                if (!(m_batch_pipelines[depth_test][transparency_mode][render_mode][texture_mode][dithering]
                                       [interlacing][check_mask] = g_gpu_device->CreatePipeline(plconfig, error)))
                {
                  return false;
                }

                progress.Increment();
              }
            }
          }
        }
      }
    }
  }

  plconfig.SetTargetFormats(VRAM_RT_FORMAT, needs_rov_depth ? GPUTexture::Format::Unknown : depth_buffer_format);
  plconfig.render_pass_flags = needs_feedback_loop ? GPUPipeline::ColorFeedbackLoop : GPUPipeline::NoRenderPassFlags;

  if (m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    std::unique_ptr<GPUShader> gs = g_gpu_device->CreateShader(GPUShaderStage::Geometry, shadergen.GetLanguage(),
                                                               shadergen.GenerateWireframeGeometryShader(), error);
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateWireframeFragmentShader(), error);
    if (!gs || !fs)
      return false;

    GL_OBJECT_NAME(gs, "Batch Wireframe Geometry Shader");
    GL_OBJECT_NAME(fs, "Batch Wireframe Fragment Shader");

    plconfig.input_layout.vertex_attributes =
      std::span<const GPUPipeline::VertexAttribute>(vertex_attributes, NUM_BATCH_VERTEX_ATTRIBUTES);
    plconfig.blend = (m_wireframe_mode == GPUWireframeMode::OverlayWireframe) ?
                       GPUPipeline::BlendState::GetAlphaBlendingState() :
                       GPUPipeline::BlendState::GetNoBlendingState();
    plconfig.blend.write_mask = 0x7;
    plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
    plconfig.vertex_shader = batch_vertex_shaders[0][0][0].get();
    plconfig.geometry_shader = gs.get();
    plconfig.fragment_shader = fs.get();

    if (!(m_wireframe_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_wireframe_pipeline, "Batch Wireframe Pipeline");

    plconfig.vertex_shader = nullptr;
    plconfig.geometry_shader = nullptr;
    plconfig.fragment_shader = nullptr;

    progress.Increment();
  }

  batch_shader_guard.Run();

  // use a depth of 1, that way writes will reset the depth
  std::unique_ptr<GPUShader> fullscreen_quad_vertex_shader = g_gpu_device->CreateShader(
    GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateScreenQuadVertexShader(1.0f), error);
  if (!fullscreen_quad_vertex_shader)
    return false;

  progress.Increment();

  // common state
  plconfig.input_layout.vertex_attributes = {};
  plconfig.input_layout.vertex_stride = 0;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.per_sample_shading = false;
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.vertex_shader = fullscreen_quad_vertex_shader.get();
  plconfig.color_formats[1] = needs_rov_depth ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;

  // VRAM fill
  for (u8 wrapped = 0; wrapped < 2; wrapped++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(),
        shadergen.GenerateVRAMFillFragmentShader(ConvertToBoolUnchecked(wrapped), ConvertToBoolUnchecked(interlaced)),
        error);
      if (!fs)
        return false;

      plconfig.fragment_shader = fs.get();
      plconfig.depth = needs_real_depth_buffer ? GPUPipeline::DepthState::GetAlwaysWriteState() :
                                                 GPUPipeline::DepthState::GetNoTestsState();

      if (!(m_vram_fill_pipelines[wrapped][interlaced] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      progress.Increment();
    }
  }

  // VRAM copy
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateVRAMCopyFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      if (depth_test && !m_write_mask_as_depth)
        continue;

      plconfig.depth.depth_write = needs_real_depth_buffer;
      plconfig.depth.depth_test =
        (depth_test != 0) ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always;

      if (!(m_vram_copy_pipelines[depth_test] = g_gpu_device->CreatePipeline(plconfig), error))
        return false;

      GL_OBJECT_NAME_FMT(m_vram_copy_pipelines[depth_test], "VRAM Write Pipeline, depth={}", depth_test);

      progress.Increment();
    }
  }

  // VRAM write
  {
    const bool use_buffer = features.supports_texture_buffers;
    const bool use_ssbo = features.texture_buffers_emulated_with_ssbo;
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateVRAMWriteFragmentShader(use_buffer, use_ssbo), error);
    if (!fs)
      return false;

    plconfig.layout = use_buffer ? GPUPipeline::Layout::SingleTextureBufferAndPushConstants :
                                   GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.fragment_shader = fs.get();
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      if (depth_test && !m_write_mask_as_depth)
        continue;

      plconfig.depth.depth_write = needs_real_depth_buffer;
      plconfig.depth.depth_test =
        (depth_test != 0) ? GPUPipeline::DepthFunc::GreaterEqual : GPUPipeline::DepthFunc::Always;

      if (!(m_vram_write_pipelines[depth_test] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      GL_OBJECT_NAME_FMT(m_vram_write_pipelines[depth_test], "VRAM Write Pipeline, depth={}", depth_test);

      progress.Increment();
    }
  }

  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;

  // VRAM write replacement
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateCopyFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
    if (!(m_vram_write_replacement_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    progress.Increment();
  }

  // VRAM update depth
  if (m_write_mask_as_depth)
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateVRAMUpdateDepthFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(GPUTexture::Format::Unknown, depth_buffer_format);
    plconfig.depth = GPUPipeline::DepthState::GetAlwaysWriteState();
    plconfig.blend.write_mask = 0;

    if (!(m_vram_update_depth_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_vram_update_depth_pipeline, "VRAM Update Depth Pipeline");

    progress.Increment();
  }

  plconfig.SetTargetFormats(VRAM_RT_FORMAT);
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;

  // VRAM read
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateVRAMReadFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();

    if (!(m_vram_readback_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_vram_readback_pipeline, "VRAM Read Pipeline");
    progress.Increment();
  }

  // Display
  {
    for (u8 shader = 0; shader < 3; shader++)
    {
      // 24-bit doesn't give you a depth buffer.
      const bool color_24bit = (shader == 1);
      const bool depth_extract = (shader == 2);
      if (depth_extract && !m_pgxp_depth_buffer)
        continue;

      std::unique_ptr<GPUShader> fs =
        g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                   shadergen.GenerateVRAMExtractFragmentShader(color_24bit, depth_extract), error);
      if (!fs)
        return false;

      plconfig.fragment_shader = fs.get();

      plconfig.layout = depth_extract ? GPUPipeline::Layout::MultiTextureAndPushConstants :
                                        GPUPipeline::Layout::SingleTextureAndPushConstants;
      plconfig.color_formats[1] = depth_extract ? VRAM_DS_COLOR_FORMAT : GPUTexture::Format::Unknown;

      if (!(m_vram_extract_pipeline[shader] = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;

      progress.Increment();
    }
  }

  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;

  if (m_pgxp_depth_buffer)
  {
    std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                               shadergen.GenerateCopyFragmentShader(), error);
    if (!fs)
      return false;

    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(VRAM_DS_COLOR_FORMAT);
    if (!(m_copy_depth_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
  }

  plconfig.SetTargetFormats(VRAM_RT_FORMAT);

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    std::unique_ptr<GPUShader> vs = g_gpu_device->CreateShader(
      GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateAdaptiveDownsampleVertexShader(), error);
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateAdaptiveDownsampleMipFragmentShader(true), error);
    if (!vs || !fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Vertex Shader");
    GL_OBJECT_NAME(fs, "Downsample First Pass Fragment Shader");
    plconfig.vertex_shader = vs.get();
    plconfig.fragment_shader = fs.get();
    if (!(m_downsample_first_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_first_pass_pipeline, "Downsample First Pass Pipeline");

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateAdaptiveDownsampleMipFragmentShader(false), error);
    if (!fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Mid Pass Fragment Shader");
    plconfig.fragment_shader = fs.get();
    if (!(m_downsample_mid_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_mid_pass_pipeline, "Downsample Mid Pass Pipeline");

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateAdaptiveDownsampleBlurFragmentShader(), error);
    if (!fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Blur Pass Fragment Shader");
    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(GPUTexture::Format::R8);
    if (!(m_downsample_blur_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_blur_pass_pipeline, "Downsample Blur Pass Pipeline");

    fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                    shadergen.GenerateAdaptiveDownsampleCompositeFragmentShader(), error);
    if (!fs)
      return false;
    GL_OBJECT_NAME(fs, "Downsample Composite Pass Fragment Shader");
    plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
    plconfig.fragment_shader = fs.get();
    plconfig.SetTargetFormats(VRAM_RT_FORMAT);
    if (!(m_downsample_composite_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_downsample_composite_pass_pipeline, "Downsample Blur Pass Pipeline");

    GPUSampler::Config config = GPUSampler::GetLinearConfig();
    config.min_lod = 0;
    config.max_lod = GPUSampler::Config::LOD_MAX;
    if (!(m_downsample_lod_sampler = g_gpu_device->CreateSampler(config)))
    {
      Error::SetStringView(error, "Failed to create downsample LOD sampler.");
      return false;
    }
    GL_OBJECT_NAME(m_downsample_lod_sampler, "Downsample LOD Sampler");
    config.mip_filter = GPUSampler::Filter::Linear;
    if (!(m_downsample_composite_sampler = g_gpu_device->CreateSampler(config)))
    {
      Error::SetStringView(error, "Failed to create downsample composite sampler.");
      return false;
    }
    GL_OBJECT_NAME(m_downsample_composite_sampler, "Downsample Trilinear Sampler");
    progress.Increment();
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    std::unique_ptr<GPUShader> fs =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                 shadergen.GenerateBoxSampleDownsampleFragmentShader(
                                   m_resolution_scale / GetBoxDownsampleScale(m_resolution_scale)),
                                 error);
    if (!fs)
      return false;

    GL_OBJECT_NAME(fs, "Downsample First Pass Fragment Shader");
    plconfig.fragment_shader = fs.get();

    if (!(m_downsample_first_pass_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_downsample_first_pass_pipeline, "Downsample First Pass Pipeline");
    progress.Increment();
  }

#undef UPDATE_PROGRESS

  return true;
}

void GPU_HW::DestroyPipelines()
{
  static constexpr auto destroy = [](std::unique_ptr<GPUPipeline>& p) { p.reset(); };

  m_wireframe_pipeline.reset();

  m_batch_pipelines.enumerate(destroy);

  m_vram_fill_pipelines.enumerate(destroy);

  for (std::unique_ptr<GPUPipeline>& p : m_vram_write_pipelines)
    destroy(p);

  for (std::unique_ptr<GPUPipeline>& p : m_vram_copy_pipelines)
    destroy(p);

  for (std::unique_ptr<GPUPipeline>& p : m_vram_extract_pipeline)
    destroy(p);

  destroy(m_vram_readback_pipeline);
  destroy(m_vram_update_depth_pipeline);
  destroy(m_vram_write_replacement_pipeline);

  destroy(m_downsample_first_pass_pipeline);
  destroy(m_downsample_mid_pass_pipeline);
  destroy(m_downsample_blur_pass_pipeline);
  destroy(m_downsample_composite_pass_pipeline);
  m_downsample_composite_sampler.reset();

  m_copy_depth_pipeline.reset();
}

GPU_HW::BatchRenderMode GPU_HW::BatchConfig::GetRenderMode() const
{
  return transparency_mode == GPUTransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                              BatchRenderMode::TransparentAndOpaque;
}

void GPU_HW::UpdateVRAMReadTexture(bool drawn, bool written)
{
  GL_SCOPE("UpdateVRAMReadTexture()");

  const auto update = [this](GSVector4i& rect, u8 dbit) {
    if (m_texpage_dirty & dbit)
    {
      m_texpage_dirty &= ~dbit;
      if (!m_texpage_dirty)
        GL_INS_FMT("{} texpage is no longer dirty", (dbit & TEXPAGE_DIRTY_DRAWN_RECT) ? "DRAW" : "WRITE");
    }

    const GSVector4i scaled_rect = rect.mul32l(GSVector4i(m_resolution_scale));
    if (m_vram_texture->IsMultisampled())
    {
      if (g_gpu_device->GetFeatures().partial_msaa_resolve)
      {
        g_gpu_device->ResolveTextureRegion(m_vram_read_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                           m_vram_texture.get(), scaled_rect.left, scaled_rect.top, scaled_rect.width(),
                                           scaled_rect.height());
      }
      else
      {
        g_gpu_device->ResolveTextureRegion(m_vram_read_texture.get(), 0, 0, 0, 0, m_vram_texture.get(), 0, 0,
                                           m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
      }
    }
    else
    {
      g_gpu_device->CopyTextureRegion(m_vram_read_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                      m_vram_texture.get(), scaled_rect.left, scaled_rect.top, 0, 0,
                                      scaled_rect.width(), scaled_rect.height());
    }

    // m_counters.num_read_texture_updates++;
    rect = INVALID_RECT;
  };

  if (drawn)
  {
    DebugAssert(!m_vram_dirty_draw_rect.eq(INVALID_RECT));
    GL_INS_FMT("Updating draw rect {}", m_vram_dirty_draw_rect);

    u8 dbits = TEXPAGE_DIRTY_DRAWN_RECT;
    if (written && m_vram_dirty_draw_rect.rintersects(m_vram_dirty_write_rect))
    {
      DebugAssert(!m_vram_dirty_write_rect.eq(INVALID_RECT));
      GL_INS_FMT("Including write rect {}", m_vram_dirty_write_rect);
      m_vram_dirty_draw_rect = m_vram_dirty_draw_rect.runion(m_vram_dirty_write_rect);
      m_vram_dirty_write_rect = INVALID_RECT;
      dbits = TEXPAGE_DIRTY_DRAWN_RECT | TEXPAGE_DIRTY_WRITTEN_RECT;
      written = false;
    }

    update(m_vram_dirty_draw_rect, dbits);
  }
  if (written)
  {
    GL_INS_FMT("Updating write rect {}", m_vram_dirty_write_rect);
    update(m_vram_dirty_write_rect, TEXPAGE_DIRTY_WRITTEN_RECT);
  }
}

void GPU_HW::UpdateDepthBufferFromMaskBit()
{
  DebugAssert(!m_pgxp_depth_buffer && m_vram_depth_texture && m_write_mask_as_depth);

  // Viewport should already be set full, only need to fudge the scissor.
  g_gpu_device->SetScissor(m_vram_texture->GetRect());
  g_gpu_device->InvalidateRenderTarget(m_vram_depth_texture.get());
  g_gpu_device->SetRenderTargets(nullptr, 0, m_vram_depth_texture.get());
  g_gpu_device->SetPipeline(m_vram_update_depth_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
  g_gpu_device->Draw(3, 0);

  // Restore.
  g_gpu_device->SetTextureSampler(0, m_vram_read_texture.get(), g_gpu_device->GetNearestSampler());
  SetVRAMRenderTarget();
  SetScissor();
}

void GPU_HW::CopyAndClearDepthBuffer()
{
  if (!m_depth_was_copied)
  {
    // Take a copy of the current depth buffer so it can be used when the previous frame/buffer gets scanned out.
    // Don't bother when we're not postprocessing, it'd just be a wasted copy.
    if (PostProcessing::InternalChain.NeedsDepthBuffer())
    {
      // TODO: Shrink this to only the active area.
      GL_SCOPE("Copy Depth Buffer");

      m_vram_texture->MakeReadyForSampling();
      g_gpu_device->InvalidateRenderTarget(m_vram_depth_copy_texture.get());
      g_gpu_device->SetRenderTarget(m_vram_depth_copy_texture.get());
      g_gpu_device->SetViewportAndScissor(0, 0, m_vram_depth_texture->GetWidth(), m_vram_depth_texture->GetHeight());
      g_gpu_device->SetTextureSampler(0, m_vram_depth_texture.get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetPipeline(m_copy_depth_pipeline.get());

      const float uniforms[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
      g_gpu_device->Draw(3, 0);
      RestoreDeviceContext();
    }

    m_depth_was_copied = true;
  }

  ClearDepthBuffer();
}

void GPU_HW::ClearDepthBuffer()
{
  GL_SCOPE("GPU_HW::ClearDepthBuffer()");
  DebugAssert(m_pgxp_depth_buffer);
  if (m_use_rov_for_shader_blend)
    g_gpu_device->ClearRenderTarget(m_vram_depth_texture.get(), 0xFF);
  else
    g_gpu_device->ClearDepth(m_vram_depth_texture.get(), 1.0f);
  m_last_depth_z = 1.0f;
}

void GPU_HW::SetScissor()
{
  g_gpu_device->SetScissor(m_clamped_drawing_area.mul32l(GSVector4i(m_resolution_scale)));
}

void GPU_HW::MapGPUBuffer(u32 required_vertices, u32 required_indices)
{
  DebugAssert(!m_batch_vertex_ptr && !m_batch_index_ptr);

  void* vb_map;
  u32 vb_space;
  g_gpu_device->MapVertexBuffer(sizeof(BatchVertex), required_vertices, &vb_map, &vb_space, &m_batch_base_vertex);
  m_batch_vertex_ptr = static_cast<BatchVertex*>(vb_map);
  m_batch_vertex_space = Truncate16(std::min<u32>(vb_space, std::numeric_limits<u16>::max()));

  u32 ib_space;
  g_gpu_device->MapIndexBuffer(required_indices, &m_batch_index_ptr, &ib_space, &m_batch_base_index);
  m_batch_index_space = Truncate16(std::min<u32>(ib_space, std::numeric_limits<u16>::max()));
}

void GPU_HW::UnmapGPUBuffer(u32 used_vertices, u32 used_indices)
{
  DebugAssert(m_batch_vertex_ptr && m_batch_index_ptr);
  g_gpu_device->UnmapVertexBuffer(sizeof(BatchVertex), used_vertices);
  g_gpu_device->UnmapIndexBuffer(used_indices);
  m_batch_vertex_ptr = nullptr;
  m_batch_vertex_count = 0;
  m_batch_vertex_space = 0;
  m_batch_index_ptr = nullptr;
  m_batch_index_count = 0;
  m_batch_index_space = 0;
}

ALWAYS_INLINE_RELEASE void GPU_HW::DrawBatchVertices(BatchRenderMode render_mode, u32 num_indices, u32 base_index,
                                                     u32 base_vertex)
{
  // [depth_test][transparency_mode][render_mode][texture_mode][dithering][interlacing][check_mask]
  const u8 texture_mode = static_cast<u8>(m_batch.texture_mode) +
                          ((m_batch.texture_mode != BatchTextureMode::Disabled && m_batch.sprite_mode) ?
                             static_cast<u8>(BatchTextureMode::SpriteStart) :
                             0);
  const u8 depth_test = BoolToUInt8(m_batch.use_depth_buffer);
  const u8 check_mask = BoolToUInt8(m_batch.check_mask_before_draw);
  g_gpu_device->SetPipeline(m_batch_pipelines[depth_test][static_cast<u8>(m_batch.transparency_mode)][static_cast<u8>(
    render_mode)][texture_mode][BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)][check_mask]
                              .get());

  GL_INS_FMT("Texture mode: {}", s_batch_texture_modes[texture_mode]);
  GL_INS_FMT("Transparency mode: {}", s_transparency_modes[static_cast<u8>(m_batch.transparency_mode)]);
  GL_INS_FMT("Render mode: {}", s_batch_render_modes[static_cast<u8>(render_mode)]);
  GL_INS_FMT("Mask bit test: {}", m_batch.check_mask_before_draw);
  GL_INS_FMT("Interlacing: {}", m_batch.check_mask_before_draw);

  // Activating ROV?
  if (render_mode == BatchRenderMode::ShaderBlend)
  {
    if (m_use_rov_for_shader_blend)
    {
      if (!m_rov_active)
      {
        GL_INS("Activating ROV.");
        m_rov_active = true;
        SetVRAMRenderTarget();
      }

      g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
    }
    else if (m_supports_framebuffer_fetch)
    {
      // No barriers needed for FBFetch.
      g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
    }
    else
    {
      // Barriers. Yucky.
      g_gpu_device->DrawIndexedWithBarrier(num_indices, base_index, base_vertex, GPUDevice::DrawBarrier::Full);
    }
  }
  else
  {
    g_gpu_device->DrawIndexed(num_indices, base_index, base_vertex);
  }
}

ALWAYS_INLINE_RELEASE void GPU_HW::HandleFlippedQuadTextureCoordinates(BatchVertex* vertices)
{
  // Taken from beetle-psx gpu_polygon.cpp
  // For X/Y flipped 2D sprites, PSX games rely on a very specific rasterization behavior. If U or V is decreasing in X
  // or Y, and we use the provided U/V as is, we will sample the wrong texel as interpolation covers an entire pixel,
  // while PSX samples its interpolation essentially in the top-left corner and splats that interpolant across the
  // entire pixel. While we could emulate this reasonably well in native resolution by shifting our vertex coords by
  // 0.5, this breaks in upscaling scenarios, because we have several samples per native sample and we need NN rules to
  // hit the same UV every time. One approach here is to use interpolate at offset or similar tricks to generalize the
  // PSX interpolation patterns, but the problem is that vertices sharing an edge will no longer see the same UV (due to
  // different plane derivatives), we end up sampling outside the intended boundary and artifacts are inevitable, so the
  // only case where we can apply this fixup is for "sprites" or similar which should not share edges, which leads to
  // this unfortunate code below.

  // It might be faster to do more direct checking here, but the code below handles primitives in any order and
  // orientation, and is far more SIMD-friendly if needed.
  const float abx = vertices[1].x - vertices[0].x;
  const float aby = vertices[1].y - vertices[0].y;
  const float bcx = vertices[2].x - vertices[1].x;
  const float bcy = vertices[2].y - vertices[1].y;
  const float cax = vertices[0].x - vertices[2].x;
  const float cay = vertices[0].y - vertices[2].y;

  // Hack for Wild Arms 2: The player sprite is drawn one line at a time with a quad, but the bottom V coordinates
  // are set to a large distance from the top V coordinate. When upscaling, this means that the coordinate is
  // interpolated between these two values, result in out-of-bounds sampling. At native, it's fine, because at the
  // top of the primitive, no amount is added to the coordinates. So, in this case, just set all coordinates to the
  // same value, from the first vertex, ensuring no interpolation occurs. Gate it based on the Y distance being one
  // pixel, limiting the risk of false positives.
  if (m_line_detect_mode == GPULineDetectMode::Quads &&
      (std::max(vertices[0].y, std::max(vertices[1].y, std::max(vertices[2].y, vertices[3].y))) -
       std::min(vertices[0].y, std::min(vertices[1].y, std::min(vertices[2].y, vertices[3].y)))) == 1.0f) [[unlikely]]
  {
    GL_INS_FMT("HLineQuad detected at [{},{}={},{} {},{}={},{} {},{}={},{} {},{}={},{}", vertices[0].x, vertices[0].y,
               vertices[0].u, vertices[0].v, vertices[1].x, vertices[1].y, vertices[1].u, vertices[1].v, vertices[2].x,
               vertices[2].y, vertices[2].u, vertices[2].v, vertices[3].x, vertices[3].y, vertices[3].u, vertices[3].v);
    vertices[1].v = vertices[0].v;
    vertices[2].v = vertices[0].v;
    vertices[3].v = vertices[0].v;
  }

  // Compute static derivatives, just assume W is uniform across the primitive and that the plane equation remains the
  // same across the quad. (which it is, there is no Z.. yet).
  const float dudx = -aby * static_cast<float>(vertices[2].u) - bcy * static_cast<float>(vertices[0].u) -
                     cay * static_cast<float>(vertices[1].u);
  const float dvdx = -aby * static_cast<float>(vertices[2].v) - bcy * static_cast<float>(vertices[0].v) -
                     cay * static_cast<float>(vertices[1].v);
  const float dudy = +abx * static_cast<float>(vertices[2].u) + bcx * static_cast<float>(vertices[0].u) +
                     cax * static_cast<float>(vertices[1].u);
  const float dvdy = +abx * static_cast<float>(vertices[2].v) + bcx * static_cast<float>(vertices[0].v) +
                     cax * static_cast<float>(vertices[1].v);
  const float area = bcx * cay - bcy * cax;

  // Detect and reject any triangles with 0 size texture area
  const s32 texArea = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
                      (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);

  // Shouldn't matter as degenerate primitives will be culled anyways.
  if (area == 0.0f || texArea == 0)
    return;

  // Use floats here as it'll be faster than integer divides.
  const float rcp_area = 1.0f / area;
  const float dudx_area = dudx * rcp_area;
  const float dudy_area = dudy * rcp_area;
  const float dvdx_area = dvdx * rcp_area;
  const float dvdy_area = dvdy * rcp_area;
  const bool neg_dudx = dudx_area < 0.0f;
  const bool neg_dudy = dudy_area < 0.0f;
  const bool neg_dvdx = dvdx_area < 0.0f;
  const bool neg_dvdy = dvdy_area < 0.0f;
  const bool zero_dudx = dudx_area == 0.0f;
  const bool zero_dudy = dudy_area == 0.0f;
  const bool zero_dvdx = dvdx_area == 0.0f;
  const bool zero_dvdy = dvdy_area == 0.0f;

  // If we have negative dU or dV in any direction, increment the U or V to work properly with nearest-neighbor in
  // this impl. If we don't have 1:1 pixel correspondence, this creates a slight "shift" in the sprite, but we
  // guarantee that we don't sample garbage at least. Overall, this is kinda hacky because there can be legitimate,
  // rare cases where 3D meshes hit this scenario, and a single texel offset can pop in, but this is way better than
  // having borked 2D overall.
  //
  // TODO: If perf becomes an issue, we can probably SIMD the 8 comparisons above,
  // create an 8-bit code, and use a LUT to get the offsets.
  // Case 1: U is decreasing in X, but no change in Y.
  // Case 2: U is decreasing in Y, but no change in X.
  // Case 3: V is decreasing in X, but no change in Y.
  // Case 4: V is decreasing in Y, but no change in X.
  if ((neg_dudx && zero_dudy) || (neg_dudy && zero_dudx))
  {
    vertices[0].u++;
    vertices[1].u++;
    vertices[2].u++;
    vertices[3].u++;
  }

  if ((neg_dvdx && zero_dvdy) || (neg_dvdy && zero_dvdx))
  {
    vertices[0].v++;
    vertices[1].v++;
    vertices[2].v++;
    vertices[3].v++;
  }

  // 2D polygons should have zero change in V on the X axis, and vice versa.
  if (m_allow_sprite_mode)
    SetBatchSpriteMode(zero_dudy && zero_dvdx);
}

bool GPU_HW::IsPossibleSpritePolygon(const BatchVertex* vertices) const
{
  const float abx = vertices[1].x - vertices[0].x;
  const float aby = vertices[1].y - vertices[0].y;
  const float bcx = vertices[2].x - vertices[1].x;
  const float bcy = vertices[2].y - vertices[1].y;
  const float cax = vertices[0].x - vertices[2].x;
  const float cay = vertices[0].y - vertices[2].y;
  const float dvdx = -aby * static_cast<float>(vertices[2].v) - bcy * static_cast<float>(vertices[0].v) -
                     cay * static_cast<float>(vertices[1].v);
  const float dudy = +abx * static_cast<float>(vertices[2].u) + bcx * static_cast<float>(vertices[0].u) +
                     cax * static_cast<float>(vertices[1].u);
  const float area = bcx * cay - bcy * cax;
  const s32 texArea = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
                      (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);

  // Doesn't matter.
  if (area == 0.0f || texArea == 0)
    return m_batch.sprite_mode;

  const float rcp_area = 1.0f / area;
  const bool zero_dudy = ((dudy * rcp_area) == 0.0f);
  const bool zero_dvdx = ((dvdx * rcp_area) == 0.0f);
  return (zero_dudy && zero_dvdx);
}

ALWAYS_INLINE_RELEASE bool GPU_HW::ExpandLineTriangles(BatchVertex* vertices)
{
  // Line expansion inspired by beetle-psx.
  BatchVertex *vshort, *vlong;
  bool vertical, horizontal;

  if (m_line_detect_mode == GPULineDetectMode::BasicTriangles)
  {
    // Given a tall/one-pixel-wide triangle, determine which vertex is the corner with axis-aligned edges.
    BatchVertex* vcorner;
    if (vertices[0].u == vertices[1].u && vertices[0].v == vertices[1].v)
    {
      // A,B,C
      vcorner = &vertices[0];
      vshort = &vertices[1];
      vlong = &vertices[2];
    }
    else if (vertices[1].u == vertices[2].u && vertices[1].v == vertices[2].v)
    {
      // B,C,A
      vcorner = &vertices[1];
      vshort = &vertices[2];
      vlong = &vertices[0];
    }
    else if (vertices[2].u == vertices[0].u && vertices[2].v == vertices[0].v)
    {
      // C,A,B
      vcorner = &vertices[2];
      vshort = &vertices[0];
      vlong = &vertices[1];
    }
    else
    {
      return false;
    }

    // Determine line direction. Vertical lines will have a width of 1, horizontal lines a height of 1.
    vertical = ((vcorner->y == vshort->y) && (std::abs(vcorner->x - vshort->x) == 1.0f));
    horizontal = ((vcorner->x == vshort->x) && (std::abs(vcorner->y - vshort->y) == 1.0f));
    if (vertical)
    {
      // Line should be vertical. Make sure the triangle is actually a right angle.
      if (vshort->x == vlong->x)
        std::swap(vshort, vcorner);
      else if (vcorner->x != vlong->x)
        return false;

      GL_INS_FMT("Vertical line from Y={} to {}", vcorner->y, vlong->y);
    }
    else if (horizontal)
    {
      // Line should be horizontal. Make sure the triangle is actually a right angle.
      if (vshort->y == vlong->y)
        std::swap(vshort, vcorner);
      else if (vcorner->y != vlong->y)
        return false;

      GL_INS_FMT("Horizontal line from X={} to {}", vcorner->x, vlong->x);
    }
    else
    {
      // Not a line-like triangle.
      return false;
    }

    // We could adjust the short texture coordinate to +1 from its original position, rather than leaving it the same.
    // However, since the texture is unlikely to be a higher resolution than the one-wide triangle, there would be no
    // benefit in doing so.
  }
  else
  {
    DebugAssert(m_line_detect_mode == GPULineDetectMode::AggressiveTriangles);

    // Find direction of line based on horizontal position.
    BatchVertex *va, *vb, *vc;
    if (vertices[0].x == vertices[1].x)
    {
      va = &vertices[0];
      vb = &vertices[1];
      vc = &vertices[2];
    }
    else if (vertices[1].x == vertices[2].x)
    {
      va = &vertices[1];
      vb = &vertices[2];
      vc = &vertices[0];
    }
    else if (vertices[2].x == vertices[0].x)
    {
      va = &vertices[2];
      vb = &vertices[0];
      vc = &vertices[1];
    }
    else
    {
      return false;
    }

    // Determine line direction. Vertical lines will have a width of 1, horizontal lines a height of 1.
    vertical = (std::abs(va->x - vc->x) == 1.0f);
    horizontal = (std::abs(va->y - vb->y) == 1.0f);
    if (!vertical && !horizontal)
      return false;

    // Determine which vertex is the right angle, based on the vertical position.
    const BatchVertex* vcorner;
    if (va->y == vc->y)
      vcorner = va;
    else if (vb->y == vc->y)
      vcorner = vb;
    else
      return false;

    // Find short/long edge of the triangle.
    BatchVertex* vother = ((vcorner == va) ? vb : va);
    vshort = horizontal ? vother : vc;
    vlong = vertical ? vother : vc;

    // Dark Forces draws its gun sprite vertically, but rotated compared to the sprite date in VRAM.
    // Therefore the difference in V should be ignored.
    vshort->u = vcorner->u;
    vshort->v = vcorner->v;
  }

  // Need to write the 4th vertex.
  DebugAssert(m_batch_vertex_space >= 1);
  BatchVertex* last = &(vertices[3] = *vlong);
  last->x = vertical ? vshort->x : vlong->x;
  last->y = horizontal ? vshort->y : vlong->y;

  // Generate indices.
  const u32 base_vertex = m_batch_vertex_count;
  DebugAssert(m_batch_index_space >= 6);
  *(m_batch_index_ptr++) = Truncate16(base_vertex);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
  *(m_batch_index_ptr++) = Truncate16(base_vertex + (vshort - vertices));
  *(m_batch_index_ptr++) = Truncate16(base_vertex + (vlong - vertices));
  *(m_batch_index_ptr++) = Truncate16(base_vertex + 3);
  m_batch_index_count += 6;
  m_batch_index_space -= 6;

  // Upload vertices.
  DebugAssert(m_batch_vertex_space >= 4);
  std::memcpy(m_batch_vertex_ptr, vertices, sizeof(BatchVertex) * 4);
  m_batch_vertex_ptr += 4;
  m_batch_vertex_count += 4;
  m_batch_vertex_space -= 4;
  return true;
}

void GPU_HW::ComputePolygonUVLimits(BatchVertex* vertices, u32 num_vertices)
{
  DebugAssert(num_vertices == 3 || num_vertices == 4);

  GSVector2i v0 = GSVector2i::load32(&vertices[0].u);
  GSVector2i v1 = GSVector2i::load32(&vertices[1].u);
  GSVector2i v2 = GSVector2i::load32(&vertices[2].u);
  GSVector2i v3;
  GSVector2i min = v0.min_u16(v1).min_u16(v2);
  GSVector2i max = v0.max_u16(v1).max_u16(v2);
  if (num_vertices == 4)
  {
    v3 = GSVector2i::load32(&vertices[3].u);
    min = min.min_u16(v3);
    max = max.max_u16(v3);
  }

  u32 min_u = min.extract16<0>();
  u32 min_v = min.extract16<1>();
  u32 max_u = max.extract16<0>();
  u32 max_v = max.extract16<1>();
  max_u = (min_u != max_u) ? (max_u - 1) : max_u;
  max_v = (min_v != max_v) ? (max_v - 1) : max_v;

  for (u32 i = 0; i < num_vertices; i++)
    vertices[i].SetUVLimits(min_u, max_u, min_v, max_v);

  if (m_texpage_dirty != 0)
    CheckForTexPageOverlap(GSVector4i(min).upl32(GSVector4i(max)).u16to32());
}

void GPU_HW::SetBatchDepthBuffer(bool enabled)
{
  if (m_batch.use_depth_buffer == enabled)
    return;

  if (m_batch_index_count > 0)
  {
    FlushRender();
    EnsureVertexBufferSpaceForCurrentCommand();
  }

  m_batch.use_depth_buffer = enabled;
}

void GPU_HW::CheckForDepthClear(const BatchVertex* vertices, u32 num_vertices)
{
  DebugAssert(num_vertices == 3 || num_vertices == 4);
  float average_z;
  if (num_vertices == 3)
    average_z = std::min((vertices[0].w + vertices[1].w + vertices[2].w) / 3.0f, 1.0f);
  else
    average_z = std::min((vertices[0].w + vertices[1].w + vertices[2].w + vertices[3].w) / 4.0f, 1.0f);

  if ((average_z - m_last_depth_z) >= g_settings.gpu_pgxp_depth_clear_threshold)
  {
    FlushRender();
    CopyAndClearDepthBuffer();
    EnsureVertexBufferSpaceForCurrentCommand();
  }

  m_last_depth_z = average_z;
}

void GPU_HW::SetBatchSpriteMode(bool enabled)
{
  if (m_batch.sprite_mode == enabled)
    return;

  if (m_batch_index_count > 0)
  {
    FlushRender();
    EnsureVertexBufferSpaceForCurrentCommand();
  }

  GL_INS_FMT("Sprite mode is now {}", enabled ? "ON" : "OFF");

  m_batch.sprite_mode = enabled;
}

void GPU_HW::DrawLine(const GSVector4 bounds, u32 col0, u32 col1, float depth)
{
  DebugAssert(m_batch_vertex_space >= 4 && m_batch_index_space >= 6);

  const float x0 = bounds.x;
  const float y0 = bounds.y;
  const float x1 = bounds.z;
  const float y1 = bounds.w;

  const float dx = x1 - x0;
  const float dy = y1 - y0;
  if (dx == 0.0f && dy == 0.0f)
  {
    // Degenerate, render a point.
    (m_batch_vertex_ptr++)->Set(x0, y0, depth, 1.0f, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x0 + 1.0f, y0, depth, 1.0f, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x1, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(x1 + 1.0f, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
  }
  else
  {
    const float abs_dx = std::fabs(dx);
    const float abs_dy = std::fabs(dy);
    float fill_dx, fill_dy;
    float pad_x0 = 0.0f;
    float pad_x1 = 0.0f;
    float pad_y0 = 0.0f;
    float pad_y1 = 0.0f;

    // Check for vertical or horizontal major lines.
    // When expanding to a rect, do so in the appropriate direction.
    // FIXME: This scheme seems to kinda work, but it seems very hard to find a method
    // that looks perfect on every game.
    // Vagrant Story speech bubbles are a very good test case here!
    if (abs_dx > abs_dy)
    {
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      const float dydk = dy / abs_dx;

      if (dx > 0.0f)
      {
        // Right
        pad_x1 = 1.0f;
        pad_y1 = dydk;
      }
      else
      {
        // Left
        pad_x0 = 1.0f;
        pad_y0 = -dydk;
      }
    }
    else
    {
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      const float dxdk = dx / abs_dy;

      if (dy > 0.0f)
      {
        // Down
        pad_y1 = 1.0f;
        pad_x1 = dxdk;
      }
      else
      {
        // Up
        pad_y0 = 1.0f;
        pad_x0 = -dxdk;
      }
    }

    const float ox0 = x0 + pad_x0;
    const float oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1;
    const float oy1 = y1 + pad_y1;

    (m_batch_vertex_ptr++)->Set(ox0, oy0, depth, 1.0f, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox0 + fill_dx, oy0 + fill_dy, depth, 1.0f, col0, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox1, oy1, depth, 1.0f, col1, 0, 0, 0);
    (m_batch_vertex_ptr++)->Set(ox1 + fill_dx, oy1 + fill_dy, depth, 1.0f, col1, 0, 0, 0);
  }

  const u32 start_index = m_batch_vertex_count;
  m_batch_vertex_count += 4;
  m_batch_vertex_space -= 4;

  *(m_batch_index_ptr++) = Truncate16(start_index + 0);
  *(m_batch_index_ptr++) = Truncate16(start_index + 1);
  *(m_batch_index_ptr++) = Truncate16(start_index + 2);
  *(m_batch_index_ptr++) = Truncate16(start_index + 3);
  *(m_batch_index_ptr++) = Truncate16(start_index + 2);
  *(m_batch_index_ptr++) = Truncate16(start_index + 1);
  m_batch_index_count += 6;
  m_batch_index_space -= 6;
}

void GPU_HW::LoadVertices()
{
  if (m_GPUSTAT.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{m_render_command.bits};
  const u32 texpage = ZeroExtend32(m_draw_mode.mode_reg.bits) | (ZeroExtend32(m_draw_mode.palette_reg.bits) << 16);
  const float depth = GetCurrentNormalizedVertexDepth();

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      const bool textured = rc.texture_enable;
      const bool raw_texture = textured && rc.raw_texture_enable;
      const bool shaded = rc.shading_enable;
      const bool pgxp = g_settings.gpu_pgxp_enable;

      const u32 first_color = rc.color_for_first_vertex;
      u32 num_vertices = rc.quad_polygon ? 4 : 3;
      std::array<BatchVertex, 4> vertices;
      std::array<GSVector2i, 4> native_vertex_positions;
      std::array<u16, 4> native_texcoords;
      bool valid_w = g_settings.gpu_pgxp_texture_correction;
      for (u32 i = 0; i < num_vertices; i++)
      {
        const u32 vert_color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const u32 color = raw_texture ? UINT32_C(0x00808080) : vert_color;
        const u64 maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
        const u16 texcoord = textured ? Truncate16(FifoPop()) : 0;
        const s32 native_x = native_vertex_positions[i].x = m_drawing_offset.x + vp.x;
        const s32 native_y = native_vertex_positions[i].y = m_drawing_offset.y + vp.y;
        native_texcoords[i] = texcoord;
        vertices[i].Set(static_cast<float>(native_x), static_cast<float>(native_y), depth, 1.0f, color, texpage,
                        texcoord, 0xFFFF0000u);

        if (pgxp)
        {
          valid_w &= CPU::PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, native_x, native_y,
                                                 m_drawing_offset.x, m_drawing_offset.y, &vertices[i].x, &vertices[i].y,
                                                 &vertices[i].w);
        }
      }
      if (pgxp)
      {
        if (!valid_w)
        {
          SetBatchDepthBuffer(false);
          if (g_settings.gpu_pgxp_disable_2d)
          {
            // NOTE: This reads uninitialized data, but it's okay, it doesn't get used.
            for (size_t i = 0; i < vertices.size(); i++)
            {
              BatchVertex& v = vertices[i];
              v.x = static_cast<float>(native_vertex_positions[i].x);
              v.y = static_cast<float>(native_vertex_positions[i].y);
              v.w = 1.0f;
            }
          }
          else
          {
            for (BatchVertex& v : vertices)
              v.w = 1.0f;
          }
        }
        else if (m_pgxp_depth_buffer)
        {
          SetBatchDepthBuffer(true);
          CheckForDepthClear(vertices.data(), num_vertices);
        }
      }

      // Use PGXP to exclude primitives that are definitely 3D.
      const bool is_3d = (vertices[0].w != vertices[1].w || vertices[0].w != vertices[2].w);
      if (m_resolution_scale > 1 && !is_3d && rc.quad_polygon)
        HandleFlippedQuadTextureCoordinates(vertices.data());
      else if (m_allow_sprite_mode)
        SetBatchSpriteMode((pgxp && !is_3d) || IsPossibleSpritePolygon(vertices.data()));

      if (m_sw_renderer)
      {
        GPUBackendDrawPolygonCommand* cmd = m_sw_renderer->NewDrawPolygonCommand(num_vertices);
        FillDrawCommand(cmd, rc);

        const u32 sw_num_vertices = rc.quad_polygon ? 4 : 3;
        for (u32 i = 0; i < sw_num_vertices; i++)
        {
          GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
          vert->x = native_vertex_positions[i].x;
          vert->y = native_vertex_positions[i].y;
          vert->texcoord = native_texcoords[i];
          vert->color = vertices[i].color;
        }

        m_sw_renderer->PushCommand(cmd);
      }

      // Cull polygons which are too large.
      const GSVector2 v0f = GSVector2::load(&vertices[0].x);
      const GSVector2 v1f = GSVector2::load(&vertices[1].x);
      const GSVector2 v2f = GSVector2::load(&vertices[2].x);
      const GSVector2 min_pos_12 = v1f.min(v2f);
      const GSVector2 max_pos_12 = v1f.max(v2f);
      const GSVector4i draw_rect_012 = GSVector4i(GSVector4(min_pos_12.min(v0f)).upld(GSVector4(max_pos_12.max(v0f))))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));
      const GSVector4i clamped_draw_rect_012 = draw_rect_012.rintersect(m_clamped_drawing_area);
      const bool first_tri_culled = (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH ||
                                     draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT || clamped_draw_rect_012.rempty());
      if (first_tri_culled)
      {
        GL_INS_FMT("Culling off-screen/too-large polygon: {},{} {},{} {},{}", native_vertex_positions[0].x,
                   native_vertex_positions[0].y, native_vertex_positions[1].x, native_vertex_positions[1].y,
                   native_vertex_positions[2].x, native_vertex_positions[2].y);

        if (!rc.quad_polygon)
          return;
      }
      else
      {
        if (textured && m_compute_uv_range)
          ComputePolygonUVLimits(vertices.data(), num_vertices);

        AddDrawnRectangle(clamped_draw_rect_012);
        AddDrawTriangleTicks(native_vertex_positions[0], native_vertex_positions[1], native_vertex_positions[2],
                             rc.shading_enable, rc.texture_enable, rc.transparency_enable);

        // Expand lines to triangles (Doom, Soul Blade, etc.)
        if (!rc.quad_polygon && m_line_detect_mode >= GPULineDetectMode::BasicTriangles && !is_3d &&
            ExpandLineTriangles(vertices.data()))
        {
          return;
        }

        const u32 start_index = m_batch_vertex_count;
        DebugAssert(m_batch_index_space >= 3);
        *(m_batch_index_ptr++) = Truncate16(start_index);
        *(m_batch_index_ptr++) = Truncate16(start_index + 1);
        *(m_batch_index_ptr++) = Truncate16(start_index + 2);
        m_batch_index_count += 3;
        m_batch_index_space -= 3;
      }

      // quads
      if (rc.quad_polygon)
      {
        const GSVector2 v3f = GSVector2::load(&vertices[3].x);
        const GSVector4i draw_rect_123 = GSVector4i(GSVector4(min_pos_12.min(v3f)).upld(GSVector4(max_pos_12.max(v3f))))
                                           .add32(GSVector4i::cxpr(0, 0, 1, 1));
        const GSVector4i clamped_draw_rect_123 = draw_rect_123.rintersect(m_clamped_drawing_area);

        // Cull polygons which are too large.
        const bool second_tri_culled =
          (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT ||
           clamped_draw_rect_123.rempty());
        if (second_tri_culled)
        {
          GL_INS_FMT("Culling off-screen/too-large polygon (quad second half): {},{} {},{} {},{}",
                     native_vertex_positions[2].x, native_vertex_positions[2].y, native_vertex_positions[1].x,
                     native_vertex_positions[1].y, native_vertex_positions[0].x, native_vertex_positions[0].y);

          if (first_tri_culled)
            return;
        }
        else
        {
          if (first_tri_culled && textured && m_compute_uv_range)
            ComputePolygonUVLimits(vertices.data(), num_vertices);

          AddDrawnRectangle(clamped_draw_rect_123);
          AddDrawTriangleTicks(native_vertex_positions[2], native_vertex_positions[1], native_vertex_positions[3],
                               rc.shading_enable, rc.texture_enable, rc.transparency_enable);

          const u32 start_index = m_batch_vertex_count;
          DebugAssert(m_batch_index_space >= 3);
          *(m_batch_index_ptr++) = Truncate16(start_index + 2);
          *(m_batch_index_ptr++) = Truncate16(start_index + 1);
          *(m_batch_index_ptr++) = Truncate16(start_index + 3);
          m_batch_index_count += 3;
          m_batch_index_space -= 3;
        }
      }

      if (num_vertices == 4)
      {
        DebugAssert(m_batch_vertex_space >= 4);
        std::memcpy(m_batch_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 4);
        m_batch_vertex_ptr += 4;
        m_batch_vertex_count += 4;
        m_batch_vertex_space -= 4;
      }
      else
      {
        DebugAssert(m_batch_vertex_space >= 3);
        std::memcpy(m_batch_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
        m_batch_vertex_ptr += 3;
        m_batch_vertex_count += 3;
        m_batch_vertex_space -= 3;
      }
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      const u32 color = (rc.texture_enable && rc.raw_texture_enable) ? UINT32_C(0x00808080) : rc.color_for_first_vertex;
      const GPUVertexPosition vp{FifoPop()};
      const s32 pos_x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      const s32 pos_y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(rc.texture_enable ? Truncate16(FifoPop()) : 0);
      u32 orig_tex_left = ZeroExtend16(texcoord_x);
      u32 orig_tex_top = ZeroExtend16(texcoord_y);
      u32 rectangle_width;
      u32 rectangle_height;
      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          rectangle_width = 1;
          rectangle_height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          rectangle_width = 8;
          rectangle_height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          rectangle_width = 16;
          rectangle_height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          rectangle_width = (width_and_height & VRAM_WIDTH_MASK);
          rectangle_height = ((width_and_height >> 16) & VRAM_HEIGHT_MASK);
        }
        break;
      }

      const GSVector4i rect =
        GSVector4i(pos_x, pos_y, pos_x + static_cast<s32>(rectangle_width), pos_y + static_cast<s32>(rectangle_height));
      const GSVector4i clamped_rect = m_clamped_drawing_area.rintersect(rect);
      if (clamped_rect.rempty()) [[unlikely]]
      {
        GL_INS_FMT("Culling off-screen rectangle {}", rect);
        return;
      }

      // we can split the rectangle up into potentially 8 quads
      SetBatchDepthBuffer(false);
      SetBatchSpriteMode(m_allow_sprite_mode);
      DebugAssert(m_batch_vertex_space >= MAX_VERTICES_FOR_RECTANGLE &&
                  m_batch_index_space >= MAX_VERTICES_FOR_RECTANGLE);

      // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
      u32 tex_top = orig_tex_top;
      for (u32 y_offset = 0; y_offset < rectangle_height;)
      {
        const s32 quad_height = std::min(rectangle_height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
        const float quad_start_y = static_cast<float>(pos_y + static_cast<s32>(y_offset));
        const float quad_end_y = quad_start_y + static_cast<float>(quad_height);
        const u32 tex_bottom = tex_top + quad_height;

        u32 tex_left = orig_tex_left;
        for (u32 x_offset = 0; x_offset < rectangle_width;)
        {
          const s32 quad_width = std::min(rectangle_width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
          const float quad_start_x = static_cast<float>(pos_x + static_cast<s32>(x_offset));
          const float quad_end_x = quad_start_x + static_cast<float>(quad_width);
          const u32 tex_right = tex_left + quad_width;
          const u32 uv_limits = BatchVertex::PackUVLimits(tex_left, tex_right - 1, tex_top, tex_bottom - 1);

          if (rc.texture_enable && m_texpage_dirty != 0)
          {
            CheckForTexPageOverlap(GSVector4i(static_cast<s32>(tex_left), static_cast<s32>(tex_top),
                                              static_cast<s32>(tex_right), static_cast<s32>(tex_bottom)));
          }

          const u32 base_vertex = m_batch_vertex_count;
          (m_batch_vertex_ptr++)
            ->Set(quad_start_x, quad_start_y, depth, 1.0f, color, texpage, Truncate16(tex_left), Truncate16(tex_top),
                  uv_limits);
          (m_batch_vertex_ptr++)
            ->Set(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, Truncate16(tex_right), Truncate16(tex_top),
                  uv_limits);
          (m_batch_vertex_ptr++)
            ->Set(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, Truncate16(tex_left), Truncate16(tex_bottom),
                  uv_limits);
          (m_batch_vertex_ptr++)
            ->Set(quad_end_x, quad_end_y, depth, 1.0f, color, texpage, Truncate16(tex_right), Truncate16(tex_bottom),
                  uv_limits);
          m_batch_vertex_count += 4;
          m_batch_vertex_space -= 4;

          *(m_batch_index_ptr++) = Truncate16(base_vertex + 0);
          *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
          *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
          *(m_batch_index_ptr++) = Truncate16(base_vertex + 2);
          *(m_batch_index_ptr++) = Truncate16(base_vertex + 1);
          *(m_batch_index_ptr++) = Truncate16(base_vertex + 3);
          m_batch_index_count += 6;
          m_batch_index_space -= 6;

          x_offset += quad_width;
          tex_left = 0;
        }

        y_offset += quad_height;
        tex_top = 0;
      }

      AddDrawnRectangle(clamped_rect);
      AddDrawRectangleTicks(clamped_rect, rc.texture_enable, rc.transparency_enable);

      if (m_sw_renderer)
      {
        GPUBackendDrawRectangleCommand* cmd = m_sw_renderer->NewDrawRectangleCommand();
        FillDrawCommand(cmd, rc);
        cmd->color = color;
        cmd->x = pos_x;
        cmd->y = pos_y;
        cmd->width = static_cast<u16>(rectangle_width);
        cmd->height = static_cast<u16>(rectangle_height);
        cmd->texcoord = (static_cast<u16>(texcoord_y) << 8) | static_cast<u16>(texcoord_x);
        m_sw_renderer->PushCommand(cmd);
      }
    }
    break;

    case GPUPrimitive::Line:
    {
      SetBatchDepthBuffer(false);

      if (!rc.polyline)
      {
        DebugAssert(m_batch_vertex_space >= 4 && m_batch_index_space >= 6);

        u32 start_color, end_color;
        GPUVertexPosition start_pos, end_pos;
        if (rc.shading_enable)
        {
          start_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_color = FifoPop() & UINT32_C(0x00FFFFFF);
          end_pos.bits = FifoPop();
        }
        else
        {
          start_color = end_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_pos.bits = FifoPop();
        }

        const GSVector4i vstart_pos = GSVector4i(start_pos.x + m_drawing_offset.x, start_pos.y + m_drawing_offset.y);
        const GSVector4i vend_pos = GSVector4i(end_pos.x + m_drawing_offset.x, end_pos.y + m_drawing_offset.y);
        const GSVector4i bounds = vstart_pos.xyxy(vend_pos);
        const GSVector4i rect =
          vstart_pos.min_i32(vend_pos).xyxy(vstart_pos.max_i32(vend_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
        const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);

        if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
        {
          GL_INS_FMT("Culling too-large/off-screen line: {},{} - {},{}", bounds.x, bounds.y, bounds.z, bounds.w);
          return;
        }

        AddDrawnRectangle(clamped_rect);
        AddDrawLineTicks(clamped_rect, rc.shading_enable);

        // TODO: Should we do a PGXP lookup here? Most lines are 2D.
        DrawLine(GSVector4(bounds), start_color, end_color, depth);

        if (m_sw_renderer)
        {
          GPUBackendDrawLineCommand* cmd = m_sw_renderer->NewDrawLineCommand(2);
          FillDrawCommand(cmd, rc);
          GSVector4i::storel(&cmd->vertices[0], bounds);
          cmd->vertices[0].color = start_color;
          GSVector4i::storeh(&cmd->vertices[1], bounds);
          cmd->vertices[1].color = end_color;
          m_sw_renderer->PushCommand(cmd);
        }
      }
      else
      {
        // Multiply by two because we don't use line strips.
        const u32 num_vertices = GetPolyLineVertexCount();
        DebugAssert(m_batch_vertex_space >= (num_vertices * 4) && m_batch_index_space >= (num_vertices * 6));

        const bool shaded = rc.shading_enable;

        u32 buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        GSVector4i start_pos = GSVector4i(start_vp.x + m_drawing_offset.x, start_vp.y + m_drawing_offset.y);
        u32 start_color = rc.color_for_first_vertex;

        GPUBackendDrawLineCommand* cmd;
        if (m_sw_renderer)
        {
          cmd = m_sw_renderer->NewDrawLineCommand(num_vertices);
          FillDrawCommand(cmd, rc);
          GSVector4i::storel(&cmd->vertices[0].x, start_pos);
          cmd->vertices[0].color = start_color;
        }
        else
        {
          cmd = nullptr;
        }

        for (u32 i = 1; i < num_vertices; i++)
        {
          const u32 end_color = shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : start_color;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          const GSVector4i end_pos = GSVector4i(m_drawing_offset.x + vp.x, m_drawing_offset.y + vp.y);
          const GSVector4i bounds = start_pos.xyxy(end_pos);
          const GSVector4i rect =
            start_pos.min_i32(end_pos).xyxy(start_pos.max_i32(end_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
          const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);
          if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
          {
            GL_INS_FMT("Culling too-large line: {},{} - {},{}", start_pos.x, start_pos.y, end_pos.x, end_pos.y);
          }
          else
          {
            AddDrawnRectangle(clamped_rect);
            AddDrawLineTicks(clamped_rect, rc.shading_enable);

            // TODO: Should we do a PGXP lookup here? Most lines are 2D.
            DrawLine(GSVector4(bounds), start_color, end_color, depth);
          }

          start_pos = end_pos;
          start_color = end_color;

          if (cmd)
          {
            GSVector4i::storel(&cmd->vertices[i], end_pos);
            cmd->vertices[i].color = end_color;
          }
        }

        if (cmd)
          m_sw_renderer->PushCommand(cmd);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

bool GPU_HW::BlitVRAMReplacementTexture(const TextureReplacements::ReplacementImage* tex, u32 dst_x, u32 dst_y,
                                        u32 width, u32 height)
{
  if (!m_vram_replacement_texture || m_vram_replacement_texture->GetWidth() < tex->GetWidth() ||
      m_vram_replacement_texture->GetHeight() < tex->GetHeight() || g_gpu_device->GetFeatures().prefer_unused_textures)
  {
    g_gpu_device->RecycleTexture(std::move(m_vram_replacement_texture));

    if (!(m_vram_replacement_texture =
            g_gpu_device->FetchTexture(tex->GetWidth(), tex->GetHeight(), 1, 1, 1, GPUTexture::Type::DynamicTexture,
                                       GPUTexture::Format::RGBA8, tex->GetPixels(), tex->GetPitch())))
    {
      return false;
    }
  }
  else
  {
    if (!m_vram_replacement_texture->Update(0, 0, tex->GetWidth(), tex->GetHeight(), tex->GetPixels(), tex->GetPitch()))
    {
      ERROR_LOG("Update {}x{} texture failed.", width, height);
      return false;
    }
  }

  GL_SCOPE_FMT("BlitVRAMReplacementTexture() {}x{} to {},{} => {},{} ({}x{})", tex->GetWidth(), tex->GetHeight(), dst_x,
               dst_y, dst_x + width, dst_y + height, width, height);

  const float src_rect[4] = {
    0.0f, 0.0f, static_cast<float>(tex->GetWidth()) / static_cast<float>(m_vram_replacement_texture->GetWidth()),
    static_cast<float>(tex->GetHeight()) / static_cast<float>(m_vram_replacement_texture->GetHeight())};

  g_gpu_device->SetTextureSampler(0, m_vram_replacement_texture.get(), g_gpu_device->GetLinearSampler());
  g_gpu_device->SetPipeline(m_vram_write_replacement_pipeline.get());
  g_gpu_device->SetViewportAndScissor(dst_x, dst_y, width, height);
  g_gpu_device->PushUniformBuffer(src_rect, sizeof(src_rect));
  g_gpu_device->Draw(3, 0);

  RestoreDeviceContext();
  return true;
}

ALWAYS_INLINE_RELEASE void GPU_HW::CheckForTexPageOverlap(GSVector4i uv_rect)
{
  DebugAssert(m_texpage_dirty != 0 && m_batch.texture_mode != BatchTextureMode::Disabled);

  if (m_texture_window_active)
  {
    const GSVector4i twin = GSVector4i::load<false>(m_batch_ubo_data.u_texture_window);
    uv_rect = ((uv_rect & twin.xyxy()) | twin.zwzw());

    // Min could be greater than max after applying window, correct for it.
    uv_rect = uv_rect.min_i32(uv_rect.zwzw()).max_i32(uv_rect.xyxy());
  }

  const GPUTextureMode tmode = m_draw_mode.mode_reg.texture_mode;
  const u32 xshift = (tmode >= GPUTextureMode::Direct16Bit) ? 0 : (2 - static_cast<u8>(tmode));
  const GSVector4i page_offset = GSVector4i::loadl(m_current_texture_page_offset).xyxy();

  uv_rect = uv_rect.blend32<5>(uv_rect.srl32(xshift));   // shift only goes on the x
  uv_rect = uv_rect.add32(page_offset);                  // page offset
  uv_rect = uv_rect.add32(GSVector4i::cxpr(0, 0, 1, 1)); // make exclusive
  uv_rect = uv_rect.rintersect(VRAM_SIZE_RECT);          // clamp to vram bounds

  const GSVector4i new_uv_rect = m_current_uv_rect.runion(uv_rect);

  if (!m_current_uv_rect.eq(new_uv_rect))
  {
    m_current_uv_rect = new_uv_rect;

    bool update_drawn = false, update_written = false;
    if (m_texpage_dirty & TEXPAGE_DIRTY_DRAWN_RECT)
    {
      DebugAssert(!m_vram_dirty_draw_rect.eq(INVALID_RECT));
      update_drawn = m_current_uv_rect.rintersects(m_vram_dirty_draw_rect);
      if (update_drawn)
      {
        GL_INS_FMT("Updating VRAM cache due to UV {} intersection with dirty DRAW {}", m_current_uv_rect,
                   m_vram_dirty_draw_rect);
      }
    }
    if (m_texpage_dirty & TEXPAGE_DIRTY_WRITTEN_RECT)
    {
      DebugAssert(!m_vram_dirty_write_rect.eq(INVALID_RECT));
      update_written = m_current_uv_rect.rintersects(m_vram_dirty_write_rect);
      if (update_written)
      {
        GL_INS_FMT("Updating VRAM cache due to UV {} intersection with dirty WRITE {}", m_current_uv_rect,
                   m_vram_dirty_write_rect);
      }
    }

    if (update_drawn || update_written)
    {
      if (m_batch_index_count > 0)
      {
        FlushRender();
        EnsureVertexBufferSpaceForCurrentCommand();
      }

      UpdateVRAMReadTexture(update_drawn, update_written);
    }
  }
}

ALWAYS_INLINE bool GPU_HW::IsFlushed() const
{
  return (m_batch_index_count == 0);
}

ALWAYS_INLINE_RELEASE bool GPU_HW::NeedsTwoPassRendering() const
{
  // We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
  // on a per-pixel basis, and the opaque pixels shouldn't be blended at all.

  return (m_batch.texture_mode != BatchTextureMode::Disabled &&
          (m_batch.transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ||
           (!m_supports_dual_source_blend && m_batch.transparency_mode != GPUTransparencyMode::Disabled)));
}

ALWAYS_INLINE_RELEASE bool GPU_HW::NeedsShaderBlending(GPUTransparencyMode transparency, BatchTextureMode texture_mode,
                                                       bool check_mask) const
{
  return (m_allow_shader_blend &&
          ((check_mask && !m_write_mask_as_depth) ||
           (transparency != GPUTransparencyMode::Disabled && m_prefer_shader_blend) ||
           (transparency == GPUTransparencyMode::BackgroundMinusForeground) ||
           (!m_supports_dual_source_blend && texture_mode != BatchTextureMode::Disabled &&
            (transparency != GPUTransparencyMode::Disabled || IsBlendedTextureFiltering(m_texture_filtering) ||
             IsBlendedTextureFiltering(m_sprite_texture_filtering)))));
}

void GPU_HW::EnsureVertexBufferSpace(u32 required_vertices, u32 required_indices)
{
  if (m_batch_vertex_ptr)
  {
    if (m_batch_vertex_space >= required_vertices && m_batch_index_space >= required_indices)
      return;

    FlushRender();
  }

  MapGPUBuffer(required_vertices, required_indices);
}

void GPU_HW::EnsureVertexBufferSpaceForCurrentCommand()
{
  u32 required_vertices;
  u32 required_indices;
  switch (m_render_command.primitive)
  {
    case GPUPrimitive::Polygon:
      required_vertices = 4; // assume quad, in case of expansion
      required_indices = 6;
      break;
    case GPUPrimitive::Rectangle:
      required_vertices = MAX_VERTICES_FOR_RECTANGLE; // TODO: WRong
      required_indices = MAX_VERTICES_FOR_RECTANGLE;
      break;
    case GPUPrimitive::Line:
    {
      // assume expansion
      const u32 vert_count = m_render_command.polyline ? GetPolyLineVertexCount() : 2;
      required_vertices = vert_count * 4;
      required_indices = vert_count * 6;
    }
    break;

    default:
      UnreachableCode();
  }

  // can we fit these vertices in the current depth buffer range?
  if ((m_current_depth + required_vertices) > MAX_BATCH_VERTEX_COUNTER_IDS)
  {
    FlushRender();
    ResetBatchVertexDepth();
    MapGPUBuffer(required_vertices, required_indices);
    return;
  }

  EnsureVertexBufferSpace(required_vertices, required_indices);
}

void GPU_HW::ResetBatchVertexDepth()
{
  DEV_LOG("Resetting batch vertex depth");

  if (m_write_mask_as_depth)
    UpdateDepthBufferFromMaskBit();

  m_current_depth = 1;
}

ALWAYS_INLINE float GPU_HW::GetCurrentNormalizedVertexDepth() const
{
  return 1.0f - (static_cast<float>(m_current_depth) / 65535.0f);
}

void GPU_HW::UpdateSoftwareRenderer(bool copy_vram_from_hw)
{
  const bool current_enabled = (m_sw_renderer != nullptr);
  const bool new_enabled = g_settings.gpu_use_software_renderer_for_readbacks;
  if (current_enabled == new_enabled)
    return;

  if (!new_enabled)
  {
    if (m_sw_renderer)
      m_sw_renderer->Shutdown();
    m_sw_renderer.reset();
    return;
  }

  std::unique_ptr<GPU_SW_Backend> sw_renderer = std::make_unique<GPU_SW_Backend>();
  if (!sw_renderer->Initialize(true))
    return;

  // We need to fill in the SW renderer's VRAM with the current state for hot toggles.
  if (copy_vram_from_hw)
  {
    FlushRender();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

    // Sync the drawing area and CLUT.
    GPUBackendSetDrawingAreaCommand* clip_cmd = sw_renderer->NewSetDrawingAreaCommand();
    clip_cmd->new_area = m_drawing_area;
    sw_renderer->PushCommand(clip_cmd);

    if (IsCLUTValid())
    {
      GPUBackendUpdateCLUTCommand* clut_cmd = sw_renderer->NewUpdateCLUTCommand();
      FillBackendCommandParameters(clut_cmd);
      clut_cmd->reg.bits = static_cast<u16>(m_current_clut_reg_bits);
      clut_cmd->clut_is_8bit = m_current_clut_is_8bit;
      sw_renderer->PushCommand(clut_cmd);
    }
  }

  m_sw_renderer = std::move(sw_renderer);
}

void GPU_HW::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = m_GPUSTAT.SkipDrawingToActiveField();
}

void GPU_HW::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->palette.bits = m_draw_mode.palette_reg.bits;
  cmd->window = m_draw_mode.texture_window;
}

void GPU_HW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  GL_SCOPE_FMT("FillVRAM({},{} => {},{} ({}x{}) with 0x{:08X}", x, y, x + width, y + height, width, height, color);
  DeactivateROV();

  if (m_sw_renderer)
  {
    GPUBackendFillVRAMCommand* cmd = m_sw_renderer->NewFillVRAMCommand();
    FillBackendCommandParameters(cmd);
    cmd->x = static_cast<u16>(x);
    cmd->y = static_cast<u16>(y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    cmd->color = color;
    m_sw_renderer->PushCommand(cmd);
  }

  GL_INS_FMT("Dirty draw area before: {}", m_vram_dirty_draw_rect);

  const GSVector4i bounds = GetVRAMTransferBounds(x, y, width, height);
  AddUnclampedDrawnRectangle(bounds);

  GL_INS_FMT("Dirty draw area after: {}", m_vram_dirty_draw_rect);

  const bool is_oversized = (((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT));
  g_gpu_device->SetPipeline(
    m_vram_fill_pipelines[BoolToUInt8(is_oversized)][BoolToUInt8(IsInterlacedRenderingEnabled())].get());

  const GSVector4i scaled_bounds = bounds.mul32l(GSVector4i(m_resolution_scale));
  g_gpu_device->SetViewportAndScissor(scaled_bounds);

  struct VRAMFillUBOData
  {
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    std::array<float, 4> u_fill_color;
    u32 u_interlaced_displayed_field;
  };
  VRAMFillUBOData uniforms;
  uniforms.u_dst_x = (x % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_dst_y = (y % VRAM_HEIGHT) * m_resolution_scale;
  uniforms.u_end_x = ((x + width) % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_end_y = ((y + height) % VRAM_HEIGHT) * m_resolution_scale;
  // drop precision unless true colour is enabled
  uniforms.u_fill_color =
    GPUDevice::RGBA8ToFloat(m_true_color ? color : VRAMRGBA5551ToRGBA8888(VRAMRGBA8888ToRGBA5551(color)));
  uniforms.u_interlaced_displayed_field = GetActiveLineLSB();
  g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
  g_gpu_device->Draw(3, 0);

  RestoreDeviceContext();
}

void GPU_HW::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  GL_PUSH_FMT("ReadVRAM({},{} => {},{} ({}x{})", x, y, x + width, y + height, width, height);

  if (m_sw_renderer)
  {
    m_sw_renderer->Sync(false);
    GL_POP();
    return;
  }

  // Get bounds with wrap-around handled.
  GSVector4i copy_rect = GetVRAMTransferBounds(x, y, width, height);

  // Has to be aligned to an even pixel for the download, due to 32-bit packing.
  if (copy_rect.left & 1)
    copy_rect.left--;
  if (copy_rect.right & 1)
    copy_rect.right++;

  DebugAssert((copy_rect.left % 2) == 0 && (copy_rect.width() % 2) == 0);
  const u32 encoded_left = copy_rect.left / 2;
  const u32 encoded_top = copy_rect.top;
  const u32 encoded_width = copy_rect.width() / 2;
  const u32 encoded_height = copy_rect.height();

  // Encode the 24-bit texture as 16-bit.
  const s32 uniforms[4] = {copy_rect.left, copy_rect.top, copy_rect.width(), copy_rect.height()};
  g_gpu_device->SetRenderTarget(m_vram_readback_texture.get());
  g_gpu_device->SetPipeline(m_vram_readback_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
  g_gpu_device->SetViewportAndScissor(0, 0, encoded_width, encoded_height);
  g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
  g_gpu_device->Draw(3, 0);
  m_vram_readback_texture->MakeReadyForSampling();
  GL_POP();

  // Stage the readback and copy it into our shadow buffer.
  if (m_vram_readback_download_texture->IsImported())
  {
    // Fast path, read directly.
    m_vram_readback_download_texture->CopyFromTexture(encoded_left, encoded_top, m_vram_readback_texture.get(), 0, 0,
                                                      encoded_width, encoded_height, 0, 0, false);
    m_vram_readback_download_texture->Flush();
  }
  else
  {
    // Copy to staging buffer, then to VRAM.
    m_vram_readback_download_texture->CopyFromTexture(0, 0, m_vram_readback_texture.get(), 0, 0, encoded_width,
                                                      encoded_height, 0, 0, true);
    m_vram_readback_download_texture->ReadTexels(0, 0, encoded_width, encoded_height,
                                                 &g_vram[copy_rect.top * VRAM_WIDTH + copy_rect.left],
                                                 VRAM_WIDTH * sizeof(u16));
  }

  RestoreDeviceContext();
}

void GPU_HW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  GL_SCOPE_FMT("UpdateVRAM({},{} => {},{} ({}x{})", x, y, x + width, y + height, width, height);

  if (m_sw_renderer)
  {
    const u32 num_words = width * height;
    GPUBackendUpdateVRAMCommand* cmd = m_sw_renderer->NewUpdateVRAMCommand(num_words);
    FillBackendCommandParameters(cmd);
    cmd->params.set_mask_while_drawing = set_mask;
    cmd->params.check_mask_before_draw = check_mask;
    cmd->x = static_cast<u16>(x);
    cmd->y = static_cast<u16>(y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    std::memcpy(cmd->data, data, sizeof(u16) * num_words);
    m_sw_renderer->PushCommand(cmd);
  }

  const GSVector4i bounds = GetVRAMTransferBounds(x, y, width, height);
  DebugAssert(bounds.right <= static_cast<s32>(VRAM_WIDTH) && bounds.bottom <= static_cast<s32>(VRAM_HEIGHT));
  AddWrittenRectangle(bounds);

  if (check_mask)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
  else
  {
    const TextureReplacements::ReplacementImage* rtex = TextureReplacements::GetVRAMReplacement(width, height, data);
    if (rtex && BlitVRAMReplacementTexture(rtex, x * m_resolution_scale, y * m_resolution_scale,
                                           width * m_resolution_scale, height * m_resolution_scale))
    {
      return;
    }
  }

  UpdateVRAMOnGPU(x, y, width, height, data, sizeof(u16) * width, set_mask, check_mask, bounds);
}

void GPU_HW::UpdateVRAMOnGPU(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_pitch, bool set_mask,
                             bool check_mask, const GSVector4i bounds)
{
  DeactivateROV();

  std::unique_ptr<GPUTexture> upload_texture;
  u32 map_index;

  if (!g_gpu_device->GetFeatures().supports_texture_buffers)
  {
    map_index = 0;
    upload_texture = g_gpu_device->FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture,
                                                GPUTexture::Format::R16U, data, data_pitch);
    if (!upload_texture)
    {
      ERROR_LOG("Failed to get {}x{} upload texture. Things are gonna break.", width, height);
      return;
    }
  }
  else
  {
    const u32 num_pixels = width * height;
    const u32 dst_pitch = width * sizeof(u16);
    void* map = m_vram_upload_buffer->Map(num_pixels);
    map_index = m_vram_upload_buffer->GetCurrentPosition();
    StringUtil::StrideMemCpy(map, dst_pitch, data, data_pitch, dst_pitch, height);
    m_vram_upload_buffer->Unmap(num_pixels);
  }

  struct VRAMWriteUBOData
  {
    u32 u_dst_x;
    u32 u_dst_y;
    u32 u_end_x;
    u32 u_end_y;
    u32 u_width;
    u32 u_height;
    u32 u_buffer_base_offset;
    u32 u_mask_or_bits;
    float u_depth_value;
  };
  const VRAMWriteUBOData uniforms = {
    (x % VRAM_WIDTH), (y % VRAM_HEIGHT), ((x + width) % VRAM_WIDTH),  ((y + height) % VRAM_HEIGHT),     width,
    height,           map_index,         (set_mask) ? 0x8000u : 0x00, GetCurrentNormalizedVertexDepth()};

  // the viewport should already be set to the full vram, so just adjust the scissor
  const GSVector4i scaled_bounds = bounds.mul32l(GSVector4i(m_resolution_scale));
  g_gpu_device->SetScissor(scaled_bounds.left, scaled_bounds.top, scaled_bounds.width(), scaled_bounds.height());
  g_gpu_device->SetPipeline(m_vram_write_pipelines[BoolToUInt8(check_mask && m_write_mask_as_depth)].get());
  g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
  if (upload_texture)
  {
    g_gpu_device->SetTextureSampler(0, upload_texture.get(), g_gpu_device->GetNearestSampler());
    g_gpu_device->Draw(3, 0);
    g_gpu_device->RecycleTexture(std::move(upload_texture));
  }
  else
  {
    g_gpu_device->SetTextureBuffer(0, m_vram_upload_buffer.get());
    g_gpu_device->Draw(3, 0);
  }

  RestoreDeviceContext();
}

void GPU_HW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  GL_SCOPE_FMT("CopyVRAM({}x{} @ {},{} => {},{}", width, height, src_x, src_y, dst_x, dst_y);

  if (m_sw_renderer)
  {
    GPUBackendCopyVRAMCommand* cmd = m_sw_renderer->NewCopyVRAMCommand();
    FillBackendCommandParameters(cmd);
    cmd->src_x = static_cast<u16>(src_x);
    cmd->src_y = static_cast<u16>(src_y);
    cmd->dst_x = static_cast<u16>(dst_x);
    cmd->dst_y = static_cast<u16>(dst_y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    m_sw_renderer->PushCommand(cmd);
  }

  // masking enabled, oversized, or overlapping
  const bool use_shader =
    (m_GPUSTAT.IsMaskingEnabled() || ((src_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
     ((src_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT || ((dst_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
     ((dst_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT);
  const GSVector4i src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
  const GSVector4i dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
  const bool intersect_with_draw = m_vram_dirty_draw_rect.rintersects(src_bounds);
  const bool intersect_with_write = m_vram_dirty_write_rect.rintersects(src_bounds);

  if (use_shader || IsUsingMultisampling())
  {
    if (intersect_with_draw || intersect_with_write)
      UpdateVRAMReadTexture(intersect_with_draw, intersect_with_write);
    AddUnclampedDrawnRectangle(dst_bounds);

    DeactivateROV();

    struct VRAMCopyUBOData
    {
      u32 u_src_x;
      u32 u_src_y;
      u32 u_dst_x;
      u32 u_dst_y;
      u32 u_end_x;
      u32 u_end_y;
      u32 u_width;
      u32 u_height;
      u32 u_set_mask_bit;
      float u_depth_value;
    };
    const VRAMCopyUBOData uniforms = {(src_x % VRAM_WIDTH) * m_resolution_scale,
                                      (src_y % VRAM_HEIGHT) * m_resolution_scale,
                                      (dst_x % VRAM_WIDTH) * m_resolution_scale,
                                      (dst_y % VRAM_HEIGHT) * m_resolution_scale,
                                      ((dst_x + width) % VRAM_WIDTH) * m_resolution_scale,
                                      ((dst_y + height) % VRAM_HEIGHT) * m_resolution_scale,
                                      width * m_resolution_scale,
                                      height * m_resolution_scale,
                                      m_GPUSTAT.set_mask_while_drawing ? 1u : 0u,
                                      GetCurrentNormalizedVertexDepth()};

    // VRAM read texture should already be bound.
    const GSVector4i dst_bounds_scaled = dst_bounds.mul32l(GSVector4i(m_resolution_scale));
    g_gpu_device->SetViewportAndScissor(dst_bounds_scaled);
    g_gpu_device->SetPipeline(
      m_vram_copy_pipelines[BoolToUInt8(m_GPUSTAT.check_mask_before_draw && m_write_mask_as_depth)].get());
    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
    g_gpu_device->Draw(3, 0);
    RestoreDeviceContext();

    if (m_GPUSTAT.check_mask_before_draw && !m_pgxp_depth_buffer)
      m_current_depth++;

    return;
  }

  GPUTexture* src_tex = m_vram_texture.get();
  const bool overlaps_with_self = src_bounds.rintersects(dst_bounds);
  if (!g_gpu_device->GetFeatures().texture_copy_to_self || overlaps_with_self)
  {
    src_tex = m_vram_read_texture.get();
    if (intersect_with_draw || intersect_with_write)
      UpdateVRAMReadTexture(intersect_with_draw, intersect_with_write);
  }

  if (intersect_with_draw)
  {
    AddUnclampedDrawnRectangle(dst_bounds);
  }
  else if (intersect_with_write)
  {
    AddWrittenRectangle(dst_bounds);
  }
  else
  {
    const bool use_write =
      (!m_vram_dirty_write_rect.eq(INVALID_RECT) && !m_vram_dirty_draw_rect.eq(INVALID_RECT) &&
       RectDistance(m_vram_dirty_write_rect, dst_bounds) < RectDistance(m_vram_dirty_draw_rect, dst_bounds));
    if (use_write)
      AddWrittenRectangle(dst_bounds);
    else
      AddUnclampedDrawnRectangle(dst_bounds);
  }

  if (m_GPUSTAT.check_mask_before_draw)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }

  g_gpu_device->CopyTextureRegion(m_vram_texture.get(), dst_x * m_resolution_scale, dst_y * m_resolution_scale, 0, 0,
                                  src_tex, src_x * m_resolution_scale, src_y * m_resolution_scale, 0, 0,
                                  width * m_resolution_scale, height * m_resolution_scale);
  if (src_tex != m_vram_texture.get())
    m_vram_read_texture->MakeReadyForSampling();
}

void GPU_HW::DispatchRenderCommand()
{
  const GPURenderCommand rc{m_render_command.bits};

  BatchTextureMode texture_mode = BatchTextureMode::Disabled;
  if (rc.IsTexturingEnabled())
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_draw_mode.IsTexturePageChanged())
    {
      m_draw_mode.ClearTexturePageChangedFlag();

#if 0
      if (!m_vram_dirty_draw_rect.eq(INVALID_RECT) || !m_vram_dirty_write_rect.eq(INVALID_RECT))
      {
        GL_INS_FMT("VRAM DIRTY: {} {}", m_vram_dirty_draw_rect, m_vram_dirty_write_rect);
        GL_INS_FMT("PAGE RECT: {}", m_draw_mode.mode_reg.GetTexturePageRectangle());
        if (m_draw_mode.mode_reg.IsUsingPalette())
          GL_INS_FMT("PALETTE RECT: {}", m_draw_mode.palette_reg.GetRectangle(m_draw_mode.mode_reg.texture_mode));
      }
#endif

      if (m_draw_mode.mode_reg.IsUsingPalette())
      {
        const GSVector4i palette_rect = m_draw_mode.palette_reg.GetRectangle(m_draw_mode.mode_reg.texture_mode);
        const bool update_drawn = palette_rect.rintersects(m_vram_dirty_draw_rect);
        const bool update_written = palette_rect.rintersects(m_vram_dirty_write_rect);
        if (update_drawn || update_written)
        {
          GL_INS("Palette in VRAM dirty area, flushing cache");
          if (!IsFlushed())
            FlushRender();

          UpdateVRAMReadTexture(update_drawn, update_written);
        }
      }

      const GSVector4i page_rect = m_draw_mode.mode_reg.GetTexturePageRectangle();
      GSVector4i::storel(m_current_texture_page_offset, page_rect);

      u8 new_texpage_dirty = m_vram_dirty_draw_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_DRAWN_RECT : 0;
      new_texpage_dirty |= m_vram_dirty_write_rect.rintersects(page_rect) ? TEXPAGE_DIRTY_WRITTEN_RECT : 0;

      if (new_texpage_dirty != 0)
      {
        GL_INS("Texpage is in dirty area, checking UV ranges");
        m_texpage_dirty = new_texpage_dirty;
        m_compute_uv_range = true;
        m_current_uv_rect = INVALID_RECT;
      }
      else
      {
        m_compute_uv_range = m_clamp_uvs;
        if (m_texpage_dirty)
          GL_INS("Texpage is no longer dirty");
        m_texpage_dirty = 0;
      }
    }

    texture_mode = (m_draw_mode.mode_reg.texture_mode == GPUTextureMode::Reserved_Direct16Bit) ?
                     BatchTextureMode::Direct16Bit :
                     static_cast<BatchTextureMode>(m_draw_mode.mode_reg.texture_mode.GetValue());
  }

  // has any state changed which requires a new batch?
  // Reverse blending breaks with mixed transparent and opaque pixels, so we have to do one draw per polygon.
  // If we have fbfetch, we don't need to draw it in two passes. Test case: Suikoden 2 shadows.
  const GPUTransparencyMode transparency_mode =
    rc.transparency_enable ? m_draw_mode.mode_reg.transparency_mode : GPUTransparencyMode::Disabled;
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? m_GPUSTAT.dither_enable : false;
  if (texture_mode != m_batch.texture_mode || transparency_mode != m_batch.transparency_mode ||
      (transparency_mode == GPUTransparencyMode::BackgroundMinusForeground && !m_allow_shader_blend) ||
      dithering_enable != m_batch.dithering)
  {
    FlushRender();
  }

  EnsureVertexBufferSpaceForCurrentCommand();

  if (m_batch_index_count == 0)
  {
    // transparency mode change
    const bool check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
    if (transparency_mode != GPUTransparencyMode::Disabled && !m_rov_active && !m_prefer_shader_blend &&
        !NeedsShaderBlending(transparency_mode, texture_mode, check_mask_before_draw))
    {
      static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};

      const float src_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][0];
      const float dst_alpha_factor = transparent_alpha[static_cast<u32>(transparency_mode)][1];
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_src_alpha_factor != src_alpha_factor ||
                            m_batch_ubo_data.u_dst_alpha_factor != dst_alpha_factor);
      m_batch_ubo_data.u_src_alpha_factor = src_alpha_factor;
      m_batch_ubo_data.u_dst_alpha_factor = dst_alpha_factor;
    }

    const bool set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
    if (m_batch.check_mask_before_draw != check_mask_before_draw ||
        m_batch.set_mask_while_drawing != set_mask_while_drawing)
    {
      m_batch.check_mask_before_draw = check_mask_before_draw;
      m_batch.set_mask_while_drawing = set_mask_while_drawing;
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_set_mask_while_drawing != BoolToUInt32(set_mask_while_drawing));
      m_batch_ubo_data.u_set_mask_while_drawing = BoolToUInt32(set_mask_while_drawing);
    }

    m_batch.interlacing = IsInterlacedRenderingEnabled();
    if (m_batch.interlacing)
    {
      const u32 displayed_field = GetActiveLineLSB();
      m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlaced_displayed_field != displayed_field);
      m_batch_ubo_data.u_interlaced_displayed_field = displayed_field;
    }

    // update state
    m_batch.texture_mode = texture_mode;
    m_batch.transparency_mode = transparency_mode;
    m_batch.dithering = dithering_enable;

    if (m_draw_mode.IsTextureWindowChanged())
    {
      m_draw_mode.ClearTextureWindowChangedFlag();

      m_batch_ubo_data.u_texture_window[0] = ZeroExtend32(m_draw_mode.texture_window.and_x);
      m_batch_ubo_data.u_texture_window[1] = ZeroExtend32(m_draw_mode.texture_window.and_y);
      m_batch_ubo_data.u_texture_window[2] = ZeroExtend32(m_draw_mode.texture_window.or_x);
      m_batch_ubo_data.u_texture_window[3] = ZeroExtend32(m_draw_mode.texture_window.or_y);

      m_texture_window_active = ((m_draw_mode.texture_window.and_x & m_draw_mode.texture_window.and_y) != 0xFF ||
                                 ((m_draw_mode.texture_window.or_x | m_draw_mode.texture_window.or_y) != 0));
      m_batch_ubo_dirty = true;
    }

    if (m_drawing_area_changed)
    {
      m_drawing_area_changed = false;
      SetClampedDrawingArea();
      SetScissor();

      if (m_pgxp_depth_buffer && m_last_depth_z < 1.0f)
      {
        FlushRender();
        CopyAndClearDepthBuffer();
        EnsureVertexBufferSpaceForCurrentCommand();
      }

      if (m_sw_renderer)
      {
        GPUBackendSetDrawingAreaCommand* cmd = m_sw_renderer->NewSetDrawingAreaCommand();
        cmd->new_area = m_drawing_area;
        m_sw_renderer->PushCommand(cmd);
      }
    }
  }

  LoadVertices();
}

void GPU_HW::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  // Not done in HW, but need to forward through to SW if using that for readbacks
  if (m_sw_renderer)
  {
    GPUBackendUpdateCLUTCommand* cmd = m_sw_renderer->NewUpdateCLUTCommand();
    FillBackendCommandParameters(cmd);
    cmd->reg.bits = reg.bits;
    cmd->clut_is_8bit = clut_is_8bit;
    m_sw_renderer->PushCommand(cmd);
  }
}

void GPU_HW::FlushRender()
{
  const u32 base_vertex = m_batch_base_vertex;
  const u32 base_index = m_batch_base_index;
  const u32 index_count = m_batch_index_count;
  DebugAssert((m_batch_vertex_ptr != nullptr) == (m_batch_index_ptr != nullptr));
  if (m_batch_vertex_ptr)
    UnmapGPUBuffer(m_batch_vertex_count, index_count);
  if (index_count == 0)
    return;

#ifdef _DEBUG
  GL_SCOPE_FMT("Hardware Draw {}", ++s_draw_number);
#endif

  GL_INS_FMT("Dirty draw area: {}", m_vram_dirty_draw_rect);

  if (m_batch_ubo_dirty)
  {
    g_gpu_device->UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    // m_counters.num_ubo_updates++;
    m_batch_ubo_dirty = false;
  }

  if (m_wireframe_mode != GPUWireframeMode::OnlyWireframe)
  {
    if (NeedsShaderBlending(m_batch.transparency_mode, m_batch.texture_mode, m_batch.check_mask_before_draw) ||
        m_rov_active || (m_use_rov_for_shader_blend && m_pgxp_depth_buffer))
    {
      DrawBatchVertices(BatchRenderMode::ShaderBlend, index_count, base_index, base_vertex);
    }
    else if (NeedsTwoPassRendering())
    {
      DrawBatchVertices(BatchRenderMode::OnlyOpaque, index_count, base_index, base_vertex);
      DrawBatchVertices(BatchRenderMode::OnlyTransparent, index_count, base_index, base_vertex);
    }
    else
    {
      DrawBatchVertices(m_batch.GetRenderMode(), index_count, base_index, base_vertex);
    }
  }

  if (m_wireframe_mode != GPUWireframeMode::Disabled)
  {
    // This'll be less than ideal, but wireframe is for debugging, so take the perf hit.
    DeactivateROV();
    g_gpu_device->SetPipeline(m_wireframe_pipeline.get());
    g_gpu_device->DrawIndexed(index_count, base_index, base_vertex);
  }
}

void GPU_HW::UpdateDisplay()
{
  FlushRender();
  DeactivateROV();

  GL_SCOPE("UpdateDisplay()");

  if (g_settings.debugging.show_vram)
  {
    if (IsUsingMultisampling())
    {
      UpdateVRAMReadTexture(true, true);
      SetDisplayTexture(m_vram_read_texture.get(), nullptr, 0, 0, m_vram_read_texture->GetWidth(),
                        m_vram_read_texture->GetHeight());
    }
    else
    {
      SetDisplayTexture(m_vram_texture.get(), nullptr, 0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
    }

    return;
  }

  const bool interlaced = IsInterlacedDisplayEnabled();
  const u32 interlaced_field = GetInterlacedDisplayField();
  const u32 resolution_scale = m_GPUSTAT.display_area_color_depth_24 ? 1 : m_resolution_scale;
  const u32 scaled_vram_offset_x = m_crtc_state.display_vram_left * resolution_scale;
  const u32 scaled_vram_offset_y = (m_crtc_state.display_vram_top * resolution_scale) +
                                   ((interlaced && m_GPUSTAT.vertical_resolution) ? interlaced_field : 0);
  const u32 scaled_display_width = m_crtc_state.display_vram_width * resolution_scale;
  const u32 scaled_display_height = m_crtc_state.display_vram_height * resolution_scale;
  const u32 read_height = interlaced ? (scaled_display_height / 2u) : scaled_display_height;
  const u32 line_skip = BoolToUInt32(interlaced && m_GPUSTAT.vertical_resolution);
  bool drew_anything = false;

  // Don't bother grabbing depth if postfx doesn't need it.
  GPUTexture* depth_source = (!m_GPUSTAT.display_area_color_depth_24 && m_pgxp_depth_buffer &&
                              PostProcessing::InternalChain.NeedsDepthBuffer()) ?
                               (m_depth_was_copied ? m_vram_depth_copy_texture.get() : m_vram_depth_texture.get()) :
                               nullptr;

  if (IsDisplayDisabled())
  {
    ClearDisplayTexture();
    return;
  }
  else if (!m_GPUSTAT.display_area_color_depth_24 && !IsUsingMultisampling() &&
           (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture->GetWidth() &&
           (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture->GetHeight() &&
           !PostProcessing::InternalChain.IsActive())
  {
    SetDisplayTexture(m_vram_texture.get(), depth_source, scaled_vram_offset_x, scaled_vram_offset_y,
                      scaled_display_width, read_height);

    // Fast path if no copies are needed.
    if (interlaced)
    {
      GL_INS("Deinterlace fast path");
      drew_anything = true;
      Deinterlace(interlaced_field, line_skip);
    }
    else
    {
      GL_INS("Direct display");
    }
  }
  else
  {
    if (!m_vram_extract_texture || m_vram_extract_texture->GetWidth() != scaled_display_width ||
        m_vram_extract_texture->GetHeight() != read_height)
    {
      if (!g_gpu_device->ResizeTexture(&m_vram_extract_texture, scaled_display_width, read_height,
                                       GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8)) [[unlikely]]
      {
        ClearDisplayTexture();
        return;
      }
    }

    m_vram_texture->MakeReadyForSampling();
    g_gpu_device->InvalidateRenderTarget(m_vram_extract_texture.get());

    if (depth_source &&
        ((m_vram_extract_depth_texture && m_vram_extract_depth_texture->GetWidth() == scaled_display_width &&
          m_vram_extract_depth_texture->GetHeight() == scaled_display_height) ||
         !g_gpu_device->ResizeTexture(&m_vram_extract_depth_texture, scaled_display_width, scaled_display_height,
                                      GPUTexture::Type::RenderTarget, VRAM_DS_COLOR_FORMAT)))
    {
      depth_source->MakeReadyForSampling();
      g_gpu_device->InvalidateRenderTarget(m_vram_extract_depth_texture.get());

      GPUTexture* targets[] = {m_vram_extract_texture.get(), m_vram_extract_depth_texture.get()};
      g_gpu_device->SetRenderTargets(targets, static_cast<u32>(std::size(targets)), nullptr);
      g_gpu_device->SetPipeline(m_vram_extract_pipeline[2].get());

      g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, depth_source, g_gpu_device->GetNearestSampler());
    }
    else
    {
      g_gpu_device->SetRenderTarget(m_vram_extract_texture.get());
      g_gpu_device->SetPipeline(m_vram_extract_pipeline[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)].get());
      g_gpu_device->SetTextureSampler(0, m_vram_texture.get(), g_gpu_device->GetNearestSampler());
    }

    const u32 reinterpret_start_x = m_crtc_state.regs.X * resolution_scale;
    const u32 skip_x = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * resolution_scale;
    GL_INS_FMT("VRAM extract, depth = {}, 24bpp = {}, skip_x = {}, line_skip = {}", depth_source ? "yes" : "no",
               m_GPUSTAT.display_area_color_depth_24.GetValue(), skip_x, line_skip);
    GL_INS_FMT("Source: {},{} => {},{} ({}x{})", reinterpret_start_x, scaled_vram_offset_y,
               reinterpret_start_x + scaled_display_width, scaled_vram_offset_y + read_height, scaled_display_width,
               read_height);

    const u32 uniforms[4] = {reinterpret_start_x, scaled_vram_offset_y, skip_x, line_skip};
    g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));

    g_gpu_device->SetViewportAndScissor(0, 0, scaled_display_width, read_height);
    g_gpu_device->Draw(3, 0);

    m_vram_extract_texture->MakeReadyForSampling();
    if (depth_source)
    {
      // Thanks DX11...
      m_vram_extract_depth_texture->MakeReadyForSampling();
      g_gpu_device->SetTextureSampler(1, nullptr, nullptr);
    }

    drew_anything = true;

    SetDisplayTexture(m_vram_extract_texture.get(), depth_source ? m_vram_extract_depth_texture.get() : nullptr, 0, 0,
                      scaled_display_width, read_height);
    if (g_settings.display_24bit_chroma_smoothing)
    {
      if (ApplyChromaSmoothing())
      {
        if (interlaced)
          Deinterlace(interlaced_field, 0);
      }
    }
    else
    {
      if (interlaced)
        Deinterlace(interlaced_field, 0);
    }
  }

  if (m_downsample_mode != GPUDownsampleMode::Disabled && !m_GPUSTAT.display_area_color_depth_24)
  {
    DebugAssert(m_display_texture);
    DownsampleFramebuffer();
  }

  if (drew_anything)
    RestoreDeviceContext();
}

void GPU_HW::UpdateDownsamplingLevels()
{
  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    m_downsample_scale_or_levels = 0;
    u32 current_width = VRAM_WIDTH * m_resolution_scale;
    while (current_width >= VRAM_WIDTH)
    {
      m_downsample_scale_or_levels++;
      current_width /= 2;
    }
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    m_downsample_scale_or_levels = m_resolution_scale / GetBoxDownsampleScale(m_resolution_scale);
  }
  else
  {
    m_downsample_scale_or_levels = 0;
  }

  // Toss downsampling buffer, it's likely going to change resolution.
  g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
}

void GPU_HW::OnBufferSwapped()
{
  GL_INS("OnBufferSwapped()");
  m_depth_was_copied = false;
}

void GPU_HW::DownsampleFramebuffer()
{
  GPUTexture* source = m_display_texture;
  const u32 left = m_display_texture_view_x;
  const u32 top = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
    DownsampleFramebufferAdaptive(source, left, top, width, height);
  else
    DownsampleFramebufferBoxFilter(source, left, top, width, height);
}

void GPU_HW::DownsampleFramebufferAdaptive(GPUTexture* source, u32 left, u32 top, u32 width, u32 height)
{
  GL_PUSH_FMT("DownsampleFramebufferAdaptive ({},{} => {},{})", left, top, left + width, left + height);

  struct SmoothingUBOData
  {
    float min_uv[2];
    float max_uv[2];
    float rcp_size[2];
    float lod;
  };

  if (!m_downsample_texture || m_downsample_texture->GetWidth() != width || m_downsample_texture->GetHeight() != height)
  {
    g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
    m_downsample_texture =
      g_gpu_device->FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget, VRAM_RT_FORMAT);
  }
  std::unique_ptr<GPUTexture, GPUDevice::PooledTextureDeleter> level_texture = g_gpu_device->FetchAutoRecycleTexture(
    width, height, 1, m_downsample_scale_or_levels, 1, GPUTexture::Type::Texture, VRAM_RT_FORMAT);
  std::unique_ptr<GPUTexture, GPUDevice::PooledTextureDeleter> weight_texture =
    g_gpu_device->FetchAutoRecycleTexture(std::max(width >> (m_downsample_scale_or_levels - 1), 1u),
                                          std::max(height >> (m_downsample_scale_or_levels - 1), 1u), 1, 1, 1,
                                          GPUTexture::Type::RenderTarget, GPUTexture::Format::R8);
  if (!m_downsample_texture || !level_texture || !weight_texture)
  {
    ERROR_LOG("Failed to create {}x{} RTs for adaptive downsampling", width, height);
    return;
  }

  g_gpu_device->CopyTextureRegion(level_texture.get(), 0, 0, 0, 0, source, left, top, 0, 0, width, height);
  g_gpu_device->SetTextureSampler(0, level_texture.get(), m_downsample_lod_sampler.get());

  SmoothingUBOData uniforms;

  // create mip chain
  for (u32 level = 1; level < m_downsample_scale_or_levels; level++)
  {
    GL_SCOPE_FMT("Create miplevel {}", level);

    const u32 level_width = width >> level;
    const u32 level_height = height >> level;
    const float rcp_width = 1.0f / static_cast<float>(level_texture->GetMipWidth(level));
    const float rcp_height = 1.0f / static_cast<float>(level_texture->GetMipHeight(level));
    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = static_cast<float>(level_width) * rcp_width;
    uniforms.max_uv[1] = static_cast<float>(level_height) * rcp_height;
    uniforms.rcp_size[0] = rcp_width;
    uniforms.rcp_size[1] = rcp_height;
    uniforms.lod = static_cast<float>(level - 1);

    g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, level_width, level_height));
    g_gpu_device->SetPipeline((level == 1) ? m_downsample_first_pass_pipeline.get() :
                                             m_downsample_mid_pass_pipeline.get());
    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
    g_gpu_device->Draw(3, 0);
    g_gpu_device->CopyTextureRegion(level_texture.get(), 0, 0, 0, level, m_downsample_texture.get(), 0, 0, 0, 0,
                                    level_width, level_height);
  }

  // blur pass at lowest level
  {
    GL_SCOPE("Blur");

    const u32 last_level = m_downsample_scale_or_levels - 1;
    const u32 last_width = level_texture->GetMipWidth(last_level);
    const u32 last_height = level_texture->GetMipHeight(last_level);
    const float rcp_width = 1.0f / static_cast<float>(m_downsample_texture->GetWidth());
    const float rcp_height = 1.0f / static_cast<float>(m_downsample_texture->GetHeight());
    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = static_cast<float>(last_width) * rcp_width;
    uniforms.max_uv[1] = static_cast<float>(last_height) * rcp_height;
    uniforms.rcp_size[0] = rcp_width;
    uniforms.rcp_size[1] = rcp_height;
    uniforms.lod = 0.0f;

    m_downsample_texture->MakeReadyForSampling();
    g_gpu_device->InvalidateRenderTarget(weight_texture.get());
    g_gpu_device->SetRenderTarget(weight_texture.get());
    g_gpu_device->SetTextureSampler(0, m_downsample_texture.get(), g_gpu_device->GetNearestSampler());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, last_width, last_height));
    g_gpu_device->SetPipeline(m_downsample_blur_pass_pipeline.get());
    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
    g_gpu_device->Draw(3, 0);
    weight_texture->MakeReadyForSampling();
  }

  // composite downsampled and upsampled images together
  {
    GL_SCOPE("Composite");

    uniforms.min_uv[0] = 0.0f;
    uniforms.min_uv[1] = 0.0f;
    uniforms.max_uv[0] = 1.0f;
    uniforms.max_uv[1] = 1.0f;

    g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetRenderTarget(m_downsample_texture.get());
    g_gpu_device->SetTextureSampler(0, level_texture.get(), m_downsample_composite_sampler.get());
    g_gpu_device->SetTextureSampler(1, weight_texture.get(), m_downsample_lod_sampler.get());
    g_gpu_device->SetViewportAndScissor(GSVector4i(0, 0, width, height));
    g_gpu_device->SetPipeline(m_downsample_composite_pass_pipeline.get());
    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));
    g_gpu_device->Draw(3, 0);
    m_downsample_texture->MakeReadyForSampling();
  }

  GL_POP();

  RestoreDeviceContext();

  SetDisplayTexture(m_downsample_texture.get(), m_display_depth_buffer, 0, 0, width, height);
}

void GPU_HW::DownsampleFramebufferBoxFilter(GPUTexture* source, u32 left, u32 top, u32 width, u32 height)
{
  GL_SCOPE_FMT("DownsampleFramebufferBoxFilter({},{} => {},{} ({}x{})", left, top, left + width, top + height, width,
               height);

  const u32 ds_width = width / m_downsample_scale_or_levels;
  const u32 ds_height = height / m_downsample_scale_or_levels;

  if (!m_downsample_texture || m_downsample_texture->GetWidth() != ds_width ||
      m_downsample_texture->GetHeight() != ds_height)
  {
    g_gpu_device->RecycleTexture(std::move(m_downsample_texture));
    m_downsample_texture =
      g_gpu_device->FetchTexture(ds_width, ds_height, 1, 1, 1, GPUTexture::Type::RenderTarget, VRAM_RT_FORMAT);
  }
  if (!m_downsample_texture)
  {
    ERROR_LOG("Failed to create {}x{} RT for box downsampling", width, height);
    return;
  }

  source->MakeReadyForSampling();

  const u32 uniforms[4] = {left, top, 0u, 0u};

  g_gpu_device->InvalidateRenderTarget(m_downsample_texture.get());
  g_gpu_device->SetRenderTarget(m_downsample_texture.get());
  g_gpu_device->SetPipeline(m_downsample_first_pass_pipeline.get());
  g_gpu_device->SetTextureSampler(0, source, g_gpu_device->GetNearestSampler());
  g_gpu_device->SetViewportAndScissor(0, 0, ds_width, ds_height);
  g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
  g_gpu_device->Draw(3, 0);

  RestoreDeviceContext();

  SetDisplayTexture(m_downsample_texture.get(), m_display_depth_buffer, 0, 0, ds_width, ds_height);
}

void GPU_HW::DrawRendererStats()
{
  if (ImGui::CollapsingHeader("Renderer Statistics", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
    static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f * Host::GetOSDScale());

    ImGui::TextUnformatted("Resolution Scale:");
    ImGui::NextColumn();
    ImGui::Text("%u (VRAM %ux%u)", m_resolution_scale, VRAM_WIDTH * m_resolution_scale,
                VRAM_HEIGHT * m_resolution_scale);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Effective Display Resolution:");
    ImGui::NextColumn();
    ImGui::Text("%ux%u", m_crtc_state.display_vram_width * m_resolution_scale,
                m_crtc_state.display_vram_height * m_resolution_scale);
    ImGui::NextColumn();

    ImGui::TextUnformatted("True Color:");
    ImGui::NextColumn();
    ImGui::TextColored(m_true_color ? active_color : inactive_color, m_true_color ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    const bool debanding = (g_settings.gpu_true_color && g_settings.gpu_debanding);
    ImGui::TextUnformatted("Debanding:");
    ImGui::NextColumn();
    ImGui::TextColored(debanding ? active_color : inactive_color, debanding ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    const bool scaled_dithering = (m_resolution_scale > 1 && g_settings.gpu_scaled_dithering);
    ImGui::TextUnformatted("Scaled Dithering:");
    ImGui::NextColumn();
    ImGui::TextColored(scaled_dithering ? active_color : inactive_color, scaled_dithering ? "Enabled" : "Disabled");
    ImGui::NextColumn();

    ImGui::TextUnformatted("Texture Filtering:");
    ImGui::NextColumn();
    ImGui::TextColored((m_texture_filtering != GPUTextureFilter::Nearest) ? active_color : inactive_color, "%s",
                       Settings::GetTextureFilterDisplayName(m_texture_filtering));
    ImGui::NextColumn();

    ImGui::TextUnformatted("PGXP:");
    ImGui::NextColumn();
    ImGui::TextColored(g_settings.gpu_pgxp_enable ? active_color : inactive_color, "Geom");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_culling) ? active_color : inactive_color,
                       "Cull");
    ImGui::SameLine();
    ImGui::TextColored(
      (g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction) ? active_color : inactive_color, "Tex");
    ImGui::SameLine();
    ImGui::TextColored((g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_vertex_cache) ? active_color : inactive_color,
                       "Cache");
    ImGui::NextColumn();

    ImGui::Columns(1);
  }
}

std::unique_ptr<GPU> GPU::CreateHardwareRenderer()
{
  std::unique_ptr<GPU_HW> gpu(std::make_unique<GPU_HW>());
  if (!gpu->Initialize())
    return nullptr;

  return gpu;
}
