// Quake III's MD3 models on VU1 (libgl: a vertex program with inputs of its
// own, tr_shade.c RB_PS2_DrawModel): the two frames' vertices as the model
// file has them (md3XyzNormal_t: x, y, z in 1/64 units and the normal as
// latitude and longitude bytes: four shorts), blended here, lit here
// (rgbGen lightingDiffuse: ambient + directed * N.L of the new frame's
// normal), with the file's texture coordinates or tcGen environment's.
// libgl gathers them by the surface's strips and fills adc.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 cscale;
    float4 tms;
    float4 tmt;
    float4 blend;   // x: the old frame's share (backlerp), y: the vertices' unit (1/64)
    float4 ldir;    // the light's direction in the model's space
    float4 amb;     // the colour without light, 0..255 (a: the alpha)
    float4 dirc;    // the directed light's colour, 0..255 (a 0)
    float4 env;     // xyz: the viewer in the model's space; w 1: tcGen environment
};

struct In {
    int4 pn : POSITION_V4_16;  // the new frame's
    int4 po : NORMAL_V4_16;    // the old frame's
    float2 st : TEXCOORD;
    uint4 adc : ADC_V4_8U;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

// sin(2 pi t) for t in 0..1: folded to a quarter turn either side of 0, then
// x - x^3/6 + x^5/120
float sin1(float t) {
    float v = frac(t + 0.25) - 0.5;
    float x = (0.25 - abs(v)) * 6.2831853;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * 0.0083333333));
}

[shader("vertex")]
Out q3model(In v) {
    float3 a = float3(v.pn.xyz), b = float3(v.po.xyz);
    float3 pos = (a + (b - a) * blend.x) * blend.y;
    float4 p = mul(mvp, float4(pos, 1));
    float q = rcp(p.w);
    // the normal word: latitude in its high byte, longitude in its low one
    float w = float(v.pn.w);
    w = w + 65536.0 * step(w, -0.5);
    float lat = floor(w * (1.0 / 256.0));
    float lng = w - lat * 256.0;
    lat = lat * (1.0 / 256.0), lng = lng * (1.0 / 256.0);
    float slng = sin1(lng);
    float3 n = float3(sin1(lat + 0.25) * slng, sin1(lat) * slng, sin1(lng + 0.25));
    // tcGen environment (RB_CalcEnvironmentTexCoords): the viewer's direction
    // mirrored in the normal; else the file's coordinates
    float3 vw = normalize(env.xyz - pos);
    float3 refl = n * (2.0 * dot(n, vw)) - vw;
    float2 fst = v.st + (float2(0.5 + refl.y * 0.5, 0.5 - refl.z * 0.5) - v.st) * env.w;
    float2 st = tms.xy * fst.x + tmt.xy * fst.y + float2(tms.z, tmt.z);
    float d = max(dot(n, ldir.xyz), 0.0);
    float4 col = min(amb + dirc * d, (float4)255.0);
    Out o;
    o.st = float4(st * q, q, 0);
    o.rgbaq = vu::rgba8(col * cscale);
    o.xyz2 = vu::ftoi4(p * q);
    o.xyz2.w = vu::adc(v.adc.x);
    return o;
}
