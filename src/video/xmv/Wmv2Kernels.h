// The XMV library's WMV2 pixel kernels (default.xbe section XMV), written by hand from what each routine computes
// (Wmv2Kernels.cpp, with `// orig:` addresses); integer-exact, verified frame by frame against the game's own decoder
// (docs/analysis/movies_port.md 5).
// Strides are signed byte strides; "fx / fy" are the motion vector's half-pel bits (0 / 1).
#pragma once

#include <cstdint>

namespace xmv {
class XmvImage;
}

namespace xmv::kernels {

// The kernels read their constants (IDCT / filter words at 0x0026C690..) from the loaded section.
void SetImage(const XmvImage* image);

// orig: 0x002651AB. One 16 x 16 macroblock to YUY2 (Y0 U Y1 V), each chroma row used for two output rows.
void Yuy2Block16(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t yStride, int32_t cStride, uint8_t* dst,
                 int32_t dstPitch);
// orig: 0x002664CD. Skipped macroblock: 16 x 16 luma and 8 x 8 + 8 x 8 chroma copied from the reference.
void CopyMb(const uint8_t* refY, const uint8_t* refU, const uint8_t* refV, uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
            int32_t yStride, int32_t cStride);
// orig: 0x00266536. Edge emulation: `rows` rows of 16 bytes, shifted by `shift` bytes with the edge pixel replicated
// (mode -1: left edge, 1: right edge, 0: none), then the last row repeated up to 11 rows.
void EdgeCopy(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t shift, int32_t mode,
              int32_t rows);
// orig: 0x002676C6. Intra 8 x 8 inverse DCT of 64 int32 coefficients, stored with saturation (put).
void IdctIntra(uint8_t* dst, int32_t stride, const int32_t* coef);
// orig: 0x00267B33. Inter 8 x 8 inverse DCT of 64 int16 coefficients into 64 int16 residuals; `mask` = the columns
// with a nonzero coefficient below row 0 (0x00266672's return).
void IdctInter(int16_t* out, const int16_t* coef, uint32_t mask);
// orig: 0x002681B5 / 0x0026842C. ABT 8x4 / 4x8 inverse transforms of one half (0 / 1) into `out` (64 int16).
void Abt8x4(int16_t* out, const int16_t* coef, int32_t half);
void Abt4x8(int16_t* out, const int16_t* coef, int32_t half);
// orig: 0x00268E15 (rnd 0) / 0x00269096 (rnd 1): 8 x 8 half-pel prediction, put.
void McCopy(int32_t rnd, const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx,
            int32_t fy);
// orig: 0x0026940E (rnd 0) / 0x0026960D (rnd 1): 8 x 8 half-pel prediction + residual.
void McAdd(int32_t rnd, const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
           const int16_t* residual);
// orig: 0x002697F2. 8 x 8 luma prediction with the mspel filter, put.
void MspelCopy(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
               int32_t hshift);
// orig: 0x00269E5F. 8 x 8 luma prediction with the mspel filter + residual.
void MspelAdd(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
              int32_t hshift, const int16_t* residual);

} // namespace xmv::kernels
