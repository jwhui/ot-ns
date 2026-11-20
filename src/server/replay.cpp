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

#include "server/replay.hpp"

#include <sstream>

#include "core/path_security.hpp"
#include "nlohmann/json.hpp"

namespace otns {
namespace Server {

namespace {

static constexpr uintmax_t kMaxReplayFileBytes = 64u * 1024u * 1024u;

} // namespace

JsonlRecorder::~JsonlRecorder(void) { Close(); }

bool JsonlRecorder::OpenContained(const std::filesystem::path &aBaseDir, const std::string &aRequestedPath)
{
    std::filesystem::path resolved;

    if (!Core::ResolveContainedPath(aBaseDir, aRequestedPath, resolved))
    {
        return false;
    }

    return OpenDirect(resolved);
}

bool JsonlRecorder::OpenDirect(const std::filesystem::path &aPath)
{
    Close();

    std::error_code       ec;
    std::filesystem::path parent = aPath.parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return false;
        }
    }

    mFile = std::fopen(aPath.c_str(), "wb");
    if (mFile == nullptr)
    {
        return false;
    }

    mEventCount = 0;

    return true;
}

void JsonlRecorder::Close(void)
{
    if (mFile != nullptr)
    {
        std::fflush(mFile);
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool JsonlRecorder::WriteEvent(const std::string &aJsonLine)
{
    if (mFile == nullptr || aJsonLine.empty())
    {
        return false;
    }

    if (std::fwrite(aJsonLine.data(), 1, aJsonLine.size(), mFile) != aJsonLine.size() || std::fputc('\n', mFile) == EOF)
    {
        return false;
    }

    mEventCount += 1;

    return true;
}

bool JsonlReplayer::LoadContained(const std::filesystem::path &aBaseDir,
                                  const std::string           &aRequestedPath,
                                  std::string                 &aError)
{
    std::filesystem::path resolved;

    aError.clear();
    if (!Core::ResolveContainedPath(aBaseDir, aRequestedPath, resolved))
    {
        aError = "Replay path rejected by containment check";
        return false;
    }

    return LoadDirect(resolved, aError);
}

bool JsonlReplayer::LoadDirect(const std::filesystem::path &aPath, std::string &aError)
{
    aError.clear();
    mEntries.clear();
    mCursor = 0;

    std::error_code ec;
    uintmax_t       sz = std::filesystem::file_size(aPath, ec);

    if (ec || sz > kMaxReplayFileBytes)
    {
        aError = "Replay file unreadable or exceeds size limit";
        return false;
    }

    std::FILE *fp = std::fopen(aPath.c_str(), "rb");

    if (fp == nullptr)
    {
        aError = "Failed to open replay file";
        return false;
    }

    std::string content(static_cast<size_t>(sz), '\0');
    size_t      n = (sz > 0) ? std::fread(content.data(), 1, content.size(), fp) : 0;

    std::fclose(fp);
    if (n != content.size())
    {
        aError = "Short read loading replay file";
        return false;
    }

    std::istringstream iss(content);
    std::string        line;

    while (std::getline(iss, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        nlohmann::json obj = nlohmann::json::parse(line, nullptr, false);

        if (obj.is_discarded() || !obj.is_object() || !obj.contains("v") || obj["v"] != 1)
        {
            aError = "Invalid JSONL replay event record";
            mEntries.clear();
            return false;
        }

        ReplayEntry entry;

        if (obj.contains("ts_us") && obj["ts_us"].is_number_unsigned())
        {
            entry.mTimestampUs = obj["ts_us"].get<uint64_t>();
        }
        entry.mJsonLine = std::move(line);
        mEntries.push_back(std::move(entry));
    }

    return true;
}

uint64_t JsonlReplayer::GetDurationUs(void) const { return mEntries.empty() ? 0 : mEntries.back().mTimestampUs; }

std::vector<std::string> JsonlReplayer::PopDueEvents(uint64_t aNowUs)
{
    std::vector<std::string> out;

    while (mCursor < mEntries.size() && mEntries[mCursor].mTimestampUs <= aNowUs)
    {
        out.push_back(mEntries[mCursor].mJsonLine);
        ++mCursor;
    }

    return out;
}

} // namespace Server
} // namespace otns
