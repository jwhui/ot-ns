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

#include "core/path_security.hpp"

#include <system_error>
#include <vector>

namespace otns {
namespace Core {

namespace {

bool IsWithinBase(const std::filesystem::path &aBase, const std::filesystem::path &aCandidate)
{
    auto baseIt = aBase.begin();
    auto candIt = aCandidate.begin();

    for (; baseIt != aBase.end(); ++baseIt, ++candIt)
    {
        if (candIt == aCandidate.end() || *baseIt != *candIt)
        {
            return false;
        }
    }

    // Must be a strict descendant of base (not base directory itself).
    return candIt != aCandidate.end();
}

} // namespace

bool ResolveContainedPath(const std::filesystem::path &aBaseDir,
                          const std::string           &aRequestedPath,
                          std::filesystem::path       &aOutPath,
                          bool                         aAllowSymlink)
{
    std::error_code ec;

    aOutPath.clear();

    if (aBaseDir.empty() || aRequestedPath.empty())
    {
        return false;
    }

    if (aRequestedPath.find('\0') != std::string::npos)
    {
        return false;
    }

    std::filesystem::path reqPath(aRequestedPath);

    for (const auto &part : reqPath)
    {
        if (part == "..")
        {
            return false;
        }
    }

    std::filesystem::path canonBase = std::filesystem::canonical(aBaseDir, ec);

    if (ec || !std::filesystem::is_directory(canonBase, ec) || ec)
    {
        return false;
    }

    std::filesystem::path combined = reqPath.is_absolute() ? reqPath : (canonBase / reqPath);

    combined = combined.lexically_normal();
    if (!IsWithinBase(canonBase, combined))
    {
        return false;
    }

    // Walk up to the deepest existing ancestor and canonicalize it to catch symlink escapes.
    std::filesystem::path              existing = combined;
    std::vector<std::filesystem::path> tailParts;

    while (!existing.empty() && !std::filesystem::exists(existing, ec) && !ec)
    {
        std::filesystem::path leaf = existing.filename();

        if (!leaf.empty() && leaf != ".")
        {
            tailParts.push_back(leaf);
        }

        std::filesystem::path parent = existing.parent_path();

        if (parent == existing)
        {
            return false;
        }
        existing = parent;
    }

    if (ec)
    {
        return false;
    }

    // If the leaf itself already exists, verify whether it is a symlink.
    if (tailParts.empty() && !aAllowSymlink)
    {
        std::filesystem::file_status st = std::filesystem::symlink_status(combined, ec);

        if (ec || std::filesystem::is_symlink(st))
        {
            return false;
        }
    }

    std::filesystem::path resolved = std::filesystem::canonical(existing, ec);

    if (ec)
    {
        return false;
    }

    for (auto it = tailParts.rbegin(); it != tailParts.rend(); ++it)
    {
        resolved /= *it;
    }

    resolved = resolved.lexically_normal();
    if (!IsWithinBase(canonBase, resolved))
    {
        return false;
    }

    aOutPath = std::move(resolved);

    return true;
}

} // namespace Core
} // namespace otns
