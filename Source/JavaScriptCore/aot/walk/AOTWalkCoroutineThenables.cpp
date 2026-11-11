/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutinePromises.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTPrimitives.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesThatAreNoValuesForThenables = Primitive::Absent | Primitive::Unspecified;

static bool lookupFindsNothingOnObjectsOfForThenables(const Program& program, const LinkTables& link, TypeArena& types, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (id.index() >= link.layouts.size() || !isFoundByShapeTest(layout) || builtinClassWrittenAbove(types, id))
        return false;
    const LinkedLayout& linked = link.layout(id);
    if (linked.reachesUnseenCode || !linked.listsEveryMemberAbove || possibleChangesForbid(linked.possibleChanges, TypedAccess::FindsNoProperty))
        return false;

    if (!link.findsNothing(program, id, ClassSide::Prototype, key, Of::ObjectItself))
        return false;
    if (isOnObjectPrototype(program.nameClassOf(key)) || layout.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (layout.kind == LayoutKind::Record)
        return !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    return link.findsNothing(program, id, ClassSide::Prototype, key, Of::HolderAndAbove).has_value();
}

static bool isProvenToHaveNoThen(const Program& program, const Facts& facts, TypeArena& types, TypeID id)
{
    if (!id || isNeverType(id))
        return false;
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || (type.primitives & primitivesThatAreNoValuesForThenables))
        return false;
    if (!type.hasObjects())
        return true;
    if (!facts.mechanisms().contains(Mechanism::TypedOperations))
        return false;

    Type objects = types.at(types.withOnlyObjects(id));
    if (!objects.hasOnlyLayouts() || objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return false;
    std::span<const BuiltinName> names = program.builtins().keysReadBy(Protocol::Thenable);
    if (names.empty())
        return false;

    const LinkTables& link = facts.linked();
    return std::ranges::all_of(membersOf(types, objects), [&](LayoutID layout) {
        if (layout.index() < link.layouts.size() && !link.layout(layout).isAllocated)
            return true;
        return std::ranges::all_of(names, [&](BuiltinName name) {
            PropertyKeyID key = program.keyOf(name);
            return key && lookupFindsNothingOnObjectsOfForThenables(program, link, types, layout, key);
        });
    });
}

static bool nothingEverLiesUnderKeyFromForThenables(const Program& program, BuiltinID start, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    if (!start || !key)
        return false;
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (members.findOwnMember(holder) || !listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder) || !table.hasLockedPrototypeLink(holder))
            return false;
        switch (table.at(holder).prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(table.at(holder).inheritsFrom);
            break;
        }
    }
}

static bool isFreshListWithoutThenForThenables(Walker& walker, Reference value)
{
    if (!walker.has(Mechanism::TypedOperations) || !isFreshPristineArray(walker, value))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    std::span<const BuiltinName> names = table.keysReadBy(Protocol::Thenable);
    return !names.empty() && table.mayHavePristineInstances(BuiltinClasses::Array) && std::ranges::all_of(names, [&](BuiltinName name) {
        return nothingEverLiesUnderKeyFromForThenables(program, table.at(BuiltinClasses::Array).prototype, program.keyOf(name));
    });
}

bool hasNoThen(const Walker& walker, TypeID type)
{
    return isProvenToHaveNoThen(walker.program(), walker.facts(), walker.types(), type);
}

bool isProvenToHaveNoThen(Walker& walker, Reference value)
{
    if (!value)
        return false;
    TypeID type = walker.typeOf(value);
    if (!walker.has(Mechanism::TypedOperations) && type && walker.types().at(type).hasObjects())
        return false;
    return isProvenToHaveNoThen(walker.program(), walker.facts(), walker.types(), type) || isFreshListWithoutThenForThenables(walker, value);
}

bool leansOnProofOfNoThen(Walker& walker, Reference value, Origin origin)
{
    if (!isProvenToHaveNoThen(walker, value))
        return false;
    TypeID type = walker.typeOf(value);
    if (!walker.checksProofsOf(ProofSource::RememberedFact) || !value.isRecord() || !type || !walker.types().at(type).hasObjects() || walker.isUnreachable())
        return true;
    origin.isTailCall = false;
    for (BuiltinName name : walker.program().builtins().keysReadBy(Protocol::Thenable)) {
        Draft test(Opcode::FindsNoMember, origin);
        walker.chooseConservativeFields(test);
        test.setObjectOperand(value);
        test.setKey(walker.program().keyOf(name));
        test.setAsksRunTime(true);
        test.setAsksObjectByInquiry(true);
        walker.verifyRememberedTest(WTF::move(test));
    }
    return true;
}

Reference buildTestForNoThen(Walker& walker, Reference value, Origin origin)
{
    auto answer = [&](bool hasNoThen) {
        Draft constant(Opcode::Boolean);
        constant.setBoolean(hasNoThen);
        return walker.constant(WTF::move(constant));
    };
    if (leansOnProofOfNoThen(walker, value, origin))
        return answer(true);

    TypeArena& types = walker.types();
    TypeID type = walker.typeOf(value);
    if (type && types.at(type).isObject() && types.isSubset(type, types.builtinClass(BuiltinClasses::Promise)))
        return answer(false);

    origin.isTailCall = false;
    Draft test(Opcode::IsNotThenable, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    return walker.build(WTF::move(test));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
