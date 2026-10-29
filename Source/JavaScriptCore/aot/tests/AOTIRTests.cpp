/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTEffects.h"
#include "AOTIRTestSupport.h"
#include "AOTLimits.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
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

DeclaredType createDeclaredType(TypeID type, OptionSet<PositionFlags> flags = { })
{
    return flags.contains(PositionFlags::IsExpected) ? DeclaredType(ExpectedType(type)) : DeclaredType(Proven::byPosition(type));
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
    CompactList<uint32_t> copyOfSingleElement = original;
    copyOfSingleElement[0] = 8;
    AOT_TEST_CHECK(hasElements(original, { 9u }) && hasElements(copyOfSingleElement, { 8u }) && copyOfSingleElement.capacity() == 1);
    CompactList<uint32_t> empty;
    CompactList<uint32_t> copyOfEmpty = empty;
    copyOfEmpty.append(std::array<uint32_t, 2> { 1, 2 });
    AOT_TEST_CHECK(empty.isEmpty() && !empty.capacity() && hasElements(copyOfEmpty, { 1u, 2u }));
    moved.shrink(0);
    CompactList<uint32_t> copyOfEmptied = moved;
    AOT_TEST_CHECK(copyOfEmptied.isEmpty() && !copyOfEmptied.capacity() && moved.capacity() == 2);
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

static void compactListStoresSingleElementInline()
{
    CompactList<uint32_t> list { 7 };
    AOT_TEST_CHECK(hasElements(list, { 7u }) && list.capacity() == 1);
    AOT_TEST_CHECK(reinterpret_cast<uintptr_t>(list.begin()) - reinterpret_cast<uintptr_t>(&list) < sizeof(list));
    CompactList<uint32_t> moved = WTF::move(list);
    AOT_TEST_CHECK(list.isEmpty() && hasElements(moved, { 7u }));
    moved[0] = UINT32_MAX;
    moved.append(moved.span());
    AOT_TEST_CHECK(hasElements(moved, { UINT32_MAX, UINT32_MAX }));
    moved.removeLast();
    moved.shrinkToFit();
    AOT_TEST_CHECK(hasElements(moved, { UINT32_MAX }) && moved.capacity() == 1);
    moved.insert(0, 1);
    AOT_TEST_CHECK(hasElements(moved, { 1u, UINT32_MAX }));
    moved.shrink(0);
    moved.grow(1);
    AOT_TEST_CHECK(hasElements(moved, { 0u }));
    moved.clear();
    moved.grow(1);
    moved.removeAll(0);
    AOT_TEST_CHECK(moved.isEmpty() && !moved.capacity());
    CompactList<int64_t> wide { -1 };
    AOT_TEST_CHECK(hasElements(wide, { static_cast<int64_t>(-1) }));
}

static void inlineListSpillsAndShrinksToFit()
{
    InlineList<uint32_t, 3> list { 1, 2, 3 };
    AOT_TEST_CHECK(list.capacity() == 3 && sizeof(list) == 16);
    list.insert(1, 9);
    AOT_TEST_CHECK(hasElements(list, { 1u, 9u, 2u, 3u }) && list.capacity() > 3);
    list.append(list.span().first(2));
    list.insertSpan(0, list.span().last(1));
    AOT_TEST_CHECK(hasElements(list, { 9u, 1u, 9u, 2u, 3u, 1u, 9u }));
    AOT_TEST_CHECK(list.removeAll(9) == 3 && list.removeFirst(2) && !list.removeFirst(2) && list.reverseFind(1) == 2);
    AOT_TEST_CHECK(hasElements(list, { 1u, 3u, 1u }) && list.capacity() > 3);
    InlineList<uint32_t, 3> copy = list;
    AOT_TEST_CHECK(copy == list && copy.capacity() == 3);
    list.shrinkToFit();
    AOT_TEST_CHECK(hasElements(list, { 1u, 3u, 1u }) && list.capacity() == 3);
    InlineList<uint32_t, 3> moved = WTF::move(list);
    AOT_TEST_CHECK(list.isEmpty() && moved == copy && moved.takeLast() == 1);
    moved.grow(5);
    moved.removeAt(1, 3);
    AOT_TEST_CHECK(hasElements(moved, { 1u, 0u }));
}

static void inlineListCopiesElementsInPlace()
{
    InlineList<uint32_t, 3> list { 1, 2 };
    InlineList<uint32_t, 3> copy = list;
    copy.append(3);
    AOT_TEST_CHECK(hasElements(list, { 1u, 2u }) && hasElements(copy, { 1u, 2u, 3u }) && copy.capacity() == 3);
    copy = list;
    AOT_TEST_CHECK(hasElements(copy, { 1u, 2u }) && copy.capacity() == 3);
    copy.append(std::array<uint32_t, 2> { 3, 4 });
    AOT_TEST_CHECK(hasElements(copy, { 1u, 2u, 3u, 4u }) && copy.capacity() > 3);
    list = copy;
    copy = InlineList<uint32_t, 3> { 5 };
    AOT_TEST_CHECK(hasElements(list, { 1u, 2u, 3u, 4u }) && hasElements(copy, { 5u }));
    InlineList<uint32_t, 3> empty;
    list = empty;
    InlineList<uint32_t, 3> copyOfEmpty = empty;
    AOT_TEST_CHECK(list.isEmpty() && copyOfEmpty.isEmpty() && copyOfEmpty.capacity() == 3);
}

static void inlineListMovesAndDestroysElements()
{
    TargetList targets;
    for (uint32_t index = 0; index < 4; ++index)
        targets.append(Target { BlockID(index), { ValueID(index), ValueID(index + 1) }, { index } });
    TargetList copy = targets;
    targets.removeAt(1);
    targets.insert(0, copy[3]);
    AOT_TEST_CHECK(targets.size() == 4 && targets[0] == copy[3] && targets[1] == copy[0] && targets[2] == copy[2] && targets[3] == copy[3]);
    targets.shrink(2);
    targets.shrinkToFit();
    AOT_TEST_CHECK(targets.capacity() == 2 && hasElements(targets[1].arguments, { ValueID(0u), ValueID(1u) }) && hasElements(targets[0].proofs, { 3u }));
    copy = WTF::move(targets);
    AOT_TEST_CHECK(targets.isEmpty() && copy.size() == 2 && copy.last().block == BlockID(0u));
}

static void outOfLineOptionalMatchesStdOptional()
{
    OutOfLineOptional<Target> handler;
    AOT_TEST_CHECK(!handler && handler == std::nullopt && sizeof(handler) == sizeof(void*));
    handler = Target { BlockID(3u), { ValueID(1u), ValueID(2u) }, { } };
    OutOfLineOptional<Target> copy = handler;
    copy->block = BlockID(4u);
    AOT_TEST_CHECK(handler->block == BlockID(3u) && copy != handler && hasElements(copy->arguments, { ValueID(1u), ValueID(2u) }));
    std::optional<Target> plain = handler;
    AOT_TEST_CHECK(plain && handler == plain && *plain == *handler);
    OutOfLineOptional<Target> taken = std::exchange(handler, std::nullopt);
    AOT_TEST_CHECK(!handler && taken == plain);
    handler = plain;
    AOT_TEST_CHECK(handler == taken);
    handler = std::optional<Target>();
    AOT_TEST_CHECK(!handler.has_value());
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

static void positionsThatManyOperationsDeclareAreReadFromOneTable()
{
    for (Opcode opcode : allMembersOf<Opcode>) {
        AOT_TEST_CHECK(PayloadWord::key(opcode) == PayloadWord::keyAsDeclared(opcode));
        AOT_TEST_CHECK(PayloadWord::layout(opcode) == PayloadWord::layoutAsDeclared(opcode));
        AOT_TEST_CHECK(OperandPosition::value(opcode) == OperandPosition::valueAsDeclared(opcode));
    }
    AOT_TEST_CHECK(PayloadWord::key(Opcode::PropGet) == 0u && PayloadWord::key(Opcode::DefineMethod) == 4u && !PayloadWord::key(Opcode::Undefined));
    for (unsigned noMember : { numberOfEnumerators<Opcode>, unsigned { UINT16_MAX } })
        AOT_TEST_CHECK(!PayloadWord::key(static_cast<Opcode>(noMember)) && !OperandPosition::value(static_cast<Opcode>(noMember)));
}

static void operationsThatHoldTheSameReferencesAreWalkedByOneCase()
{
    for (Opcode opcode : allMembersOf<Opcode>)
        AOT_TEST_CHECK((CaseTable<Opcode, caseOfWalkValue>::at(opcode)) == caseOfWalkValue(opcode));
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        AOT_TEST_CHECK((CaseTable<TerminatorKind, caseOfWalkTerminator>::at(kind)) == caseOfWalkTerminator(kind));
    AOT_TEST_CHECK(caseOfWalkValue(Opcode::Add) == caseOfWalkValue(Opcode::Sub) && caseOfWalkValue(Opcode::String) == caseOfWalkValue(Opcode::BigInt));
    AOT_TEST_CHECK(caseOfWalkValue(Opcode::Add) != caseOfWalkValue(Opcode::String) && caseOfWalkValue(Opcode::String) != caseOfWalkValue(Opcode::NewClosure));
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
    AOT_TEST_CHECK(call.child(0) == ValueID(10u) && call.child(1) == ValueID(11u) && !call.child(2));
    AOT_TEST_CHECK(hasElements(call.operands(), { ValueID(10u), ValueID(11u), ValueID(12u), ValueID(13u) }) && hasElements(call.variadicOperands(), { ValueID(12u), ValueID(13u) }));
    AOT_TEST_CHECK(!call.variadicOperandIndex(1) && call.variadicOperandIndex(2) == 0u && call.variadicOperandIndex(3) == 1u);
    call.setOperand(3, ValueID(23u));
    AOT_TEST_CHECK(hasElements(call.variadicOperands(), { ValueID(12u), ValueID(23u) }));
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

static void valueFourNamedOperands()
{
    Value store(Opcode::SuperSet);
    AOT_TEST_CHECK(store.numberOfOperands() == 4 && hasElements(store.operands(), { ValueID(), ValueID(), ValueID(), ValueID() }));
    store.setOperand(3, ValueID(7u));
    AOT_TEST_CHECK(store.operand(3) == ValueID(7u) && hasElements(store.operands(), { ValueID(), ValueID(), ValueID(), ValueID(7u) }));
    AOT_TEST_CHECK(!store.numberOfVariadicOperands() && store.variadicOperands().empty());
}

static void valueVariadicOperandsFollowFiveNamedOperands()
{
    Value value(Opcode::DefineOwnProperty);
    AOT_TEST_CHECK(value.numberOfNamedOperands() == 5 && value.numberOfOperands() == 5 && hasElements(value.operands(), { ValueID(), ValueID(), ValueID(), ValueID(), ValueID() }));
    for (uint32_t index = 0; index < 5; ++index)
        value.setOperand(index, ValueID(10 + index));
    AOT_TEST_CHECK(hasElements(value.operands(), { ValueID(10u), ValueID(11u), ValueID(12u), ValueID(13u), ValueID(14u) }));
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
    AOT_TEST_CHECK(hasElements(value.operands().subspan(3), { ValueID(13u), ValueID(14u), ValueID(16u), ValueID(17u) }));
    value.removeVariadicOperand(0);
    AOT_TEST_CHECK(hasElements(value.operands().subspan(3), { ValueID(13u), ValueID(14u), ValueID(17u) }) && value.operand(5) == ValueID(17u));
    value.setVariadicOperands({ });
    AOT_TEST_CHECK(value.numberOfOperands() == 5 && hasElements(value.operands().subspan(3), { ValueID(13u), ValueID(14u) }));
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
    value.type = TypeTable::object;
    value.setExpected(ExpectedType(TypeTable::function));
    value.setOrigin({ 40, 2 });
    value.representation = Representation::Cell;
    value.flags = UINT8_MAX;
    value.setHoles(std::array<uint32_t, 2> { 1, 2 });
    value.setOperand(0, ValueID(10u));
    value.appendVariadicOperand(ValueID(11u));
    AOT_TEST_CHECK(!value.outOfLinePayload().empty());

    value.become(Opcode::Integer);
    AOT_TEST_CHECK(value.opcode == Opcode::Integer && !value.flags);
    AOT_TEST_CHECK(!value.numberOfOperands() && value.operands().empty() && !value.child(0));
    AOT_TEST_CHECK(value.outOfLinePayload().empty() && !value.integer());
    AOT_TEST_CHECK(value.type == TypeTable::object && value.expected().predicted() == TypeTable::function);
    AOT_TEST_CHECK(value.position == 40 && value.inlineFrame == 2 && value.representation == Representation::Cell);
}

static void valueWithoutExpectedTypeFallsBackToItsType()
{
    Value value(Opcode::Undefined);
    value.type = TypeTable::anything;
    AOT_TEST_CHECK(!value.expected().predicted() && value.expectedOrType().predicted() == TypeTable::anything);
    value.type = TypeTable::object;
    AOT_TEST_CHECK(!value.expected().predicted() && value.expectedOrType().predicted() == TypeTable::object);
    value.setExpected(ExpectedType(TypeTable::function));
    AOT_TEST_CHECK(value.type == TypeTable::object && value.expected().predicted() == TypeTable::function && value.expectedOrType().predicted() == TypeTable::function);
    value.setExpected({ });
    AOT_TEST_CHECK(!value.expected().predicted() && value.expectedOrType().predicted() == TypeTable::object);
}

static void valueExpectedTypeSurvivesRelayout()
{
    Value call(Opcode::Call);
    call.setOperand(0, ValueID(10u));
    call.setExpected(ExpectedType(TypeTable::function));
    call.appendVariadicOperand(ValueID(12u));
    AOT_TEST_CHECK(call.expected().predicted() == TypeTable::function && call.operand(0) == ValueID(10u) && call.operand(2) == ValueID(12u));
    Value copy = call;
    Value moved = WTF::move(call);
    AOT_TEST_CHECK(copy.expected().predicted() == TypeTable::function && moved.expected().predicted() == TypeTable::function && moved.operand(2) == ValueID(12u));
    copy.become(Opcode::Undefined);
    AOT_TEST_CHECK(copy.expected().predicted() == TypeTable::function && !copy.numberOfOperands());

    for (Opcode opcode : allMembersOf<Opcode>) {
        Value value(opcode);
        uint32_t numberOfOperands = value.numberOfNamedOperands();
        unsigned numberOfWords = defaultPayloadOf(opcode).size();
        for (uint32_t index = 0; index < numberOfOperands; ++index)
            value.setOperand(index, ValueID(20 + index));
        for (unsigned word = 0; word < numberOfWords; ++word)
            value.setPayloadWordAt(word, 100 + word);
        for (TypeID expected : { TypeTable::object, TypeID(), TypeTable::function }) {
            value.setExpected(ExpectedType(expected));
            AOT_TEST_CHECK(value.expected().predicted() == expected && value.numberOfOperands() == numberOfOperands);
            for (uint32_t index = 0; index < numberOfOperands; ++index)
                AOT_TEST_CHECK(value.operand(index) == ValueID(20 + index));
            for (unsigned word = 0; word < numberOfWords; ++word)
                AOT_TEST_CHECK(value.payloadWordAt(word) == 100 + word);
        }
    }
}

static void flagsAndInlineFrameFitBitFields()
{
    Value value(Opcode::Undefined);
    value.flags = UINT8_MAX;
    constexpr uint64_t maxInlineFrames = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());
    value.inlineFrame = maxInlineFrames;
    AOT_TEST_CHECK(value.flags == UINT8_MAX && value.inlineFrame == maxInlineFrames);
    for (Opcode opcode : allMembersOf<Opcode>)
        AOT_TEST_CHECK(declaredFlagsOf(opcode) <= UINT8_MAX);
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        AOT_TEST_CHECK(declaredFlagsOf(kind) <= UINT8_MAX);
}

static void valueSpansSurviveMove()
{
    Vector<Value> values;
    values.append(Value(Opcode::NewArray));
    values[0].appendVariadicOperand(ValueID(11u));
    values[0].setHoles(std::array<uint32_t, 2> { 1, 2 });
    std::span<const ValueID> operands = values[0].variadicOperands();
    std::span<const uint32_t> holes = values[0].holes();
    for (unsigned count = 0; count < 100; ++count)
        values.append(Value(Opcode::Undefined));
    AOT_TEST_CHECK(operands.data() == values[0].variadicOperands().data() && holes.data() == values[0].holes().data());
    AOT_TEST_CHECK(hasElements(operands, { ValueID(11u) }) && hasElements(holes, { 1u, 2u }));
}

static void checkFixedWordsSurviveListPayload(Opcode opcode, ASCIILiteral listName)
{
    std::span<const PayloadFieldInfo> fields = payloadFieldsOf(opcode);
    const PayloadFieldInfo& list = fieldNamed(opcode, listName);
    const unsigned numberOfFixedWords = list.firstWord;
    AOT_TEST_CHECK(list.kind == PayloadKind::List && numberOfFixedWords > numberOfInlinePayloadWords);
    AOT_TEST_CHECK(std::ranges::all_of(fields, [&](const PayloadFieldInfo& field) {
        return field.kind == PayloadKind::List || field.firstWord < numberOfFixedWords;
    }));
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
    checkFixedWordsSurviveListPayload(Opcode::NewArray, "holes"_s);
    checkFixedWordsSurviveListPayload(Opcode::NewObject, "fields"_s);

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
    test.setExpected(ExpectedType(TypeTable::boolean));
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
    AOT_TEST_CHECK(test.type == TypeTable::anything && test.expected().predicted() == TypeTable::boolean);

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
    function.edit(entry).handler = Target { handler, { kept }, { } };
    function.edit(entry).subject = subject;
    function.edit(handler).role = BlockRole::CatchHandler;
    AOT_TEST_CHECK(function.at(handler).isHandlerBlock());

    Vector<BlockID> targets;
    std::as_const(function.at(entry)).forEachTarget([&](const Target& target) { targets.append(target.block); });
    AOT_TEST_CHECK(hasElements(targets, { exit, handler }));
    Vector<ValueID> children;
    std::as_const(function.at(entry)).forEachChildOutsideValues([&](ValueID child) { children.append(child); });
    AOT_TEST_CHECK(hasElements(children, { passed, kept, subject }));

    function.edit(entry).forEachTarget([&](Target& target) { target.block = entry; });
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
    function.edit(condition).setTestedType(TypeTable::number);
    branch(function, entry, condition, taken, notTaken);
    function.edit(entry).terminator.taken().arguments = { subject };
    function.edit(entry).terminator.taken().proofs = { 0u };
    function.edit(entry).terminator.notTaken().proofs = { 0u, 5u };

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
    ValueID condition = addValue(function, entry, Opcode::IsSlotFilled, { object });
    branch(function, entry, condition, taken, notTaken);
    function.edit(entry).terminator.taken().proofs = { 0u, 1u };
    function.edit(entry).terminator.notTaken().proofs = { 0u };

    EdgeResult guard = edgeResultAt(function, { entry, 0, 0 });
    AOT_TEST_CHECK(provesForStretch(Proof::SlotFilled) && !provesForStretch(Proof::TestedType));
    AOT_TEST_CHECK(guard.kind == EdgeResultKind::Guard && guard.proof == Proof::SlotFilled && guard.side == EdgeSide::Taken && guard.test == condition && guard.subject == object);
    AOT_TEST_CHECK(!tryGetEdgeResult(function, { entry, 0, 1 }) && !tryGetEdgeResult(function, { entry, 1, 0 }));

    function.edit(condition).become(Opcode::Boolean);
    AOT_TEST_CHECK(!proofOfTerminator(function, function.at(entry)) && !tryGetEdgeResult(function, { entry, 0, 0 }));

    jump(function, entry, taken);
    function.edit(entry).terminator.next().proofs = { 0u };
    AOT_TEST_CHECK(!numberOfEdgeResults(function, entry, 0) && !tryGetEdgeResult(function, { entry, 0, 0 }));
}

static void recordFitsRoleForEveryRole()
{
    struct RecordWithRoles {
        CreatorRecord record;
        Vector<FunctionRole> roles;
    };
    const RecordWithRoles cases[] = {
        { CreatorRecord::None { }, { FunctionRole::Source, FunctionRole::Library, FunctionRole::Model, FunctionRole::Signature, FunctionRole::OutlinedBody } },
        { CreatorRecord::Entrypoint { }, { FunctionRole::GenericEntrypoint, FunctionRole::TableEntrypoint } },
        { CreatorRecord::TableLookup { }, { FunctionRole::TableLookup } },
        { CreatorRecord::Version { }, { FunctionRole::GenericVersion, FunctionRole::AllocatedThisVersion, FunctionRole::ConstructBody, FunctionRole::CallSiteVersion, FunctionRole::CalleeVersion } },
        { CreatorRecord::Coroutine { }, { FunctionRole::CoroutineRamp, FunctionRole::CoroutineBody, FunctionRole::AsyncWorker } },
        { CreatorRecord::BoundFunction { }, { FunctionRole::BoundFunction } },
        { CreatorRecord::ResultWrapper { }, { FunctionRole::ResultWrapper } },
        { CreatorRecord::ResultWorker { }, { FunctionRole::ResultWorker } },
        { CreatorRecord::ParameterWrapper { }, { FunctionRole::ParameterWrapper } },
        { CreatorRecord::EntryChecks { }, { FunctionRole::EntryChecks } },
        { CreatorRecord::Serializer { }, { FunctionRole::Serializer } },
        { CreatorRecord::Parser { }, { FunctionRole::Parser } },
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
    function.editAllBlocks().clear();
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
    function.edit(ValueID(0u)).setOrigin({ 90, 1 });
    function.edit(ValueID(1u)).setOrigin({ 70, 0 });
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
    function.edit(ValueID(0u)).setOrigin({ 90, 2 });
    function.edit(ValueID(1u)).setOrigin({ Instruction::noPosition, 2 });
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
    function.edit(entry).terminator.become(TerminatorKind::Return);
    function.edit(entry).terminator.setValueOperand(addValue(function, entry, Opcode::Add, { left, right }));
    function.edit(entry).values.removeFirst(dropped);

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
    function.edit(sum).setOperand(0, parameter);
    function.edit(sum).setOperand(1, argument);
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
    function.parameters = { createDeclaredType(TypeID(102u)), createDeclaredType(TypeID(103u), PositionFlags::IsExpected) };
    function.results = { createDeclaredType(TypeID(104u)) };
    function.role = FunctionRole::CoroutineRamp;
    function.creator = createCoroutineRecord(CoroutineKind::AsyncFunction, TypeID(106u));
    Vector<TypeID> visited;
    forEachType(static_cast<const FunctionRecord&>(function), [&](TypeID id) { visited.append(id); });
    std::ranges::sort(visited);
    AOT_TEST_CHECK(hasElements(visited, { TypeID(100u), TypeID(101u), TypeID(102u), TypeID(103u), TypeID(104u), TypeID(106u) }));
}

#define FOR_EACH_AOT_IR_TEST(v) \
    v(compactListAppendInsertRemove) v(compactListAppendOwnSpan) v(compactListCopyAndMove) v(compactListRemoveAtEnd) v(compactListEmptyOperationsLeaveSharedHeader) \
    v(compactListStoresSingleElementInline) v(inlineListSpillsAndShrinksToFit) v(inlineListCopiesElementsInPlace) v(inlineListMovesAndDestroysElements) v(outOfLineOptionalMatchesStdOptional) \
    v(effectsDumpUsesSchemaGrammar) v(effectsDumpCountsLocationsOfPartialHeap) v(effectsOnEdgesDumpShowsHandlerThatDiffers) v(abstractHeapSetIterationAscends) \
    v(parseEnumFindsEveryOpcode) v(parseEnumRejectsUnknownNames) v(positionsThatManyOperationsDeclareAreReadFromOneTable) \
    v(operationsThatHoldTheSameReferencesAreWalkedByOneCase) \
    v(valueOperandsOfCall) v(valueAbsentOperandKeepsIndex) v(valueFourNamedOperands) v(valueVariadicOperandsFollowFiveNamedOperands) \
    v(valueRemoveVariadicOperandKeepsNamedOperands) v(valueBecomeKeepsTypesOriginAndRepresentation) v(valueWithoutExpectedTypeFallsBackToItsType) \
    v(valueExpectedTypeSurvivesRelayout) v(flagsAndInlineFrameFitBitFields) v(valueSpansSurviveMove) \
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
