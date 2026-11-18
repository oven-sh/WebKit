/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKeyedLookup.h"

#if ENABLE(AOT)

#include "AOTCompiledCallee.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotAccess.h"
#include "JSCInlines.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

KeyedLookup::KeyedLookup(const JSCell* callee, JSGlobalObject* realmOfCallee)
    : m_callee(callee)
{
    std::optional<uint32_t> closureNumber = closureNumberOf(callee);
    if (!closureNumber)
        return;
    std::span<const ReturnedCaptureData> rows = programTables().keyedLookups();
    auto row = std::ranges::lower_bound(rows, *closureNumber + 1, { }, [](const ReturnedCaptureData& each) { return each.closureNumberPlusOne(); });
    if (row == rows.end() || row->closureNumberPlusOne() != *closureNumber + 1)
        return;
    m_row = &*row;
    m_instance = realmOfCallee->aotInstance();
    m_object = objectInVariable();
}

JSObject* KeyedLookup::objectInVariable() const
{
    if (!m_row)
        return nullptr;
    static constexpr uint32_t offsetMask = (1u << 24) - 1;
    uint32_t base = m_row->closureNumberAndBase >> ReturnedCaptureData::numberOfClosureNumberBits;
    const auto* holder = reinterpret_cast<const uint8_t*>(m_callee);
    if (base == ReturnedCaptureData::baseOfStatics)
        holder = m_instance->statics();
    else if (base != ReturnedCaptureData::baseOfClosure)
        holder = *reinterpret_cast<const uint8_t* const*>(holder + (base << ReturnedCaptureData::baseShift));
    const uint8_t* variable = holder + (m_row->slot & offsetMask);
    auto representation = static_cast<SlotRepresentation>(m_row->slot >> 24);
    if (representation != SlotRepresentation::JSValue && slotHoldsMarker(representation, variable))
        return nullptr;
    JSValue value = readSlot(representation, variable, *m_instance);
    return value && value.isObject() ? asObject(value) : nullptr;
}

JSValue KeyedLookup::tryGetOwn(JSGlobalObject* globalObject, JSString* key)
{
    Structure* structure = m_object->structure();
    if (!JSCell::canUseFastGetOwnProperty(*structure))
        return { };
    auto name = key->toExistingAtomString(globalObject);
    if (!name)
        return { };
    std::optional<uint32_t> index = parseIndex(PropertyName(name));
    if (!index)
        return m_object->fastGetOwnProperty(globalObject->vm(), *structure, PropertyName(name));
    return m_object->canGetIndexQuickly(*index) ? m_object->getIndexQuickly(*index) : JSValue();
}

JSValue KeyedLookup::tryGetFromObject(JSGlobalObject* globalObject, JSString* key)
{
    JSValue value = tryGetOwn(globalObject, key);
    if (!value)
        value = JSValue::decode(operationAOTGetByValGeneric(m_instance, m_object, key));
    return value && value.isObject() ? JSValue() : value;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
