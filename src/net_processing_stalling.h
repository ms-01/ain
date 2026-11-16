// Copyright (c) 2024-present The DeFiChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_NET_PROCESSING_STALLING_H
#define DEFI_NET_PROCESSING_STALLING_H

#include <chrono>

/** Default time during which a peer must stall block download progress before being disconnected.
 *  The actual timeout is increased temporarily if peers are disconnected for hitting the timeout. */
static constexpr auto BLOCK_STALLING_TIMEOUT_DEFAULT{std::chrono::seconds{2}};
/** Maximum timeout for stalling block download. */
static constexpr auto BLOCK_STALLING_TIMEOUT_MAX{std::chrono::seconds{64}};
/** Time to avoid requesting blocks from a manual peer after it stalls block download. */
static constexpr auto MANUAL_PEER_BLOCK_DOWNLOAD_COOLDOWN{std::chrono::minutes{2}};

/** Timeout to be used after a peer stalled the block download.
 *
 *  The timeout is doubled on every stall, so that we don't repeatedly react to apparent
 *  stalls caused by insufficient local bandwidth, and is capped at BLOCK_STALLING_TIMEOUT_MAX. */
constexpr std::chrono::seconds NextStallingTimeoutAfterStall(std::chrono::seconds current_timeout)
{
    const auto next_timeout{current_timeout * 2};
    return next_timeout > BLOCK_STALLING_TIMEOUT_MAX ? BLOCK_STALLING_TIMEOUT_MAX : next_timeout;
}

/** Timeout to be used after a block was connected.
 *
 *  A temporarily increased timeout is reduced slowly (by 15 percent per connected block)
 *  back to BLOCK_STALLING_TIMEOUT_DEFAULT. */
constexpr std::chrono::seconds NextStallingTimeoutAfterBlock(std::chrono::seconds current_timeout)
{
    if (current_timeout <= BLOCK_STALLING_TIMEOUT_DEFAULT) {
        return BLOCK_STALLING_TIMEOUT_DEFAULT;
    }
    const auto next_timeout{std::chrono::duration_cast<std::chrono::seconds>(current_timeout * 0.85)};
    return next_timeout < BLOCK_STALLING_TIMEOUT_DEFAULT ? BLOCK_STALLING_TIMEOUT_DEFAULT : next_timeout;
}

#endif // DEFI_NET_PROCESSING_STALLING_H
