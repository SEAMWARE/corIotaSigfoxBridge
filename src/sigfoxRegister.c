//
// FILE            sigfoxRegister.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// The iotasigfox bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// The IoT Agent for Sigfox: the Sigfox backend's callbacks as DataChannel records - a DataChannel's
// endpoint is <path>[/<deviceId>] - and with "channelDirection": "both" a written attribute is a downlink,
// the answer to the device's next bidirectional callback (sigfoxSouth.c). A Channel or an AssetChannel is
// refused - this bridge carries DataChannels only.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell, snprintf
#include <stdlib.h>                                   // malloc, free

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "sigfoxSouth.h"                              // sigfoxSouth*, sigfoxEndpoint*, sigfoxPublish



static const BridgeBroker* hostP = NULL;



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// iotasigfoxInit - the southbound port, from the --bridgeConfig file's member of this plugin:
//
//   { "iotasigfox": { "southbound": { "port": 17428, "idField": "id" },
//                     "ngsild": { "dataChannels": { "update": { ... } } } } }
//
// No "southbound", or no "port" in it: iotagent-sigfox's 17428.
//
static int iotasigfoxInit(const char* configFile, const BridgeBroker* brokerP)
{
  CorAlloc     ka;
  CorJson      cj;
  char         buf[16 * 1024];
  char*        text    = (configFile != NULL) ? configLoad(configFile) : NULL;
  long long    port    = 17428;
  const char*  idField = NULL;
  int          r       = BRIDGE_OK;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotasigfox config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "iotasigfox") : NULL;
    CorNode* southP  = (bridgeP != NULL) ? corTreeLookup(bridgeP, "southbound") : NULL;

    if (southP != NULL)
    {
      CorNode* portP = corTreeLookup(southP, "port");
      CorNode* idP   = corTreeLookup(southP, "idField");

      if (portP != NULL)
      {
        if ((portP->type != CorInt) || (portP->value.i < 1) || (portP->value.i > 65535))
        {
          COR_E("iotasigfox: southbound port - a port number, 1-65535");
          r = BRIDGE_ERR;
        }
        else
          port = portP->value.i;
      }

      if (idP != NULL)
      {
        if (idP->type != CorString)
        {
          COR_E("iotasigfox: southbound idField - the name of the callback's parameter that names the device");
          r = BRIDGE_ERR;
        }
        else
          idField = idP->value.s;
      }
    }
  }

  hostP = brokerP;

  if (r == BRIDGE_OK)
    r = sigfoxSouthOpen((unsigned short) port, idField, brokerP);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept
  free(text);
  return r;
}



// -----------------------------------------------------------------------------
//
// iotasigfoxClose - the broker has stopped serving the port: no handler runs
//
static void iotasigfoxClose(void)
{
  sigfoxSouthClose();
}



// -----------------------------------------------------------------------------
//
// The DataChannels - each a callback path, or one device of it. A broker of ABI 15 says which channel type
// an endpoint is (channelTypedAdd); an older one hands every channel over alike (channelAddInfo), and each
// is taken as a DataChannel.
//
static int iotasigfoxChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  (void) kind;
  (void) direction;                                   // in: the callbacks; both: also downlinks

  int r = sigfoxEndpointAdd(endpoint, info);

  if (r == BRIDGE_BAD_INPUT)
    COR_W("iotasigfox: DataChannel '%s' refused - its endpoint is <callback path>[/<deviceId>], its channelInfo mapping and downlinkMapping", endpoint);

  return r;
}

#if BRIDGE_ABI_VERSION >= 15
static int iotasigfoxChannelTypedAdd(const char* endpoint, BridgeChannelType type, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  if (type == BridgeChannelTypeDataChannel)
    return iotasigfoxChannelAddInfo(endpoint, kind, direction, info);

  const char* reason = "the iotasigfox bridge carries DataChannels only - a callback is a record";

  if ((hostP != NULL) && (hostP->abiVersion >= 12) && (hostP->channelStatusIn != NULL))
    hostP->channelStatusIn("iotasigfox", endpoint, BridgeChannelDormant, reason);

  COR_W("iotasigfox: '%s' refused - %s", endpoint, reason);
  return BRIDGE_UNSUPPORTED;
}
#endif

static int iotasigfoxChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return iotasigfoxChannelAddInfo(endpoint, kind, direction, NULL);
}

static int iotasigfoxChannelDel(const char* endpoint)
{
  return sigfoxEndpointDel(endpoint);
}

static int iotasigfoxPublish(const char* endpoint, const char* json)
{
  return sigfoxPublish(endpoint, json);
}



// -----------------------------------------------------------------------------
//
// iotasigfoxVersionInfo - one line for GET /version
//
static const char* iotasigfoxVersionInfo(void)
{
  return "iotasigfox 0.1.0";
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// A slot is filled only when the broker's abiVersion has it; abiVersion is then overwritten with this
// plugin's. Which channel type an endpoint is, ABI 15: before it, every channel is taken as a DataChannel.
// The southbound port needs ABI 16 - init() refuses an older broker.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "iotasigfox";                // --bridges iotasigfox, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = iotasigfoxInit;
  driverP->close       = iotasigfoxClose;
  driverP->versionInfo = iotasigfoxVersionInfo;
  driverP->channelAdd  = iotasigfoxChannelAdd;
  driverP->channelDel  = iotasigfoxChannelDel;
  driverP->publish     = iotasigfoxPublish;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = iotasigfoxChannelAddInfo;

#if BRIDGE_ABI_VERSION >= 15
  if (driverP->abiVersion >= 15)
    driverP->channelTypedAdd = iotasigfoxChannelTypedAdd;
#endif

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
