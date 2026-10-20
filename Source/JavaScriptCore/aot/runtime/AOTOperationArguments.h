/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "ArgList.h"
#include "JSObject.h"
#include "PropertyName.h"
#include <optional>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

[[noreturn]] inline NEVER_INLINE void crashOnWrongOperand(Entry entry)
{
    dataLogLn("AOT: an operand of ", nameOf(entry), " has the wrong kind");
    CRASH();
}

template<typename T>
ALWAYS_INLINE T* cellArgument(JSCell* cell, Entry entry)
{
    T* result = cell ? dynamicDowncast<T>(cell) : nullptr;
    if (!result) [[unlikely]]
        crashOnWrongOperand(entry);
    return result;
}

ALWAYS_INLINE JSObject* objectArgument(EncodedJSValue value, Entry entry)
{
    JSValue decoded = JSValue::decode(value);
    if (!decoded.isObject()) [[unlikely]]
        crashOnWrongOperand(entry);
    return asObject(decoded);
}

template<typename T>
ALWAYS_INLINE const T& requestAt(Instance* instance, uint32_t request)
{
    std::span<const uint32_t> requests = instance->tables().requests();
    ASSERT(request + sizeof(T) <= requests.size_bytes() && !(request % alignof(T)));
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(requests.data()) + request);
}

inline void fill(MarkedArgumentBuffer& buffer, uint32_t count, const EncodedJSValue* values)
{
    for (uint32_t i = 0; i < count; ++i)
        buffer.append(JSValue::decode(values[i]));
}

ALWAYS_INLINE std::optional<uint32_t> indexNamedBy(uint32_t name, const Identifier& identifier)
{
    if (Image::current()->text().identifier(name).nameClass() != NameClass::Index) [[likely]]
        return std::nullopt;
    return parseIndex(identifier);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
