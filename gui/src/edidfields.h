#pragma once
// Pure, bounded structural inspection. No host access, no EDID writes.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <iterator>
#include <vector>
namespace edidfields {
enum class Kind { Header, Manufacturer, Product, Serial, Manufacture, Version, Parameters, Chromaticity, Established, Standard, DetailedTiming, Descriptor, ExtensionCount, Checksum, ExtensionHeader, CtaData, DisplayIdData, DisplayIdChecksum, Padding, Unknown };
struct Field { std::size_t offset, length; Kind kind; int tag = -1; };
struct Block { std::size_t index; bool declared, checksum, structure; };
struct Inspection { bool header = false, complete = false; std::size_t declared = 0; std::vector<Block> blocks; std::vector<Field> fields; };
inline Inspection inspect(std::span<const std::uint8_t> data) {
    Inspection out;
    constexpr std::uint8_t magic[] = {0,255,255,255,255,255,255,0};
    if (data.size() < 128) return out;
    out.header = std::equal(std::begin(magic), std::end(magic), data.begin());
    out.declared = 1u + data[126];
    out.complete = data.size() == out.declared * 128 && out.declared <= 32;
    auto field = [&](std::size_t off, std::size_t len, Kind k, int tag=-1) { if (len && off <= data.size() && len <= data.size()-off) out.fields.push_back({off,len,k,tag}); };
    field(0,8,Kind::Header); field(8,2,Kind::Manufacturer); field(10,2,Kind::Product); field(12,4,Kind::Serial); field(16,2,Kind::Manufacture); field(18,2,Kind::Version); field(20,5,Kind::Parameters); field(25,10,Kind::Chromaticity); field(35,3,Kind::Established); field(38,16,Kind::Standard);
    for (std::size_t off=54;off<=108;off+=18) field(off,18,(data[off]||data[off+1])?Kind::DetailedTiming:Kind::Descriptor, (data[off]||data[off+1])?-1:data[off+3]);
    field(126,1,Kind::ExtensionCount);
    for (std::size_t b=0;b<std::min<std::size_t>(32,data.size()/128);++b) {
        const auto base=b*128; unsigned sum=0; for (std::size_t i=0;i<128;++i) sum+=data[base+i];
        bool structure=true;
        if (b && data[base]==2) {
            const auto end=std::size_t(data[base+2]); field(base,4,Kind::ExtensionHeader,2);
            if (end==0) {
                const bool empty = std::all_of(data.begin()+base+4,data.begin()+base+127,[](auto byte){return byte==0;});
                structure = empty && (data[base+3] & 0x0f) == 0;
                field(base+4,123,structure?Kind::Padding:Kind::Unknown);
            }
            else if (end<4 || end>127) { structure=false; field(base+4,123,Kind::Unknown); }
            else {
                for (std::size_t pos=4;pos<end;) {
                    const auto len=std::size_t(data[base+pos]&31)+1;
                    if (len>end-pos) { structure=false; field(base+pos,end-pos,Kind::Unknown); break; }
                    field(base+pos,len,Kind::CtaData,data[base+pos]>>5); pos+=len;
                }
                std::size_t pos=end;
                for (; pos + 18 <= 127 && (data[base + pos] || data[base + pos + 1]); pos += 18)
                    field(base + pos, 18, Kind::DetailedTiming);
                const bool zeroPadding = std::all_of(data.begin() + base + pos, data.begin() + base + 127,
                    [](auto byte) { return byte == 0; });
                structure &= zeroPadding;
                field(base + pos, 127 - pos, zeroPadding ? Kind::Padding : Kind::Unknown);
            }
        } else if (b && data[base] == 0x70 && ((data[base+1] >> 4) == 1 || (data[base+1] >> 4) == 2)) {
            constexpr std::size_t headerSize = 5;
            const auto end = headerSize + std::size_t(data[base+2]);
            field(base, headerSize, Kind::ExtensionHeader, 0x70);
            if (end >= 127) {
                structure = false;
                field(base+headerSize, 127-headerSize, Kind::Unknown);
            } else {
                // The section checksum excludes the EDID extension tag at byte zero.
                unsigned sectionSum = 0;
                for (std::size_t pos = 1; pos <= end; ++pos) sectionSum += data[base+pos];
                structure = (sectionSum & 255) == 0;
                for (std::size_t pos = headerSize; pos < end;) {
                    const auto remaining = end - pos;
                    const auto length = remaining >= 3 ? 3 + std::size_t(data[base+pos+2]) : 0;
                    if (length == 0 || length > remaining) {
                        structure = false;
                        field(base+pos, remaining, Kind::Unknown);
                        break;
                    }
                    field(base+pos, length, Kind::DisplayIdData, data[base+pos]);
                    pos += length;
                }
                field(base+end, 1, Kind::DisplayIdChecksum);
                const bool zeroPadding = std::all_of(data.begin()+base+end+1, data.begin()+base+127,
                    [](auto byte) { return byte == 0; });
                structure &= zeroPadding;
                field(base+end+1, 126-end, zeroPadding ? Kind::Padding : Kind::Unknown);
            }
        } else if (b) { field(base,1,Kind::ExtensionHeader,data[base]); field(base+1,126,Kind::Unknown); }
        field(base+127,1,Kind::Checksum);
        out.blocks.push_back({b,b<out.declared,(sum&255)==0,structure});
    }
    const auto mapped = std::min<std::size_t>(32,data.size()/128)*128;
    if (mapped < data.size()) field(mapped, data.size()-mapped, Kind::Unknown);
    return out;
}
inline bool trusted(const Inspection &i) {
    return i.header && i.complete && !i.blocks.empty() && std::all_of(i.blocks.begin(),i.blocks.end(),[](const Block &b){return b.declared && b.checksum && b.structure;});
}
// Only complete, structurally trusted captures are eligible for decoded timings.
// Unknown extension formats remain opaque.
enum class TimingSource { Base, Cta, DisplayIdTypeI, DisplayIdTypeVII };
struct Timing {
    std::size_t offset;
    unsigned hActive, hBlank, vActive, vBlank, pixelClockKHz;
    bool interlaced = false;
    TimingSource source = TimingSource::Base;
    std::size_t length = 18;
    bool preferred = false;
    bool fieldEncoded() const { return interlaced && (source == TimingSource::Base || source == TimingSource::Cta); }
    unsigned frameActiveLines() const { return fieldEncoded() ? 2 * vActive : vActive; }
    unsigned frameTotalLines() const {
        // The interlaced descriptor counts lines in one field. The full frame
        // includes the additional line from the two half-line offsets.
        return fieldEncoded() ? 2 * (vActive + vBlank) + 1 : vActive + vBlank;
    }
    double refreshHz() const { return double(pixelClockKHz) * 1000.0 / (double(hActive+hBlank)*frameTotalLines()); }
};
inline void appendDisplayIdTimings(std::vector<Timing> &out, std::span<const std::uint8_t> data, const Field &f) {
    const auto version = data[(f.offset / 128) * 128 + 1] >> 4;
    const bool typeI = f.tag == 0x03 && version == 1;
    const bool typeVII = f.tag == 0x22 && version == 2;
    if (!typeI && !typeVII) return;
    const std::size_t length = 20 + (typeVII ? (data[f.offset + 1] >> 4) & 7 : 0);
    if ((f.length - 3) % length != 0) return;
    const auto originalSize = out.size();
    for (std::size_t pos = f.offset + 3; pos < f.offset + f.length; pos += length) {
        const auto d = data.subspan(pos, length);
        const auto word = [&d](std::size_t i) { return unsigned(d[i]) | (unsigned(d[i + 1]) << 8); };
        const auto clock = (1 + (word(0) | (unsigned(d[2]) << 16))) * (typeI ? 10u : 1u);
        const auto hBlank = 1 + word(6), vBlank = 1 + word(14);
        const auto hSyncEnd = 2 + (word(8) & 0x7fff) + word(10);
        const auto vSyncEnd = 2 + (word(16) & 0x7fff) + word(18);
        if (hSyncEnd > hBlank || vSyncEnd > vBlank) {
            out.resize(originalSize);
            return;
        }
        out.push_back({pos, 1 + word(4), hBlank, 1 + word(12), vBlank, clock, bool(d[3] & 0x10),
            typeI ? TimingSource::DisplayIdTypeI : TimingSource::DisplayIdTypeVII, length,
            (data[f.offset + 1] & 7) < 2 && bool(d[3] & 0x80)});
    }
}

inline std::vector<Timing> detailedTimings(std::span<const std::uint8_t> data) {
    const auto inspection = inspect(data);
    std::vector<Timing> out;
    if (!trusted(inspection)) return out;
    for (const auto &f : inspection.fields) {
        if (f.kind == Kind::DisplayIdData) {
            appendDisplayIdTimings(out, data, f);
            continue;
        }
        if (f.kind != Kind::DetailedTiming || f.length != 18) continue;
        const auto d = data.subspan(f.offset, f.length);
        const unsigned clock = unsigned(d[0]) | (unsigned(d[1]) << 8);
        const unsigned ha = unsigned(d[2]) | ((unsigned(d[4]) & 0xf0) << 4);
        const unsigned hb = unsigned(d[3]) | ((unsigned(d[4]) & 0x0f) << 8);
        const unsigned va = unsigned(d[5]) | ((unsigned(d[7]) & 0xf0) << 4);
        const unsigned vb = unsigned(d[6]) | ((unsigned(d[7]) & 0x0f) << 8);
        if (!clock || !ha || !hb || !va || !vb) continue;
        out.push_back({f.offset, ha, hb, va, vb, clock * 10, bool(d[17] & 0x80),
            f.offset < 128 ? TimingSource::Base : TimingSource::Cta, 18, f.offset == 54});
    }
    return out;
}
inline std::vector<Timing> progressiveTimings(std::span<const std::uint8_t> data) {
    auto timings = detailedTimings(data);
    std::erase_if(timings, [](const Timing &timing) { return timing.interlaced; });
    return timings;
}
} // namespace edidfields
