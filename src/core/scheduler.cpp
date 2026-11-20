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

#include "core/scheduler.hpp"

#include <algorithm>

namespace otns {
namespace Core {

EventId Scheduler::ScheduleAt(uint64_t aTimestampUs, Callback aCallback)
{
    EventId  id        = mNextId++;
    uint64_t timestamp = std::max(aTimestampUs, mNowUs);

    mQueue.push(ScheduledEvent{timestamp, id, std::move(aCallback)});

    return id;
}

EventId Scheduler::ScheduleAfter(uint64_t aDelayUs, Callback aCallback)
{
    return ScheduleAt(mNowUs + aDelayUs, std::move(aCallback));
}

void Scheduler::Cancel(EventId aId)
{
    if (aId != kInvalidEventId)
    {
        mCanceled.insert(aId);
    }
}

void Scheduler::Clear(void)
{
    while (!mQueue.empty())
    {
        mQueue.pop();
    }

    mCanceled.clear();
    mNowUs           = 0;
    mNextId          = 1;
    mDispatchedCount = 0;
}

void Scheduler::PruneCanceledTop(void)
{
    while (!mQueue.empty())
    {
        auto it = mCanceled.find(mQueue.top().mId);

        if (it == mCanceled.end())
        {
            break;
        }

        mCanceled.erase(it);
        mQueue.pop();
    }
}

std::optional<uint64_t> Scheduler::GetNextEventTimeUs(void)
{
    PruneCanceledTop();

    if (mQueue.empty())
    {
        return std::nullopt;
    }

    return mQueue.top().mTimestampUs;
}

bool Scheduler::DispatchNextIfBeforeOrAt(uint64_t aMaxTimeUs)
{
    PruneCanceledTop();

    if (mQueue.empty() || mQueue.top().mTimestampUs > aMaxTimeUs)
    {
        return false;
    }

    ScheduledEvent event = std::move(const_cast<ScheduledEvent &>(mQueue.top()));

    mQueue.pop();
    mNowUs = event.mTimestampUs;
    mDispatchedCount += 1;

    if (event.mCallback)
    {
        event.mCallback();
    }

    return true;
}

} // namespace Core
} // namespace otns
