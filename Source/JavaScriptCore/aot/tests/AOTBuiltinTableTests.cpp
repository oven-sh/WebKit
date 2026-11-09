/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTRuntimeDescription.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "IdentifierInlines.h"
#include "JSLock.h"
#include "VM.h"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

namespace JSC { namespace AOT {

using Outcome = BuiltinMemberLookup::Outcome;

static const BuiltinTable& table() { return BuiltinTable::singleton(); }

template<typename ID>
static void forEachID(unsigned count, const Invocable<void(ID)> auto& functor)
{
    for (unsigned index = 0; index < count; ++index)
        functor(ID(index));
}

static void forEachBuiltin(const Invocable<void(BuiltinID, const Builtin&)> auto& functor)
{
    forEachID<BuiltinID>(table().builtinCount(), [&](BuiltinID id) {
        functor(id, table().at(id));
    });
}

static bool isFound(BuiltinMemberLookup lookup, BuiltinID member, BuiltinID holder)
{
    return lookup.outcome == Outcome::Found && lookup.member == member && lookup.holder == holder;
}

static void testIDTypes()
{
    AOT_TEST_CHECK(!BuiltinID() && !BuiltinID(0xffffu) && !BuiltinID(0x10000u) && !BuiltinID(UINT32_MAX));
    AOT_TEST_CHECK(BuiltinID(0u) && !BuiltinID(0u).index() && BuiltinID(0xfffeu).index() == BuiltinID::hashTableDeletedIndex);
    AOT_TEST_CHECK(!RuntimeFunctionID() && !RuntimeFunctionID(UINT32_MAX) && RuntimeFunctionID(0x20000u).index() == 0x20000u);
    AOT_TEST_CHECK(!table().contains(BuiltinID()) && !table().contains(BuiltinOverloadID()) && !table().contains(BuiltinClassID()) && !table().contains(BuiltinSlotID()));
    AOT_TEST_CHECK(!table().contains(BuiltinLayoutID()) && !table().contains(BuiltinTypeID()) && !table().contains(BuiltinName()) && !table().contains(RuntimeModuleID()) && !table().contains(RuntimeFunctionID()));
    AOT_TEST_CHECK(table().contains(BuiltinID(table().builtinCount() - 1)) && !table().contains(BuiltinID(table().builtinCount())));

    HashSet<BuiltinID> set;
    AOT_TEST_CHECK(set.add(BuiltinID(0u)).isNewEntry && set.add(BuiltinID(7u)).isNewEntry && !set.add(BuiltinID(0u)).isNewEntry && set.remove(BuiltinID(0u)) && !set.contains(BuiltinID(0u)) && set.contains(BuiltinID(7u)));

    AOT_TEST_CHECK(!BuiltinOperand::receiver().coversArgument(0) && BuiltinOperand::receiver().isReceiver());
    AOT_TEST_CHECK(BuiltinOperand::argument(2).coversArgument(2) && !BuiltinOperand::argument(2).coversArgument(1) && !BuiltinOperand::argument(2).coversArgument(3));
    AOT_TEST_CHECK(BuiltinOperand::argumentsFrom(2).coversArgument(2) && BuiltinOperand::argumentsFrom(2).coversArgument(200) && !BuiltinOperand::argumentsFrom(2).coversArgument(1));
    AOT_TEST_CHECK(BuiltinOperand::argument(2) != BuiltinOperand::argumentsFrom(2));
}

static void testTableDescribesItself()
{
    auto bytesOf = [](const BuiltinTableDescription& description) {
        return std::span(reinterpret_cast<const uint8_t*>(&description), sizeof(description));
    };
    AOT_TEST_CHECK(!findRuntimeDifference(bytesOf(describedBuiltinTable)));

    BuiltinTableDescription other = describedBuiltinTable;
    other.hashOfTable ^= 1;
    std::optional<RuntimeDifference> difference = findRuntimeDifference(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the table of built-ins"_s);

    other = describedBuiltinTable;
    other.hashOfHostBindings ^= 1;
    difference = findRuntimeDifference(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the table of native functions"_s);

    other = describedBuiltinTable;
    other.size += 4;
    difference = findRuntimeDifference(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the size of the description of the table of built-ins"_s && difference->there == difference->here + 4);
}

static void testNamesOfKeys()
{
    RefPtr<VM> vm = VM::create();
    JSLockHolder locker(*vm);
    forEachID<BuiltinName>(table().nameCount(), [&](BuiltinName name) {
        Identifier key = table().identifierOf(*vm, name);
        AOT_TEST_CHECK(table().nameOf(*vm, key.impl()) == name);
        AOT_TEST_CHECK(table().textNameOf(key.impl()) == (table().isSymbolName(name) ? BuiltinName() : name));
    });

    for (ASCIILiteral text : { "no name of any built-in"_s, ""_s, "Push"_s, "pus"_s, "pushh"_s })
        AOT_TEST_CHECK(!table().nameOf(*vm, Identifier::fromString(*vm, String(text)).impl()));
    const char16_t wideText[] = { 'p', 'u', 's', 'h' };
    AOT_TEST_CHECK(table().nameOf(*vm, Identifier::fromString(*vm, String(std::span<const char16_t> { wideText })).impl()) == BuiltinNames::push);
    const char16_t otherWideText[] = { 'p', 'u', 's', 0x125 };
    AOT_TEST_CHECK(!table().nameOf(*vm, Identifier::fromString(*vm, String(std::span<const char16_t> { otherWideText })).impl()));

    AOT_TEST_CHECK(!table().nameOf(*vm, SymbolImpl::create(*String("iterator"_s).impl()).ptr()));
    AOT_TEST_CHECK(!table().nameOf(*vm, protect(vm->symbolRegistry())->symbolForKey("registered by the program"_s).ptr()));
    vm = nullptr;
}

static void testUnknownTokens()
{
    AOT_TEST_CHECK(!table().builtinFromToken("NoSuchBuiltin"_s) && !table().builtinFromToken(""_s) && !table().builtinFromToken("arrayPrototypePush"_s));
    AOT_TEST_CHECK(!table().classFromToken("NoSuchBuiltin"_s) && !table().classFromToken("ArrayPrototypePush"_s));
    AOT_TEST_CHECK(!table().slotFromToken("NoSuchBuiltin"_s) && !table().slotFromToken("Map"_s));
    AOT_TEST_CHECK(!table().layoutFromToken("NoSuchBuiltin"_s) && !table().moduleFromToken("NoSuchBuiltin"_s));
    for (ASCIILiteral token : { "ArrayPrototypePush"_s, "ArrayPrototypePush/"_s, "ArrayPrototypePush/999"_s, "ArrayPrototypePush/x"_s, "ArrayPrototypePush/-1"_s, "ArrayPrototypePush/0x"_s, "ArrayPrototypePush/99999999999999999999"_s, "NoSuchBuiltin/0"_s, "/0"_s, "/"_s, ""_s })
        AOT_TEST_CHECK(!table().overloadFromToken(token));
    AOT_TEST_CHECK(table().overloadFromToken("ArrayPrototypePush/0"_s) == table().at(Builtins::ArrayPrototypePush).firstOverload);
}

static void markLeaves(BuiltinDispatchTarget target, BuiltinID builtin, BuiltinCallMode mode, BitVector& isLeaf)
{
    if (target.isOverload()) {
        AOT_TEST_CHECK(!target.node && table().at(target.overload).builtin == builtin && table().at(target.overload).mode == mode);
        isLeaf.set(target.overload.index());
        return;
    }
    const BuiltinDispatchNode& node = table().at(target.node);
    AOT_TEST_CHECK(!node.branches.empty() && table().at(node.branches.back().tested).isAnything());
    for (const BuiltinDispatchBranch& branch : node.branches)
        markLeaves(branch.target, builtin, mode, isLeaf);
}

static void testDispatchIsTotal()
{
    AOT_TEST_CHECK(table().at(BuiltinTypeID(0u)).isAnything() && !table().at(BuiltinTypeID(0u)).isNever());
    AOT_TEST_CHECK(table().at(BuiltinTypeID(1u)).isNever() && !table().at(BuiltinTypeID(1u)).isAnything() && !table().at(BuiltinTypeID(1u)).mayBeObject());

    BitVector isLeaf(table().overloadCount());
    forEachBuiltin([&](BuiltinID id, const Builtin&) {
        for (BuiltinCallMode mode : { BuiltinCallMode::Call, BuiltinCallMode::Construct, BuiltinCallMode::Get, BuiltinCallMode::Set }) {
            if (std::optional<BuiltinDispatchTarget> root = table().dispatchOf(id, mode))
                markLeaves(*root, id, mode, isLeaf);
        }
    });
    AOT_TEST_CHECK(isLeaf.bitCount() == table().overloadCount());
}

static void testClassesAndSlots()
{
    forEachID<BuiltinClassID>(table().classCount(), [&](BuiltinClassID id) {
        const BuiltinClass& builtinClass = table().at(id);
        AOT_TEST_CHECK(builtinClass.id == id && builtinClass.parent != id && table().isSameOrSubclassOf(id, id));
        AOT_TEST_CHECK(!id.index() || codePointCompareLessThan(table().tokenOf(BuiltinClassID(id.index() - 1)), table().tokenOf(id)));
        AOT_TEST_CHECK(!builtinClass.prototype || table().at(builtinClass.prototype).instanceClass == id);
        unsigned numberOfSlotsInType = 0;
        for (BuiltinSlotID slot : builtinClass.slots) {
            AOT_TEST_CHECK(table().isSameOrSubclassOf(id, table().at(slot).owner) && table().findSlot(id, table().at(slot).name) == slot);
            if (!table().at(slot).contentsSlot) {
                AOT_TEST_CHECK(!table().at(slot).indexInType);
                continue;
            }
            AOT_TEST_CHECK(table().at(slot).indexInType == numberOfSlotsInType);
            AOT_TEST_CHECK(table().findSlot(id, *table().at(slot).contentsSlot) == slot);
            ++numberOfSlotsInType;
        }
        AOT_TEST_CHECK(table().numberOfSlotsInType(id) == numberOfSlotsInType);
    });
    forEachID<BuiltinSlotID>(table().slotCount(), [&](BuiltinSlotID id) {
        AOT_TEST_CHECK(table().at(id).id == id && std::ranges::contains(table().at(table().at(id).owner).slots, id));
        AOT_TEST_CHECK(!id.index() || table().at(BuiltinSlotID(id.index() - 1)).owner <= table().at(id).owner);
    });
    forEachID<BuiltinLayoutID>(table().layoutCount(), [&](BuiltinLayoutID id) {
        AOT_TEST_CHECK(table().at(id).id == id && !table().at(id).fields.empty());
        AOT_TEST_CHECK(!id.index() || codePointCompareLessThan(table().tokenOf(BuiltinLayoutID(id.index() - 1)), table().tokenOf(id)));
    });

    AOT_TEST_CHECK(table().findSlot(BuiltinClasses::Map, ContentsSlot::Key) == BuiltinSlots::MapKey && table().findSlot(BuiltinClasses::Map, ContentsSlot::Value) == BuiltinSlots::MapValue);
    AOT_TEST_CHECK(!table().findSlot(BuiltinClasses::Map, ContentsSlot::Element) && table().numberOfSlotsInType(BuiltinClasses::Map) == 2);
    AOT_TEST_CHECK(table().findSlot(BuiltinClasses::Array, ContentsSlot::Element) == BuiltinSlots::ArrayElement && table().numberOfSlotsInType(BuiltinClasses::Array) == 1);
    AOT_TEST_CHECK(table().findSlot(BuiltinClasses::Int8Array, ContentsSlot::Element) == BuiltinSlots::TypedArrayElement);
}

static BuiltinMemberLookup findMemberByTheRule(BuiltinID holder, BuiltinName name)
{
    for (BuiltinID object = table().canonicalID(holder);; object = table().canonicalID(table().at(object).inheritsFrom)) {
        if (BuiltinID member = table().findOwnMember(object, name))
            return { Outcome::Found, member, object };
        if (!table().at(object).flags.contains(BuiltinFlag::ListsAllMembers) || table().at(object).prototypeLink == BuiltinPrototypeLink::Unknown)
            return { };
        if (table().at(object).prototypeLink == BuiltinPrototypeLink::Null)
            return { Outcome::Absent, { }, { } };
    }
}

static void testMemberLookup()
{
    AOT_TEST_CHECK(isFound(table().findMember(Builtins::ArrayPrototype, BuiltinNames::push), Builtins::ArrayPrototypePush, Builtins::ArrayPrototype));
    AOT_TEST_CHECK(isFound(table().findMember(Builtins::ArrayPrototype, BuiltinNames::hasOwnProperty), Builtins::ObjectPrototypeHasOwnProperty, Builtins::ObjectPrototype));
    AOT_TEST_CHECK(table().findMember(Builtins::ObjectPrototype, BuiltinNames::push).outcome == Outcome::Absent);
    AOT_TEST_CHECK(table().findMember(Builtins::ArrayPrototype, BuiltinNames::then).outcome == Outcome::Absent);
    AOT_TEST_CHECK(table().findMember(Builtins::GlobalThis, BuiltinNames::push).outcome == Outcome::Unknown);
    AOT_TEST_CHECK(BuiltinMemberLookup { }.outcome == Outcome::Unknown);

    AOT_TEST_CHECK(isFound(table().findForInstance(BuiltinClasses::Array, BuiltinNames::push), Builtins::ArrayPrototypePush, Builtins::ArrayPrototype));
    AOT_TEST_CHECK(isFound(table().findForInstance(BuiltinClasses::Array, BuiltinNames::hasOwnProperty), Builtins::ObjectPrototypeHasOwnProperty, Builtins::ObjectPrototype));
    AOT_TEST_CHECK(table().findForInstance(BuiltinClasses::Array, BuiltinNames::then).outcome == Outcome::Absent);
    AOT_TEST_CHECK(isFound(table().findForInstance(BuiltinClasses::Promise, BuiltinNames::then), table().findOwnMember(Builtins::PromisePrototype, BuiltinNames::then), Builtins::PromisePrototype));

    AOT_TEST_CHECK(std::ranges::contains(table().classesWithInstanceMember(BuiltinNames::push), BuiltinClasses::Array));
    AOT_TEST_CHECK(!std::ranges::contains(table().classesWithInstanceMember(BuiltinNames::push), BuiltinClasses::Map));
    AOT_TEST_CHECK(table().classesWithInstanceMember(BuiltinNames::globalThis).empty());

    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        if (!isObjectKind(builtin.kind))
            return;
        AOT_TEST_CHECK((builtin.prototypeLink == BuiltinPrototypeLink::Builtin) == !!builtin.inheritsFrom);
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::push, BuiltinNames::hasOwnProperty, BuiltinNames::then, BuiltinNames::constructor, BuiltinNames::toString, table().nameOf(WellKnownSymbol::Iterator) }) {
            BuiltinMemberLookup expected = findMemberByTheRule(id, name);
            BuiltinMemberLookup found = table().findMember(id, name);
            AOT_TEST_CHECK(found.outcome == expected.outcome && found.member == expected.member && found.holder == expected.holder);
        }
    });
}

static void testMutabilityFollowsTheLock()
{
    HashSet<BuiltinID> lockedObjects;
    for (BuiltinID id : table().lockedObjects())
        AOT_TEST_CHECK(lockedObjects.add(id).isNewEntry);
    HashSet<BuiltinID> lockedMembers;
    for (BuiltinID id : table().lockedMembers())
        AOT_TEST_CHECK(lockedMembers.add(id).isNewEntry);

    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(builtin.flags.contains(BuiltinFlag::IsLockedObject) == lockedObjects.contains(id));
        AOT_TEST_CHECK(table().isLocationOfLockedObject(id) == (builtin.holder && lockedObjects.contains(builtin.holder)));
        bool hasLockOfItsOwn = builtin.flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::HasLockedMembers });
        AOT_TEST_CHECK(table().hasLockedPrototypeLink(id) == (builtin.prototypeLink != BuiltinPrototypeLink::Unknown && (hasLockOfItsOwn || table().isFunctionOfEngine(id) || table().isNativeFunctionOfHost(id) || table().closesOnlyNamesOfCalls(id))));
        if (table().isNativeFunctionOfHost(id)) {
            AOT_TEST_CHECK(!table().isFunctionOfEngine(id) && builtin.origin == BuiltinOrigin::Host);
            for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::call, BuiltinNames::apply, BuiltinNames::bind, table().nameOf(WellKnownSymbol::HasInstance) })
                AOT_TEST_CHECK(table().isClosedKey(id, name));
            AOT_TEST_CHECK(!table().isClosedKey(id, BuiltinNames::toString) && !table().isClosedKey(id, BuiltinNames::then));
        }
        if (table().ignoresReceiver(id)) {
            AOT_TEST_CHECK(!table().overloadsOf(table().canonicalID(id)).empty());
            for (const BuiltinOverload& row : table().overloadsOf(table().canonicalID(id)))
                AOT_TEST_CHECK(!row.isUndeclared() && !row.receiver);
            for (const BuiltinOverloadSummary& overload : table().overloadSummariesOf(table().canonicalID(id)))
                AOT_TEST_CHECK(overload.callee == BuiltinCallee::HostFunction || overload.callee == BuiltinCallee::InternalFunction);
        }
        AOT_TEST_CHECK(table().closesIndexKeys(id) == (table().hasLockedPrototypeLink(id) && (builtin.kind == BuiltinKind::Prototype || builtin.flags.contains(BuiltinFlag::IsLockedObject))));
        for (BuiltinName textOfPrimitive : { BuiltinNames::undefined, BuiltinNames::null, BuiltinNames::trueKeyword, BuiltinNames::falseKeyword }) {
            AOT_TEST_CHECK(!table().closesIndexKeys(id) || builtin.flags.contains(BuiltinFlag::IsLockedObject) || table().isClosedKey(table().canonicalID(id), textOfPrimitive));
            AOT_TEST_CHECK(!table().closesIndexKeys(id) || !table().canTakeNameInTestRuns(table().canonicalID(id), textOfPrimitive));
        }
        bool isLockedLocation = !builtin.holder || table().isLocationOfLockedObject(id) || lockedMembers.contains(id);
        if (isLockedLocation && !builtin.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
            AOT_TEST_CHECK(builtin.mutability == BuiltinMutability::AlwaysLocked);
        if (builtin.flags.contains(BuiltinFlag::HasLockedMembers)) {
            AOT_TEST_CHECK(std::ranges::any_of(table().membersOf(id), [&](BuiltinID member) {
                return lockedMembers.contains(member);
            }));
        }
        if (!builtin.flags.contains(BuiltinFlag::HasLockedMembers))
            AOT_TEST_CHECK(!builtin.flags.contains(BuiltinFlag::IsClosedToOtherNames) && table().openNamesOf(id).empty());
        for (BuiltinName name : table().openNamesOf(id))
            AOT_TEST_CHECK(!std::ranges::binary_search(table().closedKeysOf(id), name));
        if (!table().hasLockedPrototypeLink(id))
            AOT_TEST_CHECK(table().closedKeysOf(id).empty());
    });
}

static void testNoReturnCanLieAboveIteratorsOfEngine()
{
    for (BuiltinID next : { Builtins::ArrayIteratorPrototypeNext, Builtins::MapIteratorPrototypeNext, Builtins::SetIteratorPrototypeNext, Builtins::StringIteratorPrototypeNext }) {
        BuiltinID last;
        for (BuiltinID holder = table().canonicalID(table().at(next).holder); holder; holder = table().at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? table().canonicalID(table().at(holder).inheritsFrom) : BuiltinID()) {
            AOT_TEST_CHECK(!table().findOwnMember(holder, BuiltinNames::returnKeyword));
            AOT_TEST_CHECK(table().at(holder).flags.contains(BuiltinFlag::IsLockedObject) || table().isClosedKey(holder, BuiltinNames::returnKeyword));
            AOT_TEST_CHECK(table().at(holder).flags.contains(BuiltinFlag::ListsAllMembers));
            AOT_TEST_CHECK(table().hasLockedPrototypeLink(holder));
            AOT_TEST_CHECK(!table().canTakeNameInTestRuns(holder, BuiltinNames::returnKeyword));
            last = holder;
        }
        AOT_TEST_CHECK(last == table().canonicalID(Builtins::ObjectPrototype) && table().at(last).prototypeLink == BuiltinPrototypeLink::Null);
    }
}

static std::unique_ptr<Typed::Program> parseProgramForBuiltinTableTests(ASCIILiteral text, BuildContext& build)
{
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(text, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);
    return program;
}

static void testNoObjectIsProvenToBePlainData()
{
    using namespace Typed;
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseProgramForBuiltinTableTests(R"IR(module m0 path="main.js" displayPath="main.js"
layout L0 name="Literal"
)IR"_s, build);
    LayoutID literal(0u);
    AOT_TEST_CHECK(program->at(literal).kind == LayoutKind::Record);
    TypeArena arena(program->types, ArenaKeeps::InProgram);
    BuiltinTypeContext context { *program, arena };

    BuiltinTypeID parsed = table().overloadsOf(Builtins::JSONParse)[0].result.type;
    AOT_TEST_CHECK(table().at(parsed).flags.contains(BuiltinTypeFlag::HasPlainData) && !table().at(parsed).flags.contains(BuiltinTypeFlag::HasAnyObject));
    auto fitIn = [&](TypeID operand) {
        return fitOf(context, operand, parsed).fit;
    };
    AOT_TEST_CHECK(fitIn(arena.forLayout(literal)) == TypeFit::Unknown);
    AOT_TEST_CHECK(fitIn(arena.builtinClass(BuiltinClasses::Array)) == TypeFit::Unknown && fitIn(arena.builtinClass(BuiltinClasses::Object)) == TypeFit::Unknown);
    AOT_TEST_CHECK(fitIn(TypeTable::object) == TypeFit::Unknown && fitIn(arena.builtinClass(BuiltinClasses::Map)) == TypeFit::Outside);
    AOT_TEST_CHECK(fitIn(TypeTable::string) == TypeFit::Inside && fitIn(TypeTable::symbol) == TypeFit::Outside);
}

static void testTypeNarrowedToClassWrittenBelowParameterLiesInIt()
{
    using namespace Typed;
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseProgramForBuiltinTableTests(R"IR(module m0 path="main.js" displayPath="main.js"
layout L0 kind=Instance name="Below"
layout L1 kind=Instance name="BelowConstructorFunction"
layout L2 kind=Instance name="BelowOtherClass"
layout L3 kind=Instance name="BelowNothing"
)IR"_s, build);
    auto layoutWrittenBelow = [&](uint32_t index, BuiltinClassID writtenAbove, bool mayBeOrdinaryObject) {
        LayoutID id(index);
        Layout& layout = program->at(id);
        AOT_TEST_CHECK(layout.kind == LayoutKind::Instance);
        layout.writtenBuiltinBase = writtenAbove;
        layout.mayBeOrdinaryObject = mayBeOrdinaryObject;
        return id;
    };
    LayoutID below = layoutWrittenBelow(0, BuiltinClasses::Error, false);
    LayoutID belowConstructorFunction = layoutWrittenBelow(1, BuiltinClasses::Error, true);
    LayoutID belowOtherClass = layoutWrittenBelow(2, BuiltinClasses::Map, false);
    LayoutID belowNothing = layoutWrittenBelow(3, { }, false);
    TypeArena arena(program->types, ArenaKeeps::InProgram);
    BuiltinTypeContext context { *program, arena };

    BuiltinTypeID receiver = table().parameterFor(table().overloadsOf(Builtins::ErrorPrototypeMessage)[0], BuiltinOperand::receiver())->accepted;
    TypeID instances = arena.builtinClass(BuiltinClasses::Error);
    auto narrowedTo = [&](LayoutID layout) {
        return arena.unionOf(arena.forLayout(layout), instances);
    };
    auto liesInParameter = [&](TypeID operand) {
        return fitsIfPristine(context, operand, receiver);
    };
    AOT_TEST_CHECK(liesInParameter(instances));
    AOT_TEST_CHECK(arena.isSubset(narrowedTo(below), instances) && liesInParameter(narrowedTo(below)) && liesInParameter(arena.forLayout(below)));
    for (LayoutID outside : { belowConstructorFunction, belowOtherClass, belowNothing })
        AOT_TEST_CHECK(!arena.isSubset(narrowedTo(outside), instances) && !liesInParameter(narrowedTo(outside)));
}

static void testTypesAreFoundByAddressWhereTheTableKeepsThem()
{
    using namespace Typed;
    constexpr uint32_t numberOfLayouts = 11;
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Program> program = parseProgramForBuiltinTableTests(R"IR(module m0 path="main.js" displayPath="main.js"
layout L0 name="Record0"
layout L1 name="Record1"
layout L2 name="Record2"
layout L3 name="Record3"
layout L4 name="Record4"
layout L5 name="Record5"
layout L6 name="Record6"
layout L7 name="Record7"
layout L8 name="Record8"
layout L9 name="Record9"
layout L10 name="Record10"
)IR"_s, build);
    TypeArena arena(program->types, ArenaKeeps::InProgram);
    Vector<std::pair<TypeID, const Type*>> kept;
    auto keepSetsOfLayouts = [&](uint32_t firstSet, uint32_t endOfSets) {
        for (uint32_t set = firstSet; set < endOfSets; ++set) {
            Vector<LayoutID, numberOfLayouts> layouts;
            for (uint32_t index = 0; index < numberOfLayouts; ++index) {
                if (set >> index & 1)
                    layouts.append(LayoutID(index));
            }
            TypeID type = arena.forLayouts(layouts.span());
            kept.append({ type, &program->types.typeKeptAt(type) });
        }
    };
    auto everyTypeLiesWhereItLay = [&] {
        return std::ranges::all_of(kept, [&](const std::pair<TypeID, const Type*>& one) {
            return &program->types.typeKeptAt(one.first) == one.second;
        });
    };
    keepSetsOfLayouts(1, 1200);
    AOT_TEST_CHECK(program->types.numberOfTypes() > firstPositionOfInternedSegment(2));
    program->types.keepAddressesOfTypes();
    AOT_TEST_CHECK(everyTypeLiesWhereItLay());
    keepSetsOfLayouts(1200, 1u << numberOfLayouts);
    AOT_TEST_CHECK(program->types.numberOfTypes() > firstPositionOfInternedSegment(3) && everyTypeLiesWhereItLay());
    program->types.keepAddressesOfTypes();
    AOT_TEST_CHECK(everyTypeLiesWhereItLay());
}

static void testMarkIsHadByWhomItsListNames()
{
    size_t numberListed = 0;
    for (unsigned number = 0; number <= static_cast<unsigned>(BuiltinMark::AsksOptionsForListedNamesAlone); ++number) {
        BuiltinMark mark = static_cast<BuiltinMark>(number);
        BitVector listed(table().builtinCount());
        for (const BuiltinMarkedEntry& entry : table().marked(mark))
            listed.quickSet(entry.builtin.index());
        numberListed += table().marked(mark).size();
        for (unsigned index = 0; index < table().builtinCount(); ++index)
            AOT_TEST_CHECK(table().has(mark, BuiltinID(index)) == listed.quickGet(index));
        AOT_TEST_CHECK(!table().has(mark, BuiltinID()) && !table().has(mark, BuiltinID(table().builtinCount())));
    }
    AOT_TEST_CHECK(numberListed);
}

std::span<const NativeTest> builtinTableTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-table/id-types"_s, testIDTypes },
        { "builtin-table/a-mark-is-had-by-whom-its-list-names"_s, testMarkIsHadByWhomItsListNames },
        { "type-table/types-are-found-by-address-where-the-table-keeps-them"_s, testTypesAreFoundByAddressWhereTheTableKeepsThem },
        { "builtin-table/describes-itself"_s, testTableDescribesItself },
        { "builtin-table/names-of-keys"_s, testNamesOfKeys },
        { "builtin-table/unknown-tokens"_s, testUnknownTokens },
        { "builtin-table/dispatch-is-total"_s, testDispatchIsTotal },
        { "builtin-table/classes-and-slots"_s, testClassesAndSlots },
        { "builtin-table/member-lookup"_s, testMemberLookup },
        { "builtin-table/mutability-follows-the-lock"_s, testMutabilityFollowsTheLock },
        { "builtin-table/no-return-can-lie-above-iterators-of-engine"_s, testNoReturnCanLieAboveIteratorsOfEngine },
        { "builtin-table/no-object-is-proven-to-be-plain-data"_s, testNoObjectIsProvenToBePlainData },
        { "builtin-table/type-narrowed-to-class-written-below-parameter-lies-in-it"_s, testTypeNarrowedToClassWrittenBelowParameterLiesInIt },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
