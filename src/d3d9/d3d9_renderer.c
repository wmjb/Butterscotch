#include "d3d9_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "image_decoder.h"
#include "utils.h"
#include "stb_ds.h"

#include <math.h>
#include <string.h>

#define MAX_QUADS 4096
#define FLOATS_PER_VERTEX 8  // x, y, u, v, r, g, b, a
#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

typedef struct {
    float x, y, z, rhw;   // XYZRHW
    uint32_t color;       // DIFFUSE
    float u, v;           // TEX1
} D3D9Vertex;

#define D3D9_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_DIFFUSE)

// ===[ Batch Flush ]===

static void d3d9_flushBatch(D3D9Renderer* dr) {
    if (dr->quadCount == 0)
        return;

    int32_t vertexCount = dr->quadCount * VERTICES_PER_QUAD;
    int32_t indexCount  = dr->quadCount * INDICES_PER_QUAD;

    // Guard: texture must exist
    if (!dr->currentTexture) {
        fprintf(stderr,
            "D3D9: flushBatch ERROR — currentTexture=NULL (quads=%d)\n",
            dr->quadCount);
        dr->quadCount = 0;
        return;
    }

    // Lock VB
    void* vbPtr = NULL;
    HRESULT hr = IDirect3DVertexBuffer9_Lock(
        dr->vb, 0,
        vertexCount * sizeof(D3D9Vertex),
        &vbPtr, D3DLOCK_DISCARD
    );

    if (FAILED(hr)) {
        fprintf(stderr,
            "D3D9: flushBatch VB Lock FAILED hr=0x%08lx (quads=%d)\n",
            hr, dr->quadCount);
        dr->quadCount = 0;
        return;
    }

    // Copy vertices
    D3D9Vertex* dst = (D3D9Vertex*)vbPtr;
    float* src = dr->vertexData;

    for (int32_t i = 0; i < vertexCount; ++i) {
        float x = src[0];
        float y = src[1];
        float u = src[2];
        float v = src[3];
        float r = src[4];
        float g = src[5];
        float b = src[6];
        float a = src[7];

        dst[i].x   = x;
        dst[i].y   = y;
        dst[i].z   = 0.0f;
        dst[i].rhw = 1.0f;

        uint8_t R = (uint8_t)(r * 255.0f);
        uint8_t G = (uint8_t)(g * 255.0f);
        uint8_t B = (uint8_t)(b * 255.0f);
        uint8_t A = (uint8_t)(a * 255.0f);
        dst[i].color = D3DCOLOR_ARGB(A, R, G, B);

        dst[i].u = u;
        dst[i].v = v;

        src += FLOATS_PER_VERTEX;
    }

    IDirect3DVertexBuffer9_Unlock(dr->vb);

    // Bind VB/IB
    IDirect3DDevice9_SetStreamSource(
        dr->device, 0, dr->vb, 0, sizeof(D3D9Vertex));

    IDirect3DDevice9_SetFVF(dr->device, D3D9_FVF);

    IDirect3DDevice9_SetTexture(
        dr->device, 0, (IDirect3DBaseTexture9*)dr->currentTexture);

    IDirect3DDevice9_SetIndices(dr->device, dr->ib);

    // Diagnostic logging before draw

/*
    fprintf(stderr,
        "D3D9: flushBatch DRAW quads=%d verts=%d idx=%d tex=%p\n",
        dr->quadCount, vertexCount, indexCount,
        (void*)dr->currentTexture);
*/

    hr = IDirect3DDevice9_DrawIndexedPrimitive(
        dr->device,
        D3DPT_TRIANGLELIST,
        0, 0, vertexCount,
        0, indexCount / 3
    );
/*
    fprintf(stderr,
        "D3D9: flushBatch DrawIndexedPrimitive hr=0x%08lx\n", hr);
*/
    dr->quadCount = 0;
}

// ===[ Texture helpers ]===

static bool d3d9_ensureTextureLoaded(D3D9Renderer* dr, uint32_t pageId) {
    if (dr->textureLoaded[pageId]) return (dr->textureWidths[pageId] != 0);

    dr->textureLoaded[pageId] = true;

    DataWin* dw   = dr->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    int w, h;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(
        txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h
    );
    if (!pixels) {
        fprintf(stderr, "D3D9: Failed to decode TXTR page %u\n", pageId);
        return false;
    }

    dr->textureWidths[pageId]  = w;
    dr->textureHeights[pageId] = h;

    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(
            dr->device, w, h, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &tex, NULL))) {
        free(pixels);
        fprintf(stderr, "D3D9: CreateTexture failed for TXTR page %u\n", pageId);
        return false;
    }

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
        uint8_t* dst = (uint8_t*)lr.pBits;
        uint8_t* src = pixels;

        for (int y = 0; y < h; ++y) {
            uint8_t* d = dst + y * lr.Pitch;
            uint8_t* s = src + y * w * 4;

            for (int x = 0; x < w; ++x) {
                uint8_t r = s[0];
                uint8_t g = s[1];
                uint8_t b = s[2];
                uint8_t a = s[3];

                if (a == 0) {
                    // Kill padding colour on fully transparent pixels
                    d[0] = 0;
                    d[1] = 0;
                    d[2] = 0;
                    d[3] = 0;
                } else {
                    // Straight BGRA for visible pixels
                    d[0] = b;
                    d[1] = g;
                    d[2] = r;
                    d[3] = a;
                }

                s += 4;
                d += 4;
            }
        }

        IDirect3DTexture9_UnlockRect(tex, 0);
    }

    free(pixels);
    dr->d3dTextures[pageId] = tex;

    fprintf(stderr, "D3D9: Loaded TXTR page %u (%dx%d)\n", pageId, w, h);
    return true;
}



static bool d3d9_resolveSpriteTexture(D3D9Renderer* dr, int32_t tpagIndex,
                                      TexturePageItem** outTpag,
                                      IDirect3DTexture9** outTex,
                                      int32_t* outTexW, int32_t* outTexH) {
    DataWin* dw = dr->base.dataWin;
    if (tpagIndex < 0 || dw->tpag.count <= (uint32_t)tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || dr->textureCount <= (uint32_t)pageId) return false;
    if (!d3d9_ensureTextureLoaded(dr, (uint32_t)pageId)) return false;

    *outTpag = tpag;
    *outTex  = dr->d3dTextures[pageId];
    *outTexW = dr->textureWidths[pageId];
    *outTexH = dr->textureHeights[pageId];
    return true;
}

static void d3d9_emitTexturedQuad(
    D3D9Renderer* dr,
    IDirect3DTexture9* tex,
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float x3, float y3,
    float u0, float v0,
    float u1, float v1,
    float r, float g, float b, float alpha
) {
    if (!dr) {
       fprintf(stderr, "D3D9: emitTexturedQuad called with NULL renderer\n");
        return;
    }

    if (!dr->vertexData) {
       fprintf(stderr, "D3D9: emitTexturedQuad: vertexData is NULL\n");
        return;
    }

    if (dr->quadCount < 0 || dr->quadCount > MAX_QUADS) {
        fprintf(stderr, "D3D9: emitTexturedQuad: quadCount out of range: %d (MAX_QUADS=%d)\n",
                dr->quadCount, MAX_QUADS);
        dr->quadCount = 0;
    }

    if (dr->quadCount > 0 && dr->currentTexture != tex) {
    //    fprintf(stderr, "D3D9: emitTexturedQuad: texture change %p -> %p, flushing\n",
    //            (void*)dr->currentTexture, (void*)tex);
        d3d9_flushBatch(dr);
    }

    if (dr->quadCount >= MAX_QUADS) {
    //    fprintf(stderr, "D3D9: emitTexturedQuad: quadCount=%d >= MAX_QUADS=%d, flushing\n",
    //            dr->quadCount, MAX_QUADS);
        d3d9_flushBatch(dr);
    }

    dr->currentTexture = tex;

    float* verts = dr->vertexData + dr->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;
/*
    fprintf(stderr,
            "D3D9: emitTexturedQuad q=%d tex=%p "
            "pos[(%f,%f),(%f,%f),(%f,%f),(%f,%f)] "
            "uv[(%f,%f)->(%f,%f)] color=(%f,%f,%f,%f)\n",
            dr->quadCount, (void*)tex,
            x0, y0, x1, y1, x2, y2, x3, y3,
            u0, v0, u1, v1, r, g, b, alpha);
*/
    float tx, ty;

    // Vertex 0
    Matrix4f_transformPoint(&dr->viewMatrix, x0, y0, &tx, &ty);
    verts[0] = tx; verts[1] = ty; verts[2] = u0; verts[3] = v0;
    verts[4] = r;  verts[5] = g;  verts[6] = b;  verts[7] = alpha;

    // Vertex 1
    Matrix4f_transformPoint(&dr->viewMatrix, x1, y1, &tx, &ty);
    verts[8]  = tx; verts[9]  = ty; verts[10] = u1; verts[11] = v0;
    verts[12] = r;  verts[13] = g;  verts[14] = b;  verts[15] = alpha;

    // Vertex 2
    Matrix4f_transformPoint(&dr->viewMatrix, x2, y2, &tx, &ty);
    verts[16] = tx; verts[17] = ty; verts[18] = u1; verts[19] = v1;
    verts[20] = r;  verts[21] = g;  verts[22] = b;  verts[23] = alpha;

    // Vertex 3
    Matrix4f_transformPoint(&dr->viewMatrix, x3, y3, &tx, &ty);
    verts[24] = tx; verts[25] = ty; verts[26] = u0; verts[27] = v1;
    verts[28] = r;  verts[29] = g;  verts[30] = b;  verts[31] = alpha;

    dr->quadCount++;
}




typedef struct {
    Font* font;
    TexturePageItem* fontTpag;      // single TPAG for regular fonts (nullptr for sprite fonts)
    IDirect3DTexture9* tex;
    int32_t texW, texH;
    Sprite* spriteFontSprite;       // source sprite for sprite fonts (nullptr for regular fonts)
} D3D9FontState;


// ===[ Core vtable methods ]===

static void d3d9_destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (dr->rtSurface) IDirect3DSurface9_Release(dr->rtSurface);
    if (dr->rtTexture) IDirect3DTexture9_Release(dr->rtTexture);
    if (dr->whiteTexture) IDirect3DTexture9_Release(dr->whiteTexture);

    if (dr->vb) IDirect3DVertexBuffer9_Release(dr->vb);
    if (dr->ib) IDirect3DIndexBuffer9_Release(dr->ib);

    if (dr->d3dTextures) {
        for (uint32_t i = 0; i < dr->textureCount; ++i) {
            if (dr->d3dTextures[i]) IDirect3DTexture9_Release(dr->d3dTextures[i]);
        }
    }

    if (dr->surfaces) {
        for (uint32_t i = 0; i < dr->ssurfaceCount; ++i) {
            if (dr->surfaceTexture[i]) IDirect3DTexture9_Release(dr->surfaceTexture[i]);
            if (dr->surfaces[i]) IDirect3DSurface9_Release(dr->surfaces[i]);
        }
    }

    free(dr->d3dTextures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->textureLoaded);

    free(dr->surfaces);
    free(dr->surfaceTexture);
    free(dr->surfaceWidth);
    free(dr->surfaceHeight);

    free(dr->vertexData);

    if (dr->defaultColorRT) IDirect3DSurface9_Release(dr->defaultColorRT);

    free(dr);
}

static void d3d9_init(Renderer* renderer, DataWin* dataWin) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    renderer->dataWin = dataWin;

    // Grab backbuffer ONCE
    IDirect3DDevice9_GetRenderTarget(dr->device, 0, &dr->defaultColorRT);

    if (dr->defaultColorRT) {
        D3DSURFACE_DESC bbDesc;
        IDirect3DSurface9_GetDesc(dr->defaultColorRT, &bbDesc);
        dr->bbWidth  = (int)bbDesc.Width;
        dr->bbHeight = (int)bbDesc.Height;
        dr->rtFormat = bbDesc.Format;
    } else {
        dr->bbWidth  = 0;
        dr->bbHeight = 0;
        dr->rtFormat = D3DFMT_A8R8G8B8; // safe fallback
    }

    // Offscreen RT (created/resized in beginFrame)
    dr->rtTexture = NULL;
    dr->rtSurface = NULL;
    dr->rtWidth   = 0;
    dr->rtHeight  = 0;

    dr->maxQuads      = MAX_QUADS;
    dr->quadCount     = 0;
    dr->currentTexture = NULL;

    // Create dynamic VB
    IDirect3DDevice9_CreateVertexBuffer(
        dr->device,
        dr->maxQuads * VERTICES_PER_QUAD * sizeof(D3D9Vertex),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        D3D9_FVF,
        D3DPOOL_DEFAULT,
        &dr->vb,
        NULL
    );

    // Create static IB with quad pattern
    IDirect3DDevice9_CreateIndexBuffer(
        dr->device,
        dr->maxQuads * INDICES_PER_QUAD * sizeof(uint16_t),
        D3DUSAGE_WRITEONLY,
        D3DFMT_INDEX16,
        D3DPOOL_DEFAULT,
        &dr->ib,
        NULL
    );

    dr->vertexData = safeMalloc(
        MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX * sizeof(float)
    );

    dr->ssurfaceCount   = 0;
    dr->surfaces        = NULL;
    dr->surfaceTexture  = NULL;
    dr->surfaceWidth    = NULL;
    dr->surfaceHeight   = NULL;
    memset(dr->surfaceStack, -1, sizeof(dr->surfaceStack));

    // Fill index buffer
    uint16_t* indices = NULL;
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(dr->ib, 0, 0, (void**)&indices, 0))) {
        for (int32_t i = 0; i < dr->maxQuads; ++i) {
            uint16_t base = (uint16_t)(i * 4);
            indices[i * 6 + 0] = base + 0;
            indices[i * 6 + 1] = base + 1;
            indices[i * 6 + 2] = base + 2;
            indices[i * 6 + 3] = base + 2;
            indices[i * 6 + 4] = base + 3;
            indices[i * 6 + 5] = base + 0;
        }
        IDirect3DIndexBuffer9_Unlock(dr->ib);
    }

    // Texture arrays
    dr->textureCount   = dataWin->txtr.count;
    dr->d3dTextures    = safeCalloc(dr->textureCount, sizeof(IDirect3DTexture9*));
    dr->textureWidths  = safeCalloc(dr->textureCount, sizeof(int32_t));
    dr->textureHeights = safeCalloc(dr->textureCount, sizeof(int32_t));
    dr->textureLoaded  = safeCalloc(dr->textureCount, sizeof(bool));

    // 1×1 white texture
    IDirect3DTexture9* white = NULL;
    IDirect3DDevice9_CreateTexture(
        dr->device, 1, 1, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
        &white, NULL
    );
    if (white) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(white, 0, &lr, NULL, 0))) {
            *(uint32_t*)lr.pBits = 0xFFFFFFFFu;
            IDirect3DTexture9_UnlockRect(white, 0);
        }
    }
    dr->whiteTexture = white;

    dr->originalTexturePageCount = dr->textureCount;
    dr->originalTpagCount        = dataWin->tpag.count;
    dr->originalSpriteCount      = dataWin->sprt.count;

    dr->surfaceCount   = 0;
    dr->surfaces       = NULL;
    dr->surfaceTexture = NULL;
    dr->surfaceWidth   = NULL;
    dr->surfaceHeight  = NULL;
    dr->ssurfaceCount  = 0;
    memset(dr->surfaceStack, -1, sizeof(dr->surfaceStack));

// Default blend (straight alpha)
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, TRUE);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);


    // 2D pipeline
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ZENABLE,  D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_FOGENABLE, FALSE);

    // Vertex color modulates texture
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_COLORVERTEX, TRUE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);

    // Texture stage 0: tex * diffuse
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    // Alpha = tex alpha * vertex alpha
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dr->device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    // MAXIMUM‑QUALITY SAMPLING
IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    fprintf(stderr,
        "D3D9: Renderer initialized (%u texture pages, fmt=%d, bb=%dx%d)\n",
        dr->textureCount, dr->rtFormat, dr->bbWidth, dr->bbHeight
    );
}

static void d3d9_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                            int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    dr->quadCount      = 0;
    dr->currentTexture = NULL;
    dr->windowW        = windowW;
    dr->windowH        = windowH;
    dr->gameW          = gameW;
    dr->gameH          = gameH;

    // Resize RT if needed
    if (gameW != dr->rtWidth || gameH != dr->rtHeight) {
        if (dr->rtSurface) { IDirect3DSurface9_Release(dr->rtSurface); dr->rtSurface = NULL; }
        if (dr->rtTexture) { IDirect3DTexture9_Release(dr->rtTexture); dr->rtTexture = NULL; }

        IDirect3DDevice9_CreateTexture(
            dr->device,
            gameW, gameH,
            1,
            D3DUSAGE_RENDERTARGET,
            dr->rtFormat,
            D3DPOOL_DEFAULT,
            &dr->rtTexture,
            NULL
        );

        if (dr->rtTexture) {
            IDirect3DTexture9_GetSurfaceLevel(dr->rtTexture, 0, &dr->rtSurface);
        }

        dr->rtWidth  = gameW;
        dr->rtHeight = gameH;
        fprintf(stderr, "D3D9: RT resized to %dx%d (fmt=%d)\n", gameW, gameH, dr->rtFormat);
    }

    if (dr->rtSurface) {
        IDirect3DDevice9_SetRenderTarget(dr->device, 0, dr->rtSurface);

        D3DVIEWPORT9 vp = {
            0, 0,
            (DWORD)dr->rtWidth,
            (DWORD)dr->rtHeight,
            0.0f, 1.0f
        };
        IDirect3DDevice9_SetViewport(dr->device, &vp);

        RECT sc = {0, 0, dr->rtWidth, dr->rtHeight};
        IDirect3DDevice9_SetScissorRect(dr->device, &sc);
        IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SCISSORTESTENABLE, FALSE);
    }

    renderer->CPortX = 0;
    renderer->CPortY = 0;
    renderer->CPortW = gameW;
    renderer->CPortH = gameH;

    IDirect3DDevice9_BeginScene(dr->device);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_Clear(dr->device, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
}


// OLD:
// static void d3d9_endFrame(Renderer* renderer) { ... }

static void d3d9_endFrameInit(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    // Finish any in‑flight batched draws to the offscreen RT
    d3d9_flushBatch(dr);
    if (!dr->rtTexture || !dr->defaultColorRT)
        return;

    int winW = dr->bbWidth;
    int winH = dr->bbHeight;

    float scaleX = (float)winW / (float)dr->gameW;
    float scaleY = (float)winH / (float)dr->gameH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;

    int scaledW = (int)(dr->gameW * scale);
    int scaledH = (int)(dr->gameH * scale);

    int left = (winW - scaledW) / 2;
    int top  = (winH - scaledH) / 2;

    // Switch to the default backbuffer RT
    IDirect3DDevice9_SetRenderTarget(dr->device, 0, dr->defaultColorRT);

    // Make sure the RT texture is not bound on any stage when we sample it
    IDirect3DDevice9_SetTexture(dr->device, 0, NULL);
    IDirect3DDevice9_SetTexture(dr->device, 1, NULL);
    IDirect3DDevice9_SetTexture(dr->device, 2, NULL);
    IDirect3DDevice9_SetTexture(dr->device, 3, NULL);

    // Reset shaders to a simple fixed‑function path
    IDirect3DDevice9_SetPixelShader(dr->device, NULL);
    IDirect3DDevice9_SetVertexShader(dr->device, NULL);

    // Full‑window viewport
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)winW, (DWORD)winH, 0.0f, 1.0f };
    IDirect3DDevice9_SetViewport(dr->device, &vp);

    // Clear backbuffer
    IDirect3DDevice9_Clear(dr->device, 0, NULL,
                           D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);

    // Screen‑space transform
    Matrix4f_identity(&dr->viewMatrix);

    float x0 = (float)left;
    float y0 = (float)top;
    float x1 = (float)(left + scaledW);
    float y1 = (float)(top  + scaledH);

    // Half‑texel UVs for the RT
    float u0 = 0.5f / (float)dr->rtWidth;
    float v0 = 0.5f / (float)dr->rtHeight;
    float u1 = (dr->rtWidth  - 0.5f) / (float)dr->rtWidth;
    float v1 = (dr->rtHeight - 0.5f) / (float)dr->rtHeight;

    // Draw the RT texture to the backbuffer
    d3d9_emitTexturedQuad(
        dr,
        dr->rtTexture,
        x0, y0,
        x1, y0,
        x1, y1,
        x0, y1,
        u0, v0, u1, v1,
        1.0f, 1.0f, 1.0f, 1.0f
    );

    // DO NOT EndScene or Present here.
    // Also: don't flush yet; GUI will add more quads.
}

static void d3d9_endFrameEnd(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    // Flush fullscreen quad + any GUI draws
    d3d9_flushBatch(dr);

    // Now actually end the scene
    IDirect3DDevice9_EndScene(dr->device);
}




static void d3d9_beginView(Renderer* renderer,
                           int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                           float viewAngle)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    dr->quadCount      = 0;
    dr->currentTexture = NULL;

    // Viewport for this view
    D3DVIEWPORT9 vp = {
        (DWORD)portX,
        (DWORD)portY,
        (DWORD)portW,
        (DWORD)portH,
        0.0f, 1.0f
    };
    IDirect3DDevice9_SetViewport(dr->device, &vp);

    // Match GL: update current port on base renderer
    renderer->CPortX = portX;
    renderer->CPortY = portY;  // no Y‑flip in D3D9
    renderer->CPortW = portW;
    renderer->CPortH = portH;

    Matrix4f cam;
    Matrix4f_identity(&cam);

    // 1) rotation (around view center)
    if (viewAngle != 0.0f) {
        float cx = viewX + viewW * 0.5f;
        float cy = viewY + viewH * 0.5f;
        float angleRad = viewAngle * (float)M_PI / 180.0f;

        Matrix4f rot;
        Matrix4f_identity(&rot);
        Matrix4f_translate(&rot, cx, cy, 0.0f);
        Matrix4f_rotateZ(&rot, -angleRad);
        Matrix4f_translate(&rot, -cx, -cy, 0.0f);

        Matrix4f tmp;
        Matrix4f_multiply(&tmp, &cam, &rot);
        cam = tmp;
    }

    // 2) scale FIRST (GL‑style)
    float sx = (float)portW / (float)viewW;
    float sy = (float)portH / (float)viewH;
    Matrix4f_scale(&cam, sx, sy, 1.0f);

    // 3) translate world into view
    Matrix4f_translate(&cam, -(float)viewX, -(float)viewY, 0.0f);

    // 4) move into viewport
    Matrix4f_translate(&cam, (float)portX, (float)portY, 0.0f);

    dr->viewMatrix = cam;

    renderer->PreviousViewMatrix = cam;
}

static void d3d9_endView(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
}

static void d3d9_beginGUI(Renderer* renderer,
                          int32_t guiW, int32_t guiH,
                          int32_t portX, int32_t portY,
                          int32_t portW, int32_t portH)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    // Flush world batch before switching state
    d3d9_flushBatch(dr);

    // Render GUI directly to backbuffer
    IDirect3DDevice9_SetRenderTarget(dr->device, 0, dr->defaultColorRT);

    // GUI viewport is in window coordinates
    D3DVIEWPORT9 vp = {
        (DWORD)portX,
        (DWORD)portY,
        (DWORD)portW,
        (DWORD)portH,
        0.0f, 1.0f
    };
    IDirect3DDevice9_SetViewport(dr->device, &vp);

// Default blend (straight alpha)
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, TRUE);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);


    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_BLENDOP,   D3DBLENDOP_ADD);

    // Alpha test for fonts/UI
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHATESTENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHAREF, 1);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);

    // Disable depth
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ZENABLE, FALSE);

    // GUI projection
    Matrix4f proj;
    Matrix4f_identity(&proj);
    Matrix4f_ortho(&proj,
                   0.0f,
                   (float)guiW,
                   (float)guiH,
                   0.0f,
                   -1.0f, 1.0f);

    // D3D9 half‑texel correction
    Matrix4f guiTexelFix;
    Matrix4f_identity(&guiTexelFix);
    Matrix4f_translate(&guiTexelFix, -0.5f, -0.5f, 0.0f);

    Matrix4f guiFixedProj;
    Matrix4f_multiply(&guiFixedProj, &guiTexelFix, &proj);
    proj = guiFixedProj;

    IDirect3DDevice9_SetVertexShaderConstantF(dr->device, 0, (float*)&proj, 4);

    // Ensure linear filtering for crisp but smooth GUI
    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    // Reset batching state
    dr->quadCount      = 0;
    dr->currentTexture = NULL;

    // GUI draws in screen space
    Matrix4f_identity(&dr->viewMatrix);
}

static void d3d9_endGUI(Renderer* renderer)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);

    // Disable alpha test again for world
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHATESTENABLE, FALSE);

    // World transform will be set again in next beginView
    Matrix4f_identity(&dr->viewMatrix);
}




static void d3d9_clearScreen(Renderer* renderer, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);

    D3DCOLOR c = D3DCOLOR_ARGB(a, r, g, b);

    IDirect3DDevice9_Clear(dr->device, 0, NULL, D3DCLEAR_TARGET, c, 1.0f, 0);
}

static void d3d9_rendererFlush(Renderer* renderer) {
    d3d9_flushBatch((D3D9Renderer*)renderer);
}

// ===[ Blend / alpha / fog ]===
// (Direct mappings from the GL helpers; you can extend to match exactly.)

static void d3d9_gpuSetBlendEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
}

static bool d3d9_gpuGetBlendEnable(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DWORD v = 0;
    IDirect3DDevice9_GetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, &v);
    return v != 0;
}

static void d3d9_gpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (dr->alphaTestEnable == enable) return;
    d3d9_flushBatch(dr);
    dr->alphaTestEnable = enable;
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHATESTENABLE, enable ? TRUE : FALSE);
    if (enable) {
        IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHAREF, (DWORD)(dr->alphaTestRef * 255.0f));
    }
}

static void d3d9_gpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    float refF = ref / 255.0f;
    if (dr->alphaTestRef == refF) return;
    d3d9_flushBatch(dr);
    dr->alphaTestRef = refF;
    if (dr->alphaTestEnable) {
        IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHAREF, ref);
    }
}

static void d3d9_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
    DWORD mask = 0;
    if (red)   mask |= D3DCOLORWRITEENABLE_RED;
    if (green) mask |= D3DCOLORWRITEENABLE_GREEN;
    if (blue)  mask |= D3DCOLORWRITEENABLE_BLUE;
    if (alpha) mask |= D3DCOLORWRITEENABLE_ALPHA;
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_COLORWRITEENABLE, mask);
}

static void d3d9_gpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (dr->fogEnable == enable && dr->fogColor == color) return;
    d3d9_flushBatch(dr);
    dr->fogEnable = enable;
    dr->fogColor = color;

    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    D3DCOLOR c = D3DCOLOR_XRGB(r, g, b);

    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_FOGENABLE, enable ? TRUE : FALSE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_FOGCOLOR, c);
}

static void d3d9_drawSprite(Renderer* renderer,
                            int32_t tpagIndex,
                            float x, float y,
                            float originX, float originY,
                            float xscale, float yscale,
                            float angleDeg,
                            uint32_t color,
                            float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    TexturePageItem* tpag;
    IDirect3DTexture9* tex;
    int32_t texW, texH;

    if (!d3d9_resolveSpriteTexture(dr, tpagIndex, &tpag, &tex, &texW, &texH))
        return;

    // Half‑texel corrected UVs
    float u0 = (tpag->sourceX + 0.5f) / texW;
    float v0 = (tpag->sourceY + 0.5f) / texH;
    float u1 = (tpag->sourceX + tpag->sourceWidth  - 0.5f) / texW;
    float v1 = (tpag->sourceY + tpag->sourceHeight - 0.5f) / texH;

    // Local quad
    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float localX1 = localX0 + (float)tpag->sourceWidth;
    float localY1 = localY0 + (float)tpag->sourceHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float x0, y0, x1p, y1p, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0,  &y0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1p, &y1p);
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2,  &y2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3,  &y3);

    float r = BGR_R(color) / 255.0f;
    float g = BGR_G(color) / 255.0f;
    float b = BGR_B(color) / 255.0f;

    d3d9_emitTexturedQuad(dr, tex,
                          x0,  y0,
                          x1p, y1p,
                          x2,  y2,
                          x3,  y3,
                          u0, v0, u1, v1,
                          r, g, b, alpha);
}
static bool d3d9_resolveFontState(D3D9Renderer* dr, DataWin* dw, Font* font, D3D9FontState* state) {
    state->font = font;
    state->fontTpag = NULL;
    state->tex = NULL;
    state->texW = 0;
    state->texH = 0;
    state->spriteFontSprite = NULL;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (fontTpagIndex < 0) return false;

        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (pageId < 0 || (uint32_t)pageId >= dr->textureCount) return false;
        if (!d3d9_ensureTextureLoaded(dr, (uint32_t)pageId)) return false;

        state->tex  = dr->d3dTextures[pageId];
        state->texW = dr->textureWidths[pageId];
        state->texH = dr->textureHeights[pageId];
    } else if (font->tpagIndex >= 0 && dw->sprt.count > (uint32_t)font->tpagIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->tpagIndex];
    }
    return true;
}

static void d3d9_drawSpritePos(Renderer* renderer,
                               int32_t tpagIndex,
                               float x1, float y1,
                               float x2, float y2,
                               float x3, float y3,
                               float x4, float y4,
                               float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    TexturePageItem* tpag;
    IDirect3DTexture9* tex;
    int32_t texW, texH;
    if (!d3d9_resolveSpriteTexture(dr, tpagIndex, &tpag, &tex, &texW, &texH))
        return;

    float u0 = (tpag->sourceX + 0.5f) / texW;
    float v0 = (tpag->sourceY + 0.5f) / texH;
    float u1 = (tpag->sourceX + tpag->sourceWidth  - 0.5f) / texW;
    float v1 = (tpag->sourceY + tpag->sourceHeight - 0.5f) / texH;

    d3d9_emitTexturedQuad(dr, tex,
                          x1, y1,
                          x2, y2,
                          x3, y3,
                          x4, y4,
                          u0, v0, u1, v1,
                          1.0f, 1.0f, 1.0f, alpha);
}


static void d3d9_drawSpritePart(Renderer* renderer,
                                int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY,
                                int32_t srcW, int32_t srcH,
                                float x, float y,
                                float xscale, float yscale,
                                float angleDeg,
                                float pivotX, float pivotY,
                                uint32_t color,
                                float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    TexturePageItem* tpag;
    IDirect3DTexture9* tex;
    int32_t texW, texH;
    if (!d3d9_resolveSpriteTexture(dr, tpagIndex, &tpag, &tex, &texW, &texH))
        return;

    int32_t sx = tpag->sourceX + srcOffX;
    int32_t sy = tpag->sourceY + srcOffY;

    float u0 = (sx + 0.5f) / texW;
    float v0 = (sy + 0.5f) / texH;
    float u1 = (sx + srcW - 0.5f) / texW;
    float v1 = (sy + srcH - 0.5f) / texH;

    float localX0 = -pivotX;
    float localY0 = -pivotY;
    float localX1 = localX0 + (float)srcW;
    float localY1 = localY0 + (float)srcH;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float x0, y0, x1p, y1p, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0,  &y0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1p, &y1p);
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2,  &y2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3,  &y3);

    float r = BGR_R(color) / 255.0f;
    float g = BGR_G(color) / 255.0f;
    float b = BGR_B(color) / 255.0f;

    d3d9_emitTexturedQuad(dr, tex,
                          x0,  y0,
                          x1p, y1p,
                          x2,  y2,
                          x3,  y3,
                          u0, v0, u1, v1,
                          r, g, b, alpha);
}



static bool d3d9_resolveGlyph(
    D3D9Renderer* dr, DataWin* dw, D3D9FontState* state, FontGlyph* glyph,
    float cursorX, float cursorY,
    IDirect3DTexture9** outTex,
    float* outU0, float* outV0, float* outU1, float* outV1,
    float* outLocalX0, float* outLocalY0
) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != NULL) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
        if (glyphIndex < 0 || glyphIndex >= (int32_t)sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (tpagIdx < 0) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (pid < 0 || (uint32_t)pid >= dr->textureCount) return false;
        if (!d3d9_ensureTextureLoaded(dr, (uint32_t)pid)) return false;

        *outTex = dr->d3dTextures[pid];
        int32_t tw = dr->textureWidths[pid];
        int32_t th = dr->textureHeights[pid];

        *outU0 = (float)glyphTpag->sourceX / (float)tw;
        *outV0 = (float)glyphTpag->sourceY / (float)th;
        *outU1 = (float)(glyphTpag->sourceX + glyphTpag->sourceWidth) / (float)tw;
        *outV1 = (float)(glyphTpag->sourceY + glyphTpag->sourceHeight) / (float)th;

        *outLocalX0 = cursorX + (float)glyph->offset;
        *outLocalY0 = cursorY + (float)((int32_t)glyphTpag->targetY - sprite->originY);
    } else {
        *outTex = state->tex;
        *outU0 = (float)(state->fontTpag->sourceX + glyph->sourceX) / (float)state->texW;
        *outV0 = (float)(state->fontTpag->sourceY + glyph->sourceY) / (float)state->texH;
        *outU1 = (float)(state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float)state->texW;
        *outV1 = (float)(state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float)state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void d3d9_drawText(Renderer* renderer, const char* text,
                          float x, float y, float y1cale, float x2cale, float y3) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || dw->font.count <= (uint32_t)fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    D3D9FontState fontState;
    if (!d3d9_resolveFontState(dr, dw, font, &fontState)) return;

    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;
    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = TextUtils_lineStride(font);

    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -y3 * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, y1cale * font->scaleX, x2cale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL) {
                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    IDirect3DTexture9* glyphTex = NULL;

                    if (d3d9_resolveGlyph(dr, dw, &fontState, glyph, cursorX, cursorY,
                                          &glyphTex, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        if (dr->quadCount > 0 && dr->currentTexture != glyphTex) d3d9_flushBatch(dr);
                        if (dr->quadCount >= MAX_QUADS) d3d9_flushBatch(dr);
                        dr->currentTexture = glyphTex;

                        float localX1 = localX0 + (float)glyph->sourceWidth;
                        float localY1 = localY0 + (float)glyph->sourceHeight;

                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

                        d3d9_emitTexturedQuad(dr, glyphTex,
                                              px0, py0,
                                              px1, py1,
                                              px2, py2,
                                              px3, py3,
                                              u0, v0, u1, v1,
                                              r, g, b, alpha);
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}


static void d3d9_drawTextColor(Renderer* renderer, const char* text,
                               float x, float y, float y1cale, float x2cale, float y3,
                               int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || dw->font.count <= (uint32_t)fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    D3D9FontState fontState;
    if (!d3d9_resolveFontState(dr, dw, font, &fontState)) return;

    int32_t textLen = (int32_t)strlen(text);
    if (textLen == 0) return;

    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = TextUtils_lineStride(font);

    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -y3 * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, y1cale * font->scaleX, x2cale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        float gradientX = 0.0f;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL) {
                float advance = (float)glyph->shift;
                float leftFrac  = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                int32_t c1 = Color_lerp(_c1, _c2, leftFrac);
                int32_t c2 = Color_lerp(_c1, _c2, rightFrac);
                int32_t c3 = Color_lerp(_c4, _c3, rightFrac);
                int32_t c4 = Color_lerp(_c4, _c3, leftFrac);

                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    IDirect3DTexture9* glyphTex = NULL;

                    if (d3d9_resolveGlyph(dr, dw, &fontState, glyph, cursorX, cursorY,
                                          &glyphTex, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        if (dr->quadCount > 0 && dr->currentTexture != glyphTex) d3d9_flushBatch(dr);
                        if (dr->quadCount >= MAX_QUADS) d3d9_flushBatch(dr);
                        dr->currentTexture = glyphTex;

                        float localX1 = localX0 + (float)glyph->sourceWidth;
                        float localY1 = localY0 + (float)glyph->sourceHeight;

                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);

                        float r1 = (float)BGR_R(c1) / 255.0f;
                        float g1 = (float)BGR_G(c1) / 255.0f;
                        float b1 = (float)BGR_B(c1) / 255.0f;

                        float r2 = (float)BGR_R(c2) / 255.0f;
                        float g2 = (float)BGR_G(c2) / 255.0f;
                        float b2 = (float)BGR_B(c2) / 255.0f;

                        float r3 = (float)BGR_R(c3) / 255.0f;
                        float g3 = (float)BGR_G(c3) / 255.0f;
                        float b3 = (float)BGR_B(c3) / 255.0f;

                        float r4 = (float)BGR_R(c4) / 255.0f;
                        float g4 = (float)BGR_G(c4) / 255.0f;
                        float b4 = (float)BGR_B(c4) / 255.0f;

                        // We can’t vary color per vertex inside emit, so write directly into vertexData
                        if (dr->quadCount >= MAX_QUADS) d3d9_flushBatch(dr);
                        if (dr->quadCount > 0 && dr->currentTexture != glyphTex) d3d9_flushBatch(dr);
                        dr->currentTexture = glyphTex;

                        float* verts = dr->vertexData + dr->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

                        // top-left
                        verts[0] = px0; verts[1] = py0; verts[2] = u0; verts[3] = v0;
                        verts[4] = r1;  verts[5] = g1;  verts[6] = b1;  verts[7] = alpha;

                        // top-right
                        verts[8]  = px1; verts[9]  = py1; verts[10] = u1; verts[11] = v0;
                        verts[12] = r2;  verts[13] = g2;  verts[14] = b2;  verts[15] = alpha;

                        // bottom-right
                        verts[16] = px2; verts[17] = py2; verts[18] = u1; verts[19] = v1;
                        verts[20] = r3;  verts[21] = g3;  verts[22] = b3;  verts[23] = alpha;

                        // bottom-left
                        verts[24] = px3; verts[25] = py3; verts[26] = u0; verts[27] = v1;
                        verts[28] = r4;  verts[29] = g4;  verts[30] = b4;  verts[31] = alpha;

                        dr->quadCount++;
                        drewSuccessfully = true;
                    }
                }

                cursorX   += glyph->shift;
                gradientX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX   += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static uint32_t d3d9_findOrAllocSurfaceSlot(D3D9Renderer* dr) {
    for (uint32_t i = 0; i < dr->ssurfaceCount; ++i) {
        if (dr->surfaces[i] == NULL) return i;
    }
    uint32_t newIndex = dr->ssurfaceCount;
    dr->ssurfaceCount++;

    dr->surfaces       = safeRealloc(dr->surfaces,       dr->ssurfaceCount * sizeof(IDirect3DSurface9*));
    dr->surfaceTexture = safeRealloc(dr->surfaceTexture, dr->ssurfaceCount * sizeof(IDirect3DTexture9*));
    dr->surfaceWidth   = safeRealloc(dr->surfaceWidth,   dr->ssurfaceCount * sizeof(int32_t));
    dr->surfaceHeight  = safeRealloc(dr->surfaceHeight,  dr->ssurfaceCount * sizeof(int32_t));

    dr->surfaces[newIndex]       = NULL;
    dr->surfaceTexture[newIndex] = NULL;
    dr->surfaceWidth[newIndex]   = 0;
    dr->surfaceHeight[newIndex]  = 0;
    return newIndex;
}

static int32_t d3d9_findSurfaceStackSlot(D3D9Renderer* dr) {
    for (int32_t i = 0; i < 16; ++i) {
        if (dr->surfaceStack[i] == -1) return i;
    }
    return -1;
}

static void d3d9_removeSurfaceStackSlot(D3D9Renderer* dr) {
    for (int32_t i = 15; i >= 0; --i) {
        if (dr->surfaceStack[i] != -1) {
            dr->surfaceStack[i] = -1;
            return;
        }
    }
}

static int32_t d3d9_findSurfaceStackTop(D3D9Renderer* dr) {
    for (int32_t i = 15; i >= 0; --i) {
        if (dr->surfaceStack[i] != -1) return i;
    }
    return -1;
}

static bool d3d9_setRenderTargetInternal(Renderer* renderer, int32_t surfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

fprintf(stderr, "D3D9: setRTInternal id=%d rtSurf=%p ssCount=%u\n",
        surfaceId, (void*)dr->rtSurface, dr->ssurfaceCount);


    d3d9_flushBatch(dr);

    IDirect3DSurface9* target = NULL;
    int32_t w = 0, h = 0;

    if (surfaceId >= 0) {
        if ((uint32_t)surfaceId >= dr->ssurfaceCount) return false;
        if (!dr->surfaces[surfaceId]) return false;

        target = dr->surfaces[surfaceId];
        w = dr->surfaceWidth[surfaceId];
        h = dr->surfaceHeight[surfaceId];
    } else {
        // -1 = application_surface / main RT
        if (dr->rtSurface) {
            target = dr->rtSurface;
            w = dr->rtWidth;
            h = dr->rtHeight;
        } else {
            // Fallback: use actual backbuffer if RT not set up yet
            if (!dr->defaultColorRT) return false;
            target = dr->defaultColorRT;
            w = dr->bbWidth;
            h = dr->bbHeight;
        }
    }

    if (!target) return false;

    if (FAILED(IDirect3DDevice9_SetRenderTarget(dr->device, 0, target)))
        return false;

    D3DVIEWPORT9 vp = {
        0, 0,
        (DWORD)w,
        (DWORD)h,
        0.0f, 1.0f
    };
    IDirect3DDevice9_SetViewport(dr->device, &vp);

    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dr->device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    if (surfaceId >= 0) {
        IDirect3DDevice9_Clear(
            dr->device,
            0, NULL,
            D3DCLEAR_TARGET,
            D3DCOLOR_ARGB(0, 0, 0, 0),
            1.0f, 0
        );

        Matrix4f projection;
        Matrix4f_identity(&projection);
        Matrix4f_ortho(&projection,
                       0.0f, (float)w,
                       (float)h, 0.0f,
                       -1.0f, 1.0f);
        renderer->PreviousViewMatrix = projection;
    }

    return true;
}

static bool d3d9_setRenderTarget(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    d3d9_flushBatch(dr);
    int32_t slot = d3d9_findSurfaceStackSlot(dr);
    if (slot == -1) return false;

    // -1 = back to RT; anything else must be a valid, existing surface
    if (surfaceID >= 0) {
        if ((uint32_t)surfaceID >= dr->ssurfaceCount || !dr->surfaces[surfaceID]) {
            fprintf(stderr, "D3D9: setRenderTarget invalid surface %d\n", surfaceID);
            return false;
        }
    }

    dr->surfaceStack[slot] = surfaceID;
    return d3d9_setRenderTargetInternal(renderer, surfaceID);
}


static bool d3d9_resetSurfaceTarget(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    d3d9_flushBatch(dr);
    d3d9_removeSurfaceStackSlot(dr);
    int32_t top = d3d9_findSurfaceStackTop(dr);
    int32_t id  = (top != -1) ? dr->surfaceStack[top] : -1;
    if (top == -1)
        fprintf(stderr, "D3D9: resetSurfaceTarget -> default RT\n");
    return d3d9_setRenderTargetInternal(renderer, id);
}


static int32_t d3d9_createSurface(Renderer* renderer, int32_t width, int32_t height) {

    fprintf(stderr, "D3D9: createSurface STUB %dx%d\n", width, height);


    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);

    uint32_t idx = d3d9_findOrAllocSurfaceSlot(dr);

    IDirect3DTexture9* tex = NULL;

    // Surfaces MUST always be A8R8G8B8 (GM requirement)
    D3DFORMAT fmt = D3DFMT_A8R8G8B8;

    if (FAILED(IDirect3DDevice9_CreateTexture(
            dr->device,
            (UINT)width, (UINT)height,
            1,
            D3DUSAGE_RENDERTARGET,
            fmt,
            D3DPOOL_DEFAULT,
            &tex, NULL))) {
        fprintf(stderr, "D3D9: CreateSurface FAILED (%dx%d fmt=%d)\n", width, height, fmt);
        return -1;
    }

    IDirect3DSurface9* surf = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf))) {
        IDirect3DTexture9_Release(tex);
        return -1;
    }

    dr->surfaceTexture[idx] = tex;
    dr->surfaces[idx]       = surf;
    dr->surfaceWidth[idx]   = width;
    dr->surfaceHeight[idx]  = height;

    if (idx >= dr->ssurfaceCount)
        dr->ssurfaceCount = idx + 1;

    fprintf(stderr, "D3D9: Created surface %u (%dx%d fmt=%d)\n", idx, width, height, fmt);
    return (int32_t)idx;
}


static void d3d9_surfaceFree(Renderer* renderer, int32_t surfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount) return;

    if (dr->surfaceTexture[surfaceId]) {
        IDirect3DTexture9_Release(dr->surfaceTexture[surfaceId]);
        dr->surfaceTexture[surfaceId] = NULL;
    }
    if (dr->surfaces[surfaceId]) {
        IDirect3DSurface9_Release(dr->surfaces[surfaceId]);
        dr->surfaces[surfaceId] = NULL;
    }
    dr->surfaceWidth[surfaceId]  = 0;
    dr->surfaceHeight[surfaceId] = 0;

    fprintf(stderr, "D3D9: Freed surface %d\n", surfaceId);
}

static void d3d9_surfaceResize(Renderer* renderer, int32_t surfaceId, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);


fprintf(stderr, "D3D9: surfaceResize id=%d old=%dx%d new=%dx%d\n",
        surfaceId,
        dr->surfaceWidth[surfaceId], dr->surfaceHeight[surfaceId],
        width, height);


if (width <= 0 || height <= 0) {
    fprintf(stderr, "D3D9: surfaceResize to non‑positive size, ignoring (id=%d)\n", surfaceId);
    return;
}


    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount)
        return;

    if (dr->surfaceWidth[surfaceId] == width &&
        dr->surfaceHeight[surfaceId] == height)
        return;

    // Free old surface
    d3d9_surfaceFree(renderer, surfaceId);

    // Surfaces MUST always be A8R8G8B8
    D3DFORMAT fmt = D3DFMT_A8R8G8B8;

    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(
            dr->device,
            (UINT)width, (UINT)height,
            1,
            D3DUSAGE_RENDERTARGET,
            fmt,
            D3DPOOL_DEFAULT,
            &tex, NULL))) {
        fprintf(stderr, "D3D9: ResizeSurface FAILED (%dx%d fmt=%d)\n", width, height, fmt);
        return;
    }

    IDirect3DSurface9* surf = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf))) {
        IDirect3DTexture9_Release(tex);
        return;
    }

    dr->surfaceTexture[surfaceId] = tex;
    dr->surfaces[surfaceId]       = surf;
    dr->surfaceWidth[surfaceId]   = width;
    dr->surfaceHeight[surfaceId]  = height;

    fprintf(stderr, "D3D9: Resized surface %d (%dx%d fmt=%d)\n",
            surfaceId, width, height, fmt);
}

static bool d3d9_surfaceExists(Renderer* renderer, int32_t surfaceId) {
  

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount)
        return false;
    return dr->surfaces[surfaceId] != NULL;
}


static bool d3d9_surfaceGetPixels(Renderer* renderer, int32_t surfaceId, uint8_t* outRGBA) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    fprintf(stderr, "D3D9: surfaceGetPixels id=%d\n", surfaceId);

    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount)
        return false;
    if (!dr->surfaces[surfaceId])
        return false;

    d3d9_flushBatch(dr);

    int32_t w = dr->surfaceWidth[surfaceId];
    int32_t h = dr->surfaceHeight[surfaceId];
    if (w <= 0 || h <= 0)
        return false;

    IDirect3DSurface9* sysSurf = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
            dr->device,
            (UINT)w, (UINT)h,
            D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,
            &sysSurf, NULL))) {
        return false;
    }

    // POINT is required for exact pixel readback
    if (FAILED(IDirect3DDevice9_StretchRect(
            dr->device,
            dr->surfaces[surfaceId], NULL,
            sysSurf, NULL,
            D3DTEXF_POINT))) {
        IDirect3DSurface9_Release(sysSurf);
        return false;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(sysSurf, &lr, NULL, D3DLOCK_READONLY))) {
        IDirect3DSurface9_Release(sysSurf);
        return false;
    }

    uint8_t* srcBase = (uint8_t*)lr.pBits;
    int32_t srcPitch = lr.Pitch;
    int32_t rowBytes = w * 4;

    // Flip vertically to match GM's top‑down expectation
    for (int32_t y = 0; y < h; ++y) {
        uint8_t* src = srcBase + (h - 1 - y) * srcPitch;
        memcpy(outRGBA + (size_t)y * (size_t)rowBytes, src, (size_t)rowBytes);
    }

    IDirect3DSurface9_UnlockRect(sysSurf);
    IDirect3DSurface9_Release(sysSurf);
    return true;
}





static void d3d9_surfaceCopy(Renderer* renderer,
                             int32_t destSurfaceId, int32_t destX, int32_t destY,
                             int32_t srcSurfaceId,  int32_t srcX,  int32_t srcY,
                             int32_t srcW, int32_t srcH, bool part)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);


    fprintf(stderr, "D3D9: surfaceCopy dst=%d src=%d part=%d\n",
            destSurfaceId, srcSurfaceId, part);

    IDirect3DSurface9* srcSurf = NULL;
    int32_t srcWFull = 0, srcHFull = 0;

    if (srcSurfaceId >= 0) {
        if ((uint32_t)srcSurfaceId >= dr->ssurfaceCount) return;
        if (!dr->surfaces[srcSurfaceId]) return;
        srcSurf   = dr->surfaces[srcSurfaceId];
        srcWFull  = dr->surfaceWidth[srcSurfaceId];
        srcHFull  = dr->surfaceHeight[srcSurfaceId];
    } else {
        srcSurf   = dr->rtSurface;
        srcWFull  = dr->rtWidth;
        srcHFull  = dr->rtHeight;
    }


if (!srcSurf) {
    fprintf(stderr, "D3D9: surfaceCopy null srcSurf (srcId=%d)\n", srcSurfaceId);
    return;
}

    IDirect3DSurface9* dstSurf = NULL;
    int32_t dstHFull = 0;

    if (destSurfaceId >= 0) {
        if ((uint32_t)destSurfaceId >= dr->ssurfaceCount) return;
        if (!dr->surfaces[destSurfaceId]) return;
        dstSurf   = dr->surfaces[destSurfaceId];
        dstHFull  = dr->surfaceHeight[destSurfaceId];
    } else {
        dstSurf   = dr->rtSurface;
        dstHFull  = dr->rtHeight;
    }

if (!dstSurf) {
    fprintf(stderr, "D3D9: surfaceCopy null dstSurf (dstId=%d)\n", destSurfaceId);
    return;
}


    RECT srcRect, dstRect;

    if (part) {
        srcRect.left   = srcX;
        srcRect.top    = srcY;
        srcRect.right  = srcX + srcW;
        srcRect.bottom = srcY + srcH;

        dstRect.left   = destX;
        dstRect.top    = destY;
        dstRect.right  = destX + srcW;
        dstRect.bottom = destY + srcH;
    } else {
        srcRect.left   = 0;
        srcRect.top    = 0;
        srcRect.right  = srcWFull;
        srcRect.bottom = srcHFull;

        dstRect.left   = destX;
        dstRect.top    = destY;
        dstRect.right  = destX + srcWFull;
        dstRect.bottom = destY + srcHFull;
    }

    // Clamp to avoid sampling padding texels
    if (srcRect.left   < 0)         srcRect.left   = 0;
    if (srcRect.top    < 0)         srcRect.top    = 0;
    if (srcRect.right  > srcWFull)  srcRect.right  = srcWFull;
    if (srcRect.bottom > srcHFull)  srcRect.bottom = srcHFull;

    if (dstRect.top < 0) dstRect.top = 0;
    if (dstRect.bottom > dstHFull) dstRect.bottom = dstHFull;

    IDirect3DDevice9_StretchRect(
        dr->device,
        srcSurf, &srcRect,
        dstSurf, &dstRect,
        D3DTEXF_POINT   // POINT avoids bleeding from neighbouring texels
    );
}

static float d3d9_getSurfaceWidth(Renderer* renderer, int32_t surfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount) return 0.0f;
    if (!dr->surfaces[surfaceId]) return 0.0f;
    return (float)dr->surfaceWidth[surfaceId];
}

static float d3d9_getSurfaceHeight(Renderer* renderer, int32_t surfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);
    if (surfaceId < 0 || (uint32_t)surfaceId >= dr->ssurfaceCount) return 0.0f;
    if (!dr->surfaces[surfaceId]) return 0.0f;
    return (float)dr->surfaceHeight[surfaceId];
}



void d3d9_drawSurface(
    Renderer* renderer,
    int32_t surfaceID,
    int32_t srcLeft,
    int32_t srcTop,
    int32_t srcWidth,
    int32_t srcHeight,
    float x,
    float y,
    float xscale,
    float yscale,
    float angleDeg,
    uint32_t color,
    float alpha
)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if ((uint32_t)surfaceID >= dr->ssurfaceCount)
        return;

    IDirect3DTexture9* tex = dr->surfaceTexture[surfaceID];
    int32_t texW_i = dr->surfaceWidth[surfaceID];
    int32_t texH_i = dr->surfaceHeight[surfaceID];

    if (!tex)
        return;

    // Self‑heal: if stored size is bogus, query the texture
    if (texW_i <= 0 || texH_i <= 0) {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(IDirect3DTexture9_GetLevelDesc(tex, 0, &desc))) {
            texW_i = (int32_t)desc.Width;
            texH_i = (int32_t)desc.Height;
            dr->surfaceWidth[surfaceID]  = texW_i;
            dr->surfaceHeight[surfaceID] = texH_i;
            fprintf(stderr, "D3D9: surface %d size repaired to %dx%d from texture\n",
                    surfaceID, texW_i, texH_i);
        } else {
            fprintf(stderr, "D3D9: GetLevelDesc FAILED for surface %d\n", surfaceID);
            return;
        }
    }

    // GM convention: -1,-1 means "full surface"
    if (srcWidth  < 0) srcWidth  = texW_i - srcLeft;
    if (srcHeight < 0) srcHeight = texH_i - srcTop;

    if (srcWidth <= 0 || srcHeight <= 0)
        return;

    float texW = (float)texW_i;
    float texH = (float)texH_i;

    float u0 = (float)srcLeft / texW;
    float v0 = (float)srcTop  / texH;
    float u1 = (float)(srcLeft + srcWidth)  / texW;
    float v1 = (float)(srcTop  + srcHeight) / texH;

    float w = srcWidth  * xscale;
    float h = srcHeight * yscale;

    float x0 = x;
    float y0 = y;
    float x1 = x + w;
    float y1 = y;
    float x2 = x + w;
    float y2 = y + h;
    float x3 = x;
    float y3 = y + h;

    if (angleDeg != 0.0f) {
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;
        float rad = -angleDeg * (float)M_PI / 180.0f;

        Matrix4f rot;
        Matrix4f_setTransform2D(&rot, cx, cy, 1.0f, 1.0f, rad);

        Matrix4f_transformPoint(&rot, x0 - cx, y0 - cy, &x0, &y0);
        Matrix4f_transformPoint(&rot, x1 - cx, y1 - cy, &x1, &y1);
        Matrix4f_transformPoint(&rot, x2 - cx, y2 - cy, &x2, &y2);
        Matrix4f_transformPoint(&rot, x3 - cx, y3 - cy, &x3, &y3);
    }

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    // NaN / inf guard
    if (!(x0 == x0 && y0 == y0 && x1 == x1 && y1 == y1 &&
          x2 == x2 && y2 == y2 && x3 == x3 && y3 == y3 &&
          u0 == u0 && v0 == v0 && u1 == u1 && v1 == v1)) {
        fprintf(stderr, "D3D9: drawSurface NaN/inf in vertices or UVs, aborting\n");
        return;
    }

    d3d9_emitTexturedQuad(
        dr, tex,
        x0, y0,
        x1, y1,
        x2, y2,
        x3, y3,
        u0, v0, u1, v1,
        r, g, b, alpha
    );
}

static void d3d9_drawSurfacePart(Renderer* renderer, int32_t surfaceId,
                                 int32_t x, int32_t y,
                                 int32_t left, int32_t top,
                                 int32_t width, int32_t height,
                                 float xscale, float yscale,
                                 uint32_t color, float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    fprintf(stderr, "D3D9: drawSurfacePart id=%d\n", surfaceId);

    IDirect3DTexture9* tex = NULL;
    int32_t texW = 0, texH = 0;

    if (surfaceId >= 0) {
        if ((uint32_t)surfaceId >= dr->ssurfaceCount) return;
        tex  = dr->surfaceTexture[surfaceId];
        texW = dr->surfaceWidth[surfaceId];
        texH = dr->surfaceHeight[surfaceId];
    } else {
        tex  = dr->rtTexture;
        texW = dr->rtWidth;
        texH = dr->rtHeight;
    }
    if (!tex || texW <= 0 || texH <= 0) return;

    if (dr->quadCount > 0 && dr->currentTexture != tex) d3d9_flushBatch(dr);
    if (dr->quadCount >= MAX_QUADS) d3d9_flushBatch(dr);
    dr->currentTexture = tex;

    // Half‑texel corrected UVs
    float u0 = (left + 0.5f) / (float)texW;
    float v0 = (top  + 0.5f) / (float)texH;
    float u1 = (left + width  - 0.5f) / (float)texW;
    float v1 = (top  + height - 0.5f) / (float)texH;

    float localX0 = 0.0f;
    float localY0 = 0.0f;
    float localX1 = (float)width;
    float localY1 = (float)height;

    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, (float)x, (float)y, xscale, yscale, 0.0f);

    float x0p, y0p, x1p, y1p, x2p, y2p, x3p, y3p;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0p, &y0p);
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1p, &y1p);
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2p, &y2p);
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3p, &y3p);

    float r = BGR_R(color) / 255.0f;
    float g = BGR_G(color) / 255.0f;
    float b = BGR_B(color) / 255.0f;

    d3d9_emitTexturedQuad(
        dr, tex,
        x0p, y0p,
        x1p, y1p,
        x2p, y2p,
        x3p, y3p,
        u0, v0, u1, v1,
        r, g, b, alpha
    );
}

static void d3d9_drawSurfaceStretched(Renderer* renderer, int32_t surfaceId,
                                      float x, float y, float width, float height)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    fprintf(stderr, "D3D9: drawSurfaceStretched id=%d\n", surfaceId);

    IDirect3DTexture9* tex = NULL;
    int32_t texW = 0, texH = 0;

    if (surfaceId >= 0) {
        if ((uint32_t)surfaceId >= dr->ssurfaceCount) return;
        tex  = dr->surfaceTexture[surfaceId];
        texW = dr->surfaceWidth[surfaceId];
        texH = dr->surfaceHeight[surfaceId];
    } else {
        tex  = dr->rtTexture;
        texW = dr->rtWidth;
        texH = dr->rtHeight;
    }
    if (!tex || texW <= 0 || texH <= 0) return;

    if (dr->quadCount > 0 && dr->currentTexture != tex) d3d9_flushBatch(dr);
    if (dr->quadCount >= MAX_QUADS) d3d9_flushBatch(dr);
    dr->currentTexture = tex;

    // Full‑surface half‑texel UVs
    float u0 = 0.5f / (float)texW;
    float v0 = 0.5f / (float)texH;
    float u1 = (texW - 0.5f) / (float)texW;
    float v1 = (texH - 0.5f) / (float)texH;

    float x0  = x;
    float y0  = y;
    float x1p = x + width;
    float y1p = y + height;

    float r = 1.0f, g = 1.0f, b = 1.0f;
    float alpha = 1.0f;

    d3d9_emitTexturedQuad(
        dr, tex,
        x0,  y0,
        x1p, y0,
        x1p, y1p,
        x0,  y1p,
        u0, v0, u1, v1,
        r, g, b, alpha
    );
}




static uint32_t d3d9_findOrAllocTexturePageSlot(D3D9Renderer* dr) {
    // Reuse dynamic slots first
    for (uint32_t i = dr->originalTexturePageCount; i < dr->textureCount; ++i) {
        if (dr->d3dTextures[i] == NULL) return i;
    }

    uint32_t newPageId = dr->textureCount;
    dr->textureCount++;

    dr->d3dTextures   = safeRealloc(dr->d3dTextures,   dr->textureCount * sizeof(IDirect3DTexture9*));
    dr->textureWidths = safeRealloc(dr->textureWidths, dr->textureCount * sizeof(int32_t));
    dr->textureHeights= safeRealloc(dr->textureHeights,dr->textureCount * sizeof(int32_t));
    dr->textureLoaded = safeRealloc(dr->textureLoaded, dr->textureCount * sizeof(bool));

    dr->d3dTextures[newPageId]   = NULL;
    dr->textureWidths[newPageId] = 0;
    dr->textureHeights[newPageId]= 0;
    dr->textureLoaded[newPageId] = false;

    return newPageId;
}

static uint32_t d3d9_findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; i < dw->tpag.count; ++i) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }

    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}


static int32_t d3d9_createSpriteFromSurface(Renderer* renderer,
                                            int32_t surfaceID,
                                            int32_t x, int32_t y,
                                            int32_t w, int32_t h,
                                            bool removeback, bool smooth,
                                            int32_t xorig, int32_t yorig) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (w <= 0 || h <= 0) return -1;

    d3d9_flushBatch(dr);

    // Choose source render target
    IDirect3DSurface9* srcSurf = NULL;
    int32_t srcH = 0;

    if (surfaceID == -1) {
        srcSurf = dr->rtSurface;
        srcH    = dr->rtHeight;
    } else {
        if (surfaceID < 0 || (uint32_t)surfaceID >= dr->ssurfaceCount) return -1;
        if (dr->surfaces[surfaceID] == NULL) return -1;
        srcSurf = dr->surfaces[surfaceID];
        srcH    = dr->surfaceHeight[surfaceID];
    }

    // Clamp rect
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return -1;

    RECT srcRect;
    srcRect.left   = x;
    srcRect.top    = y;
    srcRect.right  = x + w;
    srcRect.bottom = y + h;

    // System-memory surface to receive pixels
    IDirect3DSurface9* sysSurf = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
            dr->device,
            (UINT)w, (UINT)h,
            D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,
            &sysSurf, NULL))) {
        return -1;
    }

    if (FAILED(IDirect3DDevice9_StretchRect(
            dr->device,
            srcSurf, &srcRect,
            sysSurf, NULL,
            D3DTEXF_POINT))) {
        IDirect3DSurface9_Release(sysSurf);
        return -1;
    }

    // GPU texture (DEFAULT pool)
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(
            dr->device,
            (UINT)w, (UINT)h,
            1,
            0,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &tex, NULL))) {
        IDirect3DSurface9_Release(sysSurf);
        return -1;
    }

    IDirect3DSurface9* texSurf = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &texSurf))) {
        IDirect3DTexture9_Release(tex);
        IDirect3DSurface9_Release(sysSurf);
        return -1;
    }

    // Copy sysmem -> GPU texture
    if (FAILED(IDirect3DDevice9_UpdateSurface(
            dr->device,
            sysSurf, NULL,
            texSurf, NULL))) {
        IDirect3DSurface9_Release(texSurf);
        IDirect3DSurface9_Release(sysSurf);
        IDirect3DTexture9_Release(tex);
        return -1;
    }

    IDirect3DSurface9_Release(texSurf);
    IDirect3DSurface9_Release(sysSurf);

    // Register as a dynamic texture page
    uint32_t pageId = d3d9_findOrAllocTexturePageSlot(dr);
    dr->d3dTextures[pageId]   = tex;
    dr->textureWidths[pageId] = w;
    dr->textureHeights[pageId]= h;
    dr->textureLoaded[pageId] = true;

    // Allocate TPAG entry
    uint32_t tpagIndex = d3d9_findOrAllocTpagSlot(dw, dr->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX       = 0;
    tpag->sourceY       = 0;
    tpag->sourceWidth   = (uint16_t)w;
    tpag->sourceHeight  = (uint16_t)h;
    tpag->targetX       = 0;
    tpag->targetY       = 0;
    tpag->targetWidth   = (uint16_t)w;
    tpag->targetHeight  = (uint16_t)h;
    tpag->boundingWidth = (uint16_t)w;
    tpag->boundingHeight= (uint16_t)h;
    tpag->texturePageId = (int16_t)pageId;

    // Allocate sprite slot
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, dr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];

    // name already set by DataWin_allocSpriteSlot
    sprite->width        = (uint32_t)w;
    sprite->height       = (uint32_t)h;
    sprite->originX      = xorig;
    sprite->originY      = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices  = safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount    = 0;
    sprite->masks        = NULL;

    fprintf(stderr, "D3D9: Created dynamic sprite %u (%dx%d) from surface %d at (%d,%d)\n",
            spriteIndex, w, h, surfaceID, x, y);
    return (int32_t)spriteIndex;
}

static void d3d9_deleteSprite(Renderer* renderer, int32_t spriteIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count) return;

    // Refuse to delete original data.win sprites
    if ((uint32_t)spriteIndex < dr->originalSpriteCount) {
        fprintf(stderr, "D3D9: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Free GL-equivalent dynamic texture pages and mark TPAG slots reusable
    for (uint32_t i = 0; i < sprite->textureCount; ++i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t)tpagIdx >= dr->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && (uint32_t)pageId < dr->textureCount) {
                if (dr->d3dTextures[pageId]) {
                    IDirect3DTexture9_Release(dr->d3dTextures[pageId]);
                    dr->d3dTextures[pageId] = NULL;
                }
            }
            tpag->texturePageId = -1; // mark TPAG slot free
        }
    }

    // Clear sprite entry but keep name string alive
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    fprintf(stderr, "D3D9: Deleted sprite %d\n", spriteIndex);
}

static void d3d9_gpuGetColorWriteEnable(Renderer* renderer,
                                        bool* red, bool* green,
                                        bool* blue, bool* alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    DWORD mask = 0;
    IDirect3DDevice9_GetRenderState(dr->device, D3DRS_COLORWRITEENABLE, &mask);

    if (red)   *red   = (mask & D3DCOLORWRITEENABLE_RED)   != 0;
    if (green) *green = (mask & D3DCOLORWRITEENABLE_GREEN) != 0;
    if (blue)  *blue  = (mask & D3DCOLORWRITEENABLE_BLUE)  != 0;
    if (alpha) *alpha = (mask & D3DCOLORWRITEENABLE_ALPHA) != 0;
}


// ===[ Helper: colored quad using whiteTexture ]===

static void d3d9_emitColoredQuad(D3D9Renderer* dr,
                                 float x0, float y0,
                                 float x1, float y1,
                                 float r, float g, float b, float a)
{
    // All UVs point to center of 1x1 white texture
    float u = 0.5f, v = 0.5f;

    d3d9_emitTexturedQuad(dr, dr->whiteTexture,
                          x0, y0,
                          x1, y0,
                          x1, y1,
                          x0, y1,
                          u, v, u, v,
                          r, g, b, a);
}


// ===[ Rectangle ]===

static void d3d9_drawRectangle(Renderer* renderer,
                               float x1, float y1,
                               float x2, float y2,
                               uint32_t color,
                               float alpha,
                               bool outline)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    if (outline) {
        // 4 one-pixel edges: top, bottom, left, right
        d3d9_emitColoredQuad(dr, x1,     y1,     x2 + 1.0f, y1 + 1.0f, r, g, b, alpha); // top
        d3d9_emitColoredQuad(dr, x1,     y2,     x2 + 1.0f, y2 + 1.0f, r, g, b, alpha); // bottom
        d3d9_emitColoredQuad(dr, x1,     y1 + 1.0f, x1 + 1.0f, y2,     r, g, b, alpha); // left
        d3d9_emitColoredQuad(dr, x2,     y1 + 1.0f, x2 + 1.0f, y2,     r, g, b, alpha); // right
    } else {
        // Filled rect: GML adds +1 to width/height
        d3d9_emitColoredQuad(dr, x1, y1, x2 + 1.0f, y2 + 1.0f, r, g, b, alpha);
    }
}


// ===[ Line ]===

static void d3d9_drawLine(Renderer* renderer,
                          float x1, float y1,
                          float x2, float y2,
                          float width,
                          uint32_t color,
                          float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    // Use whiteTexture quad with per-vertex color
    float u = 0.5f, v = 0.5f;

    d3d9_emitTexturedQuad(dr, dr->whiteTexture,
                          x1 + px, y1 + py,
                          x1 - px, y1 - py,
                          x2 - px, y2 - py,
                          x2 + px, y2 + py,
                          u, v, u, v,
                          r, g, b, alpha);
}

static void d3d9_drawLineColor(Renderer* renderer,
                               float x1, float y1,
                               float x2, float y2,
                               float width,
                               uint32_t color1,
                               uint32_t color2,
                               float alpha)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    float r1 = (float)BGR_R(color1) / 255.0f;
    float g1 = (float)BGR_G(color1) / 255.0f;
    float b1 = (float)BGR_B(color1) / 255.0f;

    float r2 = (float)BGR_R(color2) / 255.0f;
    float g2 = (float)BGR_G(color2) / 255.0f;
    float b2 = (float)BGR_B(color2) / 255.0f;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    float u = 0.5f, v = 0.5f;

    // Start side (color1), end side (color2)
    if (dr->quadCount > 0 && dr->currentTexture != dr->whiteTexture)
        d3d9_flushBatch(dr);
    if (dr->quadCount >= MAX_QUADS)
        d3d9_flushBatch(dr);
    dr->currentTexture = dr->whiteTexture;

    float* verts = dr->vertexData + dr->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // v0: start + perp (color1)
    verts[0] = x1 + px; verts[1] = y1 + py; verts[2] = u; verts[3] = v;
    verts[4] = r1;      verts[5] = g1;      verts[6] = b1; verts[7] = alpha;

    // v1: start - perp (color1)
    verts[8]  = x1 - px; verts[9]  = y1 - py; verts[10] = u; verts[11] = v;
    verts[12] = r1;      verts[13] = g1;      verts[14] = b1; verts[15] = alpha;

    // v2: end - perp (color2)
    verts[16] = x2 - px; verts[17] = y2 - py; verts[18] = u; verts[19] = v;
    verts[20] = r2;      verts[21] = g2;      verts[22] = b2; verts[23] = alpha;

    // v3: end + perp (color2)
    verts[24] = x2 + px; verts[25] = y2 + py; verts[26] = u; verts[27] = v;
    verts[28] = r2;      verts[29] = g2;      verts[30] = b2; verts[31] = alpha;

    dr->quadCount++;
}


// ===[ Gradient rectangle ]===

static void d3d9_drawRectangleColor(Renderer* renderer,
                                    float x1, float y1,
                                    float x2, float y2,
                                    uint32_t c1, uint32_t c2,
                                    uint32_t c3, uint32_t c4,
                                    float alpha,
                                    bool outline)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    float r1 = (float)BGR_R(c1) / 255.0f;
    float g1 = (float)BGR_G(c1) / 255.0f;
    float b1 = (float)BGR_B(c1) / 255.0f;

    float r2 = (float)BGR_R(c2) / 255.0f;
    float g2 = (float)BGR_G(c2) / 255.0f;
    float b2 = (float)BGR_B(c2) / 255.0f;

    float r3 = (float)BGR_R(c3) / 255.0f;
    float g3 = (float)BGR_G(c3) / 255.0f;
    float b3 = (float)BGR_B(c3) / 255.0f;

    float r4 = (float)BGR_R(c4) / 255.0f;
    float g4 = (float)BGR_G(c4) / 255.0f;
    float b4 = (float)BGR_B(c4) / 255.0f;

    if (outline) {
        // Use gradient lines for edges
        d3d9_drawLineColor(renderer, x1, y1, x2, y1, 1.0f, c1, c2, alpha);
        d3d9_drawLineColor(renderer, x2, y1, x2, y2, 1.0f, c2, c3, alpha);
        d3d9_drawLineColor(renderer, x2, y2, x1, y2, 1.0f, c3, c4, alpha);
        d3d9_drawLineColor(renderer, x1, y2, x1, y1, 1.0f, c4, c1, alpha);
        return;
    }

    // Filled gradient rect: +1 to width/height
    if (dr->quadCount > 0 && dr->currentTexture != dr->whiteTexture)
        d3d9_flushBatch(dr);
    if (dr->quadCount >= MAX_QUADS)
        d3d9_flushBatch(dr);
    dr->currentTexture = dr->whiteTexture;

    float* verts = dr->vertexData + dr->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;
    float u = 0.5f, v = 0.5f;

    // v0: top-left (c1)
    verts[0] = x1;       verts[1] = y1;       verts[2] = u; verts[3] = v;
    verts[4] = r1;       verts[5] = g1;       verts[6] = b1; verts[7] = alpha;

    // v1: top-right (c2)
    verts[8]  = x2 + 1;  verts[9]  = y1;       verts[10] = u; verts[11] = v;
    verts[12] = r2;      verts[13] = g2;      verts[14] = b2; verts[15] = alpha;

    // v2: bottom-right (c3)
    verts[16] = x2 + 1;  verts[17] = y2 + 1;  verts[18] = u; verts[19] = v;
    verts[20] = r3;      verts[21] = g3;      verts[22] = b3; verts[23] = alpha;

    // v3: bottom-left (c4)
    verts[24] = x1;      verts[25] = y2 + 1;  verts[26] = u; verts[27] = v;
    verts[28] = r4;      verts[29] = g4;      verts[30] = b4; verts[31] = alpha;

    dr->quadCount++;
}


// ===[ Triangle ]===

static void d3d9_drawTriangle(Renderer* renderer,
                              float x1, float y1,
                              float x2, float y2,
                              float x3, float y3,
                              bool outline)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (outline) {
        d3d9_drawLine(renderer, x1, y1, x2, y2, 1.0f, renderer->drawColor, 1.0f);
        d3d9_drawLine(renderer, x2, y2, x3, y3, 1.0f, renderer->drawColor, 1.0f);
        d3d9_drawLine(renderer, x3, y3, x1, y1, 1.0f, renderer->drawColor, 1.0f);
        return;
    }

    float r = (float)BGR_R(renderer->drawColor) / 255.0f;
    float g = (float)BGR_G(renderer->drawColor) / 255.0f;
    float b = (float)BGR_B(renderer->drawColor) / 255.0f;
    float a = renderer->drawAlpha;

    d3d9_flushBatch(dr);

    D3D9Vertex verts[3];
    for (int i = 0; i < 3; ++i) {
        verts[i].z   = 0.0f;
        verts[i].rhw = 1.0f;
        verts[i].u   = 0.5f;
        verts[i].v   = 0.5f;
        verts[i].color = D3DCOLOR_ARGB(
            (uint8_t)(a * 255.0f),
            (uint8_t)(r * 255.0f),
            (uint8_t)(g * 255.0f),
            (uint8_t)(b * 255.0f)
        );
    }

    verts[0].x = x1; verts[0].y = y1;
    verts[1].x = x2; verts[1].y = y2;
    verts[2].x = x3; verts[2].y = y3;

    IDirect3DDevice9_SetFVF(dr->device, D3D9_FVF);
    IDirect3DDevice9_SetTexture(dr->device, 0, (IDirect3DBaseTexture9*)dr->whiteTexture);
    IDirect3DDevice9_DrawPrimitiveUP(dr->device,
                                     D3DPT_TRIANGLELIST,
                                     1,
                                     verts,
                                     sizeof(D3D9Vertex));
}


// ===[ Blend modes ]===
// Minimal, non-crashing versions; you can refine mapping to GMS enums later.

static void d3d9_gpuSetBlendMode(Renderer* renderer, int32_t mode)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);

    // Default: normal alpha blending
 //   DWORD src = D3DBLEND_SRCALPHA;
 //   DWORD dst = D3DBLEND_INVSRCALPHA;
    DWORD op  = D3DBLENDOP_ADD;

    // You can wire real GMS constants here if you want exact behaviour
    switch (mode) {
    // case bm_add:      src = D3DBLEND_SRCALPHA; dst = D3DBLEND_ONE;        op = D3DBLENDOP_ADD;        break;
    // case bm_subtract: src = D3DBLEND_SRCALPHA; dst = D3DBLEND_INVSRCALPHA; op = D3DBLENDOP_REVSUBTRACT; break;
    default:
        break;
    }

// Default blend (straight alpha)
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_ALPHABLENDENABLE, TRUE);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);


    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_BLENDOP, op);
}

static void d3d9_gpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    d3d9_flushBatch(dr);

    // For now, treat all extended modes as normal alpha blend.
    // You can later map sfactor/dfactor to D3DBLEND_* once you line up the enums.
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_SRCBLEND,  D3DBLEND_ONE);
IDirect3DDevice9_SetRenderState(dr->device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    IDirect3DDevice9_SetRenderState(dr->device, D3DRS_BLENDOP,   D3DBLENDOP_ADD);
}

static int32_t d3d9_ensureApplicationSurface(Renderer* renderer,
                                             int32_t width,
                                             int32_t height)
{
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    Runner* runner = renderer->runner;

    int32_t id = runner->applicationSurfaceId;

    bool needsCreate =
        (id < 0) ||
        ((uint32_t)id >= dr->ssurfaceCount) ||
        (dr->surfaces[id] == NULL);

    if (needsCreate) {
        id = d3d9_createSurface(renderer, width, height);

        // Publish immediately so anything querying the runner sees the new ID
        runner->applicationSurfaceId = id;
        return id;
    }

    // Resize if dimensions changed
    if (dr->surfaceWidth[id] != width || dr->surfaceHeight[id] != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }

    return id;
}



// ===[ Vtable ]===

static RendererVtable d3d9Vtable = {
    .init = d3d9_init,
    .destroy = d3d9_destroy,
    .beginFrame = d3d9_beginFrame,
.endFrameInit = d3d9_endFrameInit,
.endFrameEnd  = d3d9_endFrameEnd,

    .beginView = d3d9_beginView,
    .endView = d3d9_endView,
    .beginGUI = d3d9_beginGUI,
    .endGUI = d3d9_endGUI,

    .drawSprite = d3d9_drawSprite,
    .drawSpritePart = d3d9_drawSpritePart,
    .drawSpritePos = d3d9_drawSpritePos,
    .drawRectangle = d3d9_drawRectangle,
    .drawRectangleColor = d3d9_drawRectangleColor,
    .drawLine = d3d9_drawLine,
    .drawTriangle = d3d9_drawTriangle,
    .drawLineColor = d3d9_drawLineColor,
    .drawText = d3d9_drawText,
    .drawTextColor = d3d9_drawTextColor,

    .flush = d3d9_rendererFlush,
    .clearScreen = d3d9_clearScreen,

    .createSpriteFromSurface = d3d9_createSpriteFromSurface,
    .deleteSprite = d3d9_deleteSprite,

    .gpuSetBlendMode = d3d9_gpuSetBlendMode,
    .gpuSetBlendModeExt = d3d9_gpuSetBlendModeExt,
    .gpuSetBlendEnable = d3d9_gpuSetBlendEnable,
    .gpuSetAlphaTestEnable = d3d9_gpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef = d3d9_gpuSetAlphaTestRef,
    .gpuSetColorWriteEnable = d3d9_gpuSetColorWriteEnable,
    .gpuGetColorWriteEnable = d3d9_gpuGetColorWriteEnable,
    .gpuGetBlendEnable = d3d9_gpuGetBlendEnable,
    .gpuSetFog = d3d9_gpuSetFog,

    .drawTile = NULL,
    .drawTiled = NULL,

    .createSurface = d3d9_createSurface,
    .surfaceExists = d3d9_surfaceExists,
    .setRenderTarget = d3d9_setRenderTarget,

    .getSurfaceWidth = d3d9_getSurfaceWidth,
    .getSurfaceHeight = d3d9_getSurfaceHeight,

    .drawSurface = d3d9_drawSurface,

    .surfaceResize = d3d9_surfaceResize,
    .surfaceFree = d3d9_surfaceFree,

    .surfaceCopy = d3d9_surfaceCopy,
    .surfaceGetPixels = d3d9_surfaceGetPixels,

    .drawTiledPart = NULL,
.ensureApplicationSurface = d3d9_ensureApplicationSurface,

};



// ===[ Public API ]===

Renderer* D3D9Renderer_create(IDirect3DDevice9* device) {
    D3D9Renderer* dr = safeCalloc(1, sizeof(D3D9Renderer));
    dr->base.vtable = &d3d9Vtable;
    dr->base.drawColor = 0xFFFFFF;
    dr->base.drawAlpha = 1.0f;
    dr->base.drawFont = -1;
    dr->base.drawHalign = 0;
    dr->base.drawValign = 0;
    dr->base.circlePrecision = 24;
    memset(dr->surfaceStack, -1, sizeof(dr->surfaceStack));
    dr->device = device;
    return (Renderer*)dr;
}
