// Copyright (c) 2014-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_TIMEDATA_H
#define DEFI_TIMEDATA_H

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <cstddef>
#include <deque>
#include <stdint.h>
#include <vector>

#include <sync.h>

static const int64_t DEFAULT_MAX_TIME_ADJUSTMENT = 70 * 60;

class CNetAddr;

/**
 * Median filter over a stream of values.
 * Returns the median of the last N numbers
 */
template <typename T>
class CMedianFilter
{
private:
    std::vector<T> vValues;
    std::vector<T> vSorted;
    unsigned int nSize;

public:
    CMedianFilter(unsigned int _size, T initial_value) : nSize(_size)
    {
        vValues.reserve(_size);
        vValues.push_back(initial_value);
        vSorted = vValues;
    }

    void input(T value)
    {
        if (vValues.size() == nSize) {
            vValues.erase(vValues.begin());
        }
        vValues.push_back(value);

        vSorted.resize(vValues.size());
        std::copy(vValues.begin(), vValues.end(), vSorted.begin());
        std::sort(vSorted.begin(), vSorted.end());
    }

    T median() const
    {
        int vSortedSize = vSorted.size();
        assert(vSortedSize > 0);
        if (vSortedSize & 1) // Odd number of elements
        {
            return vSorted[vSortedSize / 2];
        } else // Even number of elements
        {
            return (vSorted[vSortedSize / 2 - 1] + vSorted[vSortedSize / 2]) / 2;
        }
    }

    int size() const
    {
        return vValues.size();
    }

    std::vector<T> sorted() const
    {
        return vSorted;
    }
};

/**
 * Time offset tracker, modelled after Bitcoin Core's node::TimeOffsets.
 *
 * It keeps the most recent MAX_SIZE offset samples between the local clock and the clocks of
 * our outbound peers in a FIFO deque and exposes their median. The median is only computed once
 * at least MIN_SAMPLES samples are available. If the median exceeds WARN_THRESHOLD an operator
 * warning is raised, because a clock that is out of sync with the network may lead to consensus
 * failures.
 */
class TimeOffsets
{
public:
    //! Maximum number of time offsets stored.
    static constexpr size_t MAX_SIZE{50};
    //! Minimum number of samples required before the median is computed.
    static constexpr size_t MIN_SAMPLES{5};
    //! Minimum difference between system and network time for a warning to be raised.
    static constexpr std::chrono::minutes WARN_THRESHOLD{10};

    /** Add a new time offset sample. */
    void Add(std::chrono::seconds offset);

    /** Drop all collected samples. */
    void Clear();

    /**
     * Compute and return the median of the collected time offset samples. The median is returned
     * as 0 when there are less than MIN_SAMPLES samples.
     */
    std::chrono::seconds Median() const;

    /**
     * Raise an operator warning if the median time offset exceeds the warning threshold.
     * Returns true if a warning was raised.
     */
    bool WarnIfOutOfSync() const;

private:
    mutable Mutex m_mutex;
    /**
     * The observed time differences between our local clock and those of our outbound peers. A
     * positive offset means the peer's clock is ahead of our local clock.
     */
    std::deque<std::chrono::seconds> m_offsets GUARDED_BY(m_mutex){};
};

/** Global instance of the time offset tracker. */
TimeOffsets &GetTimeOffsets();

/** Functions to keep track of adjusted P2P time */
/** The median of the collected time offset samples (bounded by -maxtimeadjustment). */
int64_t GetTimeOffset();
/**
 * The legacy applied time offset (frozen 200 slot median filter). It is still used by the
 * consensus relevant 2 hour future time rule below the DF25 activation height, so that the
 * acceptance window for historical blocks does not change.
 */
int64_t GetLegacyTimeOffset();
int64_t GetAdjustedTime();
void AddTimeData(const CNetAddr& ip, int64_t nTime);
/** Add a time offset sample to the time offset tracker (outbound peers only). */
void AddTimeOffsetSample(int64_t nOffsetSample);

#endif // DEFI_TIMEDATA_H
