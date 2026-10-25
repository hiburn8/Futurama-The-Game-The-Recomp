// The XMV library's WMV2 (Windows Media Video 8) frame decoder (default.xbe section XMV, 0x00264400..0x0026C690),
// written by hand from the original and verified frame by frame against the game's own decoder run under Unicorn
// (tools/difftest/xmv_oracle.py). Tables (VLCs, run / level descriptors, scan orders, transform constants) are read
// from the user's default.xbe through XmvImage at their original addresses.
//
// Scope: what the game's movies use (docs/analysis/movies_port.md 1.1): I-frames (0x002660DE), P-frames (0x00266BB8)
// with mspel, ABT and per-MB run / level tables, the YUY2 packing (0x002652A2 / 0x002651AB). Not ported (not used by
// any file on the disc; DecodeFrame fails if a stream asks for them): J-frames (IntraX8, 0x0026C3AD), the loop filter
// (0x00268CBB), the RGB converters.
//
// Buffers follow the original (decoder +0xEC.. / +0xFC..): the "current" planes are the last frame handed to the
// display and the reference of the next P-frame; a frame is decoded into the "ahead" planes and becomes current on
// Swap(). Planes are (mbWidth * 16) x (mbHeight * 16) luma and half that chroma, with no border.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xmv {

class XmvImage;

class Wmv2Decoder {
public:
    enum class Result { Ok, Unsupported, NotCreated };

    // orig: XMVDecoder_CreateDecoderForPackets 0x00264400 (the video part). width / height from the XMV file header
    // (+0x14 / +0x18). firstTrackFormatTag: the first audio track's format tag (0 without tracks): Create copies the
    // track descriptors over the start of the macroblock flag row (decoder +0x160), so the byte left of macroblock
    // column 0 is that tag's low byte for the whole movie (only its bits 4 and 2 are ever read: 0 for 0x69).
    bool Create(const XmvImage& image, uint32_t width, uint32_t height, uint32_t firstTrackFormatTag);

    // orig: 0x00264C5A. The 4-byte extension header of the first video packet: bit 0 mspel, 1 loop filter, 2 ABT,
    // 3 J-frames, 4 top-left MV, 5 per-MB run / level, bits 6..8 the slice code.
    void SetExtension(uint32_t ext);

    // orig: 0x0026765B (+ the frame setup in GetNextFrame 0x00264CFE). Decodes one frame bitstream (the bytes after
    // the frame's 4-byte header, little-endian 32-bit words) into the ahead planes, using the current planes as the
    // reference. The decoder may read up to 8 bytes past `size` (VLC peeks), as the original reads into the packet
    // buffer: keep them readable.
    Result DecodeAhead(const uint8_t* data, size_t size);
    // orig: GetNextFrame's plane swap (0x00264D9C): the ahead frame becomes current.
    void Swap();
    // DecodeAhead + Swap: what GetNextFrame does with every frame it decodes (late frames are swapped too).
    Result DecodeFrame(const uint8_t* data, size_t size) {
        const Result r = DecodeAhead(data, size);
        Swap();
        return r;
    }

    // orig: 0x00265BAB / 0x002652A2 for a D3DFMT_YUY2 surface: the current frame as Y0 U Y1 V, each chroma row used for
    // two output rows; width x height pixels (the header's size), `pitch` bytes per output row.
    void ConvertToYuy2(uint8_t* dst, int32_t pitch) const;

    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    bool LastFrameWasIntra() const { return lastIntra_; }

    // The current planes (diagnostics / tests).
    const uint8_t* PlaneY() const { return cur_[0]; }
    const uint8_t* PlaneU() const { return cur_[1]; }
    const uint8_t* PlaneV() const { return cur_[2]; }
    int32_t StrideY() const { return int32_t(mbw_ * 16); }
    int32_t StrideC() const { return int32_t(mbw_ * 8); }

private:
    struct BitReader;
    struct RunLevel;
    struct Escape;
    struct IntraBlockCtx;

    Result DecodeIFrame(BitReader& br, uint32_t q);
    Result DecodePFrame(BitReader& br);
    void DecodeIntraMb(BitReader& br, uint32_t dcScale, uint32_t q, uint8_t cbp, uint32_t acPred,
                       const RunLevel& lumaRl, const RunLevel& chromaRl, Escape& esc, int32_t col,
                       uint8_t* y, uint8_t* u, uint8_t* v, int32_t* topLeft, int32_t* leftY, int32_t& topLeftU,
                       int32_t* leftU, int32_t& topLeftV, int32_t* leftV, uint32_t lumaDcVlc, uint32_t chromaDcVlc);
    void DecodeIntraBlock(BitReader& br, uint8_t* dst, int32_t stride, uint32_t dcScale, uint32_t q, bool coded,
                          uint32_t acPred, int32_t topLeftDc, int32_t* top, int32_t* left, uint32_t dcVlc,
                          const RunLevel& rl, Escape& esc);
    uint32_t DecodeInterCoefs(BitReader& br, uint32_t q, const RunLevel& rl, uint32_t scanVa, int16_t* coef,
                              Escape& esc);
    void DecodeInterBlock(BitReader& br, uint32_t q, bool coded, uint32_t abtType, uint32_t perBlockAbt,
                          const int8_t* mv, uint32_t mspel, uint32_t hshift, const RunLevel& rl, int32_t xPix,
                          int32_t yPix, int32_t planeW, int32_t planeH, int32_t stride, uint8_t* dst,
                          const uint8_t* ref, Escape& esc);
    RunLevel LoadRunLevel(uint32_t setVa, uint32_t index) const;
    const uint16_t* Vlc(uint32_t va) const;

    const XmvImage* image_ = nullptr;
    uint32_t width_ = 0, height_ = 0;     // +0x40 / +0x44
    uint32_t mbw_ = 0, mbh_ = 0;          // +0xDC / +0xE0
    // Extension flags (+0xA0..+0xA5, +0xA8).
    bool mspelBit_ = false, loopFilter_ = false, abtFlag_ = false, jType_ = false, topLeftMv_ = false,
         perMbRlBit_ = false;
    uint32_t sliceCode_ = 1;
    uint32_t noRounding_ = 0;             // +0x114: 0 after an I-frame, toggled after every P-frame

    std::vector<uint8_t> planes_;         // current Y U V, then ahead Y U V
    uint8_t* cur_[3] = {};                // +0xEC / +0xF0 / +0xF4
    uint8_t* ahead_[3] = {};              // +0xFC / +0x100 / +0x104

    std::vector<uint8_t> mbFlags_;        // +0x118: [0] left of column 0, [1 + col] per macroblock (CBP bits)
    std::vector<int32_t> predY_;          // +0x108: 16 per column (the DC / AC predictors of the two lower blocks)
    std::vector<int32_t> predU_, predV_;  // +0x10C / +0x110: 8 per column
    std::vector<int8_t> mvRow_;           // +0x12C: (x, y) per column, entry 0 left of column 0, entry mbw + 1 right
    std::vector<uint32_t> skip_;          // +0x128: one bit per macroblock
    bool lastIntra_ = false;
};

} // namespace xmv
