// The D3D11 backend's shaders (src/render/xd3d/D3D11Device.cpp: the fixed-function pipeline of the Xbox D3D8
// stream, the clear quad, the presentation blit), compiled ahead of time by tools/compile_shaders.py into
// ShaderBlobs.h. Edit this file, run the tool, commit both.
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
    centroid float4 t0 : TEXCOORD0; centroid float4 t1 : TEXCOORD1; centroid float4 t2 : TEXCOORD2;
    centroid float4 t3 : TEXCOORD3;
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
    // viewport constants 0x00238000 subtract 0 (0.5 only in a multisample mode; the original game's surfaces are not
    // multisampled, 0x00146210: 0x11): the NV2A samples at pixel centres (+0.5, as D3D11), so the geometry moves by
    // 0.53125 + (x, y) pixels: D3D8's integer pixel centres plus 1 / 32 that keeps edges on integer coordinates off the
    // sample points (without it thin quads such as NiOutline's edge strips break up into dots). Under the render
    // scale the 0.53125 is a PHYSICAL pixel (it is about the raster the GPU samples): ssOffset.zw = logical / physical
    // pixels of the target (1 at scale 1); the game's (x, y) stays logical.
    if (misc2.w > 0.5) {                               // XYZRHW: screen pixels
        float w = i.pos.w != 0 ? 1 / i.pos.w : 1;
        float2 s = i.pos.xy + ssOffset.xy + 0.53125 * ssOffset.zw - vp.xy;
        o.pos = float4((s.x / vp.z * 2 - 1) * w, (1 - s.y / vp.w * 2) * w, i.pos.z * w, w);
        pv = float3(i.pos.xy, i.pos.z);
    } else {
        precise float4 pw = mul(float4(i.pos.xyz, 1), world);
        precise float4 v = mul(pw, view);
        precise float4 c = mul(v, proj);
        c.x += c.w / vp.z * 2 * (0.53125 * ssOffset.z + ssOffset.x);   // the screen-space offset above, in clip units
        c.y -= c.w / vp.w * 2 * (0.53125 * ssOffset.w + ssOffset.y);
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

// The presentation (D3D11Device::Swap): the Xbox back buffer drawn over the viewport (the 4:3 rectangle of the window).
Texture2D blitTex : register(t4);
SamplerState blitSamp : register(s4);
struct BlitOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
BlitOut BlitVS(uint id : SV_VertexID) {
    BlitOut o;
    o.uv = float2(id & 1, id >> 1);
    o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
    return o;
}
float4 BlitPS(BlitOut i) : SV_Target { return float4(blitTex.Sample(blitSamp, i.uv).rgb, 1); }
