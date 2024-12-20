// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DGfx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <strsafe.h>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/DXPipeline.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

#include <tinygltf/stb_image_write.h>
#include "!Emre-TextureShare.h"
HANDLE shared_handle;
ComPtr<IDXGIResource> dxgi_resource;
ComPtr<ID3D11Texture2D> backBuffer;
namespace DX11
{
Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
}

Gfx::~Gfx() = default;

bool Gfx::IsHeadless() const
{
  return !m_swap_chain;
}

std::unique_ptr<AbstractTexture> Gfx::CreateTexture(const TextureConfig& config,
                                                    std::string_view name)
{
  return DXTexture::Create(config, name);
}

std::unique_ptr<AbstractStagingTexture> Gfx::CreateStagingTexture(StagingTextureType type,
                                                                  const TextureConfig& config)
{
  return DXStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractFramebuffer>
Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                       std::vector<AbstractTexture*> additional_color_attachments)
{
  return DXFramebuffer::Create(static_cast<DXTexture*>(color_attachment),
                               static_cast<DXTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader>
Gfx::CreateShaderFromSource(ShaderStage stage, std::string_view source, std::string_view name)
{
  auto bytecode = DXShader::CompileShader(D3D::feature_level, stage, source);
  if (!bytecode)
    return nullptr;

  return DXShader::CreateFromBytecode(stage, std::move(*bytecode), name);
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                            size_t length, std::string_view name)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, length), name);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                      const void* cache_data,
                                                      size_t cache_data_length)
{
  return DXPipeline::Create(config);
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  const DXPipeline* dx_pipeline = static_cast<const DXPipeline*>(pipeline);
  if (m_current_pipeline == dx_pipeline)
    return;

  if (dx_pipeline)
  {
    D3D::stateman->SetRasterizerState(dx_pipeline->GetRasterizerState());
    D3D::stateman->SetDepthState(dx_pipeline->GetDepthState());
    D3D::stateman->SetBlendState(dx_pipeline->GetBlendState());
    D3D::stateman->SetPrimitiveTopology(dx_pipeline->GetPrimitiveTopology());
    D3D::stateman->SetInputLayout(dx_pipeline->GetInputLayout());
    D3D::stateman->SetVertexShader(dx_pipeline->GetVertexShader());
    D3D::stateman->SetGeometryShader(dx_pipeline->GetGeometryShader());
    D3D::stateman->SetPixelShader(dx_pipeline->GetPixelShader());
    D3D::stateman->SetIntegerRTV(dx_pipeline->UseLogicOp());
  }
  else
  {
    // These will be destroyed at pipeline destruction.
    D3D::stateman->SetInputLayout(nullptr);
    D3D::stateman->SetVertexShader(nullptr);
    D3D::stateman->SetGeometryShader(nullptr);
    D3D::stateman->SetPixelShader(nullptr);
  }
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  // TODO: Move to stateman
  const CD3D11_RECT rect(rc.left, rc.top, std::max(rc.right, rc.left + 1),
                         std::max(rc.bottom, rc.top + 1));
  D3D::context->RSSetScissorRects(1, &rect);
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  // TODO: Move to stateman
  const CD3D11_VIEWPORT vp(x, y, width, height, near_depth, far_depth);
  D3D::context->RSSetViewports(1, &vp);
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  D3D::stateman->Apply();
  D3D::context->Draw(num_vertices, base_vertex);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  D3D::stateman->Apply();
  D3D::context->DrawIndexed(num_indices, base_index, base_vertex);
}

void Gfx::DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                                u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z)
{
  D3D::stateman->SetComputeShader(static_cast<const DXShader*>(shader)->GetD3DComputeShader());
  D3D::stateman->SyncComputeBindings();
  D3D::context->Dispatch(groups_x, groups_y, groups_z);
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
  return true;
}
void StageandSavePNG(ComPtr<ID3D11Texture2D> shared_texture, UINT width, UINT height)
{
  ComPtr<ID3D11Texture2D> staging_texture;
  D3D11_TEXTURE2D_DESC staging_desc;
  shared_texture->GetDesc(&staging_desc);
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;  // Staging textures cannot have bind flags
  staging_desc.MiscFlags = 0;  // No special miscellaneous flags

  HRESULT hr = D3D::device->CreateTexture2D(&staging_desc, nullptr, staging_texture.GetAddressOf());
  if (FAILED(hr))
  {
    // std::cerr << "Failed to create staging texture: " << std::hex << hr << std::endl;
    return;
  }
  D3D::context->CopyResource(staging_texture.Get(), shared_texture.Get());
    D3D::context->Flush();  // Ensure all GPU commands are executed
  // Map the staging texture to CPU memory
  D3D11_MAPPED_SUBRESOURCE mappedResource;
  hr = D3D::context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "Failed to map staging texture: ");
    return;
  }
  BYTE* data = reinterpret_cast<BYTE*>(mappedResource.pData);
  if (!data)
  {
    NOTICE_LOG_FMT(VIDEO, "Failed to allocate CPU buffer");
    D3D::context->Unmap(staging_texture.Get(), 0);
    return;
  }

  if (!stbi_write_png("before_cuda.png", width, height, 4, data, mappedResource.RowPitch))
  {
    NOTICE_LOG_FMT(VIDEO, "failed to write DX11 as PNG");
  }
  // Unmap and release the staging texture
  D3D::context->Unmap(staging_texture.Get(), 0);
  staging_texture.Reset();
}
void Gfx::PresentBackbuffer()
{


#pragma region
  /*
    Emre Tekmen
    Set the handle and write to shared memory
    */
  HRESULT hr = m_swap_chain->GetDXGISwapChain()->GetBuffer(
      0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf()));
  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "Failed to get backbuffer");
    return;
  }
  D3D11_TEXTURE2D_DESC desc;
  ComPtr<ID3D11Texture2D> shared_texture;

  backBuffer->GetDesc(&desc);

  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  hr = D3D::device->CreateTexture2D(&desc, nullptr, shared_texture.GetAddressOf());

  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "Failed to create shared texture");
    return;
  }
  D3D::context->CopyResource(shared_texture.Get(), backBuffer.Get());

 //  NOTICE_LOG_FMT(VIDEO, "BackBuffer is unorm format: {}", (UINT)desc.Format);
  UINT width = desc.Width;
  UINT height = desc.Height;

  hr = shared_texture.As(&dxgi_resource);
  //NOTICE_LOG_FMT(VIDEO, "test 1");

  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "backbuffer.As has failed, HRESULT {}", hr);
    return;
  }
  NOTICE_LOG_FMT(VIDEO, "saving...");
  StageandSavePNG(shared_texture, width, height);

  hr = dxgi_resource->GetSharedHandle(&shared_handle);
  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "Failed to get shared handle. HRESULT: {}", hr);
    return;
  }
  void* pMappedMem = MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, 0,
                                   32);  // map 32 bytes into the virtual address space

  struct SharedTextureData sd;
  sd.handle = shared_handle;
  sd.width = width;
  sd.height = height;
 // NOTICE_LOG_FMT(VIDEO, "handle: {} width {} height {}", sd.handle, sd.width, sd.height);
  if (pMappedMem)
  {
    std::memcpy(pMappedMem, &sd, sizeof(SharedTextureData));
  }
  //NOTICE_LOG_FMT(VIDEO, "waiting");
  D3D::context->Flush();
  SetEvent(hSharedEvent);
  hr = WaitForSingleObject(hSharedEvent, INFINITE);
  if (FAILED(hr))
  {
    NOTICE_LOG_FMT(VIDEO, "Wait Failed, HRESULT: {}, GetLastError: {}", hr, GetLastError());
  }

  if(pMappedMem) {
    UnmapViewOfFile(pMappedMem);
  }
  shared_texture.Reset();
  dxgi_resource.Reset();

  /*
    End
  */
#pragma endregion

  m_swap_chain->Present();
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  // Quad-buffer changes require swap chain recreation.
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE && m_swap_chain)
    m_swap_chain->SetStereo(SwapChain::WantsStereo());

  if (bits & CONFIG_CHANGE_BIT_HDR && m_swap_chain)
    m_swap_chain->SetHDR(SwapChain::WantsHDR());
}

void Gfx::CheckForSwapChainChanges()
{
  const bool surface_changed = g_presenter->SurfaceChangedTestAndClear();
  const bool surface_resized =
      g_presenter->SurfaceResizedTestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!surface_changed && !surface_resized)
    return;

  if (surface_changed)
  {
    m_swap_chain->ChangeSurface(g_presenter->GetNewSurfaceHandle());
  }
  else
  {
    m_swap_chain->ResizeSwapChain();
  }

  g_presenter->SetBackbuffer(m_swap_chain->GetWidth(), m_swap_chain->GetHeight());
}

void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  // We can't leave the framebuffer bound as a texture and a render target.
  DXFramebuffer* fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->Unbind();

  D3D::stateman->SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  SetFramebuffer(framebuffer);
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  SetFramebuffer(framebuffer);
  D3D::stateman->Apply();

  DXFramebuffer* fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->Clear(color_value, depth_value);
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  D3D::stateman->SetTexture(index, texture ? static_cast<const DXTexture*>(texture)->GetD3DSRV() :
                                             nullptr);
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  D3D::stateman->SetSampler(index, m_state_cache.Get(state));
}

void Gfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  D3D::stateman->SetComputeUAV(index,
                               texture ? static_cast<DXTexture*>(texture)->GetD3DUAV() : nullptr);
}

void Gfx::UnbindTexture(const AbstractTexture* texture)
{
  if (D3D::stateman->UnsetTexture(static_cast<const DXTexture*>(texture)->GetD3DSRV()) != 0)
    D3D::stateman->ApplyTextures();
}

void Gfx::Flush()
{
  D3D::context->Flush();
}

void Gfx::WaitForGPUIdle()
{
  // There is no glFinish() equivalent in D3D.
  D3D::context->Flush();
}

void Gfx::SetFullscreen(bool enable_fullscreen)
{
  if (m_swap_chain)
    m_swap_chain->SetFullscreen(enable_fullscreen);
}

bool Gfx::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetFullscreen();
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  return {m_swap_chain ? static_cast<u32>(m_swap_chain->GetWidth()) : 0,
          m_swap_chain ? static_cast<u32>(m_swap_chain->GetHeight()) : 0, m_backbuffer_scale,
          m_swap_chain ? m_swap_chain->GetFormat() : AbstractTextureFormat::Undefined};
}

}  // namespace DX11
