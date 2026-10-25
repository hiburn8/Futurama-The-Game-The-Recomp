// The D3D11 backend; see D3D11Device.h. Semantics: renderer_lighting_port.md 9 (D3D8 / XDK; (L) where the NV2A may
// differ: xemu is the reference, renderer_plan.md 4).
#include "render/xd3d/D3D11Device.h"

#include "engine/ni/NiObjects.h"
#include "render/TextureDecode.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace fut::render::xd3d {
using Microsoft::WRL::ComPtr;

namespace {

void NotPorted(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// The fixed-function pipeline (renderer_lighting_port.md 9). Row vectors: v * M.
const char kShader[] = R"(
struct Light {
    float4 diffuse, specular, ambient;
    float4 pos;      // view space, w: type (1 point, 2 spot, 3 directional)
    float4 dir;      // view space, w: range
    float4 att;      // a0, a1, a2, falloff
    float4 spot;     // cos(theta / 2), cos(phi / 2)
};
cbuffer Draw : register(b0) {
    row_major float4x4 world, view, proj;
    row_major float4x4 texm[4];
    float4 vp;            // x, y, width, height of the viewport
    float4 ssOffset;      // the screen-space offset (xy)
    float4 matDiffuse, matAmbient, matSpecular, matEmissive;
    float4 ambient;       // RS AMBIENT
    float4 misc0;         // power, lighting, colorVertex, normalizeNormals
    float4 misc1;         // diffuse / ambient / specular / emissive material sources (0 material, 1 COLOR1, 2 COLOR2)
    float4 misc2;         // hasNormal, hasDiffuse, hasSpecular, rhw
    float4 misc3;         // specularEnable, localViewer, fogEnable, fog mode (0 linear 0x2601, 1 exp 0x800, 2 exp2 0x801)
    float4 fogParams;     // K0, K1 (the XDK's fog constants), -, generation (0 specular alpha, 1 radial, 2 planar)
    float4 fogColor;
    float4 tfactor;
    float4 alphaTest;     // enable, func (0 never .. 7 always), ref, flat shading
    int4 tci[4];          // coordinate set, generation (0 pass, 1 normal, 2 position, 3 reflection), count, projected
    int4 colorOp[4];      // op, arg1, arg2, arg0
    int4 alphaOp[4];
    int4 stageMisc[4];    // resultArg (1 current, 5 temp), texture bound, set dimension
    int4 lightCount;
    Light lights[8];
    float4 texScale[4];   // uv scale (a linear texture's texel coordinates: 1 / size), z: the YUV conversion
};
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
Texture2D tex3 : register(t3);
SamplerState samp0 : register(s0);
SamplerState samp1 : register(s1);
SamplerState samp2 : register(s2);
SamplerState samp3 : register(s3);

struct VSIn {
    float4 pos : POSITION; float4 nrm : NORMAL; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float4 uv0 : TEXCOORD0; float4 uv1 : TEXCOORD1; float4 uv2 : TEXCOORD2; float4 uv3 : TEXCOORD3;
};
struct VSOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0; float4 d1 : COLOR1;
    nointerpolation float4 fd0 : COLOR2; nointerpolation float4 fd1 : COLOR3;
    float4 fog : TEXCOORD4;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
};

float4 Source(int s, float4 vertexColor, float4 material) {
    return s == 1 && misc2.y > 0.5 ? vertexColor : s == 2 && misc2.z > 0.5 ? vertexColor : material;
}

float4 TexCoord(int stage, float4 uv[4], float3 pv, float3 nv) {
    int4 c = tci[stage];
    float4 t;
    if (c.y == 1) t = float4(nv, 1);
    else if (c.y == 2) t = float4(pv, 1);
    else if (c.y == 3) { float3 e = normalize(pv); t = float4(e - 2 * dot(nv, e) * nv, 1); }
    else {
        t = uv[min(c.x, 3)];
        if (c.z > 0) {                                 // a passthrough set with a transform: (u, v, 1, 0) (L)
            int dim = stageMisc[stage].z;
            if (dim == 1) t = float4(t.x, 1, 0, 0);
            else if (dim == 2) t = float4(t.xy, 1, 0);
            else if (dim == 3) t = float4(t.xyz, 1);
        }
    }
    if (c.z > 0) {
        float4 r = mul(t, texm[stage]);
        float4 o = float4(0, 0, 0, 1);
        o.x = r.x;
        if (c.z >= 2) o.y = r.y;
        if (c.z >= 3) o.z = r.z;
        if (c.z >= 4) o.w = r.w;
        if (c.w != 0) {                                // PROJECTED: divided by the last component (in the PS)
            float q = c.z == 2 ? r.y : c.z == 3 ? r.z : r.w;
            o = float4(o.xy, 0, q);
        } else {
            o.w = 1;
        }
        return o;
    }
    return float4(t.xy, t.z, 1);
}

VSOut VSMain(VSIn i) {
    VSOut o;
    float3 pv = 0, nv = float3(0, 0, 1);
    // The screen-space offset: D3DDevice_SetScreenSpaceOffset 0x00235590 stores 0.53125 ([0x00242250]) + (x, y) and the
    // viewport constants 0x00238000 subtract 0 (0.5 only in a multisample mode; the game's surfaces are not
    // multisampled, 0x00146210: 0x11): the NV2A samples at pixel centres (+0.5, as D3D11), so the geometry moves by
    // 0.53125 + (x, y) pixels: D3D8's integer pixel centres plus 1 / 32 that keeps edges on integer coordinates off the
    // sample points (without it thin quads such as NiOutline's edge strips break up into dots).
    if (misc2.w > 0.5) {                               // XYZRHW: screen pixels
        float w = i.pos.w != 0 ? 1 / i.pos.w : 1;
        float2 s = i.pos.xy + ssOffset.xy + 0.53125 - vp.xy;
        o.pos = float4((s.x / vp.z * 2 - 1) * w, (1 - s.y / vp.w * 2) * w, i.pos.z * w, w);
        pv = float3(i.pos.xy, i.pos.z);
    } else {
        precise float4 pw = mul(float4(i.pos.xyz, 1), world);
        precise float4 v = mul(pw, view);
        precise float4 c = mul(v, proj);
        c.x += c.w / vp.z * 2 * (0.53125 + ssOffset.x);   // the screen-space offset above, in clip units
        c.y -= c.w / vp.w * 2 * (0.53125 + ssOffset.y);
        o.pos = c;
        pv = v.xyz;
        if (misc2.x > 0.5) {
            nv = mul(mul(float4(i.nrm.xyz, 0), world), view).xyz;
            if (misc0.w > 0.5) nv = normalize(nv);
        }
    }
    float4 cd = misc2.y > 0.5 ? i.d0 : float4(1, 1, 1, 1);
    float4 cs = misc2.z > 0.5 ? i.d1 : float4(0, 0, 0, 0);
    if (misc0.y > 0.5 && misc2.w < 0.5) {
        bool cv = misc0.z > 0.5;
        float4 md = cv ? Source((int)misc1.x, misc1.x == 2 ? cs : cd, matDiffuse) : matDiffuse;
        float4 ma = cv ? Source((int)misc1.y, misc1.y == 2 ? cs : cd, matAmbient) : matAmbient;
        float4 ms = cv ? Source((int)misc1.z, misc1.z == 2 ? cs : cd, matSpecular) : matSpecular;
        float4 me = cv ? Source((int)misc1.w, misc1.w == 2 ? cs : cd, matEmissive) : matEmissive;
        float3 a = ma.rgb * ambient.rgb, dif = 0, spec = 0;
        for (int k = 0; k < lightCount.x; ++k) {
            Light L = lights[k];
            float3 l;
            float att = 1, sp = 1;
            if (L.pos.w > 2.5) {
                l = -normalize(L.dir.xyz);
            } else {
                float3 d = L.pos.xyz - pv;
                float dist = length(d);
                l = d / max(dist, 1e-20);
                att = dist <= L.dir.w ? 1 / (L.att.x + L.att.y * dist + L.att.z * dist * dist) : 0;
                if (L.pos.w > 1.5) {
                    float rho = dot(-l, normalize(L.dir.xyz));
                    sp = rho > L.spot.x ? 1 : rho <= L.spot.y ? 0
                       : pow(saturate((rho - L.spot.y) / max(L.spot.x - L.spot.y, 1e-20)), L.att.w);
                }
            }
            float k2 = att * sp;
            a += k2 * ma.rgb * L.ambient.rgb;
            float nl = dot(nv, l);
            dif += k2 * max(0, nl) * L.diffuse.rgb * md.rgb;
            if (misc3.x > 0.5 && nl > 0) {
                float3 e = misc3.y > 0.5 ? normalize(-pv) : float3(0, 0, 1);   // (L: the non-local viewer)
                float3 h = normalize(e + l);
                spec += k2 * pow(max(0, dot(nv, h)), misc0.x) * L.specular.rgb * ms.rgb;
            }
        }
        cd = float4(saturate(me.rgb + a + dif), saturate(md.a));
        cs = float4(misc3.x > 0.5 ? saturate(spec) : float3(0, 0, 0), 0);
    }
    o.d0 = cd; o.d1 = cs; o.fd0 = cd; o.fd1 = cs;
    // orig: the XDK's fog state 0x0023D5C0 (K0 / K1 / mode / generation, below) with the fog plane (0, 0, 1, 0) of
    // SetShaderConstantMode 0x00237EA0: the depth is the eye-space z (signed), radial |eye| with RANGEFOGENABLE; the
    // NV2A's evaluation per xemu (L: per vertex, interpolated, clamped per pixel). renderer_census_port.md 5.
    float f = 1;
    if (misc3.z > 0.5) {
        int gen = (int)fogParams.w;
        float d = gen == 1 ? length(pv) : pv.z;
        int m = (int)misc3.w;
        if (gen == 0) f = cs.a;                        // table mode NONE: the vertex specular alpha
        else if (m == 0) f = fogParams.x + fogParams.y * d - 1.0;
        else if (m == 1) f = fogParams.x + exp2(16.0 * fogParams.y * d) - 1.5;
        else f = fogParams.x + exp2(-32.0 * (fogParams.y * d) * (fogParams.y * d)) - 1.5;
    }
    o.fog = float4(f, 0, 0, 0);
    float4 uv[4] = {i.uv0, i.uv1, i.uv2, i.uv3};
    o.t0 = TexCoord(0, uv, pv, nv);
    o.t1 = TexCoord(1, uv, pv, nv);
    o.t2 = TexCoord(2, uv, pv, nv);
    o.t3 = TexCoord(3, uv, pv, nv);
    return o;
}

float4 Arg(int a, float4 diffuse, float4 current, float4 tex, float4 spec, float4 temp) {
    int s = a & 15;
    float4 v = s == 0 ? diffuse : s == 1 ? current : s == 2 ? tex : s == 3 ? tfactor : s == 4 ? spec : temp;
    if (a & 0x20) v = v.aaaa;
    if (a & 0x10) v = 1 - v;
    return v;
}

// The texture-stage ops (Xbox numbering) on one channel set; `alpha` selects the alpha-only forms.
float4 Op(int op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfac) {
    if (op == 2) return a1;
    if (op == 3) return a2;
    if (op == 4) return a1 * a2;
    if (op == 5) return a1 * a2 * 2;
    if (op == 6) return a1 * a2 * 4;
    if (op == 7) return a1 + a2;
    if (op == 8) return a1 + a2 - 0.5;
    if (op == 9) return (a1 + a2 - 0.5) * 2;
    if (op == 10) return a1 - a2;
    if (op == 11) return a1 + a2 - a1 * a2;
    if (op == 12) return lerp(a2, a1, diffuse.a);
    if (op == 13) return lerp(a2, a1, current.a);
    if (op == 14) return lerp(a2, a1, tex.a);
    if (op == 15) return lerp(a2, a1, tfac.a);
    if (op == 16) return a1 + a2 * (1 - tex.a);
    if (op == 18) return float4(a1.rgb + a1.a * a2.rgb, a1.a);
    if (op == 19) return float4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);
    if (op == 20) return float4((1 - a1.a) * a2.rgb + a1.rgb, a1.a);
    if (op == 21) return float4((1 - a1.rgb) * a2.rgb + a1.a, a1.a);
    if (op == 22) { float d = saturate(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4); return float4(d, d, d, d); }
    if (op == 23) return a0 + a1 * a2;
    if (op == 24) return lerp(a2, a1, a0);
    return a1;
}

// NV097_SET_CONTROL0's colour-space conversion (D3DRS_YUVENABLE) of a YUY2 texel (r = Y, g = Cb, b = Cr). Stand-in
// (L): the XMV library's own software converter 0x002653FF / 0x002655EA (BT.601 studio range: y' = (Y - 16) 38139,
// R = y' + 52298 (Cr - 128), G = y' - 12812 (Cb - 128) - 26640 (Cr - 128), B = y' + 66126 (Cb - 128), / 2^15); the
// NV2A's coefficients and rounding are not known (docs/analysis/movies_port.md open question 1).
float4 YuvToRgb(float4 c) {
    float y = (c.r * 255 - 16) * 38139, u = c.g * 255 - 128, v = c.b * 255 - 128;
    float3 rgb = float3(y + 52298 * v, y - 12812 * u - 26640 * v, y + 66126 * u) / 32768;
    return float4(saturate(rgb / 255), c.a);
}

float4 Sample(int s, float4 t) {
    float2 uv = t.w != 0 && t.w != 1 ? t.xy / t.w : t.xy;
    uv *= texScale[s].xy;
    float4 c;
    if (s == 0) c = tex0.Sample(samp0, uv);
    else if (s == 1) c = tex1.Sample(samp1, uv);
    else if (s == 2) c = tex2.Sample(samp2, uv);
    else c = tex3.Sample(samp3, uv);
    return texScale[s].z > 0.5 ? YuvToRgb(c) : c;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 diffuse = alphaTest.w > 0.5 ? i.fd0 : i.d0;
    float4 spec = alphaTest.w > 0.5 ? i.fd1 : i.d1;
    float4 current = diffuse, temp = 0;
    float4 t[4] = {i.t0, i.t1, i.t2, i.t3};
    [unroll] for (int s = 0; s < 4; ++s) {
        int4 c = colorOp[s], a = alphaOp[s];
        if (c.x == 1) break;                           // DISABLE
        float4 tex = stageMisc[s].y ? Sample(s, t[s]) : float4(0, 0, 0, 1);
        float4 cr = Op(c.x, Arg(c.y, diffuse, current, tex, spec, temp), Arg(c.z, diffuse, current, tex, spec, temp),
                       Arg(c.w, diffuse, current, tex, spec, temp), diffuse, current, tex, tfactor);
        float4 ar = a.x == 1 ? current
                  : Op(a.x, Arg(a.y | 0x20, diffuse, current, tex, spec, temp),
                       Arg(a.z | 0x20, diffuse, current, tex, spec, temp), Arg(a.w | 0x20, diffuse, current, tex, spec, temp),
                       diffuse, current, tex, tfactor);
        float4 r = saturate(float4(cr.rgb, ar.a));
        if (stageMisc[s].x == 5) temp = r; else current = r;
    }
    float4 c = current;
    if (misc3.x > 0.5) c.rgb = saturate(c.rgb + spec.rgb);
    if (misc3.z > 0.5) c.rgb = lerp(fogColor.rgb, c.rgb, saturate(i.fog.x));
    if (alphaTest.x > 0.5) {
        int f = (int)alphaTest.y;
        float r = alphaTest.z, v = round(c.a * 255) / 255;
        bool ok = f == 7 || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r) || (f == 4 && v > r) ||
                  (f == 5 && v != r) || (f == 6 && v >= r);
        if (!ok) discard;
    }
    return c;
}

// The rectangle clear of a depth / stencil buffer (and colour, where ClearView is unavailable): a quad at z.
cbuffer ClearCB : register(b1) { float4 clearRect; float4 clearColor; float4 clearZ; };
float4 ClearVS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2(id & 1 ? clearRect.z : clearRect.x, id & 2 ? clearRect.w : clearRect.y);
    return float4(p, clearZ.x, 1);
}
float4 ClearPS() : SV_Target { return clearColor; }
)";

struct LightCB {
    f32 diffuse[4], specular[4], ambient[4], pos[4], dir[4], att[4], spot[4];
};
struct DrawCB {
    D3DMATRIX world, view, proj, texm[4];
    f32 vp[4], ssOffset[4];
    f32 matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
    f32 ambient[4], misc0[4], misc1[4], misc2[4], misc3[4], fogParams[4], fogColor[4], tfactor[4], alphaTest[4];
    s32 tci[4][4], colorOp[4][4], alphaOp[4][4], stageMisc[4][4], lightCount[4];
    LightCB lights[8];
    f32 texScale[4][4];
};
struct ClearCB {
    f32 rect[4], color[4], z[4];
};

struct CVertex {                                      // the canonical vertex the input layout reads
    f32 pos[4], nrm[3], d0[4], d1[4], uv[4][4];
};

void Color(u32 argb, f32 out[4]) {
    out[0] = f32((argb >> 16) & 0xFF) / 255.0f, out[1] = f32((argb >> 8) & 0xFF) / 255.0f;
    out[2] = f32(argb & 0xFF) / 255.0f, out[3] = f32(argb >> 24) / 255.0f;
}
f32 AsFloat(u32 v) {
    f32 f;
    std::memcpy(&f, &v, 4);
    return f;
}

D3D11_BLEND Blend(u32 v) {                            // the Xbox D3DBLEND values (GL)
    switch (v) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR;
    case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR;
    case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND AlphaBlend(D3D11_BLEND b) {
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}
D3D11_BLEND_OP BlendOp(u32 v) {
    switch (v) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;           // 0x8006
    }
}
D3D11_COMPARISON_FUNC Compare(u32 v) {                // D3DCMP 0x200 NEVER .. 0x207 ALWAYS
    static const D3D11_COMPARISON_FUNC m[8] = {D3D11_COMPARISON_NEVER,         D3D11_COMPARISON_LESS,
                                               D3D11_COMPARISON_EQUAL,         D3D11_COMPARISON_LESS_EQUAL,
                                               D3D11_COMPARISON_GREATER,       D3D11_COMPARISON_NOT_EQUAL,
                                               D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS};
    return m[(v - 0x200) & 7];
}
D3D11_STENCIL_OP StencilOp(u32 v) {
    switch (v) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;         // 0x1E00
    }
}
D3D11_TEXTURE_ADDRESS_MODE Address(u32 v) {
    switch (v) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;       // (L: clamp to edge)
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

// The texture's Xbox texels (the texture manager's CopyDataToSurface layout: the levels one after the other, the
// swizzled formats in the Swizzler's order, x bits then y bits from the lowest) as D3D11 levels: the 16 / 32-bit
// formats deswizzled and expanded to RGBA8 (the channel's v * 255 / max, rounded: L, NV2A expands by replicating
// the high bits), DXT1 / DXT3 / DXT5 as BC1 / BC2 / BC3. False for the formats not handled.
bool DecodeXboxTexels(const Resource& r, DecodedTexture& out) {
    struct Masks { u32 a, r, g, b, bytes; };
    Masks m{};
    switch (r.format) {
    case 0x02: m = {0x8000, 0x7C00, 0x03E0, 0x001F, 2}; break;   // A1R5G5B5
    case 0x03: m = {0, 0x7C00, 0x03E0, 0x001F, 2}; break;        // X1R5G5B5
    case 0x04: m = {0xF000, 0x0F00, 0x00F0, 0x000F, 2}; break;   // A4R4G4B4
    case 0x05: m = {0, 0xF800, 0x07E0, 0x001F, 2}; break;        // R5G6B5
    case 0x06: m = {0xFF000000u, 0xFF0000, 0xFF00, 0xFF, 4}; break;   // A8R8G8B8
    case 0x07: m = {0, 0xFF0000, 0xFF00, 0xFF, 4}; break;        // X8R8G8B8
    case 0x0C: case 0x0E: case 0x0F: break;
    default: return false;
    }
    out = DecodedTexture();
    out.width = r.width, out.height = r.height;
    const bool block = r.format == 0x0C || r.format == 0x0E || r.format == 0x0F;
    if (block) out.kind = r.format == 0x0C ? DecodedTexture::Kind::BC1 : r.format == 0x0E ? DecodedTexture::Kind::BC2
                                                                                        : DecodedTexture::Kind::BC3;
    auto channel = [](u32 v, u32 mask, u8 empty) {
        if (!mask) return empty;
        u32 shift = 0;
        while (!((mask >> shift) & 1)) ++shift;
        u32 bits = 0;
        for (u32 k = mask >> shift; k & 1; k >>= 1) ++bits;
        const u32 max = (1u << bits) - 1, x = (v & mask) >> shift;
        return static_cast<u8>((x * 255 + max / 2) / max);
    };
    size_t at = 0;
    for (u32 l = 0, w = r.width, h = r.height; l < std::max(r.levels, 1u); ++l, w = std::max(w / 2, 1u),
                                                                          h = std::max(h / 2, 1u)) {
        const size_t bytes = LevelBytes(r.format, w, h);
        if (at + bytes > r.data.size()) return false;
        const u8* src = r.data.data() + at;
        if (block) {
            out.levels.emplace_back(src, src + bytes);
        } else {
            u32 xmask = 0, ymask = 0;                 // (0x001556F0)
            for (u32 k = 1, bit = 1;; k <<= 1) {
                bool used = false;
                if (k < w) xmask |= bit, bit <<= 1, used = true;
                if (k < h) ymask |= bit, bit <<= 1, used = true;
                if (!used) break;
            }
            std::vector<u8> rgba(size_t(w) * h * 4);
            u32 yoff = 0;
            for (u32 y = 0; y < h; ++y) {
                u32 xoff = 0;
                for (u32 x = 0; x < w; ++x) {
                    const u8* p = src + size_t(xoff | yoff) * m.bytes;
                    const u32 v = m.bytes == 2 ? u32(p[0] | p[1] << 8) : u32(p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24);
                    u8* d = &rgba[(size_t(y) * w + x) * 4];
                    d[0] = channel(v, m.r, 0), d[1] = channel(v, m.g, 0), d[2] = channel(v, m.b, 0);
                    d[3] = channel(v, m.a, 0xFF);
                    xoff = (xoff - xmask) & xmask;
                }
                yoff = (yoff - ymask) & ymask;
            }
            out.levels.push_back(std::move(rgba));
        }
        at += bytes;
    }
    return true;
}

}  // namespace

struct D3D11Device::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11VertexShader> vs, clearVs;
    ComPtr<ID3D11PixelShader> ps, clearPs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> drawCB, clearCB, vb, ib;
    u32 vbSize = 0, ibSize = 0;
    struct Target {                                   // a D3D11 view of a colour / depth resource
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11ShaderResourceView> srv;
        u32 width = 0, height = 0;
    };
    std::unordered_map<const Resource*, Target> targets;   // render targets (the back buffer, rendered textures)
    struct Tex {
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Texture2D> texture;              // (a YUY2 texture: updated when its texels change)
        u32 revision = 0;
    };
    std::unordered_map<const Resource*, Tex> textures;
    ComPtr<ID3D11ShaderResourceView> white;
    std::map<std::vector<u32>, ComPtr<ID3D11BlendState>> blends;
    std::map<std::vector<u32>, ComPtr<ID3D11DepthStencilState>> depths;
    std::map<std::vector<u32>, ComPtr<ID3D11RasterizerState>> rasters;
    std::map<std::vector<u32>, ComPtr<ID3D11SamplerState>> samplers;
    std::vector<CVertex> verts;
    std::vector<u32> idx;

    Target* ColorTarget(Resource* r);
    Target* DepthTarget(Resource* r);
    ID3D11ShaderResourceView* Texture(Resource* r);
    ID3D11ShaderResourceView* Yuy2Texture(Resource& r);
    void Upload(ComPtr<ID3D11Buffer>& b, u32& size, const void* data, u32 bytes, UINT bind);
};

D3D11Device::D3D11Device() : impl_(std::make_unique<Impl>()) {}
D3D11Device::~D3D11Device() = default;

bool D3D11Device::Init(HWND__* window, std::string& error) {
    Impl& d = *impl_;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr;
    if (window) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 640, sd.BufferDesc.Height = 480;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = window;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &d.swapChain, &d.device, nullptr, &d.ctx);
    } else {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &d.device,
                               nullptr, &d.ctx);
    }
    if (FAILED(hr)) {
        char b[64];
        std::snprintf(b, sizeof b, "D3D11 device creation failed (0x%08lX)", static_cast<unsigned long>(hr));
        error = b;
        return false;
    }
    d.ctx.As(&d.ctx1);
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errors;
        if (SUCCEEDED(D3DCompile(kShader, sizeof kShader - 1, "fixedfunction.hlsl", nullptr, nullptr, entry, target,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
            return true;
        error = std::string("shader compile failed: ") +
                (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
        return false;
    };
    ComPtr<ID3DBlob> vsb, psb, cvb, cpb;
    if (!compile("VSMain", "vs_4_0", vsb) || !compile("PSMain", "ps_4_0", psb) || !compile("ClearVS", "vs_4_0", cvb) ||
        !compile("ClearPS", "ps_4_0", cpb))
        return false;
    d.device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &d.vs);
    d.device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &d.ps);
    d.device->CreateVertexShader(cvb->GetBufferPointer(), cvb->GetBufferSize(), nullptr, &d.clearVs);
    d.device->CreatePixelShader(cpb->GetBufferPointer(), cpb->GetBufferSize(), nullptr, &d.clearPs);
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(CVertex, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, d0), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, d1), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, uv[0]), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, uv[1]), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, uv[2]), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(CVertex, uv[3]), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    d.device->CreateInputLayout(il, 8, vsb->GetBufferPointer(), vsb->GetBufferSize(), &d.layout);
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = (sizeof(DrawCB) + 15) & ~15u;
    d.device->CreateBuffer(&bd, nullptr, &d.drawCB);
    bd.ByteWidth = (sizeof(ClearCB) + 15) & ~15u;
    d.device->CreateBuffer(&bd, nullptr, &d.clearCB);
    {                                                 // a white 1x1 texture (an unloadable source)
        const u32 px = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 1, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_IMMUTABLE, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{&px, 4, 4};
        ComPtr<ID3D11Texture2D> t;
        d.device->CreateTexture2D(&td, &init, &t);
        d.device->CreateShaderResourceView(t.Get(), nullptr, &d.white);
    }
    return d.ColorTarget(backBuffer()) && d.DepthTarget(depthBuffer());
}

D3D11Device::Impl::Target* D3D11Device::Impl::ColorTarget(Resource* r) {
    if (!r) return nullptr;
    Resource* key = r->parent ? r->parent : r;        // a texture's surface: the texture
    auto it = targets.find(key);
    if (it != targets.end()) return &it->second;
    Target t;
    t.width = std::max(key->width, 1u), t.height = std::max(key->height, 1u);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = t.width, td.Height = t.height, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex)) ||
        FAILED(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)))
        return nullptr;
    device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    return &(targets[key] = std::move(t));
}

D3D11Device::Impl::Target* D3D11Device::Impl::DepthTarget(Resource* r) {
    if (!r) return nullptr;
    auto it = targets.find(r);
    if (it != targets.end()) return &it->second;
    Target t;
    t.width = std::max(r->width, 1u), t.height = std::max(r->height, 1u);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = t.width, td.Height = t.height, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT, td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex)) ||
        FAILED(device->CreateDepthStencilView(t.tex.Get(), nullptr, &t.dsv)))
        return nullptr;
    return &(targets[r] = std::move(t));
}

ID3D11ShaderResourceView* D3D11Device::Impl::Texture(Resource* r) {
    if (!r) return nullptr;
    if (r->rendered) {                                // a render target's texture: its D3D11 target
        Target* t = ColorTarget(r);
        return t ? t->srv.Get() : white.Get();
    }
    auto it = textures.find(r);
    if (r->format == 0x24) return Yuy2Texture(*r);    // (its texels change every movie frame)
    if (it != textures.end()) return it->second.srv.Get();
    Tex& tex = textures[r];
    const auto* src = static_cast<const ni::NiObject*>(r->source);
    const auto* st = dynamic_cast<const ni::NiSourceTexture*>(src);
    DecodedTexture dt;
    // the Xbox texels the texture manager wrote; a texture made at its first use (its creator not ported) has none:
    // the source's NiPixelData decoded instead (L)
    const bool xbox = r->levels && !std::all_of(r->data.begin(), r->data.end(), [](u8 v) { return v == 0; }) &&
                      DecodeXboxTexels(*r, dt);
    if (!xbox && (!st || !st->pixelDataObj || !DecodePixelData(*st->pixelDataObj, dt))) {
        NotPorted("D3D11Device: a texture without decodable pixel data (white)");
        tex.srv = white;
        return tex.srv.Get();
    }
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    u32 block = 0;
    switch (dt.kind) {
    case DecodedTexture::Kind::BC1: fmt = DXGI_FORMAT_BC1_UNORM, block = 8; break;
    case DecodedTexture::Kind::BC2: fmt = DXGI_FORMAT_BC2_UNORM, block = 16; break;
    case DecodedTexture::Kind::BC3: fmt = DXGI_FORMAT_BC3_UNORM, block = 16; break;
    default: break;
    }
    std::vector<D3D11_SUBRESOURCE_DATA> init(dt.levels.size());
    u32 w = dt.width, levels = 0;
    for (size_t i = 0; i < dt.levels.size(); ++i, ++levels) {
        if (block && (std::max(w, 1u) < 4 && i > 0)) break;   // (BC levels below 4 texels: the chain is cut)
        init[i].pSysMem = dt.levels[i].data();
        init[i].SysMemPitch = block ? std::max(1u, (w + 3) / 4) * block : w * 4;
        w = std::max(1u, w / 2);
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = dt.width, td.Height = dt.height, td.MipLevels = std::max(levels, 1u), td.ArraySize = 1;
    td.Format = fmt, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    if ((block && (dt.width % 4 || dt.height % 4)) || FAILED(device->CreateTexture2D(&td, init.data(), &t)) ||
        FAILED(device->CreateShaderResourceView(t.Get(), nullptr, &tex.srv))) {
        NotPorted("D3D11Device: a texture the device cannot create (white)");
        tex.srv = white;
    }
    return tex.srv.Get();
}

// D3DFMT_YUY2 (0x24, linear: the movie player's texture, docs/analysis/movies_port.md 2.6): Y0 U Y1 V per pixel pair,
// expanded to one texel per pixel (r = Y, g = U, b = V, a = 1: each pair's chroma for both, L: the NV2A's 4:2:2
// fetch) and re-uploaded when LockRect was called since the last upload; the conversion is the shader's YuvToRgb.
ID3D11ShaderResourceView* D3D11Device::Impl::Yuy2Texture(Resource& r) {
    Tex& tex = textures[&r];
    const u32 w = r.width & ~1u, h = r.height;
    if (!w || !h || r.data.size() < size_t(w) * h * 2) return white.Get();
    if (!tex.texture) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &tex.texture)) ||
            FAILED(device->CreateShaderResourceView(tex.texture.Get(), nullptr, &tex.srv))) {
            tex.texture.Reset();
            return white.Get();
        }
        tex.revision = r.revision - 1;
    }
    if (tex.revision != r.revision) {
        tex.revision = r.revision;
        std::vector<u8> rgba(size_t(w) * h * 4);
        for (u32 y = 0; y < h; ++y) {
            const u8* s = r.data.data() + size_t(y) * w * 2;
            u8* d = rgba.data() + size_t(y) * w * 4;
            for (u32 x = 0; x < w; x += 2, s += 4, d += 8) {
                d[0] = s[0], d[1] = s[1], d[2] = s[3], d[3] = 0xFF;
                d[4] = s[2], d[5] = s[1], d[6] = s[3], d[7] = 0xFF;
            }
        }
        ctx->UpdateSubresource(tex.texture.Get(), 0, nullptr, rgba.data(), w * 4, 0);
    }
    return tex.srv.Get();
}

void D3D11Device::Impl::Upload(ComPtr<ID3D11Buffer>& b, u32& size, const void* data, u32 bytes, UINT bind) {
    if (!b || size < bytes) {
        size = std::max(bytes, size * 2);
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC, bd.BindFlags = bind, bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = size;
        b.Reset();
        device->CreateBuffer(&bd, nullptr, &b);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, data, bytes);
        ctx->Unmap(b.Get(), 0);
    }
}

void D3D11Device::Clear(u32 count, const D3DRECT* rects, u32 flags, u32 color, f32 z, u32 stencil) {
    XD3DDevice::Clear(count, rects, flags, color, z, stencil);
    Impl& d = *impl_;
    if (!d.device) return;
    Impl::Target* ct = d.ColorTarget(renderTarget);
    Impl::Target* dt = d.DepthTarget(depthStencil);
    if (!ct) return;
    f32 c[4];
    Color(color, c);
    // the rect: the given one, else the viewport (L: the XDK clears the viewport without rects)
    D3DRECT r{s32(viewport.X), s32(viewport.Y), s32(viewport.X + viewport.Width), s32(viewport.Y + viewport.Height)};
    if (rects && count) r = rects[0];
    if (count > 1) NotPorted("D3D11Device::Clear: more than one rectangle");
    const bool whole = r.x1 <= 0 && r.y1 <= 0 && u32(r.x2) >= ct->width && u32(r.y2) >= ct->height;
    if ((flags & 0xF0) && whole) d.ctx->ClearRenderTargetView(ct->rtv.Get(), c);
    else if ((flags & 0xF0) && d.ctx1) {
        const D3D11_RECT rr{r.x1, r.y1, r.x2, r.y2};
        d.ctx1->ClearView(ct->rtv.Get(), c, &rr, 1);
    }
    if (!(flags & 3) || !dt) return;
    if (whole) {
        d.ctx->ClearDepthStencilView(dt->dsv.Get(), ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) |
                                     ((flags & 2) ? D3D11_CLEAR_STENCIL : 0), z, u8(stencil));
        return;
    }
    // a depth / stencil rectangle: a quad at z with the depth test always passing and no colour writes
    ClearCB cb{};
    cb.rect[0] = f32(r.x1) / ct->width * 2 - 1, cb.rect[1] = 1 - f32(r.y1) / ct->height * 2;
    cb.rect[2] = f32(r.x2) / ct->width * 2 - 1, cb.rect[3] = 1 - f32(r.y2) / ct->height * 2;
    cb.z[0] = z;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(d.ctx->Map(d.clearCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof cb);
        d.ctx->Unmap(d.clearCB.Get(), 0);
    }
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = (flags & 1) != 0, ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.StencilEnable = (flags & 2) != 0, ds.StencilReadMask = 0xFF, ds.StencilWriteMask = 0xFF;
    ds.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                    D3D11_COMPARISON_ALWAYS};
    ds.BackFace = ds.FrontFace;
    ComPtr<ID3D11DepthStencilState> dss;
    d.device->CreateDepthStencilState(&ds, &dss);
    D3D11_BLEND_DESC bdsc = {};
    bdsc.RenderTarget[0].RenderTargetWriteMask = 0;
    ComPtr<ID3D11BlendState> bs;
    d.device->CreateBlendState(&bdsc, &bs);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = FALSE;
    ComPtr<ID3D11RasterizerState> rs;
    d.device->CreateRasterizerState(&rd, &rs);
    const D3D11_VIEWPORT v{0, 0, f32(ct->width), f32(ct->height), 0, 1};
    d.ctx->OMSetRenderTargets(1, ct->rtv.GetAddressOf(), dt->dsv.Get());
    d.ctx->RSSetViewports(1, &v);
    d.ctx->RSSetState(rs.Get());
    d.ctx->OMSetDepthStencilState(dss.Get(), u8(stencil));
    d.ctx->OMSetBlendState(bs.Get(), nullptr, 0xFFFFFFFF);
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    d.ctx->VSSetShader(d.clearVs.Get(), nullptr, 0);
    d.ctx->PSSetShader(d.clearPs.Get(), nullptr, 0);
    d.ctx->VSSetConstantBuffers(1, 1, d.clearCB.GetAddressOf());
    d.ctx->PSSetConstantBuffers(1, 1, d.clearCB.GetAddressOf());
    d.ctx->Draw(4, 0);
}

void D3D11Device::Swap(u32 flags) {
    XD3DDevice::Swap(flags);
    Impl& d = *impl_;
    if (!d.device) return;
    Impl::Target* bb = d.ColorTarget(backBuffer());
    if (!bb) return;
    if (d.swapChain) {
        ComPtr<ID3D11Texture2D> back;
        if (SUCCEEDED(d.swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) d.ctx->CopyResource(back.Get(), bb->tex.Get());
        d.swapChain->Present(1, 0);
    }
}

void D3D11Device::DrawVertices(u32 primitive, u32 start, u32 count) {
    XD3DDevice::DrawVertices(primitive, start, count);
    Draw(primitive, nullptr, count, start);
}

void D3D11Device::DrawIndexedVertices(u32 primitive, u32 count, const u16* data) {
    XD3DDevice::DrawIndexedVertices(primitive, count, data);
    Draw(primitive, data, count, 0);
}

void D3D11Device::Draw(u32 primitive, const u16* list, u32 count, u32 start) {
    Impl& d = *impl_;
    if (!d.device || !count || !streamSource || !streamStride) return;
    if (pixelShader) NotPorted("D3D11Device: a combiner pixel shader program (the texture stages are used)");
    const u32 fvf = vertexShader;
    if (fvf & 0xFFFF0000u & ~0x00FF0000u) NotPorted("D3D11Device: an FVF beyond the texture-coordinate sizes");
    // the referenced vertex range
    u32 lo = start, hi = start + count - 1;
    if (list) {
        lo = 0xFFFFFFFFu, hi = 0;
        for (u32 i = 0; i < count; ++i) lo = std::min<u32>(lo, list[i]), hi = std::max<u32>(hi, list[i]);
        lo += baseVertex, hi += baseVertex;
    }
    const u32 n = hi - lo + 1;
    const std::vector<u8>& src = streamSource->data;
    if (size_t(hi + 1) * streamStride > src.size()) return;
    // the FVF layout (D3D8: XYZ 0x002 / XYZRHW 0x004, NORMAL 0x010, DIFFUSE 0x040, SPECULAR 0x080, TEXCOUNT << 8,
    // the coordinate sizes at bit 16 + 2i: 0 two, 1 three, 2 four, 3 one float)
    const u32 posKind = fvf & 0x00E;
    const bool rhw = posKind == 0x004, hasN = (fvf & 0x010) != 0, hasD = (fvf & 0x040) != 0, hasS = (fvf & 0x080) != 0;
    const u32 texCount = (fvf >> 8) & 0xF;
    u32 dims[8] = {};
    for (u32 i = 0; i < texCount && i < 8; ++i) {
        const u32 sz = (fvf >> (16 + 2 * i)) & 3;
        dims[i] = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
    }
    d.verts.resize(n);
    for (u32 v = 0; v < n; ++v) {
        const u8* p = src.data() + size_t(lo + v) * streamStride;
        CVertex& c = d.verts[v];
        std::memset(&c, 0, sizeof c);
        const u32 pc = rhw ? 4 : 3;
        std::memcpy(c.pos, p, pc * 4);
        if (!rhw) c.pos[3] = 1;
        p += pc * 4;
        if (fvf & 0x020) p += 4;                      // (PSIZE)
        if (hasN) std::memcpy(c.nrm, p, 12), p += 12;
        if (hasD) {
            u32 a;
            std::memcpy(&a, p, 4);
            Color(a, c.d0);
            p += 4;
        }
        if (hasS) {
            u32 a;
            std::memcpy(&a, p, 4);
            Color(a, c.d1);
            p += 4;
        }
        for (u32 t = 0; t < texCount && t < 4; ++t) {
            std::memcpy(c.uv[t], p, dims[t] * 4);
            p += dims[t] * 4;
        }
    }
    // the primitive (Xbox numbering) as a D3D11 topology; fans and quads as lists
    d.idx.clear();
    auto at = [&](u32 i) -> u32 { return list ? u32(list[i]) + baseVertex - lo : start + i - lo; };
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (primitive) {
    case PT_POINTLIST: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; for (u32 i = 0; i < count; ++i) d.idx.push_back(at(i)); break;
    case PT_LINELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; for (u32 i = 0; i < count; ++i) d.idx.push_back(at(i)); break;
    case PT_LINESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; for (u32 i = 0; i < count; ++i) d.idx.push_back(at(i)); break;
    case PT_TRIANGLELIST: for (u32 i = 0; i < count; ++i) d.idx.push_back(at(i)); break;
    case PT_TRIANGLESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; for (u32 i = 0; i < count; ++i) d.idx.push_back(at(i)); break;
    case PT_TRIANGLEFAN:
    case PT_POLYGON:
        for (u32 i = 2; i < count; ++i) d.idx.insert(d.idx.end(), {at(0), at(i - 1), at(i)});
        break;
    case PT_QUADLIST:
        for (u32 i = 0; i + 3 < count; i += 4) d.idx.insert(d.idx.end(), {at(i), at(i + 1), at(i + 2), at(i), at(i + 2), at(i + 3)});
        break;
    default: NotPorted("D3D11Device: a primitive type (line loops, quad strips)"); return;
    }
    if (d.idx.empty()) return;

    // ---- the constants ----
    DrawCB cb{};
    cb.world = transforms[TS_WORLD], cb.view = transforms[TS_VIEW], cb.proj = transforms[TS_PROJECTION];
    for (u32 s = 0; s < 4; ++s) cb.texm[s] = transforms[TS_TEXTURE0 + s];
    cb.vp[0] = f32(viewport.X), cb.vp[1] = f32(viewport.Y), cb.vp[2] = f32(std::max(viewport.Width, 1u));
    cb.vp[3] = f32(std::max(viewport.Height, 1u));
    cb.ssOffset[0] = screenSpaceOffset[0], cb.ssOffset[1] = screenSpaceOffset[1];
    std::memcpy(cb.matDiffuse, material.diffuse, 16), std::memcpy(cb.matAmbient, material.ambient, 16);
    std::memcpy(cb.matSpecular, material.specular, 16), std::memcpy(cb.matEmissive, material.emissive, 16);
    const u32* rs = renderState;
    Color(rs[RS_AMBIENT], cb.ambient);
    cb.misc0[0] = material.power, cb.misc0[1] = f32(rs[RS_LIGHTING] != 0), cb.misc0[2] = f32(rs[RS_COLORVERTEX] != 0);
    cb.misc0[3] = f32(rs[RS_NORMALIZENORMALS] != 0);
    cb.misc1[0] = f32(rs[RS_DIFFUSEMATERIALSOURCE]), cb.misc1[1] = f32(rs[RS_AMBIENTMATERIALSOURCE]);
    cb.misc1[2] = f32(rs[RS_SPECULARMATERIALSOURCE]), cb.misc1[3] = f32(rs[RS_EMISSIVEMATERIALSOURCE]);
    cb.misc2[0] = f32(hasN), cb.misc2[1] = f32(hasD), cb.misc2[2] = f32(hasS), cb.misc2[3] = f32(rhw);
    cb.misc3[0] = f32(rs[RS_SPECULARENABLE] != 0), cb.misc3[1] = f32(rs[RS_LOCALVIEWER] != 0);
    cb.misc3[2] = f32(rs[RS_FOGENABLE] != 0);
    {   // orig: 0x0023D5C0, the XDK's lazy fog flush (x87 in the game's 53-bit precision: doubles, F() at the stores)
        const f32 fogStart = AsFloat(rs[RS_FOGSTART]), end = AsFloat(rs[RS_FOGEND]), dens = AsFloat(rs[RS_FOGDENSITY]);
        u32 gen = rs[RS_RANGEFOGENABLE] == 0 ? 2u : 1u, mode = 0;   // 2 PLANAR, 1 RADIAL; mode 0 = 0x2601 LINEAR
        f32 k0 = 1.0f, k1 = 1.0f;
        switch (rs[RS_FOGTABLEMODE]) {
        case 0:                                       // NONE: the vertex's specular alpha (SPEC_ALPHA)
            gen = 0;
            break;
        case 3: {                                     // LINEAR: FLD end; FSUB start; FDIVR 1.0 (8192 [0x00243308])
            const double sc = end == fogStart ? 8192.0 : 1.0 / (double(end) - double(fogStart));   // FCOMP; TEST AH,0x44
            k0 = f32(double(end) * sc + 1.0);         // FLD end; FMUL ST(1); FADD 1.0; FSTP
            k1 = f32(-sc);                            // FCHS; FSTP
            break;
        }
        case 1:                                       // EXP: [0x002A5F9C]
            k0 = 1.5f, k1 = f32(double(dens) * double(std::bit_cast<f32>(0xBDB8AA0Au))), mode = 1;
            break;
        default:                                      // EXP2: [0x002A5F98]
            k0 = 1.5f, k1 = f32(double(dens) * double(std::bit_cast<f32>(0xBE596D0Bu))), mode = 2;
            break;
        }
        cb.misc3[3] = f32(mode);
        cb.fogParams[0] = k0, cb.fogParams[1] = k1, cb.fogParams[2] = 0, cb.fogParams[3] = f32(gen);
    }
    Color(rs[RS_FOGCOLOR], cb.fogColor);
    Color(rs[RS_TEXTUREFACTOR], cb.tfactor);
    cb.alphaTest[0] = f32(rs[RS_ALPHATESTENABLE] != 0), cb.alphaTest[1] = f32((rs[RS_ALPHAFUNC] - 0x200) & 7);
    cb.alphaTest[2] = f32(rs[RS_ALPHAREF] & 0xFF) / 255.0f, cb.alphaTest[3] = f32(rs[RS_SHADEMODE] == 0x1D00);
    ID3D11ShaderResourceView* srv[4] = {};
    ID3D11SamplerState* smp[4] = {};
    for (u32 s = 0; s < 4; ++s) {
        const u32* ts = textureState[s];
        const u32 tci = ts[TSS_TEXCOORDINDEX], ttf = ts[TSS_TEXTURETRANSFORMFLAGS];
        cb.tci[s][0] = s32(tci & 0xFFFF), cb.tci[s][1] = s32((tci >> 16) & 0xF);
        if ((tci >> 16) > 3) NotPorted("D3D11Device: a texture-coordinate generation mode above 3");
        cb.tci[s][2] = s32(ttf & 0xFF), cb.tci[s][3] = (ttf & 0x100) != 0;
        cb.colorOp[s][0] = s32(ts[TSS_COLOROP]), cb.colorOp[s][1] = s32(ts[TSS_COLORARG1]);
        cb.colorOp[s][2] = s32(ts[TSS_COLORARG2]), cb.colorOp[s][3] = s32(ts[TSS_COLORARG0]);
        cb.alphaOp[s][0] = s32(ts[TSS_ALPHAOP]), cb.alphaOp[s][1] = s32(ts[TSS_ALPHAARG1]);
        cb.alphaOp[s][2] = s32(ts[TSS_ALPHAARG2]), cb.alphaOp[s][3] = s32(ts[TSS_ALPHAARG0]);
        if (ts[TSS_COLOROP] == 25 || ts[TSS_COLOROP] == 26) NotPorted("D3D11Device: the bump-env ops");
        cb.stageMisc[s][0] = s32(ts[TSS_RESULTARG]);
        cb.stageMisc[s][2] = s32((tci & 0xFFFF) < 8 ? dims[tci & 0xFFFF] : 2);
        cb.texScale[s][0] = cb.texScale[s][1] = 1.0f, cb.texScale[s][2] = 0.0f;
        if (Resource* t = textures[s]) {
            srv[s] = d.Texture(t);
            cb.stageMisc[s][1] = srv[s] != nullptr;
            if (t->format == 0x24 && t->width && t->height) {   // linear: texel coordinates (L: the NV2A convention)
                cb.texScale[s][0] = 1.0f / f32(t->width), cb.texScale[s][1] = 1.0f / f32(t->height);
                cb.texScale[s][2] = rs[RS_YUVENABLE] != 0 ? 1.0f : 0.0f;   // NV097_SET_CONTROL0 bit 28
            }
            std::vector<u32> key = {ts[TSS_ADDRESSU], ts[TSS_ADDRESSV], ts[TSS_ADDRESSW], ts[TSS_MAGFILTER],
                                    ts[TSS_MINFILTER], ts[TSS_MIPFILTER], ts[TSS_MIPMAPLODBIAS], ts[TSS_MAXMIPLEVEL],
                                    ts[TSS_BORDERCOLOR]};
            auto& sp = d.samplers[key];
            if (!sp) {
                D3D11_SAMPLER_DESC sd = {};
                const bool magL = ts[TSS_MAGFILTER] >= 2, minL = ts[TSS_MINFILTER] >= 2, mipL = ts[TSS_MIPFILTER] >= 2;
                sd.Filter = D3D11_FILTER((minL ? 0x10 : 0) | (magL ? 0x4 : 0) | (mipL ? 0x1 : 0));
                sd.AddressU = Address(ts[TSS_ADDRESSU]), sd.AddressV = Address(ts[TSS_ADDRESSV]);
                sd.AddressW = Address(ts[TSS_ADDRESSW]);
                sd.MipLODBias = AsFloat(ts[TSS_MIPMAPLODBIAS]);
                sd.MinLOD = f32(ts[TSS_MAXMIPLEVEL]);
                sd.MaxLOD = ts[TSS_MIPFILTER] == 0 ? sd.MinLOD : D3D11_FLOAT32_MAX;   // MIPFILTER NONE: the top level
                Color(ts[TSS_BORDERCOLOR], sd.BorderColor);
                sd.MaxAnisotropy = 1, sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                d.device->CreateSamplerState(&sd, &sp);
            }
            smp[s] = sp.Get();
        }
    }
    u32 nl = 0;
    for (const auto& [index, on] : lightEnabled) {
        if (!on || nl >= 8) continue;
        auto it = lights.find(index);
        if (it == lights.end()) continue;
        const D3DLIGHT8& l = it->second;
        LightCB& o = cb.lights[nl++];
        std::memcpy(o.diffuse, l.diffuse, 16), std::memcpy(o.specular, l.specular, 16);
        std::memcpy(o.ambient, l.ambient, 16);
        const D3DMATRIX& V = transforms[TS_VIEW];
        for (int c = 0; c < 3; ++c) {
            o.pos[c] = l.position[0] * V.m[0][c] + l.position[1] * V.m[1][c] + l.position[2] * V.m[2][c] + V.m[3][c];
            o.dir[c] = l.direction[0] * V.m[0][c] + l.direction[1] * V.m[1][c] + l.direction[2] * V.m[2][c];
        }
        o.pos[3] = f32(l.type), o.dir[3] = l.range;
        o.att[0] = l.attenuation0, o.att[1] = l.attenuation1, o.att[2] = l.attenuation2, o.att[3] = l.falloff;
        o.spot[0] = std::cos(l.theta * 0.5f), o.spot[1] = std::cos(l.phi * 0.5f);
    }
    if (lightEnabled.size() > 8 && nl == 8) NotPorted("D3D11Device: more than eight enabled lights");
    cb.lightCount[0] = s32(nl);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(d.ctx->Map(d.drawCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof cb);
        d.ctx->Unmap(d.drawCB.Get(), 0);
    }

    // ---- the fixed state objects ----
    std::vector<u32> bk = {rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND], rs[RS_DESTBLEND], rs[RS_BLENDOP],
                           rs[RS_COLORWRITEENABLE]};
    auto& bs = d.blends[bk];
    if (!bs) {
        D3D11_BLEND_DESC bd = {};
        auto& t = bd.RenderTarget[0];
        t.BlendEnable = rs[RS_ALPHABLENDENABLE] != 0;
        t.SrcBlend = Blend(rs[RS_SRCBLEND]), t.DestBlend = Blend(rs[RS_DESTBLEND]);
        t.SrcBlendAlpha = AlphaBlend(t.SrcBlend), t.DestBlendAlpha = AlphaBlend(t.DestBlend);
        t.BlendOp = t.BlendOpAlpha = BlendOp(rs[RS_BLENDOP]);
        const u32 cw = rs[RS_COLORWRITEENABLE];       // Xbox: red 1 << 16, green 1 << 8, blue 1, alpha 1 << 24
        t.RenderTargetWriteMask = u8(((cw & 0x10000) ? 1 : 0) | ((cw & 0x100) ? 2 : 0) | ((cw & 1) ? 4 : 0) |
                                     ((cw & 0x1000000) ? 8 : 0));
        d.device->CreateBlendState(&bd, &bs);
    }
    std::vector<u32> dk = {rs[RS_ZENABLE], rs[RS_ZWRITEENABLE], rs[RS_ZFUNC], rs[RS_STENCILENABLE],
                           rs[RS_STENCILFUNC], rs[RS_STENCILMASK], rs[RS_STENCILWRITEMASK], rs[RS_STENCILFAIL],
                           rs[RS_STENCILZFAIL], rs[RS_STENCILPASS]};
    auto& ds = d.depths[dk];
    if (!ds) {
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = rs[RS_ZENABLE] != 0;
        dd.DepthWriteMask = rs[RS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = Compare(rs[RS_ZFUNC]);
        dd.StencilEnable = rs[RS_STENCILENABLE] != 0;
        dd.StencilReadMask = u8(rs[RS_STENCILMASK]), dd.StencilWriteMask = u8(rs[RS_STENCILWRITEMASK]);
        dd.FrontFace = {StencilOp(rs[RS_STENCILFAIL]), StencilOp(rs[RS_STENCILZFAIL]), StencilOp(rs[RS_STENCILPASS]),
                        Compare(rs[RS_STENCILFUNC])};
        dd.BackFace = dd.FrontFace;
        d.device->CreateDepthStencilState(&dd, &ds);
    }
    std::vector<u32> rk = {rs[RS_CULLMODE], rs[RS_FILLMODE], rs[RS_SOLIDOFFSETENABLE], rs[RS_POLYGONOFFSETZOFFSET],
                           rs[RS_POLYGONOFFSETZSLOPESCALE], rs[RS_FRONTFACE]};
    auto& rz = d.rasters[rk];
    if (!rz) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = rs[RS_FILLMODE] == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        // CULLMODE 0x900 culls clockwise, 0x901 counter-clockwise triangles (screen space; FRONTFACE 0x901 swaps, L)
        const bool swap = rs[RS_FRONTFACE] == 0x901;
        rd.FrontCounterClockwise = FALSE;
        rd.CullMode = rs[RS_CULLMODE] == 0 ? D3D11_CULL_NONE
                    : (rs[RS_CULLMODE] == 0x900) != swap ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        if (rs[RS_SOLIDOFFSETENABLE]) {
            rd.DepthBias = INT(AsFloat(rs[RS_POLYGONOFFSETZOFFSET]));
            rd.SlopeScaledDepthBias = AsFloat(rs[RS_POLYGONOFFSETZSLOPESCALE]);
        }
        rd.DepthClipEnable = TRUE;
        d.device->CreateRasterizerState(&rd, &rz);
    }

    // ---- the draw ----
    Impl::Target* ct = d.ColorTarget(renderTarget);
    Impl::Target* dt = d.DepthTarget(depthStencil);
    if (!ct) return;
    ID3D11ShaderResourceView* none[4] = {};
    d.ctx->PSSetShaderResources(0, 4, none);         // (a texture that is the target is unbound first)
    d.ctx->OMSetRenderTargets(1, ct->rtv.GetAddressOf(), dt ? dt->dsv.Get() : nullptr);
    const D3D11_VIEWPORT v{f32(viewport.X), f32(viewport.Y), f32(viewport.Width), f32(viewport.Height),
                           viewport.MinZ, viewport.MaxZ};
    d.ctx->RSSetViewports(1, &v);
    d.ctx->RSSetState(rz.Get());
    f32 bf[4];
    Color(rs[RS_BLENDCOLOR], bf);
    d.ctx->OMSetBlendState(bs.Get(), bf, 0xFFFFFFFF);
    d.ctx->OMSetDepthStencilState(ds.Get(), u8(rs[RS_STENCILREF]));
    d.Upload(d.vb, d.vbSize, d.verts.data(), u32(d.verts.size() * sizeof(CVertex)), D3D11_BIND_VERTEX_BUFFER);
    d.Upload(d.ib, d.ibSize, d.idx.data(), u32(d.idx.size() * 4), D3D11_BIND_INDEX_BUFFER);
    const UINT stride = sizeof(CVertex), offset = 0;
    d.ctx->IASetVertexBuffers(0, 1, d.vb.GetAddressOf(), &stride, &offset);
    d.ctx->IASetIndexBuffer(d.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    d.ctx->IASetInputLayout(d.layout.Get());
    d.ctx->IASetPrimitiveTopology(topo);
    d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    d.ctx->PSSetShader(d.ps.Get(), nullptr, 0);
    d.ctx->VSSetConstantBuffers(0, 1, d.drawCB.GetAddressOf());
    d.ctx->PSSetConstantBuffers(0, 1, d.drawCB.GetAddressOf());
    for (u32 s = 0; s < 4; ++s)
        if (!srv[s]) srv[s] = d.white.Get();
    d.ctx->PSSetShaderResources(0, 4, srv);
    for (u32 s = 0; s < 4; ++s)
        if (!smp[s] && !d.samplers.empty()) smp[s] = d.samplers.begin()->second.Get();
    d.ctx->PSSetSamplers(0, 4, smp);
    d.ctx->DrawIndexed(UINT(d.idx.size()), 0, 0);
    ++draws_;
}

}  // namespace fut::render::xd3d
