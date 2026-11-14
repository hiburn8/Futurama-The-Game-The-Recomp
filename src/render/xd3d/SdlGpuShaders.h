// MSL shader source for SdlGpuDevice (matching fixedfunction.hlsl).
#pragma once

namespace fut::render::xd3d::sdl_shaders {

inline constexpr const char kMslShaderSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct RowMat {
    float4 r0;
    float4 r1;
    float4 r2;
    float4 r3;
};

inline float4 mulRow(float4 v, RowMat m) {
    return v.x * m.r0 + v.y * m.r1 + v.z * m.r2 + v.w * m.r3;
}

struct Light {
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 pos;
    float4 dir;
    float4 att;
    float4 spot;
};

struct DrawCB {
    RowMat world;
    RowMat view;
    RowMat proj;
    RowMat texm[4];
    float4 vp;
    float4 ssOffset;
    float4 matDiffuse;
    float4 matAmbient;
    float4 matSpecular;
    float4 matEmissive;
    float4 ambient;
    float4 misc0;
    float4 misc1;
    float4 misc2;
    float4 misc3;
    float4 fogParams;
    float4 fogColor;
    float4 tfactor;
    float4 alphaTest;
    int4 tci[4];
    int4 colorOp[4];
    int4 alphaOp[4];
    int4 stageMisc[4];
    int4 lightCount;
    Light lights[8];
    float4 texScale[4];
};

struct VSIn {
    float4 pos [[attribute(0)]];
    float3 nrm [[attribute(1)]];
    float4 d0  [[attribute(2)]];
    float4 d1  [[attribute(3)]];
    float4 uv0 [[attribute(4)]];
    float4 uv1 [[attribute(5)]];
    float4 uv2 [[attribute(6)]];
    float4 uv3 [[attribute(7)]];
};

struct VSOut {
    float4 pos [[position]];
    float4 d0;
    float4 d1;
    float4 fd0 [[flat]];
    float4 fd1 [[flat]];
    float4 fog;
    float4 t0;
    float4 t1;
    float4 t2;
    float4 t3;
};

inline float4 SourceColor(int s, float4 vertexColor, float4 material, float4 misc2) {
    return (s == 1 && misc2.y > 0.5f) ? vertexColor
         : (s == 2 && misc2.z > 0.5f) ? vertexColor
         : material;
}

inline float4 TexCoord(constant DrawCB& cb, int stage, thread const float4 uv[4], float3 pv, float3 nv) {
    int4 c = cb.tci[stage];
    float4 t;
    if (c.y == 1) {
        t = float4(nv, 1.0f);
    } else if (c.y == 2) {
        t = float4(pv, 1.0f);
    } else if (c.y == 3) {
        float3 e = normalize(pv);
        t = float4(e - 2.0f * dot(nv, e) * nv, 1.0f);
    } else {
        t = uv[min(c.x, 3)];
        if (c.z > 0) {
            int dim = cb.stageMisc[stage].z;
            if (dim == 1) t = float4(t.x, 1.0f, 0.0f, 0.0f);
            else if (dim == 2) t = float4(t.xy, 1.0f, 0.0f);
            else if (dim == 3) t = float4(t.xyz, 1.0f);
        }
    }
    if (c.z > 0) {
        float4 r = mulRow(t, cb.texm[stage]);
        float4 o = float4(0.0f, 0.0f, 0.0f, 1.0f);
        o.x = r.x;
        if (c.z >= 2) o.y = r.y;
        if (c.z >= 3) o.z = r.z;
        if (c.z >= 4) o.w = r.w;
        if (c.w != 0) {
            float q = c.z == 2 ? r.y : (c.z == 3 ? r.z : r.w);
            o = float4(o.xy, 0.0f, q);
        } else {
            o.w = 1.0f;
        }
        return o;
    }
    return float4(t.xy, t.z, 1.0f);
}

vertex VSOut VSMain(VSIn i [[stage_in]], constant DrawCB& cb [[buffer(0)]]) {
    VSOut o;
    float3 pv = float3(0.0f);
    float3 nv = float3(0.0f, 0.0f, 1.0f);

    if (cb.misc2.w > 0.5f) {
        float w = i.pos.w != 0.0f ? 1.0f / i.pos.w : 1.0f;
        float2 s = i.pos.xy + cb.ssOffset.xy + 0.53125f * cb.ssOffset.zw - cb.vp.xy;
        o.pos = float4((s.x / cb.vp.z * 2.0f - 1.0f) * w, (1.0f - s.y / cb.vp.w * 2.0f) * w, i.pos.z * w, w);
        pv = float3(i.pos.xy, i.pos.z);
    } else {
        float4 pw = mulRow(float4(i.pos.xyz, 1.0f), cb.world);
        float4 v = mulRow(pw, cb.view);
        float4 c = mulRow(v, cb.proj);
        c.x += c.w / cb.vp.z * 2.0f * (0.53125f * cb.ssOffset.z + cb.ssOffset.x);
        c.y -= c.w / cb.vp.w * 2.0f * (0.53125f * cb.ssOffset.w + cb.ssOffset.y);
        o.pos = c;
        pv = v.xyz;
        if (cb.misc2.x > 0.5f) {
            nv = mulRow(mulRow(float4(i.nrm.xyz, 0.0f), cb.world), cb.view).xyz;
            if (cb.misc0.w > 0.5f) {
                float nl2 = dot(nv, nv);
                if (nl2 > 0.0f) nv = nv * rsqrt(nl2);
            }
        }
    }

    float4 cd = cb.misc2.y > 0.5f ? i.d0 : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float4 cs = cb.misc2.z > 0.5f ? i.d1 : float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (cb.misc0.y > 0.5f && cb.misc2.w < 0.5f) {
        bool cv = cb.misc0.z > 0.5f;
        float4 md = cv ? SourceColor(int(cb.misc1.x), int(cb.misc1.x) == 2 ? cs : cd, cb.matDiffuse, cb.misc2) : cb.matDiffuse;
        float4 ma = cv ? SourceColor(int(cb.misc1.y), int(cb.misc1.y) == 2 ? cs : cd, cb.matAmbient, cb.misc2) : cb.matAmbient;
        float4 ms = cv ? SourceColor(int(cb.misc1.z), int(cb.misc1.z) == 2 ? cs : cd, cb.matSpecular, cb.misc2) : cb.matSpecular;
        float4 me = cv ? SourceColor(int(cb.misc1.w), int(cb.misc1.w) == 2 ? cs : cd, cb.matEmissive, cb.misc2) : cb.matEmissive;
        float3 a = ma.rgb * cb.ambient.rgb;
        float3 dif = float3(0.0f);
        float3 spec = float3(0.0f);
        for (int k = 0; k < cb.lightCount.x; ++k) {
            Light L = cb.lights[k];
            float3 l;
            float att = 1.0f;
            float sp = 1.0f;
            if (L.pos.w > 2.5f) {
                l = -normalize(L.dir.xyz);
            } else {
                float3 d = L.pos.xyz - pv;
                float dist = length(d);
                l = d / max(dist, 1e-20f);
                att = dist <= L.dir.w ? 1.0f / (L.att.x + L.att.y * dist + L.att.z * dist * dist) : 0.0f;
                if (L.pos.w > 1.5f) {
                    float rho = dot(-l, normalize(L.dir.xyz));
                    if (rho > L.spot.x) {
                        sp = 1.0f;
                    } else if (rho <= L.spot.y) {
                        sp = 0.0f;
                    } else {
                        float sb = saturate((rho - L.spot.y) / max(L.spot.x - L.spot.y, 1e-20f));
                        sp = sb > 0.0f ? pow(sb, L.att.w) : (L.att.w == 0.0f ? 1.0f : 0.0f);
                    }
                }
            }
            float k2 = att * sp;
            a += k2 * ma.rgb * L.ambient.rgb;
            float nl = dot(nv, l);
            dif += k2 * max(0.0f, nl) * L.diffuse.rgb * md.rgb;
            if (cb.misc3.x > 0.5f && nl > 0.0f) {
                float3 e = cb.misc3.y > 0.5f ? normalize(-pv) : float3(0.0f, 0.0f, 1.0f);
                float3 h = normalize(e + l);
                float ndh = max(0.0f, dot(nv, h));
                float p = ndh > 0.0f ? pow(ndh, cb.misc0.x) : (cb.misc0.x == 0.0f ? 1.0f : 0.0f);
                spec += k2 * p * L.specular.rgb * ms.rgb;
            }
        }
        cd = float4(saturate(me.rgb + a + dif), saturate(md.a));
        cs = float4(cb.misc3.x > 0.5f ? saturate(spec) : float3(0.0f), 0.0f);
    }
    o.d0 = cd;
    o.d1 = cs;
    o.fd0 = cd;
    o.fd1 = cs;

    float f = 1.0f;
    if (cb.misc3.z > 0.5f) {
        int gen = int(cb.fogParams.w);
        float d = gen == 1 ? length(pv) : pv.z;
        int m = int(cb.misc3.w);
        if (gen == 0) f = cs.a;
        else if (m == 0) f = cb.fogParams.x + cb.fogParams.y * d - 1.0f;
        else if (m == 1) f = cb.fogParams.x + exp2(16.0f * cb.fogParams.y * d) - 1.5f;
        else f = cb.fogParams.x + exp2(-32.0f * (cb.fogParams.y * d) * (cb.fogParams.y * d)) - 1.5f;
    }
    o.fog = float4(f, 0.0f, 0.0f, 0.0f);
    float4 uv[4] = {i.uv0, i.uv1, i.uv2, i.uv3};
    o.t0 = TexCoord(cb, 0, uv, pv, nv);
    o.t1 = TexCoord(cb, 1, uv, pv, nv);
    o.t2 = TexCoord(cb, 2, uv, pv, nv);
    o.t3 = TexCoord(cb, 3, uv, pv, nv);
    return o;
}

inline float4 ArgFull(int a, float4 diffuse, float4 current, float4 tex, float4 tfac, float4 spec, float4 temp) {
    int s = a & 15;
    float4 v = s == 0 ? diffuse : s == 1 ? current : s == 2 ? tex : s == 3 ? tfac : s == 4 ? spec : temp;
    if ((a & 0x20) != 0) v = v.aaaa;
    if ((a & 0x10) != 0) v = 1.0f - v;
    return v;
}

inline float4 StageOp(int op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfac) {
    if (op == 2) return a1;
    if (op == 3) return a2;
    if (op == 4) return a1 * a2;
    if (op == 5) return a1 * a2 * 2.0f;
    if (op == 6) return a1 * a2 * 4.0f;
    if (op == 7) return a1 + a2;
    if (op == 8) return a1 + a2 - 0.5f;
    if (op == 9) return (a1 + a2 - 0.5f) * 2.0f;
    if (op == 10) return a1 - a2;
    if (op == 11) return a1 + a2 - a1 * a2;
    if (op == 12) return mix(a2, a1, diffuse.a);
    if (op == 13) return mix(a2, a1, current.a);
    if (op == 14) return mix(a2, a1, tex.a);
    if (op == 15) return mix(a2, a1, tfac.a);
    if (op == 16) return a1 + a2 * (1.0f - tex.a);
    if (op == 18) return float4(a1.rgb + a1.a * a2.rgb, a1.a);
    if (op == 19) return float4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);
    if (op == 20) return float4((1.0f - a1.a) * a2.rgb + a1.rgb, a1.a);
    if (op == 21) return float4((1.0f - a1.rgb) * a2.rgb + a1.a, a1.a);
    if (op == 22) {
        float d = saturate(dot(a1.rgb - 0.5f, a2.rgb - 0.5f) * 4.0f);
        return float4(d, d, d, d);
    }
    if (op == 23) return a0 + a1 * a2;
    if (op == 24) return mix(a2, a1, a0);
    return a1;
}

inline float4 YuvToRgb(float4 c) {
    float y = (c.r * 255.0f - 16.0f) * 38139.0f;
    float u = c.g * 255.0f - 128.0f;
    float v = c.b * 255.0f - 128.0f;
    float3 rgb = float3(y + 52298.0f * v, y - 12812.0f * u - 26640.0f * v, y + 66126.0f * u) / 32768.0f;
    return float4(saturate(rgb / 255.0f), c.a);
}

inline float4 SampleStage(constant DrawCB& cb, int s, float4 t,
                          texture2d<float> t0, sampler s0,
                          texture2d<float> t1, sampler s1,
                          texture2d<float> t2, sampler s2,
                          texture2d<float> t3, sampler s3) {
    float2 uv = (t.w != 0.0f && t.w != 1.0f) ? t.xy / t.w : t.xy;
    uv *= cb.texScale[s].xy;
    float4 c;
    if (s == 0) c = t0.sample(s0, uv);
    else if (s == 1) c = t1.sample(s1, uv);
    else if (s == 2) c = t2.sample(s2, uv);
    else c = t3.sample(s3, uv);
    return cb.texScale[s].z > 0.5f ? YuvToRgb(c) : c;
}

fragment float4 PSMain(VSOut i [[stage_in]],
                       constant DrawCB& cb [[buffer(0)]],
                       texture2d<float> tex0 [[texture(0)]], sampler samp0 [[sampler(0)]],
                       texture2d<float> tex1 [[texture(1)]], sampler samp1 [[sampler(1)]],
                       texture2d<float> tex2 [[texture(2)]], sampler samp2 [[sampler(2)]],
                       texture2d<float> tex3 [[texture(3)]], sampler samp3 [[sampler(3)]]) {
    float4 diffuse = cb.alphaTest.w > 0.5f ? i.fd0 : i.d0;
    float4 spec = cb.alphaTest.w > 0.5f ? i.fd1 : i.d1;
    float4 current = diffuse;
    float4 temp = float4(0.0f);
    float4 t[4] = {i.t0, i.t1, i.t2, i.t3};
    for (int s = 0; s < 4; ++s) {
        int4 c = cb.colorOp[s];
        int4 a = cb.alphaOp[s];
        if (c.x == 1) break;
        float4 tex = cb.stageMisc[s].y != 0
                   ? SampleStage(cb, s, t[s], tex0, samp0, tex1, samp1, tex2, samp2, tex3, samp3)
                   : float4(0.0f, 0.0f, 0.0f, 1.0f);
        float4 cr = StageOp(c.x,
                            ArgFull(c.y, diffuse, current, tex, cb.tfactor, spec, temp),
                            ArgFull(c.z, diffuse, current, tex, cb.tfactor, spec, temp),
                            ArgFull(c.w, diffuse, current, tex, cb.tfactor, spec, temp),
                            diffuse, current, tex, cb.tfactor);
        float4 ar = a.x == 1 ? current
                  : StageOp(a.x,
                            ArgFull(a.y | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                            ArgFull(a.z | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                            ArgFull(a.w | 0x20, diffuse, current, tex, cb.tfactor, spec, temp),
                            diffuse, current, tex, cb.tfactor);
        float4 r = saturate(float4(cr.rgb, ar.a));
        if (cb.stageMisc[s].x == 5) temp = r;
        else current = r;
    }
    float4 c = current;
    if (cb.misc3.x > 0.5f) c.rgb = saturate(c.rgb + spec.rgb);
    if (cb.misc3.z > 0.5f) c.rgb = mix(cb.fogColor.rgb, c.rgb, saturate(i.fog.x));
    if (cb.alphaTest.x > 0.5f) {
        int f = int(cb.alphaTest.y);
        float r = cb.alphaTest.z;
        float v = round(c.a * 255.0f) / 255.0f;
        bool ok = f == 7 || (f == 1 && v < r) || (f == 2 && v == r) || (f == 3 && v <= r) ||
                  (f == 4 && v > r) || (f == 5 && v != r) || (f == 6 && v >= r);
        if (!ok) discard_fragment();
    }
    return c;
}

struct ClearCB {
    float4 clearRect;
    float4 clearColor;
    float4 clearZ;
};

struct ClearOut {
    float4 pos [[position]];
};

vertex ClearOut ClearVS(uint id [[vertex_id]], constant ClearCB& cb [[buffer(0)]]) {
    ClearOut o;
    float2 p = float2((id & 1u) != 0 ? cb.clearRect.z : cb.clearRect.x,
                      (id & 2u) != 0 ? cb.clearRect.w : cb.clearRect.y);
    o.pos = float4(p, cb.clearZ.x, 1.0f);
    return o;
}

fragment float4 ClearPS(ClearOut i [[stage_in]], constant ClearCB& cb [[buffer(0)]]) {
    (void)i;
    return cb.clearColor;
}

struct BlitOut {
    float4 pos [[position]];
    float2 uv;
};

vertex BlitOut BlitVS(uint id [[vertex_id]]) {
    BlitOut o;
    o.uv = float2(float(id & 1u), float(id >> 1u));
    o.pos = float4(o.uv.x * 2.0f - 1.0f, 1.0f - o.uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

fragment float4 BlitPS(BlitOut i [[stage_in]],
                       texture2d<float> blitTex [[texture(0)]],
                       sampler blitSamp [[sampler(0)]]) {
    return float4(blitTex.sample(blitSamp, i.uv).rgb, 1.0f);
}
)MSL";

}  // namespace fut::render::xd3d::sdl_shaders
