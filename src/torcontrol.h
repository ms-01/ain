// Copyright (c) 2015-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

/**
 * Functionality for communicating with Tor.
 */
#ifndef DEFI_TORCONTROL_H
#define DEFI_TORCONTROL_H

#include <scheduler.h>

#include <string>

class CService;

extern const std::string DEFAULT_TOR_CONTROL;

/**
 * The key type that is requested from Tor for the hidden service.
 *
 * Tor v3 (ED25519-V3) services require Tor >= 0.3.5; Tor removed the
 * deprecated Tor v2 (RSA1024) services in 0.4.6.
 */
extern const std::string TOR_ONION_KEY_TYPE;

static const bool DEFAULT_LISTEN_ONION = true;

/**
 * Derive the onion service from the service id that Tor returned in the reply
 * to an ADD_ONION command.
 *
 * The service id of a Tor v3 hidden service is the 56 character base32 encoded
 * ed25519 public key; the onion address is derived from it.
 *
 * @param[in] service_id the service id as reported by Tor
 * @param[in] port the port the hidden service is reachable at
 * @returns the onion service, or an invalid service if the service id is not a
 *          valid Tor v3 onion address.
 */
CService TorServiceIdToService(const std::string& service_id, int port);

void StartTorControl();
void InterruptTorControl();
void StopTorControl();

#endif /* DEFI_TORCONTROL_H */
