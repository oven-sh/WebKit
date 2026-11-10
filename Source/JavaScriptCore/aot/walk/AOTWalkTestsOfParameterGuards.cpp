/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"

namespace JSC { namespace AOT { namespace Typed {

static TypeID withoutOmittedNullishForTestsOfGuards(TypeArena& types, TypeID type, const DeclaredType& declared)
{
    uint16_t omitted = 0;
    if (declared.has(PositionFlags::OmitsNull))
        omitted |= Primitive::Null;
    if (declared.has(PositionFlags::OmitsUndefined))
        omitted |= Primitive::Undefined;
    if (!omitted)
        return type;
    TypeID written = types.withoutPrimitives(type, omitted);
    return isNeverType(written) ? type : written;
}

static bool mayTestForLessThanIsWrittenForTestsOfGuards(const Function& function, uint32_t entryParameter)
{
    if (function.role != FunctionRole::Source || function.constructedClass || function.has(FunctionFlags::HasNewTarget))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        break;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    switch (function.entryParameterKind(entryParameter)) {
    case EntryParameter::This:
        return true;
    case EntryParameter::Parameter:
        return entryParameter - function.firstParameterIndex() < function.numberOfPositionalParameters();
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeThatCastsOfOnlyMentionEnterForTestsOfGuards(TypeArena& types, const BodyIndex& index, ValueID parameter)
{
    if (index.block(entryBlock).hasHandler())
        return { };
    TypeID entered;
    ValueID name = parameter;
    for (ValueID user = index.onlyUsingValue(name); user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(name)) {
        BodyValue cast = index.value(user);
        if (cast.operand(0) != name)
            break;
        name = user;
        if (cast.opcode() != Opcode::Convert)
            continue;
        DeclaredType position = cast.declared();
        if (!position.isGuarded() || !position.written() || position.has(PositionFlags::IsInt32Promise) || describesBagOfOptions(types, position.written()))
            continue;
        TypeID written = withoutOmittedNullishForTestsOfGuards(types, types.testablePartOf(position.written()), position);
        entered = entered ? types.intersectionOf(entered, written) : written;
    }
    return entered;
}

TypeID withoutOmittedNullish(Walker& walker, FunctionID id, uint32_t entryParameter, TypeID type)
{
    const DeclaredType* declared = walker.program().at(id).declaredEntryParameter(entryParameter);
    return type && declared ? withoutOmittedNullishForTestsOfGuards(walker.types(), type, *declared) : type;
}

static TypeID withoutUndefinedOfElements(Walker& walker, FunctionID id, uint32_t entryParameter, TypeID seen)
{
    TypeArena& types = walker.types();
    const DeclaredType* declared = walker.program().at(id).declaredEntryParameter(entryParameter);
    Type type = types.at(seen);
    bool mayBeElement = !type.hasObjects() && (type.primitives & ~Primitive::UndefinedOrNull);
    return mayBeElement && declared && declared->has(PositionFlags::OmitsUndefined) ? types.withoutPrimitives(seen, Primitive::Undefined) : seen;
}

static bool everyArrivalInSightFitsForTestsOfGuards(Walker& walker, FunctionID id, uint32_t entryParameter, TypeID tested, TypeID narrower)
{
    const LinkTables& held = walker.facts().held();
    if (id.index() >= held.functions.size())
        return true;
    std::span<const LinkedParameter> parameters = held.parametersOf(id);
    TypeID seen = entryParameter < parameters.size() ? parameters[entryParameter].proven : TypeID();
    if (!seen)
        return true;
    TypeArena& types = walker.types();
    Type type = types.at(seen);
    if (!type.isJavaScriptValue())
        return false;
    if (type.isAnything())
        return true;
    seen = types.intersectionOf(withoutUndefinedOfElements(walker, id, entryParameter, seen), tested);
    if (types.withOnlyObjects(tested) == types.withOnlyObjects(narrower))
        seen = types.withoutObjects(seen);
    return types.isSubset(seen, narrower);
}

TypeID typeTestedInPlaceOfWrittenType(Walker& walker, FunctionID id, uint32_t entryParameter, TypeID tested)
{
    if (!tested)
        return tested;
    const Function& function = walker.program().at(id);
    const DeclaredType* declared = function.declaredEntryParameter(entryParameter);
    if (!mayTestForLessThanIsWrittenForTestsOfGuards(function, entryParameter) || (declared && declared->has(PositionFlags::IsInt32Promise)))
        return tested;

    TypeArena& types = walker.types();
    TypeID narrowest = tested;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeID checked = typeThatCastsOfOnlyMentionEnterForTestsOfGuards(types, index, index.block(entryBlock).parameter(entryParameter));
    bool asksSameOfObjects = checked && types.withOnlyObjects(checked) == types.withOnlyObjects(tested);
    if (asksSameOfObjects && checked != tested && !isNeverType(checked) && types.isSubset(checked, tested) && changesCodeWhereProven(types, checked) && everyArrivalInSightFitsForTestsOfGuards(walker, id, entryParameter, tested, checked))
        narrowest = checked;
    if (declared) {
        TypeID written = withoutOmittedNullishForTestsOfGuards(types, narrowest, *declared);
        if (written != narrowest && everyArrivalInSightFitsForTestsOfGuards(walker, id, entryParameter, tested, written))
            narrowest = written;
    }
    return narrowest;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
