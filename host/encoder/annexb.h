#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace quest {
// FFmpeg hardware encoders emit complete Annex-B AUs. Cache parameter sets so
// every IDR sent on reconnect is independently decodable, without AVCC framing.
inline bool prepareAnnexB(std::vector<uint8_t> &au, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) {
    struct Nal { size_t start, header; };
    std::vector<Nal> nals;
    for (size_t i = 0; i + 3 < au.size();) {
        size_t length = 0;
        if (!au[i] && !au[i+1] && au[i+2] == 1) length = 3;
        else if (!au[i] && !au[i+1] && !au[i+2] && au[i+3] == 1) length = 4;
        if (length) { if (i + length >= au.size()) throw std::runtime_error("Empty H264 NAL"); nals.push_back({i, i+length}); i += length; }
        else ++i;
    }
    if (nals.empty() || nals.front().start != 0) throw std::runtime_error("Hardware encoder did not emit Annex-B");
    bool idr = false, hasSps = false, hasPps = false, vcl = false;
    for (size_t i = 0; i < nals.size(); ++i) {
        const auto type = au[nals[i].header] & 31;
        const auto end = i + 1 < nals.size() ? nals[i+1].start : au.size();
        if (type == 7 || type == 8) {
            auto &set = type == 7 ? sps : pps;
            set.assign(au.begin()+nals[i].start, au.begin()+end);
            (type == 7 ? hasSps : hasPps) = true;
        }
        idr |= type == 5; vcl |= type == 1 || type == 5;
    }
    if (!vcl) throw std::runtime_error("Hardware packet contains no H264 picture");
    if (idr) {
        if (sps.empty() || pps.empty()) throw std::runtime_error("IDR lacks SPS/PPS");
        if (!hasSps || !hasPps) {
            std::vector<uint8_t> prefix;
            if (!hasSps) prefix.insert(prefix.end(), sps.begin(), sps.end());
            if (!hasPps) prefix.insert(prefix.end(), pps.begin(), pps.end());
            au.insert(au.begin(), prefix.begin(), prefix.end());
        }
    }
    return idr;
}
}
