#pragma once
#include <Windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <d3d11.h>
/*
    Emre Tekmen
    shared handle and memory region for IPC with RL program.
*/
using Microsoft::WRL::ComPtr;

extern HANDLE hMapFile;
extern HANDLE shared_handle;
extern Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
extern HANDLE hSharedEvent;
extern Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;

struct SharedTextureData
{
  HANDLE handle;
  UINT width;
  UINT height;
};
