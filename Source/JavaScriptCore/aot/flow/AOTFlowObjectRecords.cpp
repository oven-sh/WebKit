/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFunctionKinds.h"

namespace JSC { namespace AOT { namespace Typed {

static BuiltinClassID builtinClassOf(const FlowSiteDescription& description)
{
    switch (description.kind) {
    case FlowSiteKind::Layout:
    case FlowSiteKind::Closure:
        return { };
    case FlowSiteKind::Instance:
    case FlowSiteKind::Opaque:
        return description.builtinClass;
    case FlowSiteKind::PlainObject:
        return BuiltinClasses::Object;
    case FlowSiteKind::PlainArray:
        return BuiltinClasses::Array;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t numberOfSlotsOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    return builtinClass ? static_cast<uint32_t>(builtins.at(builtinClass).slots.size()) : 0;
}

static FlowObject makeInstance(const BuiltinTable& builtins, BuiltinClassID builtinClass, uint16_t numberOfPositions)
{
    RELEASE_ASSERT(builtinClass);
    RELEASE_ASSERT(!numberOfPositions || indexedElementSlotOf(builtins, builtinClass));

    FlowObject record;
    record.kind = FlowObjectKind::Instance;
    record.builtinClass = builtinClass;
    record.numberOfDeclaredParts = numberOfSlotsOf(builtins, builtinClass);
    record.numberOfPositions = numberOfPositions;
    return record;
}

FlowObject makeFlowObject(const Program& program, const FlowSiteDescription& description)
{
    RELEASE_ASSERT(description.kind == FlowSiteKind::Instance || !description.numberOfPositions);

    const BuiltinTable& builtins = program.builtins();
    FlowObject record;
    switch (description.kind) {
    case FlowSiteKind::Layout:
        RELEASE_ASSERT(description.layout);
        record.kind = FlowObjectKind::Allocation;
        record.layout = description.layout;
        record.numberOfDeclaredParts = static_cast<uint32_t>(program.at(description.layout).fields.size());
        return record;
    case FlowSiteKind::Closure:
        RELEASE_ASSERT(description.closureFunction);
        record.kind = FlowObjectKind::Closure;
        record.function = description.closureFunction;
        record.numberOfDeclaredParts = static_cast<uint32_t>(program.at(description.closureFunction).captures.size());
        return record;
    case FlowSiteKind::Opaque:
        record.kind = FlowObjectKind::Opaque;
        record.builtinClass = description.builtinClass;
        record.numberOfDeclaredParts = numberOfSlotsOf(builtins, description.builtinClass);
        record.hasUndeclaredPart = true;
        return record;
    case FlowSiteKind::Instance:
    case FlowSiteKind::PlainObject:
    case FlowSiteKind::PlainArray:
        return makeInstance(builtins, builtinClassOf(description), description.numberOfPositions);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowAllocationClassMembers makeAllocationClassMembers(const FlowSiteDescription& description)
{
    FlowAllocationClassMembers members;
    switch (description.kind) {
    case FlowSiteKind::Layout:
        members.layout = description.layout;
        return members;
    case FlowSiteKind::Closure:
        members.closureFunction = description.closureFunction;
        return members;
    case FlowSiteKind::Instance:
    case FlowSiteKind::Opaque:
    case FlowSiteKind::PlainObject:
    case FlowSiteKind::PlainArray:
        members.builtinClass = builtinClassOf(description);
        return members;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<uint32_t, 8> requiredFieldsOf(const Layout& layout)
{
    Vector<uint32_t, 8> fields;
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        if (layout.fields[index].flags.contains(FieldFlags::IsRequired))
            fields.append(index);
    }
    return fields;
}

// The run time answers the same question with FunctionTables::hasPrototypePropertyAtCreation.
bool hasPrototypeProperty(const Program& program, const Function& function)
{
    switch (function.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        break;
    }
    if (isInRuntimeModule(program, function))
        return false;
    if (function.has(FunctionFlags::IsGenerator))
        return true;
    return function.kind == FunctionKind::Plain && !function.has(FunctionFlags::IsAsync);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
