#pragma once

#include "common.h"
#include "renderer.h"
#include <windows.h>
#include <d3d9.h>

// ===[ D3D9Renderer Struct ]===
// Mirrors GLRenderer layout/intent, but with D3D9 resources.
typedef struct {
    Renderer base; // Must be first field for struct embedding

    // Device + default RT
    IDirect3DDevice9* device;
    IDirect3DSurface9* defaultColorRT; // backbuffer

    // "Shader" state equivalents (fixed function)
    bool alphaTestEnable;
    float alphaTestRef;
    bool fogEnable;
    uint32_t fogColor; // BGR

    // Dynamic quad batch
    IDirect3DVertexBuffer9* vb;
    IDirect3DIndexBuffer9* ib;
    int32_t quadCount;
    int32_t maxQuads;

    // Current texture
    IDirect3DTexture9* currentTexture;

    // Texture pages (TXTR)
    IDirect3DTexture9** d3dTextures; // one D3D texture per TXTR page
    int32_t* textureWidths;
    int32_t* textureHeights;
    bool* textureLoaded;
    uint32_t textureCount;

    // 1x1 white texture
    IDirect3DTexture9* whiteTexture;

    // Offscreen render target (FBO equivalent)
    IDirect3DTexture9* rtTexture;
    IDirect3DSurface9* rtSurface;
    int32_t rtWidth;
    int32_t rtHeight;
    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;

int bbWidth;
int bbHeight;


    // Original counts from data.win
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    // Surfaces (GM surfaces)
    uint32_t surfaceCount;
    IDirect3DSurface9** surfaces;
    IDirect3DTexture9** surfaceTexture;
    int32_t* surfaceWidth;
    int32_t* surfaceHeight;
    uint32_t ssurfaceCount;
    int32_t surfaceStack[16];

    // CPU-side vertex buffer (same layout as GL)
    float* vertexData; // MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX


} D3D9Renderer;

Renderer* D3D9Renderer_create(IDirect3DDevice9* device);


