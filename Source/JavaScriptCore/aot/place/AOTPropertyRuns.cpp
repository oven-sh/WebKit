/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool canDescribe(NameClass nameClass)
{
    switch (nameClass) {
    case NameClass::Index:
    case NameClass::CanonicalNumericString:
    case NameClass::Proto:
        return false;
    case NameClass::Ordinary:
    case NameClass::Constructor:
    case NameClass::ObjectPrototypeMember:
    case NameClass::Length:
    case NameClass::Prototype:
    case NameClass::CallOrApply:
    case NameClass::Bind:
    case NameClass::LastIndex:
    case NameClass::Then:
    case NameClass::ToJSON:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canDescribe(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return canDescribe(program.nameClassOf(key));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasRepeatedKey(std::span<const PropertyKeyID> keys)
{
    Vector<PropertyKeyID, 8> sortedKeys(keys);
    std::ranges::sort(sortedKeys);
    return std::ranges::adjacent_find(sortedKeys) != sortedKeys.end();
}

bool namesPropertyRun(Opcode opcode)
{
    return opcode == Opcode::TryPutByIds || opcode == Opcode::CopyDataProperties || opcode == Opcode::CopyObject;
}

std::span<const PropertyKeyID> keysOfPropertyRun(const Value& value)
{
    RELEASE_ASSERT(namesPropertyRun(value.opcode));
    return value.opcode == Opcode::TryPutByIds ? value.keys() : value.excludedKeys();
}

std::optional<PlacedPropertyRun> propertyRunOf(const PlacementContext& context, FunctionID function, ValueID id)
{
    const Program& program = context.program;
    const Value& value = program.at(function).at(id);
    if (!namesPropertyRun(value.opcode))
        return std::nullopt;

    std::span<const PropertyKeyID> keys = keysOfPropertyRun(value);
    if (keys.empty())
        return std::nullopt;
    bool canDescribeEveryKey = std::ranges::all_of(keys, [&](PropertyKeyID key) {
        return canDescribe(program, key);
    });
    if (!canDescribeEveryKey || hasRepeatedKey(keys))
        return std::nullopt;
    if (context.build.exceeds(LimitID::MaxPropertyListNames, keys.size(), { .function = function, .value = id }))
        return std::nullopt;
    return PlacedPropertyRun { Vector<PropertyKeyID, 8>(keys) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
