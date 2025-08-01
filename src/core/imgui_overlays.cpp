// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui_overlays.h"
#include "cdrom.h"
#include "controller.h"
#include "cpu_core_private.h"
#include "dma.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "host.h"
#include "mdec.h"
#include "settings.h"
#include "spu.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_animated.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/media_capture.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
#include "common/timer.h"

#include "IconsFontAwesome5.h"
#include "fmt/chrono.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <span>

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
static void FormatProcessorStat(SmallStringBase& text, double usage, double time);
static void DrawPerformanceOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing);
static void DrawEnhancementsOverlay();
static void DrawInputsOverlay();
} // namespace ImGuiManager

static std::tuple<float, float> GetMinMax(std::span<const float> values)
{
  GSVector4 vmin(GSVector4::load<false>(values.data()));
  GSVector4 vmax(vmin);

  const u32 count = static_cast<u32>(values.size());
  const u32 aligned_count = Common::AlignDownPow2(count, 4);
  u32 i = 4;
  for (; i < aligned_count; i += 4)
  {
    const GSVector4 v(GSVector4::load<false>(&values[i]));
    vmin = vmin.min(v);
    vmax = vmax.max(v);
  }

  float min = std::min(vmin.x, std::min(vmin.y, std::min(vmin.z, vmin.w)));
  float max = std::max(vmax.x, std::max(vmax.y, std::max(vmax.z, vmax.w)));
  for (; i < count; i++)
  {
    min = std::min(min, values[i]);
    max = std::max(max, values[i]);
  }

  return std::tie(min, max);
}

void Host::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/, int progress_max /*= -1*/,
                                int progress_value /*= -1*/)
{
  if (!g_gpu_device)
  {
    INFO_LOG("{}: {}/{}", message, progress_value, progress_max);
    return;
  }

  const auto& io = ImGui::GetIO();
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = (400.0f * scale);
  const bool has_progress = (progress_min < progress_max);

  // eat the last imgui frame, it might've been partially rendered by the caller.
  ImGui::EndFrame();
  ImGui::NewFrame();

  const float logo_width = 260.0f * scale;
  const float logo_height = 260.0f * scale;

  ImGui::SetNextWindowSize(ImVec2(logo_width, logo_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) - (50.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("LoadingScreenLogo", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground))
  {
    GPUTexture* tex = ImGuiFullscreen::GetCachedTexture("images/duck.png");
    if (tex)
      ImGui::Image(tex, ImVec2(logo_width, logo_height));
  }
  ImGui::End();

  const float padding_and_rounding = 15.0f * scale;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
  ImGui::SetNextWindowSize(ImVec2(width, (has_progress ? 80.0f : 50.0f) * scale), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) + (100.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  if (ImGui::Begin("LoadingScreen", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (has_progress)
    {
      ImGui::TextUnformatted(message);

      TinyString buf;
      buf.format("{}/{}", progress_value, progress_max);

      const ImVec2 prog_size = ImGui::CalcTextSize(buf.c_str(), buf.end_ptr());
      ImGui::SameLine();
      ImGui::SetCursorPosX(width - padding_and_rounding - prog_size.x);
      ImGui::TextUnformatted(buf.c_str(), buf.end_ptr());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);

      ImGui::ProgressBar(static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min),
                         ImVec2(-1.0f, 0.0f), "");
      INFO_LOG("{}: {}", message, buf);
    }
    else
    {
      const ImVec2 text_size(ImGui::CalcTextSize(message));
      ImGui::SetCursorPosX((width - text_size.x) / 2.0f);
      ImGui::TextUnformatted(message);
      INFO_LOG(message);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);

  ImGui::EndFrame();

  // TODO: Glass effect or something.

  if (g_gpu_device->BeginPresent(false))
  {
    g_gpu_device->RenderImGui();
    g_gpu_device->EndPresent(false);
  }

  ImGui::NewFrame();
}

void ImGuiManager::RenderDebugWindows()
{
  if (System::IsValid())
  {
    if (g_settings.debugging.show_gpu_state)
      g_gpu->DrawDebugStateWindow();
    if (g_settings.debugging.show_cdrom_state)
      CDROM::DrawDebugWindow();
    if (g_settings.debugging.show_timers_state)
      Timers::DrawDebugStateWindow();
    if (g_settings.debugging.show_spu_state)
      SPU::DrawDebugStateWindow();
    if (g_settings.debugging.show_mdec_state)
      MDEC::DrawDebugStateWindow();
    if (g_settings.debugging.show_dma_state)
      DMA::DrawDebugStateWindow();
  }
}

void ImGuiManager::RenderTextOverlays()
{
  const System::State state = System::GetState();
  if (state != System::State::Shutdown)
  {
    const float scale = ImGuiManager::GetGlobalScale();
    const float margin = std::ceil(10.0f * scale);
    const float spacing = std::ceil(5.0f * scale);
    float position_y = margin;
    DrawPerformanceOverlay(position_y, scale, margin, spacing);
    DrawFrameTimeOverlay(position_y, scale, margin, spacing);
    DrawMediaCaptureOverlay(position_y, scale, margin, spacing);

    if (g_settings.display_show_enhancements && state != System::State::Paused)
      DrawEnhancementsOverlay();

    if (g_settings.display_show_inputs && state != System::State::Paused)
      DrawInputsOverlay();
  }
}

void ImGuiManager::FormatProcessorStat(SmallStringBase& text, double usage, double time)
{
  // Some values, such as GPU (and even CPU to some extent) can be out of phase with the wall clock,
  // which the processor time is divided by to get a utilization percentage. Let's clamp it at 100%,
  // so that people don't get confused, and remove the decimal places when it's there while we're at it.
  if (usage >= 99.95)
    text.append_format("100% ({:.2f}ms)", time);
  else
    text.append_format("{:.1f}% ({:.2f}ms)", usage, time);
}

void ImGuiManager::DrawPerformanceOverlay(float& position_y, float scale, float margin, float spacing)
{
  if (!(g_settings.display_show_fps || g_settings.display_show_speed || g_settings.display_show_gpu_stats ||
        g_settings.display_show_resolution || g_settings.display_show_cpu_usage ||
        (g_settings.display_show_status_indicators &&
         (System::IsPaused() || System::IsFastForwardEnabled() || System::IsTurboEnabled()))))
  {
    return;
  }

  const float shadow_offset = std::ceil(1.0f * scale);
  ImFont* fixed_font = ImGuiManager::GetFixedFont();
  ImFont* standard_font = ImGuiManager::GetStandardFont();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  SmallString text;
  ImVec2 text_size;

#define DRAW_LINE(font, text, color)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    text_size =                                                                                                        \
      font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, (text), nullptr, nullptr);         \
    dl->AddText(                                                                                                       \
      font, font->FontSize,                                                                                            \
      ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),         \
      IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());                                                           \
    dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y), color,  \
                (text));                                                                                               \
    position_y += text_size.y + spacing;                                                                               \
  } while (0)

  const System::State state = System::GetState();
  if (state == System::State::Running)
  {
    const float speed = System::GetEmulationSpeed();
    if (g_settings.display_show_fps)
      text.append_format("G: {:.2f} | V: {:.2f}", System::GetFPS(), System::GetVPS());
    if (g_settings.display_show_speed)
    {
      text.append_format("{}{}%", text.empty() ? "" : " | ", static_cast<u32>(std::round(speed)));

      const float target_speed = System::GetTargetSpeed();
      if (target_speed <= 0.0f)
        text.append(" (Max)");
      else
        text.append_format(" ({:.0f}%)", target_speed * 100.0f);
    }
    if (!text.empty())
    {
      ImU32 color;
      if (speed < 95.0f)
        color = IM_COL32(255, 100, 100, 255);
      else if (speed > 105.0f)
        color = IM_COL32(100, 255, 100, 255);
      else
        color = IM_COL32(255, 255, 255, 255);

      DRAW_LINE(fixed_font, text, color);
    }

    if (g_settings.display_show_gpu_stats)
    {
      g_gpu->GetStatsString(text);
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      g_gpu->GetMemoryStatsString(text);
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_resolution)
    {
      // TODO: this seems wrong?
      const auto [effective_width, effective_height] = g_gpu->GetEffectiveDisplayResolution();
      const bool interlaced = g_gpu->IsInterlacedDisplayEnabled();
      const bool pal = g_gpu->IsInPALMode();
      text.format("{}x{} {} {}", effective_width, effective_height, pal ? "PAL" : "NTSC",
                  interlaced ? "Interlaced" : "Progressive");
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_latency_stats)
    {
      System::FormatLatencyStats(text);
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_cpu_usage)
    {
      text.format("{:.2f}ms | {:.2f}ms | {:.2f}ms", System::GetMinimumFrameTime(), System::GetAverageFrameTime(),
                  System::GetMaximumFrameTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (g_settings.cpu_overclock_active || CPU::g_state.using_interpreter ||
          g_settings.cpu_execution_mode != CPUExecutionMode::Recompiler || g_settings.cpu_recompiler_icache ||
          g_settings.cpu_recompiler_memory_exceptions)
      {
        bool first = true;
        text.assign("CPU[");
        if (g_settings.cpu_overclock_active)
        {
          text.append_format("{}", g_settings.GetCPUOverclockPercent());
          first = false;
        }
        if (CPU::g_state.using_interpreter)
        {
          text.append_format("{}{}", first ? "" : "/", "I");
          first = false;
        }
        else if (g_settings.cpu_execution_mode == CPUExecutionMode::CachedInterpreter)
        {
          text.append_format("{}{}", first ? "" : "/", "CI");
          first = false;
        }
        else if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
        {
          text.append_format("{}{}", first ? "" : "/", "NR");
          first = false;
        }
        else
        {
          if (g_settings.cpu_recompiler_icache)
          {
            text.append_format("{}{}", first ? "" : "/", "IC");
            first = false;
          }
          if (g_settings.cpu_recompiler_memory_exceptions)
          {
            text.append_format("{}{}", first ? "" : "/", "ME");
            first = false;
          }
        }

        text.append("]: ");
      }
      else
      {
        text.assign("CPU: ");
      }
      FormatProcessorStat(text, System::GetCPUThreadUsage(), System::GetCPUThreadAverageTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));

      if (g_gpu->GetSWThread())
      {
        text.assign("SW: ");
        FormatProcessorStat(text, System::GetSWThreadUsage(), System::GetSWThreadAverageTime());
        DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
      }

#ifndef __ANDROID__
      if (MediaCapture* cap = System::GetMediaCapture())
      {
        text.assign("CAP: ");
        FormatProcessorStat(text, cap->GetCaptureThreadUsage(), cap->GetCaptureThreadTime());
        DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
      }
#endif
    }

    if (g_settings.display_show_gpu_usage && g_gpu_device->IsGPUTimingEnabled())
    {
      text.assign("GPU: ");
      FormatProcessorStat(text, System::GetGPUUsage(), System::GetGPUAverageTime());
      DRAW_LINE(fixed_font, text, IM_COL32(255, 255, 255, 255));
    }

    if (g_settings.display_show_status_indicators)
    {
      const bool rewinding = System::IsRewinding();
      if (rewinding || System::IsFastForwardEnabled() || System::IsTurboEnabled())
      {
        text.assign(rewinding ? ICON_FA_FAST_BACKWARD : ICON_FA_FAST_FORWARD);
        DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
      }
    }
  }
  else if (g_settings.display_show_status_indicators && state == System::State::Paused &&
           !FullscreenUI::HasActiveWindow())
  {
    text.assign(ICON_FA_PAUSE);
    DRAW_LINE(standard_font, text, IM_COL32(255, 255, 255, 255));
  }

#undef DRAW_LINE
}

void ImGuiManager::DrawEnhancementsOverlay()
{
  LargeString text;
  text.append_format("{} {}-{}", Settings::GetConsoleRegionName(System::GetRegion()),
                     GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()),
                     g_gpu->IsHardwareRenderer() ? "HW" : "SW");

  if (g_settings.rewind_enable)
    text.append_format(" RW={}/{}", g_settings.rewind_save_frequency, g_settings.rewind_save_slots);
  if (g_settings.IsRunaheadEnabled())
    text.append_format(" RA={}", g_settings.runahead_frames);

  if (g_settings.cpu_overclock_active)
    text.append_format(" CPU={}%", g_settings.GetCPUOverclockPercent());
  if (g_settings.enable_8mb_ram)
    text.append(" 8MB");
  if (g_settings.cdrom_read_speedup != 1)
    text.append_format(" CDR={}x", g_settings.cdrom_read_speedup);
  if (g_settings.cdrom_seek_speedup != 1)
    text.append_format(" CDS={}x", g_settings.cdrom_seek_speedup);
  if (g_settings.gpu_resolution_scale != 1)
    text.append_format(" IR={}x", g_settings.gpu_resolution_scale);
  if (g_settings.gpu_multisamples != 1)
  {
    text.append_format(" {}x{}", g_settings.gpu_multisamples, g_settings.gpu_per_sample_shading ? "SSAA" : "MSAA");
  }
  if (g_settings.gpu_true_color)
  {
    if (g_settings.gpu_debanding)
    {
      text.append(" TrueColDeband");
    }
    else
    {
      text.append(" TrueCol");
    }
  }
  if (g_settings.gpu_disable_interlacing)
    text.append(" ForceProg");
  if (g_settings.gpu_force_ntsc_timings && System::GetRegion() == ConsoleRegion::PAL)
    text.append(" PAL60");
  if (g_settings.gpu_texture_filter != GPUTextureFilter::Nearest)
  {
    if (g_settings.gpu_sprite_texture_filter != g_settings.gpu_texture_filter)
    {
      text.append_format(" {}/{}", Settings::GetTextureFilterName(g_settings.gpu_texture_filter),
                         Settings::GetTextureFilterName(g_settings.gpu_sprite_texture_filter));
    }
    else
    {
      text.append_format(" {}", Settings::GetTextureFilterName(g_settings.gpu_texture_filter));
    }
  }
  if (g_settings.gpu_widescreen_hack && g_settings.display_aspect_ratio != DisplayAspectRatio::Auto &&
      g_settings.display_aspect_ratio != DisplayAspectRatio::R4_3)
  {
    text.append(" WSHack");
  }
  if (g_settings.gpu_line_detect_mode != GPULineDetectMode::Disabled)
    text.append_format(" LD={}", Settings::GetLineDetectModeName(g_settings.gpu_line_detect_mode));
  if (g_settings.gpu_pgxp_enable)
  {
    text.append(" PGXP");
    if (g_settings.gpu_pgxp_culling)
      text.append("/Cull");
    if (g_settings.gpu_pgxp_texture_correction)
      text.append("/Tex");
    if (g_settings.gpu_pgxp_color_correction)
      text.append("/Col");
    if (g_settings.gpu_pgxp_vertex_cache)
      text.append("/VC");
    if (g_settings.gpu_pgxp_cpu)
      text.append("/CPU");
    if (g_settings.gpu_pgxp_depth_buffer)
      text.append("/Depth");
  }

  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - font->FontSize;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImVec2 text_size = font->CalcTextSizeA(font->FontSize, std::numeric_limits<float>::max(), -1.0f, text.c_str(),
                                         text.end_ptr(), nullptr);
  dl->AddText(font, font->FontSize,
              ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x + shadow_offset, position_y + shadow_offset),
              IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
  dl->AddText(font, font->FontSize, ImVec2(ImGui::GetIO().DisplaySize.x - margin - text_size.x, position_y),
              IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
}

void ImGuiManager::DrawMediaCaptureOverlay(float& position_y, float scale, float margin, float spacing)
{
#ifndef __ANDROID__
  MediaCapture* const cap = System::GetMediaCapture();
  if (!cap || FullscreenUI::HasActiveWindow())
    return;

  const float shadow_offset = std::ceil(scale);
  ImFont* const standard_font = ImGuiManager::GetStandardFont();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  static constexpr const char* ICON = ICON_FA_VIDEO;
  const time_t elapsed_time = cap->GetElapsedTime();
  const TinyString text_msg = TinyString::from_format(" {:02d}:{:02d}:{:02d}", elapsed_time / 3600,
                                                      (elapsed_time % 3600) / 60, (elapsed_time % 3600) % 60);
  const ImVec2 icon_size = standard_font->CalcTextSizeA(standard_font->FontSize, std::numeric_limits<float>::max(),
                                                        -1.0f, ICON, nullptr, nullptr);
  const ImVec2 text_size = standard_font->CalcTextSizeA(standard_font->FontSize, std::numeric_limits<float>::max(),
                                                        -1.0f, text_msg.c_str(), text_msg.end_ptr(), nullptr);

  const float box_margin = 2.0f * scale;
  const ImVec2 box_size = ImVec2(icon_size.x + shadow_offset + text_size.x + box_margin * 2.0f,
                                 std::max(icon_size.x, text_size.y) + box_margin * 2.0f);
  const ImVec2 box_pos = ImVec2(ImGui::GetIO().DisplaySize.x - margin - box_size.x, position_y);
  dl->AddRectFilled(box_pos, box_pos + box_size, IM_COL32(0, 0, 0, 64), box_margin);

  const ImVec2 text_start = ImVec2(box_pos.x + box_margin, box_pos.y + box_margin);
  dl->AddText(standard_font, standard_font->FontSize,
              ImVec2(text_start.x + shadow_offset, text_start.y + shadow_offset), IM_COL32(0, 0, 0, 100), ICON);
  dl->AddText(standard_font, standard_font->FontSize,
              ImVec2(text_start.x + icon_size.x + shadow_offset, text_start.y + shadow_offset), IM_COL32(0, 0, 0, 100),
              text_msg.c_str(), text_msg.end_ptr());
  dl->AddText(standard_font, standard_font->FontSize, text_start, IM_COL32(255, 0, 0, 255), ICON);
  dl->AddText(standard_font, standard_font->FontSize, ImVec2(text_start.x + icon_size.x, text_start.y),
              IM_COL32(255, 255, 255, 255), text_msg.c_str(), text_msg.end_ptr());

  position_y += box_size.y + spacing;
#endif
}

void ImGuiManager::DrawFrameTimeOverlay(float& position_y, float scale, float margin, float spacing)
{
  if (!g_settings.display_show_frame_times || System::IsPaused())
    return;

  const float shadow_offset = std::ceil(1.0f * scale);
  ImFont* fixed_font = ImGuiManager::GetFixedFont();

  const ImVec2 history_size(200.0f * scale, 50.0f * scale);
  ImGui::SetNextWindowSize(ImVec2(history_size.x, history_size.y));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - margin - history_size.x, position_y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  if (ImGui::Begin("##frame_times", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs))
  {
    ImGui::PushFont(fixed_font);

    auto [min, max] = GetMinMax(System::GetFrameTimeHistory());

    // add a little bit of space either side, so we're not constantly resizing
    if ((max - min) < 4.0f)
    {
      min = min - std::fmod(min, 1.0f);
      max = max - std::fmod(max, 1.0f) + 1.0f;
      min = std::max(min - 2.0f, 0.0f);
      max += 2.0f;
    }

    ImGui::PlotEx(
      ImGuiPlotType_Lines, "##frame_times",
      [](void*, int idx) -> float {
        return System::GetFrameTimeHistory()[((System::GetFrameTimeHistoryPos() + idx) %
                                              System::NUM_FRAME_TIME_SAMPLES)];
      },
      nullptr, System::NUM_FRAME_TIME_SAMPLES, 0, nullptr, min, max, history_size);

    ImDrawList* win_dl = ImGui::GetCurrentWindow()->DrawList;
    const ImVec2 wpos(ImGui::GetCurrentWindow()->Pos);

    TinyString text;
    text.format("{:.1f} ms", max);
    ImVec2 text_size = fixed_font->CalcTextSizeA(fixed_font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset, wpos.y + shadow_offset),
                    IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y), IM_COL32(255, 255, 255, 255),
                    text.c_str(), text.end_ptr());

    text.format("{:.1f} ms", min);
    text_size = fixed_font->CalcTextSizeA(fixed_font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.end_ptr());
    win_dl->AddText(ImVec2(wpos.x + history_size.x - text_size.x - spacing + shadow_offset,
                           wpos.y + history_size.y - fixed_font->FontSize + shadow_offset),
                    IM_COL32(0, 0, 0, 100), text.c_str(), text.end_ptr());
    win_dl->AddText(
      ImVec2(wpos.x + history_size.x - text_size.x - spacing, wpos.y + history_size.y - fixed_font->FontSize),
      IM_COL32(255, 255, 255, 255), text.c_str(), text.end_ptr());
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleVar(5);
  ImGui::PopStyleColor(3);

  position_y += history_size.y + spacing;
}

void ImGuiManager::DrawInputsOverlay()
{
  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  const float spacing = 5.0f * scale;
  ImFont* font = ImGuiManager::GetStandardFont();

  static constexpr u32 text_color = IM_COL32(0xff, 0xff, 0xff, 255);
  static constexpr u32 shadow_color = IM_COL32(0x00, 0x00, 0x00, 100);

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  u32 num_ports = 0;
  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] != ControllerType::None)
      num_ports++;
  }

  float current_x = margin;
  float current_y = display_size.y - margin - ((static_cast<float>(num_ports) * (font->FontSize + spacing)) - spacing);

  const ImVec4 clip_rect(current_x, current_y, display_size.x - margin, display_size.y - margin);

  SmallString text;

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (g_settings.controller_types[port] == ControllerType::None)
      continue;

    const Controller* controller = System::GetController(port);
    const Controller::ControllerInfo* cinfo =
      controller ? Controller::GetControllerInfo(controller->GetType()) : nullptr;
    if (!cinfo)
      continue;

    if (cinfo->icon_name)
      text.format("{} {}", cinfo->icon_name, port + 1u);
    else
      text.format("{} |", port + 1u);

    for (const Controller::ControllerBindingInfo& bi : cinfo->bindings)
    {
      switch (bi.type)
      {
        case InputBindingInfo::Type::Axis:
        case InputBindingInfo::Type::HalfAxis:
        {
          // axes are always shown
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= (254.0f / 255.0f))
            text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
          else if (value > (1.0f / 255.0f))
            text.append_format(" {}: {:.2f}", bi.icon_name ? bi.icon_name : bi.name, value);
        }
        break;

        case InputBindingInfo::Type::Button:
        {
          // buttons only shown when active
          const float value = controller->GetBindState(bi.bind_index);
          if (value >= 0.5f)
            text.append_format(" {}", bi.icon_name ? bi.icon_name : bi.name);
        }
        break;

        case InputBindingInfo::Type::Motor:
        case InputBindingInfo::Type::Macro:
        case InputBindingInfo::Type::Unknown:
        case InputBindingInfo::Type::Pointer:
        default:
          break;
      }
    }

    dl->AddText(font, font->FontSize, ImVec2(current_x + shadow_offset, current_y + shadow_offset), shadow_color,
                text.c_str(), text.end_ptr(), 0.0f, &clip_rect);
    dl->AddText(font, font->FontSize, ImVec2(current_x, current_y), text_color, text.c_str(), text.end_ptr(), 0.0f,
                &clip_rect);

    current_y += font->FontSize + spacing;
  }
}

namespace SaveStateSelectorUI {
namespace {
struct ListEntry
{
  std::string summary;
  std::string game_details; // only in global slots
  std::string filename;
  std::unique_ptr<GPUTexture> preview_texture;
  s32 slot;
  bool global;
};
} // namespace

static void InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global);
static void InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, const std::string& path, s32 slot,
                                bool global);

static void DestroyTextures();
static void RefreshHotkeyLegend();
static void Draw();
static void ShowSlotOSDMessage();
static std::string GetCurrentSlotPath();

static constexpr const char* DATE_TIME_FORMAT =
  TRANSLATE_NOOP("SaveStateSelectorUI", "Saved at {0:%H:%M} on {0:%a} {0:%Y/%m/%d}.");

static std::shared_ptr<GPUTexture> s_placeholder_texture;

static std::string s_load_legend;
static std::string s_save_legend;
static std::string s_prev_legend;
static std::string s_next_legend;

static llvm::SmallVector<ListEntry, System::PER_GAME_SAVE_STATE_SLOTS + System::GLOBAL_SAVE_STATE_SLOTS> s_slots;
static s32 s_current_slot = 0;
static bool s_current_slot_global = false;

static float s_open_time = 0.0f;
static float s_close_time = 0.0f;

static ImAnimatedFloat s_scroll_animated;
static ImAnimatedFloat s_background_animated;

static bool s_open = false;
} // namespace SaveStateSelectorUI

bool SaveStateSelectorUI::IsOpen()
{
  return s_open;
}

void SaveStateSelectorUI::Open(float open_time /* = DEFAULT_OPEN_TIME */)
{
  const std::string& serial = System::GetGameSerial();

  s_open_time = 0.0f;
  s_close_time = open_time;

  if (s_open)
    return;

  if (!s_placeholder_texture)
    s_placeholder_texture = ImGuiFullscreen::LoadTexture("no-save.png");

  s_open = true;
  RefreshList(serial);
  RefreshHotkeyLegend();
}

void SaveStateSelectorUI::Close()
{
  s_open = false;
  s_load_legend = {};
  s_save_legend = {};
  s_prev_legend = {};
  s_next_legend = {};
}

void SaveStateSelectorUI::RefreshList(const std::string& serial)
{
  for (ListEntry& entry : s_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }
  s_slots.clear();

  if (System::IsShutdown())
    return;

  if (!serial.empty())
  {
    for (s32 i = 1; i <= System::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::string path(System::GetGameSaveStateFileName(serial, i));
      std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

      ListEntry li;
      if (ssi)
        InitializeListEntry(&li, &ssi.value(), std::move(path), i, false);
      else
        InitializePlaceholderListEntry(&li, std::move(path), i, false);

      s_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= System::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::string path(System::GetGlobalSaveStateFileName(i));
    std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(path.c_str());

    ListEntry li;
    if (ssi)
      InitializeListEntry(&li, &ssi.value(), std::move(path), i, true);
    else
      InitializePlaceholderListEntry(&li, std::move(path), i, true);

    s_slots.push_back(std::move(li));
  }
}

void SaveStateSelectorUI::Clear()
{
  // called on CPU thread at shutdown, textures should already be deleted, unless running
  // big picture UI, in which case we have to delete them here...
  ClearList();

  s_current_slot = 0;
  s_current_slot_global = false;
  s_scroll_animated.Reset(0.0f);
  s_background_animated.Reset(0.0f);
}

void SaveStateSelectorUI::ClearList()
{
  for (ListEntry& li : s_slots)
  {
    if (li.preview_texture)
      g_gpu_device->RecycleTexture(std::move(li.preview_texture));
  }
  s_slots.clear();
}

void SaveStateSelectorUI::DestroyTextures()
{
  Close();

  for (ListEntry& entry : s_slots)
  {
    if (entry.preview_texture)
      g_gpu_device->RecycleTexture(std::move(entry.preview_texture));
  }

  s_placeholder_texture.reset();
}

void SaveStateSelectorUI::RefreshHotkeyLegend()
{
  auto format_legend_entry = [](SmallString binding, std::string_view caption) {
    InputManager::PrettifyInputBinding(binding);
    return fmt::format("{} - {}", binding, caption);
  };

  s_load_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "LoadSelectedSaveState"),
                                      TRANSLATE_SV("SaveStateSelectorUI", "Load"));
  s_save_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SaveSelectedSaveState"),
                                      TRANSLATE_SV("SaveStateSelectorUI", "Save"));
  s_prev_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SelectPreviousSaveStateSlot"),
                                      TRANSLATE_SV("SaveStateSelectorUI", "Select Previous"));
  s_next_legend = format_legend_entry(Host::GetSmallStringSettingValue("Hotkeys", "SelectNextSaveStateSlot"),
                                      TRANSLATE_SV("SaveStateSelectorUI", "Select Next"));
}

void SaveStateSelectorUI::SelectNextSlot(bool open_selector)
{
  const s32 total_slots = s_current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
  s_current_slot++;
  if (s_current_slot >= total_slots)
  {
    s_current_slot -= total_slots;
    s_current_slot_global = !s_current_slot_global;
    if (System::GetGameSerial().empty() && !s_current_slot_global)
    {
      s_current_slot_global = false;
      s_current_slot = 0;
    }
  }

  if (open_selector)
  {
    if (!s_open)
      Open();

    s_open_time = 0.0f;
  }
  else
  {
    ShowSlotOSDMessage();
  }
}

void SaveStateSelectorUI::SelectPreviousSlot(bool open_selector)
{
  s_current_slot--;
  if (s_current_slot < 0)
  {
    s_current_slot_global = !s_current_slot_global;
    s_current_slot += s_current_slot_global ? System::GLOBAL_SAVE_STATE_SLOTS : System::PER_GAME_SAVE_STATE_SLOTS;
    if (System::GetGameSerial().empty() && !s_current_slot_global)
    {
      s_current_slot_global = false;
      s_current_slot = 0;
    }
  }

  if (open_selector)
  {
    if (!s_open)
      Open();

    s_open_time = 0.0f;
  }
  else
  {
    ShowSlotOSDMessage();
  }
}

void SaveStateSelectorUI::InitializeListEntry(ListEntry* li, ExtendedSaveStateInfo* ssi, const std::string& path,
                                              s32 slot, bool global)
{
  if (global)
    li->game_details = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "{} ({})"), ssi->title, ssi->serial);

  li->summary = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", DATE_TIME_FORMAT), fmt::localtime(ssi->timestamp));
  li->filename = Path::GetFileName(path);
  li->slot = slot;
  li->global = global;

  // Might not have a display yet, we're called at startup..
  if (g_gpu_device)
  {
    g_gpu_device->RecycleTexture(std::move(li->preview_texture));

    if (ssi->screenshot.IsValid())
    {
      li->preview_texture = g_gpu_device->FetchTexture(ssi->screenshot.GetWidth(), ssi->screenshot.GetHeight(), 1, 1, 1,
                                                       GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
                                                       ssi->screenshot.GetPixels(), ssi->screenshot.GetPitch());
      if (!li->preview_texture) [[unlikely]]
        ERROR_LOG("Failed to upload save state image to GPU");
    }
  }
}

void SaveStateSelectorUI::InitializePlaceholderListEntry(ListEntry* li, const std::string& path, s32 slot, bool global)
{
  li->summary = TRANSLATE_STR("SaveStateSelectorUI", "No save present in this slot.");
  li->slot = slot;
  li->global = global;
}

void SaveStateSelectorUI::Draw()
{
  static constexpr float SCROLL_ANIMATION_TIME = 0.25f;
  static constexpr float BG_ANIMATION_TIME = 0.15f;

  const auto& io = ImGui::GetIO();
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = (600.0f * scale);
  const float height = (420.0f * scale);

  const float padding_and_rounding = 15.0f * scale;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, padding_and_rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding_and_rounding, padding_and_rounding));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.15f, 0.17f, 0.8f));
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));

  if (ImGui::Begin("##save_state_selector", nullptr,
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar))
  {
    // Leave 2 lines for the legend
    const float legend_margin = ImGui::GetFontSize() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float padding = 10.0f * scale;

    ImGui::BeginChild("##item_list", ImVec2(0, -legend_margin), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoBackground);
    {
      const s32 current_slot = GetCurrentSlot();
      const bool current_slot_global = IsCurrentSlotGlobal();
      const ImVec2 image_size = ImVec2(128.0f * scale, (128.0f / (4.0f / 3.0f)) * scale);
      const float item_width = std::floor(width - (padding_and_rounding * 2.0f) - ImGui::GetStyle().ScrollbarSize);
      const float item_height = std::floor(image_size.y + padding * 2.0f);
      const float text_indent = image_size.x + padding + padding;

      for (size_t i = 0; i < s_slots.size(); i++)
      {
        const ListEntry& entry = s_slots[i];
        const float y_start = item_height * static_cast<float>(i);

        if (entry.slot == current_slot && entry.global == current_slot_global)
        {
          ImGui::SetCursorPosY(y_start);

          const ImVec2 p_start(ImGui::GetCursorScreenPos());
          const ImVec2 p_end(p_start.x + item_width, p_start.y + item_height);
          const ImRect item_rect(p_start, p_end);
          const ImRect& window_rect = ImGui::GetCurrentWindow()->ClipRect;
          if (!window_rect.Contains(item_rect))
          {
            float scroll_target = ImGui::GetScrollY();
            if (item_rect.Min.y < window_rect.Min.y)
              scroll_target = (ImGui::GetScrollY() - (window_rect.Min.y - item_rect.Min.y));
            else if (item_rect.Max.y > window_rect.Max.y)
              scroll_target = (ImGui::GetScrollY() + (item_rect.Max.y - window_rect.Max.y));

            if (scroll_target != s_scroll_animated.GetEndValue())
              s_scroll_animated.Start(ImGui::GetScrollY(), scroll_target, SCROLL_ANIMATION_TIME);
          }

          if (s_scroll_animated.IsActive())
            ImGui::SetScrollY(s_scroll_animated.UpdateAndGetValue());

          if (s_background_animated.GetEndValue() != p_start.y)
            s_background_animated.Start(s_background_animated.UpdateAndGetValue(), p_start.y, BG_ANIMATION_TIME);

          ImVec2 highlight_pos;
          if (s_background_animated.IsActive())
            highlight_pos = ImVec2(p_start.x, s_background_animated.UpdateAndGetValue());
          else
            highlight_pos = p_start;

          ImGui::GetWindowDrawList()->AddRectFilled(highlight_pos,
                                                    ImVec2(highlight_pos.x + item_width, highlight_pos.y + item_height),
                                                    ImColor(0.22f, 0.30f, 0.34f, 0.9f), padding_and_rounding);
        }

        if (GPUTexture* preview_texture =
              entry.preview_texture ? entry.preview_texture.get() : s_placeholder_texture.get())
        {
          ImGui::SetCursorPosY(y_start + padding);
          ImGui::SetCursorPosX(padding);
          ImGui::Image(preview_texture, image_size);
        }

        ImGui::SetCursorPosY(y_start + padding);

        ImGui::Indent(text_indent);

        ImGui::TextUnformatted(TinyString::from_format(entry.global ?
                                                         TRANSLATE_FS("SaveStateSelectorUI", "Global Slot {}") :
                                                         TRANSLATE_FS("SaveStateSelectorUI", "Game Slot {}"),
                                                       entry.slot)
                                 .c_str());
        if (entry.global)
          ImGui::TextUnformatted(entry.game_details.c_str(), entry.game_details.c_str() + entry.game_details.length());
        ImGui::TextUnformatted(entry.summary.c_str(), entry.summary.c_str() + entry.summary.length());
        ImGui::PushFont(ImGuiManager::GetFixedFont());
        ImGui::TextUnformatted(entry.filename.data(), entry.filename.data() + entry.filename.length());
        ImGui::PopFont();

        ImGui::Unindent(text_indent);
        ImGui::SetCursorPosY(y_start);
        ImGui::ItemSize(ImVec2(item_width, item_height));
      }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##legend", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    {
      ImGui::SetCursorPosX(padding);
      if (ImGui::BeginTable("table", 2))
      {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_load_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_prev_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_save_legend.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s_next_legend.c_str());

        ImGui::EndTable();
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  // auto-close
  s_open_time += io.DeltaTime;
  if (s_open_time >= s_close_time)
    Close();
}

s32 SaveStateSelectorUI::GetCurrentSlot()
{
  return s_current_slot + 1;
}

bool SaveStateSelectorUI::IsCurrentSlotGlobal()
{
  return s_current_slot_global;
}

std::string SaveStateSelectorUI::GetCurrentSlotPath()
{
  std::string filename;
  if (!s_current_slot_global)
  {
    if (const std::string& serial = System::GetGameSerial(); !serial.empty())
      filename = System::GetGameSaveStateFileName(serial, s_current_slot + 1);
  }
  else
  {
    filename = System::GetGlobalSaveStateFileName(s_current_slot + 1);
  }

  return filename;
}

void SaveStateSelectorUI::LoadCurrentSlot()
{
  if (std::string path = GetCurrentSlotPath(); !path.empty())
  {
    if (FileSystem::FileExists(path.c_str()))
    {
      Error error;
      if (!System::LoadState(path.c_str(), &error, true))
      {
        Host::AddKeyedOSDMessage("LoadState",
                                 fmt::format(TRANSLATE_FS("OSDMessage", "Failed to load state from slot {0}:\n{1}"),
                                             GetCurrentSlot(), error.GetDescription()),
                                 Host::OSD_ERROR_DURATION);
      }
    }
    else
    {
      Host::AddIconOSDMessage(
        "LoadState", ICON_FA_SD_CARD,
        IsCurrentSlotGlobal() ?
          fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "No save state found in Global Slot {}."), GetCurrentSlot()) :
          fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "No save state found in Slot {}."), GetCurrentSlot()),
        Host::OSD_INFO_DURATION);
    }
  }

  Close();
}

void SaveStateSelectorUI::SaveCurrentSlot()
{
  if (std::string path = GetCurrentSlotPath(); !path.empty())
  {
    Error error;
    if (!System::SaveState(path.c_str(), &error, g_settings.create_save_state_backups))
    {
      Host::AddIconOSDMessage("SaveState", ICON_FA_EXCLAMATION_TRIANGLE,
                              fmt::format(TRANSLATE_FS("OSDMessage", "Failed to save state to slot {0}:\n{1}"),
                                          GetCurrentSlot(), error.GetDescription()),
                              Host::OSD_ERROR_DURATION);
    }
  }

  Close();
}

void SaveStateSelectorUI::ShowSlotOSDMessage()
{
  const std::string path = GetCurrentSlotPath();
  FILESYSTEM_STAT_DATA sd;
  std::string date;
  if (!path.empty() && FileSystem::StatFile(path.c_str(), &sd))
    date = fmt::format(TRANSLATE_FS("SaveStateSelectorUI", DATE_TIME_FORMAT), fmt::localtime(sd.ModificationTime));
  else
    date = TRANSLATE_STR("SaveStateSelectorUI", "no save yet");

  Host::AddIconOSDMessage(
    "ShowSlotOSDMessage", ICON_FA_SEARCH,
    IsCurrentSlotGlobal() ?
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Global Save Slot {0} selected ({1})."), GetCurrentSlot(), date) :
      fmt::format(TRANSLATE_FS("SaveStateSelectorUI", "Save Slot {0} selected ({1})."), GetCurrentSlot(), date),
    Host::OSD_QUICK_DURATION);
}

void ImGuiManager::RenderOverlayWindows()
{
  const System::State state = System::GetState();
  if (state != System::State::Shutdown)
  {
    if (SaveStateSelectorUI::s_open)
      SaveStateSelectorUI::Draw();
  }
}

void ImGuiManager::DestroyOverlayTextures()
{
  SaveStateSelectorUI::DestroyTextures();
}
