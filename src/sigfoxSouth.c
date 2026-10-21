//
// FILE            sigfoxSouth.c
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
// The Sigfox backend's callbacks - see header. What iotagent-sigfox answers (lib/sigfoxHandlers.js), its
// errors {"name", "message"} included.
//
// Threads: a callback is handled on the broker's thread for the port, and handed to the broker there
// (sampleIn); publish() runs on a broker thread and only queues the downlink. One mutex guards the
// DataChannels and the queued downlinks.
//
#include <pthread.h>                                  // pthread_mutex_*
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, calloc, free, strtoll
#include <string.h>                                   // strcmp, strchr, strdup, strlen, memcpy
#include <strings.h>                                  // strcasecmp

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BridgeHttpRequest, BRIDGE_*

#include "sigfoxCodec.h"                              // SigfoxFormat, sigfoxFormatParse, sigfoxDecode, sigfoxEncode, sigfoxHex*
#include "sigfoxSouth.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// Endpoint - a DataChannel's: the callback's path, its device (NULL: every device that calls on the path)
//
typedef struct Endpoint
{
  char*             endpoint;
  char*             path;
  char*             device;
  bool              mapped;                           // "mapping" given: the data decoded
  SigfoxFormat      up;
  bool              downlinks;                        // "downlinkMapping" given: written attributes are downlinks
  SigfoxFormat      down;
  struct Endpoint*  next;
} Endpoint;



// -----------------------------------------------------------------------------
//
// Downlink - a device's queued downlink: the 8 bytes of the next bidirectional callback's answer
//
#define DOWNLINK_SIZE      8
#define DOWNLINKS_MAX   4096

typedef struct Downlink
{
  char*             device;
  unsigned char     data[DOWNLINK_SIZE];
  struct Downlink*  next;
} Downlink;



static const BridgeBroker*  brokerP      = NULL;
static char                 idField[64]  = "";       // "": "id", else "device"
static pthread_mutex_t      mutex        = PTHREAD_MUTEX_INITIALIZER;
static Endpoint*            endpointList = NULL;
static Downlink*            downlinkList = NULL;
static int                  downlinkCount = 0;



// -----------------------------------------------------------------------------
//
// The callback's variables that are not its data - Sigfox's names ({device}, {time}, ...). A GET's query
// cannot be listed (the broker hands over a parameter by name), so these are the ones looked for; a POST's
// JSON body is taken whole.
//
static const char* variableV[] =
{
  "time", "seqNumber", "deviceTypeId", "ack", "longPolling", "duplicate", "station", "snr", "avgSnr", "rssi",
  "lat", "lng", "linkQuality", "operatorName", "countryCode", "computedLocation", "fixedLat", "fixedLng",
  "statin", "device", "id"
};



// -----------------------------------------------------------------------------
//
// reply / error - a status and a JSON body for the broker to send; iotagent-sigfox's error body
// {"name": "<NAME>", "message": "<message>"}
//
static int reply(char** bodyP, int status, const char* json)
{
  *bodyP = (json != NULL) ? strdup(json) : NULL;
  return ((json == NULL) || (*bodyP != NULL)) ? status : 500;
}

static int error(char** bodyP, int status, const char* name, const char* message)
{
  char body[1024];
  int  at = snprintf(body, sizeof(body), "{\"name\":\"%s\",\"message\":\"", name);

  for (const char* s = message; (*s != 0) && (at < (int) sizeof(body) - 8); s++)
  {
    if ((*s == '"') || (*s == '\\'))
      body[at++] = '\\';
    body[at++] = ((unsigned char) *s < 0x20) ? ' ' : *s;
  }
  snprintf(&body[at], sizeof(body) - at, "\"}");

  return reply(bodyP, status, body);
}



// -----------------------------------------------------------------------------
//
// endpointFind - the DataChannel of a device's callback on 'path': the device's own, else the path's; under the mutex
//
static Endpoint* endpointFind(const char* path, const char* device)
{
  Endpoint* pathP = NULL;

  for (Endpoint* eP = endpointList; eP != NULL; eP = eP->next)
  {
    if (strcmp(eP->path, path) != 0)
      continue;

    if (eP->device == NULL)
      pathP = eP;
    else if (strcasecmp(eP->device, device) == 0)
      return eP;
  }

  return pathP;
}



// -----------------------------------------------------------------------------
//
// downlinkTake - a device's queued downlink, unlinked; NULL: none. Under the mutex
//
static Downlink* downlinkTake(const char* device)
{
  for (Downlink** dPP = &downlinkList; *dPP != NULL; dPP = &(*dPP)->next)
  {
    if (strcasecmp((*dPP)->device, device) == 0)
    {
      Downlink* dP = *dPP;

      *dPP = dP->next;
      downlinkCount -= 1;
      return dP;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// timeOf - the callback's {time}, seconds since the epoch, as nanoseconds; 0: none (the broker's clock)
//
static int64_t timeOf(const char* s)
{
  char*     rest;
  long long t = (s != NULL) ? strtoll(s, &rest, 10) : 0;

  if ((s == NULL) || (s[0] == 0) || (*rest != 0) || (t <= 0))
    return 0;

  return (int64_t) t * 1000000000LL;
}



// -----------------------------------------------------------------------------
//
// serve - a callback:
//
//   GET  /<path>?id=<device>&data=<hex>&time=...     iotagent-sigfox's (its path is /update)
//   POST /<path>  { "device": ..., "data": ..., ... }   a Sigfox custom callback, application/json
//
// 200 {} - and to a bidirectional callback (ack true) with a downlink queued for the device,
// { "<device>": { "downlinkData": "<16 hex digits>" } }; 204 when there is none.
//
static int serve(const BridgeHttpRequest* reqP, char** bodyP, const char** contentTypeP)
{
  (void) contentTypeP;                                // application/json

  const char* path = reqP->path;

  while (*path == '/')
    ++path;

  if ((path[0] == 0) || (strchr(path, '/') != NULL))
    return error(bodyP, 404, "NOT_FOUND", "a callback's path is /<name> - /update for iotagent-sigfox's");

  CorAlloc ka;
  CorJson  cj;
  char     buf[8 * 1024];
  CorNode* bodyTreeP = NULL;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotasigfox callback");
  corJsonCreate(&cj, &ka);

  bool post = (strcmp(reqP->method, "POST") == 0);

  if ((post == false) && (strcmp(reqP->method, "GET") != 0))
  {
    corAllocBufferReset(&ka, false);
    return error(bodyP, 405, "METHOD_NOT_ALLOWED", "a callback is a GET or a POST");
  }

  if (post == true)
  {
    char* text = corAlloc(&ka, reqP->bodyLen + 1);

    if (text != NULL)
    {
      memcpy(text, reqP->body, reqP->bodyLen);
      text[reqP->bodyLen] = 0;
      bodyTreeP = corJsonParse(&cj, text);
    }

    if ((bodyTreeP == NULL) || (bodyTreeP->type != CorObject))
    {
      corAllocBufferReset(&ka, false);
      return error(bodyP, 400, "BAD_PAYLOAD", "a POST callback's body is a JSON object");
    }
  }

  //
  // The device and its data - iotagent-sigfox's mandatory fields
  //
  const char* idName = (idField[0] != 0) ? idField : "id";
  const char* device = NULL;
  const char* data   = NULL;

  if (post == true)
  {
    CorNode* idP   = corTreeLookup(bodyTreeP, idName);
    CorNode* dataP = corTreeLookup(bodyTreeP, "data");

    if ((idP == NULL) && (idField[0] == 0))
      idP = corTreeLookup(bodyTreeP, idName = "device");

    device = ((idP != NULL) && (idP->type == CorString)) ? idP->value.s : NULL;
    data   = ((dataP != NULL) && (dataP->type == CorString)) ? dataP->value.s : NULL;
  }
  else
  {
    device = reqP->param(reqP, idName);
    if ((device == NULL) && (idField[0] == 0))
      device = reqP->param(reqP, idName = "device");
    data = reqP->param(reqP, "data");
  }

  if ((device == NULL) || (device[0] == 0) || (data == NULL))
  {
    char message[256];

    char idQuoted[80] = "";

    if ((device == NULL) || (device[0] == 0))
      snprintf(idQuoted, sizeof(idQuoted), "\"%s\"", (idField[0] != 0) ? idField : "id");

    snprintf(message, sizeof(message), "Some of the mandatory fields for the request were not found: [%s%s%s]",
             idQuoted,
             (((device == NULL) || (device[0] == 0)) && (data == NULL)) ? "," : "",
             (data == NULL) ? "\"data\"" : "");
    corAllocBufferReset(&ka, false);
    COR_W("iotasigfox: a callback on /%s refused - %s", path, message);
    return error(bodyP, 400, "MANDATORY_FIELDS_NOT_FOUND", message);
  }

  unsigned char frame[12];
  int           frameLen = sigfoxHexDecode(data, frame, sizeof(frame));

  if (frameLen < 0)
  {
    corAllocBufferReset(&ka, false);
    COR_W("iotasigfox: a callback of %s refused - its data '%s' is not 0 to 12 bytes of hex", device, data);
    return error(bodyP, 400, "BAD_DATA", "the data is not 0 to 12 bytes of hex");
  }

  //
  // Its DataChannel, and the record - the data decoded by the DataChannel's mapping, the callback's
  // other variables as @-columns
  //
  char         endpoint[512];
  SigfoxFormat up;
  bool         mapped = false;
  bool         found  = false;

  pthread_mutex_lock(&mutex);

  Endpoint* eP = endpointFind(path, device);

  if (eP != NULL)
  {
    found  = true;
    mapped = eP->mapped;
    snprintf(endpoint, sizeof(endpoint), "%s", eP->endpoint);
    if (mapped)
      up = eP->up;
  }

  pthread_mutex_unlock(&mutex);

  if (found == false)
  {
    char message[300];

    snprintf(message, sizeof(message), "No device was found with id:%s", device);
    corAllocBufferReset(&ka, false);
    COR_V("iotasigfox: a callback of %s on /%s - no DataChannel has it", device, path);
    return error(bodyP, 404, "DEVICE_NOT_FOUND", message);
  }

  CorNode*    recordP = corTreeObject(&ka, NULL);
  const char* ack     = NULL;
  const char* t       = NULL;
  char        tBuf[32];

  if (recordP == NULL)
  {
    corAllocBufferReset(&ka, false);
    return error(bodyP, 500, "INTERNAL_ERROR", "out of memory");
  }

  if (mapped)
    sigfoxDecode(&ka, &up, frame, frameLen, recordP);

  corTreeChildAdd(recordP, corTreeString(&ka, "@deviceId", device));
  corTreeChildAdd(recordP, corTreeString(&ka, "@data", data));

  if (post == true)
  {
    for (CorNode* memberP = bodyTreeP->value.head; memberP != NULL; memberP = memberP->next)
    {
      if ((strcmp(memberP->name, idName) == 0) || (strcmp(memberP->name, "data") == 0))
        continue;

      CorNode* copyP = corTreeClone(&ka, memberP);
      char*    name  = corAlloc(&ka, strlen(memberP->name) + 2);

      if ((copyP == NULL) || (name == NULL))
        continue;

      snprintf(name, strlen(memberP->name) + 2, "@%s", memberP->name);
      copyP->name = name;
      corTreeChildAdd(recordP, copyP);

      if (strcmp(memberP->name, "ack") == 0)
        ack = (memberP->type == CorBoolean) ? (memberP->value.b ? "true" : "false") : (memberP->type == CorString) ? memberP->value.s : NULL;
      else if (strcmp(memberP->name, "time") == 0)
      {
        if (memberP->type == CorInt)
        {
          snprintf(tBuf, sizeof(tBuf), "%lld", memberP->value.i);
          t = tBuf;
        }
        else if (memberP->type == CorString)
          t = memberP->value.s;
      }
    }
  }
  else
  {
    for (unsigned int ix = 0; ix < sizeof(variableV) / sizeof(variableV[0]); ix++)
    {
      const char* v = (strcmp(variableV[ix], idName) != 0) ? reqP->param(reqP, variableV[ix]) : NULL;

      if (v == NULL)
        continue;

      char* name = corAlloc(&ka, strlen(variableV[ix]) + 2);

      if (name == NULL)
        continue;

      snprintf(name, strlen(variableV[ix]) + 2, "@%s", variableV[ix]);
      corTreeChildAdd(recordP, corTreeString(&ka, name, v));
    }

    ack = reqP->param(reqP, "ack");
    t   = reqP->param(reqP, "time");
  }

  int   size = corJsonFastRenderSize(recordP);
  char* json = malloc(size + 1);

  if (json != NULL)
  {
    corJsonFastRender(recordP, json);

    int r = brokerP->sampleIn("iotasigfox", endpoint, json, timeOf(t));

    COR_V("iotasigfox: record on %s (%d): %s", endpoint, r, json);
    free(json);
  }

  //
  // The answer - a bidirectional callback takes the device's queued downlink
  //
  bool bidirectional = (ack != NULL) && (strcmp(ack, "true") == 0);
  int  status        = 200;

  if (bidirectional == false)
    status = reply(bodyP, 200, "{}");
  else
  {
    pthread_mutex_lock(&mutex);
    Downlink* dP = downlinkTake(device);
    pthread_mutex_unlock(&mutex);

    if (dP == NULL)
      status = reply(bodyP, 204, NULL);
    else
    {
      char hex[2 * DOWNLINK_SIZE + 1];
      char body[512];

      sigfoxHexEncode(dP->data, DOWNLINK_SIZE, hex);
      snprintf(body, sizeof(body), "{\"%s\":{\"downlinkData\":\"%s\"}}", device, hex);   // the device id: no '"' - Sigfox's is hex
      COR_V("iotasigfox: the downlink of %s: %s", device, hex);

      free(dP->device);
      free(dP);
      status = reply(bodyP, 200, body);
    }
  }

  corAllocBufferReset(&ka, false);
  return status;
}



// -----------------------------------------------------------------------------
//
// sigfoxSouthOpen -
//
int sigfoxSouthOpen(unsigned short port, const char* _idField, const BridgeBroker* _brokerP)
{
  brokerP = _brokerP;

  if (_idField != NULL)
  {
    if ((_idField[0] == 0) || (strlen(_idField) >= sizeof(idField)))
    {
      COR_E("iotasigfox: southbound idField '%s' - a parameter name", _idField);
      return BRIDGE_ERR;
    }
    snprintf(idField, sizeof(idField), "%s", _idField);
  }

  if ((brokerP->abiVersion < 16) || (brokerP->httpListen == NULL))
  {
    COR_E("iotasigfox: the southbound port %u needs a broker that serves it (bridge ABI 16, httpListen)", port);
    return BRIDGE_ERR;
  }

  if (brokerP->httpListen("iotasigfox", port, serve) != BRIDGE_OK)
  {
    COR_E("iotasigfox: cannot listen on the southbound port %u", port);
    return BRIDGE_ERR;
  }

  COR_V("iotasigfox: the southbound port %u", port);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// sigfoxSouthClose -
//
void sigfoxSouthClose(void)
{
  pthread_mutex_lock(&mutex);

  while (endpointList != NULL)
  {
    Endpoint* eP = endpointList;

    endpointList = eP->next;
    free(eP->endpoint);
    free(eP->path);
    free(eP->device);
    free(eP);
  }

  while (downlinkList != NULL)
  {
    Downlink* dP = downlinkList;

    downlinkList = dP->next;
    free(dP->device);
    free(dP);
  }
  downlinkCount = 0;

  pthread_mutex_unlock(&mutex);
  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// infoRead - a DataChannel's channelInfo ([ { "key": ..., "value": ... } ]): "mapping" and "downlinkMapping"
// into the endpoint; false: a key or a format refused
//
static bool infoRead(const char* endpoint, const char* info, Endpoint* eP)
{
  if (info == NULL)
    return true;

  CorAlloc ka;
  CorJson  cj;
  char     kaBuf[8 * 1024];
  char*    copy = strdup(info);
  bool     ok   = true;

  if (copy == NULL)
    return false;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "iotasigfox info");
  corJsonCreate(&cj, &ka);

  CorNode* arrayP = corJsonParse(&cj, copy);

  for (CorNode* itemP = ((arrayP != NULL) && (arrayP->type == CorArray)) ? arrayP->value.head : NULL; (itemP != NULL) && ok; itemP = itemP->next)
  {
    CorNode* keyP   = corTreeLookup(itemP, "key");
    CorNode* valueP = corTreeLookup(itemP, "value");
    char     why[256];

    if ((keyP == NULL) || (keyP->type != CorString) || (valueP == NULL) || (valueP->type != CorString))
    {
      COR_W("iotasigfox: DataChannel '%s' - a channelInfo item is { \"key\": <string>, \"value\": <string> }", endpoint);
      ok = false;
    }
    else if (strcmp(keyP->value.s, "mapping") == 0)
    {
      if ((ok = sigfoxFormatParse(valueP->value.s, &eP->up, why, sizeof(why))) == false)
        COR_W("iotasigfox: DataChannel '%s' - mapping: %s", endpoint, why);
      eP->mapped = ok;
    }
    else if (strcmp(keyP->value.s, "downlinkMapping") == 0)
    {
      if ((ok = sigfoxFormatParse(valueP->value.s, &eP->down, why, sizeof(why))) == false)
        COR_W("iotasigfox: DataChannel '%s' - downlinkMapping: %s", endpoint, why);
      else if (eP->down.length > DOWNLINK_SIZE)
      {
        COR_W("iotasigfox: DataChannel '%s' - downlinkMapping covers %d bytes: a downlink is %d", endpoint, eP->down.length, DOWNLINK_SIZE);
        ok = false;
      }
      eP->downlinks = ok;
    }
    else
    {
      COR_W("iotasigfox: DataChannel '%s' - channelInfo key '%s': mapping or downlinkMapping", endpoint, keyP->value.s);
      ok = false;
    }
  }

  corAllocBufferReset(&ka, false);
  free(copy);
  return ok;
}



// -----------------------------------------------------------------------------
//
// sigfoxEndpointAdd -
//
int sigfoxEndpointAdd(const char* endpoint, const char* info)
{
  char  copy[512];
  char* device;

  if (strlen(endpoint) >= sizeof(copy))
    return BRIDGE_BAD_INPUT;
  strcpy(copy, endpoint);

  if ((copy[0] == 0) || (copy[0] == '/'))
    return BRIDGE_BAD_INPUT;

  if ((device = strchr(copy, '/')) != NULL)
  {
    *device++ = 0;
    if ((device[0] == 0) || (strchr(device, '/') != NULL) || (strchr(device, '"') != NULL))
      return BRIDGE_BAD_INPUT;
  }

  Endpoint* eP = calloc(1, sizeof(Endpoint));

  if (eP == NULL)
    return BRIDGE_ERR;

  if (infoRead(endpoint, info, eP) == false)
  {
    free(eP);
    return BRIDGE_BAD_INPUT;
  }

  eP->endpoint = strdup(endpoint);
  eP->path     = strdup(copy);
  eP->device   = (device != NULL) ? strdup(device) : NULL;

  pthread_mutex_lock(&mutex);
  eP->next     = endpointList;
  endpointList = eP;
  pthread_mutex_unlock(&mutex);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// sigfoxEndpointDel -
//
int sigfoxEndpointDel(const char* endpoint)
{
  Endpoint* foundP = NULL;

  pthread_mutex_lock(&mutex);

  for (Endpoint** epP = &endpointList; *epP != NULL; epP = &(*epP)->next)
  {
    if (strcmp((*epP)->endpoint, endpoint) == 0)
    {
      foundP = *epP;
      *epP   = foundP->next;
      break;
    }
  }

  pthread_mutex_unlock(&mutex);

  if (foundP == NULL)
    return BRIDGE_NOT_FOUND;

  free(foundP->endpoint);
  free(foundP->path);
  free(foundP->device);
  free(foundP);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// sigfoxPublish - the fragment's columns encoded by the DataChannel's downlinkMapping, into the device's
// queued downlink: a write before the device has called back is added to the one queued (a field not
// written is 0). The device: the fragment's @deviceId (the id template's), else the endpoint's own.
//
int sigfoxPublish(const char* endpoint, const char* json)
{
  SigfoxFormat down;
  bool         found       = false;
  bool         writable    = false;
  char         device[256] = "";

  pthread_mutex_lock(&mutex);

  for (Endpoint* eP = endpointList; eP != NULL; eP = eP->next)
  {
    if (strcmp(eP->endpoint, endpoint) == 0)
    {
      found    = true;
      writable = eP->downlinks;
      if (writable)
        down = eP->down;
      if (eP->device != NULL)
        snprintf(device, sizeof(device), "%s", eP->device);
      break;
    }
  }

  pthread_mutex_unlock(&mutex);

  if (found == false)
    return BRIDGE_NOT_FOUND;

  if (writable == false)
  {
    COR_W("iotasigfox: a write on '%s' refused - the DataChannel has no downlinkMapping", endpoint);
    return BRIDGE_UNSUPPORTED;
  }

  CorAlloc ka;
  CorJson  cj;
  char     buf[8 * 1024];
  char*    copy = strdup(json);

  if (copy == NULL)
    return BRIDGE_ERR;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotasigfox downlink");
  corJsonCreate(&cj, &ka);

  CorNode* fragmentP = corJsonParse(&cj, copy);
  int      r         = BRIDGE_OK;

  if ((fragmentP == NULL) || (fragmentP->type != CorObject))
    r = BRIDGE_BAD_INPUT;
  else
  {
    CorNode* idP = corTreeLookup(fragmentP, "@deviceId");

    if ((idP != NULL) && (idP->type == CorString) && (idP->value.s[0] != 0))
      snprintf(device, sizeof(device), "%s", idP->value.s);

    if ((device[0] == 0) || (strchr(device, '"') != NULL) || (strchr(device, '\\') != NULL))
    {
      COR_W("iotasigfox: a write on '%s' refused - which device: no @deviceId in the entity's id template, and the endpoint names none", endpoint);
      r = BRIDGE_BAD_INPUT;
    }
  }

  if (r == BRIDGE_OK)
  {
    pthread_mutex_lock(&mutex);

    Downlink*     dP = NULL;
    unsigned char frame[DOWNLINK_SIZE] = { 0 };

    for (dP = downlinkList; (dP != NULL) && (strcasecmp(dP->device, device) != 0); dP = dP->next)
      ;

    if (dP != NULL)
      memcpy(frame, dP->data, DOWNLINK_SIZE);

    for (CorNode* memberP = fragmentP->value.head; (memberP != NULL) && (r == BRIDGE_OK); memberP = memberP->next)
    {
      if (memberP->name[0] == '@')
        continue;

      const char* why = sigfoxEncode(&down, memberP->name, memberP, frame);

      if (why != NULL)
      {
        COR_W("iotasigfox: a write of %s on '%s' refused - %s: %s", device, endpoint, memberP->name, why);
        r = BRIDGE_BAD_INPUT;
      }
    }

    if ((r == BRIDGE_OK) && (dP == NULL))
    {
      if (downlinkCount >= DOWNLINKS_MAX)
      {
        COR_W("iotasigfox: a write of %s on '%s' refused - %d downlinks are waiting for their devices", device, endpoint, DOWNLINKS_MAX);
        r = BRIDGE_ERR;
      }
      else if (((dP = calloc(1, sizeof(Downlink))) == NULL) || ((dP->device = strdup(device)) == NULL))
      {
        free(dP);
        dP = NULL;
        r  = BRIDGE_ERR;
      }
      else
      {
        dP->next     = downlinkList;
        downlinkList = dP;
        downlinkCount += 1;
      }
    }

    if (r == BRIDGE_OK)
    {
      char hex[2 * DOWNLINK_SIZE + 1];

      memcpy(dP->data, frame, DOWNLINK_SIZE);
      sigfoxHexEncode(frame, DOWNLINK_SIZE, hex);
      COR_V("iotasigfox: the downlink of %s queued: %s", device, hex);
    }

    pthread_mutex_unlock(&mutex);
  }

  corAllocBufferReset(&ka, false);
  free(copy);
  return r;
}
