//
// FILE            sigfoxCodec.c
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
// A Sigfox payload format, its data decoded and encoded - see header.
//
#include <math.h>                                     // floor
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t, int64_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // strtol
#include <string.h>                                   // strcmp, strchr, strlen, memcpy, memset

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeInteger, corTreeFloat, corTreeBoolean, corTreeString, corTreeChildAdd

#include "sigfoxCodec.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// fieldParse - one "<name>:<byte index>:<type>:<arguments>"; false: why
//
static bool fieldParse(char* text, SigfoxField* fP, const SigfoxField* prevP, char* why, int whyLen)
{
  char* partV[5] = { NULL };
  int   parts    = 0;

  for (char* s = text; (s != NULL) && (parts < 5); )
  {
    partV[parts++] = s;
    s = strchr(s, ':');
    if (s != NULL)
      *s++ = 0;
  }

  if ((parts < 4) || (partV[0][0] == 0))
  {
    snprintf(why, whyLen, "'%s' is not <name>:<byte index>:<type>:<arguments>", text);
    return false;
  }

  if (strlen(partV[0]) >= sizeof(fP->name))
  {
    snprintf(why, whyLen, "the field name '%s' is too long", partV[0]);
    return false;
  }

  memset(fP, 0, sizeof(SigfoxField));
  strcpy(fP->name, partV[0]);

  const char* type = partV[2];
  const char* arg  = partV[3];
  const char* end  = (parts == 5) ? partV[4] : NULL;
  char*       rest;
  long        n    = strtol(arg, &rest, 10);

  if ((arg[0] == 0) || (*rest != 0))
  {
    snprintf(why, whyLen, "field '%s': '%s' is not a number", fP->name, arg);
    return false;
  }

  if (end != NULL)
  {
    if (strcmp(end, "little-endian") == 0)
      fP->little = true;
    else if (strcmp(end, "big-endian") != 0)
    {
      snprintf(why, whyLen, "field '%s': '%s' is little-endian or big-endian", fP->name, end);
      return false;
    }
  }

  if ((strcmp(type, "uint") == 0) || (strcmp(type, "int") == 0))
  {
    fP->type = (type[0] == 'u') ? SigfoxUint : SigfoxInt;
    if ((n < 8) || (n > 64) || ((n % 8) != 0))
    {
      snprintf(why, whyLen, "field '%s': %s of %ld bits - 8, 16, ... 64 (a field of fewer bits is not built)", fP->name, type, n);
      return false;
    }
    fP->size = n / 8;
  }
  else if (strcmp(type, "float") == 0)
  {
    fP->type = SigfoxFloat;
    if ((n != 32) && (n != 64))
    {
      snprintf(why, whyLen, "field '%s': a float is 32 or 64 bits", fP->name);
      return false;
    }
    fP->size = n / 8;
  }
  else if ((strcmp(type, "bool") == 0) && (end == NULL))
  {
    fP->type = SigfoxBool;
    if ((n < 0) || (n > 7))
    {
      snprintf(why, whyLen, "field '%s': a bool's bit is 0 to 7", fP->name);
      return false;
    }
    fP->bit  = n;
    fP->size = 1;
  }
  else if ((strcmp(type, "char") == 0) && (end == NULL))
  {
    fP->type = SigfoxChar;
    if ((n < 1) || (n > 12))
    {
      snprintf(why, whyLen, "field '%s': a char field is 1 to 12 bytes", fP->name);
      return false;
    }
    fP->size = n;
  }
  else
  {
    snprintf(why, whyLen, "field '%s': type '%s%s%s' - uint, int, float, bool or char", fP->name, type, (end != NULL) ? ":...:" : "", (end != NULL) ? end : "");
    return false;
  }

  //
  // Where it starts: its byte index, else after the field before it - a bool after a bool in its byte
  //
  if (partV[1][0] != 0)
  {
    long b = strtol(partV[1], &rest, 10);

    if ((*rest != 0) || (b < 0) || (b > 11))
    {
      snprintf(why, whyLen, "field '%s': the byte index '%s' is 0 to 11", fP->name, partV[1]);
      return false;
    }
    fP->byte = b;
  }
  else if (prevP == NULL)
    fP->byte = 0;
  else if ((fP->type == SigfoxBool) && (prevP->type == SigfoxBool))
    fP->byte = prevP->byte;
  else
    fP->byte = prevP->byte + prevP->size;

  if (fP->byte + fP->size > 12)
  {
    snprintf(why, whyLen, "field '%s' ends past the 12 bytes of a Sigfox message", fP->name);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// sigfoxFormatParse -
//
bool sigfoxFormatParse(const char* text, SigfoxFormat* formatP, char* why, int whyLen)
{
  char copy[2048];

  if (strlen(text) >= sizeof(copy))
  {
    snprintf(why, whyLen, "the format is too long");
    return false;
  }
  strcpy(copy, text);
  memset(formatP, 0, sizeof(SigfoxFormat));

  char* s = copy;

  while (*s != 0)
  {
    while ((*s == ' ') || (*s == '\t') || (*s == '\n') || (*s == '\r'))
      ++s;
    if (*s == 0)
      break;

    char* fieldText = s;

    while ((*s != 0) && (*s != ' ') && (*s != '\t') && (*s != '\n') && (*s != '\r'))
      ++s;
    if (*s != 0)
      *s++ = 0;

    if (formatP->fields == SIGFOX_FIELDS_MAX)
    {
      snprintf(why, whyLen, "more than %d fields", SIGFOX_FIELDS_MAX);
      return false;
    }

    SigfoxField* fP    = &formatP->fieldV[formatP->fields];
    SigfoxField* prevP = (formatP->fields > 0) ? &formatP->fieldV[formatP->fields - 1] : NULL;

    if (fieldParse(fieldText, fP, prevP, why, whyLen) == false)
      return false;

    for (int ix = 0; ix < formatP->fields; ix++)
    {
      if (strcmp(formatP->fieldV[ix].name, fP->name) == 0)
      {
        snprintf(why, whyLen, "two fields named '%s'", fP->name);
        return false;
      }
    }

    if (fP->byte + fP->size > formatP->length)
      formatP->length = fP->byte + fP->size;

    formatP->fields += 1;
  }

  if (formatP->fields == 0)
  {
    snprintf(why, whyLen, "no fields");
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// sigfoxHexDecode -
//
int sigfoxHexDecode(const char* hex, unsigned char* bytes, int size)
{
  int len = strlen(hex);

  if (((len % 2) != 0) || (len / 2 > size))
    return -1;

  for (int ix = 0; ix < len / 2; ix++)
  {
    int v = 0;

    for (int d = 0; d < 2; d++)
    {
      char c = hex[2 * ix + d];

      v <<= 4;
      if ((c >= '0') && (c <= '9'))       v |= c - '0';
      else if ((c >= 'a') && (c <= 'f'))  v |= c - 'a' + 10;
      else if ((c >= 'A') && (c <= 'F'))  v |= c - 'A' + 10;
      else                                return -1;
    }

    bytes[ix] = v;
  }

  return len / 2;
}



// -----------------------------------------------------------------------------
//
// sigfoxHexEncode -
//
void sigfoxHexEncode(const unsigned char* bytes, int len, char* hex)
{
  static const char digit[] = "0123456789abcdef";

  for (int ix = 0; ix < len; ix++)
  {
    hex[2 * ix]     = digit[bytes[ix] >> 4];
    hex[2 * ix + 1] = digit[bytes[ix] & 0xF];
  }
  hex[2 * len] = 0;
}



// -----------------------------------------------------------------------------
//
// bitsRead / bitsWrite - a field's bytes as an unsigned number, by its endianness
//
static uint64_t bitsRead(const SigfoxField* fP, const unsigned char* data)
{
  uint64_t v = 0;

  for (int ix = 0; ix < fP->size; ix++)
  {
    int at = fP->little ? (fP->byte + fP->size - 1 - ix) : (fP->byte + ix);

    v = (v << 8) | data[at];
  }

  return v;
}

static void bitsWrite(const SigfoxField* fP, uint64_t v, unsigned char* frame)
{
  for (int ix = fP->size - 1; ix >= 0; ix--)
  {
    int at = fP->little ? (fP->byte + fP->size - 1 - ix) : (fP->byte + ix);

    frame[at] = v & 0xFF;
    v >>= 8;
  }
}



// -----------------------------------------------------------------------------
//
// float32Shortest - a float as the double of its shortest decimal: 21.3, not 21.299999237060547
//
static double float32Shortest(float f)
{
  char buf[32];

  for (int digits = 6; digits < 9; digits++)
  {
    snprintf(buf, sizeof(buf), "%.*g", digits, f);
    if ((float) strtod(buf, NULL) == f)
      return strtod(buf, NULL);
  }

  return f;
}



// -----------------------------------------------------------------------------
//
// sigfoxDecode -
//
void sigfoxDecode(CorAlloc* kaP, const SigfoxFormat* formatP, const unsigned char* data, int len, CorNode* recordP)
{
  for (int ix = 0; ix < formatP->fields; ix++)
  {
    const SigfoxField* fP     = &formatP->fieldV[ix];
    CorNode*           valueP = NULL;

    if (fP->byte + fP->size > len)
      continue;

    uint64_t bits = (fP->type == SigfoxChar) ? 0 : bitsRead(fP, data);

    switch (fP->type)
    {
    case SigfoxUint:
      if (bits > INT64_MAX)
        valueP = corTreeFloat(kaP, fP->name, (double) bits);
      else
        valueP = corTreeInteger(kaP, fP->name, (long long) bits);
      break;

    case SigfoxInt:
      if (fP->size < 8)
      {
        uint64_t sign = 1ULL << (fP->size * 8 - 1);

        if ((bits & sign) != 0)
          bits |= ~((sign << 1) - 1);                 // sign-extended
      }
      valueP = corTreeInteger(kaP, fP->name, (long long) (int64_t) bits);
      break;

    case SigfoxFloat:
      if (fP->size == 4)
      {
        uint32_t u = (uint32_t) bits;
        float    f;

        memcpy(&f, &u, 4);
        valueP = corTreeFloat(kaP, fP->name, float32Shortest(f));
      }
      else
      {
        double d;

        memcpy(&d, &bits, 8);
        valueP = corTreeFloat(kaP, fP->name, d);
      }
      if ((valueP != NULL) && (isfinite(valueP->value.f) == 0))
        valueP = corTreeNull(kaP, fP->name);          // NaN, infinity: no JSON number
      break;

    case SigfoxBool:
      valueP = corTreeBoolean(kaP, fP->name, ((data[fP->byte] >> fP->bit) & 1) == 1);
      break;

    case SigfoxChar:
      {
        char* s = corAlloc(kaP, fP->size + 1);

        if (s == NULL)
          return;

        int n = 0;

        for (; (n < fP->size) && (data[fP->byte + n] != 0); n++)
          s[n] = ((data[fP->byte + n] >= 0x20) && (data[fP->byte + n] < 0x7F)) ? data[fP->byte + n] : '?';
        s[n] = 0;

        valueP = corTreeString(kaP, fP->name, s);
      }
      break;
    }

    if (valueP != NULL)
      corTreeChildAdd(recordP, valueP);
  }
}



// -----------------------------------------------------------------------------
//
// sigfoxEncode -
//
const char* sigfoxEncode(const SigfoxFormat* formatP, const char* name, CorNode* valueP, unsigned char* frame)
{
  const SigfoxField* fP = NULL;

  for (int ix = 0; (ix < formatP->fields) && (fP == NULL); ix++)
  {
    if (strcmp(formatP->fieldV[ix].name, name) == 0)
      fP = &formatP->fieldV[ix];
  }

  if (fP == NULL)
    return "no such field in the downlink format";

  bool   integral = (valueP->type == CorInt) || ((valueP->type == CorFloat) && (floor(valueP->value.f) == valueP->value.f));
  double d        = (valueP->type == CorInt) ? (double) valueP->value.i : (valueP->type == CorFloat) ? valueP->value.f : 0;

  switch (fP->type)
  {
  case SigfoxUint:
  case SigfoxInt:
    {
      if (integral == false)
        return "not an integer";

      int bits = fP->size * 8;

      if (valueP->type == CorInt)
      {
        long long v = valueP->value.i;

        if (fP->type == SigfoxUint)
        {
          if ((v < 0) || ((bits < 64) && ((unsigned long long) v >= (1ULL << bits))))
            return "out of the field's range";
        }
        else if ((bits < 64) && ((v < -(1LL << (bits - 1))) || (v >= (1LL << (bits - 1)))))
          return "out of the field's range";

        bitsWrite(fP, (uint64_t) v, frame);
      }
      else
      {
        double max = ldexp(1.0, (fP->type == SigfoxUint) ? bits : bits - 1);
        double min = (fP->type == SigfoxUint) ? 0 : -max;

        if ((d < min) || (d >= max))
          return "out of the field's range";

        bitsWrite(fP, (fP->type == SigfoxUint) ? (uint64_t) d : (uint64_t) (int64_t) d, frame);
      }
    }
    break;

  case SigfoxFloat:
    if ((valueP->type != CorInt) && (valueP->type != CorFloat))
      return "not a number";

    if (fP->size == 4)
    {
      float    f = (float) d;
      uint32_t u;

      memcpy(&u, &f, 4);
      bitsWrite(fP, u, frame);
    }
    else
    {
      uint64_t u;

      memcpy(&u, &d, 8);
      bitsWrite(fP, u, frame);
    }
    break;

  case SigfoxBool:
    if (valueP->type != CorBoolean)
      return "not a boolean";

    if (valueP->value.b)
      frame[fP->byte] |= (1 << fP->bit);
    else
      frame[fP->byte] &= ~(1 << fP->bit);
    break;

  case SigfoxChar:
    if (valueP->type != CorString)
      return "not a string";
    if ((int) strlen(valueP->value.s) > fP->size)
      return "longer than the field";

    memset(&frame[fP->byte], 0, fP->size);
    memcpy(&frame[fP->byte], valueP->value.s, strlen(valueP->value.s));
    break;
  }

  return NULL;
}
