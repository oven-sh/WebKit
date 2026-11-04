/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, values without a `then`: whoever awaits one or resolves a promise with one looks nothing up, runs no code of the program and goes on with the value itself.
// A yes by the type holds in every run and at every place, so neither a suspension nor a throw ends it. A yes for a fresh list holds where the walk stands, and a passed IsNotThenable only in the arm behind it: nothing is kept.
// Withheld: primitives alone have no `then`.

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

// Above a record stands Object.prototype alone, which takes no new name. Above an instance stand the members of its row of classes, which Link lists, and then Object.prototype.
// What Object.create() makes has whatever it was handed above it, and an instance of a class below one of the engine's is a cell of that class.
static bool lookupFindsNothingOnObjectsOfForThenables(const Program& program, const LinkTables& link, TypeArena& types, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (id.index() >= link.layouts.size() || !isFoundByShapeTest(layout) || builtinClassWrittenAbove(types, id))
        return false;
    const LinkedLayout& linked = link.layout(id);
    if (linked.reachesUnseenCode || !linked.listsEveryMemberAbove || possibleChangesForbid(linked.possibleChanges, TypedAccess::FindsNoProperty))
        return false;

    std::span<const PropertyKeyID> appendedKeys = link.appendedKeysOf(id);
    if (findField(program, id, key) || std::ranges::find(appendedKeys, key) != appendedKeys.end())
        return false;
    if (isOnObjectPrototype(program.nameClassOf(key)) || layout.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (layout.kind == LayoutKind::Record)
        return !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    return link.findMember(program, id, ClassSide::Prototype, key).kind == MemberFound::Kind::Nothing;
}

bool isProvenToHaveNoThen(const Program& program, const Facts& facts, TypeArena& types, TypeID id)
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
        // No object of it is ever made.
        if (layout.index() < link.layouts.size() && !link.layout(layout).isAllocated)
            return true;
        return std::ranges::all_of(names, [&](BuiltinName name) {
            PropertyKeyID key = program.keyOf(name);
            return key && lookupFindsNothingOnObjectsOfForThenables(program, link, types, layout, key);
        });
    });
}

// From `start` upward nothing lies under the key, and nothing ever will: each object lists all that it has, takes no such name, and keeps what stands above it.
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

// Nothing but this code has the list, and it has the names and the prototype that a list is made with.
static bool isFreshListWithoutThenForThenables(Walker& walker, Reference value)
{
    if (!walker.has(Mechanism::TypedOperations) || !isFreshListAsMade(walker, value))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    std::span<const BuiltinName> names = table.keysReadBy(Protocol::Thenable);
    return !names.empty() && table.mayHavePristineInstances(BuiltinClasses::Array) && std::ranges::all_of(names, [&](BuiltinName name) {
        return nothingEverLiesUnderKeyFromForThenables(program, table.at(BuiltinClasses::Array).prototype, program.keyOf(name));
    });
}

bool isProvenToHaveNoThen(Walker& walker, Reference value)
{
    if (!value)
        return false;
    // A SlowBody leans on nothing that Link has proven.
    TypeID type = walker.typeOf(value);
    if (!walker.has(Mechanism::TypedOperations) && type && walker.types().at(type).hasObjects())
        return false;
    return isProvenToHaveNoThen(walker.program(), walker.facts(), walker.types(), type) || isFreshListWithoutThenForThenables(walker, value);
}

Reference buildTestForNoThen(Walker& walker, Reference value, Origin origin)
{
    auto answer = [&](bool hasNoThen) {
        Draft constant(Opcode::Boolean);
        constant.setBoolean(hasNoThen);
        return walker.constant(WTF::move(constant));
    };
    if (isProvenToHaveNoThen(walker, value))
        return answer(true);

    // The test passes only for what is surely no thenable, so a no claims nothing. It never passes for a promise.
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
