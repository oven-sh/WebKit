/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTCompileOptionWords.h"
#include "AOTHostEntriesTestSupport.h"
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
#include <wtf/text/MakeString.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

namespace JSC { namespace AOT {

using Outcome = BuiltinMemberLookup::Outcome;

static const BuiltinTable& table() { return BuiltinTable::singleton(); }

static BuiltinName nameOf(ASCIILiteral text)
{
    BuiltinName name = table().nameOf(byteCast<uint8_t>(text.span()));
    AOT_TEST_CHECK(name);
    return name;
}

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
    AOT_TEST_CHECK(!findDifferenceFromThisRuntime(bytesOf(describedBuiltinTable)));

    BuiltinTableDescription other = describedBuiltinTable;
    other.hashOfTable ^= 1;
    std::optional<RuntimeDifference> difference = findDifferenceFromThisRuntime(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the table of built-ins"_s);

    other = describedBuiltinTable;
    other.hashOfHostBindings ^= 1;
    difference = findDifferenceFromThisRuntime(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the table of native functions"_s);

    other = describedBuiltinTable;
    other.size += 4;
    difference = findDifferenceFromThisRuntime(bytesOf(other));
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

static void testTokensRoundTrip()
{
    HashSet<String> tokens;
    uint32_t hash = 0x811c9dc5;
    forEachBuiltin([&](BuiltinID id, const Builtin&) {
        AOT_TEST_CHECK(!table().tokenOf(id).isEmpty() && tokens.add(table().tokenOf(id)).isNewEntry);
        for (char16_t character : StringView(table().tokenOf(id)).codeUnits())
            hash = (hash ^ character) * 0x01000193;
        hash *= 0x01000193;
        AOT_TEST_CHECK(table().builtinFromToken(table().tokenOf(id)) == id);
        unsigned indexInEntry = 0;
        for (BuiltinOverloadID overload : table().overloadIDsOf(id)) {
            AOT_TEST_CHECK(table().tokenOf(overload) == makeString(table().tokenOf(id), '/', indexInEntry++));
            AOT_TEST_CHECK(table().overloadFromToken(table().tokenOf(overload)) == overload);
        }
    });
    AOT_TEST_CHECK(hash == describedBuiltinTable.hashOfBuiltinTokens);
    forEachID<BuiltinClassID>(table().classCount(), [&](BuiltinClassID id) {
        AOT_TEST_CHECK(table().classFromToken(table().tokenOf(id)) == id);
    });
    forEachID<BuiltinSlotID>(table().slotCount(), [&](BuiltinSlotID id) {
        AOT_TEST_CHECK(table().slotFromToken(table().tokenOf(id)) == id);
        AOT_TEST_CHECK(table().tokenOf(id).startsWith(table().tokenOf(table().at(id).owner)));
    });
    forEachID<BuiltinLayoutID>(table().layoutCount(), [&](BuiltinLayoutID id) {
        AOT_TEST_CHECK(table().layoutFromToken(table().tokenOf(id)) == id);
    });
    forEachID<RuntimeModuleID>(table().moduleCount(), [&](RuntimeModuleID id) {
        AOT_TEST_CHECK(table().moduleFromToken(table().tokenOf(id)) == id);
    });

    AOT_TEST_CHECK(table().tokenOf(Builtins::ArrayPrototypePush) == "ArrayPrototypePush"_s && table().tokenOf(Builtins::MapPrototypeSymbolIterator) == "MapPrototypeSymbolIterator"_s);
    AOT_TEST_CHECK(table().tokenOf(hostBuiltin("NodeFsReadFile"_s)) == "NodeFsReadFile"_s && table().tokenOf(Builtins::GlobalThis) == "GlobalThis"_s);
    AOT_TEST_CHECK(table().tokenOf(BuiltinClasses::Map) == "Map"_s && table().tokenOf(BuiltinSlots::MapKey) == "MapKey"_s && table().tokenOf(BuiltinLayouts::IteratorResult) == "IteratorResult"_s);
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

static void testClassRelations()
{
    AOT_TEST_CHECK(table().isSameOrSubclassOf(BuiltinClasses::Int8Array, BuiltinClasses::TypedArray) && !table().isSameOrSubclassOf(BuiltinClasses::TypedArray, BuiltinClasses::Int8Array));
    AOT_TEST_CHECK(table().isSameOrSubclassOf(BuiltinClasses::Buffer, BuiltinClasses::Uint8Array) && table().isSameOrSubclassOf(BuiltinClasses::Buffer, BuiltinClasses::TypedArray));
    AOT_TEST_CHECK(!table().isSameOrSubclassOf(BuiltinClasses::Uint8Array, BuiltinClasses::Buffer) && !table().isSameOrSubclassOf(BuiltinClasses::Map, BuiltinClasses::Set));
    AOT_TEST_CHECK(!table().isSameOrSubclassOf(BuiltinClasses::Map, BuiltinClassID()));

    AOT_TEST_CHECK(table().typedArrayClassOf(BuiltinClasses::Int8Array) == BuiltinClasses::Int8Array && table().typedArrayTypeOf(BuiltinClasses::Int8Array) == TypeInt8);
    AOT_TEST_CHECK(table().typedArrayClassOf(BuiltinClasses::Buffer) == BuiltinClasses::Uint8Array && table().typedArrayTypeOf(BuiltinClasses::Buffer) == TypeUint8);
    AOT_TEST_CHECK(table().typedArrayTypeOf(BuiltinClasses::DataView) == NotTypedArray && table().typedArrayTypeOf(BuiltinClasses::TypedArray) == NotTypedArray && !table().typedArrayClassOf(BuiltinClasses::Array));
    AOT_TEST_CHECK(!table().typedArrayClassOf(BuiltinClasses::TypedArray) && !table().typedArrayClassOf(BuiltinClasses::Map) && !table().typedArrayClassOf(BuiltinClasses::Array));

    AOT_TEST_CHECK(table().at(BuiltinClasses::Array).constructor == Builtins::Array && table().at(BuiltinClasses::Array).prototype == Builtins::ArrayPrototype);
    AOT_TEST_CHECK(table().receiverClassOf(Builtins::ArrayPrototypePush) == BuiltinClasses::Array && table().receiverClassOf(Builtins::MapPrototypeEntries) == BuiltinClasses::Map);
    AOT_TEST_CHECK(!table().receiverClassOf(Builtins::ArrayOf) && !table().receiverClassOf(Builtins::MathMax) && !table().receiverClassOf(Builtins::GlobalThis));

    AOT_TEST_CHECK(table().at(BuiltinClasses::Array).origin == BuiltinOrigin::Engine && table().at(hostClass("Request"_s)).origin == BuiltinOrigin::Host);
    AOT_TEST_CHECK(table().at(hostClass("Request"_s)).instanceTest == BuiltinInstanceTest::ClassInfoInherits);
    AOT_TEST_CHECK(table().at(BuiltinClasses::Map).instanceTest == BuiltinInstanceTest::JSTypeEquals);
    AOT_TEST_CHECK(table().at(BuiltinClasses::ArrayBuffer).instanceTest == BuiltinInstanceTest::SharingModeEquals && table().at(BuiltinClasses::ArrayBuffer).sharingMode == ArrayBufferSharingMode::Default);
    AOT_TEST_CHECK(table().at(BuiltinClasses::SharedArrayBuffer).instanceTest == BuiltinInstanceTest::SharingModeEquals && table().at(BuiltinClasses::SharedArrayBuffer).sharingMode == ArrayBufferSharingMode::Shared);
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

static void testMembersAskedByByteAlone()
{
    AOT_TEST_CHECK(table().at(hostBuiltin("ObjectPrototype__proto__"_s)).mutability == BuiltinMutability::AlwaysLocked && !isMemberAskedByByteAlone(table(), hostBuiltin("ObjectPrototype__proto__"_s)));
    AOT_TEST_CHECK(isMemberAskedByByteAlone(table(), hostBuiltin("AbortSignalPrototypeAborted"_s)));
    AOT_TEST_CHECK(isMemberAskedByByteAlone(table(), hostBuiltin("AbortControllerPrototypeSignal"_s)));
    AOT_TEST_CHECK(!isMemberAskedByByteAlone(table(), Builtins::ArrayPrototypePush));
    AOT_TEST_CHECK(!isMemberAskedByByteAlone(table(), Builtins::ObjectPrototype));
    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(builtin.kind != BuiltinKind::Accessor || !builtin.flags.contains(BuiltinFlag::IsCompilerHelper));
        if (!isMemberAskedByByteAlone(table(), id))
            return;
        AOT_TEST_CHECK(builtin.kind == BuiltinKind::Accessor && builtin.holder && builtin.name);
        AOT_TEST_CHECK(cellThatNamesMemberAskedByItsByte(table(), id) == BuiltinCellKey::getter(id) && !BuiltinCellKey::getter(id).hasCell(table()));
    });
}

static void testMemberMissingOnTargetIsNotFound()
{
    BuiltinID member = hostBuiltin("ProcessCwd"_s);
    BuiltinID holder = table().at(member).holder;
    BuiltinName name = table().at(member).name;
    AOT_TEST_CHECK(table().findOwnMember(holder, name) == member);
    AOT_TEST_CHECK(table().at(holder).flags.contains(BuiltinFlag::ListsAllMembers));

    Vector<Builtin> rows(builtinTableData.builtins);
    rows[member.index()].platforms.remove(targetOfThisBuild().platform);
    BuiltinTableData data = builtinTableData;
    data.builtins = rows.span();
    BuiltinTable tableWithoutMember(data);
    AOT_TEST_CHECK(!tableWithoutMember.existsOn(member, targetOfThisBuild().platform));
    AOT_TEST_CHECK(!tableWithoutMember.findOwnMember(holder, name));
    AOT_TEST_CHECK(tableWithoutMember.findMember(holder, name).outcome != Outcome::Found);
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

static void testLockedObjects()
{
    AOT_TEST_CHECK(table().at(Builtins::ObjectPrototype).flags.contains(BuiltinFlag::IsLockedObject));
    for (BuiltinID id : table().lockedObjects())
        AOT_TEST_CHECK(table().canonicalID(id) == table().canonicalID(Builtins::ObjectPrototype));
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::RegExpPrototype, Builtins::PromisePrototype, Builtins::GeneratorPrototype, Builtins::AsyncGeneratorPrototype, hostBuiltin("RequestPrototype"_s), hostBuiltin("ResponsePrototype"_s), hostBuiltin("BlobPrototype"_s), hostBuiltin("URLPrototype"_s), hostBuiltin("URLSearchParamsPrototype"_s), hostBuiltin("HeadersPrototype"_s) }) {
        AOT_TEST_CHECK(table().at(id).flags.contains(BuiltinFlag::HasLockedMembers) && !table().at(id).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }));
        AOT_TEST_CHECK(table().hasLockedPrototypeLink(id) && table().closesIndexKeys(id));
        for (BuiltinID member : table().membersOf(id))
            AOT_TEST_CHECK(table().at(member).mutability == BuiltinMutability::AlwaysLocked || table().at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::then, table().nameOf(WellKnownSymbol::ToPrimitive) })
            AOT_TEST_CHECK(table().findOwnMember(id, name) || table().isClosedKey(id, name));
    }
    AOT_TEST_CHECK(table().at(Builtins::ObjectPrototype).flags.contains(BuiltinFlag::ListsAllMembers) && table().at(Builtins::ArrayPrototype).flags.contains(BuiltinFlag::ListsAllMembers));
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::GlobalThis, Builtins::Math, Builtins::Array, Builtins::ArrayPrototype, Builtins::Object, Builtins::Map, Builtins::MapPrototype, Builtins::Promise, Builtins::Symbol, Builtins::IteratorPrototype, Builtins::ArrayIteratorPrototype, Builtins::Bun, hostBuiltin("Request"_s) })
        AOT_TEST_CHECK(!table().at(id).flags.contains(BuiltinFlag::IsLockedObject));

    AOT_TEST_CHECK(table().at(Builtins::ArrayPrototypePush).mutability == BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(table().at(Builtins::PromiseResolve).mutability == BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(table().at(Builtins::MathPI).mutability == BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(table().at(Builtins::Symbol).mutability == BuiltinMutability::AlwaysLocked && table().at(Builtins::SymbolIterator).mutability == BuiltinMutability::AlwaysLocked);
    for (BuiltinID id : { Builtins::Math, Builtins::MathMax, Builtins::MathImul })
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::AlwaysLocked);
    for (BuiltinID id : { Builtins::Object, Builtins::ObjectKeys, Builtins::ObjectGetOwnPropertyNames, Builtins::Array, Builtins::ArrayOf, Builtins::Number, Builtins::NumberIsInteger, Builtins::String, Builtins::StringFromCharCode, Builtins::Map, Builtins::JSON, Builtins::Reflect, Builtins::ReflectGet, Builtins::Atomics, Builtins::AtomicsAdd, Builtins::Uint8Array, Builtins::RegExp, Builtins::Proxy, Builtins::DateUTC })
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::AlwaysLocked);
    for (BuiltinID id : { Builtins::MathRandom, Builtins::MathSign, Builtins::MathSinh, Builtins::MathSumPrecise, Builtins::JSONParse, Builtins::JSONStringify, Builtins::NumberParseFloat, Builtins::NumberParseInt, Builtins::MapGroupBy, Builtins::PromiseTry, Builtins::RegExpEscape, Builtins::Date, Builtins::DateNow, Builtins::DateParse, Builtins::Promise, Builtins::WeakMap, Builtins::Intl, Builtins::Error, Builtins::ErrorCaptureStackTrace, Builtins::ErrorIsError, Builtins::Function })
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::ConstantUnlessWritten);
}

static void testLockedMembers()
{
    for (BuiltinID id : { Builtins::StringPrototype, Builtins::Symbol, Builtins::SymbolPrototype, Builtins::MapPrototype, Builtins::SetPrototype, Builtins::ArrayPrototype, Builtins::Promise, Builtins::IteratorPrototype, Builtins::ArrayIteratorPrototype, Builtins::Math }) {
        AOT_TEST_CHECK(table().at(id).flags.contains(BuiltinFlag::HasLockedMembers) && !table().at(id).flags.contains(BuiltinFlag::IsClosedToOtherNames));
        AOT_TEST_CHECK(table().hasLockedPrototypeLink(id) && table().closesIndexKeys(id) == (table().at(id).kind == BuiltinKind::Prototype));
    }
    AOT_TEST_CHECK(table().at(Builtins::MapPrototypeEntries).mutability == BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(std::ranges::contains(table().lockedMembers(), Builtins::MapPrototypeEntries));
    AOT_TEST_CHECK(std::ranges::is_sorted(table().lockedMembers()));

    AOT_TEST_CHECK(table().at(Builtins::FunctionPrototype).flags.containsAll({ BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames }));
    AOT_TEST_CHECK(table().openNamesOf(Builtins::FunctionPrototype).size() == 2 && std::ranges::is_sorted(table().openNamesOf(Builtins::FunctionPrototype)));
    for (BuiltinName name : table().openNamesOf(Builtins::FunctionPrototype))
        AOT_TEST_CHECK(!table().isClosedKey(Builtins::FunctionPrototype, name));
    for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::then, BuiltinNames::push, hostName("Bun"_s), table().nameOf(WellKnownSymbol::ToPrimitive) })
        AOT_TEST_CHECK(table().isClosedKey(Builtins::FunctionPrototype, name));
    AOT_TEST_CHECK(!table().isClosedKey(Builtins::FunctionPrototype, BuiltinNames::toString) && !table().isClosedKey(Builtins::FunctionPrototype, BuiltinNames::call));
    AOT_TEST_CHECK(table().at(Builtins::FunctionPrototypeCall).mutability == BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(table().at(Builtins::FunctionPrototypeToString).mutability == BuiltinMutability::ConstantUnlessWritten);
    for (BuiltinID id : { Builtins::ObjectPrototypeHasOwnProperty, Builtins::ObjectPrototypeToString, Builtins::ArrayPrototypeSlice, Builtins::MathMax, Builtins::FunctionPrototypeBind }) {
        AOT_TEST_CHECK(table().isHostFunctionOfEngine(id) && table().hasLockedPrototypeLink(id));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::call, BuiltinNames::apply, BuiltinNames::bind, table().nameOf(WellKnownSymbol::HasInstance) })
            AOT_TEST_CHECK(table().isClosedKey(id, name));
        AOT_TEST_CHECK(!table().isClosedKey(id, BuiltinNames::toString) && !table().isClosedKey(id, BuiltinNames::then) && !table().isClosedKey(id, table().nameOf(WellKnownSymbol::ToPrimitive)));
    }
    for (BuiltinID id : { Builtins::ArrayPrototypeMap, Builtins::Error, Builtins::Math, Builtins::ArrayPrototype })
        AOT_TEST_CHECK(!table().isHostFunctionOfEngine(id));
    for (BuiltinID id : { hostBuiltin("Fetch"_s), hostBuiltin("SetTimeout"_s), hostBuiltin("ConsoleLog"_s) })
        AOT_TEST_CHECK(table().ignoresReceiver(id) && table().isNativeFunctionOfHost(id));
    AOT_TEST_CHECK(!table().ignoresReceiver(hostBuiltin("ProcessNextTick"_s)) && !table().isNativeFunctionOfHost(hostBuiltin("ProcessNextTick"_s)));
    for (BuiltinID id : { Builtins::Math, Builtins::ArrayPrototype, Builtins::ArrayPrototypeMap, Builtins::ArrayPrototypeSlice, Builtins::ObjectPrototypeValueOf, Builtins::ObjectPrototypeHasOwnProperty, Builtins::FunctionPrototypeBind })
        AOT_TEST_CHECK(!table().ignoresReceiver(id));
    for (BuiltinID id : { Builtins::ArrayPrototypeMap, Builtins::FunctionPrototypeCall, Builtins::FunctionPrototypeApply, Builtins::ReflectGet }) {
        AOT_TEST_CHECK(table().isFunctionOfEngine(id) && !table().isHostFunctionOfEngine(id) && table().hasLockedPrototypeLink(id));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::call, BuiltinNames::apply, BuiltinNames::bind, table().nameOf(WellKnownSymbol::HasInstance) })
            AOT_TEST_CHECK(table().isClosedKey(id, name));
        AOT_TEST_CHECK(!table().isClosedKey(id, BuiltinNames::toString) && !table().isClosedKey(id, BuiltinNames::then));
    }
    for (BuiltinID id : { Builtins::Error, Builtins::Math, Builtins::ArrayPrototype })
        AOT_TEST_CHECK(!table().isFunctionOfEngine(id));
    AOT_TEST_CHECK(!std::ranges::contains(table().lockedMembers(), Builtins::FunctionPrototypeToString));
    for (BuiltinID id : { Builtins::FunctionPrototypeName, Builtins::FunctionPrototypeLength }) {
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::AlwaysLocked && table().overloadsOf(id).size() == 1);
        const BuiltinOverload& read = table().overloadsOf(id)[0];
        AOT_TEST_CHECK(read.mode == BuiltinCallMode::Get && read.receiver->uses == OperandUses { OperandUse::Inspects } && read.effects.capability == BuiltinCapability::Heap);
    }
    AOT_TEST_CHECK(!table().at(BuiltinClasses::BoundFunction).hasClosureInstances && !table().at(BuiltinClasses::Function).hasClosureInstances);
    AOT_TEST_CHECK(table().definesCharactersOnNewInstances(BuiltinClasses::String));
    for (BuiltinClassID otherClass : { BuiltinClasses::Number, BuiltinClasses::Object, BuiltinClasses::Array, BuiltinClasses::StringIterator })
        AOT_TEST_CHECK(!table().definesCharactersOnNewInstances(otherClass));

    for (BuiltinClassID errorClass : { BuiltinClasses::Error, BuiltinClasses::TypeError, BuiltinClasses::AggregateError, BuiltinClasses::SuppressedError }) {
        AOT_TEST_CHECK(!table().listsOwnPropertiesOfNewInstances(errorClass));
        for (BuiltinName name : { BuiltinNames::message, BuiltinNames::cause, BuiltinNames::stack })
            AOT_TEST_CHECK(table().mayDefineOnNewInstances(errorClass, name));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::name, BuiltinNames::then, BuiltinNames::length, table().nameOf(WellKnownSymbol::ToPrimitive), BuiltinName() })
            AOT_TEST_CHECK(!table().mayDefineOnNewInstances(errorClass, name));
    }
    AOT_TEST_CHECK(table().mayDefineOnNewInstances(BuiltinClasses::AggregateError, BuiltinNames::errors) && !table().mayDefineOnNewInstances(BuiltinClasses::TypeError, BuiltinNames::errors));
    AOT_TEST_CHECK(table().listsOwnPropertiesOfNewInstances(BuiltinClasses::Set) && !table().mayDefineOnNewInstances(BuiltinClasses::Set, BuiltinNames::message));
    AOT_TEST_CHECK(table().overloadsOf(Builtins::FunctionPrototypeBind)[0].result.prototypeSource == BuiltinOperand::receiver());

    for (BuiltinID id : { Builtins::ArraySymbolSpecies, Builtins::MapSymbolSpecies })
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::AlwaysLocked);

    for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::then, BuiltinNames::hasOwnProperty, BuiltinNames::valueOf, table().nameOf(WellKnownSymbol::ToPrimitive), table().nameOf(WellKnownSymbol::IsConcatSpreadable) })
        AOT_TEST_CHECK(table().isClosedKey(Builtins::MapPrototype, name));
    AOT_TEST_CHECK(!table().isClosedKey(Builtins::MapPrototype, BuiltinNames::push) && !table().isClosedKey(Builtins::MapPrototype, hostName("Bun"_s)));
    AOT_TEST_CHECK(!table().isClosedKey(Builtins::MapPrototype, BuiltinNames::toJSON) && !table().isClosedKey(Builtins::SetPrototype, BuiltinNames::toJSON));
    AOT_TEST_CHECK(table().isClosedKey(Builtins::ArrayPrototype, BuiltinNames::toJSON) && table().isClosedKey(Builtins::ArrayPrototype, BuiltinNames::then));
    for (BuiltinName textOfPrimitive : { BuiltinNames::undefined, BuiltinNames::null, BuiltinNames::trueKeyword, BuiltinNames::falseKeyword }) {
        for (BuiltinID prototype : { Builtins::ArrayPrototype, Builtins::StringPrototype, Builtins::MapPrototype, Builtins::Uint8ArrayPrototype, Builtins::FunctionPrototype })
            AOT_TEST_CHECK(table().isClosedKey(prototype, textOfPrimitive));
        for (BuiltinID other : { Builtins::Promise, Builtins::Symbol, Builtins::Math, Builtins::Array })
            AOT_TEST_CHECK(!table().isClosedKey(other, textOfPrimitive));
    }
    for (BuiltinID id : { Builtins::StringPrototypeBold, Builtins::StringPrototypeAt, Builtins::SetPrototypeUnion })
        AOT_TEST_CHECK(table().at(id).mutability == BuiltinMutability::ConstantUnlessWritten);
    AOT_TEST_CHECK(std::ranges::contains(table().lockedMembers(), Builtins::MathImul) && std::ranges::contains(table().lockedMembers(), Builtins::Math));
    AOT_TEST_CHECK(!std::ranges::contains(table().lockedMembers(), Builtins::MathRandom));
    AOT_TEST_CHECK(table().isClosedKey(Builtins::Math, BuiltinNames::valueOf) && table().isClosedKey(Builtins::Math, table().nameOf(WellKnownSymbol::ToPrimitive)));
    AOT_TEST_CHECK(!table().isClosedKey(Builtins::Math, BuiltinNames::then) && !table().isClosedKey(Builtins::Math, BuiltinNames::push) && !table().isClosedKey(Builtins::Math, BuiltinNames::random));
    AOT_TEST_CHECK(!table().isClosedKey(Builtins::Intl, BuiltinNames::valueOf) && table().closedKeysOf(Builtins::Intl).empty());
    for (BuiltinID id : { Builtins::Object, Builtins::Array, Builtins::Number, Builtins::JSON, Builtins::Reflect, Builtins::Date })
        AOT_TEST_CHECK(table().at(id).flags.contains(BuiltinFlag::HasLockedMembers) && !table().isClosedKey(id, BuiltinNames::push));
    AOT_TEST_CHECK(!table().hasLockedPrototypeLink(Builtins::Intl));
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::Error, Builtins::TypeError, Builtins::Function, Builtins::WeakRef, hostBuiltin("Blob"_s), hostBuiltin("FormData"_s), Builtins::URL }) {
        AOT_TEST_CHECK(table().closesOnlyNamesOfCalls(id) && table().hasLockedPrototypeLink(id) && !table().closesIndexKeys(id));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::call, BuiltinNames::apply, BuiltinNames::bind, table().nameOf(WellKnownSymbol::HasInstance) })
            AOT_TEST_CHECK(table().isClosedKey(id, name));
        AOT_TEST_CHECK(!table().isClosedKey(id, BuiltinNames::toString) && !table().isClosedKey(id, BuiltinNames::then) && !table().isClosedKey(id, table().nameOf(WellKnownSymbol::Species)));
    }
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::Object, Builtins::Array, Builtins::Math, Builtins::ArrayPrototype, Builtins::ArrayPrototypeMap, hostBuiltin("BunGlob"_s) })
        AOT_TEST_CHECK(!table().closesOnlyNamesOfCalls(id));
    AOT_TEST_CHECK(std::ranges::is_sorted(table().closedKeysOf(Builtins::MapPrototype)));
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

static void testStringConstantsFollowTheTarget()
{
    for (BuiltinID id : std::initializer_list<BuiltinID> { hostBuiltin("ProcessPlatform"_s), hostBuiltin("ProcessArch"_s) })
        AOT_TEST_CHECK(table().at(id).kind == BuiltinKind::Constant && table().at(id).mutability == BuiltinMutability::ConstantUnlessWritten);
    AOT_TEST_CHECK(table().nameOfString(hostBuiltin("ProcessPlatform"_s), { BuiltinPlatform::Windows, BuiltinArchitecture::X86_64 }) == nameOf("win32"_s));
    AOT_TEST_CHECK(table().nameOfString(hostBuiltin("ProcessPlatform"_s), { BuiltinPlatform::Darwin, BuiltinArchitecture::ARM64 }) == nameOf("darwin"_s));
    AOT_TEST_CHECK(table().nameOfString(hostBuiltin("ProcessArch"_s), { BuiltinPlatform::Linux, BuiltinArchitecture::ARM64 }) == nameOf("arm64"_s));
    AOT_TEST_CHECK(table().nameOfString(hostBuiltin("ProcessArch"_s), { BuiltinPlatform::Linux, BuiltinArchitecture::X86_64 }) == nameOf("x64"_s));
    AOT_TEST_CHECK(table().nameOfString(hostBuiltin("ProcessPlatform"_s)) == table().nameOfString(hostBuiltin("ProcessPlatform"_s), targetOfThisBuild()));
    AOT_TEST_CHECK(table().stringOf(hostBuiltin("ProcessPlatform"_s)) && !table().stringOf(hostBuiltin("ProcessExecPath"_s)) && !table().stringOf(Builtins::MathPI));

    std::span<const BuiltinStringConstant> constants = builtinTableData.attachments.stringConstants;
    AOT_TEST_CHECK(!constants.empty());
    for (const BuiltinStringConstant& constant : constants) {
        AOT_TEST_CHECK(constant.text && !constant.platforms.isEmpty() && !constant.architectures.isEmpty());
        for (BuiltinPlatform platform : constant.platforms) {
            for (BuiltinArchitecture architecture : constant.architectures)
                AOT_TEST_CHECK(table().nameOfString(constant.builtin, { platform, architecture }) == constant.text);
        }
    }
}

template<typename Record>
static void checkRecordsByBuiltin(std::span<const Record> all, std::span<const Record> (BuiltinTable::*recordsOf)(BuiltinID) const)
{
    size_t numberOfRecords = 0;
    forEachBuiltin([&](BuiltinID id, const Builtin&) {
        std::span<const Record> records = (table().*recordsOf)(id);
        numberOfRecords += records.size();
        for (const Record& record : records)
            AOT_TEST_CHECK(record.builtin == id);
    });
    AOT_TEST_CHECK(numberOfRecords == all.size());
}

static void testAttachmentsAreFoundByEntry()
{
    const BuiltinTableAttachments& attachments = builtinTableData.attachments;
    checkRecordsByBuiltin(attachments.featureUses, &BuiltinTable::featureUsesOf);
    checkRecordsByBuiltin(attachments.moduleLoads, &BuiltinTable::moduleLoadsOf);
    checkRecordsByBuiltin(attachments.calledFunctions, &BuiltinTable::calledFunctionsOf);
    AOT_TEST_CHECK(!attachments.featureUses.empty() && !attachments.moduleLoads.empty() && !attachments.numberConstants.empty());

    for (const BuiltinUnseenCode& record : attachments.unseenCode)
        AOT_TEST_CHECK(table().unseenCodeOf(record.builtin) == &record && record.condition);
    for (const BuiltinForms& record : attachments.forms)
        AOT_TEST_CHECK(table().formsOf(record.builtin) == &record);
    for (const BuiltinBoxedCall& record : table().boxedCalls())
        AOT_TEST_CHECK(table().boxedCallStubOf(record.builtin) == record.stub);
    for (const BuiltinBoxedCall& record : table().boxedCalls())
        AOT_TEST_CHECK(table().methodCalledBy(record.stub) == table().canonicalID(record.builtin));
    for (unsigned stub = 0; stub < numberOfBuiltinBoxedStubs; ++stub)
        AOT_TEST_CHECK(table().boxedCallStubOf(table().methodCalledBy(static_cast<BuiltinBoxedStub>(stub))) == static_cast<BuiltinBoxedStub>(stub));
    for (const BuiltinNumberConstant& record : attachments.numberConstants)
        AOT_TEST_CHECK(table().numberOf(record.builtin).has_value());

    AOT_TEST_CHECK(table().featureUsesOf(Builtins::MathMax).empty() && table().moduleLoadsOf(Builtins::MathMax).empty() && table().calledFunctionsOf(Builtins::MathMax).empty());
    AOT_TEST_CHECK(!table().unseenCodeOf(Builtins::MathMax) && !table().at(Builtins::MathMax).flags.contains(BuiltinFlag::RunsUnseenCode) && !table().boxedCallStubOf(Builtins::Math));
}

static void testHostUsesOfStartupLoads()
{
    std::span<const BuiltinCalledFunction> functions = table().startupCalledFunctions();
    AOT_TEST_CHECK(std::ranges::equal(functions, table().functionsCalledWithStartupLoads(), { }, &BuiltinCalledFunction::function));
    for (const BuiltinCalledFunction& called : functions)
        AOT_TEST_CHECK(!called.builtin && !called.condition);

    AOT_TEST_CHECK(table().startupLoads().size() == 1 && table().startupLoads()[0].exports.size() == 1);
    const BuiltinHostUses& usesOfInspect = table().startupLoads()[0].usesOfExports[0];
    AOT_TEST_CHECK(usesOfInspect.areListed && usesOfInspect.list.size() == 1 && usesOfInspect.list[0].use == BuiltinHostReadUse::Kept);
    BuiltinName inspectCell = usesOfInspect.list[0].nativeCell;

    auto options = std::ranges::find_if(table().hostObjects(), [](const BuiltinHostObject& object) {
        return std::ranges::contains(object.fields, nameOf("stylize"_s), &BuiltinHostObjectField::name);
    });
    AOT_TEST_CHECK(options != table().hostObjects().end() && options->prototype == Builtins::ObjectPrototype && options->fields.size() == 4);
    HashSet<uint32_t> cellsOfStylize;
    for (const BuiltinHostObjectField& field : options->fields) {
        if (field.name == nameOf("stylize"_s))
            cellsOfStylize.add(field.value.nativeCell.index() + 1);
    }
    AOT_TEST_CHECK(cellsOfStylize.size() == 2 && std::ranges::count(functions, true, [](const BuiltinCalledFunction& called) { return called.uses.areListed; }) == 2);
    for (const BuiltinCalledFunction& called : functions) {
        if (!called.uses.areListed)
            continue;
        AOT_TEST_CHECK(called.uses.list.size() == 1);
        const BuiltinHostUse& use = called.uses.list[0];
        const BuiltinHostUse& kept = use.call ? use.call->result.list[0] : use;
        AOT_TEST_CHECK(kept.use == BuiltinHostReadUse::Kept && cellsOfStylize.contains(kept.nativeCell.index() + 1));
        AOT_TEST_CHECK(!use.call || (use.call->arguments.size() == 1 && use.call->arguments[0].nativeCell == inspectCell));
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

static void testModulesListWhatRowsSayNativeCodeReads()
{
    size_t namesInRows = 0;
    auto checkRow = [&](const BuiltinModuleLoad& load) {
        const RuntimeModuleData& module = table().at(load.module);
        AOT_TEST_CHECK(!load.readsEveryExport || module.flags.contains(RuntimeModuleFlag::NativeCodeReadsEveryExport));
        for (BuiltinName name : load.exports)
            AOT_TEST_CHECK(std::ranges::contains(module.exportsReadByNativeCode, name));
        namesInRows += load.exports.size();
    };
    for (uint32_t index = 0; index < table().builtinCount(); ++index)
        std::ranges::for_each(table().moduleLoadsOf(BuiltinID(index)), checkRow);
    std::ranges::for_each(table().startupLoads(), checkRow);
    AOT_TEST_CHECK(namesInRows);
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
        { "builtin-table/tokens-round-trip"_s, testTokensRoundTrip },
        { "builtin-table/unknown-tokens"_s, testUnknownTokens },
        { "builtin-table/dispatch-is-total"_s, testDispatchIsTotal },
        { "builtin-table/classes-and-slots"_s, testClassesAndSlots },
        { "builtin-table/class-relations"_s, testClassRelations },
        { "builtin-table/member-lookup"_s, testMemberLookup },
        { "builtin-table/members-asked-by-their-byte-alone"_s, testMembersAskedByByteAlone },
        { "builtin-table/member-missing-on-target-is-not-found"_s, testMemberMissingOnTargetIsNotFound },
        { "builtin-table/mutability-follows-the-lock"_s, testMutabilityFollowsTheLock },
        { "builtin-table/locked-objects"_s, testLockedObjects },
        { "builtin-table/locked-members"_s, testLockedMembers },
        { "builtin-table/no-return-can-lie-above-iterators-of-engine"_s, testNoReturnCanLieAboveIteratorsOfEngine },
        { "builtin-table/string-constants-follow-the-target"_s, testStringConstantsFollowTheTarget },
        { "builtin-table/attachments-are-found-by-entry"_s, testAttachmentsAreFoundByEntry },
        { "builtin-table/host-uses-of-startup-loads"_s, testHostUsesOfStartupLoads },
        { "builtin-table/modules-list-what-rows-say-native-code-reads"_s, testModulesListWhatRowsSayNativeCodeReads },
        { "builtin-table/no-object-is-proven-to-be-plain-data"_s, testNoObjectIsProvenToBePlainData },
        { "builtin-table/type-narrowed-to-class-written-below-parameter-lies-in-it"_s, testTypeNarrowedToClassWrittenBelowParameterLiesInIt },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
