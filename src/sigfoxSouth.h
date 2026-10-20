#ifndef SIGFOX_SOUTH_H_
#define SIGFOX_SOUTH_H_

//
// FILE            sigfoxSouth.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The Sigfox backend's callbacks on the southbound port, which the broker serves for the plugin
// (BridgeBroker.httpListen, ABI 16): a device's message as a DataChannel record, and a downlink, queued
// by a written attribute, as the answer to the device's next bidirectional callback.
//
#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// sigfoxSouthOpen - ask the broker to serve 'port'; idField: the callback's parameter that names the
// device (NULL: "id", else "device"). BRIDGE_OK or BRIDGE_ERR. No close: the broker stops serving the port
// before the plugin's close()
//
extern int sigfoxSouthOpen(unsigned short port, const char* idField, const BridgeBroker* brokerP);



// -----------------------------------------------------------------------------
//
// sigfoxSouthClose - the DataChannels and the queued downlinks freed
//
extern void sigfoxSouthClose(void);



// -----------------------------------------------------------------------------
//
// sigfoxEndpointAdd / Del - a DataChannel's endpoint, <path>[/<deviceId>]; info: its channelInfo (JSON
// text, NULL: none) - "mapping" (the data's payload format) and "downlinkMapping" (a downlink's)
//
extern int sigfoxEndpointAdd(const char* endpoint, const char* info);
extern int sigfoxEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// sigfoxPublish - a write-back fragment into its device's queued downlink
//
extern int sigfoxPublish(const char* endpoint, const char* json);

#endif  // SIGFOX_SOUTH_H_
