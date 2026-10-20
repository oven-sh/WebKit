/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTEffects.h"
#include "AOTIRTestSupport.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTTypeArena.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!ValueID() && !ValueID(nullptr) && ValueID(0u) && !ValueID(UINT32_MAX));
static_assert(ValueID(size_t { 7 }).index() == 7 && ValueID(1u) < ValueID(2u));
static_assert(!std::is_convertible_v<uint32_t, ValueID> && !std::is_convertible_v<ValueID, BlockID>);
template<typename T> concept AssignableFromLiteralZero = requires(T id) { id = 0; };
static_assert(!AssignableFromLiteralZero<ValueID>);
static_assert(sizeof(ValueID) == 4 && sizeof(Arguments) == 8 && sizeof(Value) <= 72);

namespace {

const PayloadFieldInfo& fieldNamed(Opcode opcode, ASCIILiteral name)
{
    std::span<const PayloadFieldInfo> fields = payloadFieldsOf(opcode);
    auto field = std::ranges::find(fields, name, &PayloadFieldInfo::name);
    AOT_TEST_CHECK(field != fields.end());
    return *field;
}

Terminator createTerminator(TerminatorKind kind, unsigned numberOfTargets)
{
    Terminator terminator(kind);
    terminator.targets.grow(numberOfTargets);
    return terminator;
}

DeclaredType createDeclaredType(TypeID type, TypeID predicted = { })
{
    DeclaredType declared;
    declared.type = type;
    declared.predicted = predicted;
    return declared;
}

CreatorRecord::Coroutine createCoroutineRecord(CoroutineKind kind, TypeID promiseType = { })
{
    CreatorRecord::Coroutine record;
    record.kind = kind;
    record.promiseType = promiseType;
    return record;
}

Function createFunctionWithRole(FunctionRole role, CreatorRecord&& creator = { })
{
    Function function;
    function.role = role;
    function.creator = WTF::move(creator);
    function.thisType = createDeclaredType(TypeTable::anything);
    function.results = { createDeclaredType(TypeTable::anything) };
    function.capabilities = initialCapabilitiesOf(function);
    return function;
}

bool isAt(Wire::SourceLocation location, ModuleID module, uint32_t position)
{
    return location.module == module.index() && location.position == position;
}

ASCIILiteral nameOfTarget(const Terminator& terminator, uint32_t targetIndex)
{
    const TargetInfo* info = infoOfTarget(terminator.kind, targetIndex, static_cast<uint32_t>(terminator.targets.size()));
    AOT_TEST_CHECK(info);
    return info->name;
}

} // anonymous namespace

static void compactListAppendInsertRemove()
{
    CompactList<uint32_t> list;
    list.append(2);
    list.append(3);
    list.insert(0, 1);
    list.insert(list.size(), 4);
    list.append(std::array<uint32_t, 2> { 5, 6 });
    AOT_TEST_CHECK(hasElements(list, { 1u, 2u, 3u, 4u, 5u, 6u }));
    list.removeAt(1, 2);
    AOT_TEST_CHECK(hasElements(list, { 1u, 4u, 5u, 6u }));
    AOT_TEST_CHECK(list.takeLast() == 6);
    AOT_TEST_CHECK(hasElements(list, { 1u, 4u, 5u }));
    AOT_TEST_CHECK(list.find(5) == 2 && list.find(2) == notFound);
}

static void compactListAppendOwnSpan()
{
    CompactList<uint32_t> list { 1, 2, 3 };
    AOT_TEST_CHECK(list.capacity() == list.size());
    list.append(list.span());
    AOT_TEST_CHECK(hasElements(list, { 1u, 2u, 3u, 1u, 2u, 3u }));
}

static void compactListCopyAndMove()
{
    CompactList<uint32_t> original { 1, 2 };
    CompactList<uint32_t> copy = original;
    copy.append(3);
    AOT_TEST_CHECK(hasElements(original, { 1u, 2u }) && hasElements(copy, { 1u, 2u, 3u }));
    CompactList<uint32_t> moved = WTF::move(original);
    AOT_TEST_CHECK(hasElements(moved, { 1u, 2u }) && original.isEmpty());
    original.append(9);
    AOT_TEST_CHECK(hasElements(moved, { 1u, 2u }) && hasElements(original, { 9u }));
}

static void compactListRemoveAtEnd()
{
    CompactList<uint32_t> list { 1, 2 };
    list.removeAt(list.size(), 0);
    AOT_TEST_CHECK(hasElements(list, { 1u, 2u }));
    list.removeAt(list.size() - 1);
    AOT_TEST_CHECK(hasElements(list, { 1u }));
}

static void compactListEmptyOperationsLeaveSharedHeader()
{
    auto runOperations = [](auto list) {
        list.shrink(0);
        list.resize(0);
        list.clear();
        list.removeAt(0, 0);
        list.shrinkToFit();
        list.removeAll({ });
        list.reverse();
    };
    runOperations(CompactList<uint32_t>());
    runOperations(CompactList<int64_t>());
    runOperations(Arguments());
    CompactList<uint32_t> list;
    AOT_TEST_CHECK(!list.size() && !list.capacity());
}

static void effectsDumpUsesSchemaGrammar()
{
    AOT_TEST_CHECK(WTF::toString(Effects::none()) == "none"_s);
    AOT_TEST_CHECK(WTF::toString(Effects::unknownCode()) == "unknownCode"_s);
    AOT_TEST_CHECK(WTF::toString(Effects()) == "unknownCode suspends"_s);
    Effects load = Effects::none();
    load.reads = AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::ObjectFields };
    AOT_TEST_CHECK(WTF::toString(load) == "reads(ObjectFields, ShapeWords)"_s);
    Effects allocation = Effects::none();
    allocation.writes = AbstractHeapSet { AbstractHeap::RuntimeState };
    allocation.mayCollect = true;
    AOT_TEST_CHECK(WTF::toString(allocation) == "writes(RuntimeState) collects"_s);
    Effects store = Effects::none();
    store.reads = LocationMasks::top();
    store.writes = AbstractHeapSet { AbstractHeap::ModuleVariables };
    store.mayThrow = true;
    AOT_TEST_CHECK(WTF::toString(store) == "reads(all) writes(ModuleVariables) throws"_s);
    Effects withoutCollection = Effects::unknownCode();
    withoutCollection.mayCollect = false;
    AOT_TEST_CHECK(WTF::toString(withoutCollection) == "reads(all) writes(all) throws callsUnknownCode terminates"_s);
}

static void effectsDumpCountsLocationsOfPartialHeap()
{
    Effects load = Effects::none();
    load.reads.add(AbstractHeap::ObjectFields, 0b101);
    load.reads.add(AbstractHeap::AllocatorState);
    AOT_TEST_CHECK(WTF::toString(load) == "reads(ObjectFields/2, AllocatorState)"_s);
    load.reads.add(AbstractHeap::ObjectFields);
    AOT_TEST_CHECK(WTF::toString(load) == "reads(ObjectFields, AllocatorState)"_s);
}

static void effectsOnEdgesDumpShowsHandlerThatDiffers()
{
    Effects store = Effects::none();
    store.writes = AbstractHeapSet { AbstractHeap::ModuleVariables };
    store.mayThrow = true;
    AOT_TEST_CHECK(WTF::toString(EffectsOnEdges::onBothEdges(store)) == "writes(ModuleVariables) throws"_s);
    AOT_TEST_CHECK(WTF::toString(EffectsOnEdges { store, Effects::none() }) == "writes(ModuleVariables) throws | on throw: none"_s);
    AOT_TEST_CHECK(WTF::toString(EffectsOnEdges::none()) == "none"_s);
}

static void abstractHeapSetIterationAscends()
{
    Vector<AbstractHeap> visited;
    for (AbstractHeap heap : AbstractHeapSet())
        visited.append(heap);
    AOT_TEST_CHECK(visited.isEmpty());
    AbstractHeapSet heaps { AbstractHeap::RuntimeState, AbstractHeap::ShapeWords, AbstractHeap::ModuleVariables };
    for (AbstractHeap heap : heaps)
        visited.append(heap);
    AOT_TEST_CHECK(hasElements(visited, { AbstractHeap::ModuleVariables, AbstractHeap::ShapeWords, AbstractHeap::RuntimeState }));

    visited.clear();
    LocationMasks(heaps).forEach([&](AbstractHeap heap, uint64_t mask) {
        AOT_TEST_CHECK(mask == LocationMasks::entireHeap);
        visited.append(heap);
    });
    AOT_TEST_CHECK(hasElements(visited, { AbstractHeap::ModuleVariables, AbstractHeap::ShapeWords, AbstractHeap::RuntimeState }));
}

static void parseEnumFindsEveryOpcode()
{
    for (Opcode opcode : allMembersOf<Opcode>)
        AOT_TEST_CHECK(parseEnum<Opcode>(nameOf(opcode)) == opcode);
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        AOT_TEST_CHECK(parseEnum<TerminatorKind>(nameOf(kind)) == kind);
}

static void parseEnumRejectsUnknownNames()
{
    AOT_TEST_CHECK(parseEnum<Opcode>("Call"_s) == Opcode::Call);
    for (ASCIILiteral name : { "call"_s, ""_s, "Callx"_s, "Cal"_s, "Jump"_s })
        AOT_TEST_CHECK(!parseEnum<Opcode>(name));
    AOT_TEST_CHECK(parseEnum<TerminatorKind>("Jump"_s) == TerminatorKind::Jump && !parseEnum<TerminatorKind>("Call"_s));
}

static void valueOperandsOfCall()
{
    Value call(Opcode::Call);
    AOT_TEST_CHECK(call.numberOfNamedOperands() == 2 && call.numberOfOperands() == 2 && !call.numberOfVariadicOperands());
    for (uint32_t index = 0; index < 2; ++index)
        call.setOperand(index, ValueID(10 + index));
    for (uint32_t index = 2; index < 4; ++index)
        call.appendVariadicOperand(ValueID(10 + index));
    AOT_TEST_CHECK(call.numberOfOperands() == 4 && call.numberOfVariadicOperands() == 2);
    AOT_TEST_CHECK(call.operand(2) == ValueID(12u) && call.operand(3) == ValueID(13u));
    AOT_TEST_CHECK(hasElements(call.children, { ValueID(10u), ValueID(11u), ValueID() }));
    AOT_TEST_CHECK(hasElements(call.arguments, { ValueID(12u), ValueID(13u) }) && hasElements(call.variadicOperands(), { ValueID(12u), ValueID(13u) }));
    AOT_TEST_CHECK(!call.variadicOperandIndex(1) && call.variadicOperandIndex(2) == 0u && call.variadicOperandIndex(3) == 1u);
    call.setOperand(3, ValueID(23u));
    AOT_TEST_CHECK(hasElements(call.arguments, { ValueID(12u), ValueID(23u) }));
}

static void valueAbsentOperandKeepsIndex()
{
    Value call(Opcode::Call);
    call.setOperand(0, ValueID(10u));
    call.appendVariadicOperand(ValueID(12u));
    call.appendVariadicOperand(ValueID(13u));
    Vector<uint32_t> indexes;
    std::as_const(call).forEachOperand([&](ValueID operand, uint32_t index) {
        AOT_TEST_CHECK(operand == ValueID(10 + index));
        indexes.append(index);
    });
    AOT_TEST_CHECK(hasElements(indexes, { 0u, 2u, 3u }));
    AOT_TEST_CHECK(call.numberOfOperands() == 4 && !call.operand(1));
}

static void valueFourthNamedOperandIsInArguments()
{
    Value store(Opcode::SuperSet);
    AOT_TEST_CHECK(store.numberOfOperands() == 4 && hasElements(store.arguments, { ValueID() }));
    store.setOperand(3, ValueID(7u));
    AOT_TEST_CHECK(store.operand(3) == ValueID(7u) && hasElements(store.arguments, { ValueID(7u) }));
    AOT_TEST_CHECK(hasElements(store.children, { ValueID(), ValueID(), ValueID() }));
    AOT_TEST_CHECK(!store.numberOfVariadicOperands() && store.variadicOperands().empty());
}

static void valueVariadicOperandsFollowFiveNamedOperands()
{
    Value value(Opcode::DefineOwnProperty);
    AOT_TEST_CHECK(value.numberOfNamedOperands() == 5 && value.numberOfOperands() == 5 && hasElements(value.arguments, { ValueID(), ValueID() }));
    for (uint32_t index = 0; index < 5; ++index)
        value.setOperand(index, ValueID(10 + index));
    AOT_TEST_CHECK(hasElements(value.children, { ValueID(10u), ValueID(11u), ValueID(12u) }) && hasElements(value.arguments, { ValueID(13u), ValueID(14u) }));
    AOT_TEST_CHECK(!value.numberOfVariadicOperands() && !value.variadicOperandIndex(4));

    for (uint32_t index = 5; index < 8; ++index)
        value.appendVariadicOperand(ValueID(10 + index));
    AOT_TEST_CHECK(value.numberOfOperands() == 8 && value.numberOfVariadicOperands() == 3 && value.variadicOperandIndex(5) == 0u);
    AOT_TEST_CHECK(hasElements(value.variadicOperands(), { ValueID(15u), ValueID(16u), ValueID(17u) }));
    uint32_t nextIndex = 0;
    value.forEachOperand([&](ValueID& operand, uint32_t index) {
        AOT_TEST_CHECK(index == nextIndex++ && operand == ValueID(10 + index) && value.operand(index) == operand);
    });
    AOT_TEST_CHECK(nextIndex == 8);

    value.setVariadicOperands(value.variadicOperands().subspan(1));
    AOT_TEST_CHECK(hasElements(value.arguments, { ValueID(13u), ValueID(14u), ValueID(16u), ValueID(17u) }));
    value.removeVariadicOperand(0);
    AOT_TEST_CHECK(hasElements(value.arguments, { ValueID(13u), ValueID(14u), ValueID(17u) }) && value.operand(5) == ValueID(17u));
    value.setVariadicOperands({ });
    AOT_TEST_CHECK(value.numberOfOperands() == 5 && hasElements(value.arguments, { ValueID(13u), ValueID(14u) }));
}

static void valueRemoveVariadicOperandKeepsNamedOperands()
{
    Value call(Opcode::Call);
    call.setOperand(0, ValueID(10u));
    call.setOperand(1, ValueID(11u));
    for (uint32_t index = 2; index < 5; ++index)
        call.appendVariadicOperand(ValueID(10 + index));
    call.removeVariadicOperand(1);
    AOT_TEST_CHECK(hasElements(call.variadicOperands(), { ValueID(12u), ValueID(14u) }));
    call.removeVariadicOperand(1);
    call.removeVariadicOperand(0);
    AOT_TEST_CHECK(call.numberOfOperands() == 2 && call.operand(0) == ValueID(10u) && call.operand(1) == ValueID(11u));
}

static void valueBecomeKeepsTypesOriginAndRepresentation()
{
    Value value(Opcode::NewArray);
    value.type = TypeTable::anything;
    value.setWritten(TypeTable::object);
    value.setPrediction(TypeTable::function);
    value.setOrigin({ 40, 2 });
    value.representation = Representation::Cell;
    value.flags = UINT16_MAX;
    value.setHoles(std::array<uint32_t, 2> { 1, 2 });
    value.setOperand(0, ValueID(10u));
    value.appendVariadicOperand(ValueID(11u));
    AOT_TEST_CHECK(!value.outOfLinePayload().empty());

    value.become(Opcode::Integer);
    AOT_TEST_CHECK(value.opcode == Opcode::Integer && !value.flags);
    AOT_TEST_CHECK(!value.numberOfOperands() && value.arguments.isEmpty() && hasElements(value.children, { ValueID(), ValueID(), ValueID() }));
    AOT_TEST_CHECK(value.outOfLinePayload().empty() && !value.integer());
    AOT_TEST_CHECK(value.type == TypeTable::anything && value.written() == TypeTable::object && value.prediction() == TypeTable::function);
    AOT_TEST_CHECK(value.position == 40 && value.inlineFrame == 2 && value.representation == Representation::Cell);
}

static void valueTypeLevelsFallBackToStrongerLevel()
{
    Value value(Opcode::Undefined);
    value.type = TypeTable::anything;
    AOT_TEST_CHECK(!value.written() && !value.prediction());
    AOT_TEST_CHECK(value.writtenOrType() == TypeTable::anything && value.predictionOrType() == TypeTable::anything);
    value.setWritten(TypeTable::object);
    AOT_TEST_CHECK(value.writtenOrType() == TypeTable::object && value.predictionOrType() == TypeTable::object);
    value.setPrediction(TypeTable::function);
    AOT_TEST_CHECK(value.writtenOrType() == TypeTable::object && value.predictionOrType() == TypeTable::function);
}

static void checkFixedWordsSurviveListPayload(Opcode opcode, ASCIILiteral listName, unsigned numberOfFixedWords)
{
    std::span<const PayloadFieldInfo> fields = payloadFieldsOf(opcode);
    const PayloadFieldInfo& list = fieldNamed(opcode, listName);
    AOT_TEST_CHECK(list.kind == PayloadKind::List && list.firstWord == numberOfFixedWords && numberOfFixedWords > numberOfInlinePayloadWords);
    size_t numberOfFixedOutOfLineWords = numberOfFixedWords - numberOfInlinePayloadWords;

    Value value(opcode);
    AOT_TEST_CHECK(value.outOfLinePayload().size() == numberOfFixedOutOfLineWords && value.payloadListFrom(list.firstWord).empty());
    for (unsigned word = 0; word < numberOfFixedWords; ++word)
        value.setPayloadWordAt(word, 100 + word);
    auto hasFixedWords = [&] {
        return std::ranges::all_of(fields, [&](const PayloadFieldInfo& field) {
            return field.kind == PayloadKind::List || value.payloadWordAt(field.firstWord) == 100u + field.firstWord;
        });
    };

    value.setPayloadListFrom(list.firstWord, std::array<uint32_t, 3> { 3, 1, 2 });
    AOT_TEST_CHECK(hasElements(value.payloadListFrom(list.firstWord), { 3u, 1u, 2u }));
    AOT_TEST_CHECK(value.outOfLinePayload().size() == numberOfFixedOutOfLineWords + 3 && hasFixedWords());
    value.setPayloadListFrom(list.firstWord, value.payloadListFrom(list.firstWord).subspan(1));
    AOT_TEST_CHECK(hasElements(value.payloadListFrom(list.firstWord), { 1u, 2u }));
    AOT_TEST_CHECK(value.outOfLinePayload().size() == numberOfFixedOutOfLineWords + 2 && hasFixedWords());
    value.mutablePayloadListFrom(list.firstWord)[0] = 9;
    AOT_TEST_CHECK(hasElements(value.payloadListFrom(list.firstWord), { 9u, 2u }) && hasFixedWords());
}

static void valueListPayloadFollowsFixedWords()
{
    checkFixedWordsSurviveListPayload(Opcode::NewArray, "holes"_s, 4);
    checkFixedWordsSurviveListPayload(Opcode::NewObject, "fields"_s, 5);

    Value array(Opcode::NewArray);
    array.setContents(TypeTable::number);
    array.setHoles(std::array<uint32_t, 2> { 0, 4 });
    AOT_TEST_CHECK(array.contents() == TypeTable::number && hasElements(array.holes(), { 0u, 4u }) && !array.allocationSite());
}

static void valueListPayloadStartsAfterInlineWords()
{
    Value copy(Opcode::CopyDataProperties);
    AOT_TEST_CHECK(fieldNamed(Opcode::CopyDataProperties, "excludedKeys"_s).firstWord == numberOfInlinePayloadWords);
    copy.setExcludedKeys(std::array<PropertyKeyID, 2> { PropertyKeyID(8u), PropertyKeyID(5u) });
    AOT_TEST_CHECK(hasElements(copy.excludedKeys(), { PropertyKeyID(8u), PropertyKeyID(5u) }) && copy.outOfLinePayload().size() == 2);
    AOT_TEST_CHECK(hasElements(copy.inlinePayload(), { absentPayloadWord, absentPayloadWord, absentPayloadWord }));
}

static void valueInt64PayloadHasTwoWords()
{
    const PayloadFieldInfo& integer = fieldNamed(Opcode::Integer, "integer"_s);
    AOT_TEST_CHECK(integer.kind == PayloadKind::Int64 && integer.numberOfWords == 2);
    Value value(Opcode::Integer);
    value.setPayloadWordAt(integer.firstWord, 0x55667788);
    AOT_TEST_CHECK(!value.payloadWordAt(integer.firstWord + 1));
    value.setPayloadWordAt(integer.firstWord + 1, 0x11223344);
    AOT_TEST_CHECK(value.integer() == 0x1122334455667788 && value.payloadWordPairAt(integer.firstWord) == 0x1122334455667788u);
    value.setInteger(-2);
    AOT_TEST_CHECK(value.payloadWordAt(integer.firstWord) == 0xfffffffeu && value.payloadWordAt(integer.firstWord + 1) == UINT32_MAX && value.integer() == -2);
}

static void valueSamePayloadComparesFlagsAndBothParts()
{
    Value first(Opcode::NewArray);
    Value second(Opcode::NewArray);
    second.type = TypeTable::object;
    second.appendVariadicOperand(ValueID(3u));
    AOT_TEST_CHECK(first.hasSamePayloadAs(second));
    second.setHoles(std::array<uint32_t, 1> { 0 });
    AOT_TEST_CHECK(!first.hasSamePayloadAs(second));
    first.setHoles(std::array<uint32_t, 1> { 0 });
    AOT_TEST_CHECK(first.hasSamePayloadAs(second));
    first.setContents(TypeTable::number);
    AOT_TEST_CHECK(!first.hasSamePayloadAs(second));
    second.setContents(TypeTable::number);
    second.flags = 1;
    AOT_TEST_CHECK(!first.hasSamePayloadAs(second));
}

static void valueForEachReferenceSkipsNull()
{
    Value test(Opcode::TypeTest);
    unsigned numberOfCalls = 0;
    forEachReference(test, [&](auto&) { ++numberOfCalls; });
    AOT_TEST_CHECK(!numberOfCalls);

    test.type = TypeTable::anything;
    test.setPrediction(TypeTable::boolean);
    test.setTestedType(TypeTable::string);
    forEachReference(test, [&]<typename Reference>(Reference& reference) {
        ++numberOfCalls;
        if constexpr (std::is_same_v<Reference, TypeID>) {
            AOT_TEST_CHECK(reference == TypeTable::string);
            reference = TypeTable::function;
        } else
            AOT_TEST_CHECK(false);
    });
    AOT_TEST_CHECK(numberOfCalls == 1 && test.testedType() == TypeTable::function);
    AOT_TEST_CHECK(test.type == TypeTable::anything && test.prediction() == TypeTable::boolean);

    Vector<TypeID> visited;
    forEachType(test, [&](TypeID id) { visited.append(id); });
    AOT_TEST_CHECK(hasElements(visited, { TypeTable::function }));
}

static void terminatorNamedTargets()
{
    Terminator jump = createTerminator(TerminatorKind::Jump, 1);
    AOT_TEST_CHECK(&jump.next() == &jump.targets[0] && nameOfTarget(jump, 0) == "next"_s);
    for (TerminatorKind kind : { TerminatorKind::Branch, TerminatorKind::TestAndBranch }) {
        Terminator branch = createTerminator(kind, 2);
        AOT_TEST_CHECK(branch.isConditionalBranch() && !branch.hasCases());
        AOT_TEST_CHECK(&branch.taken() == &branch.targets[0] && &branch.notTaken() == &branch.targets[1]);
        AOT_TEST_CHECK(nameOfTarget(branch, 0) == "taken"_s && nameOfTarget(branch, 1) == "notTaken"_s);
    }
    Terminator call = createTerminator(TerminatorKind::TryCallInline, 2);
    AOT_TEST_CHECK(&call.done() == &call.targets[0] && &call.miss() == &call.targets[1]);
    AOT_TEST_CHECK(nameOfTarget(call, 0) == "done"_s && nameOfTarget(call, 1) == "miss"_s);
    for (TerminatorKind kind : { TerminatorKind::Switch, TerminatorKind::SwitchString, TerminatorKind::SwitchOnShape }) {
        Terminator terminator = createTerminator(kind, 3);
        AOT_TEST_CHECK(terminator.hasCases() && !terminator.isConditionalBranch());
        AOT_TEST_CHECK(terminator.caseTargets().data() == &terminator.targets[0] && terminator.caseTargets().size() == 2 && &terminator.otherwise() == &terminator.targets[2]);
        AOT_TEST_CHECK(nameOfTarget(terminator, 0) == "case"_s && nameOfTarget(terminator, 1) == "case"_s && nameOfTarget(terminator, 2) == "otherwise"_s);
    }
    for (TerminatorKind kind : { TerminatorKind::Return, TerminatorKind::ReturnCall, TerminatorKind::Throw, TerminatorKind::Rethrow, TerminatorKind::Unreachable })
        AOT_TEST_CHECK(numberOfFixedTargetsOf(kind) == 0u && targetInfosOf(kind).empty() && !infoOfTarget(kind, 0, 1));
}

static void terminatorStringCases()
{
    Terminator terminator = createTerminator(TerminatorKind::SwitchString, 3);
    terminator.setStrings(std::array<StringID, 2> { StringID(8u), StringID(5u) });
    AOT_TEST_CHECK(hasElements(terminator.strings(), { StringID(8u), StringID(5u) }) && terminator.numberOfCases() == 2);
    AOT_TEST_CHECK(&terminator.otherwise() == &terminator.targets[2]);
}

static void terminatorIntegerCasesHaveTwoWords()
{
    Terminator terminator = createTerminator(TerminatorKind::Switch, 3);
    terminator.setCases(std::array<int64_t, 2> { -1, int64_t { 1 } << 40 });
    AOT_TEST_CHECK(terminator.numberOfCases() == 2 && terminator.outOfLinePayload().size() == 4);
    AOT_TEST_CHECK(terminator.cases()[0] == -1 && terminator.cases()[1] == int64_t { 1 } << 40);
}

static void terminatorProofs()
{
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::Branch, 0, 2) == TargetResults::Proofs && targetResultsOf(TerminatorKind::Branch, 1, 2) == TargetResults::Proofs);
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::Switch, 0, 3) == TargetResults::Proofs && targetResultsOf(TerminatorKind::Switch, 1, 3) == TargetResults::Proofs);
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::Switch, 2, 3) == TargetResults::None && targetResultsOf(TerminatorKind::Switch, 0, 1) == TargetResults::None);
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::Jump, 0, 1) == TargetResults::None);
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::TryCallInline, 0, 2) == TargetResults::DeclaredResults && targetResultsOf(TerminatorKind::TryCallInline, 1, 2) == TargetResults::None);
    AOT_TEST_CHECK(targetResultsOf(TerminatorKind::Branch, 2, 2) == TargetResults::None && targetResultsOf(TerminatorKind::Return, 0, 0) == TargetResults::None);

    Terminator terminator = createTerminator(TerminatorKind::Branch, 2);
    terminator.setOrigin({ 40, 2 });
    terminator.setConditionOperand(ValueID(1u));
    terminator.taken() = Target { BlockID(1u), { ValueID(0u) }, { 0u } };
    for (TerminatorKind kind : { TerminatorKind::TestAndBranch, TerminatorKind::Jump }) {
        terminator.become(kind);
        AOT_TEST_CHECK(terminator.kind == kind && terminator.targets.isEmpty() && terminator.position == 40 && terminator.inlineFrame == 2);
        unsigned numberOfChildren = 0;
        terminator.forEachChild([&](ValueID&) { ++numberOfChildren; });
        AOT_TEST_CHECK(!numberOfChildren);
    }
}

static void terminatorChildrenAreOperandsThenArguments()
{
    Terminator terminator = createTerminator(TerminatorKind::Branch, 2);
    terminator.setConditionOperand(ValueID(1u));
    terminator.taken().arguments = { ValueID(2u), ValueID(3u) };
    terminator.notTaken().arguments = { ValueID(4u) };
    Vector<ValueID> children;
    std::as_const(terminator).forEachChild([&](ValueID child) { children.append(child); });
    AOT_TEST_CHECK(hasElements(children, { ValueID(1u), ValueID(2u), ValueID(3u), ValueID(4u) }));
}

static void blockHandlerIsATarget()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    BlockID handler = addBlock(function);
    ValueID kept = addValue(function, entry, Opcode::Undefined);
    ValueID passed = addValue(function, entry, Opcode::Integer);
    ValueID subject = addValue(function, entry, Opcode::Null);
    jump(function, entry, exit, { passed });
    AOT_TEST_CHECK(!function.at(entry).handler && !function.at(handler).isHandlerBlock());
    function.at(entry).handler = Target { handler, { kept }, { } };
    function.at(entry).subject = subject;
    function.at(handler).role = BlockRole::CatchHandler;
    AOT_TEST_CHECK(function.at(handler).isHandlerBlock());

    Vector<BlockID> targets;
    std::as_const(function.at(entry)).forEachTarget([&](const Target& target) { targets.append(target.block); });
    AOT_TEST_CHECK(hasElements(targets, { exit, handler }));
    Vector<ValueID> children;
    std::as_const(function.at(entry)).forEachChildOutsideValues([&](ValueID child) { children.append(child); });
    AOT_TEST_CHECK(hasElements(children, { passed, kept, subject }));

    function.at(entry).forEachTarget([&](Target& target) { target.block = entry; });
    AOT_TEST_CHECK(function.at(entry).terminator.next().block == entry && function.at(entry).handler->block == entry);
}

static void edgeResultsFillTrailingParameters()
{
    TestProgram test;
    Function function;
    BlockID entry = addBlock(function);
    BlockID taken = addBlock(function);
    BlockID notTaken = addBlock(function);
    ValueID subject = addParameter(function, entry);
    ValueID condition = addValue(function, entry, Opcode::TypeTest, { subject });
    function.at(condition).setTestedType(TypeTable::number);
    branch(function, entry, condition, taken, notTaken);
    function.at(entry).terminator.taken().arguments = { subject };
    function.at(entry).terminator.taken().proofs = { 0u };
    function.at(entry).terminator.notTaken().proofs = { 0u, 5u };

    AOT_TEST_CHECK(firstEdgeResultParameter(function, entry, 0) == 1 && numberOfEdgeResults(function, entry, 0) == 1);
    AOT_TEST_CHECK(!firstEdgeResultParameter(function, entry, 1) && numberOfEdgeResults(function, entry, 1) == 2);
    AOT_TEST_CHECK(proofOfTerminator(function, function.at(entry)) == Proof::TestedType);

    EdgeResult onTaken = edgeResultAt(function, { entry, 0, 0 });
    AOT_TEST_CHECK(onTaken.kind == EdgeResultKind::NarrowedOperand && onTaken.proof == Proof::TestedType && onTaken.side == EdgeSide::Taken);
    AOT_TEST_CHECK(onTaken.test == condition && onTaken.subject == subject && !onTaken.operandIndex && !onTaken.caseIndex);
    EdgeResult onNotTaken = edgeResultAt(function, { entry, 1, 0 });
    AOT_TEST_CHECK(onNotTaken.kind == EdgeResultKind::NarrowedOperand && onNotTaken.side == EdgeSide::NotTaken && onNotTaken.subject == subject);
    AOT_TEST_CHECK(!tryGetEdgeResult(function, { entry, 1, 1 }) && !tryGetEdgeResult(function, { entry, 0, 1 }) && !tryGetEdgeResult(function, { entry, 2, 0 }));

    TypeArena arena(test.program.types);
    AOT_TEST_CHECK(typeOnEdge(test.program, function, { entry, 0, 0 }, arena) == TypeTable::number);
    const Type& otherType = test.program.at(typeOnEdge(test.program, function, { entry, 1, 0 }, arena));
    AOT_TEST_CHECK(!(otherType.primitives & Primitive::Number) && otherType.primitives & Primitive::String && otherType.anyObject);
}

static void edgeResultOfStretchProofIsAGuardOnTaken()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID taken = addBlock(function);
    BlockID notTaken = addBlock(function);
    ValueID object = addParameter(function, entry);
    ValueID condition = addValue(function, entry, Opcode::ShapeTest, { object });
    branch(function, entry, condition, taken, notTaken);
    function.at(entry).terminator.taken().proofs = { 0u, 1u };
    function.at(entry).terminator.notTaken().proofs = { 0u };

    EdgeResult guard = edgeResultAt(function, { entry, 0, 0 });
    AOT_TEST_CHECK(guard.kind == EdgeResultKind::Guard && guard.proof == Proof::Shape && guard.side == EdgeSide::Taken && guard.test == condition);
    AOT_TEST_CHECK(!tryGetEdgeResult(function, { entry, 0, 1 }) && !tryGetEdgeResult(function, { entry, 1, 0 }));

    function.at(condition).become(Opcode::Boolean);
    AOT_TEST_CHECK(!proofOfTerminator(function, function.at(entry)) && !tryGetEdgeResult(function, { entry, 0, 0 }));

    jump(function, entry, taken);
    function.at(entry).terminator.next().proofs = { 0u };
    AOT_TEST_CHECK(!numberOfEdgeResults(function, entry, 0) && !tryGetEdgeResult(function, { entry, 0, 0 }));
}

static void recordFitsRoleForEveryRole()
{
    struct RecordWithRoles {
        CreatorRecord record;
        Vector<FunctionRole> roles;
    };
    const RecordWithRoles cases[] = {
        { CreatorRecord::None { }, { FunctionRole::Source, FunctionRole::Library, FunctionRole::Model, FunctionRole::OutlinedBody } },
        { CreatorRecord::Entrypoint { }, { FunctionRole::GenericEntrypoint, FunctionRole::GroupEntrypoint } },
        { CreatorRecord::Version { }, { FunctionRole::GenericVersion, FunctionRole::AllocatedThisVersion, FunctionRole::CallSiteVersion } },
        { CreatorRecord::Coroutine { }, { FunctionRole::CoroutineRamp, FunctionRole::CoroutineBody } },
        { CreatorRecord::BoundFunction { }, { FunctionRole::BoundFunction } },
        { CreatorRecord::ResultWrapper { }, { FunctionRole::ResultWrapper } },
        { CreatorRecord::ResultWorker { }, { FunctionRole::ResultWorker } },
        { CreatorRecord::ParameterWrapper { }, { FunctionRole::ParameterWrapper } },
        { CreatorRecord::Continuation { }, { FunctionRole::Continuation } },
        { CreatorRecord::Serializer { }, { FunctionRole::Serializer } },
        { CreatorRecord::Parser { }, { FunctionRole::Parser, FunctionRole::ValidatingParser } },
        { CreatorRecord::Validator { }, { FunctionRole::Validator } },
        { CreatorRecord::ChainStep { }, { FunctionRole::ChainStep } },
        { CreatorRecord::StubBody { }, { FunctionRole::StubBody } },
    };
    static_assert(std::size(cases) == WTF::VariantSizeV<CreatorRecord::Cases>);
    size_t numberOfRoles = 0;
    for (const RecordWithRoles& fitting : cases) {
        numberOfRoles += fitting.roles.size();
        for (FunctionRole role : fitting.roles) {
            for (const RecordWithRoles& other : cases)
                AOT_TEST_CHECK((nameOf(other.record.kind()) == recordOf(role)) == (&other == &fitting));
        }
    }
    AOT_TEST_CHECK(numberOfRoles == numberOfEnumerators<FunctionRole>);
    AOT_TEST_CHECK(CreatorRecord().is<CreatorRecord::None>());
}

static void entryParameterPositions()
{
    for (unsigned combination = 0; combination < 8; ++combination) {
        Function function;
        function.captures.fill(createDeclaredType(TypeTable::anything), 2);
        function.parameters.fill(createDeclaredType(TypeTable::anything), 3);
        unsigned nextIndex = 2;
        auto addFlag = [&](FunctionFlags flag, unsigned bit) -> std::optional<unsigned> {
            if (!(combination & bit))
                return std::nullopt;
            function.flags.add(flag);
            return nextIndex++;
        };
        std::optional<unsigned> thisIndex = addFlag(FunctionFlags::HasThis, 1);
        std::optional<unsigned> calleeIndex = addFlag(FunctionFlags::TakesCallee, 2);
        std::optional<unsigned> newTargetIndex = addFlag(FunctionFlags::HasNewTarget, 4);
        AOT_TEST_CHECK(function.thisParameterIndex() == thisIndex && function.calleeParameterIndex() == calleeIndex && function.newTargetParameterIndex() == newTargetIndex);
        AOT_TEST_CHECK(function.firstParameterIndex() == nextIndex && function.numberOfEntryParameters() == nextIndex + 3);
    }
}

static void declaredEntryTypeOfCalleeIsFunction()
{
    TestProgram test;
    TypeArena arena(test.program.types);
    Function function;
    function.flags = { FunctionFlags::TakesCallee };
    addParameter(function, addBlock(function), TypeTable::string);
    TypingContext typing { test.program, function, arena };
    AOT_TEST_CHECK(function.calleeParameterIndex() == 0u && !function.declaredEntryParameter(0));
    AOT_TEST_CHECK(declaredEntryTypeOf(typing, function, 0) == TypeTable::function);
    function.blocks.clear();
    AOT_TEST_CHECK(declaredEntryTypeOf(typing, function, 0) == TypeTable::function);
}

static void declaredEntryTypeOfEachPosition()
{
    TestProgram test;
    TypeArena arena(test.program.types);
    Function function;
    function.flags = { FunctionFlags::HasThis, FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest };
    function.captures = { createDeclaredType(TypeTable::string) };
    function.thisType = createDeclaredType(TypeTable::number);
    function.parameters = { createDeclaredType(TypeTable::boolean) };
    TypingContext typing { test.program, function, arena };
    const TypeID restContents[] { TypeTable::anything };
    const TypeID expectedTypes[] {
        TypeTable::string, TypeTable::number, TypeTable::function, TypeTable::anything, arena.intern(Type::integer(Range::uint32())), TypeTable::boolean,
        arena.intern(Type::builtinClass(BuiltinClasses::Array, restContents)),
    };
    AOT_TEST_CHECK(function.numberOfEntryParameters() == std::size(expectedTypes));
    for (unsigned index = 0; index < std::size(expectedTypes); ++index) {
        AOT_TEST_CHECK(function.entryParameterKind(index) == static_cast<EntryParameter>(index));
        AOT_TEST_CHECK(declaredEntryTypeOf(typing, function, index) == expectedTypes[index]);
    }
}

static void unsplitAsyncFunctionHasNoOrdinaryFrame()
{
    Function function = createFunctionWithRole(FunctionRole::Source);
    AOT_TEST_CHECK(!function.isUnsplitCoroutine() && function.hasOrdinaryFrame() && function.mayBeInlined());
    function.flags = { FunctionFlags::IsAsync };
    function.capabilities = initialCapabilitiesOf(function);
    AOT_TEST_CHECK(function.isUnsplitCoroutine() && !function.hasOrdinaryFrame() && !function.mayBeInlined() && function.mayBeCloned());
    function.role = FunctionRole::CoroutineRamp;
    function.creator = createCoroutineRecord(CoroutineKind::AsyncFunction);
    function.capabilities = initialCapabilitiesOf(function);
    AOT_TEST_CHECK(!function.isUnsplitCoroutine() && function.hasOrdinaryFrame() && function.mayBeInlined());
}

static void coroutineBodyHasNoCapability()
{
    for (CoroutineKind kind : allMembersOf<CoroutineKind>) {
        Function body = createFunctionWithRole(FunctionRole::CoroutineBody, createCoroutineRecord(kind));
        AOT_TEST_CHECK(body.capabilities.isEmpty() && !body.isUnsplitCoroutine());
        AOT_TEST_CHECK(!body.mayBeInlined() && !body.mayBeCloned() && !body.mayChangeSignature() && !body.hasOrdinaryFrame());
        AOT_TEST_CHECK(body.mayBeCalledByHost() && !body.hasOnlyDirectCallers());
    }
}

static void hiddenByFlagOrCapability()
{
    Function function = createFunctionWithRole(FunctionRole::Source);
    AOT_TEST_CHECK(!function.isHiddenInStackTraces());
    function.flags = { FunctionFlags::IsHiddenInStackTraces };
    AOT_TEST_CHECK(function.isHiddenInStackTraces() && !function.has(FunctionCapabilities::IsHiddenInStackTraces));
    function.flags = { };
    function.capabilities.add(FunctionCapabilities::IsHiddenInStackTraces);
    AOT_TEST_CHECK(function.isHiddenInStackTraces());

    AOT_TEST_CHECK(createFunctionWithRole(FunctionRole::GenericEntrypoint, CreatorRecord::Entrypoint { }).isHiddenInStackTraces());
    AOT_TEST_CHECK(createFunctionWithRole(FunctionRole::CoroutineRamp, createCoroutineRecord(CoroutineKind::Generator)).isHiddenInStackTraces());
    for (FunctionRole role : { FunctionRole::Library, FunctionRole::Model })
        AOT_TEST_CHECK(!createFunctionWithRole(role).isHiddenInStackTraces());
    AOT_TEST_CHECK(!createFunctionWithRole(FunctionRole::CoroutineBody, createCoroutineRecord(CoroutineKind::Generator)).isHiddenInStackTraces());
    AOT_TEST_CHECK(!createFunctionWithRole(FunctionRole::GenericVersion, CreatorRecord::Version { }).isHiddenInStackTraces());
}

static void locationOfInlinedValue()
{
    Function function = createFunction({ Opcode::Undefined, Opcode::Undefined });
    function.module = ModuleID(1u);
    function.inlineFrames.grow(2);
    function.inlineFrames[1].callPosition = 40;
    function.inlineFrames[1].module = ModuleID(2u);
    function.values[0].setOrigin({ 90, 1 });
    function.values[1].setOrigin({ 70, 0 });
    AOT_TEST_CHECK(isAt(function.locationOf(ValueID(0u)), ModuleID(2u), 90));
    AOT_TEST_CHECK(isAt(function.locationOf(ValueID(1u)), ModuleID(1u), 70));
}

static void locationWithoutFrameModuleIsCallSite()
{
    Function function = createFunction({ Opcode::Undefined, Opcode::Undefined });
    function.module = ModuleID(1u);
    function.inlineFrames.grow(3);
    function.inlineFrames[1].callPosition = 40;
    function.inlineFrames[2].callPosition = 55;
    function.inlineFrames[2].parent = 1;
    function.values[0].setOrigin({ 90, 2 });
    function.values[1].setOrigin({ Instruction::noPosition, 2 });
    AOT_TEST_CHECK(isAt(function.locationOf(ValueID(0u)), ModuleID(1u), 40));
    AOT_TEST_CHECK(isAt(function.locationOf(ValueID(1u)), ModuleID(), 0));
}

static void locationOfMalformedFrameListIsAbsent()
{
    Function function;
    function.module = ModuleID(1u);
    function.inlineFrames.grow(2);
    AOT_TEST_CHECK(isAt(function.locationOf(Origin { 90, 5 }), ModuleID(), 0));
    function.inlineFrames[1].parent = 1;
    AOT_TEST_CHECK(isAt(function.locationOf(Origin { 90, 1 }), ModuleID(), 0));
    function.inlineFrames.grow(3);
    function.inlineFrames[1].parent = 2;
    function.inlineFrames[2].parent = 1;
    AOT_TEST_CHECK(isAt(function.locationOf(Origin { 90, 2 }), ModuleID(), 0));
}

static void compactDropsUnreachableCycle()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID first = addBlock(function);
    BlockID second = addBlock(function);
    BlockID exit = addBlock(function);
    jump(function, entry, exit);
    jump(function, first, second);
    jump(function, second, first);
    addValue(function, first, Opcode::Integer);
    addValue(function, exit, Opcode::Undefined);

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.blocks.size() == 2 && function.blocks[0].terminator.targets[0].block == BlockID(1u));
    AOT_TEST_CHECK(function.values.size() == 1 && function.values[0].opcode == Opcode::Undefined);
    AOT_TEST_CHECK(hasElements(function.blocks[1].values, { ValueID(0u) }));
}

static void compactDropsUnlistedValue()
{
    Function function;
    BlockID entry = addBlock(function);
    ValueID left = addValue(function, entry, Opcode::Undefined);
    ValueID dropped = addValue(function, entry, Opcode::Call);
    ValueID right = addValue(function, entry, Opcode::Integer);
    function.at(entry).terminator.become(TerminatorKind::Return);
    function.at(entry).terminator.setValueOperand(addValue(function, entry, Opcode::Add, { left, right }));
    function.at(entry).values.removeFirst(dropped);

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.values.size() == 3 && function.at(entry).values.size() == 3);
    const Value& returned = function.at(function.at(entry).terminator.valueOperand());
    AOT_TEST_CHECK(returned.opcode == Opcode::Add);
    AOT_TEST_CHECK(function.at(returned.operand(0)).opcode == Opcode::Undefined && function.at(returned.operand(1)).opcode == Opcode::Integer);
}

static void compactKeepsCompactFunction()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    ValueID argument = addValue(function, entry, Opcode::Undefined);
    ValueID parameter = addParameter(function, exit);
    ValueID sum = addValue(function, exit, Opcode::Add, { parameter, argument });
    jump(function, entry, exit, { argument });

    AOT_TEST_CHECK(!compactFunction(function));
    AOT_TEST_CHECK(function.blocks.size() == 2 && function.values.size() == 3);
    AOT_TEST_CHECK(hasElements(function.at(entry).values, { argument }));
    AOT_TEST_CHECK(function.at(entry).terminator.targets[0].block == exit && hasElements(function.at(entry).terminator.targets[0].arguments, { argument }));
    AOT_TEST_CHECK(hasElements(function.at(exit).parameters, { parameter }) && hasElements(function.at(exit).values, { sum }));
    AOT_TEST_CHECK(function.at(sum).operand(0) == parameter && function.at(sum).operand(1) == argument);

    Function withoutBody;
    AOT_TEST_CHECK(!compactFunction(withoutBody));
}

static void compactRenumbersInBlockOrder()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    ValueID sum = addValue(function, exit, Opcode::Add);
    addUnlistedValue(function, Opcode::Call);
    ValueID parameter = addParameter(function, exit);
    ValueID argument = addValue(function, entry, Opcode::Integer);
    function.at(sum).setOperand(0, parameter);
    function.at(sum).setOperand(1, argument);
    jump(function, entry, exit, { argument });

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.values.size() == 3 && hasElements(function.at(entry).values, { ValueID(0u) }));
    AOT_TEST_CHECK(hasElements(function.at(exit).parameters, { ValueID(1u) }) && hasElements(function.at(exit).values, { ValueID(2u) }));
    AOT_TEST_CHECK(function.values[0].opcode == Opcode::Integer && function.values[1].opcode == Opcode::Parameter && function.values[2].opcode == Opcode::Add);
    AOT_TEST_CHECK(function.values[2].operand(0) == ValueID(1u) && function.values[2].operand(1) == ValueID(0u));
    AOT_TEST_CHECK(hasElements(function.at(entry).terminator.targets[0].arguments, { ValueID(0u) }));
}

static void forEachTypeVisitsEveryPosition()
{
    Function function;
    function.captures = { createDeclaredType(TypeID(100u)) };
    function.thisType = createDeclaredType(TypeID(101u));
    function.parameters = { createDeclaredType(TypeID(102u), TypeID(103u)) };
    function.parameters[0].written = TypeID(107u);
    function.results = { createDeclaredType(TypeID(104u)) };
    function.role = FunctionRole::CoroutineRamp;
    function.creator = createCoroutineRecord(CoroutineKind::AsyncFunction, TypeID(106u));
    Vector<TypeID> visited;
    forEachType(static_cast<const FunctionRecord&>(function), [&](TypeID id) { visited.append(id); });
    std::ranges::sort(visited);
    AOT_TEST_CHECK(hasElements(visited, { TypeID(100u), TypeID(101u), TypeID(102u), TypeID(103u), TypeID(104u), TypeID(106u), TypeID(107u) }));
}

#define FOR_EACH_AOT_IR_TEST(v) \
    v(compactListAppendInsertRemove) v(compactListAppendOwnSpan) v(compactListCopyAndMove) v(compactListRemoveAtEnd) v(compactListEmptyOperationsLeaveSharedHeader) \
    v(effectsDumpUsesSchemaGrammar) v(effectsDumpCountsLocationsOfPartialHeap) v(effectsOnEdgesDumpShowsHandlerThatDiffers) v(abstractHeapSetIterationAscends) \
    v(parseEnumFindsEveryOpcode) v(parseEnumRejectsUnknownNames) \
    v(valueOperandsOfCall) v(valueAbsentOperandKeepsIndex) v(valueFourthNamedOperandIsInArguments) v(valueVariadicOperandsFollowFiveNamedOperands) \
    v(valueRemoveVariadicOperandKeepsNamedOperands) v(valueBecomeKeepsTypesOriginAndRepresentation) v(valueTypeLevelsFallBackToStrongerLevel) \
    v(valueListPayloadFollowsFixedWords) v(valueListPayloadStartsAfterInlineWords) v(valueInt64PayloadHasTwoWords) v(valueSamePayloadComparesFlagsAndBothParts) \
    v(valueForEachReferenceSkipsNull) \
    v(terminatorNamedTargets) v(terminatorStringCases) v(terminatorIntegerCasesHaveTwoWords) v(terminatorProofs) v(terminatorChildrenAreOperandsThenArguments) \
    v(blockHandlerIsATarget) v(edgeResultsFillTrailingParameters) v(edgeResultOfStretchProofIsAGuardOnTaken) \
    v(recordFitsRoleForEveryRole) v(entryParameterPositions) v(declaredEntryTypeOfCalleeIsFunction) v(declaredEntryTypeOfEachPosition) \
    v(unsplitAsyncFunctionHasNoOrdinaryFrame) v(coroutineBodyHasNoCapability) v(hiddenByFlagOrCapability) \
    v(locationOfInlinedValue) v(locationWithoutFrameModuleIsCallSite) v(locationOfMalformedFrameListIsAbsent) \
    v(compactDropsUnreachableCycle) v(compactDropsUnlistedValue) v(compactKeepsCompactFunction) v(compactRenumbersInBlockOrder) v(forEachTypeVisitsEveryPosition)

std::span<const NativeTest> irTests()
{
#define AOT_IR_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_IR_TEST(AOT_IR_TEST_ENTRY) };
#undef AOT_IR_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
