/*
 * Copyright (C) 2009-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include "ArgList.h"
#include "JSObject.h"

namespace JSC {

class JSONObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | HasStaticPropertyTable;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(JSONObject, Base);
        return &vm.plainObjectSpace();
    }

    static JSONObject* create(VM& vm, JSGlobalObject* globalObject, Structure* structure)
    {
        JSONObject* object = new (NotNull, allocateCell<JSONObject>(vm)) JSONObject(vm, structure);
        object->finishCreation(vm, globalObject);
        return object;
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_INFO;

private:
    JSONObject(VM&, Structure*);
    void finishCreation(VM&, JSGlobalObject*);
};

JS_EXPORT_PRIVATE JSValue JSONParse(JSGlobalObject*, StringView);
// Bytes that are all ASCII are parsed where they are. Empty without an exception for any other bytes: they are parsed as a string.
JS_EXPORT_PRIVATE JSValue JSONParse(JSGlobalObject*, std::span<const char8_t> utf8);
enum class CompiledJSONParser : bool { Try, Skip };
JSValue JSONParseWithException(JSGlobalObject*, StringView, CompiledJSONParser = CompiledJSONParser::Try);
JS_EXPORT_PRIVATE String JSONStringify(JSGlobalObject*, JSValue, JSValue space);
JS_EXPORT_PRIVATE String JSONStringify(JSGlobalObject*, JSValue, unsigned indent);

#if USE(BUN_JSC_ADDITIONS)
struct StreamingJSONParseResult {
    size_t charactersConsumed;
    // NeedsText: the bytes are not all ASCII. Nothing was parsed, and they are to be parsed as a string.
    enum class Status : uint8_t { Complete, NeedMoreData, Error, NeedsText } status;
};

JS_EXPORT_PRIVATE StreamingJSONParseResult streamingJSONParse(
    JSGlobalObject*, StringView input, MarkedArgumentBuffer& results);
// charactersConsumed counts bytes.
JS_EXPORT_PRIVATE StreamingJSONParseResult streamingJSONParse(
    JSGlobalObject*, std::span<const char8_t> utf8, MarkedArgumentBuffer& results);
#endif

} // namespace JSC
