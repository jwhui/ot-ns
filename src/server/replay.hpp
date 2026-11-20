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

#ifndef OTNS_SERVER_REPLAY_HPP_
#define OTNS_SERVER_REPLAY_HPP_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace otns {
namespace Server {

/**
 * Writes schema-compliant OTNS/Nexus visualization events as newline-delimited JSON (.jsonl).
 */
class JsonlRecorder
{
public:
    JsonlRecorder(void) = default;
    ~JsonlRecorder(void);

    JsonlRecorder(const JsonlRecorder &)            = delete;
    JsonlRecorder &operator=(const JsonlRecorder &) = delete;

    bool     OpenContained(const std::filesystem::path &aBaseDir, const std::string &aRequestedPath);
    bool     OpenDirect(const std::filesystem::path &aPath);
    void     Close(void);
    bool     IsOpen(void) const { return mFile != nullptr; }
    bool     WriteEvent(const std::string &aJsonLine);
    uint64_t GetEventCount(void) const { return mEventCount; }

private:
    std::FILE *mFile       = nullptr;
    uint64_t   mEventCount = 0;
};

struct ReplayEntry
{
    uint64_t    mTimestampUs = 0;
    std::string mJsonLine;
};

/**
 * Loads and streams .jsonl replay files ordered by virtual simulation timestamp.
 */
class JsonlReplayer
{
public:
    bool LoadContained(const std::filesystem::path &aBaseDir, const std::string &aRequestedPath, std::string &aError);
    bool LoadDirect(const std::filesystem::path &aPath, std::string &aError);

    void                     Reset(void) { mCursor = 0; }
    bool                     IsFinished(void) const { return mCursor >= mEntries.size(); }
    size_t                   GetEventCount(void) const { return mEntries.size(); }
    uint64_t                 GetDurationUs(void) const;
    std::vector<std::string> PopDueEvents(uint64_t aNowUs);

private:
    std::vector<ReplayEntry> mEntries;
    size_t                   mCursor = 0;
};

} // namespace Server
} // namespace otns

#endif // OTNS_SERVER_REPLAY_HPP_
