/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONScanner.h"
#include "Identifier.h"
#include "JSArray.h"
#include "JSObject.h"
#include "MarkedVector.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class JSONValueParser {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(JSONValueParser);
public:
    JSONValueParser(Instance&, VM&, JSGlobalObject*);

    ParsedJSONValue parse(const uint8_t* start);
    ParsedJSONValue parseWholeSource(const uint8_t* start);

    struct ParsedName {
        Identifier name;
        const uint8_t* afterColon { nullptr };
    };
    ParsedName parseNameAndColon(const uint8_t* quote);

private:
    using Cursor = const uint8_t*;

    static constexpr size_t inlineElementCapacity = 32;

    struct Members {
        Structure* structure { nullptr };
        unsigned nameIndex { 0 };
        unsigned count { 0 };
        EncodedJSValue values[JSFinalObject::maxInlineCapacity];
    };

    struct NextMember {
        Structure* structure { nullptr };
        Cursor value { nullptr };
        unsigned nameIndex { 0 };
    };

    ParsedJSONValue parseValue(Cursor);
    ParsedJSONValue parseString(Cursor quote);
    ParsedJSONValue parseNumber(Cursor);
    ParsedJSONValue parseObject(Cursor brace);
    ParsedJSONValue parseArray(Cursor bracket);

    NextMember findKnownMember(const Members&, Cursor quote);
    NextMember findOrAddMember(const Members&, Cursor quote);
    JSObject* createObject(Members&);
    ParsedJSONValue parseRemainingMembers(JSObject*, Cursor quote);
    JSArray* createArray(const JSValue* values, unsigned length);

    Instance& m_instance;
    const JSONWords& m_words;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSONCache& m_cache;
    JSONCache::ParsingScope m_parsingScope;
    MarkedArgumentBufferWithSize<inlineElementCapacity> m_elements;
};

ParsedJSONValue parseJSONValueAt(Instance&, VM&, JSGlobalObject*, const uint8_t* start);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
