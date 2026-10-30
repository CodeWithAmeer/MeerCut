// HDR sources (M4). MeerCut is an SDR Rec.709 editor and makes no HDR claims: it never outputs HDR. Video whose
// transfer characteristic is PQ (SMPTE ST 2084) or HLG (ARIB STD-B67) is converted to SDR on decoding instead of
// being shown as washed-out, wrongly mapped SDR. The conversion is a simple, documented tone mapping (not a
// grading tool; a colourist's HDR->SDR conversion will look better):
//   1. YUV -> 16-bit R'G'B' with the stream's matrix (BT.2020)
//   2. EOTF -> display light in nits: PQ absolute; HLG via its inverse OETF and the BT.2100 OOTF for a 1000-nit
//      display (system gamma 1.2)
//   3. BT.2020 -> BT.709 primaries in linear light (when the stream says BT.2020); out-of-gamut colours clipped
//   4. relative to HDR reference white 203 nits (ITU-R BT.2408) = SDR 100 %; below 75 % unchanged, above it the
//      brightest channel is compressed smoothly (extended Reinhard) so that the content peak (1000 nits by default)
//      lands on 100 %; all three channels are scaled together (hue kept)
//   5. inverse BT.1886 (gamma 2.4) -> 8-bit R'G'B'
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fa {

enum class HdrTransfer { None, PQ, HLG };

HdrTransfer hdrTransferFromName(const std::string& avTransferName);   // "smpte2084", "arib-std-b67", ...
const char* hdrTransferLabel(HdrTransfer t);                         // "PQ (SMPTE ST 2084)", ...

class HdrToSdr {
public:
    HdrToSdr(HdrTransfer transfer, bool bt2020Primaries, float peakNits = 1000.0f);
    // One row of 16-bit R'G'B' (RGB48, native endian) -> RGBA8 (alpha 255).
    void convertRow(const uint16_t* rgb48, uint8_t* rgba8, int width) const;
    // One pixel, signal 0..1 in -> SDR signal 0..1 out (for tests and documentation).
    std::array<float, 3> convert(float r, float g, float b) const;

    static float pqToNits(float signal);           // SMPTE ST 2084 EOTF
    static float hlgToScene(float signal);         // inverse HLG OETF, scene light 0..1
    float toneMap(float relative) const;           // relative to reference white (1 = 203 nits) -> 0..1

private:
    HdrTransfer transfer_;
    bool bt2020_;
    float peak_;                                   // content peak relative to reference white
    std::vector<float> eotf_;                      // 65536 entries: signal -> display light relative to 203 nits
    std::vector<uint8_t> oetf_;                    // 16384 entries on sqrt(SDR display light) -> 8-bit signal
};

}  // namespace fa
