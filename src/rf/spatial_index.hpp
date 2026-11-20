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

#ifndef OTNS_RF_SPATIAL_INDEX_HPP_
#define OTNS_RF_SPATIAL_INDEX_HPP_

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rf/types.hpp"

namespace otns {
namespace Rf {

/**
 * Uniform 3D spatial hash grid for fast candidate neighbor queries on 500-1000+ node topologies.
 *
 * QueryRadius() always returns deterministically sorted NodeIds.
 */
class SpatialIndex
{
public:
    explicit SpatialIndex(double aCellSize = 160.0);

    void   SetCellSize(double aCellSize);
    double GetCellSize(void) const { return mCellSize; }

    void Upsert(NodeId aId, const Position3D &aPos);
    void Remove(NodeId aId);
    void Clear(void);

    std::vector<NodeId> QueryRadius(const Position3D &aCenter, double aRadius) const;
    size_t              GetSize(void) const { return mEntries.size(); }

private:
    struct CellCoord
    {
        bool operator==(const CellCoord &aOther) const
        {
            return mCx == aOther.mCx && mCy == aOther.mCy && mCz == aOther.mCz;
        }

        int32_t mCx = 0;
        int32_t mCy = 0;
        int32_t mCz = 0;
    };

    struct CellHash
    {
        size_t operator()(const CellCoord &aCoord) const noexcept
        {
            uint64_t h1 = static_cast<uint64_t>(static_cast<uint32_t>(aCoord.mCx)) * 73856093ULL;
            uint64_t h2 = static_cast<uint64_t>(static_cast<uint32_t>(aCoord.mCy)) * 19349663ULL;
            uint64_t h3 = static_cast<uint64_t>(static_cast<uint32_t>(aCoord.mCz)) * 83492791ULL;

            return static_cast<size_t>(h1 ^ h2 ^ h3);
        }
    };

    struct Entry
    {
        Position3D mPos;
        CellCoord  mCell;
    };

    CellCoord ToCell(const Position3D &aPos) const;
    void      RebuildGrid(void);

    double                                                       mCellSize;
    std::unordered_map<NodeId, Entry>                            mEntries;
    std::unordered_map<CellCoord, std::vector<NodeId>, CellHash> mGrid;
};

} // namespace Rf
} // namespace otns

#endif // OTNS_RF_SPATIAL_INDEX_HPP_
