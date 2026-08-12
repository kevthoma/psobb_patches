// Contrast-Adaptive Sharpening (AMD FidelityFX CAS), adapted for the d3d8to9 post-FX chain.
// Runs last, on the fully composited frame (3D + HUD/text), to counteract the softness of the
// upscaled f512 HUD atlas without the halos of a naive unsharp mask. Strength is a runtime
// parameter (0..1) driven by widescreen.cfg's SharpenStrength.

extern float SharpenStrength = 0.5;   //[0.00 to 1.00] Sharpening amount. 0 = off-ish, 1 = maximum.

#ifndef PIXEL_SIZE // compile-time; the C++ side injects the real backbuffer pixel size.
#define PIXEL_SIZE float2(1.0 / 1280.0, 1.0 / 720.0)
#endif

static float2 rcpres = PIXEL_SIZE;

texture2D frameTex2D;
sampler frameSampler = sampler_state
{
    Texture = <frameTex2D>;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
};

struct VSOUT { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
struct VSIN  { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN IN)
{
    VSOUT OUT;
    OUT.vertPos = float4(IN.vertPos.x, IN.vertPos.y, IN.vertPos.z, 1.0f);
    OUT.UVCoord = float2(IN.UVCoord.x, IN.UVCoord.y);
    return OUT;
}

float3 tap(float2 uv, float2 off) { return tex2D(frameSampler, uv + off).rgb; }

float4 SharpenPass(VSOUT IN) : COLOR0
{
    float2 px = rcpres;
    float2 uv = IN.UVCoord;

    // 3x3 neighborhood:
    //  a b c
    //  d e f
    //  g h i
    float3 a = tap(uv, float2(-px.x, -px.y));
    float3 b = tap(uv, float2( 0.0,  -px.y));
    float3 c = tap(uv, float2( px.x, -px.y));
    float3 d = tap(uv, float2(-px.x,  0.0 ));
    float3 e = tap(uv, float2( 0.0,   0.0 ));
    float3 f = tap(uv, float2( px.x,  0.0 ));
    float3 g = tap(uv, float2(-px.x,  px.y));
    float3 h = tap(uv, float2( 0.0,   px.y));
    float3 i = tap(uv, float2( px.x,  px.y));

    // Soft min/max: cross first, then fold in the corners (CAS "sum of two extrema" trick).
    float3 mnRGB  = min(min(min(d, e), f), min(b, h));
    float3 mnRGB2 = min(min(min(mnRGB, a), c), min(g, i));
    mnRGB += mnRGB2;

    float3 mxRGB  = max(max(max(d, e), f), max(b, h));
    float3 mxRGB2 = max(max(max(mxRGB, a), c), max(g, i));
    mxRGB += mxRGB2;

    // Adaptive amplitude from local contrast, then shaped.
    // max() guards against a divide-by-zero (NaN) in flat black/grey regions.
    float3 rcpMRGB = 1.0 / max(mxRGB, 1e-4);
    float3 ampRGB  = saturate(min(mnRGB, 2.0 - mxRGB) * rcpMRGB);
    ampRGB = sqrt(ampRGB);

    // Strength [0..1] -> filter peak (lower divisor = stronger sharpening).
    float peak = -1.0 / lerp(8.0, 5.0, saturate(SharpenStrength));
    float3 wRGB = ampRGB * peak;
    float3 rcpWeightRGB = 1.0 / (1.0 + 4.0 * wRGB);

    float3 outColor = saturate((b * wRGB + d * wRGB + f * wRGB + h * wRGB + e) * rcpWeightRGB);
    return float4(outColor, 1.0);
}

technique t0
{
    pass p0
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 SharpenPass();
        ZEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;
        ColorWriteEnable = RED|GREEN|BLUE;
        SRGBWriteEnable = false;
    }
}
