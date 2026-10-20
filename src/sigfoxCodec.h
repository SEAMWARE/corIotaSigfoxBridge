#ifndef SIGFOX_CODEC_H_
#define SIGFOX_CODEC_H_

//
// FILE            sigfoxCodec.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A Sigfox message's data as values, and values as a downlink's data - by a payload format in the
// grammar of the Sigfox backend's custom payload configuration (and of iotagent-sigfox's "mapping"):
//
//   <name>:<byte index>:<type>:<arguments>     fields separated by white space
//
//   uint:<bits>[:little-endian|:big-endian]    bits 8, 16, ... 64 - big-endian when not said
//   int:<bits>[:little-endian|:big-endian]     two's complement
//   float:<32|64>[:little-endian|:big-endian]  IEEE 754
//   bool:<bit>                                 bit 0 (the least significant) to 7 of the byte
//   char:<length>                              that many bytes, a string (up to its first NUL)
//
// The byte index may be left out (iotagent-sigfox's "counter::uint:32"): the field follows the one
// before it - a bool after a bool is in the same byte.
//
#include <stdbool.h>                                  // bool

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// SigfoxFieldType -
//
typedef enum SigfoxFieldType
{
  SigfoxUint,
  SigfoxInt,
  SigfoxFloat,
  SigfoxBool,
  SigfoxChar
} SigfoxFieldType;



// -----------------------------------------------------------------------------
//
// SigfoxField - one field of a format
//
typedef struct SigfoxField
{
  char             name[64];
  SigfoxFieldType  type;
  int              byte;                              // where it starts
  int              size;                              // its bytes (a bool: 1)
  int              bit;                               // a bool's bit
  bool             little;                            // little-endian
} SigfoxField;



// -----------------------------------------------------------------------------
//
// SigfoxFormat - a payload format, parsed
//
#define SIGFOX_FIELDS_MAX  32

typedef struct SigfoxFormat
{
  SigfoxField  fieldV[SIGFOX_FIELDS_MAX];
  int          fields;
  int          length;                                // the bytes the fields cover
} SigfoxFormat;



// -----------------------------------------------------------------------------
//
// sigfoxFormatParse - a format's text; false: why in 'why'
//
extern bool sigfoxFormatParse(const char* text, SigfoxFormat* formatP, char* why, int whyLen);



// -----------------------------------------------------------------------------
//
// sigfoxHexDecode - hex text (either case) as bytes; their number, -1: not hex, or more than 'size' bytes
//
extern int sigfoxHexDecode(const char* hex, unsigned char* bytes, int size);



// -----------------------------------------------------------------------------
//
// sigfoxHexEncode - bytes as hex text, lower case (as the Sigfox backend writes {data}); 'hex' holds 2 * len + 1
//
extern void sigfoxHexEncode(const unsigned char* bytes, int len, char* hex);



// -----------------------------------------------------------------------------
//
// sigfoxDecode - the fields of 'data' as members of the object 'recordP', each named by its field; a field
// the data is too short for is left out (the Sigfox backend leaves it out as well)
//
extern void sigfoxDecode(CorAlloc* kaP, const SigfoxFormat* formatP, const unsigned char* data, int len, CorNode* recordP);



// -----------------------------------------------------------------------------
//
// sigfoxEncode - a value into its field of 'frame' (formatP->length bytes); NULL: done, else why it
// does not fit
//
extern const char* sigfoxEncode(const SigfoxFormat* formatP, const char* name, CorNode* valueP, unsigned char* frame);

#endif  // SIGFOX_CODEC_H_
