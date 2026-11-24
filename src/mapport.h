// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_MAPPORT_H
#define DEFI_MAPPORT_H

//! Opt-in by default to avoid unexpected outbound UDP 5351 / gateway queries on masternodes and servers.
inline constexpr bool DEFAULT_NATPMP = false;

void StartMapPort(bool enable);
void InterruptMapPort();
void StopMapPort();

#endif // DEFI_MAPPORT_H
