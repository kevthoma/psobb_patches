// Sharpen.h - Contrast-Adaptive Sharpening post-process pass.

#ifndef SHARPEN_H
#define SHARPEN_H

#include <d3d9.h>
#include <d3dx9.h>
#include <dxerr.h>
#include <dxgi.h>

extern HMODULE g_hModule;

class Sharpen {
    public:
        Sharpen(IDirect3DDevice9 *device, int width, int height);
        ~Sharpen();

        void go(IDirect3DTexture9 *frame, IDirect3DSurface9 *dst, float strength);

    private:
        void sharpenPass(IDirect3DTexture9* frame, IDirect3DSurface9 *dst, float strength);
        void quad(int width, int height);

        IDirect3DDevice9 *device;
        ID3DXEffect *effect;
        IDirect3DVertexDeclaration9 *vertexDeclaration;

        D3DXHANDLE frameTexHandle;
        D3DXHANDLE strengthHandle;

        int width, height;
};

#endif
