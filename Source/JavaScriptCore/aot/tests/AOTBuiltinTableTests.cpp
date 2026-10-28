/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTHostEntriesTestSupport.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTResultRules.h"
#include "AOTRuntimeDescription.h"
#include "AOTStubsConstruct.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "IdentifierInlines.h"
#include "JSLock.h"
#include "VM.h"
#include <algorithm>
#include <numbers>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
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

static void testCountsAgreeWithGeneratedIDs()
{
    AOT_TEST_CHECK(table().builtinCount() == builtinTableData.builtins.size() && table().classCount() == builtinTableData.classes.size());
    AOT_TEST_CHECK(table().slotCount() == numberOfBuiltinSlots && table().layoutCount() == builtinTableData.layouts.size());
    AOT_TEST_CHECK(&BuiltinTable::singleton() == &BuiltinTable::singleton());
}

static void testTableDescribesItself()
{
    auto bytesOf = [](const BuiltinTableDescription& description) {
        return std::span(reinterpret_cast<const uint8_t*>(&description), sizeof(description));
    };
    AOT_TEST_CHECK(describedBuiltinTable.numberOfBuiltins == table().builtinCount() && describedBuiltinTable.numberOfClasses == table().classCount());
    AOT_TEST_CHECK(!findDifferenceFromThisRuntime(bytesOf(describedBuiltinTable)));

    BuiltinTableDescription other = describedBuiltinTable;
    ++other.numberOfBuiltins;
    std::optional<RuntimeDifference> difference = findDifferenceFromThisRuntime(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name == "the number of built-ins"_s && difference->there == difference->here + 1);

    other = describedBuiltinTable;
    other.size += 4;
    difference = findDifferenceFromThisRuntime(bytesOf(other));
    AOT_TEST_CHECK(difference && difference->name.isNull());
}

static void testNamedListsTranslate()
{
    AOT_TEST_CHECK(table().tokenOf(Builtins::ArrayPrototypePush) == "ArrayPrototypePush"_s && table().tokenOf(Builtins::BunServe) == "BunServe"_s);
    AOT_TEST_CHECK(table().at(Builtins::ArrayPrototypePush).origin == BuiltinOrigin::Engine && table().at(Builtins::BunServe).origin == BuiltinOrigin::Host);
    AOT_TEST_CHECK(table().tokenOf(BuiltinClasses::Array) == "Array"_s && table().tokenOf(BuiltinClasses::URLSearchParams) == "URLSearchParams"_s);
    for (unsigned position = 0; position < numberOfNamedBuiltins; ++position)
        AOT_TEST_CHECK(table().contains(BuiltinID(NamedBuiltin(position))));
}

static void testMarks()
{
    AOT_TEST_CHECK(table().has(BuiltinMark::IsSpy, hostBuiltin("BunTestSpyOn"_s)) && !table().has(BuiltinMark::HoldsLoader, hostBuiltin("BunTestSpyOn"_s)) && !table().has(BuiltinMark::IsSpy, BuiltinID()));
    AOT_TEST_CHECK(table().marked(BuiltinMark::IsNotEnumerable).size() == 2 && table().has(BuiltinMark::IsNotEnumerable, hostBuiltin("Console_stdout"_s)));
    AOT_TEST_CHECK(table().has(BuiltinMark::MayBeAbsent, Builtins::SharedArrayBuffer) && !table().has(BuiltinMark::MayBeAbsent, Builtins::ArrayBuffer));
    for (BuiltinMark mark : { BuiltinMark::IsAddedByOption, BuiltinMark::MayBeAbsent, BuiltinMark::EvaluatesTextInGlobalScope, BuiltinMark::HoldsLoader, BuiltinMark::IsSpy, BuiltinMark::IsNotEnumerable }) {
        std::span<const BuiltinMarkedEntry> marked = table().marked(mark);
        AOT_TEST_CHECK(!marked.empty() && std::ranges::is_sorted(marked, { }, &BuiltinMarkedEntry::builtin));
        for (const BuiltinMarkedEntry& entry : marked)
            AOT_TEST_CHECK(entry.mark == mark && table().contains(entry.builtin) && table().has(mark, entry.builtin));
    }
    auto isReportedAs = [](BuiltinID callee, BuiltinID named) {
        return std::ranges::any_of(table().loadersOfMachineCode(), [&](const BuiltinLoaderOfMachineCode& loader) {
            return loader.callee == callee && loader.named == named;
        });
    };
    AOT_TEST_CHECK(table().loadersOfMachineCode().size() == 5 && isReportedAs(Builtins::ProcessDlopen, Builtins::ProcessDlopen));
    AOT_TEST_CHECK(isReportedAs(hostBuiltin("NativeFunctionsFfiBindTsCc"_s), hostBuiltin("BunFfiCc"_s)));
    AOT_TEST_CHECK(isReportedAs(hostBuiltin("BunFFIDlopen"_s), hostBuiltin("BunFfiDlopen"_s)) && isReportedAs(hostBuiltin("BunFFILinkSymbols"_s), hostBuiltin("BunFfiLinkSymbols"_s)));
    AOT_TEST_CHECK(isReportedAs(hostBuiltin("BunFFICfunction"_s), hostBuiltin("BunFfiCFunction"_s)));
}

static void testGlobalThisIsFirst()
{
    AOT_TEST_CHECK(!Builtins::GlobalThis.index());
    const Builtin& global = table().at(Builtins::GlobalThis);
    AOT_TEST_CHECK(!global.holder && global.flags.contains(BuiltinFlag::IsGlobalObject) && global.name == BuiltinNames::globalThis);
    AOT_TEST_CHECK(table().findGlobal(BuiltinNames::globalThis) == Builtins::GlobalThis);
    AOT_TEST_CHECK(table().findGlobal(nameOf("Array"_s)) == Builtins::Array);
    AOT_TEST_CHECK(!table().findGlobal(BuiltinNames::push));
    AOT_TEST_CHECK(table().textOf(Builtins::GlobalThis) == "globalThis"_s);

    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(builtin.flags.contains(BuiltinFlag::IsGlobalObject) == (table().canonicalID(id) == Builtins::GlobalThis));
    });
}

static void testMembersAreOneRunInNameOrder()
{
    unsigned numberOfRoots = 0;
    while (numberOfRoots < table().builtinCount() && !table().at(BuiltinID(numberOfRoots)).holder)
        ++numberOfRoots;
    AOT_TEST_CHECK(numberOfRoots > 1);

    unsigned nextMember = numberOfRoots;
    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(!builtin.holder == (id.index() < numberOfRoots));
        auto members = table().membersOf(id);
        AOT_TEST_CHECK(members.size() == builtin.memberCount);
        if (members.isEmpty())
            return;
        AOT_TEST_CHECK(builtin.firstMember.index() == nextMember && isObjectKind(builtin.kind));
        nextMember += members.size();

        BuiltinName previousName;
        for (BuiltinID member : members) {
            const Builtin& data = table().at(member);
            AOT_TEST_CHECK(data.holder == id && table().contains(data.name));
            AOT_TEST_CHECK(!previousName || previousName < data.name);
            previousName = data.name;
            bool isFound = !data.flags.contains(BuiltinFlag::IsDefaultExport) && table().existsOn(member, targetOfThisBuild().platform);
            AOT_TEST_CHECK(table().findOwnMember(id, data.name) == (isFound ? member : BuiltinID()));
            AOT_TEST_CHECK(table().isUnder(member, id) && !table().isUnder(id, member));
        }
    });
    AOT_TEST_CHECK(nextMember == table().builtinCount());
}

static void testHiddenRootsAscendByPath()
{
    String previousPath;
    for (unsigned index = 1; !table().at(BuiltinID(index)).holder; ++index) {
        String path = table().textOf(BuiltinID(index));
        AOT_TEST_CHECK(previousPath.isNull() || codePointCompareLessThan(previousPath, path));
        AOT_TEST_CHECK(table().rootOf(BuiltinID(index)) == BuiltinID(index));
        previousPath = path;
    }
    AOT_TEST_CHECK(table().rootOf(Builtins::ArrayPrototypePush) == Builtins::Array && table().rootOf(Builtins::Array) == Builtins::Array);
    AOT_TEST_CHECK(table().rootOf(hostBuiltin("NodeFsReadFile"_s)) == hostBuiltin("NodeFs"_s) && !table().at(hostBuiltin("NodeFs"_s)).holder);
}

static void testNamesAreThreeRuns()
{
    unsigned numberOfTextNames = 0;
    unsigned numberOfSymbolNames = 0;
    unsigned numberOfRegisteredSymbolNames = 0;
    std::span<const uint8_t> previousText;
    unsigned previousRun = 0;
    forEachID<BuiltinName>(table().nameCount(), [&](BuiltinName name) {
        unsigned run = table().isRegisteredSymbolName(name) ? 2 : table().isSymbolName(name) ? 1 : 0;
        ++(run == 2 ? numberOfRegisteredSymbolNames : run == 1 ? numberOfSymbolNames : numberOfTextNames);
        AOT_TEST_CHECK(run >= previousRun);
        std::span<const uint8_t> text = table().textOf(name);
        AOT_TEST_CHECK(!name.index() || run != previousRun || compareSpans(previousText, text) < 0);
        if (!run)
            AOT_TEST_CHECK(table().nameOf(text) == name && !table().wellKnownSymbolOf(name));
        else
            AOT_TEST_CHECK(table().nameOf(text) != name);
        AOT_TEST_CHECK(table().wellKnownSymbolOf(name).has_value() == (run == 1));
        previousText = text;
        previousRun = run;
    });
    AOT_TEST_CHECK(numberOfTextNames && numberOfSymbolNames == numberOfEnumerators<WellKnownSymbol>);
    AOT_TEST_CHECK(!table().isSymbolName(BuiltinName()) && !table().isRegisteredSymbolName(BuiltinName()));
    AOT_TEST_CHECK(!table().nameOf(byteCast<uint8_t>("no name of any built-in"_s.span())));
}

static void testSymbolNames()
{
    for (unsigned index = 0; index < numberOfEnumerators<WellKnownSymbol>; ++index) {
        auto symbol = static_cast<WellKnownSymbol>(index);
        BuiltinName name = table().nameOf(symbol);
        AOT_TEST_CHECK(name && table().isSymbolName(name) && !table().isRegisteredSymbolName(name) && table().wellKnownSymbolOf(name) == symbol);
    }
    BuiltinName iteratorSymbol = table().nameOf(WellKnownSymbol::Iterator);
    AOT_TEST_CHECK(iteratorSymbol != BuiltinNames::iterator && equalSpans(table().textOf(iteratorSymbol), table().textOf(BuiltinNames::iterator)));
    AOT_TEST_CHECK(table().nameOfSymbol(Builtins::SymbolIterator) == iteratorSymbol);
    AOT_TEST_CHECK(!table().nameOfSymbol(Builtins::SymbolFor) && !table().nameOfSymbol(Builtins::MathMax));
    AOT_TEST_CHECK(table().findOwnMember(Builtins::ArrayPrototype, iteratorSymbol) == Builtins::ArrayPrototypeSymbolIterator);
    AOT_TEST_CHECK(!table().findOwnMember(Builtins::ArrayPrototype, BuiltinNames::iterator));
    AOT_TEST_CHECK(table().findOwnMember(Builtins::Symbol, BuiltinNames::iterator) == Builtins::SymbolIterator);

    AOT_TEST_CHECK(table().textOf(Builtins::ArrayPrototypePush) == "Array.prototype.push"_s);
    AOT_TEST_CHECK(table().textOf(Builtins::ArrayPrototypeSymbolIterator) == "Array.prototype[Symbol.iterator]"_s);
}

// identifierOf() makes the key of a name, and nameOf() finds the name of a key by the hash the key has.
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
    // The lock holder has the last reference, and gives it up while it holds the lock.
    vm = nullptr;
}

// functionNameOf() gives the name that the engine gives a function under the key of identifierOf(), and asks no atom table for it.
static void testFunctionNamesFollowKeys()
{
    RefPtr<VM> vm = VM::create();
    JSLockHolder locker(*vm);
    Vector<String> functionNames;
    // The end of a collection, which names the frames of an error, has no atom table either.
    auto* atoms = Thread::currentSingleton().setCurrentAtomStringTable(nullptr);
    forEachID<BuiltinName>(table().nameCount(), [&](BuiltinName name) {
        functionNames.append(table().functionNameOf(name));
    });
    Thread::currentSingleton().setCurrentAtomStringTable(atoms);

    forEachID<BuiltinName>(table().nameCount(), [&](BuiltinName name) {
        Identifier key = table().identifierOf(*vm, name);
        AOT_TEST_CHECK(functionNames[name.index()] == (key.isSymbol() ? makeString('[', key.string(), ']') : key.string()));
    });
    AOT_TEST_CHECK(table().functionNameOf(BuiltinNames::push) == "push"_s);
    AOT_TEST_CHECK(table().functionNameOf(table().nameOf(WellKnownSymbol::Iterator)) == "[Symbol.iterator]"_s);
    // The lock holder has the last reference, and gives it up while it holds the lock.
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

static void testOverloadsAreOneRunInModeOrder()
{
    unsigned nextOverload = 0;
    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        std::span<const BuiltinOverload> overloads = table().overloadsOf(id);
        AOT_TEST_CHECK(overloads.size() == builtin.overloadCount && table().overloadIDsOf(id).size() == builtin.overloadCount);
        AOT_TEST_CHECK(overloads.empty() || builtin.firstOverload.index() == nextOverload);
        nextOverload += overloads.size();

        bool hasMode[4] { };
        for (size_t index = 0; index < overloads.size(); ++index) {
            AOT_TEST_CHECK(overloads[index].builtin == id);
            AOT_TEST_CHECK(&overloads[index] == &table().at(table().overloadIDsOf(id).at(index)));
            AOT_TEST_CHECK(!index || overloads[index - 1].mode <= overloads[index].mode);
            hasMode[static_cast<unsigned>(overloads[index].mode)] = true;
        }
        for (BuiltinCallMode mode : { BuiltinCallMode::Call, BuiltinCallMode::Construct, BuiltinCallMode::Get, BuiltinCallMode::Set })
            AOT_TEST_CHECK(table().dispatchOf(id, mode).has_value() == hasMode[static_cast<unsigned>(mode)]);
    });
    AOT_TEST_CHECK(nextOverload == table().overloadCount());

    AOT_TEST_CHECK(table().dispatchOf(Builtins::MathMax, BuiltinCallMode::Call) && !table().dispatchOf(Builtins::MathMax, BuiltinCallMode::Get));
    AOT_TEST_CHECK(table().dispatchOf(Builtins::Map, BuiltinCallMode::Call) && table().dispatchOf(Builtins::Map, BuiltinCallMode::Construct));
    AOT_TEST_CHECK(table().dispatchOf(Builtins::ArrayBufferPrototypeByteLength, BuiltinCallMode::Get) && !table().dispatchOf(Builtins::ArrayBufferPrototypeByteLength, BuiltinCallMode::Call));
    AOT_TEST_CHECK(!table().dispatchOf(Builtins::Math, BuiltinCallMode::Call) && table().overloadsOf(Builtins::Math).empty());
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

static void testUndeclaredOverloadClaimsNothing()
{
    unsigned numberOfUndeclaredOverloads = 0;
    forEachID<BuiltinOverloadID>(table().overloadCount(), [&](BuiltinOverloadID id) {
        const BuiltinOverload& overload = table().at(id);
        if (!overload.isUndeclared())
            return;
        ++numberOfUndeclaredOverloads;
        AOT_TEST_CHECK(overload.throws() && overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode));
        AOT_TEST_CHECK(table().at(overload.result.type).isAnything());
        AOT_TEST_CHECK(overload.parameters.size() == 1 && overload.parameters[0].mode == BuiltinParameterMode::Rest && overload.parameters[0].operand == BuiltinOperand::argumentsFrom(0));
        AOT_TEST_CHECK(table().at(overload.parameters[0].accepted).isAnything() && overload.parameters[0].uses == OperandUses::all() && overload.parameters[0].protocols == Protocols::all());
    });
    AOT_TEST_CHECK(numberOfUndeclaredOverloads);
    for (const BuiltinOverload& overload : table().overloadsOf(Builtins::MathAbs))
        AOT_TEST_CHECK(!overload.isUndeclared());
}

// A constructor that throws nothing is entered without a test of the stack.
static void testConstructorsThatThrowNothingTakeFewArguments()
{
    unsigned numberOfConstructors = 0;
    forEachID<BuiltinOverloadID>(table().overloadCount(), [&](BuiltinOverloadID id) {
        const BuiltinOverload& overload = table().at(id);
        if (overload.mode != BuiltinCallMode::Construct || overload.throws())
            return;
        ++numberOfConstructors;
        AOT_TEST_CHECK(overload.parameters.size() <= maxArgumentCountOfUntestedHostFrame);
        AOT_TEST_CHECK(std::ranges::none_of(overload.parameters, [](const BuiltinParameter& parameter) {
            return parameter.mode == BuiltinParameterMode::Rest;
        }));
    });
    AOT_TEST_CHECK(numberOfConstructors);
}

static void testParameterFor()
{
    for (const BuiltinOverload& overload : table().overloadsOf(Builtins::ArrayPrototypePush)) {
        AOT_TEST_CHECK(table().parameterFor(overload, BuiltinOperand::receiver()) == overload.receiver);
        const BuiltinParameter* first = table().parameterFor(overload, BuiltinOperand::argument(0));
        AOT_TEST_CHECK(first && first->mode == BuiltinParameterMode::Rest && table().parameterFor(overload, BuiltinOperand::argument(100)) == first);
    }
    for (const BuiltinOverload& overload : table().overloadsOf(Builtins::MathAbs)) {
        AOT_TEST_CHECK(overload.parameters.size() == 1 && table().parameterFor(overload, BuiltinOperand::argument(0)) == &overload.parameters[0]);
        AOT_TEST_CHECK(!table().parameterFor(overload, BuiltinOperand::argument(1)));
    }
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
            AOT_TEST_CHECK(table().at(slot).indexInType == numberOfSlotsInType && table().slotInType(id, numberOfSlotsInType) == slot);
            AOT_TEST_CHECK(table().findSlot(id, *table().at(slot).contentsSlot) == slot);
            ++numberOfSlotsInType;
        }
        AOT_TEST_CHECK(table().numberOfSlotsInType(id) == numberOfSlotsInType && !table().slotInType(id, numberOfSlotsInType));
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
    AOT_TEST_CHECK(table().slotInType(BuiltinClasses::Map, 0) == BuiltinSlots::MapKey && table().slotInType(BuiltinClasses::Map, 1) == BuiltinSlots::MapValue);
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

static void testCanonicalID()
{
    AOT_TEST_CHECK(table().canonicalID(Builtins::NumberParseInt) == Builtins::ParseInt && table().canonicalID(Builtins::ParseInt) == Builtins::ParseInt);
    AOT_TEST_CHECK(table().canonicalID(Builtins::SetPrototypeKeys) == table().canonicalID(Builtins::SetPrototypeValues));
    AOT_TEST_CHECK(table().canonicalID(Builtins::SetPrototypeSymbolIterator) == table().canonicalID(Builtins::SetPrototypeValues));
    AOT_TEST_CHECK(table().canonicalID(Builtins::ArrayPrototypeSymbolIterator) == table().canonicalID(Builtins::ArrayPrototypeValues));
    AOT_TEST_CHECK(table().canonicalID(Builtins::MapPrototypeSymbolIterator) == table().canonicalID(Builtins::MapPrototypeEntries));
    AOT_TEST_CHECK(table().canonicalID(hostBuiltin("Int8ArrayPrototypeConstructor"_s)) == Builtins::Int8Array);
    AOT_TEST_CHECK(table().canonicalID(Builtins::ArrayPrototypePush) == Builtins::ArrayPrototypePush);
    AOT_TEST_CHECK(table().canonicalID(Builtins::MapPrototypeEntries) != table().canonicalID(Builtins::SetPrototypeValues));

    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(builtin.sameObjectAs != id && table().canonicalID(table().canonicalID(id)) == table().canonicalID(id));
        if (builtin.sameObjectAs)
            AOT_TEST_CHECK(builtin.kind == table().at(builtin.sameObjectAs).kind && !builtin.overloadCount);
    });
}

static void testHostEntries()
{
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::Bun, Builtins::BunServe, hostBuiltin("Process"_s), hostBuiltin("Buffer"_s), hostBuiltin("BufferFrom"_s), hostBuiltin("NodeFs"_s), hostBuiltin("NodeFsReadFile"_s), hostBuiltin("Request"_s), hostBuiltin("RequestPrototype"_s), hostBuiltin("Worker"_s) })
        AOT_TEST_CHECK(table().at(id).origin == BuiltinOrigin::Host);
    for (BuiltinID id : { Builtins::GlobalThis, Builtins::Math, Builtins::MathMax, Builtins::Array, Builtins::ArrayPrototypePush, Builtins::Promise, Builtins::Symbol })
        AOT_TEST_CHECK(table().at(id).origin == BuiltinOrigin::Engine);

    AOT_TEST_CHECK(table().findGlobal(hostName("Bun"_s)) == Builtins::Bun && table().at(Builtins::BunServe).holder == Builtins::Bun);
    AOT_TEST_CHECK(table().namespaceOf(hostModule("NodeFs"_s)) == hostBuiltin("NodeFs"_s) && table().isUnder(hostBuiltin("NodeFsReadFile"_s), hostBuiltin("NodeFs"_s)));
    AOT_TEST_CHECK(!table().isUnder(hostBuiltin("NodeFsReadFile"_s), Builtins::GlobalThis) && table().isUnder(Builtins::BunServe, Builtins::GlobalThis));
    forEachID<RuntimeModuleID>(table().moduleCount(), [&](RuntimeModuleID id) {
        AOT_TEST_CHECK(table().at(id).id == id && table().namespaceOf(id) == table().at(id).namespaceObject);
    });

    forEachBuiltin([&](BuiltinID id, const Builtin& builtin) {
        AOT_TEST_CHECK(!builtin.platforms.isEmpty());
        AOT_TEST_CHECK(!builtin.holder || table().at(builtin.holder).platforms.containsAll(builtin.platforms));
        AOT_TEST_CHECK(table().existsOn(id, BuiltinPlatform::Linux) == builtin.platforms.contains(BuiltinPlatform::Linux));
    });
}

static void testRuntimeFunctions()
{
    RuntimeFunctionID last;
    for (const RuntimeFunctionData& function : table().runtimeFunctions()) {
        AOT_TEST_CHECK(function.id && (!last || last < function.id));
        last = function.id;
        AOT_TEST_CHECK(table().contains(function.id) && &table().at(function.id) == &function);
        switch (function.role) {
        case RuntimeFunctionRole::EngineBuiltin:
            AOT_TEST_CHECK(function.id.index() < 0x10000);
            break;
        case RuntimeFunctionRole::HostBuiltin:
            AOT_TEST_CHECK(function.id.index() >= 0x10000 && function.id.index() < 0x20000);
            break;
        case RuntimeFunctionRole::CompilerFunction:
        case RuntimeFunctionRole::Model:
            AOT_TEST_CHECK(function.id.index() >= 0x20000);
            break;
        }
        for (BuiltinID implemented : function.implements)
            AOT_TEST_CHECK(table().contains(implemented));
    }
    AOT_TEST_CHECK(!table().contains(RuntimeFunctionID(last.index() + 1)));
    for (RuntimeFunctionID id : table().functionsCalledWithStartupLoads())
        AOT_TEST_CHECK(table().contains(id));
    for (RuntimeFunctionID id : table().moduleLoaderFunctions())
        AOT_TEST_CHECK(table().contains(id));
    for (RuntimeModuleID id : table().startupModules())
        AOT_TEST_CHECK(table().contains(id));
    AOT_TEST_CHECK(table().startupLoads().size() == table().startupModules().size());
    for (const BuiltinModuleLoad& load : table().startupLoads()) {
        AOT_TEST_CHECK(!load.builtin && !load.condition);
        AOT_TEST_CHECK(std::ranges::find(table().startupModules(), load.module) != table().startupModules().end());
        AOT_TEST_CHECK(load.readsEveryExport == load.exports.empty());
    }
    AOT_TEST_CHECK(!table().namesOfHostMethodsCallingStartupLoads().empty());
    for (BuiltinName name : table().namesOfHostMethodsCallingStartupLoads()) {
        bool isNameOfHostMember = false;
        forEachBuiltin([&](BuiltinID, const Builtin& builtin) {
            isNameOfHostMember |= builtin.name == name && builtin.origin == BuiltinOrigin::Host;
        });
        AOT_TEST_CHECK(isNameOfHostMember);
    }
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
        AOT_TEST_CHECK(table().hasLockedPrototypeLink(id) == (hasLockOfItsOwn || table().isFunctionOfEngine(id) || table().closesOnlyNamesOfCalls(id)));
        AOT_TEST_CHECK(table().closesIndexKeys(id) == (table().hasLockedPrototypeLink(id) && (builtin.kind == BuiltinKind::Prototype || builtin.flags.contains(BuiltinFlag::IsLockedObject))));
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
    for (BuiltinID id : std::initializer_list<BuiltinID> { Builtins::ObjectPrototype, Builtins::RegExpPrototype, Builtins::PromisePrototype, Builtins::GeneratorPrototype, hostBuiltin("RequestPrototype"_s), hostBuiltin("ResponsePrototype"_s), hostBuiltin("BlobPrototype"_s), hostBuiltin("URLPrototype"_s), hostBuiltin("URLSearchParamsPrototype"_s), hostBuiltin("HeadersPrototype"_s) })
        AOT_TEST_CHECK(table().at(id).flags.contains(BuiltinFlag::IsLockedObject));
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
    // Start-up and every refused write search the list, and start-up takes the members of one holder to lie side by side.
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
    // A native function of the engine passes on whoever looks for these, and keeps its prototype. A function that the library writes in JavaScript passes them on too. A constructor has the lock of its own.
    for (BuiltinID id : { Builtins::ObjectPrototypeHasOwnProperty, Builtins::ObjectPrototypeToString, Builtins::ArrayPrototypeSlice, Builtins::MathMax, Builtins::FunctionPrototypeBind }) {
        AOT_TEST_CHECK(table().isHostFunctionOfEngine(id) && table().hasLockedPrototypeLink(id));
        for (BuiltinName name : std::initializer_list<BuiltinName> { BuiltinNames::call, BuiltinNames::apply, BuiltinNames::bind, table().nameOf(WellKnownSymbol::HasInstance) })
            AOT_TEST_CHECK(table().isClosedKey(id, name));
        AOT_TEST_CHECK(!table().isClosedKey(id, BuiltinNames::toString) && !table().isClosedKey(id, BuiltinNames::then) && !table().isClosedKey(id, table().nameOf(WellKnownSymbol::ToPrimitive)));
    }
    for (BuiltinID id : { Builtins::ArrayPrototypeMap, Builtins::Error, Builtins::Math, Builtins::ArrayPrototype })
        AOT_TEST_CHECK(!table().isHostFunctionOfEngine(id));
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
    AOT_TEST_CHECK(table().at(BuiltinClasses::BoundFunction).hasClosureInstances && !table().at(BuiltinClasses::Function).hasClosureInstances);
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
    // A constructor without a lock of its own keeps what a call of it looks for, and takes every other name.
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

static bool readsKeys(Protocol protocol, std::initializer_list<BuiltinName> expected)
{
    std::span<const BuiltinName> keys = table().keysReadBy(protocol);
    return std::ranges::all_of(expected, [&](BuiltinName name) {
        return std::ranges::contains(keys, name);
    });
}

static bool hasKeys(Protocol protocol, std::initializer_list<BuiltinName> expected)
{
    return table().keysReadBy(protocol).size() == expected.size() && readsKeys(protocol, expected);
}

static void testProtocolKeys()
{
    AOT_TEST_CHECK(hasKeys(Protocol::ToPrimitive, { table().nameOf(WellKnownSymbol::ToPrimitive), BuiltinNames::valueOf, BuiltinNames::toString }));
    AOT_TEST_CHECK(readsKeys(Protocol::Thenable, { BuiltinNames::then }));
    AOT_TEST_CHECK(!readsKeys(Protocol::Thenable, { BuiltinNames::toJSON }) && !readsKeys(Protocol::Thenable, { BuiltinNames::valueOf }) && !readsKeys(Protocol::Thenable, { BuiltinNames::next }));
    AOT_TEST_CHECK(readsKeys(Protocol::Iterate, { table().nameOf(WellKnownSymbol::Iterator), BuiltinNames::next, nameOf("return"_s) }));
    AOT_TEST_CHECK(!readsKeys(Protocol::Iterate, { BuiltinNames::then }) && !readsKeys(Protocol::Iterate, { BuiltinNames::iterator }) && !readsKeys(Protocol::Iterate, { table().nameOf(WellKnownSymbol::Species) }));
    AOT_TEST_CHECK(hasKeys(Protocol::Species, { BuiltinNames::constructor, table().nameOf(WellKnownSymbol::Species) }));
    AOT_TEST_CHECK(hasKeys(Protocol::ToJSON, { BuiltinNames::toJSON }));
    AOT_TEST_CHECK(hasKeys(Protocol::HasInstance, { table().nameOf(WellKnownSymbol::HasInstance), BuiltinNames::prototype }));
    for (Protocol protocol : { Protocol::OwnKeys, Protocol::OwnValues, Protocol::PrototypeChain, Protocol::Named, Protocol::CallsNamed })
        AOT_TEST_CHECK(table().keysReadBy(protocol).empty());
}

static void testMembersAreReadOrCalled()
{
    std::span<const BuiltinOverload> toString = table().overloadsOf(Builtins::ArrayPrototypeToString);
    AOT_TEST_CHECK(toString.size() == 2 && toString[1].receiver);
    const BuiltinParameter& arrayLike = *toString[1].receiver;
    AOT_TEST_CHECK(arrayLike.protocols.containsAll({ Protocol::Named, Protocol::CallsNamed }));
    AOT_TEST_CHECK(arrayLike.calledMembers.size() == 1 && arrayLike.calledMembers[0] == BuiltinNames::join);
    AOT_TEST_CHECK(std::ranges::contains(arrayLike.namedMembers, BuiltinNames::length) && !std::ranges::contains(arrayLike.namedMembers, BuiltinNames::join));

    std::span<const BuiltinOverload> defineProperty = table().overloadsOf(Builtins::ObjectDefineProperty);
    AOT_TEST_CHECK(defineProperty.size() == 1 && defineProperty[0].parameters.size() == 3);
    const BuiltinParameter& descriptor = defineProperty[0].parameters[2];
    AOT_TEST_CHECK(descriptor.protocols.contains(Protocol::Named) && !descriptor.protocols.contains(Protocol::CallsNamed));
    AOT_TEST_CHECK(std::ranges::contains(descriptor.namedMembers, BuiltinNames::value) && descriptor.calledMembers.empty());
}

static void testProtocolsOfContents()
{
    std::span<const BuiltinOverload> join = table().overloadsOf(Builtins::ArrayPrototypeJoin);
    AOT_TEST_CHECK(join.size() == 2 && join[0].receiver && join[1].receiver);

    const BuiltinParameter& array = *join[0].receiver;
    AOT_TEST_CHECK(array.protocols == Protocols { Protocol::ToPrimitive });
    AOT_TEST_CHECK(array.uses.contains(OperandUse::AppliesToContents) && !array.uses.contains(OperandUse::AppliesTransitively));

    // An array-like also runs protocols of its own, and one word holds for all protocols of an operand.
    const BuiltinParameter& arrayLike = *join[1].receiver;
    AOT_TEST_CHECK(arrayLike.protocols.containsAll({ Protocol::ToPrimitive, Protocol::OwnValues, Protocol::Named }));
    AOT_TEST_CHECK(arrayLike.uses.contains(OperandUse::AppliesTransitively) && !arrayLike.uses.contains(OperandUse::AppliesToContents));
}

static void testAttributesReadByPasses()
{
    auto has = [](BuiltinID id, BuiltinFlag flag) {
        return table().at(id).flags.contains(flag);
    };
    // Nothing of Bun's makes a second realm that runs the compiled program in the VM of its caller: a Worker has a VM of its own. The column stays for a host that does.
    AOT_TEST_CHECK(!has(hostBuiltin("Worker"_s), BuiltinFlag::StartsInstance) && !has(Builtins::Map, BuiltinFlag::StartsInstance));
    AOT_TEST_CHECK(has(Builtins::Eval, BuiltinFlag::EvaluatesText) && has(Builtins::Function, BuiltinFlag::EvaluatesText) && !has(Builtins::ParseInt, BuiltinFlag::EvaluatesText));
    AOT_TEST_CHECK(has(Builtins::ArrayOf, BuiltinFlag::RequiresHolderAsThis) && has(Builtins::PromiseResolve, BuiltinFlag::RequiresHolderAsThis) && !has(Builtins::MathMax, BuiltinFlag::RequiresHolderAsThis));
    AOT_TEST_CHECK(has(Builtins::MathAbs, BuiltinFlag::IsDeterministic) && !has(Builtins::MathRandom, BuiltinFlag::IsDeterministic));
    AOT_TEST_CHECK(has(hostBuiltin("BunPlugin"_s), BuiltinFlag::RunsUnseenCode) && !has(Builtins::ProcessDlopen, BuiltinFlag::RunsUnseenCode) && !has(Builtins::MathAbs, BuiltinFlag::RunsUnseenCode));

    AOT_TEST_CHECK(table().at(Builtins::Math).kind == BuiltinKind::Namespace && table().at(Builtins::Map).kind == BuiltinKind::Constructor && table().at(Builtins::ParseInt).kind == BuiltinKind::Function);
    AOT_TEST_CHECK(table().at(Builtins::MapPrototype).kind == BuiltinKind::Prototype && table().at(Builtins::ArrayPrototypePush).kind == BuiltinKind::Method);
    AOT_TEST_CHECK(table().at(Builtins::ArrayBufferPrototypeByteLength).kind == BuiltinKind::Accessor && table().at(Builtins::MathPI).kind == BuiltinKind::Constant);

    AOT_TEST_CHECK(table().numberOf(Builtins::MathPI) == std::numbers::pi && !table().numberOf(Builtins::MathMax) && !table().numberOf(Builtins::Math));
    AOT_TEST_CHECK(table().at(Builtins::MapPrototype).prototypeLink == BuiltinPrototypeLink::Builtin && table().at(Builtins::MapPrototype).inheritsFrom == Builtins::ObjectPrototype);
    AOT_TEST_CHECK(table().at(Builtins::ObjectPrototype).prototypeLink == BuiltinPrototypeLink::Null && !table().at(Builtins::ObjectPrototype).inheritsFrom);
    AOT_TEST_CHECK(table().at(Builtins::Map).inheritsFrom == Builtins::FunctionPrototype && table().at(Builtins::Map).instanceClass == BuiltinClasses::Map);
}

static void testMathOverloads()
{
    for (BuiltinID id : { Builtins::MathAbs, Builtins::MathMax, Builtins::MathFloor, Builtins::MathRandom }) {
        AOT_TEST_CHECK(table().overloadsOf(id).size() == 1 && table().overloadsOf(id)[0].mode == BuiltinCallMode::Call);
        AOT_TEST_CHECK(table().overloadsOf(id)[0].effects.capability == BuiltinCapability::None);
    }

    const BuiltinOverload& abs = table().overloadsOf(Builtins::MathAbs)[0];
    AOT_TEST_CHECK(abs.result.rangeRule == BuiltinRangeRule::AbsoluteValue && abs.result.rangeOperand == BuiltinOperand::argument(0));
    AOT_TEST_CHECK(abs.effects.reads.isEmpty() && abs.effects.writes.isEmpty());

    const BuiltinOverload& max = table().overloadsOf(Builtins::MathMax)[0];
    AOT_TEST_CHECK(max.result.rangeRule == BuiltinRangeRule::MaximumOfArguments);
    AOT_TEST_CHECK(max.parameters.size() == 1 && max.parameters[0].mode == BuiltinParameterMode::Rest && max.parameters[0].conversion == BuiltinConversion::ToNumber);

    const BuiltinOverload& floor = table().overloadsOf(Builtins::MathFloor)[0];
    AOT_TEST_CHECK(floor.result.rangeRule == BuiltinRangeRule::IntegerPart && floor.result.rangeOperand == BuiltinOperand::argument(0));
    AOT_TEST_CHECK(floor.code.implementation.kind == BuiltinImplementationKind::Inline && floor.code.implementation.inlineBuiltin == InlineBuiltin::MathFloor);

    const BuiltinOverload& random = table().overloadsOf(Builtins::MathRandom)[0];
    AOT_TEST_CHECK(random.result.rangeRule == BuiltinRangeRule::None && random.parameters.empty());
    AOT_TEST_CHECK(random.effects.reads.contains(AbstractHeap::RuntimeState) && random.effects.writes.contains(AbstractHeap::RuntimeState));
}

static void testCompiledEntriesSayWhatACallEnters()
{
    const BuiltinImplementation& sort = table().overloadsOf(Builtins::ArrayPrototypeSort)[0].code.implementation;
    AOT_TEST_CHECK(sort.kind == BuiltinImplementationKind::Compiled && sort.callee == BuiltinCallee::HostFunction && entersNativeCode(sort));

    const BuiltinImplementation& map = table().overloadsOf(Builtins::ArrayPrototypeMap)[0].code.implementation;
    AOT_TEST_CHECK(map.kind == BuiltinImplementationKind::Compiled && map.callee == BuiltinCallee::Function && !entersNativeCode(map));
}

static void testSummariesRepeatTheOverloads()
{
    forEachBuiltin([&](BuiltinID id, const Builtin&) {
        std::span<const BuiltinOverload> overloads = table().overloadsOf(id);
        std::span<const BuiltinOverloadSummary> summaries = table().overloadSummariesOf(id);
        AOT_TEST_CHECK(overloads.size() == summaries.size());
        for (size_t position = 0; position < overloads.size(); ++position) {
            const BuiltinImplementation& implementation = overloads[position].code.implementation;
            const BuiltinOverloadSummary& summary = summaries[position];
            AOT_TEST_CHECK(summary.mode == overloads[position].mode && summary.kind == implementation.kind && summary.hasOtherwise == implementation.otherwise.has_value());
            AOT_TEST_CHECK(summary.callee == implementation.callee && summary.function == implementation.function);
        }
    });
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
    for (const BuiltinNumberConstant& record : attachments.numberConstants)
        AOT_TEST_CHECK(table().numberOf(record.builtin).has_value());

    AOT_TEST_CHECK(table().featureUsesOf(Builtins::MathMax).empty() && table().moduleLoadsOf(Builtins::MathMax).empty() && table().calledFunctionsOf(Builtins::MathMax).empty());
    AOT_TEST_CHECK(!table().unseenCodeOf(Builtins::MathMax) && !table().at(Builtins::MathMax).flags.contains(BuiltinFlag::RunsUnseenCode) && !table().boxedCallStubOf(Builtins::Math));
}

static void checkHostValue(const BuiltinPlace& place, const HashSet<uint32_t>& writtenCells)
{
    AOT_TEST_CHECK(!place.operand && !place.slot && place.memberPath.empty());
    switch (place.kind) {
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::NativeMemory:
        AOT_TEST_CHECK(!place.newValueType && !place.nativeCell);
        return;
    case BuiltinPlaceKind::NewValue:
        AOT_TEST_CHECK(table().contains(place.newValueType) && !table().at(place.newValueType).isNever() && !place.nativeCell);
        return;
    case BuiltinPlaceKind::NativeCell:
        AOT_TEST_CHECK(table().contains(place.nativeCell) && writtenCells.contains(place.nativeCell.index() + 1));
        return;
    default:
        AOT_TEST_CHECK(false);
    }
}

static void forEachHostUse(const BuiltinHostUses& uses, const Invocable<void(const BuiltinHostUse&)> auto& functor)
{
    AOT_TEST_CHECK(uses.areListed || uses.list.empty());
    for (const BuiltinHostUse& use : uses.list) {
        functor(use);
        if (use.call)
            forEachHostUse(use.call->result, functor);
    }
}

static void forEachWrittenHostUse(const Invocable<void(const BuiltinHostUse&)> auto& functor)
{
    const BuiltinTableAttachments& attachments = builtinTableData.attachments;
    for (std::span<const BuiltinModuleLoad> loads : { attachments.moduleLoads, attachments.startupLoads }) {
        for (const BuiltinModuleLoad& load : loads) {
            AOT_TEST_CHECK(load.usesOfExports.size() == load.exports.size());
            for (const BuiltinHostUses& uses : load.usesOfExports)
                forEachHostUse(uses, functor);
        }
    }
    for (std::span<const BuiltinCalledFunction> functions : { attachments.calledFunctions, table().startupCalledFunctions() }) {
        for (const BuiltinCalledFunction& called : functions)
            forEachHostUse(called.uses, functor);
    }
    for (const BuiltinHostObject& object : table().hostObjects()) {
        for (const BuiltinHostObjectInternalField& field : object.internalFields)
            forEachHostUse(field.uses, functor);
    }
    for (const BuiltinHostMethodCall& method : table().hostMethodCalls())
        forEachHostUse(method.call->result, functor);
}

static void testHostUsesNameWhatIsThere()
{
    HashSet<uint32_t> writtenCells;
    for (unsigned index = 0; index < table().overloadCount(); ++index) {
        const BuiltinOverload& overload = table().at(BuiltinOverloadID(index));
        for (const BuiltinTransfer& transfer : overload.flow.transfers) {
            if (transfer.to.kind == BuiltinPlaceKind::NativeCell)
                writtenCells.add(transfer.to.nativeCell.index() + 1);
        }
    }
    for (const BuiltinHostObject& object : table().hostObjects())
        AOT_TEST_CHECK(writtenCells.add(object.nativeCell.index() + 1).isNewEntry);
    forEachWrittenHostUse([&](const BuiltinHostUse& use) {
        if (use.use == BuiltinHostReadUse::Kept)
            writtenCells.add(use.nativeCell.index() + 1);
    });

    auto checkCall = [&](const BuiltinHostCall& call) {
        checkHostValue(call.thisValue, writtenCells);
        checkHostValue(call.otherArguments, writtenCells);
        AOT_TEST_CHECK(call.listsArguments || (call.arguments.empty() && call.otherArguments.kind == BuiltinPlaceKind::None));
        for (const BuiltinPlace& argument : call.arguments)
            checkHostValue(argument, writtenCells);
    };
    forEachWrittenHostUse([&](const BuiltinHostUse& use) {
        bool namesBuiltin = use.use == BuiltinHostReadUse::PassedTo || use.use == BuiltinHostReadUse::IsValueOf;
        AOT_TEST_CHECK(namesBuiltin == !!use.builtin && (!use.builtin || table().canonicalID(use.builtin) == use.builtin));
        AOT_TEST_CHECK((use.use == BuiltinHostReadUse::Kept) == !!use.nativeCell);
        AOT_TEST_CHECK(use.use == BuiltinHostReadUse::Called || !use.call);
        AOT_TEST_CHECK(use.use == BuiltinHostReadUse::PassedTo || !use.argumentIndex);
        for (BuiltinName name : use.memberPath)
            AOT_TEST_CHECK(table().contains(name));
        if (use.call)
            checkCall(*use.call);
    });

    for (const BuiltinHostObject& object : table().hostObjects()) {
        AOT_TEST_CHECK(!object.function || table().dispatchOf(object.function, BuiltinCallMode::Call));
        AOT_TEST_CHECK(!object.prototype || isObjectKind(table().at(object.prototype).kind));
        for (const BuiltinHostObjectField& field : object.fields) {
            // A record without a name speaks of every other name.
            AOT_TEST_CHECK((!field.name || table().contains(field.name)) && field.value.kind != BuiltinPlaceKind::None);
            checkHostValue(field.value, writtenCells);
        }
        // What keeps a string of whatever is stored holds strings from the start, under whatever name.
        if (object.keepsStringsOfStoredValues) {
            AOT_TEST_CHECK(!object.function && !object.fields.empty());
            for (const BuiltinHostObjectField& field : object.fields) {
                AOT_TEST_CHECK(field.value.kind == BuiltinPlaceKind::NewValue);
                const BuiltinType& held = table().at(field.value.newValueType);
                AOT_TEST_CHECK(held.primitives == OptionSet<BuiltinPrimitive> { BuiltinPrimitive::String } && held.flags.isEmpty() && held.parts.empty());
            }
        }
    }
    for (const BuiltinHostMethodCall& method : table().hostMethodCalls()) {
        AOT_TEST_CHECK(std::ranges::contains(table().namesOfHostMethodsCallingStartupLoads(), method.name));
        AOT_TEST_CHECK(writtenCells.contains(method.nativeCell.index() + 1));
        checkCall(*method.call);
    }
}

static const BuiltinHostCall* findCallOfOnlyFunction(BuiltinID builtin, size_t numberOfFunctions)
{
    std::span<const BuiltinCalledFunction> called = table().calledFunctionsOf(builtin);
    AOT_TEST_CHECK(called.size() == numberOfFunctions);
    const BuiltinHostUses& uses = called.back().uses;
    AOT_TEST_CHECK(uses.areListed && uses.list.size() == 1 && uses.list[0].use == BuiltinHostReadUse::Called && uses.list[0].memberPath.empty());
    return uses.list[0].call;
}

static void testHostUsesOfLibraryFunctions()
{
    // What initializeNextTickQueue answers is process.nextTick.
    const BuiltinHostCall* call = findCallOfOnlyFunction(hostBuiltin("ProcessNextTick"_s), 1);
    AOT_TEST_CHECK(call && call->times == BuiltinCallbackTimes::Once && call->listsArguments && call->arguments.size() == 3);
    AOT_TEST_CHECK(call->arguments[0].kind == BuiltinPlaceKind::NewValue && call->arguments[1].kind == BuiltinPlaceKind::NativeCell && call->arguments[2].kind == BuiltinPlaceKind::NativeCell);
    AOT_TEST_CHECK(call->result.areListed && call->result.list.size() == 1);
    AOT_TEST_CHECK(call->result.list[0].use == BuiltinHostReadUse::IsValueOf && call->result.list[0].builtin == hostBuiltin("ProcessNextTick"_s));

    // Whoever reads the property only has it made. A native that queues with it also calls it, and drops what it answers: for a warning with a function of its own and the warning.
    AOT_TEST_CHECK(std::ranges::any_of(table().calledFunctionsOf(hostBuiltin("ProcessEmitWarning"_s)), [](const BuiltinCalledFunction& called) {
        if (!called.uses.areListed || called.uses.list.size() != 1 || called.uses.list[0].use != BuiltinHostReadUse::Called)
            return false;
        const BuiltinHostUses& answer = called.uses.list[0].call->result;
        if (!answer.areListed || answer.list.size() < 2 || answer.list[0].use != BuiltinHostReadUse::IsValueOf || answer.list[0].builtin != hostBuiltin("ProcessNextTick"_s))
            return false;
        return std::ranges::all_of(answer.list.subspan(1), [](const BuiltinHostUse& use) {
            return use.use == BuiltinHostReadUse::Called && use.call->result.areListed && use.call->result.list.empty()
                && use.call->listsArguments && use.call->arguments.size() == 2 && std::ranges::all_of(use.call->arguments, [](const BuiltinPlace& argument) {
                    return argument.kind == BuiltinPlaceKind::NativeCell;
                });
        });
    }));

    // The queue is an object of native code, and what the library stores into its third internal field is called without operands.
    auto queue = std::ranges::find(table().hostObjects(), call->arguments[1].nativeCell, &BuiltinHostObject::nativeCell);
    AOT_TEST_CHECK(queue != table().hostObjects().end() && !queue->function && !queue->prototype && queue->fields.empty() && queue->internalFields.size() == 1);
    AOT_TEST_CHECK(queue->internalFields[0].index == 2 && queue->internalFields[0].uses.list.size() == 1);
    const BuiltinHostCall* drain = queue->internalFields[0].uses.list[0].call;
    AOT_TEST_CHECK(drain && drain->listsArguments && drain->arguments.empty() && drain->result.areListed && drain->result.list.empty());
    auto drainMicrotasks = std::ranges::find(table().hostObjects(), call->arguments[2].nativeCell, &BuiltinHostObject::nativeCell);
    AOT_TEST_CHECK(drainMicrotasks != table().hostObjects().end() && drainMicrotasks->function && table().at(drainMicrotasks->function).kind == BuiltinKind::Function && table().at(drainMicrotasks->function).holder == Builtins::NativeFunctions);

    // The stream is the first element of what getStdioWriteStream answers.
    for (BuiltinID stream : std::initializer_list<BuiltinID> { hostBuiltin("ProcessStdout"_s), hostBuiltin("ProcessStderr"_s) }) {
        const BuiltinHostCall* creation = findCallOfOnlyFunction(stream, 2);
        AOT_TEST_CHECK(creation && creation->arguments.size() == 4 && creation->result.list.size() == (stream == hostBuiltin("ProcessStdout"_s) ? 2 : 4));
        const BuiltinHostUse& first = creation->result.list[0];
        AOT_TEST_CHECK(first.use == BuiltinHostReadUse::IsValueOf && first.builtin == stream && first.memberPath.size() == 1 && first.memberPath[0] == nameOf("0"_s));
        const BuiltinHostUse& last = creation->result.list.back();
        AOT_TEST_CHECK(last.use == BuiltinHostReadUse::Inspected && last.memberPath.size() == 1 && last.memberPath[0] == nameOf("1"_s));
    }

    // Whoever prints a warning calls `write` of the stream, with the stream as the receiver, and drops what that answers.
    std::span<const BuiltinHostUse> usesOfStandardError = findCallOfOnlyFunction(hostBuiltin("ProcessStderr"_s), 2)->result.list;
    const BuiltinHostUse& write = usesOfStandardError[2];
    AOT_TEST_CHECK(usesOfStandardError[1].use == BuiltinHostReadUse::Kept && write.use == BuiltinHostReadUse::Called);
    AOT_TEST_CHECK(write.memberPath.size() == 2 && write.memberPath[0] == nameOf("0"_s) && write.memberPath[1] == nameOf("write"_s));
    AOT_TEST_CHECK(write.call->thisValue.nativeCell == usesOfStandardError[1].nativeCell && write.call->arguments.size() == 1 && write.call->result.areListed && write.call->result.list.empty());

    // A row that says nothing leaves everything open.
    for (const BuiltinCalledFunction& called : table().calledFunctionsOf(hostBuiltin("BunBuild"_s)))
        AOT_TEST_CHECK(!called.uses.areListed && called.uses.list.empty());
    AOT_TEST_CHECK(!table().calledFunctionsOf(hostBuiltin("BunBuild"_s)).empty());

    // The object of a function may itself be what an entry holds.
    std::span<const BuiltinCalledFunction> functionsOfPeek = table().calledFunctionsOf(hostBuiltin("BunPeek"_s));
    AOT_TEST_CHECK(functionsOfPeek.size() == 2 && functionsOfPeek[0].uses.list.size() == 1);
    AOT_TEST_CHECK(functionsOfPeek[0].uses.list[0].use == BuiltinHostReadUse::IsValueOf && functionsOfPeek[0].uses.list[0].builtin == hostBuiltin("BunPeek"_s) && !functionsOfPeek[0].uses.list[0].call);
}

static void testHostUsesOfStartupLoads()
{
    std::span<const BuiltinCalledFunction> functions = table().startupCalledFunctions();
    AOT_TEST_CHECK(std::ranges::equal(functions, table().functionsCalledWithStartupLoads(), { }, &BuiltinCalledFunction::function));
    for (const BuiltinCalledFunction& called : functions)
        AOT_TEST_CHECK(!called.builtin && !called.condition);

    // `inspect` stays with native code, which passes it on as the third operand of a custom inspect method.
    AOT_TEST_CHECK(table().startupLoads().size() == 1 && table().startupLoads()[0].exports.size() == 1);
    const BuiltinHostUses& usesOfInspect = table().startupLoads()[0].usesOfExports[0];
    AOT_TEST_CHECK(usesOfInspect.areListed && usesOfInspect.list.size() == 1 && usesOfInspect.list[0].use == BuiltinHostReadUse::Kept);
    BuiltinName inspectCell = usesOfInspect.list[0].nativeCell;

    BuiltinName customInspect = table().namesOfHostMethodsCallingStartupLoads()[0];
    const BuiltinNamedMemberCall* call = table().callOfMemberNamed(customInspect);
    AOT_TEST_CHECK(call && call->arguments.size() == 3 && call->arguments[2].kind == BuiltinPlaceKind::NativeCell && call->arguments[2].nativeCell == inspectCell);
    // The formatter passes nothing to toJSON. No record speaks of a member that native code calls with operands nobody has listed.
    const BuiltinNamedMemberCall* callOfToJSON = table().callOfMemberNamed(BuiltinNames::toJSON);
    AOT_TEST_CHECK(callOfToJSON && callOfToJSON->arguments.empty());
    AOT_TEST_CHECK(!table().callOfMemberNamed(BuiltinNames::then));

    // The options of that call hold one of the two functions under `stylize`: what one function of the library answers, and another function of the library itself.
    auto options = std::ranges::find(table().hostObjects(), call->arguments[1].nativeCell, &BuiltinHostObject::nativeCell);
    AOT_TEST_CHECK(options != table().hostObjects().end() && options->prototype == Builtins::ObjectPrototype && options->fields.size() == 4);
    HashSet<uint32_t> cellsOfStylize;
    for (const BuiltinHostObjectField& field : options->fields) {
        if (field.name == nameOf("stylize"_s))
            cellsOfStylize.add(field.value.nativeCell.index() + 1);
    }
    AOT_TEST_CHECK(cellsOfStylize.size() == 2 && std::ranges::count(functions, true, [](const BuiltinCalledFunction& called) { return called.uses.areListed; }) == 2);
    for (const BuiltinCalledFunction& called : functions) {
        // A row that says nothing leaves everything open.
        if (!called.uses.areListed)
            continue;
        AOT_TEST_CHECK(called.uses.list.size() == 1);
        const BuiltinHostUse& use = called.uses.list[0];
        const BuiltinHostUse& kept = use.call ? use.call->result.list[0] : use;
        AOT_TEST_CHECK(kept.use == BuiltinHostReadUse::Kept && cellsOfStylize.contains(kept.nativeCell.index() + 1));
        AOT_TEST_CHECK(!use.call || (use.call->arguments.size() == 1 && use.call->arguments[0].nativeCell == inspectCell));
    }

    // A method by which a class of the host prints itself calls `inspect`, and one of them calls `stylize` of the options.
    AOT_TEST_CHECK(table().hostMethodCalls().size() == 3);
    for (const BuiltinHostMethodCall& method : table().hostMethodCalls()) {
        AOT_TEST_CHECK(method.name == customInspect && method.call->arguments.size() == 2);
        AOT_TEST_CHECK(method.nativeCell == inspectCell || (cellsOfStylize.contains(method.nativeCell.index() + 1) && method.call->thisValue.nativeCell == options->nativeCell && !method.call->result.areListed));
    }
}

static bool namesPlainData(BuiltinTypeID id)
{
    if (!id)
        return false;
    const BuiltinType& type = table().at(id);
    return type.flags.contains(BuiltinTypeFlag::HasPlainData) || std::ranges::any_of(type.parts, [](const BuiltinTypePart& part) {
        return std::ranges::any_of(part.contents, [](const BuiltinSlotContents& held) {
            return namesPlainData(held.type);
        });
    });
}

// Plain data describes what a call makes. An overload that asked it of an operand would claim that a call with an object literal runs no getter.
static void testNoOperandIsTestedForPlainData()
{
    auto isTestedForPlainData = [](const BuiltinParameter& parameter) {
        return namesPlainData(parameter.accepted) || namesPlainData(parameter.converted) || namesPlainData(parameter.refused);
    };
    unsigned numberOfPlainDataResults = 0;
    forEachID<BuiltinOverloadID>(table().overloadCount(), [&](BuiltinOverloadID id) {
        const BuiltinOverload& overload = table().at(id);
        AOT_TEST_CHECK(!overload.receiver || !isTestedForPlainData(*overload.receiver));
        AOT_TEST_CHECK(std::ranges::none_of(overload.parameters, isTestedForPlainData));
        numberOfPlainDataResults += namesPlainData(overload.result.type);
    });
    AOT_TEST_CHECK(numberOfPlainDataResults);
}

static void testNoObjectIsProvenToBePlainData()
{
    using namespace Typed;
    TestProgram test;
    Layout record;
    record.kind = LayoutKind::Record;
    test.program.layouts.append(WTF::move(record));
    LayoutID literal(test.program.layouts.size() - 1);
    TypeArena arena(test.program.types);
    Function function;
    TypingContext typing { test.program, function, arena };

    BuiltinTypeID parsed = table().overloadsOf(Builtins::JSONParse)[0].result.type;
    AOT_TEST_CHECK(table().at(parsed).flags.contains(BuiltinTypeFlag::HasPlainData) && !table().at(parsed).flags.contains(BuiltinTypeFlag::HasAnyObject));
    auto fitIn = [&](const Type& operand) {
        return fitOf(typing, arena.intern(operand), parsed).fit;
    };
    AOT_TEST_CHECK(fitIn(Type::forLayout(literal, arena)) == TypeFit::Unknown);
    AOT_TEST_CHECK(fitIn(Type::builtinClass(BuiltinClasses::Array)) == TypeFit::Unknown && fitIn(Type::builtinClass(BuiltinClasses::Object)) == TypeFit::Unknown);
    AOT_TEST_CHECK(fitIn(Type::object()) == TypeFit::Unknown && fitIn(Type::builtinClass(BuiltinClasses::Map)) == TypeFit::Outside);
    AOT_TEST_CHECK(fitIn(Type::fromPrimitives(Primitive::String)) == TypeFit::Inside && fitIn(Type::fromPrimitives(Primitive::Symbol)) == TypeFit::Outside);
}

std::span<const NativeTest> builtinTableTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-table/id-types"_s, testIDTypes },
        { "builtin-table/counts-agree-with-generated-ids"_s, testCountsAgreeWithGeneratedIDs },
        { "builtin-table/describes-itself"_s, testTableDescribesItself },
        { "builtin-table/named-lists-translate"_s, testNamedListsTranslate },
        { "builtin-table/marks"_s, testMarks },
        { "builtin-table/global-this-is-first"_s, testGlobalThisIsFirst },
        { "builtin-table/members-are-one-run-in-name-order"_s, testMembersAreOneRunInNameOrder },
        { "builtin-table/hidden-roots-ascend-by-path"_s, testHiddenRootsAscendByPath },
        { "builtin-table/names-are-three-runs"_s, testNamesAreThreeRuns },
        { "builtin-table/symbol-names"_s, testSymbolNames },
        { "builtin-table/names-of-keys"_s, testNamesOfKeys },
        { "builtin-table/function-names-follow-keys"_s, testFunctionNamesFollowKeys },
        { "builtin-table/tokens-round-trip"_s, testTokensRoundTrip },
        { "builtin-table/unknown-tokens"_s, testUnknownTokens },
        { "builtin-table/overloads-are-one-run-in-mode-order"_s, testOverloadsAreOneRunInModeOrder },
        { "builtin-table/dispatch-is-total"_s, testDispatchIsTotal },
        { "builtin-table/undeclared-overload-claims-nothing"_s, testUndeclaredOverloadClaimsNothing },
        { "builtin-table/constructors-that-throw-nothing-take-few-arguments"_s, testConstructorsThatThrowNothingTakeFewArguments },
        { "builtin-table/parameter-for"_s, testParameterFor },
        { "builtin-table/classes-and-slots"_s, testClassesAndSlots },
        { "builtin-table/class-relations"_s, testClassRelations },
        { "builtin-table/member-lookup"_s, testMemberLookup },
        { "builtin-table/member-missing-on-target-is-not-found"_s, testMemberMissingOnTargetIsNotFound },
        { "builtin-table/canonical-id"_s, testCanonicalID },
        { "builtin-table/host-entries"_s, testHostEntries },
        { "builtin-table/runtime-functions"_s, testRuntimeFunctions },
        { "builtin-table/mutability-follows-the-lock"_s, testMutabilityFollowsTheLock },
        { "builtin-table/locked-objects"_s, testLockedObjects },
        { "builtin-table/locked-members"_s, testLockedMembers },
        { "builtin-table/protocol-keys"_s, testProtocolKeys },
        { "builtin-table/members-are-read-or-called"_s, testMembersAreReadOrCalled },
        { "builtin-table/protocols-of-contents"_s, testProtocolsOfContents },
        { "builtin-table/attributes-read-by-passes"_s, testAttributesReadByPasses },
        { "builtin-table/math-overloads"_s, testMathOverloads },
        { "builtin-table/compiled-entries-say-what-a-call-enters"_s, testCompiledEntriesSayWhatACallEnters },
        { "builtin-table/summaries-repeat-the-overloads"_s, testSummariesRepeatTheOverloads },
        { "builtin-table/string-constants-follow-the-target"_s, testStringConstantsFollowTheTarget },
        { "builtin-table/attachments-are-found-by-entry"_s, testAttachmentsAreFoundByEntry },
        { "builtin-table/host-uses-name-what-is-there"_s, testHostUsesNameWhatIsThere },
        { "builtin-table/host-uses-of-library-functions"_s, testHostUsesOfLibraryFunctions },
        { "builtin-table/host-uses-of-startup-loads"_s, testHostUsesOfStartupLoads },
        { "builtin-table/no-operand-is-tested-for-plain-data"_s, testNoOperandIsTestedForPlainData },
        { "builtin-table/no-object-is-proven-to-be-plain-data"_s, testNoObjectIsProvenToBePlainData },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
