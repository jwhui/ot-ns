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

#include "rf/spatial_index.hpp"

#include <algorithm>
#include <cmath>

namespace otns {
namespace Rf {

namespace {

static constexpr double   kMinCellSize         = 1.0;
static constexpr uint64_t kMaxGridCellsToVisit = 4096;

} // namespace

SpatialIndex::SpatialIndex(double aCellSize)
    : mCellSize(std::max(aCellSize, kMinCellSize))
{
}

void SpatialIndex::SetCellSize(double aCellSize)
{
    double clamped = std::max(aCellSize, kMinCellSize);

    if (clamped != mCellSize)
    {
        mCellSize = clamped;
        RebuildGrid();
    }
}

SpatialIndex::CellCoord SpatialIndex::ToCell(const Position3D &aPos) const
{
    return {
        static_cast<int32_t>(std::floor(aPos.mX / mCellSize)),
        static_cast<int32_t>(std::floor(aPos.mY / mCellSize)),
        static_cast<int32_t>(std::floor(aPos.mZ / mCellSize)),
    };
}

void SpatialIndex::Upsert(NodeId aId, const Position3D &aPos)
{
    CellCoord newCell = ToCell(aPos);
    auto      it      = mEntries.find(aId);

    if (it != mEntries.end())
    {
        if (it->second.mCell == newCell)
        {
            it->second.mPos = aPos;
            return;
        }

        auto &oldBucket = mGrid[it->second.mCell];

        oldBucket.erase(std::remove(oldBucket.begin(), oldBucket.end(), aId), oldBucket.end());
        if (oldBucket.empty())
        {
            mGrid.erase(it->second.mCell);
        }

        it->second.mPos  = aPos;
        it->second.mCell = newCell;
    }
    else
    {
        mEntries.emplace(aId, Entry{aPos, newCell});
    }

    mGrid[newCell].push_back(aId);
}

void SpatialIndex::Remove(NodeId aId)
{
    auto it = mEntries.find(aId);

    if (it == mEntries.end())
    {
        return;
    }

    auto git = mGrid.find(it->second.mCell);

    if (git != mGrid.end())
    {
        auto &bucket = git->second;

        bucket.erase(std::remove(bucket.begin(), bucket.end(), aId), bucket.end());
        if (bucket.empty())
        {
            mGrid.erase(git);
        }
    }

    mEntries.erase(it);
}

void SpatialIndex::Clear(void)
{
    mEntries.clear();
    mGrid.clear();
}

void SpatialIndex::RebuildGrid(void)
{
    mGrid.clear();
    for (auto &pair : mEntries)
    {
        pair.second.mCell = ToCell(pair.second.mPos);
        mGrid[pair.second.mCell].push_back(pair.first);
    }
}

std::vector<NodeId> SpatialIndex::QueryRadius(const Position3D &aCenter, double aRadius) const
{
    std::vector<NodeId> result;

    if (mEntries.empty())
    {
        return result;
    }

    if (aRadius < 0.0 || !std::isfinite(aRadius))
    {
        result.reserve(mEntries.size());
        for (const auto &pair : mEntries)
        {
            result.push_back(pair.first);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    double    radiusSq = aRadius * aRadius;
    CellCoord minCell  = ToCell({aCenter.mX - aRadius, aCenter.mY - aRadius, aCenter.mZ - aRadius});
    CellCoord maxCell  = ToCell({aCenter.mX + aRadius, aCenter.mY + aRadius, aCenter.mZ + aRadius});
    uint64_t  spanX    = static_cast<uint64_t>(static_cast<int64_t>(maxCell.mCx) - minCell.mCx + 1);
    uint64_t  spanY    = static_cast<uint64_t>(static_cast<int64_t>(maxCell.mCy) - minCell.mCy + 1);
    uint64_t  spanZ    = static_cast<uint64_t>(static_cast<int64_t>(maxCell.mCz) - minCell.mCz + 1);
    bool useGrid = (spanX <= 64 && spanY <= 64 && spanZ <= 64) && (spanX * spanY * spanZ <= kMaxGridCellsToVisit) &&
                   (spanX * spanY * spanZ < mEntries.size() * 2);

    if (!useGrid)
    {
        for (const auto &pair : mEntries)
        {
            if (aCenter.DistanceSquaredTo(pair.second.mPos) <= radiusSq)
            {
                result.push_back(pair.first);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    for (int32_t cx = minCell.mCx; cx <= maxCell.mCx; ++cx)
    {
        for (int32_t cy = minCell.mCy; cy <= maxCell.mCy; ++cy)
        {
            for (int32_t cz = minCell.mCz; cz <= maxCell.mCz; ++cz)
            {
                auto git = mGrid.find({cx, cy, cz});

                if (git == mGrid.end())
                {
                    continue;
                }

                for (NodeId id : git->second)
                {
                    const auto &entry = mEntries.find(id)->second;

                    if (aCenter.DistanceSquaredTo(entry.mPos) <= radiusSq)
                    {
                        result.push_back(id);
                    }
                }
            }
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

} // namespace Rf
} // namespace otns
