/*
 *  Copyright (c) 2026, The OTNS Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef OTNS_CORE_PCAP_WRITER_HPP_
#define OTNS_CORE_PCAP_WRITER_HPP_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/sim_node.hpp"

namespace otns {
namespace Core {

enum class PcapFormat : uint32_t
{
    kIeee802154 = 195, // DLT_IEEE802_15_4_WITHFCS
    kWpanTap    = 283, // DLT_IEEE802_15_4_TAP
};

/**
 * Libpcap / WPAN-TAP (DLT 195 / DLT 283) frame capture writer.
 */
class PcapWriter
{
public:
    PcapWriter(void) = default;
    ~PcapWriter(void);

    PcapWriter(const PcapWriter &)            = delete;
    PcapWriter &operator=(const PcapWriter &) = delete;

    bool OpenContained(const std::filesystem::path &aBaseDir,
                       const std::string           &aRequestedPath,
                       PcapFormat                   aFormat = PcapFormat::kWpanTap);
    bool OpenDirect(const std::filesystem::path &aPath, PcapFormat aFormat = PcapFormat::kWpanTap);
    void Close(void);

    bool     IsOpen(void) const { return mFile != nullptr; }
    bool     AppendFrame(const RawFrame &aFrame);
    bool     Flush(void);
    uint64_t GetFrameCount(void) const { return mFrameCount; }

private:
    bool WriteGlobalHeader(void);

    std::FILE *mFile       = nullptr;
    PcapFormat mFormat     = PcapFormat::kWpanTap;
    uint64_t   mFrameCount = 0;
};

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_PCAP_WRITER_HPP_
