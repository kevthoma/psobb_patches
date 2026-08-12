// Sharpen.cpp - Contrast-Adaptive Sharpening post-process pass.
// Mirrors the CelShader pass structure: loads Sharpen.fx from an embedded resource, renders a
// fullscreen quad from the source frame texture into the destination surface, applying CAS.

#include <sstream>
#include <vector>
#include "Sharpen.h"
using namespace std;

// Strength (0..1) from widescreen.cfg, defined in Options.c.
extern "C" float g_fSharpenStrength;

#ifndef V
#define V(x) { hr = (x); }
#endif
#ifndef SAFE_DELETE
#define SAFE_DELETE(p) { if (p) { delete (p); (p) = nullptr; } }
#endif
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p) = nullptr; } }
#endif

// Loads #included headers (if any) from resources - same helper the other passes use.
class SharpenIncludeResource : public ID3DXInclude {
    public:
        STDMETHOD(Open)(THIS_ D3DXINCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID *ppData, UINT *pBytes) {
            HRSRC src = FindResourceA(g_hModule, pFileName, RT_RCDATA);
            HGLOBAL res = LoadResource(g_hModule, src);
            *pBytes = SizeofResource(g_hModule, src);
            *ppData = (LPCVOID)LockResource(res);
            return S_OK;
        }
        STDMETHOD(Close)(THIS_ LPCVOID) { return S_OK; }
};

Sharpen::Sharpen(IDirect3DDevice9 *device, int width, int height)
        : device(device), width(width), height(height) {
    HRESULT hr;

    // Inject the real backbuffer pixel size as a compile-time macro (like the other passes).
    vector<D3DXMACRO> defines;
    stringstream s;
    s << "float2(1.0 / " << width << ", 1.0 / " << height << ")";
    string pixelSizeText = s.str();
    D3DXMACRO pixelSizeMacro = { "PIXEL_SIZE", pixelSizeText.c_str() };
    defines.push_back(pixelSizeMacro);
    D3DXMACRO null = { nullptr, nullptr };
    defines.push_back(null);

    DWORD flags = D3DXFX_NOT_CLONEABLE | D3DXSHADER_OPTIMIZATION_LEVEL3;
#ifdef D3DXFX_LARGEADDRESS_HANDLE
    flags |= D3DXFX_LARGEADDRESSAWARE;
#endif

    SharpenIncludeResource includeResource;
    V(D3DXCreateEffectFromResourceA(device, g_hModule, "Sharpen.fx", &defines.front(), &includeResource, flags, nullptr, &effect, nullptr));

    const D3DVERTEXELEMENT9 vertexElements[3] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    V(device->CreateVertexDeclaration(vertexElements, &vertexDeclaration));

    frameTexHandle = effect->GetParameterByName(NULL, "frameTex2D");
    strengthHandle = effect->GetParameterByName(NULL, "SharpenStrength");
}

Sharpen::~Sharpen() {
    SAFE_RELEASE(effect);
    SAFE_RELEASE(vertexDeclaration);
}

void Sharpen::go(IDirect3DTexture9 *frame, IDirect3DSurface9 *dst) {
    HRESULT hr;
    V(device->SetVertexDeclaration(vertexDeclaration));
    sharpenPass(frame, dst);
}

void Sharpen::sharpenPass(IDirect3DTexture9* frame, IDirect3DSurface9 *dst) {
    D3DPERF_BeginEvent(D3DCOLOR_XRGB(0, 0, 0), L"Sharpen: 1st pass");
    HRESULT hr;

    V(device->SetRenderTarget(0, dst));
    V(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0));

    V(effect->SetTexture(frameTexHandle, frame));
    if (strengthHandle) {
        V(effect->SetFloat(strengthHandle, g_fSharpenStrength));
    }

    UINT passes;
    V(effect->Begin(&passes, 0));
    V(effect->BeginPass(0));
    quad(width, height);
    V(effect->EndPass());
    V(effect->End());

    D3DPERF_EndEvent();
}

void Sharpen::quad(int width, int height) {
    HRESULT hr;
    D3DXVECTOR2 pixelSize = D3DXVECTOR2(1.0f / float(width), 1.0f / float(height));
    float quad[4][5] = {
        { -1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.5f, 0.0f, 0.0f },
        {  1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.5f, 1.0f, 0.0f },
        { -1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.5f, 0.0f, 1.0f },
        {  1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.5f, 1.0f, 1.0f }
    };
    V(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
}
