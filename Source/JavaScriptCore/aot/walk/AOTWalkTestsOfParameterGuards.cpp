/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ParameterGuards: the guard at an entry tests for what the program has written, without the undefined and null that it did not write and that nobody in sight passes.
// At a suspension and at a throw: nothing. What the narrower test turns away goes on in the slow version.
// Withheld: no function has a guard, so nothing here is asked.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"

namespace JSC { namespace AOT { namespace Typed {

// The type of a position that holds a reference lists undefined and null whatever the program has written. The type itself where the program has written both, or nothing else.
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

// What the narrower test turns away goes on in the slow version: nothing throws and nothing is converted.
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

// `function len(p?: Point) { const q: Point = p!; return q.x + q.y }`: the cast is the one mention of the value, so what the program has written of that position is all that the body goes on with.
// Null: the body does something else with the value too, or casts it into no written position.
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
        // The program has to prove what enters a position declared int32: checkInt32Promise(). A test that the compiler adds for the sake of the cast would prove it to every build.
        if (!position.isGuarded() || !position.written() || position.has(PositionFlags::IsInt32Promise) || describesBagOfOptions(types, position.written()))
            continue;
        TypeID written = withoutOmittedNullishForTestsOfGuards(types, types.testablePartOf(position.written()), position);
        entered = entered ? types.intersectionOf(entered, written) : written;
    }
    return entered;
}

// A member of the written type that some caller is seen to pass is an ordinary value of the parameter, and a test that turns it away would send ordinary calls into the slow version.
// No word, and `anything`, which stands where no caller in sight brings anything: nobody was seen, and what the program wrote decides.
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
    // What the written type turns away goes on in the slow version whichever of the two is tested for. Where the two ask the same of objects, no object tells them apart.
    seen = types.intersectionOf(seen, tested);
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
    // typeTestedByParameterGuard() has asked the layouts of `tested` for their shapes, so the casts may take primitives away and no layout.
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
