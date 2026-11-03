// A shader stage's colours on VU1 (tr_shade.c RB_PS2_DrawStage), for the
// stages whose rgbGen and alphaGen are affine in the vertex colour:
// colour = vertex colour * cmul + cadd, each channel (identity, const,
// wave, entity: cmul 0; vertex: identityLight; oneMinusVertex: negative).
// A negative cadd in r, g, b: the vertex colour's hue (scaled to a largest
// channel of 255; white below 16) times -cadd / 255, the monochrome
// lightmaps' stand-in (RB_PS2_DrawStage's hue). Otherwise libgl's own
// program (libs/libgl/src/mxglv.hlsl), with its data layout (the same six
// uniforms): switching costs no FLUSH.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 cscale;
    float4 tms;
    float4 tmt;
    float4 cmul;
    float4 cadd;
};

struct In {
    float4 pos : POSITION;
    float2 st : TEXCOORD;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out q3stage(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    float2 st = tms.xy * v.st.x + tmt.xy * v.st.y + float2(tms.z, tmt.z);
    float4 c = float4(v.col);
    float m = max(max(c.x, c.y), c.z);
    // white below 16: the hue's scale is 0 and 255 is added
    float big = saturate(m - 15.0);
    float3 hue = c.xyz * (big * 255.0 * rcp(max(m, 1.0))) + (1.0 - big) * 255.0;
    float3 h = max(-cadd.xyz, (float3)0) * (1.0 / 255.0);
    float4 col = c * cmul + max(cadd, (float4)0) + float4(hue * h, 0);
    Out o;
    o.st = float4(st * q, q, 0);
    o.rgbaq = vu::rgba8(col * cscale);
    o.xyz2 = vu::ftoi4(p * q);
    o.xyz2.w = vu::adc(uint(v.pos.w));
    return o;
}
