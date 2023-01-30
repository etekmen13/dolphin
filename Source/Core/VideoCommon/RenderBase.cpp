// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The GPU draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------

#include "VideoCommon/RenderBase.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FrameDumper.h"
 #include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

std::unique_ptr<Renderer> g_renderer;

Renderer::Renderer()
    : m_prev_efb_format{PixelFormat::INVALID_FMT},
       m_last_xfb_width{MAX_XFB_WIDTH}, m_last_xfb_height{MAX_XFB_HEIGHT}
{
  CalculateTargetSize();
  UpdateWidescreen();

  m_config_changed_handle = ConfigChangedEvent::Register([this](u32 bits) { OnConfigChanged(bits); }, "Renderer");

  // VertexManager doesn't maintain statistics in Wii mode.
  if (!SConfig::GetInstance().bWii)
    m_update_widescreen_handle = AfterFrameEvent::Register([this] { UpdateWidescreenHeuristic(); }, "WideScreen Heuristic");
}

Renderer::~Renderer() = default;

void Renderer::ClearScreen(const MathUtil::Rectangle<int>& rc, bool color_enable, bool alpha_enable,
                           bool z_enable, u32 color, u32 z)
{
  g_framebuffer_manager->FlushEFBPokes();
  g_framebuffer_manager->FlagPeekCacheAsOutOfDate();

  // Native -> EFB coordinates
  MathUtil::Rectangle<int> target_rc = Renderer::ConvertEFBRectangle(rc);
  target_rc.ClampUL(0, 0, m_target_width, m_target_height);

  // Determine whether the EFB has an alpha channel. If it doesn't, we can clear the alpha
  // channel to 0xFF.
  // On backends that don't allow masking Alpha clears, this allows us to use the fast path
  // almost all the time
  if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16 ||
      bpmem.zcontrol.pixel_format == PixelFormat::RGB8_Z24 ||
      bpmem.zcontrol.pixel_format == PixelFormat::Z24)
  {
    // Force alpha writes, and clear the alpha channel.
    alpha_enable = true;
    color &= 0x00FFFFFF;
  }

  g_gfx->ClearRegion(rc, target_rc, color_enable, alpha_enable, z_enable, color, z);

  // Scissor rect must be restored.
  BPFunctions::SetScissorAndViewport();
}

void Renderer::ReinterpretPixelData(EFBReinterpretType convtype)
{
  g_framebuffer_manager->ReinterpretPixelData(convtype);
}

u32 Renderer::AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data)
{
  if (type == EFBAccessType::PeekColor)
  {
    u32 color = g_framebuffer_manager->PeekEFBColor(x, y);

    // a little-endian value is expected to be returned
    color = ((color & 0xFF00FF00) | ((color >> 16) & 0xFF) | ((color << 16) & 0xFF0000));

    if (bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24)
    {
      color = RGBA8ToRGBA6ToRGBA8(color);
    }
    else if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16)
    {
      color = RGBA8ToRGB565ToRGBA8(color);
    }
    if (bpmem.zcontrol.pixel_format != PixelFormat::RGBA6_Z24)
    {
      color |= 0xFF000000;
    }

    // check what to do with the alpha channel (GX_PokeAlphaRead)
    PixelEngine::AlphaReadMode alpha_read_mode =
        Core::System::GetInstance().GetPixelEngine().GetAlphaReadMode();

    if (alpha_read_mode == PixelEngine::AlphaReadMode::ReadNone)
    {
      return color;
    }
    else if (alpha_read_mode == PixelEngine::AlphaReadMode::ReadFF)
    {
      return color | 0xFF000000;
    }
    else
    {
      if (alpha_read_mode != PixelEngine::AlphaReadMode::Read00)
      {
        PanicAlertFmt("Invalid PE alpha read mode: {}", static_cast<u16>(alpha_read_mode));
      }
      return color & 0x00FFFFFF;
    }
  }
  else  // if (type == EFBAccessType::PeekZ)
  {
    // Depth buffer is inverted for improved precision near far plane
    float depth = g_framebuffer_manager->PeekEFBDepth(x, y);
    if (!g_ActiveConfig.backend_info.bSupportsReversedDepthRange)
      depth = 1.0f - depth;

    // Convert to 24bit depth
    u32 z24depth = std::clamp<u32>(static_cast<u32>(depth * 16777216.0f), 0, 0xFFFFFF);

    if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16)
    {
      // When in RGB565_Z16 mode, EFB Z peeks return a 16bit value, which is presumably a
      // resolved sample from the MSAA buffer.
      // Dolphin doesn't currently emulate the 3 sample MSAA mode (and potentially never will)
      // it just transparently upgrades the framebuffer to 24bit depth and color and whatever
      // level of MSAA and higher Internal Resolution the user has configured.

      // This is mostly transparent, unless the game does an EFB read.
      // But we can simply convert the 24bit depth on the fly to the 16bit depth the game expects.

      return CompressZ16(z24depth, bpmem.zcontrol.zformat);
    }

    return z24depth;
  }
}

void Renderer::PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points)
{
  if (type == EFBAccessType::PokeColor)
  {
    for (size_t i = 0; i < num_points; i++)
    {
      // Convert to expected format (BGRA->RGBA)
      // TODO: Check alpha, depending on mode?
      const EfbPokeData& point = points[i];
      u32 color = ((point.data & 0xFF00FF00) | ((point.data >> 16) & 0xFF) |
                   ((point.data << 16) & 0xFF0000));
      g_framebuffer_manager->PokeEFBColor(point.x, point.y, color);
    }
  }
  else  // if (type == EFBAccessType::PokeZ)
  {
    for (size_t i = 0; i < num_points; i++)
    {
      // Convert to floating-point depth.
      const EfbPokeData& point = points[i];
      float depth = float(point.data & 0xFFFFFF) / 16777216.0f;
      if (!g_ActiveConfig.backend_info.bSupportsReversedDepthRange)
        depth = 1.0f - depth;

      g_framebuffer_manager->PokeEFBDepth(point.x, point.y, depth);
    }
  }
}

unsigned int Renderer::GetEFBScale() const
{
  return m_efb_scale;
}

int Renderer::EFBToScaledX(int x) const
{
  return x * static_cast<int>(m_efb_scale);
}

int Renderer::EFBToScaledY(int y) const
{
  return y * static_cast<int>(m_efb_scale);
}

float Renderer::EFBToScaledXf(float x) const
{
  return x * ((float)GetTargetWidth() / (float)EFB_WIDTH);
}

float Renderer::EFBToScaledYf(float y) const
{
  return y * ((float)GetTargetHeight() / (float)EFB_HEIGHT);
}

std::tuple<int, int> Renderer::CalculateTargetScale(int x, int y) const
{
  return std::make_tuple(x * static_cast<int>(m_efb_scale), y * static_cast<int>(m_efb_scale));
}

// return true if target size changed
bool Renderer::CalculateTargetSize()
{
  if (g_ActiveConfig.iEFBScale == EFB_SCALE_AUTO_INTEGRAL)
  {
    auto target_rectangle = g_presenter->GetTargetRectangle();
    // Set a scale based on the window size
    int width = EFB_WIDTH * target_rectangle.GetWidth() / m_last_xfb_width;
    int height = EFB_HEIGHT * target_rectangle.GetHeight() / m_last_xfb_height;
    m_efb_scale = std::max((width - 1) / EFB_WIDTH + 1, (height - 1) / EFB_HEIGHT + 1);
  }
  else
  {
    m_efb_scale = g_ActiveConfig.iEFBScale;
  }

  const u32 max_size = g_ActiveConfig.backend_info.MaxTextureSize;
  if (max_size < EFB_WIDTH * m_efb_scale)
    m_efb_scale = max_size / EFB_WIDTH;

  auto [new_efb_width, new_efb_height] = CalculateTargetScale(EFB_WIDTH, EFB_HEIGHT);
  new_efb_width = std::max(new_efb_width, 1);
  new_efb_height = std::max(new_efb_height, 1);

  if (new_efb_width != m_target_width || new_efb_height != m_target_height)
  {
    m_target_width = new_efb_width;
    m_target_height = new_efb_height;
    auto& system = Core::System::GetInstance();
    auto& pixel_shader_manager = system.GetPixelShaderManager();
    pixel_shader_manager.SetEfbScaleChanged(EFBToScaledXf(1), EFBToScaledYf(1));
    return true;
  }
  return false;
}


MathUtil::Rectangle<int> Renderer::ConvertEFBRectangle(const MathUtil::Rectangle<int>& rc) const
{
  MathUtil::Rectangle<int> result;
  result.left = EFBToScaledX(rc.left);
  result.top = EFBToScaledY(rc.top);
  result.right = EFBToScaledX(rc.right);
  result.bottom = EFBToScaledY(rc.bottom);
  return result;
}

void Renderer::UpdateWidescreen()
{
  if (SConfig::GetInstance().bWii)
    m_is_game_widescreen = Config::Get(Config::SYSCONF_WIDESCREEN);

  // suggested_aspect_mode overrides SYSCONF_WIDESCREEN
  if (g_ActiveConfig.suggested_aspect_mode == AspectMode::Analog)
    m_is_game_widescreen = false;
  else if (g_ActiveConfig.suggested_aspect_mode == AspectMode::AnalogWide)
    m_is_game_widescreen = true;

  // If widescreen hack is disabled override game's AR if UI is set to 4:3 or 16:9.
  if (!g_ActiveConfig.bWidescreenHack)
  {
    const auto aspect_mode = g_ActiveConfig.aspect_mode;
    if (aspect_mode == AspectMode::Analog)
      m_is_game_widescreen = false;
    else if (aspect_mode == AspectMode::AnalogWide)
      m_is_game_widescreen = true;
  }
}

// Heuristic to detect if a GameCube game is in 16:9 anamorphic widescreen mode.
void Renderer::UpdateWidescreenHeuristic()
{
  const auto flush_statistics = g_vertex_manager->ResetFlushAspectRatioCount();

  // If suggested_aspect_mode (GameINI) is configured don't use heuristic.
  if (g_ActiveConfig.suggested_aspect_mode != AspectMode::Auto)
    return;

  UpdateWidescreen();

  // If widescreen hack isn't active and aspect_mode (UI) is 4:3 or 16:9 don't use heuristic.
  if (!g_ActiveConfig.bWidescreenHack && (g_ActiveConfig.aspect_mode == AspectMode::Analog ||
                                          g_ActiveConfig.aspect_mode == AspectMode::AnalogWide))
    return;

  // Modify the threshold based on which aspect ratio we're already using:
  // If the game's in 4:3, it probably won't switch to anamorphic, and vice-versa.
  static constexpr u32 TRANSITION_THRESHOLD = 3;

  const auto looks_normal = [](auto& counts) {
    return counts.normal_vertex_count > counts.anamorphic_vertex_count * TRANSITION_THRESHOLD;
  };
  const auto looks_anamorphic = [](auto& counts) {
    return counts.anamorphic_vertex_count > counts.normal_vertex_count * TRANSITION_THRESHOLD;
  };

  const auto& persp = flush_statistics.perspective;
  const auto& ortho = flush_statistics.orthographic;

  const auto ortho_looks_anamorphic = looks_anamorphic(ortho);

  if (looks_anamorphic(persp) || ortho_looks_anamorphic)
  {
    // If either perspective or orthographic projections look anamorphic, it's a safe bet.
    m_is_game_widescreen = true;
  }
  else if (looks_normal(persp) || (m_was_orthographically_anamorphic && looks_normal(ortho)))
  {
    // Many widescreen games (or AR/GeckoCodes) use anamorphic perspective projections
    // with NON-anamorphic orthographic projections.
    // This can cause incorrect changes to 4:3 when perspective projections are temporarily not
    // shown. e.g. Animal Crossing's inventory menu.
    // Unless we were in a situation which was orthographically anamorphic
    // we won't consider orthographic data for changes from 16:9 to 4:3.
    m_is_game_widescreen = false;
  }

  m_was_orthographically_anamorphic = ortho_looks_anamorphic;
}

void Renderer::OnConfigChanged(u32 bits)
{
  if (bits & CONFIG_CHANGE_BIT_ASPECT_RATIO)
    UpdateWidescreen();
}

void Renderer::TrackSwaps(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks)
{
  if (xfb_addr && fb_width && fb_stride && fb_height)
  {
    // Update our last xfb values
    m_last_xfb_addr = xfb_addr;
    m_last_xfb_ticks = ticks;
    m_last_xfb_width = fb_width;
    m_last_xfb_stride = fb_stride;
    m_last_xfb_height = fb_height;
  }
}

bool Renderer::UseVertexDepthRange() const
{
  // We can't compute the depth range in the vertex shader if we don't support depth clamp.
  if (!g_ActiveConfig.backend_info.bSupportsDepthClamp)
    return false;

  // We need a full depth range if a ztexture is used.
  if (bpmem.ztex2.op != ZTexOp::Disabled && !bpmem.zcontrol.early_ztest)
    return true;

  // If an inverted depth range is unsupported, we also need to check if the range is inverted.
  if (!g_ActiveConfig.backend_info.bSupportsReversedDepthRange && xfmem.viewport.zRange < 0.0f)
    return true;

  // If an oversized depth range or a ztexture is used, we need to calculate the depth range
  // in the vertex shader.
  return fabs(xfmem.viewport.zRange) > 16777215.0f || fabs(xfmem.viewport.farZ) > 16777215.0f;
}

void Renderer::DoState(PointerWrap& p)
{
  p.Do(m_is_game_widescreen);
  p.Do(m_frame_count);
  p.Do(m_prev_efb_format);
  p.Do(m_last_xfb_ticks);
  p.Do(m_last_xfb_addr);
  p.Do(m_last_xfb_width);
  p.Do(m_last_xfb_stride);
  p.Do(m_last_xfb_height);

  g_bounding_box->DoState(p);

  if (p.IsReadMode())
  {
    m_was_orthographically_anamorphic = false;

    // This technically counts as the end of the frame
    AfterFrameEvent::Trigger();

    // re-display the most recent XFB
    g_presenter->ImmediateSwap(m_last_xfb_addr, m_last_xfb_width, m_last_xfb_stride,
                               m_last_xfb_height, m_last_xfb_ticks);
  }

#if defined(HAVE_FFMPEG)
  g_frame_dumper->DoState(p);
#endif
}
