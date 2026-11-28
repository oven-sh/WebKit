/*
 * Copyright (C) 2026 Anthropic PBC.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScriptFetchParameters.h"

#include <array>
#include <mutex>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringHash.h>

namespace JSC {

Ref<ScriptFetchParameters> ScriptFetchParameters::create(Type type)
{
    constexpr size_t sharedTypeCount = static_cast<size_t>(Type::Text) + 1;
    static_assert(static_cast<size_t>(Type::None) == 0 && static_cast<size_t>(Type::JavaScript) < sharedTypeCount && static_cast<size_t>(Type::WebAssembly) < sharedTypeCount && static_cast<size_t>(Type::JSON) < sharedTypeCount);
    if (static_cast<size_t>(type) >= sharedTypeCount)
        return createUnique(type);
    static LazyNeverDestroyed<std::array<RefPtr<ScriptFetchParameters>, sharedTypeCount>> shared;
    static std::once_flag once;
    std::call_once(once, [] {
        shared.construct();
        for (size_t i = 0; i < sharedTypeCount; ++i)
            shared.get()[i] = adoptRef(*new ScriptFetchParameters(static_cast<Type>(i)));
    });
    return *shared.get()[static_cast<size_t>(type)];
}

#if USE(BUN_JSC_ADDITIONS)
namespace ScriptFetchParametersInternal {

struct HostDefinedTypes {
    HashMap<String, ScriptFetchParameters::Type> byName;
    Vector<String> names;
};

static Lock lock;

static HostDefinedTypes& hostDefinedTypes() WTF_REQUIRES_LOCK(lock)
{
    static NeverDestroyed<HostDefinedTypes> types;
    return types;
}

}

auto ScriptFetchParameters::hostDefinedType(StringView name) -> Type
{
    Locker locker { ScriptFetchParametersInternal::lock };
    auto& types = ScriptFetchParametersInternal::hostDefinedTypes();
    if (auto known = types.byName.find<StringViewHashTranslator>(name); known != types.byName.end())
        return known->value;
    auto type = static_cast<Type>(Type::FirstHostDefined + types.names.size());
    types.names.append(name.toString().isolatedCopy());
    types.byName.add(types.names.last(), type);
    return type;
}

String ScriptFetchParameters::nameOfHostDefinedType(Type type)
{
    ASSERT(isHostDefined(type));
    Locker locker { ScriptFetchParametersInternal::lock };
    return ScriptFetchParametersInternal::hostDefinedTypes().names[type - Type::FirstHostDefined].isolatedCopy();
}
#endif

} // namespace JSC
