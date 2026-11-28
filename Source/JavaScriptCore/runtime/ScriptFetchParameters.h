/*
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>
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

#include <wtf/Ref.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/WTFString.h>
#if USE(BUN_JSC_ADDITIONS)
#include <wtf/HashMap.h>
#include <wtf/text/UniquedStringImpl.h>
#endif

namespace JSC {

// ThreadSafeRefCounted: the shared per-type instances (create(Type)) are process-wide and every VM's module records ref them.
class ScriptFetchParameters : public ThreadSafeRefCounted<ScriptFetchParameters> {
public:
#if USE(BUN_JSC_ADDITIONS)
    enum Type : uint32_t {
#else
    enum Type : uint8_t {
#endif
        None,
        JavaScript,
        WebAssembly,
        JSON,
        Text,
        CSS,
#if USE(BUN_JSC_ADDITIONS)
        // Every other name is numbered from here, as the process meets it.
        FirstHostDefined,
#endif
    };

    ScriptFetchParameters(Type type)
        : m_type(type)
#if USE(BUN_JSC_ADDITIONS)
        , m_hostDefinedImportType(isHostDefined(type) ? nameOfHostDefinedType(type) : String())
#endif
    {
    }

    virtual ~ScriptFetchParameters() = default;

    Type type() const { return m_type; }

    virtual const String& integrity() const { return nullString(); }
    virtual bool isTopLevelModule() const { return false; }


    // The plain per-type parameters carry nothing but the type, so every module request of a given type shares one
    // immortal instance instead of allocating its own (a large module graph makes thousands of requests). A request that
    // needs its own state (WebCore's ModuleFetchParameters, the name of a host-defined type, an attributes map) still allocates.
    JS_EXPORT_PRIVATE static Ref<ScriptFetchParameters> create(Type);
    static Ref<ScriptFetchParameters> createUnique(Type type) { return adoptRef(*new ScriptFetchParameters(type)); }


#if USE(BUN_JSC_ADDITIONS)
    static bool isHostDefined(Type type) { return type >= Type::FirstHostDefined; }
    JS_EXPORT_PRIVATE static Type hostDefinedType(StringView name);
    JS_EXPORT_PRIVATE static String nameOfHostDefinedType(Type);
    const String& hostDefinedImportType() const { return m_hostDefinedImportType; }

    using AttributesMap = HashMap<RefPtr<UniquedStringImpl>, String>;
    const AttributesMap& attributes() const { return m_attributes; }
    void setAttributes(AttributesMap&& attributes) { m_attributes = WTF::move(attributes); }
#endif

    static std::optional<Type> parseType(StringView string)
    {
        if (string == "json"_s)
            return Type::JSON;
#if !USE(BUN_JSC_ADDITIONS)
        // Bun loads `with { type: "text" }` and `with { type: "css" }` itself, so there they are host-defined types.
        if (string == "text"_s)
            return Type::Text;
        if (string == "css"_s)
            return Type::CSS;
#endif
        if (string == "webassembly"_s)
            return Type::WebAssembly;
#if USE(BUN_JSC_ADDITIONS)
        if (!string.isEmpty())
            return hostDefinedType(string);
#endif
        return std::nullopt;
    }

private:
    Type m_type { Type::None };
#if USE(BUN_JSC_ADDITIONS)
    String m_hostDefinedImportType;
    AttributesMap m_attributes;
#endif
    
};

} // namespace JSC
