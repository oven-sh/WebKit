/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Python {

// The codecs written in C++ and the registry of the others. (str.encode() and bytes.decode() are strEncode() and bytesDecode(),
// in PyInternals.h.) An empty value comes back with an exception.
//
// `errors` is the name of an error handler; a null string is "strict".

// ---- Encoders: from a str to bytes ----

JSValue codecEncodeUTF8(JSGlobalObject*, JSString*, const String& errors);
JSValue codecEncodeASCII(JSGlobalObject*, JSString*, const String& errors);
JSValue codecEncodeLatin1(JSGlobalObject*, JSString*, const String& errors);
JSValue codecEncodeUTF7(JSGlobalObject*, JSString*, const String& errors);
// Negative: little endian. Positive: big endian. 0: that of the machine, after a byte order mark.
JSValue codecEncodeUTF16(JSGlobalObject*, JSString*, const String& errors, int byteOrder);
JSValue codecEncodeUTF32(JSGlobalObject*, JSString*, const String& errors, int byteOrder);
JSValue codecEncodeUnicodeEscape(JSGlobalObject*, JSString*);
JSValue codecEncodeRawUnicodeEscape(JSGlobalObject*, JSString*);
// `mapping` maps code points to an int, bytes or None. Empty: Latin-1.
JSValue codecEncodeCharmap(JSGlobalObject*, JSString*, JSValue mapping, const String& errors);

// ---- Decoders: from bytes to a str ----
//
// The bytes must stay where they are until the decoder returns, whatever Python code it runs.
// With `consumed`, data that ends in the middle of a character is no error: the decoder stops there and says how far it got.

JSValue codecDecodeUTF8(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
JSValue codecDecodeASCII(JSGlobalObject*, std::span<const uint8_t>, const String& errors);
JSValue codecDecodeLatin1(JSGlobalObject*, std::span<const uint8_t>);
JSValue codecDecodeUTF7(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
// `byteOrder`, if not null, is as for the encoders; 0 becomes what a byte order mark at the start says, which is then skipped.
JSValue codecDecodeUTF16(JSGlobalObject*, std::span<const uint8_t>, const String& errors, int* byteOrder = nullptr, size_t* consumed = nullptr);
JSValue codecDecodeUTF32(JSGlobalObject*, std::span<const uint8_t>, const String& errors, int* byteOrder = nullptr, size_t* consumed = nullptr);
JSValue codecDecodeUnicodeEscape(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
JSValue codecDecodeRawUnicodeEscape(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
// `mapping` is a str indexed by byte, or maps bytes to an int, a str or None. Empty: Latin-1.
JSValue codecDecodeCharmap(JSGlobalObject*, std::span<const uint8_t>, JSValue mapping, const String& errors);

// ---- The registry ----

// codecs.lookup(encoding): the tuple, usually a CodecInfo, of (encoder, decoder, stream reader, stream writer).
JSValue codecLookup(JSGlobalObject*, const String& encoding);
// To be called before the name of a character is looked up or given. The names are the engine's, but in CPython they come with the
// module unicodedata, which is imported when one is first needed: a program can see that, and can make it fail. False: it did.
bool codecLoadCharacterNames(JSGlobalObject*);
// The same, for a codec between str and bytes: any other is a LookupError, which points to `alternateCommand` if that is not null.
JSValue codecLookupTextEncoding(JSGlobalObject*, const String& encoding, ASCIILiteral alternateCommand);
// codecs.lookup_error(name)
JSValue codecLookupError(JSGlobalObject*, const String& name);
// codecs.encode(object, encoding, errors), codecs.decode(object, encoding, errors)
JSValue codecEncodeObject(JSGlobalObject*, JSValue object, const String& encoding, const String& errors);
JSValue codecDecodeObject(JSGlobalObject*, JSValue object, const String& encoding, const String& errors);
// codecInfo.incrementalencoder(errors), codecInfo.incrementaldecoder(errors)
JSValue codecIncrementalEncoder(JSGlobalObject*, JSValue codecInfo, const String& errors);
JSValue codecIncrementalDecoder(JSGlobalObject*, JSValue codecInfo, const String& errors);

} // namespace Python
} // namespace JSC
