/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"
#include "AOTWalkScalarReplacement.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkKnown.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkSortKeys.h"
#include <algorithm>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t inlineLimitOutsideLoops = 6;
static constexpr uint32_t inlineLimitInNestedIterations = 24;
static constexpr uint32_t inlineLimitInAsyncBody = 2;
static constexpr uint32_t inlineLimitInComparators = 10;
static constexpr uint32_t inlineLimitInLoops = 40;
static constexpr uint32_t inlineLimitInLoopsWithoutObjects = 24;
static constexpr uint32_t inlineLimitOfRemovedObjects = 48;
static constexpr uint32_t maxThrowingOperations = 12;
static constexpr uint32_t inlineWindowOfWrittenCall = 40;
static constexpr uint32_t inlineWindowOfWrittenCallInLoops = 120;
static constexpr uint32_t inlineWindowOfRemovedObjects = 240;
static constexpr uint32_t inlineLimitOfLeaves = 12;
static constexpr uint32_t inlineLimitOfLeavesOutsideLoops = 3;
static constexpr uint32_t inlineLimitOfHeads = 4;
static constexpr uint32_t inlineLimitOfHeadsWhoseQuestionWasAsked = inlineLimitOfHeads + 1;
static constexpr uint32_t inlineLimitOfHeadsTestingParameters = 8;
static constexpr uint32_t inlineLimitOfHelpers = 40;
static constexpr uint32_t inlineWindowOfHelpers = 140;

static constexpr uint64_t mostInlineFramesAtInlining = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());

static constexpr OpcodeSet opcodesOfInlining { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallDirect };
static constexpr OpcodeSet constructionsOfInlining { Opcode::Construct, Opcode::SuperConstruct };
static constexpr OpcodeSet callsOfListsAtInlining { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };

struct CallOfInlining {
    FunctionID callee;
    Reference closure;
    Reference thisValue;
    Reference newTarget;
    std::span<const Reference> arguments;
};

static ALWAYS_INLINE Reference closureCalledAtInlining(const Draft& site)
{
    return site.opcode == Opcode::SuperConstruct ? site.parentOperand() : site.calleeOperand();
}

static bool endsInReturnAtInlining(BodyTerminator end)
{
    return end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall;
}

static bool isNoLargerThanCallWithItsOwnCallsAtInlining(const ThinSummary& summary, size_t numberOfOperands)
{
    size_t size = summary.numberOfOperations + static_cast<size_t>(summary.numberOfOwnCalls) * inlineLimitOutsideLoops + summary.numberOfArgumentsOfOwnCalls;
    return size <= inlineLimitOutsideLoops + numberOfOperands;
}

static bool passesFreshObject(const Walker& walker, std::span<const Reference> passedOperands)
{
    return std::ranges::any_of(passedOperands, [&](Reference operand) {
        if (!operand || !operand.isValue())
            return false;
        ValueID cell = underlyingCellOf(walker.code(), operand.id());
        return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
    });
}

static bool copyMayLetObjectFallAwayAtInlining(const Walker& walker, FunctionID callee, const std::span<const Reference>* passedOperands)
{
    if (!passedOperands)
        return true;
    if (walker.facts().summaries().at(callee).makesObjects || walker.program().at(callee).has(FunctionFlags::HasRestParameter))
        return true;
    return passesFreshObject(walker, *passedOperands);
}

static bool isOnlyCalledAtInlining(const BodyIndex& body, ValueID parameter)
{
    uint32_t calls = 0;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::Call && operation.calleeOperand() == parameter)
                ++calls;
        }
    }
    return calls && calls == body.useCount(parameter);
}

static ALWAYS_INLINE bool onlyHandsFunctionToHelperThatCallsItAtInlining(const Walker& walker, FunctionID forwarder)
{
    const ThinSummary& summary = walker.facts().summaries().at(forwarder);
    if (summary.tailCallThatIsOnlyCall.isEmpty() || summary.has(SummaryTrait::HasLoops))
        return false;
    const SummaryTailCallThatIsOnlyCall& call = summary.tailCallThatIsOnlyCall[0];
    if (summary.numberOfOperations > inlineLimitOutsideLoops + call.numberOfArguments)
        return false;
    const LinkTables& link = walker.facts().linked();
    FunctionID helper = link.calleeAt(forwarder, call.site);
    if (!helper || helper == forwarder)
        return false;
    const ThinSummary& summaryOfHelper = walker.facts().summaries().at(helper);
    if (summaryOfHelper.has(SummaryTrait::HasLoops) || summaryOfHelper.numberOfOperations > inlineLimitInLoops)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(forwarder);
    const BodyIndex* bodyOfHelper = walker.facts().bodies().find(helper);
    if (!body || !bodyOfHelper || body->opcodeOf(call.site) != Opcode::Call)
        return false;

    const Function& function = walker.program().at(helper);
    BodyBlock entry = bodyOfHelper->block(entryBlock);
    std::span<const ValueID> written = body->value(call.site).as<Opcode::Call>().argumentsOperands();
    for (unsigned index = function.firstParameterIndex(); index < function.numberOfEntryParameters() && index < entry.numberOfParameters() && index - function.firstParameterIndex() < written.size(); ++index) {
        if (function.entryParameterKind(index) != EntryParameter::Parameter)
            continue;
        ValueID argument = body->stripCasts(written[index - function.firstParameterIndex()]);
        while (argument && !body->isParameter(argument) && body->opcodeOf(argument) == Opcode::CheckInitialized)
            argument = body->stripCasts(body->value(argument).operand(0));
        if (!argument || body->isParameter(argument))
            continue;
        bool isOneFunction = body->opcodeOf(argument) == Opcode::NewClosure;
        if (body->opcodeOf(argument) == Opcode::GetModuleVariable) {
            TypeID proven = link.variable(body->value(argument).as<Opcode::GetModuleVariable>().variable()).proven;
            Type type = walker.types().at(proven ? proven : TypeID(TypeTable::anything));
            isOneFunction = type.isObject() && type.isJavaScriptValue() && singleFunctionOf(walker.types(), type);
        }
        if (isOneFunction && isOnlyCalledAtInlining(*bodyOfHelper, entry.parameter(index)))
            return true;
    }
    return false;
}

static bool copyIsDrawnAwayAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place)
{
    return walker.drawChoice(DrawnFor::CopyInLine, place.caller.index(), callee.index(), false) == Walker::ChoiceOfCost::No;
}

static bool passesTypedArrayElement(const Walker& walker, const std::span<const Reference>* passedOperands)
{
    if (!passedOperands)
        return true;
    return std::ranges::any_of(*passedOperands, [&](Reference operand) {
        return operand && operand.isValue() && !walker.code().isConstant(operand.id()) && walker.code().at(operand.id()).opcode == Opcode::GetTypedArrayElement;
    });
}

static bool readsFieldOf(const BodyIndex& body, ValueID parameter)
{
    for (uint32_t number = 0; body.opcodes().contains(Opcode::PropGet) && number < body.numberOfBlocks(); ++number) {
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::PropGet && body.stripCasts(operation.objectOperand()) == parameter)
                return true;
        }
    }
    return false;
}

static bool onlyCalculates(const Walker& walker, FunctionID leaf, const BodyIndex& body, uint32_t& numberOfOperations, unsigned depth = 0)
{
    static constexpr unsigned maxDepth = 2;
    numberOfOperations += walker.facts().summaries().at(leaf).numberOfOperations;
    static constexpr OpcodeSet calculations = allNumericOperators | allRelations | allEqualities | opcodesThatStateTypes | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::ToBoolean, Opcode::LogicalNot, Opcode::Undefined, Opcode::Builtin, Opcode::Call, Opcode::GetModuleVariable, Opcode::CheckInitialized, Opcode::PropGet };
    if ((body.opcodes() | calculations) != calculations || body.hasLoops())
        return false;
    if (!body.holdsAnyOf(OpcodeSet { Opcode::Call, Opcode::GetModuleVariable, Opcode::PropGet }))
        return true;
    const LinkTables& linked = walker.facts().linked();
    const TypeArena& types = walker.types();
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::Call && !isMathCall(walker, body, operation)) {
                FunctionID callee = linked.calleeAt(leaf, operation.id());
                const BodyIndex* called = callee && callee != leaf && depth < maxDepth ? walker.facts().bodies().find(callee) : nullptr;
                if (!called || !onlyCalculates(walker, callee, *called, numberOfOperations, depth + 1))
                    return false;
            } else if (operation.opcode() == Opcode::PropGet) {
                ValueID object = operation.isFreeName() ? ValueID() : body.stripCasts(operation.objectOperand());
                if (!object || !body.isParameter(object) || body.blockOf(object) != entryBlock)
                    return false;
            } else if (operation.opcode() == Opcode::GetModuleVariable) {
                TypeID proven = linked.variable(operation.as<Opcode::GetModuleVariable>().variable()).proven;
                Type type = types.at(proven ? proven : TypeID(TypeTable::anything));
                type.primitives &= ~Primitive::Absent;
                if (!type.isNumber() && !singleFunctionOf(types, type))
                    return false;
            }
        }
    }
    return true;
}

static bool onlyCalculatesInWindow(const Walker& walker, FunctionID leaf, const BodyIndex& body)
{
    uint32_t numberOfOperations = 0;
    return onlyCalculates(walker, leaf, body, numberOfOperations) && numberOfOperations <= inlineWindowOfWrittenCallInLoops;
}

static bool calculatesWithPassedNumbers(const Walker& walker, FunctionID leaf, const std::span<const Reference>* passedOperands)
{
    const BodyIndex* body = walker.facts().bodies().find(leaf);
    if (!passedOperands || !body || (walker.currentBlock() && walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare))
        return false;
    BodyBlock entry = body->block(entryBlock);
    bool takesNumber = false;
    for (uint32_t position = 0; position < entry.numberOfParameters(); ++position) {
        if (!body->useCount(entry.parameter(position)))
            continue;
        Reference passed = position < passedOperands->size() ? (*passedOperands)[position] : Reference();
        TypeID type = passed ? walker.typeOf(passed) : TypeID();
        if (type && readsFieldOf(*body, entry.parameter(position))) {
            if (!singleLayoutOf(walker.types(), walker.types().at(type)))
                return false;
            continue;
        }
        if (!type || !walker.types().overlap(type, TypeTable::number))
            return false;
        takesNumber = true;
    }
    return takesNumber && onlyCalculatesInWindow(walker, leaf, *body);
}

bool passesValueToCalculation(const Walker& walker, const BodyValue& call, ValueID value)
{
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id());
    const BodyIndex* body = callee ? walker.facts().bodies().find(callee) : nullptr;
    if (!body || !onlyCalculatesInWindow(walker, callee, *body))
        return false;
    const Function& function = walker.program().at(callee);
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    size_t position = std::ranges::find(arguments, value) - arguments.begin();
    if (position >= arguments.size() || position >= function.numberOfPositionalParameters())
        return false;
    ValueID parameter = body->block(entryBlock).parameter(function.firstParameterIndex() + position);
    auto isWrittenNumber = [&](ValueID operand) {
        return !body->isParameter(operand) && (body->opcodeOf(operand) == Opcode::Integer || body->opcodeOf(operand) == Opcode::Double);
    };
    auto suitsNumbers = [&](const BodyValue& operation) {
        if (numberProducingOperators.contains(operation.opcode()) || allBitwiseOperators.contains(operation.opcode()))
            return true;
        return (operation.opcode() == Opcode::Add || allRelations.contains(operation.opcode())) && (isWrittenNumber(operation.operand(0)) || isWrittenNumber(operation.operand(1)));
    };
    Vector<ValueID, 8> names { parameter };
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            bool takesValue = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) { takesValue |= names.contains(body->stripCasts(operand)); });
            if (!takesValue || opcodesThatStateTypes.contains(operation.opcode()))
                continue;
            if (suitsNumbers(operation))
                return true;
            if (operation.opcode() == Opcode::Add)
                names.append(operation.id());
        }
    }
    return false;
}

static bool leafReadsElementsOfHolderFromInFrontOfLoopAtInlining(const Walker& walker, FunctionID leaf, const std::span<const Reference>* passedOperands)
{
    const BodyIndex* body = walker.facts().bodies().find(leaf);
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    if (!passedOperands || !body || !loop || !body->holdsAnyOf(OpcodeSet { Opcode::ElemGet, Opcode::IteratorOpen }))
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue read : body->block(BlockID(number)).values()) {
            bool takesApart = read.opcode() == Opcode::IteratorOpen && read.as<Opcode::IteratorOpen>().isDestructuring() && !read.as<Opcode::IteratorOpen>().hasRest();
            if (read.opcode() != Opcode::ElemGet && !takesApart)
                continue;
            std::optional<uint32_t> position = entry.indexOfParameter(body->stripCasts(takesApart ? read.as<Opcode::IteratorOpen>().iterableOperand() : read.objectOperand()));
            if (position && *position < passedOperands->size() && (*passedOperands)[*position] && (takesApart || walker.standsInFrontOfOpenLoop(*loop, (*passedOperands)[*position])))
                return true;
        }
    }
    return false;
}

static bool readsParameterElements(const BodyIndex& body)
{
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue read : body.block(BlockID(number)).values()) {
            if (read.opcode() == Opcode::ElemGet && entry.indexOfParameter(body.stripCasts(read.objectOperand())))
                return true;
        }
    }
    return false;
}

static std::optional<uint32_t> findConstantTripCount(const BodyIndex& body, LoopID loop)
{
    BlockID head = body.loop(loop).head;
    BodyTerminator end = body.terminator(head);
    ValueID asked = end.kind() == TerminatorKind::Branch ? end.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    if (!asked || body.isParameter(asked) || (body.opcodeOf(asked) != Opcode::Less && body.opcodeOf(asked) != Opcode::LessEq))
        return std::nullopt;
    auto integerOf = [&](ValueID value) -> std::optional<int64_t> {
        value = body.stripCasts(value);
        if (!value || body.isParameter(value) || body.opcodeOf(value) != Opcode::Integer)
            return std::nullopt;
        return body.value(value).integer();
    };
    ValueID counter = body.stripCasts(body.value(asked).operand(0));
    std::optional<int64_t> bound = integerOf(body.value(asked).operand(1));
    if (!bound || !counter || !body.isParameter(counter) || body.blockOf(counter) != head)
        return std::nullopt;
    uint32_t position = counter.index() - body.block(head).firstParameter().index();
    std::optional<int64_t> start;
    bool isCounted = true;
    for (uint32_t number = 0; number < body.numberOfBlocks() && isCounted; ++number) {
        body.block(BlockID(number)).forEachOutgoingEdge([&](BodyTarget target, bool) {
            if (target.block() != head)
                return;
            ValueID brought = position < target.arguments().size() ? body.stripCasts(target.arguments()[position]) : ValueID();
            if (body.isInLoop(BlockID(number), loop)) {
                isCounted &= brought && !body.isParameter(brought) && body.opcodeOf(brought) == Opcode::Increment && body.stripCasts(body.value(brought).operand(0)) == counter;
                return;
            }
            isCounted &= !start;
            start = brought ? integerOf(brought) : std::nullopt;
        });
    }
    if (!isCounted || !start)
        return std::nullopt;
    int64_t trips = *bound - *start + (body.opcodeOf(asked) == Opcode::LessEq);
    return trips > 0 && trips <= inlineLimitInLoops ? std::optional { static_cast<uint32_t>(trips) } : std::nullopt;
}

std::optional<uint32_t> Walker::constantTripCountOf(uint32_t openLoop) const
{
    const OpenLoop& loop = m_openLoops[openLoop];
    const Frame& frame = m_frames[loop.frame];
    return frame.index && loop.loop ? findConstantTripCount(*frame.index, loop.loop) : std::nullopt;
}

static bool isShortNest(const Walker& walker, FunctionID callee, const BodyIndex& body, uint32_t numberOfOperations)
{
    if (body.numberOfLoops() > 2)
        return false;
    Vector<uint32_t, 2> trips;
    for (uint32_t loop = 0; loop < body.numberOfLoops(); ++loop) {
        std::optional<uint32_t> found = findConstantTripCount(body, LoopID(loop));
        if (!found)
            return false;
        trips.append(*found);
    }
    uint32_t size = numberOfOperations;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        uint32_t times = 1;
        for (LoopID loop = body.loopOf(BlockID(number)); loop; loop = body.loop(loop).parent)
            times *= trips[loop.index()];
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::CallDirect || operation.opcode() == Opcode::CallList || operation.opcode() == Opcode::Construct)
                return false;
            if (operation.opcode() != Opcode::Call)
                continue;
            FunctionID called = walker.facts().linked().calleeAt(callee, operation.id());
            if (!called || walker.facts().summaries().at(called).has(SummaryTrait::HasLoops))
                return false;
            size += times * walker.facts().summaries().at(called).numberOfOperations;
        }
        if (size > inlineWindowOfWrittenCallInLoops)
            return false;
    }
    return true;
}

static bool readsHolderFieldElements(const Walker& walker, const BodyIndex& body, const std::span<const Reference>* passedOperands)
{
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    if (passedOperands && !loop)
        return false;
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        for (BodyValue read : body.block(BlockID(number)).values()) {
            if (read.opcode() != Opcode::ElemGet)
                continue;
            ValueID field = body.stripCasts(read.objectOperand());
            if (!field || body.isParameter(field) || body.opcodeOf(field) != Opcode::PropGet || body.value(field).isFreeName())
                continue;
            std::optional<uint32_t> position = entry.indexOfParameter(body.stripCasts(body.value(field).objectOperand()));
            if (!position)
                continue;
            Reference holder = passedOperands && *position < passedOperands->size() ? (*passedOperands)[*position] : Reference();
            if (!passedOperands || (holder && walker.standsInFrontOfOpenLoop(*loop, holder)))
                return true;
        }
    }
    return false;
}

static bool isSmallFieldLoop(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    static constexpr uint32_t maxCallSites = 2;
    static constexpr OpcodeSet calls { Opcode::Call, Opcode::CallDirect, Opcode::CallList, Opcode::Construct, Opcode::SuperConstruct, Opcode::NewClosure, Opcode::NewObject, Opcode::NewArray };
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const LinkedFunction& linked = walker.facts().linked().function(callee);
    if (!place.isInLoop || !summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoopsWithoutObjects || !linked.numberOfCallSites || linked.numberOfCallSites > maxCallSites)
        return false;
    if (place.caller != walker.unit().function && (place.caller != walker.currentFunction() || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCallInLoops))
        return false;
    if (walker.standsUnderHandler() || (place.body && place.body->block(place.body->blockOf(place.call)).hasHandler()))
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    return body && body->numberOfLoops() == 1 && !body->holdsAnyOf(calls) && readsHolderFieldElements(walker, *body, passedOperands);
}

bool canInlineSingleCallLoop(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    if (isSmallFieldLoop(walker, callee, place, passedOperands))
        return true;
    static constexpr OpcodeSet callsOfValues { Opcode::Call, Opcode::CallDirect, Opcode::CallList, Opcode::Construct };
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!place.isInLoop || place.caller != walker.unit().function || !summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoops || !walker.facts().linked().function(callee).hasSingleCallSite)
        return false;
    if (walker.standsUnderHandler() || (place.body && place.body->block(place.body->blockOf(place.call)).hasHandler()))
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    if (body->numberOfLoops() != 1 || body->holdsAnyOf(callsOfValues)) {
        if (!isShortNest(walker, callee, *body, summary.numberOfOperations))
            return false;
        if (!passedOperands)
            return true;
        std::optional<uint32_t> loop = walker.innermostOpenLoop();
        return loop && std::ranges::any_of(*passedOperands, [&](Reference passed) {
            return passed && passed.isValue() && walker.standsInFrontOfOpenLoop(*loop, passed) && walker.types().at(walker.typeOf(passed)).hasObjects();
        });
    }
    return passedOperands ? leafReadsElementsOfHolderFromInFrontOfLoopAtInlining(walker, callee, passedOperands) : readsParameterElements(*body);
}

static bool passesStringLiteralKey(const Walker& walker, FunctionID callee, const std::span<const Reference>* passedOperands, bool inLoopOfCallee = false)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || (inLoopOfCallee && !passedOperands))
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = 0; position < entry.numberOfParameters(); ++position) {
        if (passedOperands) {
            Reference operand = position < passedOperands->size() ? (*passedOperands)[position] : Reference();
            if (!operand || !operand.isConstant() || walker.code().at(operand.id()).opcode != Opcode::String)
                continue;
        }
        ValueID parameter = entry.parameter(position);
        for (uint32_t number = 0; number < body->numberOfValues(); ++number) {
            ValueID read(number);
            if (body->isParameter(read) || (body->opcodeOf(read) != Opcode::ElemGet && (!inLoopOfCallee || body->opcodeOf(read) != Opcode::ElemSet)) || body->stripCasts(body->value(read).propertyOperand()) != parameter)
                continue;
            if (!inLoopOfCallee)
                return true;
            if (!body->loopOf(body->blockOf(read)))
                continue;
            ValueID holder = body->stripCasts(body->value(read).objectOperand());
            if (!body->isParameter(holder) && body->opcodeOf(holder) == Opcode::ElemGet)
                holder = body->stripCasts(body->value(holder).objectOperand());
            std::optional<uint32_t> handed = entry.indexOfParameter(holder);
            Reference given = handed && *handed < passedOperands->size() ? (*passedOperands)[*handed] : Reference();
            if (given && !walker.types().isUninformative(walker.typeOf(given)))
                return true;
        }
    }
    return false;
}

static bool calleeAsksForClassOfWhatItIsHandedAtInlining(const Walker& walker, FunctionID callee)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->holdsAnyOf(OpcodeSet { Opcode::InstanceOf }))
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue test : body->block(BlockID(number)).values()) {
            if (test.opcode() != Opcode::InstanceOf || !entry.indexOfParameter(body->stripCasts(test.valueOperand())))
                continue;
            ValueID asked = body->stripCasts(test.constructorOperand());
            if (!body->isParameter(asked) && body->opcodeOf(asked) == Opcode::CheckInitialized)
                asked = body->stripCasts(body->value(asked).operand(0));
            if (body->isParameter(asked) || body->opcodeOf(asked) != Opcode::GetModuleVariable)
                continue;
            TypeID held = walker.positions().variable(TypeLevel::Proven, body->value(asked).variable());
            if (held && canDecideInstanceOfByHierarchy(walker, walker.types().withoutPrimitives(held, Primitive::Absent)))
                return true;
        }
    }
    return false;
}

static bool loopOfLeafStoresParameterFields(const Walker& walker, FunctionID callee)
{
    static constexpr OpcodeSet callsOfValues { Opcode::Call, Opcode::CallDirect, Opcode::CallList, Opcode::Construct };
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || body->numberOfLoops() != 1 || body->holdsAnyOf(callsOfValues) || !body->holdsAnyOf(OpcodeSet { Opcode::PropSet }))
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        LoopID loop = body->loopOf(BlockID(number));
        if (!loop)
            continue;
        if (body->block(body->loop(loop).head).numberOfParameters())
            return false;
        for (BodyValue store : body->block(BlockID(number)).values()) {
            if (store.opcode() == Opcode::PropSet && !store.isFreeName() && entry.indexOfParameter(body->stripCasts(store.objectOperand())))
                return true;
        }
    }
    return false;
}

static bool isInRecursionWithoutLoops(const Walker& walker)
{
    return !walker.currentBodyIndex().hasLoops() && walker.isCurrentFrameRecursive();
}

static bool returnsNumberAtInlining(const Walker& walker, FunctionID callee)
{
    TypeID result = walker.facts().linked().function(callee).result;
    return result && walker.types().at(result).isNumber();
}

static uint32_t throwingOperations(const BodyIndex& body)
{
    uint32_t number = 0;
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        if (!body.isReachable(BlockID(block)) || body.terminator(BlockID(block)).kind() != TerminatorKind::Throw || body.block(BlockID(block)).hasHandler())
            continue;
        for (BodyValue operation : body.block(BlockID(block)).values())
            number += !opcodesThatStateTypes.contains(operation.opcode());
    }
    return number;
}

static uint32_t operationsOfFailingChecksOfPrivateNames(const BodyIndex& body)
{
    if (!body.holdsAnyOf(OpcodeSet { Opcode::NewRuntimeError }))
        return 0;
    uint32_t number = 0;
    bool checksPrivateNames = false;
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        TerminatorKind end = body.isReachable(BlockID(block)) ? body.terminator(BlockID(block)).kind() : TerminatorKind::Return;
        if (end != TerminatorKind::Throw && end != TerminatorKind::Unreachable)
            continue;
        for (BodyValue operation : body.block(BlockID(block)).values()) {
            bool isCheck = operation.opcode() == Opcode::NewRuntimeError && (operation.runtimeError() == RuntimeErrorKind::InvalidPrivateRead || operation.runtimeError() == RuntimeErrorKind::InvalidPrivateWrite);
            checksPrivateNames |= isCheck;
            number += isCheck || end == TerminatorKind::Unreachable;
        }
    }
    return checksPrivateNames ? number : 0;
}

static uint32_t comparisonsOfOneParameterWithConstants(const BodyIndex& body)
{
    static constexpr OpcodeSet comparisons { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq };
    Vector<uint32_t, 8> numbers;
    numbers.fill(0, body.block(entryBlock).numberOfParameters());
    uint32_t greatest = 0;
    for (uint32_t block = 0; block < body.numberOfBlocks(); ++block) {
        for (BodyValue operation : body.block(BlockID(block)).values()) {
            if (!comparisons.contains(operation.opcode()))
                continue;
            ValueID left = body.stripCasts(operation.operand(0));
            ValueID right = body.stripCasts(operation.operand(1));
            if (left && right && body.isParameter(left) && body.blockOf(left) == entryBlock && left.index() < numbers.size() && !body.isParameter(right) && body.opcodeOf(right) == Opcode::Integer)
                greatest = std::max(greatest, ++numbers[left.index()]);
        }
    }
    return greatest;
}

static bool takesListAtInlining(const Function& callee, unsigned entryParameter);

static constexpr uint32_t maxRestArrayReads = 12;

static NEVER_INLINE uint32_t restArrayReads(const Walker& walker, FunctionID callee)
{
    const Function& function = walker.program().at(callee);
    const BodyIndex* body = function.has(FunctionFlags::HasRestParameter) ? walker.facts().bodies().find(callee) : nullptr;
    if (!body)
        return 0;
    uint32_t number = 0;
    for (uint32_t block = 0; block < body->numberOfBlocks(); ++block) {
        for (BodyValue read : body->block(BlockID(block)).values()) {
            if (read.opcode() != Opcode::ElemGet && (read.opcode() != Opcode::PropGet || read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::length)))
                continue;
            ValueID list = body->stripCasts(read.objectOperand());
            std::optional<uint32_t> parameter = body->isParameter(list) ? body->block(entryBlock).indexOfParameter(list) : std::nullopt;
            number += parameter && takesListAtInlining(function, *parameter);
        }
    }
    return std::min(number, maxRestArrayReads);
}

static bool fitsLoopWithoutRestArray(const Walker& walker, FunctionID callee, uint32_t numberOfOperations)
{
    return numberOfOperations <= inlineLimitInLoops || (numberOfOperations <= inlineLimitInLoops + maxRestArrayReads && numberOfOperations - restArrayReads(walker, callee) <= inlineLimitInLoops);
}

static std::optional<uint32_t> operationsForPassedBuiltin(const Walker& walker, FunctionID callee, std::span<const Reference> passedOperands)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return std::nullopt;
    const TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    BodyBlock entry = body->block(entryBlock);
    bool comparesPassedBuiltin = false;
    auto typePassedTo = [&](ValueID operand) -> Type {
        std::optional<uint32_t> parameter = entry.indexOfParameter(body->stripCasts(operand));
        Reference passed = parameter && *parameter < passedOperands.size() ? passedOperands[*parameter] : Reference();
        TypeID type = passed ? walker.typeOf(passed) : TypeID();
        return type ? types.at(type) : Type();
    };
    auto answerOf = [&](ValueID condition) -> std::optional<bool> {
        while (!body->isParameter(condition) && body->opcodeOf(condition) == Opcode::ToBoolean)
            condition = body->value(condition).operand(0);
        if (body->isParameter(condition))
            return std::nullopt;
        BodyValue question = body->value(condition);
        if (question.opcode() == Opcode::HasJSType) {
            Type passed = typePassedTo(question.operand(0));
            const ClassPart* part = passed.isObject() ? singleBuiltinClassOf(types, passed) : nullptr;
            if (!part)
                return std::nullopt;
            JSType type = table.at(part->builtinClass).type;
            auto asked = question.as<Opcode::HasJSType>();
            return type >= toJSType(asked.first()) && type <= toJSType(asked.last());
        }
        if (question.opcode() != Opcode::StrictEq)
            return std::nullopt;
        ValueID compared = question.operand(1);
        if (body->isParameter(compared) || body->opcodeOf(compared) != Opcode::Builtin)
            return std::nullopt;
        Type passed = typePassedTo(question.operand(0));
        BuiltinID builtin = passed.isObject() ? singleBuiltinOf(types, passed) : BuiltinID();
        if (!builtin)
            return std::nullopt;
        comparesPassedBuiltin = true;
        return table.canonicalID(builtin) == table.canonicalID(body->value(compared).as<Opcode::Builtin>().builtin());
    };
    uint32_t number = 0;
    Vector<BlockID, 16> worklist { entryBlock };
    BitVector isVisited(body->numberOfBlocks());
    isVisited.set(entryBlock.index());
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        for (BodyValue operation : body->block(block).values())
            number += !opcodesThatStateTypes.contains(operation.opcode());
        BodyTerminator end = body->terminator(block);
        std::optional<bool> answer = end.kind() == TerminatorKind::Branch ? answerOf(end.operand(0)) : std::nullopt;
        uint32_t targetIndex = 0;
        for (BodyTarget target : end.targets()) {
            if ((answer && targetIndex++ != !*answer) || isVisited.set(target.block().index()))
                continue;
            worklist.append(target.block());
        }
    }
    return comparesPassedBuiltin ? std::optional<uint32_t>(number) : std::nullopt;
}

ValueID findWrittenArgument(const BodyValue& call, const Function& callee, uint32_t entryParameter)
{
    if (call.opcode() == Opcode::CallDirect) {
        std::span<const ValueID> operands = call.variadicOperands();
        return entryParameter < operands.size() ? operands[entryParameter] : ValueID();
    }
    if (call.opcode() != Opcode::Call || callee.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return { };
    std::span<const ValueID> arguments = call.argumentsOperands();
    uint32_t position = entryParameter - callee.firstParameterIndex();
    return position < arguments.size() ? arguments[position] : ValueID();
}

static bool matchesParameter(const Walker& walker, const BodyIndex& body, ValueID parameter)
{
    const Program& program = walker.program();
    const BuiltinName test = program.builtins().nameOf(byteCast<uint8_t>(("test"_s).span()));
    const std::array methods { program.keyOf(BuiltinNames::exec), test ? program.keyOf(test) : program.keyOf(BuiltinNames::exec) };
    for (uint32_t number = 0; number < body.numberOfValues(); ++number) {
        ValueID id(number);
        if (body.isParameter(id) || body.opcodeOf(id) != Opcode::Call)
            continue;
        auto call = body.value(id).as<Opcode::Call>();
        if (!call.thisValueOperand() || body.stripCasts(call.thisValueOperand()) != parameter)
            continue;
        ValueID method = body.stripCasts(call.calleeOperand());
        if (!body.isParameter(method) && body.opcodeOf(method) == Opcode::PropGet && std::ranges::find(methods, body.value(method).key()) != methods.end())
            return true;
    }
    return false;
}

static bool copyMatchesKnownPattern(const Walker& walker, FunctionID callee, const PlaceOfCall& place)
{
    if (!place.isInLoop || !place.body || place.caller != walker.unit().function || place.body != &walker.currentBodyIndex() || place.body->opcodeOf(place.call) != Opcode::Call || walker.facts().summaries().at(callee).numberOfOperations > inlineLimitInLoops)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    const BodyValue call = place.body->value(place.call);
    const Function& function = walker.program().at(callee);
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = static_cast<uint32_t>(function.captures.size()); position < entry.numberOfParameters(); ++position) {
        if (findRegExpPassed(walker, findWrittenArgument(call, function, position)) && matchesParameter(walker, *body, entry.parameter(position)))
            return true;
    }
    return false;
}

static bool isResultOnlyRead(const BodyIndex& body, ValueID call)
{
    static constexpr uint32_t mostOperationsLookedAt = 40;
    BodyBlock block = body.block(body.blockOf(call));
    uint32_t end = std::min(block.endOfValues().index(), call.index() + 1 + mostOperationsLookedAt);
    Vector<ValueID, 4> names { call };
    uint32_t usesToCome = body.useCount(call);
    if (!usesToCome)
        return false;
    for (uint32_t number = call.index() + 1; number < end && usesToCome; ++number) {
        BodyValue operation = body.value(ValueID(number));
        Opcode opcode = operation.opcode();
        bool isRead = true;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operand || !names.contains(operand))
                return;
            --usesToCome;
            if (opcodesThatStateTypes.contains(opcode)) {
                names.append(operation.id());
                usesToCome += body.useCount(operation.id());
            } else
                isRead &= opcode == Opcode::PropGet || (opcode == Opcode::Call && operandIndex);
        });
        if (!isRead)
            return false;
    }
    return !usesToCome;
}

static bool isResultStoredInto(const BodyIndex& body, ValueID call)
{
    static constexpr uint32_t mostOperationsLookedAt = 12;
    uint32_t end = std::min(body.block(body.blockOf(call)).endOfValues().index(), call.index() + 1 + mostOperationsLookedAt);
    for (uint32_t number = call.index() + 1; number < end; ++number) {
        BodyValue operation = body.value(ValueID(number));
        if (operation.opcode() == Opcode::PropSet && body.stripCasts(operation.objectOperand()) == call)
            return true;
    }
    return false;
}

static bool isConstructorWithoutCalls(const Walker& walker, FunctionID callee)
{
    static constexpr OpcodeSet calls { Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
    const Function& function = walker.program().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    bool isConstructor = isClassConstructorOf(function.kind) && function.constructedClass;
    if (!body || body->holdsAnyOf(calls) || (!isConstructor && function.kind != FunctionKind::Initializer))
        return false;
    if (!body->holdsAnyOf(OpcodeSet { Opcode::CallDirect }))
        return true;
    for (uint32_t number = 0; isConstructor && number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::CallDirect && (walker.program().at(operation.function()).kind != FunctionKind::Initializer || !isConstructorWithoutCalls(walker, operation.function())))
                return false;
        }
    }
    return isConstructor;
}

static bool passedTypesDecideOperations(const Walker& walker, FunctionID callee, std::span<const Reference> passedOperands)
{
    static constexpr OpcodeSet writtenNumbers { Opcode::Integer, Opcode::Double };
    static constexpr OpcodeSet writtenValues { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::String, Opcode::Builtin, Opcode::GetModuleVariable, Opcode::NewObject };
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    const TypeArena& types = walker.types();
    const LinkTables& link = walker.facts().linked();
    Vector<TypeID> known;
    known.fill(TypeID(), body->numberOfValues());
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = 0; position < std::min<size_t>(entry.numberOfParameters(), passedOperands.size()); ++position) {
        if (passedOperands[position])
            known[entry.parameter(position).index()] = walker.typeOf(passedOperands[position]);
    }
    auto isNumber = [&](ValueID value) {
        TypeID type = value ? known[value.index()] : TypeID();
        return type && types.at(type).isNumber();
    };
    auto fieldOf = [&](ValueID object, PropertyKeyID key) -> std::optional<FieldReference> {
        TypeID type = object && key ? known[object.index()] : TypeID();
        if (!type || (types.at(type).primitives & ~Primitive::UndefinedOrNull) || !types.at(type).hasOnlyLayouts())
            return std::nullopt;
        LayoutID layout = singleLayoutOf(types, types.at(type));
        if (!layout || !walker.plan().isInUse(layout))
            return std::nullopt;
        std::optional<FieldReference> field = findField(walker.program(), layout, key);
        return field && link.field(*field).isFilledAtEveryRead ? field : std::nullopt;
    };
    auto isCalleeOfKnownCall = [&](ValueID read) {
        for (BodyValue operation : body->block(body->blockOf(read)).values()) {
            if (operation.opcode() == Opcode::Call && body->stripCasts(operation.calleeOperand()) == read)
                return !!link.calleeAt(callee, operation.id());
        }
        return false;
    };
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            Opcode opcode = operation.opcode();
            TypeID& result = known[operation.id().index()];
            if (opcodesThatStateTypes.contains(opcode) || opcode == Opcode::NarrowByTest || opcode == Opcode::ToThis || opcode == Opcode::CheckInitialized)
                result = known[operation.operand(0).index()];
            else if (writtenNumbers.contains(opcode))
                result = TypeTable::number;
            else if (writtenValues.contains(opcode))
                continue;
            else if (opcode == Opcode::PropGet) {
                if (operation.isFreeName())
                    return false;
                if (std::optional<FieldReference> field = fieldOf(operation.objectOperand(), operation.key()))
                    result = link.field(*field).proven;
                else if (!isCalleeOfKnownCall(operation.id()) && (body->isParameter(operation.objectOperand()) || body->opcodeOf(operation.objectOperand()) != Opcode::Builtin))
                    return false;
            } else if (opcode == Opcode::PropSet) {
                if (!fieldOf(operation.objectOperand(), operation.key()))
                    return false;
            } else if (opcode == Opcode::PrivateGet || opcode == Opcode::PrivateSet) {
                FieldReference field { operation.layout(), operation.field() };
                TypeID type = known[operation.objectOperand().index()];
                if (!type || !types.at(type).hasOnlyLayouts() || singleLayoutOf(types, types.at(type)) != field.layout)
                    return false;
                if (opcode == Opcode::PrivateGet)
                    result = link.field(field).proven;
            } else if (opcode == Opcode::Call || opcode == Opcode::Construct) {
                FunctionID called = link.calleeAt(callee, operation.id());
                if (called)
                    result = walker.plan().typeHeldOfResult(walker.program(), link, called);
                else if (opcode == Opcode::Construct || body->isParameter(operation.calleeOperand()) || body->opcodeOf(body->stripCasts(operation.calleeOperand())) != Opcode::Builtin)
                    return false;
                else
                    result = TypeTable::number;
            } else if (allNumericOperators.contains(opcode) || allRelations.contains(opcode)) {
                bool everyOperandIsNumber = true;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    everyOperandIsNumber &= isNumber(operand);
                });
                if (!everyOperandIsNumber)
                    return false;
                if (allNumericOperators.contains(opcode))
                    result = TypeTable::number;
            } else
                return false;
        }
    }
    return true;
}

static bool constructsForOtherClasses(const Walker& walker, FunctionID constructor)
{
    const Function& function = walker.program().at(constructor);
    const LinkTables& link = walker.facts().linked();
    const LinkedFunction& linked = link.function(constructor);
    unsigned newTarget = function.numberOfEntryParametersBefore(EntryParameter::NewTarget);
    if (!function.has(FunctionFlags::HasNewTarget) || newTarget >= linked.numberOfParameters)
        return false;
    TypeID proven = link.parameters[linked.firstParameter + newTarget].proven;
    return !proven || !singleFunctionOf(walker.types(), walker.types().at(proven));
}

static bool isCalledByEveryNestedIteration(const Walker& walker, const PlaceOfCall& place)
{
    return place.body && place.call && place.caller == walker.unit().function && walker.facts().linked().functionsDoneInNestedIterations.get(place.caller.index())
        && place.body->dominates(place.body->blockOf(place.call), walker.lastReturnDominator(*place.body));
}

static bool copyPaysInIterations(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    bool isInLoopsOfOwnFile = walker.facts().linked().functionsCalledInLoopsOfOwnFile.get(walker.unit().function.index());
    if (!isInLoopsOfOwnFile && !walker.facts().linked().functionsDoneInNestedIterations.get(walker.unit().function.index()))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (summary.has(SummaryTrait::HasLoops))
        return false;
    if (summary.numberOfOperations <= inlineLimitInNestedIterations && isCalledByEveryNestedIteration(walker, place) && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations))
        return true;
    if (!isInLoopsOfOwnFile)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    bool makesObject = body && body->holdsAnyOf(OpcodeSet { Opcode::Construct, Opcode::NewObject });
    bool objectMayBeEliminated = makesObject && place.body && place.call && isResultOnlyRead(*place.body, place.call);
    if (place.body && place.call && summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations) && isConstructorWithoutCalls(walker, callee) && isResultStoredInto(*place.body, place.call))
        return true;
    if (summary.numberOfOperations <= inlineLimitInLoops && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations)
        && isConstructorWithoutCalls(walker, callee) && constructsForOtherClasses(walker, callee))
        return true;
    return summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations)
        && (passedOperands ? passedTypesDecideOperations(walker, callee, *passedOperands) : objectMayBeEliminated);
}

struct PartOfCalleeLeftByTypes {
    uint32_t numberOfOperations { 0 };
};

static std::optional<uint16_t> primitivesNamedByTypeOfAtInlining(const Type& type)
{
    for (uint16_t primitives : { Primitive::Number, Primitive::AnyString, Primitive::Boolean, Primitive::AnySymbol }) {
        if (type.hasOnlyPrimitives(primitives))
            return primitives;
    }
    return std::nullopt;
}

static bool holdsEveryPrimitiveAtInlining(const Type& asked, const Type& passed)
{
    return !passed.hasObjects() && !(passed.primitives & ~asked.primitives) && (!(passed.primitives & Primitive::Integer) || passed.integers.isSubsetOf(asked.integers));
}

static std::optional<PartOfCalleeLeftByTypes> findPartLeftByPassedTypes(const Walker& walker, FunctionID callee, std::span<const Reference> passedOperands, size_t mostOperations)
{
    static constexpr OpcodeSet constants { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };
    const TypeArena& types = walker.types();
    bool passesKnownType = std::ranges::any_of(passedOperands, [&](Reference operand) {
        TypeID type = operand ? walker.typeOf(operand) : TypeID();
        return type && !types.isUninformative(type);
    });
    if (!passesKnownType)
        return std::nullopt;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->holdsAnyOf(OpcodeSet { Opcode::TypeTest, Opcode::TypeOf }))
        return std::nullopt;
    Vector<TypeID> passedTypes;
    Vector<ValueID> named;
    passedTypes.fill(TypeID(), body->numberOfValues());
    named.fill(ValueID(), body->numberOfValues());
    auto sourceOf = [&](ValueID value) {
        while (value && named[value.index()])
            value = named[value.index()];
        return value;
    };
    auto typeOf = [&](ValueID value) -> Type {
        ValueID source = value ? sourceOf(value) : ValueID();
        TypeID type = source ? passedTypes[source.index()] : TypeID();
        return type ? types.at(type) : Type();
    };
    auto isKnown = [&](ValueID value) {
        ValueID source = value ? sourceOf(value) : ValueID();
        return source && passedTypes[source.index()];
    };
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = 0; position < std::min<size_t>(entry.numberOfParameters(), passedOperands.size()); ++position) {
        TypeID type = passedOperands[position] ? walker.typeOf(passedOperands[position]) : TypeID();
        if (type && !types.isUninformative(type))
            passedTypes[entry.parameter(position).index()] = type;
    }

    bool typesDecide = false;
    auto answerOf = [&](ValueID condition) -> std::optional<bool> {
        if (!condition || body->isParameter(condition))
            return std::nullopt;
        BodyValue question = body->value(condition);
        Opcode opcode = question.opcode();
        if (opcode == Opcode::TypeTest && isKnown(question.operand(0))) {
            Type passed = typeOf(question.operand(0));
            Type asked = types.at(question.testedType());
            if (holdsEveryPrimitiveAtInlining(asked, passed))
                return true;
            if (!(passed.primitives & asked.primitives) && !(passed.hasObjects() && asked.hasObjects()))
                return false;
            return std::nullopt;
        }
        if (opcode != Opcode::Eq && opcode != Opcode::StrictEq)
            return std::nullopt;
        ValueID left = question.operand(0);
        ValueID right = question.operand(1);
        if (isKnown(left) && sourceOf(left) == sourceOf(right) && !typeOf(left).mayBeNaN())
            return true;
        if (body->isParameter(left) || body->isParameter(right) || body->opcodeOf(left) != Opcode::TypeOf || body->opcodeOf(right) != Opcode::TypeOf)
            return std::nullopt;
        ValueID leftValue = body->value(left).operand(0);
        ValueID rightValue = body->value(right).operand(0);
        if (!isKnown(leftValue) || !isKnown(rightValue))
            return std::nullopt;
        std::optional<uint16_t> leftNamed = primitivesNamedByTypeOfAtInlining(typeOf(leftValue));
        std::optional<uint16_t> rightNamed = primitivesNamedByTypeOfAtInlining(typeOf(rightValue));
        if (!leftNamed || !rightNamed)
            return std::nullopt;
        return *leftNamed == *rightNamed;
    };

    PartOfCalleeLeftByTypes left;
    Vector<BlockID, 16> worklist { entryBlock };
    BitVector isVisited(body->numberOfBlocks());
    isVisited.set(entryBlock.index());
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (body->loopOf(block))
            return std::nullopt;
        for (BodyValue operation : body->block(block).values()) {
            Opcode opcode = operation.opcode();
            if (opcodesThatStateTypes.contains(opcode) || opcode == Opcode::NarrowByTest) {
                named[operation.id().index()] = operation.operand(0);
                continue;
            }
            bool isOfKnownType = (opcode == Opcode::TypeTest || opcode == Opcode::TypeOf) && isKnown(operation.operand(0));
            if (opcode == Opcode::BitOr && !body->isParameter(operation.operand(1)) && body->opcodeOf(operation.operand(1)) == Opcode::Integer && !body->value(operation.operand(1)).integer() && isKnown(operation.operand(0)) && typeOf(operation.operand(0)).isInt32())
                named[operation.id().index()] = operation.operand(0);
            else if (!constants.contains(opcode) && !isOfKnownType)
                ++left.numberOfOperations;
        }
        if (left.numberOfOperations > mostOperations)
            return std::nullopt;
        BodyTerminator end = body->terminator(block);
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        std::optional<bool> answer = answerOf(condition);
        typesDecide |= answer.has_value();
        uint32_t targetIndex = 0;
        for (BodyTarget target : end.targets()) {
            if (answer && targetIndex++ != !*answer)
                continue;
            BodyBlock next = body->block(target.block());
            if (body->numberOfIncomingEdges(target.block()) == 1) {
                uint32_t position = 0;
                for (ValueID argument : target.arguments())
                    named[next.parameter(position++).index()] = argument;
                for (uint32_t operandIndex : target.proofs()) {
                    ValueID proven = condition && !body->isParameter(condition) ? body->value(condition).operand(operandIndex) : ValueID();
                    named[next.parameter(position++).index()] = proven;
                }
            }
            if (!isVisited.set(target.block().index()))
                worklist.append(target.block());
        }
    }
    return typesDecide ? std::optional(left) : std::nullopt;
}

static bool isForwarderOfArguments(const Walker& walker, FunctionID callee)
{
    static constexpr uint32_t mostOperationsOfForwarder = 16;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    return summary.has(SummaryTrait::OnlyPassesOnGatheredArguments) && !summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= mostOperationsOfForwarder && walker.program().at(callee).has(FunctionFlags::HasHiddenRest);
}

static constexpr uint32_t maxOperationsAroundCallback = 400;

static ALWAYS_INLINE bool mayBeCallback(const Walker& walker, FunctionID callee)
{
    return walker.facts().linked().function(callee).mayCallBack || walker.isInCallbackCopy();
}

static NEVER_INLINE bool callsBackAtInlining(const Walker& walker, const BodyValue& call, FunctionID callee)
{
    ValueID receiver = call.as<Opcode::Call>().thisValueOperand();
    return receiver && walker.callsEnclosingReceiver(callee, walker.referenceOf(receiver));
}

static NEVER_INLINE bool callbackCopyPays(const Walker& walker, FunctionID callee, const std::span<const Reference>* passedOperands)
{
    const LinkTables& link = walker.facts().linked();
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!passedOperands || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > LinkTables::maxOperationsOfCallbackCopies)
        return false;
    if ((walker.depthOfCopies() ? walker.operationsCopiedAtWrittenCall() : 0) + summary.numberOfOperations > maxOperationsAroundCallback)
        return false;
    std::optional<unsigned> thisParameter = walker.program().at(callee).thisParameterIndex();
    Reference receiver = thisParameter && *thisParameter < passedOperands->size() ? (*passedOperands)[*thisParameter] : Reference();
    if (link.hasFewCallSites(callee, summary.numberOfOperations) && (walker.isInCallbackCopy() || walker.callsEnclosingReceiver(callee, receiver)))
        return true;
    return summary.numberOfOperations <= inlineLimitInLoops && walker.findEnclosingReceiver(receiver);
}

static bool copyPaysAtInlining(const Walker& walker, FunctionID callee, size_t numberOfOperands, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    if (copyIsDrawnAwayAtInlining(walker, callee, place))
        return false;
    if (place.isInLoop && mayBeCallback(walker, callee) && callbackCopyPays(walker, callee, passedOperands))
        return true;
    if (isForwarderOfArguments(walker, callee) && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + walker.facts().summaries().at(callee).numberOfOperations <= inlineWindowOfWrittenCallInLoops))
        return true;
    if (!place.isInLoop && onlyHandsFunctionToHelperThatCallsItAtInlining(walker, callee))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (walker.isInCopyOfKey() && !summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= inlineLimitInLoops && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= inlineWindowOfWrittenCallInLoops)
        return true;
    uint32_t window = place.isInLoop ? inlineWindowOfWrittenCallInLoops : inlineWindowOfWrittenCall;
    bool isLeaf = walker.facts().linked().function(callee).weightAsLeaf != LinkedFunction::notLeaf;
    size_t operations = summary.numberOfOperations - (isLeaf ? summary.numberOfOperationsThatOnlyTurnTruthRound : 0);
    bool isSmallCopy = !place.isInLoop && walker.isOutsideLoopsOfAsyncBody() && !(passedOperands && passesFreshObject(walker, *passedOperands));
    bool isInComparator = !place.isInLoop && !isSmallCopy && place.caller == walker.unit().function && walker.facts().linked().function(place.caller).isRepeatedCallback;
    size_t mostOperations = (isSmallCopy ? inlineLimitInAsyncBody : isInComparator ? inlineLimitInComparators : inlineLimitOutsideLoops) + numberOfOperands;
    if (operations > mostOperations && operations <= mostOperations + maxThrowingOperations && (place.isInLoop || (walker.isUnderCatch()))) {
        if (const BodyIndex* body = walker.facts().bodies().find(callee))
            operations -= std::min<size_t>(operations, throwingOperations(*body));
    }
    if (operations <= mostOperations
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window || isNoLargerThanCallWithItsOwnCallsAtInlining(summary, numberOfOperands)))
        return true;
    uint32_t weight = walker.facts().linked().function(callee).weightAsLeaf;
    if (place.isInLoop && weight != LinkedFunction::notLeaf)
        weight -= std::min(weight, summary.weightThatSwitchesSave);
    if (weight <= (place.isInLoop ? inlineLimitOfLeaves : inlineLimitOfLeavesOutsideLoops) + numberOfOperands
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + weight <= window))
        return true;
    if (place.isInLoop && place.caller == walker.unit().function && walker.facts().linked().function(callee).weightAsLeaf <= window && (passesTypedArrayElement(walker, passedOperands) || calculatesWithPassedNumbers(walker, callee, passedOperands) || leafReadsElementsOfHolderFromInFrontOfLoopAtInlining(walker, callee, passedOperands)))
        return true;
    if (place.isInLoop && place.caller == walker.unit().function && fitsLoopWithoutRestArray(walker, callee, summary.numberOfOperations) && passesStringLiteralKey(walker, callee, passedOperands))
        return true;
    if ((place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= inlineWindowOfWrittenCallInLoops) && summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= inlineLimitInLoops
        && passesStringLiteralKey(walker, callee, passedOperands, true))
        return true;
    if (canInlineSingleCallLoop(walker, callee, place, passedOperands))
        return true;
    if (place.isInLoop && place.caller == walker.unit().function && !summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations > inlineLimitInLoopsWithoutObjects && summary.numberOfOperations <= inlineWindowOfWrittenCallInLoops) {
        const BodyIndex* body = walker.facts().bodies().find(callee);
        uint32_t cheap = body ? 2 * comparisonsOfOneParameterWithConstants(*body) : 0;
        if (cheap >= inlineLimitOfLeaves && summary.numberOfOperations - std::min(cheap, summary.numberOfOperations) <= inlineLimitInLoopsWithoutObjects)
            return true;
    }
    if (place.isInLoop && place.caller == walker.unit().function && isLeaf && weight <= inlineWindowOfWrittenCallInLoops && walker.facts().linked().isWrittenOverBuiltin(callee))
        return true;
    if (walker.program().at(callee).kind == FunctionKind::Initializer)
        return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
    if (place.isInLoop && !summary.has(SummaryTrait::HasLoops) && walker.program().at(callee).has(FunctionFlags::IsAlwaysInlined)
        && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window)) {
        std::optional<uint32_t> operationsLeft = passedOperands ? operationsForPassedBuiltin(walker, callee, *passedOperands) : 0u;
        if (operationsLeft && *operationsLeft <= inlineLimitInLoopsWithoutObjects)
            return true;
    }
    if (place.isInLoop && (place.caller == walker.unit().function || place.caller == walker.currentFunction()) && summary.numberOfOperations <= inlineWindowOfWrittenCallInLoops
        && (summary.has(SummaryTrait::HasLoops) || place.caller == walker.unit().function || !walker.canBuildIterationGuard())) {
        if (std::optional<PartOfCalleeLeftByTypes> left = passedOperands ? findPartLeftByPassedTypes(walker, callee, *passedOperands, inlineLimitOutsideLoops + numberOfOperands) : std::nullopt; left
            && (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + left->numberOfOperations <= window))
            return true;
    }
    bool isSmallLoop = summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= inlineLimitOfLeaves + numberOfOperands;
    if (place.isInLoop && walker.canBuildIterationGuard() && (place.caller != walker.unit().function || isSmallLoop))
        return (place.caller == walker.unit().function || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window) && (isSmallLoop || !summary.has(SummaryTrait::HasLoops));
    if (passedOperands && place.isInLoop && place.caller == walker.unit().function && summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= inlineLimitInLoops && loopOfLeafStoresParameterFields(walker, callee))
        return true;
    if (place.caller == walker.unit().function && place.isInLoop && summary.has(SummaryTrait::HasLoops) && summary.numberOfOperations <= inlineLimitInLoops) {
        uint32_t size = sizeOfCopyThatWritesOutLoopsOverListsHandedIn(walker, callee);
        return size && size <= inlineWindowOfWrittenCallInLoops;
    }
    if (place.caller != walker.unit().function && place.caller == walker.currentFunction() && summary.has(SummaryTrait::HasLoops) && walker.program().at(callee).has(FunctionFlags::IsAlwaysInlined))
        return walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations <= window;
    if (!place.isInLoop && weight <= inlineLimitOfLeaves + numberOfOperands && place.caller == walker.unit().function && place.caller == walker.currentFunction() && !summary.has(SummaryTrait::HasLoops) && returnsNumberAtInlining(walker, callee)
        && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations) && isInRecursionWithoutLoops(walker))
        return true;
    if (!place.isInLoop && copyPaysInIterations(walker, callee, place, passedOperands))
        return true;
    if (!place.isInLoop || summary.has(SummaryTrait::HasLoops))
        return false;
    if (place.caller != walker.unit().function && passedOperands && summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations) && passedTypesDecideOperations(walker, callee, *passedOperands))
        return true;
    if (place.caller != walker.unit().function && (place.caller != walker.currentFunction() || (!walker.isInLoopOfBodyAtWork() && !walker.program().at(callee).has(FunctionFlags::HasRestParameter)) || walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > window))
        return false;
    if (summary.numberOfOperations > inlineLimitInLoopsWithoutObjects && summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects + maxThrowingOperations) {
        if (const BodyIndex* body = walker.facts().bodies().find(callee); body && summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects + operationsOfFailingChecksOfPrivateNames(*body))
            return true;
    }
    return summary.numberOfOperations <= inlineLimitInLoopsWithoutObjects || (fitsLoopWithoutRestArray(walker, callee, summary.numberOfOperations) && (copyMayLetObjectFallAwayAtInlining(walker, callee, passedOperands) || calleeAsksForClassOfWhatItIsHandedAtInlining(walker, callee)));
}

static bool copyFillsObjectMadeForItAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::CallDirect || !site.isConstruction() || site.isNeverInlined())
        return false;
    const Function& function = walker.program().at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCall)
        return false;
    return function.takesThisFromConstruction() && function.constructedClass && summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

static uint64_t entryParametersCalledInLoopsAtInlining(const ThinSummary& summary)
{
    return summary.entryParametersCalledInLoops;
}

static bool isClosureOfOneFunctionAtInlining(const Walker& walker, Reference value)
{
    TypeID proven = value ? walker.typeOf(value) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    return type.isObject() && type.isJavaScriptValue() && singleFunctionOf(walker.types(), type);
}

static bool calleeKnowsPassedFunction(const Walker& walker, FunctionID callee, size_t parameter, Reference passed)
{
    if (!passed || !passed.isValue() || walker.code().isConstant(passed.id()) || walker.code().at(passed.id()).opcode == Opcode::NewClosure || !walker.facts().linked().function(callee).everyCallerIsKnown)
        return false;
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(callee);
    if (parameter >= parameters.size() || !parameters[parameter].proven)
        return false;
    Type proven = walker.types().at(parameters[parameter].proven);
    return proven.isObject() && proven.isJavaScriptValue() && singleFunctionOf(walker.types(), proven);
}

static bool isSecondLoopOverWhatCopyCallsAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, std::span<const Reference> passedOperands)
{
    if (walker.walksOwnBodyOfUnit() || place.caller != walker.currentFunction())
        return false;
    uint64_t calledByCallee = entryParametersCalledInLoopsAtInlining(walker.facts().summaries().at(callee));
    uint64_t calledByCopy = entryParametersCalledInLoopsAtInlining(walker.facts().summaries().at(place.caller));
    if (!calledByCallee || !calledByCopy)
        return false;
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    for (size_t index = 0; index < std::min<size_t>(passedOperands.size(), 64); ++index) {
        if (!(calledByCallee >> index & 1) || !passedOperands[index])
            continue;
        for (uint32_t parameter = 0; parameter < std::min<uint32_t>(entry.numberOfParameters(), 64); ++parameter) {
            if ((calledByCopy >> parameter & 1) && walker.referenceOf(entry.parameter(parameter)) == passedOperands[index])
                return true;
        }
    }
    return false;
}

static bool isProfitableForPassedClosure(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitOfHelpers)
        return false;
    if (!place.isInLoop && walker.program().at(walker.unit().function).isModuleBody())
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    uint64_t called = entryParametersCalledInLoopsAtInlining(summary);
    if (!called || !passedOperands)
        return called;
    for (size_t index = 0; index < std::min<size_t>(passedOperands->size(), 64); ++index) {
        if (!(called >> index & 1))
            continue;
        Reference function = (*passedOperands)[index];
        if (!place.isInLoop && calleeKnowsPassedFunction(walker, callee, index, function))
            continue;
        if (isClosureOfOneFunctionAtInlining(walker, function) || (place.isInLoop && function && isBuiltinCalledWithoutReceiver(walker, walker.typeOf(function))))
            return true;
    }
    return false;
}

static bool copyOfNakedConstructorPaysAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    if (!passedOperands || !walker.program().at(callee).has(FunctionFlags::IsNakedConstructor))
        return false;
    if (!place.isInLoop && walker.program().at(walker.unit().function).isModuleBody())
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoops)
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    return std::ranges::any_of(*passedOperands, [&](Reference operand) {
        return isClosureOfOneFunctionAtInlining(walker, operand);
    });
}

static bool copyPaysForClosureThatCalleeOnlyCallsAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    if (!passedOperands)
        return false;
    if (!place.isInLoop && (!walker.walksOwnBodyOfUnit() || place.caller != walker.unit().function || !onlyHandsFunctionToHelperThatCallsItAtInlining(walker, place.caller)))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoops)
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body)
        return false;
    BodyBlock entry = body->block(entryBlock);
    for (uint32_t position = 0; position < std::min<uint32_t>(static_cast<uint32_t>(passedOperands->size()), entry.numberOfParameters()); ++position) {
        if (!isClosureOfOneFunctionAtInlining(walker, (*passedOperands)[position]))
            continue;
        if (isOnlyCalledAtInlining(*body, entry.parameter(position)))
            return true;
    }
    return false;
}

static bool isProfitableAsPassedClosure(const Walker& walker, FunctionID callee, const PlaceOfCall& place)
{
    if (!place.isInLoop || walker.walksOwnBodyOfUnit() || place.caller != walker.currentFunction())
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!walker.currentBodyIndex().hasLoops() && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops);
}

static bool entersBodyOfCoroutineAtInlining(const Draft& site)
{
    return site.opcode == Opcode::CallDirect && site.role() == CodeRole::CoroutineBody;
}

static bool mayBeWalkedAtInlining(const Walker& walker, FunctionID callee, bool entersBodyOfCoroutine = false)
{
    const Program& program = walker.program();
    const Function& function = program.at(callee);
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (walker.plan().promotedEnvironments().isMaker(callee))
        return false;
    bool isPlainBodyOfCoroutine = function.isCoroutine() && entersBodyOfCoroutine && walker.plan().conventions().of(callee).coroutineNeverSuspends;
    if ((function.isCoroutine() && !isPlainBodyOfCoroutine) || function.results.size() != 1 || walker.facts().isTakenToRunOnce(callee))
        return false;
    FlagsOfSummaryForInlining flags {
        .suspends = summary.has(SummaryTrait::Suspends),
        .hasDirectEval = summary.has(SummaryTrait::HasDirectEval),
        .holdsOperationsBoundToFrame = body && body->holdsAnyOf(opcodesThatBindBodyToFrame),
    };
    if (whyNoCallInlines(program, callee, PartOfCallee::WholeBody, &flags, body) != NotInlinedCause::None)
        return false;

    if (!walker.plan().conventions().exists({ callee, isPlainBodyOfCoroutine ? CodeRole::CoroutineBody : CodeRole::Body }))
        return false;
    if (walker.code().inlineFrames().size() >= mostInlineFramesAtInlining)
        return false;

    BodyBlock entry = body->block(entryBlock);
    if (body->numberOfIncomingEdges(entryBlock) || entry.hasHandler() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return false;

    bool returns = false;
    for (uint32_t block = body->numberOfBlocks(); block && !returns; --block)
        returns = body->isReachable(BlockID(block - 1)) && endsInReturnAtInlining(body->terminator(BlockID(block - 1)));
    const LinkTables& link = walker.facts().linked();
    return returns && (link.function(callee).namesOnlyWhatEveryFunctionLeadsTo || link.leadsTo(walker.unit(), callee));
}

static bool isEnteredAtInlining(const Function& function, bool isConstruction)
{
    if (!canBeCalledByValueOf(function.role) || function.has(FunctionCapabilities::RecordsCallee))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return !isConstruction;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return isConstruction && !function.has(FunctionFlags::HasThis) && (function.constructedClass || function.has(FunctionFlags::IsNakedConstructor));
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool takesListAtInlining(const Function& callee, unsigned entryParameter)
{
    EntryParameter kind = callee.entryParameterKind(entryParameter);
    return kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && entryParameter - callee.firstParameterIndex() >= callee.numberOfPositionalParameters());
}

static bool tellsAbsentFromUndefinedAtInlining(Walker& walker, const Function& callee, unsigned entryParameter)
{
    if (callee.entryParameterKind(entryParameter) != EntryParameter::Parameter)
        return false;
    TypeID written = callee.parameters[entryParameter - callee.firstParameterIndex()].written();
    return written && (walker.types().at(written).primitives & Primitive::Absent);
}

static Reference passedOperandAt(const CallOfInlining& call, const Function& callee, unsigned entryParameter)
{
    switch (callee.entryParameterKind(entryParameter)) {
    case EntryParameter::This:
        return call.thisValue;
    case EntryParameter::Callee:
        return call.closure;
    case EntryParameter::NewTarget:
        return call.newTarget;
    case EntryParameter::Parameter: {
        size_t parameter = entryParameter - callee.firstParameterIndex();
        return parameter < callee.numberOfPositionalParameters() && parameter < call.arguments.size() ? call.arguments[parameter] : Reference();
    }
    case EntryParameter::Capture:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE bool fitsParameterAtInlining(Walker& walker, const Function& callee, unsigned entryParameter, TypeID arriving)
{
    const DeclaredType* declared = callee.declaredEntryParameter(entryParameter);
    if (!declared || !declared->isEstablished())
        return true;
    TypeID established = walker.program().provenTypeOf(*declared);
    return walker.types().isSubset(arriving, established);
}

static bool operandsFitAtInlining(Walker& walker, const CallOfInlining& call, const Function& callee, std::span<const Reference> passedOperands)
{
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    if (passedOperands.size() != numberOfEntryParameters)
        return false;
    bool takesList = callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (takesList && call.arguments.size() > callee.numberOfPositionalParameters() + maxComputedElementsOfArrayLiteral)
        return false;

    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        if (walker.positions().entryParameterIsLeftOutOfCalls(call.callee, index))
            continue;
        if (passedOperands[index]) {
            TypeID arriving = walker.typeOf(passedOperands[index]);
            if (!arriving || !fitsParameterAtInlining(walker, callee, index, arriving))
                return false;
            continue;
        }

        EntryParameter kind = callee.entryParameterKind(index);
        if (!call.closure || kind == EntryParameter::Callee)
            return false;
        if (kind == EntryParameter::Capture || kind == EntryParameter::ArgumentCount)
            continue;
        if (takesListAtInlining(callee, index)) {
            if (!fitsParameterAtInlining(walker, callee, index, walker.types().builtinClass(BuiltinClasses::Array)))
                return false;
            continue;
        }
        if (!fitsParameterAtInlining(walker, callee, index, tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? TypeTable::absent : TypeTable::undefined))
            return false;
    }
    return true;
}

static Reference buildCaptureAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, size_t numberOfCaptures, uint32_t index)
{
    const Code& code = walker.code();
    if (call.closure.isValue()) {
        const Operation& creation = code.at(call.closure.id());
        if (creation.opcode == Opcode::NewClosure && creation.function() == call.callee && creation.numberOfVariadicOperands() == numberOfCaptures) {
            ValueID captured = creation.variadicOperands()[index];
            return code.isConstant(captured) ? Reference::constant(captured) : Reference::value(captured);
        }
    }
    if (Reference scope = findScopeOfClosureReadOffObjectInSight(walker, call.callee, call.closure))
        return scope;
    if (Reference atHand = findCaptureAtHand(walker, call.callee, index, call.closure))
        return atHand;
    Draft capture(Opcode::GetCapture, origin);
    walker.chooseConservativeFields(capture);
    capture.setFunction(call.callee);
    capture.setIndex(index);
    capture.setClosureOperand(call.closure);
    return walker.build(WTF::move(capture));
}

static Reference buildListOfOtherArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee)
{
    Draft list(Opcode::NewArray, origin);
    walker.chooseConservativeFields(list);
    list.standsForNoValueOfInput = true;
    list.setContents(TypeTable::anything);
    std::span<const Reference> listed = call.arguments.subspan(std::min<size_t>(callee.numberOfPositionalParameters(), call.arguments.size()));
    list.setVariadicOperands(listed);
    Reference built = walker.build(WTF::move(list));
    noteFreshList(walker, built);
    return built;
}

static Vector<Reference, 8> buildEntryArgumentsAtInlining(Walker& walker, Origin origin, const CallOfInlining& call, const Function& callee, std::span<const Reference> passedOperands)
{
    Vector<Reference, 8> operands;
    uint32_t numberOfCapturesPassed = 0;
    for (unsigned index = 0; index < passedOperands.size(); ++index) {
        EntryParameter kind = callee.entryParameterKind(index);
        uint32_t capture = kind == EntryParameter::Capture ? numberOfCapturesPassed++ : 0;
        if (kind == EntryParameter::Capture && walker.plan().promotedEnvironments().isRemoved(call.callee, capture)) {
            bool isInSight = passedOperands[index] && !walker.types().at(walker.typeOf(passedOperands[index])).mayBeUnspecified();
            operands.append(isInSight ? passedOperands[index] : valueNoLongerCaptured(walker, call.callee, capture, origin, call.closure));
            continue;
        }
        if (passedOperands[index]) {
            operands.append(passedOperands[index]);
            continue;
        }
        switch (kind) {
        case EntryParameter::Capture:
            operands.append(buildCaptureAtInlining(walker, origin, call, callee.captures.size(), capture));
            break;
        case EntryParameter::This:
        case EntryParameter::NewTarget:
            operands.append(walker.constant(Draft(Opcode::Undefined, origin)));
            break;
        case EntryParameter::ArgumentCount: {
            Draft count(Opcode::Integer, origin);
            count.setInteger(static_cast<int64_t>(call.arguments.size()));
            operands.append(walker.constant(WTF::move(count)));
            break;
        }
        case EntryParameter::Parameter:
            if (takesListAtInlining(callee, index))
                operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            else
                operands.append(walker.constant(Draft(tellsAbsentFromUndefinedAtInlining(walker, callee, index) ? Opcode::Absent : Opcode::Undefined, origin)));
            break;
        case EntryParameter::HiddenRest:
            operands.append(buildListOfOtherArgumentsAtInlining(walker, origin, call, callee));
            break;
        case EntryParameter::Callee:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return operands;
}

namespace {

struct OperandsInSightAtInlining {
    std::span<const Reference> passedOperands;
    bool callsParameterOfCopy { false };
    bool calleeStandsBehindBuiltin { false };
};

} // anonymous namespace

std::optional<TurnsOfCopyWrittenOut> turnsOfCopyWrittenOutAt(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::Call || !walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::Folding) || !walker.has(Mechanism::LoopInvariants))
        return std::nullopt;
    if (!walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops))
        return std::nullopt;
    const Function& function = walker.program().at(callee);
    Vector<std::optional<int64_t>, 8> integers;
    Vector<std::optional<uint32_t>, 8> lengths;
    integers.fill(std::nullopt, function.numberOfEntryParameters());
    lengths.fill(std::nullopt, function.numberOfEntryParameters());
    size_t numberOfArguments = site.argumentsOperands().size();
    std::span<const ValueID> written;
    if (place.body && place.call && place.body->opcodeOf(place.call) == Opcode::Call)
        written = place.body->value(place.call).as<Opcode::Call>().argumentsOperands();
    if (written.size() != numberOfArguments)
        written = { };
    TurnsOfCopyWrittenOut answer;
    answer.firstArgumentListed = static_cast<uint32_t>(std::min<size_t>(function.numberOfPositionalParameters(), numberOfArguments));
    unsigned numberOfLists = 0;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::ArgumentCount)
            integers[index] = static_cast<int64_t>(numberOfArguments);
        if (takesListAtInlining(function, index)) {
            ++numberOfLists;
            answer.entryParameterOfList = index;
            lengths[index] = static_cast<uint32_t>(numberOfArguments) - answer.firstArgumentListed;
            continue;
        }
        if (function.entryParameterKind(index) != EntryParameter::Parameter || index - function.firstParameterIndex() >= written.size())
            continue;
        ValueID argument = place.body->stripCasts(written[index - function.firstParameterIndex()]);
        if (argument && !place.body->isParameter(argument) && place.body->opcodeOf(argument) == Opcode::Integer)
            integers[index] = place.body->value(argument).integer();
    }
    bool saysSomething = numberOfLists || std::ranges::any_of(integers, [](const std::optional<int64_t>& integer) { return integer.has_value(); });
    if (numberOfLists > 1 || !saysSomething)
        return std::nullopt;
    Vector<std::optional<int64_t>, 8> noIntegers;
    Vector<std::optional<uint32_t>, 8> noLengths;
    noIntegers.fill(std::nullopt, function.numberOfEntryParameters());
    noLengths.fill(std::nullopt, function.numberOfEntryParameters());
    if (turnsOfLoopsOfCopyWrittenOut(walker, callee, noIntegers.span(), noLengths.span()))
        return std::nullopt;
    std::optional<TextOfCopyRead> text = turnsOfLoopsOfCopyWrittenOut(walker, callee, integers.span(), lengths.span());
    if (!text)
        return std::nullopt;
    answer.text = WTF::move(*text);
    return answer;
}

static bool stepsForOfAtInlining(const Walker& walker, FunctionID callee)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    for (uint32_t loop = 0; body && loop < body->numberOfLoops(); ++loop) {
        if (findForOfSteppedAtHead(*body, LoopID(loop)))
            return true;
    }
    return false;
}

static constexpr OpcodeSet operationsOfTextThatOnlyFillsObjectAtInlining = primitiveConstants | storesByName | inputPassThroughOpcodes | allNumericOperators | allRelations | allEqualities
    | OpcodeSet { Opcode::GetConstructPrototype, Opcode::NewObject, Opcode::TypeTest, Opcode::CallDirect, Opcode::PropGet };
static constexpr OpcodeSet operationsThatDoMoreThanFillObjectAtInlining = opcodesWhere([](Opcode opcode) { return !operationsOfTextThatOnlyFillsObjectAtInlining.contains(opcode); });

static bool branchAsksConstantForItsTypeAtInlining(const BodyIndex& body, BodyTerminator end)
{
    if (end.kind() != TerminatorKind::Branch)
        return false;
    ValueID test = end.operand(0);
    if (body.isParameter(test) || body.opcodeOf(test) != Opcode::TypeTest)
        return false;
    ValueID asked = body.stripCasts(body.value(test).operand(0));
    return asked && !body.isParameter(asked) && primitiveConstants.contains(body.opcodeOf(asked));
}

static bool textOnlyFillsObjectAtInlining(const Walker& walker, FunctionID function, bool mayCallInitializer)
{
    const BodyIndex* body = walker.facts().bodies().find(function);
    if (!body || body->holdsAnyOf(operationsThatDoMoreThanFillObjectAtInlining) || (!mayCallInitializer && body->holdsAnyOf(OpcodeSet { Opcode::CallDirect })))
        return false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyBlock block = body->block(BlockID(number));
        BodyTerminator end = block.terminator();
        if (block.hasHandler() || (end.kind() != TerminatorKind::Jump && end.kind() != TerminatorKind::Return && !branchAsksConstantForItsTypeAtInlining(*body, end)))
            return false;
        for (BodyValue operation : block.values()) {
            if (operation.opcode() != Opcode::CallDirect)
                continue;
            FunctionID called = operation.as<Opcode::CallDirect>().function();
            if (walker.program().at(called).kind != FunctionKind::Initializer || !textOnlyFillsObjectAtInlining(walker, called, false))
                return false;
        }
    }
    return true;
}

static constexpr OpcodeSet operationsOfTextThatFillsObjectBehindSuperCallAtInlining = operationsOfTextThatOnlyFillsObjectAtInlining | OpcodeSet { Opcode::Absent, Opcode::GetSuperBase, Opcode::SuperConstruct, Opcode::NewRuntimeError, Opcode::IsUnlockedInstance };
static constexpr OpcodeSet operationsThatDoMoreThanFillObjectBehindSuperCallAtInlining = opcodesWhere([](Opcode opcode) { return !operationsOfTextThatFillsObjectBehindSuperCallAtInlining.contains(opcode); });

static bool textFillsObjectBehindSuperCallAtInlining(const Walker& walker, FunctionID function)
{
    const BodyIndex* body = walker.facts().bodies().find(function);
    if (!body || walker.program().at(function).kind != FunctionKind::DerivedConstructor || body->holdsAnyOf(operationsThatDoMoreThanFillObjectBehindSuperCallAtInlining))
        return false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyBlock block = body->block(BlockID(number));
        BodyTerminator end = block.terminator();
        bool asksForType = end.kind() == TerminatorKind::Branch && !body->isParameter(end.operand(0)) && OpcodeSet { Opcode::TypeTest, Opcode::IsUnlockedInstance }.contains(body->opcodeOf(end.operand(0)));
        if (block.hasHandler() || (end.kind() != TerminatorKind::Jump && end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::Throw && !asksForType))
            return false;
        for (BodyValue operation : block.values()) {
            if (operation.opcode() != Opcode::CallDirect)
                continue;
            FunctionID called = operation.as<Opcode::CallDirect>().function();
            if (walker.program().at(called).kind != FunctionKind::Initializer || !textOnlyFillsObjectAtInlining(walker, called, false))
                return false;
        }
    }
    return true;
}

static bool copyMakesObjectOfClassBelowAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    if (site.opcode != Opcode::SuperConstruct || copyIsDrawnAwayAtInlining(walker, callee, place))
        return false;
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (place.isInLoop && summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops) && operationsFitWindowOfCallWrittenInLoop(walker, summary.numberOfOperations) && textFillsObjectBehindSuperCallAtInlining(walker, callee))
        return true;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCall)
        return false;
    return summary.numberOfOperations <= inlineLimitInLoops && !summary.has(SummaryTrait::HasLoops) && textOnlyFillsObjectAtInlining(walker, callee, true);
}

static bool copyOpensWhatWasJustMadeAtInlining(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const std::span<const Reference>* passedOperands)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!passedOperands || !body || summary.numberOfOperations > inlineLimitOfHelpers || !(body->opcodes().contains(Opcode::IteratorOpen) || body->numberOfLoops()))
        return false;
    if (!place.isInLoop && walker.program().at(walker.unit().function).isModuleBody())
        return false;
    if (place.caller != walker.unit().function && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfHelpers)
        return false;
    for (size_t entry = 0; entry < passedOperands->size() && entry < body->block(entryBlock).numberOfParameters(); ++entry) {
        Reference handed = (*passedOperands)[entry];
        if (!handed || !handed.isValue())
            continue;
        ValueID parameter = body->block(entryBlock).parameter(static_cast<uint32_t>(entry));
        if (isIteratorOfTableJustMade(walker, handed) && findOpeningThatAloneTakesParameter(*body, parameter))
            return true;
        const Operation& made = walker.code().at(handed.id());
        if (made.opcode != Opcode::CallDirect || made.role() == CodeRole::CoroutineBody || !walker.program().at(made.function()).has(FunctionFlags::IsGenerator) || walker.program().at(made.function()).has(FunctionFlags::IsAsync))
            continue;
        if (place.isInLoop && (findOpeningThatAloneTakesParameter(*body, parameter) || onlyStepsParameter(walker.program(), *body, parameter)))
            return true;
        if (body->numberOfLoops() && walker.openOfPassedGenerator(callee, static_cast<uint32_t>(entry)))
            return true;
        if (body->numberOfLoops() || summary.numberOfOperations > inlineLimitInLoopsWithoutObjects)
            continue;
        for (uint32_t block = 0; block < body->numberOfBlocks(); ++block) {
            for (BodyValue operation : body->block(BlockID(block)).values()) {
                if (operation.opcode() != Opcode::IteratorOpen)
                    continue;
                auto open = operation.as<Opcode::IteratorOpen>();
                if (open.iteratorKind() == IteratorKind::Sync && !open.isDelegating() && !open.isDestructuring() && body->stripCasts(open.iterableOperand()) == parameter)
                    return true;
            }
        }
    }
    return false;
}

static bool copyKeepsReceiverLocal(const Walker& walker, FunctionID callee, const PlaceOfCall& place, const OperandsInSightAtInlining* inSight)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    if (!inSight || !place.isInLoop || place.caller == walker.unit().function || place.caller != walker.currentFunction() || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > inlineLimitInLoopsWithoutObjects)
        return false;
    if (walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfWrittenCallInLoops)
        return false;
    std::optional<unsigned> thisParameter = walker.program().at(callee).thisParameterIndex();
    Reference receiver = thisParameter && *thisParameter < inSight->passedOperands.size() ? inSight->passedOperands[*thisParameter] : Reference();
    if (!receiver || !receiver.isValue())
        return false;
    ValueID cell = underlyingCellOf(walker.code(), receiver.id());
    return !walker.code().isConstant(cell) && walker.code().at(cell).opcode == Opcode::NewObject && !walker.program().at(walker.code().at(cell).layout()).isEnvironment() && walker.state().isFresh(Reference::value(cell));
}

static bool unitIsCalledInLoops(const Walker& walker)
{
    const LinkTables& link = walker.facts().linked();
    FunctionID unit = walker.unit().function;
    return link.functionsCalledInLoops.get(unit.index()) || link.function(unit).isIterationCallback || walker.isCurrentFrameRecursive();
}

static bool loopRemovesResult(const Walker& walker, ValueID call, FunctionID callee)
{
    if (!call)
        return false;
    bool isInArm = !walker.facts().linked().calleeAt(walker.currentFunction(), call);
    TypeID result = walker.facts().linked().function(callee).result;
    if (!isInArm && (!result || !holdsNumbersAlone(walker, singleLayoutOf(walker.types(), walker.types().at(result)))))
        return false;
    return isRemovableInLoops(walker, callee) && fateOfCallResult(walker, walker.currentFunction(), walker.currentBodyIndex(), call) != FateOfValue::Escapes;
}

static bool copyRemovesObjects(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place, const OperandsInSightAtInlining* inSight)
{
    const ThinSummary& summary = walker.facts().summaries().at(callee);
    bool isInCopy = place.caller != walker.unit().function;
    if (!inSight || summary.has(SummaryTrait::HasLoops))
        return false;
    if (place.isInLoop && !isInCopy)
        return summary.numberOfOperations > inlineLimitInLoopsWithoutObjects && loopRemovesResult(walker, place.call ? place.call : site.inputValueGiven, callee);
    if (summary.numberOfOperations > inlineLimitOfRemovedObjects || (isInCopy && walker.operationsCopiedAtWrittenCall() + summary.numberOfOperations > inlineWindowOfRemovedObjects))
        return false;
    auto isCalledInLoops = [&] {
        return place.isInLoop || unitIsCalledInLoops(walker);
    };
    ValueID call = place.call ? place.call : site.inputValueGiven;
    auto resultIsOnlyTakenApart = [&] {
        FateOfValue fate = call ? fateOfCallResult(walker, walker.currentFunction(), walker.currentBodyIndex(), call) : FateOfValue::Escapes;
        if (fate == FateOfValue::Returned) {
            std::optional<Walker::CallOfCopy> copy = walker.callOfCopy();
            fate = copy ? fateOfCallResult(walker, copy->caller, *copy->index, copy->call) : FateOfValue::Escapes;
        }
        return fate == FateOfValue::OnlyTakenApart;
    };
    for (unsigned index = 0; walker.state().holdsFreshObjects() && index < inSight->passedOperands.size(); ++index) {
        Reference passed = inSight->passedOperands[index];
        if (!passed || !passed.isValue())
            continue;
        ValueID cell = underlyingCellOf(walker.code(), passed.id());
        if (walker.code().isConstant(cell) || walker.code().at(cell).opcode != Opcode::NewObject || walker.program().at(walker.code().at(cell).layout()).isEnvironment() || !walker.state().isFresh(Reference::value(cell)))
            continue;
        if (!holdsNumbersAlone(walker, walker.code().at(cell).layout()) || !isCalledInLoops())
            continue;
        FateOfValue fate = fateOfParameter(walker, callee, index);
        if (fate == FateOfValue::OnlyTakenApart || (fate == FateOfValue::Returned && resultIsOnlyTakenApart()))
            return true;
    }
    TypeID result = walker.facts().linked().function(callee).result;
    return result && holdsNumbersAlone(walker, singleLayoutOf(walker.types(), walker.types().at(result))) && isCalledInLoops() && returnsNewObject(walker, callee) && resultIsOnlyTakenApart();
}

static bool unrollsWhenInlined(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place)
{
    bool isInCopy = place.caller != walker.unit().function;
    if (!walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops) || (!place.isInLoop && (isInCopy || stepsForOfAtInlining(walker, callee))))
        return false;
    if (isInCopy && !walker.program().at(callee).hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return false;
    std::optional<TurnsOfCopyWrittenOut> turns = turnsOfCopyWrittenOutAt(walker, site, callee, place);
    return turns && (!isInCopy || operationsFitWindowOfCallWrittenInLoop(walker, turns->text.numberOfOperations));
}

static bool callAdmitsCopyAtInlining(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& place, const OperandsInSightAtInlining* inSight)
{
    auto copyPays = [&](size_t numberOfOperands) {
        if (copyIsDrawnAwayAtInlining(walker, callee, place))
            return false;
        if (inSight && isSecondLoopOverWhatCopyCallsAtInlining(walker, callee, place, inSight->passedOperands))
            return false;
        return ((!inSight || !inSight->calleeStandsBehindBuiltin) && copyPaysAtInlining(walker, callee, numberOfOperands, place, inSight ? &inSight->passedOperands : nullptr))
            || isProfitableForPassedClosure(walker, callee, place, inSight ? &inSight->passedOperands : nullptr)
            || copyPaysForClosureThatCalleeOnlyCallsAtInlining(walker, callee, place, inSight ? &inSight->passedOperands : nullptr)
            || copyOfNakedConstructorPaysAtInlining(walker, callee, place, inSight ? &inSight->passedOperands : nullptr)
            || copyOpensWhatWasJustMadeAtInlining(walker, callee, place, inSight ? &inSight->passedOperands : nullptr)
            || copyMatchesKnownPattern(walker, callee, place)
            || copyKeepsReceiverLocal(walker, callee, place, inSight)
            || copyRemovesObjects(walker, site, callee, place, inSight)
            || ((!inSight || inSight->callsParameterOfCopy) && isProfitableAsPassedClosure(walker, callee, place))
            || unrollsWhenInlined(walker, site, callee, place);
    };
    if (copyFillsObjectMadeForItAtInlining(walker, site, callee, place))
        return true;
    if (site.opcode == Opcode::CallDirect)
        return !site.isNeverInlined() && copyPays(site.numberOfVariadicOperands());

    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    if (site.opcode == Opcode::Construct && site.newTargetOperand() && site.newTargetOperand() != site.calleeOperand())
        return false;
    if (site.opcode == Opcode::Call && site.isNeverInlined())
        return false;
    return isEnteredAtInlining(walker.program().at(callee), isConstruction) && (copyPays(site.argumentsOperands().size()) || copyMakesObjectOfClassBelowAtInlining(walker, site, callee, place));
}

static CallOfInlining callAsWrittenAtInlining(const Draft& site, FunctionID callee)
{
    CallOfInlining written;
    written.callee = callee;
    written.closure = closureCalledAtInlining(site);
    written.arguments = site.argumentsOperands();
    return written;
}

bool callMayEnterCopyOfBody(Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    const Function& function = walker.program().at(callee);
    if (site.opcode != Opcode::Call || site.isNeverInlined() || !isEnteredAtInlining(function, false))
        return false;
    return operandsFitAtInlining(walker, callAsWrittenAtInlining(site, callee), function, passedOperands);
}

Vector<Reference, 8> buildEntryArgumentsOfCopy(Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    Origin origin = site.origin();
    origin.isTailCall = false;
    return buildEntryArgumentsAtInlining(walker, origin, callAsWrittenAtInlining(site, callee), walker.program().at(callee), passedOperands);
}

bool operationsFitWindowOfCallWrittenInLoop(const Walker& walker, uint64_t numberOfOperations)
{
    return (walker.depthOfCopies() ? walker.operationsCopiedAtWrittenCall() : 0) + numberOfOperations <= inlineWindowOfWrittenCallInLoops;
}

bool calleeIsCopiedInLoopsAlone(const Walker& walker, FunctionID callee, size_t numberOfOperands)
{
    if (walker.facts().summaries().at(callee).has(SummaryTrait::HasLoops) || walker.isBeingWalked(callee) || !mayBeWalkedAtInlining(walker, callee))
        return false;
    if (const BodyIndex* body = walker.facts().bodies().find(callee); !body || isDirectlyRecursive(walker.program(), *body, callee))
        return false;
    std::span<const Reference> nothingInSight;
    return copyPaysAtInlining(walker, callee, numberOfOperands, { walker.currentFunction(), true }, &nothingInSight) && !copyPaysAtInlining(walker, callee, numberOfOperands, { walker.currentFunction(), false }, &nothingInSight);
}

static bool bodyOfUnitCallsItselfAtInlining(const Walker& walker, const Draft& site, FunctionID callee)
{
    if ((!walker.walksOwnBodyOfUnit() && !walker.walksSpecializedBody()) || callee != walker.unit().function || walker.unit().role != CodeRole::Body || !site.inputValueGiven)
        return false;
    const BodyIndex& body = walker.currentBodyIndex();
    BodyTerminator end = body.terminator(body.blockOf(site.inputValueGiven));
    bool answerIsReturnedAtOnce = endsInReturnAtInlining(end) && end.operand(0) && body.stripCasts(end.operand(0)) == site.inputValueGiven;
    return !answerIsReturnedAtOnce;
}

static bool bodyOfUnitTakesCopyOfItselfAtInlining(const Walker& walker, const Draft& site, FunctionID callee)
{
    return bodyOfUnitCallsItselfAtInlining(walker, site, callee) && walker.facts().summaries().at(callee).numberOfOperations <= inlineLimitInLoopsWithoutObjects;
}

static bool constructsItselfOnce(const Walker& walker, const Draft& site, FunctionID callee)
{
    const Function& function = walker.program().at(callee);
    bool isConstruction = constructionsOfInlining.contains(site.opcode) || (site.opcode == Opcode::CallDirect && site.isConstruction());
    return isConstruction && walker.currentFunction() == callee && !isClassConstructorOf(function.kind) && !walker.walksOwnBodyOfUnit() && !walker.isBeingConstructed(callee);
}

static bool isLibraryFunctionInRecursion(const Walker& walker, FunctionID callee, std::span<const Reference> passedOperands)
{
    return isLibraryCode(walker.program(), walker.program().at(callee)) && std::ranges::any_of(passedOperands, [&](Reference operand) { return isClosureOfOneFunctionAtInlining(walker, operand); }) && isInRecursionWithoutLoops(walker);
}

bool mayCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, const PlaceOfCall& written)
{
    bool callsItself = bodyOfUnitTakesCopyOfItselfAtInlining(walker, site, callee);
    PlaceOfCall place = written;
    place.isInLoop |= callsItself;
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, nullptr)
        && (callsItself || !walker.isBeingWalked(callee) || constructsItselfOnce(walker, site, callee)) && mayBeWalkedAtInlining(walker, callee, entersBodyOfCoroutineAtInlining(site));
}

bool copyOfCalleeCalledInLoopPays(const Walker& walker, FunctionID callee, size_t numberOfArguments)
{
    return copyOfCalleePaysAt(walker, callee, numberOfArguments, { walker.currentFunction(), true });
}

bool copyOfCalleePaysAt(const Walker& walker, FunctionID callee, size_t numberOfArguments, const PlaceOfCall& place)
{
    return callee && walker.has(Mechanism::Inlining) && isEnteredAtInlining(walker.program().at(callee), false) && copyPaysAtInlining(walker, callee, numberOfArguments, place, nullptr)
        && !walker.isBeingWalked(callee) && mayBeWalkedAtInlining(walker, callee);
}

bool shouldCopyCallee(const Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands, bool callsParameterOfCopy)
{
    OperandsInSightAtInlining inSight { passedOperands, callsParameterOfCopy, false };
    bool callsItself = bodyOfUnitTakesCopyOfItselfAtInlining(walker, site, callee);
    PlaceOfCall place { walker.currentFunction(), walker.isInLoop() || callsItself || isLibraryFunctionInRecursion(walker, callee, passedOperands) };
    return walker.has(Mechanism::Inlining) && opcodesOfInlining.contains(site.opcode) && callAdmitsCopyAtInlining(walker, site, callee, place, &inSight)
        && (callsItself || !walker.isBeingWalked(callee) || constructsItselfOnce(walker, site, callee)) && mayBeWalkedAtInlining(walker, callee, entersBodyOfCoroutineAtInlining(site));
}

Vector<Reference, 8> collectPassedOperands(const Draft& site, const Function& callee)
{
    if (site.opcode == Opcode::CallDirect)
        return Vector<Reference, 8>(site.variadicOperands());

    CallOfInlining request;
    request.closure = closureCalledAtInlining(site);
    request.arguments = site.argumentsOperands();
    if (site.opcode == Opcode::Construct)
        request.newTarget = request.closure;
    else if (site.opcode == Opcode::SuperConstruct)
        request.newTarget = site.newTargetOperand();
    else
        request.thisValue = site.thisValueOperand();

    Vector<Reference, 8> passedOperands;
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    for (unsigned index = 0; index < numberOfEntryParameters; ++index)
        passedOperands.append(passedOperandAt(request, callee, index));
    return passedOperands;
}

static_assert(opcodesThatInlineCalleeTakes == (opcodesOfInlining | callsOfListsAtInlining));

static bool inlineCalleeOfListWrittenOut(Walker& walker, const BodyValue& call)
{
    Reference closure = walker.referenceOf(call.opcode() == Opcode::SuperConstructList ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = closure ? findOnlyCallee(walker, call, closure) : FunctionID();
    if (!callee)
        return false;
    std::optional<Draft> site = draftOfCallWithListWrittenOut(walker, call);
    if (!site)
        return false;
    Vector<Reference, 8> passedOperands = collectPassedOperands(*site, walker.program().at(callee));
    OperandsInSightAtInlining inSight { passedOperands.span(), false, false };
    if (!callAdmitsCopyAtInlining(walker, *site, callee, { walker.currentFunction(), walker.isInLoop() }, &inSight))
        return false;
    return walker.walkInline(call, callee, passedOperands.span(), &*site);
}

bool inlineConstructionHandedOn(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee)
{
    if (!walker.has(Mechanism::Inlining))
        return false;
    Vector<Reference, 8> passedOperands = collectPassedOperands(site, walker.program().at(callee));
    OperandsInSightAtInlining inSight { passedOperands.span(), false, false };
    return callAdmitsCopyAtInlining(walker, site, callee, { walker.currentFunction(), walker.isInLoop() }, &inSight) && walker.walkInline(call, callee, passedOperands.span(), &site);
}

namespace {

struct HeadThatReturnsAtOnce {
    uint32_t entryParameter { 0 };
    bool returnsWhereTrue { false };
    ValueID returned;
};

} // anonymous namespace

static std::optional<HeadThatReturnsAtOnce> findHeadThatReturnsAtOnceAtInlining(const BodyIndex& body)
{
    BodyBlock entry = body.block(entryBlock);
    BodyTerminator end = entry.terminator();
    if (!end.isConditionalBranch() || entry.hasHandler() || body.isLoopHead(entryBlock))
        return std::nullopt;
    ValueID asked = end.operand(0);
    bool isNegated = false;
    uint32_t numberOfTests = 0;
    while (!body.isParameter(asked)) {
        Opcode opcode = body.opcodeOf(asked);
        if (opcode != Opcode::ToBoolean && opcode != Opcode::LogicalNot)
            return std::nullopt;
        isNegated ^= opcode == Opcode::LogicalNot;
        ++numberOfTests;
        asked = body.value(asked).operand(0);
    }
    std::optional<uint32_t> entryParameter = entry.indexOfParameter(asked);
    uint32_t numberOfConstants = 0;
    for (uint32_t place = 0; place < entry.numberOfValues(); ++place)
        numberOfConstants += primitiveConstants.contains(body.opcodeOf(ValueID(entry.firstValue().index() + place)));
    if (!entryParameter || numberOfTests + numberOfConstants != entry.numberOfValues())
        return std::nullopt;
    bool isTaken = true;
    for (BodyTarget target : end.targets()) {
        bool holdsWhereTrue = std::exchange(isTaken, false) != isNegated;
        BodyBlock block = body.block(target.block());
        BodyTerminator endOfBlock = block.terminator();
        if (!target.arguments().empty() || block.hasHandler() || endOfBlock.kind() != TerminatorKind::Return)
            continue;
        bool holdsConstantsAlone = true;
        for (uint32_t position = 0; position < block.numberOfParameters(); ++position)
            holdsConstantsAlone &= body.isUnused(block.parameter(position));
        for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
            Opcode opcode = body.opcodeOf(ValueID(block.firstValue().index() + place));
            holdsConstantsAlone &= primitiveConstants.contains(opcode) || opcode == Opcode::Convert;
        }
        ValueID returned = endOfBlock.operand(0) ? body.stripCasts(endOfBlock.operand(0)) : ValueID();
        if (holdsConstantsAlone && returned && !body.isParameter(returned) && primitiveConstants.contains(body.opcodeOf(returned)))
            return HeadThatReturnsAtOnce { *entryParameter, holdsWhereTrue, returned };
    }
    return std::nullopt;
}

static bool copyHeadThatReturnsAtOnceAtInlining(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    if ((call.opcode() != Opcode::Call && call.opcode() != Opcode::CallDirect) || !walker.opensGuesses() || site.resultIsReturnedAtOnce || site.isTailCall)
        return false;
    const Function& function = walker.program().at(callee);
    if (function.isCoroutine() || (call.opcode() == Opcode::CallDirect && (site.isConstruction() || site.role() != CodeRole::Body)))
        return false;
    const BodyIndex& body = walker.facts().bodies().at(callee);
    std::optional<HeadThatReturnsAtOnce> head = findHeadThatReturnsAtOnceAtInlining(body);
    if (!head || head->entryParameter >= passedOperands.size() || function.entryParameterKind(head->entryParameter) != EntryParameter::Parameter
        || takesListAtInlining(function, head->entryParameter) || tellsAbsentFromUndefinedAtInlining(walker, function, head->entryParameter))
        return false;

    TypeArena& types = walker.types();
    Reference argument = passedOperands[head->entryParameter];
    TypeID type = argument ? walker.typeOf(argument) : TypeID(TypeTable::undefined);
    if (!type || isNeverType(type) || !types.at(type).isJavaScriptValue())
        return false;
    Origin origin = site.origin();
    origin.isTailCall = false;
    auto returned = [&] {
        Draft constant = walker.draftOf(body.value(head->returned));
        constant.setOrigin(origin);
        return walker.constant(WTF::move(constant));
    };
    Reference constant = returned();
    if (!constant)
        return false;
    TypeID shown = argument ? walker.typeShown(argument) : TypeID(TypeTable::undefined);
    bool isNeverTrue = isNeverType(truthyPartOf(types, shown));
    bool isAlwaysTrue = isNeverType(falsyPartOf(types, shown));
    if (std::optional<bool> decided = argument && !isNeverTrue && !isAlwaysTrue ? outcomeKnownOf(walker, argument) : std::nullopt) {
        isNeverTrue = !*decided;
        isAlwaysTrue = *decided;
    }
    if (isNeverTrue || isAlwaysTrue) {
        if (isAlwaysTrue != head->returnsWhereTrue)
            return false;
        walker.bind(call.id(), constant);
        return true;
    }

    if (!isBooleanType(type) || walker.representationOf(argument) != Representation::Boolean)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();
    BlockID whereHeadReturns = walker.addBlock();
    BlockID whereCalleeIsEntered = walker.addBlock();
    BlockID continuationBlock = walker.addBlock();
    walker.branch(argument, head->returnsWhereTrue ? whereHeadReturns : whereCalleeIsEntered, head->returnsWhereTrue ? whereCalleeIsEntered : whereHeadReturns);
    Reference answer;
    walker.continueIn(whereCalleeIsEntered);
    {
        Walker::ClosedArm calls(walker);
        Draft known(Opcode::Boolean, origin);
        known.setBoolean(!head->returnsWhereTrue);
        Reference handedOn = walker.constant(WTF::move(known));
        call.forEachOperand([&](ValueID operand, uint32_t) {
            if (handedOn && walker.referenceOf(operand) == argument)
                walker.bind(operand, handedOn);
        });
        walker.walkStretch(block.id(), place, place + 1);
        if (walker.hasFailed())
            return true;
        Reference called = walker.isUnreachable() ? Reference() : walker.referenceOf(call.id());
        bool keepsForm = called && types.isSubset(walker.typeOf(constant), walker.typeOf(called));
        answer = walker.addParameter(continuationBlock, keepsForm ? walker.typeOf(called) : TypeID(TypeTable::anything), keepsForm ? walker.representationOf(called) : Representation::JSValue);
        if (called)
            walker.jumpTo(continuationBlock, singleElementSpan(called));
    }
    walker.continueIn(whereHeadReturns);
    walker.jumpTo(continuationBlock, singleElementSpan(constant));
    walker.continueIn(continuationBlock);
    walker.bind(call.id(), answer);
    return true;
}

static constexpr OpcodeSet operationsOfHeadsAtInlining { Opcode::Convert, Opcode::PropGet, Opcode::GetModuleVariable, Opcode::ToBoolean, Opcode::Integer, Opcode::CheckInitialized, Opcode::Undefined, Opcode::String, Opcode::GetEnvironmentField,
    Opcode::TypeTest, Opcode::StrictEq, Opcode::Boolean, Opcode::NarrowByField, Opcode::ElemGet, Opcode::BitAnd, Opcode::NarrowByTest, Opcode::LogicalNot, Opcode::BitOr, Opcode::Null, Opcode::Less, Opcode::Greater, Opcode::GreaterEq, Opcode::LessEq,
    Opcode::RequireObjectCoercible, Opcode::Double, Opcode::Absent, Opcode::HasJSType, Opcode::PrivateGet };
static constexpr OpcodeSet operationsThatWeighNothingInHeadsAtInlining { Opcode::Convert, Opcode::Integer, Opcode::Undefined, Opcode::String, Opcode::Boolean, Opcode::NarrowByField, Opcode::NarrowByTest, Opcode::Null, Opcode::Double, Opcode::Absent, Opcode::CheckInitialized };
static constexpr OpcodeSet checksOfHeadsAtInlining { Opcode::CheckInitialized, Opcode::Convert, Opcode::NarrowByTest, Opcode::NarrowByField };

static ValueID valueBehindChecksAtInlining(const BodyIndex& body, ValueID value)
{
    while (!body.isParameter(value) && checksOfHeadsAtInlining.contains(body.opcodeOf(value)))
        value = body.value(value).operand(0);
    return value;
}

static bool branchAsksFieldAtInlining(const BodyIndex& body, BodyTerminator branch)
{
    auto isConstant = [&](ValueID value) {
        return !body.isParameter(value) && primitiveConstants.contains(body.opcodeOf(value));
    };
    ValueID asked = branch.operand(0);
    for (;;) {
        asked = valueBehindChecksAtInlining(body, asked);
        if (body.isParameter(asked))
            return false;
        BodyValue value = body.value(asked);
        switch (value.opcode()) {
        case Opcode::PropGet:
            return true;
        case Opcode::ToBoolean:
        case Opcode::LogicalNot:
        case Opcode::TypeTest:
            asked = value.operand(0);
            break;
        case Opcode::StrictEq:
        case Opcode::BitAnd: {
            ValueID left = valueBehindChecksAtInlining(body, value.operand(0));
            ValueID right = valueBehindChecksAtInlining(body, value.operand(1));
            if (isConstant(left) == isConstant(right))
                return false;
            asked = isConstant(left) ? right : left;
            break;
        }
        default:
            return false;
        }
    }
}

namespace {

struct KeptByInlining final : Walker::KeptForBuild {
    BitVector functionsWithoutHeads;
};

} // anonymous namespace

static ALWAYS_INLINE KeptByInlining& keptByInlining(const Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByInlining();
    if (!kept) [[unlikely]]
        kept = makeUniqueWithoutFastMallocCheck<KeptByInlining>();
    return static_cast<KeptByInlining&>(*kept);
}

static std::optional<BitVector> findHeadThatAsksFieldsAtInlining(const BodyIndex& body, uint32_t limit = inlineLimitOfHeads)
{
    Vector<BlockID, 8> head;
    Vector<BlockID, 8> queue { entryBlock };
    uint32_t weight = 0;
    uint32_t numberOfOperations = 0;
    for (size_t next = 0; next < queue.size(); ++next) {
        BlockID id = queue[next];
        if (std::ranges::contains(queue.span().first(next), id))
            continue;
        BodyBlock block = body.block(id);
        BodyTerminator end = block.terminator();
        bool isAdmitted = !block.hasHandler() && !block.isHandlerBlock() && !body.loopDepthOf(id) && (end.kind() == TerminatorKind::Branch || end.kind() == TerminatorKind::Jump || end.kind() == TerminatorKind::Return);
        uint32_t weightOfBlock = end.kind() == TerminatorKind::Branch;
        isAdmitted &= numberOfOperations + block.numberOfValues() <= inlineWindowOfWrittenCall;
        for (uint32_t place = 0; isAdmitted && place < block.numberOfValues(); ++place) {
            Opcode opcode = body.opcodeOf(ValueID(block.firstValue().index() + place));
            weightOfBlock += !operationsThatWeighNothingInHeadsAtInlining.contains(opcode);
            isAdmitted = operationsOfHeadsAtInlining.contains(opcode) && weight + weightOfBlock <= limit;
        }
        if (!isAdmitted || weight + weightOfBlock > limit) {
            if (!next)
                return std::nullopt;
            continue;
        }
        weight += weightOfBlock;
        numberOfOperations += block.numberOfValues();
        head.append(id);
        for (BodyTarget target : end.targets())
            queue.append(target.block());
    }
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = head.removeAllMatching([&](BlockID id) {
            BodyTerminator end = body.block(id).terminator();
            if (end.kind() == TerminatorKind::Return)
                return false;
            bool leadsOn = false;
            for (BodyTarget target : end.targets())
                leadsOn |= head.contains(target.block());
            return !leadsOn;
        });
    }
    if (!head.contains(entryBlock))
        return std::nullopt;
    for (BlockID id : head) {
        BodyTerminator end = body.block(id).terminator();
        for (BodyTarget target : end.targets()) {
            if (head.contains(target.block()))
                continue;
            BodyBlock behindHead = body.block(target.block());
            if (behindHead.hasHandler() || behindHead.isHandlerBlock() || !branchAsksFieldAtInlining(body, end))
                return std::nullopt;
        }
    }
    BitVector inlinedHeadBlocks(body.numberOfBlocks());
    for (BlockID id : head)
        inlinedHeadBlocks.quickSet(id.index());
    return inlinedHeadBlocks;
}

static constexpr OpcodeSet operationsOfHeadsTestingParameters = primitiveConstants | OpcodeSet { Opcode::Convert, Opcode::NarrowByTest, Opcode::TypeTest };
static constexpr OpcodeSet operationsReturnedByHeadsTestingParameters { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Negate, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static std::optional<BitVector> findHeadThatTestsParametersAtInlining(Walker& walker, const BodyIndex& body, std::span<const Reference> passedOperands)
{
    TypeArena& types = walker.types();
    auto mayPassTest = [&](BodyTerminator end) {
        ValueID asked = valueBehindChecksAtInlining(body, end.operand(0));
        if (body.isParameter(asked) || body.opcodeOf(asked) != Opcode::TypeTest)
            return false;
        BodyValue test = body.value(asked);
        std::optional<uint32_t> entryParameter = body.block(entryBlock).indexOfParameter(valueBehindChecksAtInlining(body, test.valueOperand()));
        if (!entryParameter || *entryParameter >= passedOperands.size())
            return false;
        Reference argument = passedOperands[*entryParameter];
        TypeID type = argument ? walker.typeOf(argument) : TypeID(TypeTable::undefined);
        return type && types.overlap(type, test.testedType());
    };
    Vector<BlockID, 8> head;
    Vector<BlockID, 8> queue { entryBlock };
    uint32_t weight = 0;
    bool returnsArithmetic = false;
    for (size_t next = 0; next < queue.size() && head.size() <= inlineLimitOfHeadsTestingParameters; ++next) {
        BlockID id = queue[next];
        if (std::ranges::contains(queue.span().first(next), id))
            continue;
        BodyBlock block = body.block(id);
        BodyTerminator end = block.terminator();
        bool returns = end.kind() == TerminatorKind::Return;
        bool isAdmitted = !block.hasHandler() && !block.isHandlerBlock() && !body.loopDepthOf(id) && (returns ? next > 0 : end.kind() == TerminatorKind::Branch && mayPassTest(end));
        uint32_t weightOfBlock = !returns;
        bool holdsArithmetic = false;
        for (uint32_t place = 0; isAdmitted && place < block.numberOfValues(); ++place) {
            Opcode opcode = body.opcodeOf(ValueID(block.firstValue().index() + place));
            holdsArithmetic |= operationsReturnedByHeadsTestingParameters.contains(opcode);
            isAdmitted = operationsOfHeadsTestingParameters.contains(opcode) || (returns && operationsReturnedByHeadsTestingParameters.contains(opcode));
            weightOfBlock += !operationsThatWeighNothingInHeadsAtInlining.contains(opcode) && !primitiveConstants.contains(opcode);
        }
        if (!isAdmitted || weight + weightOfBlock > inlineLimitOfHeadsTestingParameters)
            continue;
        weight += weightOfBlock;
        returnsArithmetic |= returns && holdsArithmetic;
        head.append(id);
        for (BodyTarget target : end.targets())
            queue.append(target.block());
    }
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = head.removeAllMatching([&](BlockID id) {
            BodyTerminator end = body.block(id).terminator();
            return end.kind() != TerminatorKind::Return && std::ranges::none_of(end.targets(), [&](BodyTarget target) { return head.contains(target.block()); });
        });
    }
    if (!returnsArithmetic || !head.contains(entryBlock))
        return std::nullopt;
    BitVector inlinedHeadBlocks(body.numberOfBlocks());
    for (BlockID id : head) {
        inlinedHeadBlocks.quickSet(id.index());
        for (BodyTarget target : body.block(id).terminator().targets()) {
            BodyBlock behindHead = body.block(target.block());
            if (!head.contains(target.block()) && (behindHead.hasHandler() || behindHead.isHandlerBlock()))
                return std::nullopt;
        }
    }
    return inlinedHeadBlocks;
}

namespace {

struct QuestionOfBits {
    ValueID object;
    PropertyKeyID key;
    int64_t mask { 0 };
    bool isNegated { false };
};

} // anonymous namespace

static std::optional<QuestionOfBits> findQuestionOfBitsAtInlining(const BodyIndex& body, BodyTerminator end)
{
    if (end.kind() != TerminatorKind::Branch)
        return std::nullopt;
    QuestionOfBits question;
    ValueID asked = valueBehindChecksAtInlining(body, end.operand(0));
    while (!body.isParameter(asked) && (body.opcodeOf(asked) == Opcode::ToBoolean || body.opcodeOf(asked) == Opcode::LogicalNot)) {
        question.isNegated ^= body.opcodeOf(asked) == Opcode::LogicalNot;
        asked = valueBehindChecksAtInlining(body, body.value(asked).operand(0));
    }
    if (body.isParameter(asked) || body.opcodeOf(asked) != Opcode::BitAnd)
        return std::nullopt;
    ValueID sides[2] = { valueBehindChecksAtInlining(body, body.value(asked).operand(0)), valueBehindChecksAtInlining(body, body.value(asked).operand(1)) };
    for (unsigned side = 0; side < 2; ++side) {
        ValueID read = sides[side];
        ValueID mask = sides[1 - side];
        if (body.isParameter(read) || body.isParameter(mask) || body.opcodeOf(read) != Opcode::PropGet || body.opcodeOf(mask) != Opcode::Integer || body.value(read).as<Opcode::PropGet>().isFreeName())
            continue;
        question.object = valueBehindChecksAtInlining(body, body.value(read).objectOperand());
        question.key = body.value(read).key();
        question.mask = body.value(mask).as<Opcode::Integer>().integer();
        return question;
    }
    return std::nullopt;
}

bool Walker::questionOfHeadWasAskedAbove(const BodyValue& call, FunctionID callee, const BitVector& inlinedHeadBlocks)
{
    const BodyIndex& bodyOfCallee = facts().bodies().at(callee);
    std::optional<QuestionOfBits> ofHead = findQuestionOfBitsAtInlining(bodyOfCallee, bodyOfCallee.terminator(entryBlock));
    std::optional<uint32_t> entryParameter = ofHead ? bodyOfCallee.block(entryBlock).indexOfParameter(ofHead->object) : std::nullopt;
    if (!entryParameter)
        return false;
    auto targetsOfHead = bodyOfCallee.terminator(entryBlock).targets();
    bool staysInHeadWhereTaken = inlinedHeadBlocks.get(targetsOfHead[0].block().index());
    bool staysInHeadWhereNotTaken = inlinedHeadBlocks.get(targetsOfHead[1].block().index());
    auto asksTheSame = [&](const QuestionOfBits& other) {
        return other.key == ofHead->key && other.mask == ofHead->mask;
    };

    auto answerOfBranchLeadsIntoHead = [&](const BodyIndex& index, BlockID asking, const QuestionOfBits& asked, BlockID blockOfSite) {
        auto targets = index.terminator(asking).targets();
        for (unsigned side = 0; side < 2; ++side) {
            BlockID target = targets[side].block();
            if (targets[0].block() == targets[1].block() || index.numberOfIncomingEdges(target) != 1 || !index.dominates(target, blockOfSite))
                continue;
            bool bitsAreSet = !side != asked.isNegated;
            return bitsAreSet != ofHead->isNegated ? staysInHeadWhereTaken : staysInHeadWhereNotTaken;
        }
        return true;
    };

    size_t frameIndex = m_frames.size() - 1;
    ValueID site = call.id();
    ValueID argument = findWrittenArgument(call, program().at(callee), *entryParameter);
    while (argument) {
        const BodyIndex& index = *m_frames[frameIndex].index;
        argument = valueBehindChecksAtInlining(index, argument);
        BlockID blockOfSite = index.blockOf(site);
        for (BlockID asking : { blockOfSite == entryBlock ? BlockID() : index.immediateDominator(blockOfSite), blockOfSite == entryBlock ? BlockID() : BlockID(entryBlock) }) {
            std::optional<QuestionOfBits> asked = asking ? findQuestionOfBitsAtInlining(index, index.terminator(asking)) : std::nullopt;
            if (asked && asksTheSame(*asked) && asked->object == argument)
                return answerOfBranchLeadsIntoHead(index, asking, *asked, blockOfSite);
        }
        auto& callsOfEntryBlock = m_questionsAskedByCallsOfEntryBlocks.ensure(&index, [&] {
            Vector<QuestionAskedByCall> asked;
            BodyBlock entry = index.block(entryBlock);
            for (uint32_t place = 0; place < entry.numberOfValues(); ++place) {
                ValueID id(entry.firstValue().index() + place);
                Opcode opcode = index.opcodeOf(id);
                if (opcode != Opcode::Call && opcode != Opcode::CallDirect)
                    continue;
                BodyValue other = index.value(id);
                FunctionID calledAbove = opcode == Opcode::CallDirect ? other.function() : facts().linked().calleeAt(m_frames[frameIndex].function, id);
                const BodyIndex* bodyAbove = calledAbove ? facts().bodies().find(calledAbove) : nullptr;
                std::optional<QuestionOfBits> question = bodyAbove && bodyAbove->opcodes().contains(Opcode::BitAnd) ? findQuestionOfBitsAtInlining(*bodyAbove, bodyAbove->terminator(entryBlock)) : std::nullopt;
                std::optional<uint32_t> parameterAsked = question ? bodyAbove->block(entryBlock).indexOfParameter(question->object) : std::nullopt;
                ValueID handed = parameterAsked ? findWrittenArgument(other, program().at(calledAbove), *parameterAsked) : ValueID();
                if (handed)
                    asked.append({ valueBehindChecksAtInlining(index, handed), question->key, question->mask, id });
            }
            std::ranges::sort(asked, { }, [](const QuestionAskedByCall& question) {
                return std::tuple(question.object.index(), question.key.bits(), question.mask, question.call.index());
            });
            return asked;
        }).iterator->value;
        auto first = std::ranges::lower_bound(callsOfEntryBlock, std::tuple(argument.index(), ofHead->key.bits(), ofHead->mask, 0u), { }, [](const QuestionAskedByCall& question) {
            return std::tuple(question.object.index(), question.key.bits(), question.mask, question.call.index());
        });
        if (first != callsOfEntryBlock.end() && first->object == argument && first->key == ofHead->key && first->mask == ofHead->mask && (blockOfSite != entryBlock || first->call < site))
            return true;
        if (!frameIndex || !index.isParameter(argument) || index.blockOf(argument) != entryBlock)
            return false;
        std::optional<uint32_t> parameterOfCopy = index.block(entryBlock).indexOfParameter(argument);
        site = m_frames[frameIndex].callCopied;
        FunctionID copied = m_frames[frameIndex].function;
        --frameIndex;
        argument = parameterOfCopy && site ? findWrittenArgument(m_frames[frameIndex].index->value(site), program().at(copied), *parameterOfCopy) : ValueID();
    }
    return false;
}

static ALWAYS_INLINE bool copyHeadThatAsksFieldsAtInlining(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    if ((call.opcode() != Opcode::Call && call.opcode() != Opcode::CallDirect) || !walker.opensGuesses() || site.isNeverInlined() || !walker.facts().bodies().has(callee))
        return false;
    if (call.opcode() == Opcode::CallDirect ? site.isConstruction() || site.role() != CodeRole::Body : !isEnteredAtInlining(walker.program().at(callee), false))
        return false;
    BitVector& functionsWithoutHeads = keptByInlining(walker).functionsWithoutHeads;
    std::optional<BitVector> head;
    if (!functionsWithoutHeads.get(callee.index())) {
        head = findHeadThatAsksFieldsAtInlining(walker.facts().bodies().at(callee));
        if (!head)
            functionsWithoutHeads.set(callee.index());
    }
    if (!head && walker.isInLoop())
        head = findHeadThatTestsParametersAtInlining(walker, walker.facts().bodies().at(callee), passedOperands);
    if (!head) {
        const BodyIndex& body = walker.facts().bodies().at(callee);
        if (!body.opcodes().contains(Opcode::BitAnd) || !findQuestionOfBitsAtInlining(body, body.terminator(entryBlock)))
            return false;
        head = findHeadThatAsksFieldsAtInlining(body, inlineLimitOfHeadsWhoseQuestionWasAsked);
        if (!head || !walker.questionOfHeadWasAskedAbove(call, callee, *head))
            return false;
        if (!walker.walkInline(call, callee, passedOperands, &site, &*head))
            return false;
        walker.count(StatisticID::PartialInlinesAfterDominatingTests);
        return true;
    }
    return walker.walkInline(call, callee, passedOperands, &site, &*head);
}

namespace {

struct PredicateResults {
    uint32_t whereFalse { 0 };
    uint32_t whereTrue { 0 };
};

constexpr uint32_t callableKind = 1 << 16;
constexpr uint32_t otherObjectKind = 1 << 17;
constexpr uint32_t objectKinds = callableKind | otherObjectKind;
constexpr uint32_t everyKind = Primitive::All | objectKinds;
constexpr std::array primitiveKinds { Primitive::Undefined, Primitive::Null, Primitive::False, Primitive::True, Primitive::Number, Primitive::String, Primitive::Symbol, Primitive::BigInt };
constexpr uint32_t maxOperationsOfPredicate = 80;
constexpr uint32_t maxPredicateSteps = 64;
constexpr unsigned maxPredicateDepth = 3;
constexpr size_t maxVariablesOfPredicate = 2;

class PredicateEvaluator {
    WTF_MAKE_NONCOPYABLE(PredicateEvaluator);
public:
    explicit PredicateEvaluator(Walker& walker)
        : m_walker(walker)
    {
    }

    PredicateResults evaluate(FunctionID, uint32_t entryParameter, uint32_t kinds);

    std::span<const VariableID> checkedVariables() const LIFETIME_BOUND { return m_checkedVariables.span(); }

    static uint32_t kindsIn(const Type&);

private:
    struct Value {
        ValueID name;
        bool isArgument { false };
        uint32_t kindsWhereTrue { 0 };
    };
    using Values = Vector<Value, 16>;

    static const Value* find(const Values&, ValueID);
    uint32_t kindsPassing(TypeID tested, uint32_t& kinds);
    void evaluateFrom(FunctionID, const BodyIndex&, BlockID, uint32_t kinds, Values, PredicateResults&);
    bool evaluateOperation(FunctionID, const BodyIndex&, const BodyValue&, uint32_t& kinds, Values&);

    Walker& m_walker;
    Vector<VariableID, 2> m_checkedVariables;
    uint32_t m_numberOfSteps { 0 };
    unsigned m_depth { 0 };
};

} // anonymous namespace

static uint32_t primitivesOfKind(uint16_t kind)
{
    return kind == Primitive::String ? Primitive::AnyString : kind == Primitive::Symbol ? Primitive::AnySymbol : kind;
}

uint32_t PredicateEvaluator::kindsIn(const Type& type)
{
    uint32_t kinds = type.hasObjects() ? objectKinds : 0;
    for (uint16_t kind : primitiveKinds) {
        if (type.primitives & primitivesOfKind(kind))
            kinds |= kind;
    }
    return kinds;
}

const PredicateEvaluator::Value* PredicateEvaluator::find(const Values& values, ValueID name)
{
    for (const Value& value : values) {
        if (value.name == name)
            return &value;
    }
    return nullptr;
}

uint32_t PredicateEvaluator::kindsPassing(TypeID tested, uint32_t& kinds)
{
    TypeArena& types = m_walker.types();
    Type type = types.at(tested);
    if (!type.isJavaScriptValue()) {
        kinds = 0;
        return 0;
    }
    uint32_t passing = 0;
    for (uint16_t kind : primitiveKinds) {
        bool isWhole = kind != Primitive::Number || (type.integers == Range::full() && type.boundsOfFractions.isEmpty());
        if ((type.primitives & kind) == kind && isWhole)
            passing |= kind;
        else if (type.primitives & primitivesOfKind(kind))
            kinds &= ~kind;
    }
    if (!type.hasObjects())
        return passing;
    if (type.anyObject)
        return passing | objectKinds;
    if (types.withOnlyObjects(tested) == TypeTable::function)
        return passing | callableKind;
    kinds &= ~objectKinds;
    return passing;
}

bool PredicateEvaluator::evaluateOperation(FunctionID function, const BodyIndex& body, const BodyValue& operation, uint32_t& kinds, Values& values)
{
    TypeArena& types = m_walker.types();
    auto bind = [&](uint32_t kindsWhereTrue) {
        values.append({ operation.id(), false, kindsWhereTrue & everyKind });
        return true;
    };
    switch (operation.opcode()) {
    case Opcode::TypeTest: {
        const Value* operand = find(values, operation.valueOperand());
        return operand && operand->isArgument && bind(kindsPassing(operation.testedType(), kinds));
    }
    case Opcode::ToBoolean:
    case Opcode::LogicalNot: {
        const Value* operand = find(values, operation.operand(0));
        if (!operand)
            return false;
        bool isNegated = operation.opcode() == Opcode::LogicalNot;
        if (!operand->isArgument)
            return bind(isNegated ? ~operand->kindsWhereTrue : operand->kindsWhereTrue);
        uint32_t truthy = kindsIn(types.at(truthyPartOf(types, TypeTable::anything)));
        uint32_t falsy = kindsIn(types.at(falsyPartOf(types, TypeTable::anything)));
        kinds &= truthy ^ falsy;
        return bind(isNegated ? falsy : truthy);
    }
    case Opcode::StrictEq: {
        const Value* left = find(values, operation.operand(0));
        const Value* right = find(values, operation.operand(1));
        if (!left || !right || !left->isArgument || !right->isArgument)
            return false;
        kinds &= ~Primitive::Number;
        return bind(everyKind);
    }
    case Opcode::Boolean:
        return bind(operation.as<Opcode::Boolean>().boolean() ? everyKind : 0);
    case Opcode::GetModuleVariable:
    case Opcode::GetEnvironmentField:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Integer:
    case Opcode::String:
        return true;
    case Opcode::CheckInitialized: {
        ValueID read = operation.operand(0);
        if (body.isParameter(read) || body.opcodeOf(read) != Opcode::GetModuleVariable)
            return false;
        VariableID variable = body.value(read).variable();
        if (!m_checkedVariables.contains(variable))
            m_checkedVariables.append(variable);
        return m_checkedVariables.size() <= maxVariablesOfPredicate;
    }
    case Opcode::Call: {
        FunctionID callee = m_walker.facts().linked().calleeAt(function, operation.id());
        if (!callee)
            return false;
        const Function& called = m_walker.program().at(callee);
        for (uint32_t entryParameter = 0; entryParameter < called.numberOfEntryParameters(); ++entryParameter) {
            ValueID written = findWrittenArgument(operation, called, entryParameter);
            const Value* argument = written ? find(values, written) : nullptr;
            if (!argument || !argument->isArgument)
                continue;
            PredicateResults results = evaluate(callee, entryParameter, kinds);
            kinds &= results.whereFalse | results.whereTrue;
            return bind(results.whereTrue);
        }
        return false;
    }
    default:
        return false;
    }
}

void PredicateEvaluator::evaluateFrom(FunctionID function, const BodyIndex& body, BlockID id, uint32_t kinds, Values values, PredicateResults& results)
{
    while (kinds) {
        BodyBlock block = body.block(id);
        if (block.hasHandler() || block.isHandlerBlock())
            return;
        for (BodyValue operation : block.values()) {
            if (++m_numberOfSteps > maxPredicateSteps || !evaluateOperation(function, body, operation, kinds, values) || !kinds)
                return;
        }
        BodyTerminator end = block.terminator();
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        auto enter = [&](BodyTarget target, Values& entered) {
            BodyBlock next = body.block(target.block());
            uint32_t position = 0;
            auto pass = [&](ValueID passed) {
                ValueID parameter = next.parameter(position++);
                const Value* known = passed ? find(values, passed) : nullptr;
                if (!known)
                    return;
                Value value = *known;
                value.name = parameter;
                entered.append(value);
            };
            for (ValueID argument : target.arguments())
                pass(argument);
            for (uint32_t operandIndex : target.proofs())
                pass(condition && !body.isParameter(condition) ? body.value(condition).operand(operandIndex) : ValueID());
            return target.block();
        };
        switch (end.kind()) {
        case TerminatorKind::Return: {
            const Value* returned = end.operand(0) ? find(values, end.operand(0)) : nullptr;
            if (returned && !returned->isArgument) {
                results.whereTrue |= kinds & returned->kindsWhereTrue;
                results.whereFalse |= kinds & ~returned->kindsWhereTrue;
            }
            return;
        }
        case TerminatorKind::Jump: {
            Values entered = values;
            id = enter(end.targets()[0], entered);
            values = WTF::move(entered);
            break;
        }
        case TerminatorKind::Branch: {
            const Value* asked = find(values, condition);
            if (!asked || asked->isArgument)
                return;
            uint32_t taken = kinds & asked->kindsWhereTrue;
            uint32_t notTaken = kinds & ~asked->kindsWhereTrue;
            auto targets = end.targets();
            if (taken) {
                Values entered = values;
                BlockID next = enter(targets[0], entered);
                evaluateFrom(function, body, next, taken, WTF::move(entered), results);
            }
            Values entered = values;
            id = enter(targets[1], entered);
            values = WTF::move(entered);
            kinds = notTaken;
            break;
        }
        default:
            return;
        }
    }
}

PredicateResults PredicateEvaluator::evaluate(FunctionID id, uint32_t entryParameter, uint32_t kinds)
{
    const Function& function = m_walker.program().at(id);
    const ThinSummary& summary = m_walker.facts().summaries().at(id);
    const BodyIndex* body = m_walker.facts().bodies().find(id);
    PredicateResults results;
    if (!body || m_depth == maxPredicateDepth || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > maxOperationsOfPredicate || !isEnteredAtInlining(function, false) || function.isCoroutine()
        || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || entryParameter >= body->block(entryBlock).numberOfParameters() || body->numberOfIncomingEdges(entryBlock)
        || function.entryParameterKind(entryParameter) != EntryParameter::Parameter || takesListAtInlining(function, entryParameter) || tellsAbsentFromUndefinedAtInlining(m_walker, function, entryParameter))
        return results;
    if (const DeclaredType* declared = function.declaredEntryParameter(entryParameter); declared && declared->written() && !m_walker.types().at(declared->written()).isAnything())
        return results;
    ++m_depth;
    evaluateFrom(id, *body, entryBlock, kinds, { Value { body->block(entryBlock).parameter(entryParameter), true, 0 } }, results);
    --m_depth;
    return results;
}

static TypeID typeThatTestsKinds(TypeArena& types, uint32_t kinds, uint32_t kindsOfArgument)
{
    TypeID tested = TypeArena::fromPrimitives(kinds & Primitive::All);
    uint32_t objects = kinds & objectKinds;
    if (objects && objects == (kindsOfArgument & objectKinds))
        tested = types.unionOf(tested, TypeTable::object);
    else if (objects == callableKind)
        tested = types.unionOf(tested, TypeTable::function);
    else if (objects)
        return { };
    return types.testablePartOf(tested) == tested ? tested : TypeID();
}

static NEVER_INLINE bool inlinePredicateByKinds(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    TypeArena& types = walker.types();
    if (!types.at(walker.facts().linked().function(callee).result).isBoolean())
        return false;
    if (call.opcode() != Opcode::Call || !walker.opensGuesses() || site.resultIsReturnedAtOnce || site.isTailCall || site.isNeverInlined() || (!walker.isInLoop() && !unitIsCalledInLoops(walker)))
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->opcodes().contains(Opcode::TypeTest) || walker.facts().isTakenToRunOnce(callee))
        return false;
    const Function& function = walker.program().at(callee);
    for (uint32_t entryParameter = 0; entryParameter < passedOperands.size(); ++entryParameter) {
        Reference argument = passedOperands[entryParameter];
        if (!argument || !argument.isValue() || function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
            continue;
        TypeID type = walker.typeShown(argument);
        if (!type || isNeverType(type) || !types.at(type).isJavaScriptValue() || (types.at(type).primitives & (Primitive::Absent | Primitive::Unspecified)))
            continue;
        uint32_t kinds = PredicateEvaluator::kindsIn(types.at(type));
        PredicateEvaluator evaluator(walker);
        PredicateResults results = evaluator.evaluate(callee, entryParameter, kinds);
        if (!results.whereFalse && !results.whereTrue)
            continue;

        bool isEveryKindKnown = !(kinds & ~(results.whereFalse | results.whereTrue));
        TypeID testedForResult = isEveryKindKnown && results.whereFalse && results.whereTrue ? typeThatTestsKinds(types, results.whereTrue, kinds) : TypeID();
        bool constantResult = std::popcount(results.whereTrue) > std::popcount(results.whereFalse);
        TypeID leftToCall;
        if (!testedForResult) {
            uint32_t kindsLeft = kinds & ~(constantResult ? results.whereTrue : results.whereFalse);
            if (kindsLeft & otherObjectKind)
                kindsLeft |= kinds & callableKind;
            bool sparesCalls = !(kindsLeft & (Primitive::Number | Primitive::String));
            leftToCall = kindsLeft && kindsLeft != kinds && sparesCalls ? typeThatTestsKinds(types, kindsLeft, kinds) : TypeID();
            if (kindsLeft && !leftToCall)
                return false;
        }

        Origin origin = site.origin();
        origin.isTailCall = false;
        auto buildTest = [&](Reference value, TypeID tested) {
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(value);
            test.setTestedType(tested);
            return walker.build(WTF::move(test));
        };
        BlockID whereCalled;
        auto leaveForCall = [&](Reference isLeft) {
            if (!whereCalled)
                whereCalled = walker.addBlock();
            BlockID whereKnown = walker.addBlock();
            walker.branch(isLeft, whereCalled, whereKnown);
            walker.continueIn(whereKnown);
        };
        walker.count(StatisticID::PredicatesTestedAtCalls);
        for (VariableID variable : evaluator.checkedVariables()) {
            Draft read(Opcode::GetModuleVariable, origin);
            read.setVariable(variable);
            read.setReturnsRawSlot(true);
            walker.chooseConservativeFields(read);
            Reference held = walker.build(WTF::move(read));
            if (!held || walker.isUnreachable())
                return true;
            if (!(types.at(walker.typeOf(held)).primitives & Primitive::Absent))
                continue;
            if (Reference isAbsent = buildTest(held, TypeArena::fromPrimitives(Primitive::Absent)))
                leaveForCall(isAbsent);
            if (walker.isUnreachable() || walker.hasFailed())
                return true;
        }
        if (leftToCall) {
            if (Reference isLeft = buildTest(argument, leftToCall))
                leaveForCall(isLeft);
            if (walker.isUnreachable() || walker.hasFailed())
                return true;
        }
        Reference known;
        if (testedForResult)
            known = buildTest(argument, testedForResult);
        else {
            Draft constant(Opcode::Boolean, origin);
            constant.setBoolean(constantResult);
            known = walker.constant(WTF::move(constant));
        }
        if (!whereCalled || !known) {
            if (known)
                walker.bind(call.id(), known);
            return true;
        }

        const BodyIndex& index = walker.currentBodyIndex();
        BodyBlock block = index.block(index.blockOf(call.id()));
        uint32_t place = call.id().index() - block.firstValue().index();
        BlockID whereKnown = walker.currentBlock();
        BlockID continuationBlock = walker.addBlock();
        Reference result;
        walker.continueIn(whereCalled);
        {
            Walker::ClosedArm calls(walker);
            walker.walkStretch(block.id(), place, place + 1);
            if (walker.hasFailed())
                return true;
            Reference called = walker.isUnreachable() ? Reference() : walker.referenceOf(call.id());
            bool keepsForm = called && types.isSubset(walker.typeOf(known), walker.typeOf(called));
            result = walker.addParameter(continuationBlock, keepsForm ? walker.typeOf(called) : TypeID(TypeTable::anything), keepsForm ? walker.representationOf(called) : Representation::JSValue);
            if (called)
                walker.jumpTo(continuationBlock, singleElementSpan(called));
        }
        walker.continueIn(whereKnown);
        walker.jumpTo(continuationBlock, singleElementSpan(known));
        walker.continueIn(continuationBlock);
        walker.bind(call.id(), result);
        return true;
    }
    return false;
}

namespace {

struct NumberVersion {
    uint32_t operationsLeft { 0 };
    uint32_t operationsDecided { 0 };
    bool comparesArguments { false };
    bool returnsArgument { false };
};

constexpr uint32_t maxOperationsOfComparator = 64;
constexpr uint32_t maxOperationsLeftForNumbers = 10;
constexpr uint32_t minOperationsDecidedByNumbers = 4;

class NumberVersionEvaluator {
    WTF_MAKE_NONCOPYABLE(NumberVersionEvaluator);
public:
    explicit NumberVersionEvaluator(Walker& walker)
        : m_walker(walker)
    {
    }

    std::optional<NumberVersion> evaluate(FunctionID, std::span<const uint32_t> entryParameters);

private:
    enum class Kind : uint8_t { Other, Variable, OtherConstant, Argument, Number, Boolean, True, False };

    bool evaluateOperation(FunctionID, const BodyValue&);
    static bool isNumber(Kind kind) { return kind == Kind::Argument || kind == Kind::Number; }
    static Kind negated(Kind kind) { return kind == Kind::True ? Kind::False : kind == Kind::False ? Kind::True : kind; }
    Kind& kindOf(ValueID value) { return m_kinds[value.index()]; }

    Walker& m_walker;
    Vector<Kind> m_kinds;
    NumberVersion m_version;
};

} // anonymous namespace

bool NumberVersionEvaluator::evaluateOperation(FunctionID function, const BodyValue& operation)
{
    TypeArena& types = m_walker.types();
    Kind& kind = kindOf(operation.id());
    Opcode opcode = operation.opcode();
    switch (opcode) {
    case Opcode::Integer:
    case Opcode::Double:
        kind = Kind::Number;
        return true;
    case Opcode::Boolean:
        kind = operation.as<Opcode::Boolean>().boolean() ? Kind::True : Kind::False;
        return true;
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::String:
        kind = Kind::OtherConstant;
        return true;
    case Opcode::GetModuleVariable:
    case Opcode::GetEnvironmentField:
    case Opcode::CheckInitialized:
        kind = Kind::Variable;
        return true;
    case Opcode::TypeTest:
        if (!isNumber(kindOf(operation.valueOperand())))
            return false;
        if (types.isSubset(TypeTable::number, operation.testedType()))
            kind = Kind::True;
        else if (!types.overlap(TypeTable::number, operation.testedType()))
            kind = Kind::False;
        else
            return false;
        ++m_version.operationsDecided;
        return true;
    case Opcode::ToBoolean:
    case Opcode::LogicalNot: {
        Kind operand = kindOf(operation.operand(0));
        if (operand <= Kind::OtherConstant || isNumber(operand))
            return false;
        kind = opcode == Opcode::LogicalNot ? negated(operand) : operand;
        return true;
    }
    case Opcode::Call: {
        FunctionID callee = m_walker.facts().linked().calleeAt(function, operation.id());
        if (!callee)
            return false;
        const Function& called = m_walker.program().at(callee);
        for (uint32_t entryParameter = 0; entryParameter < called.numberOfEntryParameters(); ++entryParameter) {
            ValueID written = findWrittenArgument(operation, called, entryParameter);
            if (!written || !isNumber(kindOf(written)))
                continue;
            PredicateResults results = PredicateEvaluator(m_walker).evaluate(callee, entryParameter, Primitive::Number);
            if (results.whereTrue != Primitive::Number && results.whereFalse != Primitive::Number)
                return false;
            kind = results.whereTrue ? Kind::True : Kind::False;
            m_version.operationsDecided += 2;
            return true;
        }
        return false;
    }
    default:
        break;
    }
    bool isRelation = allRelations.contains(opcode);
    bool isEquality = opcode == Opcode::StrictEq || opcode == Opcode::Eq;
    if (!isRelation && !isEquality && !numberProducingOperators.contains(opcode))
        return false;
    if (opcode == Opcode::StrictEq) {
        Kind left = kindOf(operation.operand(0));
        Kind right = kindOf(operation.operand(1));
        if ((left == Kind::OtherConstant && isNumber(right)) || (right == Kind::OtherConstant && isNumber(left))) {
            kind = Kind::False;
            ++m_version.operationsDecided;
            return true;
        }
        if ((left == Kind::Variable && isNumber(right)) || (right == Kind::Variable && isNumber(left))) {
            kind = Kind::Boolean;
            return ++m_version.operationsLeft <= maxOperationsLeftForNumbers;
        }
    }
    bool takesNumbers = true;
    bool takesArguments = true;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        takesNumbers &= isNumber(kindOf(operand));
        takesArguments &= kindOf(operand) == Kind::Argument;
    });
    if (!takesNumbers)
        return false;
    kind = isRelation || isEquality ? Kind::Boolean : Kind::Number;
    m_version.operationsDecided += takesArguments;
    m_version.comparesArguments |= isRelation && takesArguments;
    return ++m_version.operationsLeft <= maxOperationsLeftForNumbers;
}

std::optional<NumberVersion> NumberVersionEvaluator::evaluate(FunctionID id, std::span<const uint32_t> entryParameters)
{
    const Function& function = m_walker.program().at(id);
    const ThinSummary& summary = m_walker.facts().summaries().at(id);
    const BodyIndex* body = m_walker.facts().bodies().find(id);
    if (!body || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > maxOperationsOfComparator || !body->holdsAnyOf(allRelations) || !isEnteredAtInlining(function, false) || function.isCoroutine()
        || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || body->numberOfIncomingEdges(entryBlock))
        return std::nullopt;
    m_kinds.fill(Kind::Other, body->numberOfValues());
    for (uint32_t entryParameter : entryParameters)
        kindOf(body->block(entryBlock).parameter(entryParameter)) = Kind::Argument;
    Vector<BlockID, 16> worklist { entryBlock };
    BitVector isVisited(body->numberOfBlocks());
    isVisited.set(entryBlock.index());
    while (!worklist.isEmpty()) {
        BodyBlock block = body->block(worklist.takeLast());
        if (block.hasHandler() || block.isHandlerBlock())
            return std::nullopt;
        for (BodyValue operation : block.values()) {
            if (!evaluateOperation(id, operation))
                return std::nullopt;
        }
        BodyTerminator end = block.terminator();
        if (end.kind() == TerminatorKind::Return) {
            m_version.returnsArgument |= end.operand(0) && kindOf(end.operand(0)) == Kind::Argument;
            continue;
        }
        if (end.kind() != TerminatorKind::Branch && end.kind() != TerminatorKind::Jump)
            return std::nullopt;
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        Kind asked = condition ? kindOf(condition) : Kind::True;
        if (asked <= Kind::OtherConstant || isNumber(asked))
            return std::nullopt;
        bool isTaken = true;
        for (BodyTarget target : end.targets()) {
            if (asked == (std::exchange(isTaken, false) ? Kind::False : Kind::True))
                continue;
            if (isVisited.set(target.block().index()))
                continue;
            BodyBlock next = body->block(target.block());
            uint32_t position = 0;
            for (ValueID argument : target.arguments())
                kindOf(next.parameter(position++)) = kindOf(argument);
            for (uint32_t operandIndex : target.proofs()) {
                ValueID proven = condition && !body->isParameter(condition) ? body->value(condition).operand(operandIndex) : ValueID();
                kindOf(next.parameter(position++)) = proven ? kindOf(proven) : Kind::Other;
            }
            worklist.append(target.block());
        }
    }
    return m_version;
}

static NEVER_INLINE bool inlineComparatorForNumbers(Walker& walker, const BodyValue& call, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands)
{
    if (call.opcode() != Opcode::Call || !walker.opensGuesses() || site.resultIsReturnedAtOnce || site.isTailCall || site.isNeverInlined() || (!walker.isInLoop() && !unitIsCalledInLoops(walker)))
        return false;
    const BodyIndex* body = walker.facts().bodies().find(callee);
    if (!body || !body->holdsAnyOf(allRelations) || walker.facts().isTakenToRunOnce(callee))
        return false;
    const Function& function = walker.program().at(callee);
    TypeArena& types = walker.types();
    Vector<uint32_t, 2> entryParameters;
    Vector<TestOfNumber, 2> tests;
    for (uint32_t entryParameter = 0; entryParameter < passedOperands.size(); ++entryParameter) {
        Reference argument = passedOperands[entryParameter];
        if (!argument || function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
            continue;
        if (const DeclaredType* declared = function.declaredEntryParameter(entryParameter); declared && declared->written() && !types.at(declared->written()).isAnything())
            return false;
        ValueID written = findWrittenArgument(call, function, entryParameter);
        TypeID type = walker.typeOf(argument);
        if (!written || !type || !types.overlap(type, TypeTable::number))
            return false;
        if (!types.at(type).isNumber()) {
            if (!argument.isValue() || !types.at(type).isJavaScriptValue() || walker.representationOf(argument) != Representation::JSValue)
                return false;
            tests.append(TestOfNumber { written, TypeTable::number });
        }
        entryParameters.append(entryParameter);
    }
    if (entryParameters.size() != 2 || !operationsFitWindowOfCallWrittenInLoop(walker, maxOperationsLeftForNumbers))
        return false;
    std::optional<NumberVersion> version = NumberVersionEvaluator(walker).evaluate(callee, entryParameters.span());
    if (!version || !version->comparesArguments || version->operationsDecided < minOperationsDecidedByNumbers)
        return false;
    if (tests.isEmpty()) {
        Vector<Reference, 8> numbers(passedOperands);
        for (uint32_t entryParameter : entryParameters) {
            if (version->returnsArgument || !walker.has(Mechanism::UnboxedNumbers) || !numbers[entryParameter].isValue() || walker.representationOf(numbers[entryParameter]) != Representation::JSValue)
                continue;
            Draft unboxed(Opcode::Unbox, site.origin());
            unboxed.setOperand(0, numbers[entryParameter]);
            unboxed.representation = Representation::Double;
            if (Reference number = walker.build(WTF::move(unboxed)))
                numbers[entryParameter] = number;
            if (walker.isUnreachable() || walker.hasFailed())
                return true;
        }
        return walker.walkInline(call, callee, numbers.span(), &site);
    }
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t begin = call.id().index() - block.firstValue().index();
    if (walker.inputBlockBeingWalked() != block.id() || begin >= limitOfStretchOfNumbers(walker, block))
        return false;
    walker.count(StatisticID::ComparatorsInlinedForNumbers);
    return walkStretchBehindTestsOfNumbers(walker, tests.span(), block.id(), begin, begin + 1, BlockFrequency::Normal, EndOfNumberArms::MayTakeBranch);
}

static bool copyLearnsOfThisAtInlining(Walker& walker, const BodyValue& call, FunctionID callee)
{
    ValueID written = call.as<Opcode::Call>().thisValueOperand();
    TypeID handed = written ? walker.typeOf(walker.referenceOf(written)) : TypeID();
    TypeID own = walker.program().at(callee).thisType.written();
    return !handed || !own || !walker.types().isSubset(walker.types().withoutNullish(own), handed);
}

bool isSmallLeaf(const Walker& walker, FunctionID callee, size_t numberOfArguments)
{
    const LinkedFunction& linked = walker.facts().linked().function(callee);
    if (linked.weightAsLeaf == LinkedFunction::notLeaf || linked.weightAsLeaf > inlineLimitOfLeaves + numberOfArguments)
        return false;
    return !linked.result || (walker.types().at(linked.result).primitives & ~cellPrimitives);
}

bool inlineCalleeOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (callsOfListsAtInlining.contains(call.opcode()))
        return inlineCalleeOfListWrittenOut(walker, call);
    if (!opcodesOfInlining.contains(call.opcode()))
        return false;
    if (call.opcode() == Opcode::Call && walker.hasUninformativeType(walker.referenceOf(call.calleeOperand())))
        return false;

    Reference closure = call.opcode() == Opcode::CallDirect ? Reference() : walker.referenceOf(call.opcode() == Opcode::SuperConstruct ? call.parentOperand() : call.calleeOperand());
    FunctionID callee = call.opcode() == Opcode::CallDirect ? call.function() : findOnlyCallee(walker, call, closure);
    bool calleeStandsBehindBuiltin = !callee && call.opcode() == Opcode::Call && !call.isDoneInPlace() && closure;
    if (calleeStandsBehindBuiltin)
        callee = findLibraryFunctionBehindBuiltin(walker, walker.typeOf(closure));
    if (TypeID proven = !callee && call.opcode() == Opcode::Construct && closure ? walker.typeOf(closure) : TypeID()) {
        if (Type type = walker.types().at(proven); type.isObject() && type.isJavaScriptValue())
            callee = findNakedConstructorOf(walker.program(), singleBuiltinOf(walker.types(), type));
    }
    if (!callee)
        return false;

    if (copyKeyOfComparator(walker, call, callee))
        return true;
    if (call.opcode() == Opcode::Call && walker.standsAtFrameOfRowOfShapeTests() && !copyLearnsOfThisAtInlining(walker, call, callee)
        && !(walker.isInLoop() && (isSmallLeaf(walker, callee, call.as<Opcode::Call>().argumentsOperands().size()) || loopRemovesResult(walker, call.id(), callee) || (walker.facts().linked().function(callee).mayCallBack && callsBackAtInlining(walker, call, callee)))))
        return false;
    Draft site = walker.draftOf(call);
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<Reference, 8> passedOperands = collectPassedOperands(site, walker.program().at(callee));
    bool callsParameterOfCopy = call.opcode() == Opcode::Call && !walker.walksOwnBodyOfUnit() && index.block(entryBlock).indexOfParameter(index.stripCasts(call.as<Opcode::Call>().calleeOperand()));
    OperandsInSightAtInlining inSight { passedOperands.span(), callsParameterOfCopy, calleeStandsBehindBuiltin };
    if (callAdmitsCopyAtInlining(walker, site, callee, { walker.currentFunction(), walker.isInLoop() || bodyOfUnitTakesCopyOfItselfAtInlining(walker, site, callee) || isLibraryFunctionInRecursion(walker, callee, passedOperands.span()), &index, call.id() }, &inSight) && walker.walkInline(call, callee, passedOperands.span(), &site))
        return true;
    if (calleeStandsBehindBuiltin || walker.hasFailed())
        return false;
    if (copyHeadThatReturnsAtOnceAtInlining(walker, call, site, callee, passedOperands.span()) || (!walker.hasFailed() && copyHeadThatAsksFieldsAtInlining(walker, call, site, callee, passedOperands.span())))
        return true;
    TypeID result = walker.facts().linked().function(callee).result;
    if (walker.hasFailed() || !result || !walker.types().at(result).hasOnlyPrimitives(Primitive::Boolean | Primitive::Number))
        return false;
    return inlinePredicateByKinds(walker, call, site, callee, passedOperands.span()) || (!walker.hasFailed() && inlineComparatorForNumbers(walker, call, site, callee, passedOperands.span()));
}

struct ClosureMadeForCallAtInlining {
    Reference made;
    ValueID written;
};

static ALWAYS_INLINE Vector<ClosureMadeForCallAtInlining, 2> collectClosuresMadeForCallAtInlining(Walker& walker, const BodyValue& call)
{
    Vector<ClosureMadeForCallAtInlining, 2> closures;
    const BodyIndex& index = walker.currentBodyIndex();
    call.forEachOperand([&](ValueID operand, uint32_t) {
        ValueID written = operand;
        while (written && !index.isParameter(written) && index.hasOneUse(written) && inputIdentityOpcodes.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure || index.blockOf(written) != index.blockOf(call.id()))
            return;
        Reference made = walker.referenceOf(operand);
        if (made && made.isValue() && walker.code().at(made.id()).opcode == Opcode::NewClosure)
            closures.append({ made, written });
    });
    return closures;
}

bool Walker::copyHandsBack(ValueID input)
{
    Frame& frame = m_frames.last();
    const BodyIndex& body = *frame.index;
    if (!frame.hasLookedForValueHandedBack) {
        frame.hasLookedForValueHandedBack = true;
        ValueID only;
        bool isOne = true;
        for (uint32_t number = 0; number < body.numberOfBlocks() && isOne; ++number) {
            if (!body.isReachable(BlockID(number)) || !endsInReturnAtInlining(body.terminator(BlockID(number))))
                continue;
            ValueID returned = body.terminator(BlockID(number)).operand(0);
            if (returned)
                returned = body.stripCasts(returned);
            isOne = returned && (!only || only == returned);
            only = returned;
        }
        frame.valueHandedBack = isOne ? only : ValueID();
    }
    return frame.valueHandedBack && frame.valueHandedBack == body.stripCasts(input);
}

static bool onlyReckonsWithParameter(const BodyIndex& body, ValueID parameter)
{
    static constexpr OpcodeSet takesUndefinedAsNaN = numberProducingOperators | allRelations;
    static constexpr OpcodeSet writtenNumbers { Opcode::Integer, Opcode::Double };
    bool onlyReckons = true;
    for (BodyValue operation : body.block(entryBlock).values()) {
        bool takesParameter = false;
        bool standsBesideNumbers = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            ValueID taken = operand ? body.stripCasts(operand) : ValueID();
            if (taken == parameter)
                takesParameter = true;
            else
                standsBesideNumbers &= taken && !body.isParameter(taken) && writtenNumbers.contains(body.opcodeOf(taken));
        });
        if (takesParameter && !opcodesThatStateTypes.contains(operation.opcode()))
            onlyReckons &= takesUndefinedAsNaN.contains(operation.opcode()) || (operation.opcode() == Opcode::Add && standsBesideNumbers);
    }
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        BodyTerminator end = body.terminator(BlockID(number));
        end.forEachOperand([&](ValueID operand, uint32_t) {
            onlyReckons &= !operand || body.stripCasts(operand) != parameter;
        });
        for (BodyTarget target : end.targets()) {
            for (ValueID handedOn : target.arguments())
                onlyReckons &= !handedOn || body.stripCasts(handedOn) != parameter;
        }
        onlyReckons &= !number || !body.block(BlockID(number)).numberOfValues();
    }
    return onlyReckons;
}

static bool guardsAskForUndefinedAlone(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    bool someGuardAsks = false;
    for (uint32_t index = 0; index < entryArguments.size(); ++index) {
        TypeID tested = conventions.typeBehindGuard(callee, index);
        if (!tested || types.isSubset(walker.ownTypeOf(entryArguments[index]), tested))
            continue;
        Type arriving = types.at(walker.ownTypeOf(entryArguments[index]));
        if (!types.at(tested).isNumber() || !entryArguments[index].isValue() || walker.representationOf(entryArguments[index]) != Representation::OptionalDouble
            || !arriving.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || !types.isSubset(types.intersectionOf(walker.ownTypeOf(entryArguments[index]), TypeTable::number), tested))
            return false;
        const BodyIndex& body = walker.facts().bodies().at(callee);
        if (index >= body.block(entryBlock).numberOfParameters() || !onlyReckonsWithParameter(body, body.block(entryBlock).parameter(index)))
            return false;
        someGuardAsks = true;
    }
    return someGuardAsks;
}

bool Walker::walkInlineUnderHandlerMade(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft& writtenOut, BlockID handler)
{
    SetForScope handlerMade(m_handlerMadeOverCopies, handler);
    SetForScope framesBelow(m_numberOfFramesBelowHandlerMade, m_frames.size());
    return walkInline(call, callee, arguments, &writtenOut);
}

std::optional<ArrayStorage> predictStorageInRecursion(Walker&, const BodyValue& read, TypeID list);

static bool readsParameterElements(Walker& walker, const BodyIndex& index)
{
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!index.isReachable(block) || index.loopOf(block))
            continue;
        for (BodyValue operation : index.block(block).values()) {
            if (operation.opcode() != Opcode::ElemGet)
                continue;
            ValueID list = index.stripCasts(operation.objectOperand());
            while (!index.isParameter(list) && index.opcodeOf(list) == Opcode::CheckInitialized)
                list = index.stripCasts(index.value(list).operand(0));
            if (!index.isParameter(list) && index.opcodeOf(list) == Opcode::GetModuleVariable) {
                TypeID held = walker.positions().variable(TypeLevel::Proven, index.value(list).variable());
                if (held && predictStorageInRecursion(walker, operation, walker.types().withoutPrimitives(held, Primitive::Absent)))
                    return true;
                continue;
            }
            if (!index.isParameter(list) || index.blockOf(list) != entryBlock)
                continue;
            Reference arriving = walker.referenceBoundTo(list);
            if (arriving && predictStorageInRecursion(walker, operation, walker.typeOf(arriving)))
                return true;
        }
    }
    return false;
}

bool stepsParameterRoundCycle(Walker&);

bool Walker::shouldSpecializeBody()
{
    static constexpr OpcodeSet unsupportedOpcodes { Opcode::CreateUnmappedArguments };
    const Function& function = program().at(unit().function);
    const BodyIndex& index = *m_frames[0].index;
    if (unit().role != CodeRole::Body || !m_opensGuesses || !has(Mechanism::Guesses) || !has(Mechanism::Folding) || !has(Mechanism::Inlining) || function.isCoroutine() || program().admitsInspector)
        return false;
    if (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow && function.kind != FunctionKind::Method)
        return false;
    if (function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || m_restArrayAllocator || !m_storesOfCapturedValues.empty() || index.holdsAnyOf(unsupportedOpcodes) || index.hasLoops())
        return false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (index.block(BlockID(number)).hasHandler())
            return false;
    }
    return (index.opcodes().contains(Opcode::ElemGet) && callsItself(*this, index, unit().function) && readsParameterElements(*this, index)) || stepsParameterRoundCycle(*this);
}

void guardArraysAtEntry(Walker&);

void Walker::walkSpecializedBody()
{
    const BodyIndex& index = *m_frames[0].index;
    BodyBlock entry = index.block(entryBlock);
    uint32_t inlineFrame = m_frames[0].inlineFrame;
    BlockID fallback;
    letGuessFailUnasked(fallback, m_origin);
    Vector<Reference, 8> arriving;
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
        arriving.append(referenceBoundTo(entry.parameter(parameter)));

    Frame& frame = m_frames.appendNew();
    frame.function = unit().function;
    frame.body = &index.body();
    frame.index = &index;
    frame.variablesStoredFirst = m_frames[0].variablesStoredFirst;
    frame.references.lay(index.numberOfValues());
    frame.inlineFrame = inlineFrame;
    frame.isSpecializedBody = true;
    frame.returnsInPlaceOfUnit = true;
    frame.callsAreCountedByPlan = true;
    frame.mayLeaveForCall = true;
    frame.whereCopyLeavesForCall = fallback;
    frame.inlineeRestartPoint = ++m_restartPointCount;
    frame.numberOfValuesBuiltBefore = 0;
    Vector<Frame::ReturnOfCopy, 2> returns;
    frame.returnsOfCopy = &returns;
    m_state.setInlineeRestartPoint(frame.inlineeRestartPoint);

    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter)
        bind(entry.parameter(parameter), arriving[parameter]);
    setCurrentInputBlock(entryBlock);
    guardArraysAtEntry(*this);
    for (uint32_t number = 0; number < index.numberOfBlocks() && !m_hasFailed && (number || !isUnreachable()); ++number)
        walkBlock(BlockID(number));
    m_block = { };
    m_state.clear();
    fallback = m_frames.last().whereCopyLeavesForCall;
    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    if (m_hasFailed || !fallback)
        return;
    continueIn(fallback);
    m_state.clearRestartMark();
}

bool Walker::canGuardRecursion()
{
    if (!isInRecursiveCopy() || m_frames.last().inlinedHeadBlocks || !mayLeaveCopyForCall())
        return false;
    const Frame& leaving = frameWhoseCopyLeavesForCall();
    return leaving.isSpecializedBody || leaving.canCallInstead;
}

BlockID Walker::elementGuardFallback()
{
    Frame& frame = frameWhoseCopyLeavesForCall();
    if (frame.isSpecializedBody)
        return blockWhereCopyLeavesForCall();
    if (!frame.whereHeadEnds)
        frame.whereHeadEnds = addBlock(BlockFrequency::Rare);
    return frame.whereHeadEnds;
}

bool Walker::mayLeaveCopyForCall() const
{
    const Frame& frame = m_frames.last();
    return frame.mayLeaveForCall && !isUnreachable() && m_state.isInlineeEffectFree(frame.inlineeRestartPoint) && !isUnderHandlerOtherThanCleanups() && !hasStringsInBuilders() && !m_coldCopyDepth;
}

bool Walker::walkInlineFusedChain(const BodyValue& call, FunctionID text, std::span<const Reference> arguments, const Draft& site, BlockID& asWritten)
{
    m_armOfNextFusedChain = &asWritten;
    bool isCopied = walkInline(call, text, arguments, &site);
    m_armOfNextFusedChain = nullptr;
    return isCopied;
}

bool Walker::walkInlineKey(const BodyValue& call, FunctionID comparator, std::span<const Reference> arguments, const Draft& site, ValueID key)
{
    m_keyOfNextCopy = key;
    bool isCopied = walkInline(call, comparator, arguments, &site);
    m_keyOfNextCopy = { };
    return isCopied;
}

bool Walker::isElementReadByName(Reference argument, const BodyIndex& callback, uint32_t parameter) const
{
    if (!m_frameOfFusedChain || m_frames.size() != m_frameOfFusedChain || !mayLeaveFusedChainHere() || m_code.at(underlyingCellOf(m_code, argument.id())).opcode != Opcode::GetOwnContiguousIndex)
        return false;
    ValueID element(parameter);
    std::optional<BodyUser> first = callback.firstUser(element);
    while (first && first->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(callback.opcodeOf(first->value))) {
        element = first->value;
        first = callback.firstUser(element);
    }
    if (!first || first->kind != BodyUseKind::Operand || callback.blockOf(first->value) != entryBlock || callback.opcodeOf(first->value) != Opcode::PropGet)
        return false;
    BodyValue read = callback.value(first->value);
    return !read.isFreeName() && read.objectOperand() == element;
}

bool Walker::fillsListOfFusedChain(const Draft& draft, const Effects& effects) const
{
    static constexpr OpcodeSet storesIntoNewList { Opcode::TryPutIndex, Opcode::TryPutByValDirect, Opcode::ElemDefine, Opcode::PutContiguousIndex, Opcode::PutOwnContiguousIndex, Opcode::AppendElement };
    static constexpr OpcodeSet newLists { Opcode::NewArray, Opcode::NewArrayLike, Opcode::SortKeyArray };
    static constexpr OpcodeSet allocationsOfArrayLength { Opcode::NewArrayLike, Opcode::SortKeyArray };
    if (allocationsOfArrayLength.contains(draft.opcode) && m_frames.size() == m_frameOfFusedChain)
        return true;
    if (draft.opcode == Opcode::SortByKeys && m_frames.size() == m_frameOfFusedChain)
        return true;
    if (!storesIntoNewList.contains(draft.opcode) || effects.mayRunOtherCode())
        return false;
    Reference list = draft.operand(0);
    ValueID made = list && list.isValue() ? underlyingCellOf(m_code, list.id()) : ValueID();
    return made && newLists.contains(m_code.at(made).opcode) && made.index() >= m_frames[m_frameOfFusedChain - 1].numberOfValuesBuiltBefore;
}

BlockID Walker::blockWhereFusedChainIsLeft()
{
    Frame& frame = m_frames[m_frameOfFusedChain - 1];
    if (!frame.whereCopyLeavesForCall) {
        frame.whereCopyLeavesForCall = addBlock(BlockFrequency::Rare);
        if (!frame.loopDepthAtCall)
            m_code.setLoopMarks(frame.whereCopyLeavesForCall, false, false);
    }
    return frame.whereCopyLeavesForCall;
}

bool Walker::leaveFusedChain(const Draft& draft, const Effects& effects)
{
    return !fillsListOfFusedChain(draft, effects) && leaveFusedChainUnlessUnseen(effects);
}

bool Walker::leaveFusedChainUnlessUnseen(const Effects& effects)
{
    if (effects.mayBeRunAgainUnseen() && !effects.mayThrowByRule())
        return false;
    jumpTo(blockWhereFusedChainIsLeft());
    leaveBlockPending();
    return true;
}

Reference Walker::requireCoercibleInFusedChain(const Draft& draft)
{
    Reference value = draft.operand(0);
    if (!m_types.overlap(typeOf(value), TypeTable::nullish))
        return value;
    Draft test(Opcode::TypeTest, draft.origin());
    chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(TypeTable::nullish);
    Reference isNullish = build(WTF::move(test));
    if (!isNullish)
        return { };
    BlockID passed = addBlock();
    branch(isNullish, blockWhereFusedChainIsLeft(), passed);
    continueIn(passed);
    if (isUnreachable())
        return { };
    Draft narrowed(Opcode::Narrowed, draft.origin());
    chooseConservativeFields(narrowed);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(isNullish);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    return build(WTF::move(narrowed));
}

Reference Walker::narrowInFusedChain(Reference value, TypeID expected, Origin origin)
{
    TypeID tested = value && expected ? m_types.testablePartOf(m_types.withoutNullish(expected)) : TypeID();
    if (!tested || isNeverType(tested) || m_types.isSubset(ownTypeOf(value), tested))
        return value;
    Draft test(Opcode::TypeTest, origin);
    chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    Reference passes = build(WTF::move(test));
    if (!passes)
        return { };
    BlockID passed = addBlock();
    branch(passes, passed, blockWhereFusedChainIsLeft());
    continueIn(passed);
    if (isUnreachable())
        return { };
    Draft narrowed(Opcode::Narrowed, origin);
    chooseConservativeFields(narrowed);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return build(WTF::move(narrowed));
}

bool Walker::tryLeaveInlinedHeadSlow()
{
    const Frame& frame = m_frames.last();
    if (!frame.inlinedHeadBlocks || !frame.mayLeaveForCall || isUnreachable() || !m_state.isInlineeEffectFree(frame.inlineeRestartPoint) || isUnderHandlerOtherThanCleanups() || hasStringsInBuilders())
        return false;
    jumpTo(blockWhereCopyLeavesForCall());
    leaveBlockPending();
    return true;
}

Walker::Frame& Walker::frameWhoseCopyLeavesForCall()
{
    if (m_frames.last().inlinedHeadBlocks)
        return m_frames.last();
    size_t outermost = m_frames.size() - 1;
    size_t lowest = m_trialVisits.isEmpty() ? 0 : m_trialVisits.last().frameIndex;
    while (outermost > lowest + 1) {
        const Frame& around = m_frames[outermost - 1];
        if (!around.mayLeaveForCall || around.inlinedHeadBlocks || !m_state.isInlineeEffectFree(around.inlineeRestartPoint))
            break;
        --outermost;
    }
    return m_frames[outermost];
}

BlockID Walker::blockWhereCopyLeavesForCall()
{
    Frame& frame = frameWhoseCopyLeavesForCall();
    if (frame.inlinedHeadBlocks) {
        if (!frame.whereHeadEnds)
            frame.whereHeadEnds = addBlock();
        return frame.whereHeadEnds;
    }
    if (!frame.whereCopyLeavesForCall)
        frame.whereCopyLeavesForCall = addBlock(BlockFrequency::Rare);
    return frame.whereCopyLeavesForCall;
}

bool Walker::walkInlineMethodThatProtocolCalls(const BodyValue& step, FunctionID callee, std::span<const Reference> arguments)
{
    SetForScope calledByProtocol(m_nextCopyIsOfMethodThatProtocolCalls, true);
    return walkInline(step, callee, arguments);
}

static LayoutID singleLayoutAtInlining(const Walker& walker, TypeID type)
{
    Type objects = type ? walker.types().at(type) : Type();
    return type && objects.isObject() && objects.hasOnlyLayouts() ? singleLayoutOf(walker.types(), objects) : LayoutID();
}

Reference Walker::receiverOf(const Frame& frame) const
{
    std::optional<unsigned> parameter = program().at(frame.function).thisParameterIndex();
    if (!parameter || !frame.index || *parameter >= frame.index->block(entryBlock).numberOfParameters())
        return { };
    ValueID receiver = frame.index->block(entryBlock).parameter(*parameter);
    return receiver.index() < frame.references.size() ? frame.references[receiver.index()] : Reference();
}

Reference Walker::findEnclosingReceiver(Reference receiver) const
{
    if (!isInLoop() || !isInCallbackCopy() || !receiver || !receiver.isValue())
        return { };
    auto layoutOf = [&](Reference value) {
        return value && value.isValue() ? singleLayoutAtInlining(*this, typeOf(value)) : LayoutID();
    };
    LayoutID layout = layoutOf(receiver);
    ValueID cell = underlyingCellOf(m_code, receiver.id());
    if (!layout || m_code.isConstant(cell) || m_code.at(cell).opcode != Opcode::GetSlot || layoutOf(receiverOf(m_frames.last())) == layout)
        return { };
    for (size_t place = m_frames.size() - 1; place--;) {
        Reference enclosing = receiverOf(m_frames[place]);
        if (layoutOf(enclosing) == layout)
            return underlyingCellOf(m_code, enclosing.id()) == cell ? Reference() : enclosing;
    }
    return { };
}

bool Walker::callsEnclosingReceiver(FunctionID callee, Reference receiver) const
{
    if (!isInLoop() || !facts().linked().function(callee).mayCallBack || !receiver || !receiver.isValue())
        return false;
    return callsEnclosingReceiver(callee, singleLayoutAtInlining(*this, typeOf(receiver)));
}

bool Walker::callsEnclosingReceiver(FunctionID callee, LayoutID layout) const
{
    const BodyIndex* body = isInLoop() && layout && facts().linked().function(callee).mayCallBack ? facts().bodies().find(callee) : nullptr;
    std::optional<unsigned> thisParameter = body ? program().at(callee).thisParameterIndex() : std::nullopt;
    if (!thisParameter)
        return false;
    Vector<LayoutID, 8> layoutsOfEnclosingReceivers;
    for (const Frame& frame : m_frames) {
        Reference enclosing = receiverOf(frame);
        if (LayoutID enclosingLayout = enclosing && enclosing.isValue() ? singleLayoutAtInlining(*this, typeOf(enclosing)) : LayoutID())
            layoutsOfEnclosingReceivers.append(enclosingLayout);
    }
    if (layoutsOfEnclosingReceivers.isEmpty())
        return false;
    ValueID own = body->block(entryBlock).parameter(*thisParameter);
    auto isOwnReceiver = [&](ValueID value) {
        value = body->stripCasts(value);
        while (!body->isParameter(value) && body->opcodeOf(value) == Opcode::ToThis)
            value = body->stripCasts(body->value(value).operand(0));
        return value == own;
    };
    auto isOfEnclosingReceiver = [&](PropertyKeyID key) {
        std::optional<FieldReference> field = findField(program(), layout, key);
        LayoutID called = field ? singleLayoutAtInlining(*this, facts().linked().field(*field).proven) : LayoutID();
        return called && layoutsOfEnclosingReceivers.contains(called);
    };
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            if (operation.opcode() != Opcode::Call)
                continue;
            ValueID called = operation.as<Opcode::Call>().thisValueOperand();
            called = called ? body->stripCasts(called) : ValueID();
            if (!called || body->isParameter(called) || body->opcodeOf(called) != Opcode::PropGet || body->value(called).isFreeName())
                continue;
            auto read = body->value(called).as<Opcode::PropGet>();
            if (isOwnReceiver(read.objectOperand()) && isOfEnclosingReceiver(read.key()))
                return true;
        }
    }
    return false;
}

bool directEntryTakesArguments(Walker&, FunctionID callee, std::span<const Reference> entryArguments);

bool Walker::walkInline(const BodyValue& call, FunctionID callee, std::span<const Reference> arguments, const Draft* writtenOut, const BitVector* inlinedHeadBlocks)
{
    bool isCalledByProtocol = std::exchange(m_nextCopyIsOfMethodThatProtocolCalls, false);
    BlockID* armOfFusedChain = std::exchange(m_armOfNextFusedChain, nullptr);
    ValueID keyOfCopy = std::exchange(m_keyOfNextCopy, ValueID());
    if (!m_opensGuesses || m_frames.last().inlinedHeadBlocks || (inlinedHeadBlocks && (isUnderHandlerOtherThanCleanups() || hasStringsInBuilders())))
        return false;

    std::optional<Draft> drafted;
    if (!writtenOut)
        drafted.emplace(draftOf(call));
    const Draft& site = writtenOut ? *writtenOut : *drafted;
    if (isBeingWalked(callee) && !(inlinedHeadBlocks ? bodyOfUnitCallsItselfAtInlining(*this, site, callee) : bodyOfUnitTakesCopyOfItselfAtInlining(*this, site, callee)) && !constructsItselfOnce(*this, site, callee))
        return false;
    Vector<ClosureMadeForCallAtInlining, 2> closuresMadeForCall;
    const Function& function = program().at(callee);
    bool isConstruction = constructionsOfInlining.contains(site.opcode);
    CallOfInlining written;
    written.callee = callee;
    if (isConstruction || site.opcode == Opcode::Call) {
        written.closure = closureCalledAtInlining(site);
        written.arguments = site.argumentsOperands();
    }
    if (!mayBeWalkedAtInlining(*this, callee, entersBodyOfCoroutineAtInlining(site)) || !operandsFitAtInlining(*this, written, function, arguments))
        return false;
    if (walksOwnBodyOfUnit())
        m_operationsCopiedAtWrittenCall = 0;
    m_operationsCopiedAtWrittenCall += facts().summaries().at(callee).numberOfOperations;

    Origin originOfCall = site.origin();
    originOfCall.isTailCall = false;
    m_environmentCellsOfUnit.mayNameSome |= isHandedEnvironmentWithByte(*this, callee);
    Vector<Reference, 8> entryArguments = buildEntryArgumentsAtInlining(*this, originOfCall, written, function, arguments);
    if (isUnreachable())
        return true;

    std::optional<unsigned> thisParameter = isInLoop() && mayBeCallback(*this, callee) ? function.thisParameterIndex() : std::nullopt;
    bool callsBack = !inlinedHeadBlocks && thisParameter && *thisParameter < entryArguments.size() && callsEnclosingReceiver(callee, entryArguments[*thisParameter]);
    if (callsBack)
        m_state.forbidRestart();
    callsBack |= isInCallbackCopy();

    if (isInLoop() && !canRestartIteration() && !inlinedHeadBlocks && !site.isTailCall && !site.resultIsReturnedAtOnce && guardsAskForUndefinedAlone(*this, callee, entryArguments.span()))
        m_walksCopyWhereItsGuardFailed = true;
    auto endWalkWhereGuardFailed = makeScopeExit([&] {
        m_walksCopyWhereItsGuardFailed = false;
    });
    Vector<Reference, 8> argumentsAsTheyCame;
    BlockID whereGuardFails;
    bool failingSideIsWalkedInLine = false;
    bool isWalkedWhereItsGuardFailed = std::exchange(m_walksCopyWhereItsGuardFailed, false);
    m_walksCopyWhereItsGuardFailed = isWalkedWhereItsGuardFailed;
    const CallingConventions& conventions = plan().conventions();
    bool isConstructionOfCopy = isConstruction || (call.opcode() == Opcode::CallDirect && site.isConstruction());
    bool isDerivedConstructorWithoutList = function.kind == FunctionKind::DerivedConstructor && !function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    bool copyMayLeaveForCall = (!isConstructionOfCopy || conventions.hasParameterGuard(callee) || isDerivedConstructorWithoutList) && !program().at(unit().function).isCoroutine() && !function.isCoroutine() && !entersBodyOfCoroutineAtInlining(site)
        && facts().linked().mayBeMentionedBy(unit(), callee) && unitTakesWhatCopyIsHanded(*this, callee, entryArguments.span())
        && (!(site.isTailCall || site.resultIsReturnedAtOnce) || unitMayLeaveByDirectCallOf(*this, callee));
    bool guessesInCopyKeepTheirFailingWays = isCalledByProtocol && isInLoop() && !inlinedHeadBlocks && facts().summaries().at(callee).makesObjects;
    if (inlinedHeadBlocks && !copyMayLeaveForCall) {
        m_operationsCopiedAtWrittenCall -= facts().summaries().at(callee).numberOfOperations;
        return false;
    }
    size_t numberOfExpectationsOfCopiesAround = m_expectedOfArgumentsOfCopies.size();
    auto forgetWhatIsExpectedOfArguments = makeScopeExit([&] {
        m_expectedOfArgumentsOfCopies.shrink(numberOfExpectationsOfCopiesAround);
    });
    if (uint64_t onlyRead = copyMayLeaveForCall && !inlinedHeadBlocks && has(Mechanism::Guesses) ? entryParametersOnlyReadByTwoNamesOrMore(facts().bodies().at(callee)) : 0) [[unlikely]] {
        call.forEachOperand([&](ValueID operand, uint32_t) {
            Reference handed = referenceOf(operand);
            if (!handed || !handed.isValue())
                return;
            for (uint32_t index = 0; index < entryArguments.size() && index < 64; ++index) {
                if (!(onlyRead >> index & 1) || entryArguments[index] != handed || function.entryParameterKind(index) != EntryParameter::Parameter || !isEntryParameterUninformative(*this, callee, index))
                    continue;
                if (ExpectationOfValue expectation = findExpectation(*this, operand); expectation.type && !m_types.isUninformative(expectation.type)) {
                    onlyRead &= ~(uint64_t { 1 } << index);
                    m_expectedOfArgumentsOfCopies.append({ static_cast<uint32_t>(m_frames.size()), index, expectation.type, expectation.isDeclared });
                    count(StatisticID::ArgumentSpeculationsPassedToInlinees);
                }
            }
        });
    }
    Reference beginningInEffectsNet;
    bool canCallInstead = copyMayLeaveForCall && !isConstructionOfCopy && !armOfFusedChain && !keyOfCopy && facts().bodies().at(callee).opcodes().contains(Opcode::ElemGet) && callsItself(*this, facts().bodies().at(callee), callee) && directEntryTakesArguments(*this, callee, entryArguments.span());
    if (copyMayLeaveForCall) {
        argumentsAsTheyCame = entryArguments;
        closuresMadeForCall = collectClosuresMadeForCallAtInlining(*this, call);
        if (m_inputs.options.checksEffects) [[unlikely]]
            beginningInEffectsNet = build(Draft(Opcode::BeginEffectsCheck, originOfCall));
    }
    auto holdThatNothingWasDoneInCopy = [&] {
        if (!beginningInEffectsNet || !beginningInEffectsNet.isValue() || isUnreachable())
            return;
        Draft end(Opcode::CheckInlineeFallback, originOfCall);
        end.setBeginningOperand(beginningInEffectsNet);
        build(WTF::move(end));
    };
    for (uint32_t index = 0; index < entryArguments.size() && !isUnreachable(); ++index) {
        TypeID tested = m_walksCopyWhereItsGuardFailed ? TypeID() : conventions.typeBehindGuard(callee, index);
        if (!tested || m_types.isSubset(ownTypeOf(entryArguments[index]), tested) || !m_types.overlap(typeOf(entryArguments[index]), tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        if (m_inputs.options.takesNotes && passes.isValue())
            m_code.noteTestOfGuardInFrontOfCopy({ passes.id(), callee, function.position });
        IterationPurity hoistingPurity = isIterationEffectFree() || !testOfTypeLooksAtNoShape(tested) ? IterationPurity::EffectFree : IterationPurity::KnownEffectsOnlyWithInlinees;
        bool isAskedInFront = tryBranchInPreheader(passes, true, hoistingPurity);
        if (isAskedInFront && hoistingPurity != IterationPurity::EffectFree)
            count(StatisticID::InlineeGuardsHoistedPastStores);
        if (isAskedInFront)
            recordSpeculation();
        else {
            BlockID whereTestPasses = addBlock();
            auto holdsEveryOperationInEntryBlock = [&] {
                const BodyIndex& body = facts().bodies().at(callee);
                for (uint32_t number = 1; number < body.numberOfBlocks(); ++number) {
                    if (body.block(BlockID(number)).numberOfValues())
                        return false;
                }
                return true;
            };
            auto mayTakeFraction = [&] {
                return m_types.at(tested).isInt32() && (m_types.at(typeOf(entryArguments[index])).primitives & Primitive::FractionalNumber) && onlyCalculatesInWindow(*this, callee, facts().bodies().at(callee));
            };
            bool mayWalkFailingSideInLine = !armOfFusedChain && !keyOfCopy && isInLoop() && !inlinedHeadBlocks && !site.isTailCall && !site.resultIsReturnedAtOnce && ((!canRestartIteration() && holdsEveryOperationInEntryBlock()) || mayTakeFraction());
            bool leavesCopyAround = !mayWalkFailingSideInLine && !whereGuardFails && mayLeaveCopyForCall();
            if (leavesCopyAround)
                recordSpeculation();
            else if (!whereGuardFails) {
                failingSideIsWalkedInLine = mayWalkFailingSideInLine;
                whereGuardFails = addBlock(failingSideIsWalkedInLine ? BlockFrequency::Normal : BlockFrequency::Rare);
                recordSpeculation();
                argumentsAsTheyCame = entryArguments;
                closuresMadeForCall = collectClosuresMadeForCallAtInlining(*this, call);
            }
            branch(passes, whereTestPasses, leavesCopyAround ? blockWhereCopyLeavesForCall() : whereGuardFails);
            continueIn(whereTestPasses);
        }

        SpecializedPreheaderScope preheaderScope(*this, isAskedInFront, hoistingPurity);
        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    Reference enclosing = thisParameter && *thisParameter < entryArguments.size() && copyMayLeaveForCall && !inlinedHeadBlocks && !isUnreachable() && has(Mechanism::Guesses) ? findEnclosingReceiver(entryArguments[*thisParameter]) : Reference();
    if (enclosing) {
        Draft test(Opcode::StrictEq, originOfCall);
        chooseConservativeFields(test);
        test.setLeftOperand(entryArguments[*thisParameter]);
        test.setRightOperand(enclosing);
        Reference isEnclosing = build(WTF::move(test));
        if (!isEnclosing)
            return true;
        if (!whereGuardFails) {
            whereGuardFails = addBlock(BlockFrequency::Rare);
            argumentsAsTheyCame = entryArguments;
            closuresMadeForCall = collectClosuresMadeForCallAtInlining(*this, call);
        }
        recordSpeculation();
        BlockID passed = addBlock();
        branch(isEnclosing, passed, whereGuardFails);
        continueIn(passed);
        entryArguments[*thisParameter] = enclosing;
    }
    uint64_t readAfterWrites = canRestartIteration() && !inlinedHeadBlocks && !m_walksCopyWhereItsGuardFailed && has(Mechanism::Guesses) && has(Mechanism::TypedOperations) ? numbersReadAfterWrites(facts().bodies().at(callee)) : 0;
    for (uint32_t index = 0; index < entryArguments.size() && index < 64 && readAfterWrites >> index && !isUnreachable(); ++index) {
        if (!(readAfterWrites >> index & 1) || !entryArguments[index].isValue() || function.entryParameterKind(index) != EntryParameter::Parameter)
            continue;
        TypeID arriving = ownTypeOf(entryArguments[index]);
        if (!m_types.at(arriving).isJavaScriptValue() || m_types.at(arriving).isNumber() || !m_types.overlap(arriving, TypeTable::number))
            continue;
        const DeclaredType* declared = function.declaredEntryParameter(index);
        TypeID expected = declared && !declared->isEstablished() ? declared->written() : TypeID();
        call.forEachOperand([&](ValueID operand, uint32_t) {
            if ((!expected || m_types.isUninformative(expected)) && referenceOf(operand) == entryArguments[index])
                expected = findExpectation(*this, operand).type;
        });
        if (!expected || !m_types.at(expected).isNumber())
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(TypeTable::number);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        recordSpeculation();
        count(StatisticID::InlineeNumberGuards);
        bool isAskedInFront = tryBranchInPreheader(passes, true, IterationPurity::EffectFree);
        if (!isAskedInFront) {
            BlockID restarts = addBlock(BlockFrequency::Rare);
            BlockID passed = addBlock();
            branch(passes, passed, restarts);
            continueIn(restarts);
            if (!isUnreachable())
                restartIteration();
            continueIn(passed);
            if (m_hasFailed)
                return true;
        }
        SpecializedPreheaderScope preheaderScope(*this, isAskedInFront, IterationPurity::EffectFree);
        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(entryArguments[index]);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
    }
    bool secondWalkIsRareAndKeepsSites = false;
    if (!whereGuardFails && !keyOfCopy && copyMayLeaveForCall && !m_walksCopyWhereItsGuardFailed && isInLoop() && !canRestartIteration() && !mayLeaveCopyForCall() && !inlinedHeadBlocks && !site.isTailCall && !site.resultIsReturnedAtOnce
        && facts().bodies().at(callee).numberOfBlocks() == 1 && !isUnreachable()) {
        for (uint32_t index = 0; index < entryArguments.size() && index < function.numberOfEntryParameters() && !failingSideIsWalkedInLine; ++index) {
            const DeclaredType* declared = function.declaredEntryParameter(index);
            if (!declared || !declared->isExpected() || !declared->written() || function.thisParameterIndex() == std::optional<unsigned>(index))
                continue;
            Type written = m_types.at(declared->written());
            failingSideIsWalkedInLine = written.hasObjects() && (written.primitives & Primitive::Number) && (written.primitives & Primitive::AnyString) && (written.primitives & Primitive::BigInt) && m_types.isUninformative(typeOf(entryArguments[index]));
        }
        if (failingSideIsWalkedInLine) {
            whereGuardFails = addBlock(BlockFrequency::Rare);
            secondWalkIsRareAndKeepsSites = true;
        }
    }
    if (whereGuardFails)
        letGuessFailUnasked(whereGuardFails, originOfCall);
    const LinkTables& link = facts().linked();
    const LinkedFunction& linked = link.function(callee);
    for (uint32_t index = 0; index < entryArguments.size() && checksProofsOf(ProofSource::EntryParameter) && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        if (TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID())
            checkProofOfCopy(entryArguments[index], proven, ProofSource::EntryParameter, originOfCall);
    }
    for (uint32_t index = 0; index < entryArguments.size() && has(Mechanism::Arrivals) && linked.everyCallerIsKnown && !isUnreachable(); ++index) {
        TypeID proven = index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID();
        bool isOneLook = proven && (!m_types.at(proven).hasObjects() || singleFunctionOf(m_types, m_types.at(proven)) || (m_types.at(proven).hasOnlyLayouts() && holdsOneMember(m_types.at(proven).layouts)));
        if (proven && !isOneLook)
            continue;
        TypeID tested = proven && entryArguments[index].isValue() ? m_types.testablePartOf(proven) : TypeID();
        bool leavesChainAtNullish = tested && isElementReadByName(entryArguments[index], facts().bodies().at(callee), index);
        if (TypeID present = leavesChainAtNullish ? m_types.withoutNullish(tested) : TypeID(); present && !isNeverType(present))
            tested = present;
        else
            leavesChainAtNullish = false;
        TypeID arriving = tested ? ownTypeOf(entryArguments[index]) : TypeID();
        bool isRawNumberAmongFractions = arriving && m_types.at(arriving).isNumber() && representationOf(entryArguments[index]) == Representation::Double && (m_types.at(tested).primitives & Primitive::FractionalNumber);
        if (!arriving || isRawNumberAmongFractions || !m_types.at(arriving).isJavaScriptValue() || m_types.at(tested).isAnything() || m_types.isSubset(arriving, tested) || !m_types.overlap(arriving, tested))
            continue;
        Draft test(Opcode::TypeTest, originOfCall);
        test.setValueOperand(entryArguments[index]);
        test.setTestedType(tested);
        Reference passes = build(WTF::move(test));
        if (!passes)
            return true;
        BlockID whereClaimHolds = addBlock();
        BlockID whereClaimFails = addBlock(BlockFrequency::Rare);
        branch(passes, whereClaimHolds, whereClaimFails);
        continueIn(whereClaimFails);
        if (leavesChainAtNullish) {
            jumpTo(blockWhereFusedChainIsLeft());
            leaveBlockPending();
        } else {
            buildCheckOfProof(entryArguments[index], TypeTable::never, TypeTable::anything, ProofSource::EntryParameter, originOfCall);
            endAsUnreachable(originOfCall);
        }
        continueIn(whereClaimHolds);

        Reference asItCame = entryArguments[index];
        Draft narrowed(Opcode::Narrowed, originOfCall);
        narrowed.setValueOperand(asItCame);
        narrowed.setTestOperand(passes);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        entryArguments[index] = build(WTF::move(narrowed));
        if (entryArguments[index] && !m_state.checkedNameOf(asItCame))
            m_state.noteCheckedName(asItCame, entryArguments[index]);
    }
    if (m_hasFailed)
        return true;
    bool copyIsReached = !isUnreachable();
    bool returnsInPlaceOfUnit = unitReturnsAtOnce(site);

    const BodyIndex& body = facts().bodies().at(callee);
    const BodyIndex& bodyOfCaller = currentBodyIndex();

    uint32_t loopDepthAtCall = m_frames.last().loopDepthAtCall + loopDepthInCodeOf(bodyOfCaller.blockOf(call.id()));
    bool callsAreCountedByPlan = callsOfCopyAreCountedForStackTests(call, callee);
    ExpectationOfValue ofAnswer = expectationOfAnswerOfCallBeingCopied(*this, call);
    UseOfTextAsKey useOfAnswerAsKey = isConstruction ? UseOfTextAsKey::NotKnown : useOfAnswerOfCallBeingCopiedAsKey(*this, call, body);
    uint32_t numberOfValuesBuiltBefore = m_code.numberOfValues();
    bool copyIsMadeByLinkOfChain = isCopiedByLinkOfChain(call);
    uint32_t inlineFrame = m_code.addInlineFrame({
        .callee = callee,
        .callPosition = site.position,
        .parent = m_frames.last().inlineFrame,
        .isTailCall = site.isTailCall,
        .isConstruction = isConstruction,
        .callIsPartOfConstruction = site.isPartOfConstruction,
        .module = function.module,
        .calledBuiltin = !isConstruction && function.isHiddenInStackTraces() ? site.calledBuiltin() : BuiltinID(),
    });

    Frame& frame = m_frames.appendNew();
    frame.function = callee;
    frame.body = &body.body();
    frame.index = &body;
    if (function.isModuleBody() || function.syntax.contains(FunctionSyntax::IsWrappedModuleBody))
        frame.variablesStoredFirst = firstStores().variablesStoredFirstIn(callee);
    frame.references.lay(body.numberOfValues());
    frame.loopDepthAtCall = loopDepthAtCall;
    frame.isInCallbackCopy = callsBack;
    frame.returnsInPlaceOfUnit = returnsInPlaceOfUnit;
    frame.callsAreCountedByPlan = callsAreCountedByPlan;
    if (ofAnswer.type) {
        frame.typeExpectedOfAnswer = ofAnswer.type;
        frame.answerIsDeclared = ofAnswer.isDeclared;
    }
    frame.callCopied = call.id();
    frame.useOfAnswerAsKey = useOfAnswerAsKey;
    frame.numberOfValuesBuiltBefore = numberOfValuesBuiltBefore;
    frame.isCopiedByLinkOfChain = copyIsMadeByLinkOfChain;
    frame.inlineFrame = inlineFrame;
    frame.keyOfComparator = keyOfCopy;
    frame.whereCopyLeavesForCall = whereGuardFails;
    frame.isWalkedWhereItsGuardFailed = std::exchange(m_walksCopyWhereItsGuardFailed, false);
    frame.mayLeaveForCall = copyMayLeaveForCall && copyIsReached && !guessesInCopyKeepTheirFailingWays;
    frame.canCallInstead = canCallInstead;
    frame.inlinedHeadBlocks = inlinedHeadBlocks;
    frame.inlineeRestartPoint = ++m_restartPointCount;
    uint32_t inlineeRestartPoint = frame.inlineeRestartPoint;
    uint32_t restartMarkBeforeInlinee = m_state.restartMark();
    if (frame.mayLeaveForCall)
        m_state.setInlineeRestartPoint(inlineeRestartPoint);
    bool didDisplaceRestartMark = restartMarkBeforeInlinee && m_state.restartMark() != restartMarkBeforeInlinee;
    m_frames.last().displacedRestartMark = didDisplaceRestartMark ? restartMarkBeforeInlinee : 0;

    using ReturnOfCopy = Frame::ReturnOfCopy;
    Vector<ReturnOfCopy, 2> returns;
    m_frames.last().returnsOfCopy = &returns;
    uint32_t frameOfFusedChainAround = m_frameOfFusedChain;
    if (armOfFusedChain)
        m_frameOfFusedChain = static_cast<uint32_t>(m_frames.size());

    BodyBlock entry = body.block(entryBlock);
    for (uint32_t index = 0; index < entryArguments.size() && copyIsReached; ++index)
        bind(entry.parameter(index), entryArguments[index]);
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFailed && copyIsReached; ++number)
        walkBlock(BlockID(number));
    m_expectedOfArgumentsOfCopies.shrink(numberOfExpectationsOfCopiesAround);
    m_block = { };
    m_state.clear();
    if (didDisplaceRestartMark) {
        for (ReturnOfCopy& returned : returns)
            returned.state.restoreDisplacedRestartMark(inlineeRestartPoint, restartMarkBeforeInlinee);
    }
    bool wasCopiedByLinkOfChain = m_frames.last().isCopiedByLinkOfChain;
    whereGuardFails = m_frames.last().whereCopyLeavesForCall;
    BlockID whereHeadEnds = m_frames.last().whereHeadEnds;
    Reference generatorMadeAgain = m_frames.last().generatorMadeAgain;

    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    m_frameOfFusedChain = frameOfFusedChainAround;
    if (armOfFusedChain)
        *armOfFusedChain = std::exchange(whereGuardFails, BlockID());
    for (ReturnOfCopy& returned : returns) {
        if (!returned.value)
            returned.value = constant(Draft(Opcode::Undefined, originOfCall));
    }
    m_origin = originOfCall;
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;

    auto callWithClosuresMadeAgain = [&](bool entersAsCallWould) {
        Vector<Reference, 8> handedToCall = argumentsAsTheyCame;
        for (Reference& argument : handedToCall) {
            for (const ClosureMadeForCallAtInlining& closure : closuresMadeForCall) {
                if (argument != closure.made || isUnreachable())
                    continue;
                Draft again = draftOf(currentBodyIndex().value(closure.written));
                chooseConservativeFields(again);
                if (Reference madeHere = build(WTF::move(again)))
                    argument = madeHere;
            }
            if (generatorMadeAgain && argument == generatorMadeAgain && !isUnreachable()) {
                if (Reference madeHere = makeGeneratorAgain(argument))
                    argument = madeHere;
            }
        }
        size_t placeOfRestList = function.firstParameterIndex() + function.numberOfPositionalParameters();
        bool listWasMadeForCopy = placeOfRestList < handedToCall.size() && (placeOfRestList >= arguments.size() || !arguments[placeOfRestList]) && restListMayFallAwayInCopy(*this, callee);
        if (Reference list = listWasMadeForCopy && !isUnreachable() ? handedToCall[placeOfRestList] : Reference(); list && list.isValue() && m_code.at(list.id()).opcode == Opcode::NewArray) {
            Vector<Reference, 8> listed;
            for (ValueID element : m_code.at(list.id()).variadicOperands())
                listed.append(m_code.isConstant(element) ? Reference::constant(element) : Reference::value(element));
            Draft again(Opcode::NewArray, originOfCall);
            chooseConservativeFields(again);
            again.standsForNoValueOfInput = true;
            again.setContents(TypeTable::anything);
            again.setVariadicOperands(listed.span());
            if (Reference madeHere = build(WTF::move(again))) {
                handedToCall[placeOfRestList] = madeHere;
                count(StatisticID::RestArrayAllocationsSunk);
            }
        }
        return callInPlaceOfCopy(*this, site, callee, handedToCall.span(), isConstructionOfCopy, entersAsCallWould);
    };
    if (whereHeadEnds && !m_hasFailed) {
        m_block = { };
        m_state.clear();
        continueIn(whereHeadEnds);
        Reference answer;
        {
            ClosedArm closedArm(*this);
            holdThatNothingWasDoneInCopy();
            answer = callWithClosuresMadeAgain(true);
        }
        if (answer && returnsInPlaceOfUnit)
            returnAtOnce(answer);
        else if (answer)
            returns.append(ReturnOfCopy { m_block, answer, WTF::move(m_state), false });
        m_block = { };
        m_state.clear();
    }
    size_t numberOfReturnsOfCopy = returns.size();

    if (keyOfCopy && whereGuardFails && !m_hasFailed) {
        m_block = { };
        m_state.clear();
        continueIn(std::exchange(whereGuardFails, BlockID()));
        if (!isUnreachable()) {
            jumpTo(blockWhereFusedChainIsLeft());
            leaveBlockPending();
        }
        m_block = { };
        m_state.clear();
    }
    if (whereGuardFails && !m_hasFailed) {
        m_block = { };
        m_state.clear();
        continueIn(whereGuardFails);
        bool wasWalkedInLine = false;
        if (failingSideIsWalkedInLine && closuresMadeForCall.isEmpty() && !returnsInPlaceOfUnit && !isUnreachable()) {
            uint32_t firstBlockOfSecondWalk = m_code.numberOfBlocks();
            if (secondWalkIsRareAndKeepsSites) {
                m_code.markRareBlockThatKeepsSites(whereGuardFails);
                holdThatNothingWasDoneInCopy();
            }
            m_walksCopyWhereItsGuardFailed = true;
            {
                std::optional<HeardAsArm> isArmOfDispatch;
                if (standsInArmOfRowOfShapeTests())
                    isArmOfDispatch.emplace(*this);
                wasWalkedInLine = walkInline(call, callee, arguments, writtenOut);
            }
            m_walksCopyWhereItsGuardFailed = false;
            for (uint32_t number = firstBlockOfSecondWalk; secondWalkIsRareAndKeepsSites && number < m_code.numberOfBlocks(); ++number)
                m_code.markRareBlockThatKeepsSites(BlockID(number));
            if (wasWalkedInLine)
                count(StatisticID::CallsInlinedAfterFailedGuards);
            Reference answer = wasWalkedInLine && !m_hasFailed && !isUnreachable() ? referenceOf(call.id()) : Reference();
            if (answer)
                returns.append(ReturnOfCopy { m_block, answer, WTF::move(m_state), false });
        }
        if (!wasWalkedInLine && !m_hasFailed) {
            ClosedArm closedArm(*this, true);
            holdThatNothingWasDoneInCopy();
            Reference answer = callWithClosuresMadeAgain(false);
            if (answer && returnsInPlaceOfUnit)
                returnAtOnce(answer);
            else if (answer)
                returns.append(ReturnOfCopy { m_block, answer, WTF::move(m_state), false });
        }
        m_block = { };
        m_state.clear();
    }

    for (size_t index = numberOfReturnsOfCopy; index < returns.size(); ++index)
        returns[index].isLedByClosedArm = true;

    if (m_hasFailed || returns.isEmpty()) {
        m_block = { };
        m_state.clear();
        return true;
    }

    Vector<ReturnOfCopy*, 4> passing;
    Vector<ReturnOfCopy*, 4> closed;
    for (ReturnOfCopy& returned : returns)
        (returned.isLedByClosedArm ? closed : passing).append(&returned);
    auto walkEndOfBlockBehind = [&](std::span<ReturnOfCopy* const> ways) {
        Vector<ReturnOfCopy, 2> returnsOfOneKind;
        for (ReturnOfCopy* returned : ways)
            returnsOfOneKind.append(WTF::move(*returned));
        goOnBehindReturnsOfCopy(call, function, linked, originOfCall, returnsOfOneKind, numberOfValuesBuiltBefore, !!keyOfCopy);
        walkEndOfBlockBehindReturnsOfCopy(call.id());
    };
    auto endWhereWaysWentApart = [&] {
        m_frames.last().references.placeOf<NamesOfValues::Writer::ForgetsAnswerOfCopy>(call.id().index()) = Reference();
        m_block = { };
        m_state.clear();
        m_callIsAccountedFor = true;
        m_calleeAccountedFor = callee;
        return true;
    };
    if (isConstructionOfCopy && function.takesThisFromConstruction())
        return goOnBehindReturnsOfCopy(call, function, linked, originOfCall, returns, numberOfValuesBuiltBefore);
    if (std::optional<uint32_t> loopRestartPoint = returns.size() > 1 ? turnWhoseQuietWaysGoToHeadAround(m_frames.last().block) : std::nullopt) {
        Vector<ReturnOfCopy*, 4> quiet;
        Vector<ReturnOfCopy*, 4> others;
        for (ReturnOfCopy& returned : returns)
            (returned.state.hasOnlyKnownEffectsSince(*loopRestartPoint) ? quiet : others).append(&returned);
        bool someLoudWayIsNoClosedArm = others.containsIf([](const ReturnOfCopy* returned) {
            return !returned->isLedByClosedArm;
        });
        if (!quiet.isEmpty() && someLoudWayIsNoClosedArm && letQuietReturnsOfCopyWalkEndOfBlockApart(call.id())) {
            walkEndOfBlockBehind(others.span());
            if (!m_hasFailed)
                walkEndOfBlockBehind(quiet.span());
            return endWhereWaysWentApart();
        }
    }
    if (returns.size() > 1 && closed.isEmpty()) {
        Vector<ReturnOfCopy*, 4> sayingYes;
        Vector<ReturnOfCopy*, 4> sayingNo;
        for (ReturnOfCopy& returned : returns) {
            TypeID own = ownTypeOf(returned.value);
            if (own == TypeArena::fromPrimitives(Primitive::True) && !returned.isUnderHandlerOfCallee)
                sayingYes.append(&returned);
            else if (own == TypeArena::fromPrimitives(Primitive::False) && !returned.isUnderHandlerOfCallee)
                sayingNo.append(&returned);
        }
        if (!sayingYes.isEmpty() && !sayingNo.isEmpty() && sayingYes.size() + sayingNo.size() == returns.size() && letAnswersOfCopyWalkEndOfBlockApart(call.id())) {
            walkEndOfBlockBehind(sayingNo.span());
            if (!m_hasFailed)
                walkEndOfBlockBehind(sayingYes.span());
            return endWhereWaysWentApart();
        }
    }
    if (returns.size() > 1 && closed.isEmpty() && isInLoop() && has(Mechanism::EscapeAnalysis)) {
        bool hasMixedReturns = false;
        Vector<Vector<ReturnOfCopy*, 2>, 4> shapes = groupReturnsByShape(returns, numberOfValuesBuiltBefore, hasMixedReturns);
        if (shapes.size() > 1 && letShapesOfCopyWalkEndOfBlockApart(call.id(), shapes.size())) {
            ++m_duplicatedJoinCount;
            if (hasMixedReturns)
                count(StatisticID::MixedReturnsWalkedApart);
            noteWaysApart(numberOfValuesBuiltBefore);
            for (const auto& ways : shapes) {
                if (!m_hasFailed)
                    walkEndOfBlockBehind(ways.span());
            }
            return endWhereWaysWentApart();
        }
    }
    if (wasCopiedByLinkOfChain && !passing.isEmpty() && !closed.isEmpty() && letReturnsOfCopyWalkEndOfBlockApart(call.id(), passing.span(), closed.span())) {
        {
            ClosedArm closedArm(*this);
            ++m_coldCopyDepth;
            walkEndOfBlockBehind(closed.span());
            --m_coldCopyDepth;
        }
        if (!m_hasFailed)
            walkEndOfBlockBehind(passing.span());
        return endWhereWaysWentApart();
    }
    return goOnBehindReturnsOfCopy(call, function, linked, originOfCall, returns, numberOfValuesBuiltBefore, !!keyOfCopy);
}

bool Walker::goOnBehindReturnsOfCopy(const BodyValue& call, const Function& function, const LinkedFunction& linked, Origin originOfCall, Vector<Frame::ReturnOfCopy, 2>& returns, uint32_t numberOfValuesBuiltBefore, bool returnsKey)
{
    using ReturnOfCopy = Frame::ReturnOfCopy;

    if (returns.size() == 1 && !returns[0].isUnderHandlerOfCallee) {
        m_block = returns[0].block;
        m_state = WTF::move(returns[0].state);
        if (checksProofsOf(ProofSource::ResultOfCall) && !function.isCoroutine() && !returnsKey)
            checkProofOfResultOfCopy(returns[0].value, linked.result, originOfCall, holdsNaNForUndefined(returns[0].value));
        bind(call.id(), guardNumericCallResult(*this, call, returns[0].value, originOfCall));
        return true;
    }

    TypeID type;
    Vector<Representation, 2> forms;
    for (const ReturnOfCopy& returned : returns) {
        TypeID arriving = m_code.at(returned.value.id()).type;
        type = type ? joinTypes(type, arriving) : arriving;
        forms.append(representationOf(returned.value));
    }
    std::optional<ObjectsThatMeet> objects;
    if (has(Mechanism::EscapeAnalysis)) {
        Vector<const WalkState*, 4> ways;
        Vector<Reference, 4> values;
        for (const ReturnOfCopy& returned : returns) {
            ways.append(&returned.state);
            values.append(returned.value);
        }
        objects = findObjectsThatMeetAsOne(*this, ways.span(), values.span(), numberOfValuesBuiltBefore);
    }
    BlockID continuationBlock = addBlock();
    auto isNullish = [&](const ReturnOfCopy& returned) {
        return returned.value.isConstant() && m_types.at(ownTypeOf(returned.value)).isUndefinedOrNull();
    };
    auto isOptionalInteger = [&](const ReturnOfCopy& returned) {
        return representationOf(returned.value) == Representation::OptionalInt64;
    };
    bool nullMeetsIntegers = has(Mechanism::UnboxedNumbers) && std::ranges::any_of(returns, isOptionalInteger) && std::ranges::all_of(returns, [&](const ReturnOfCopy& returned) {
        return isOptionalInteger(returned) || isNullish(returned);
    });
    Reference result = addParameter(continuationBlock, type, nullMeetsIntegers && isLegalRepresentation(m_types.at(type), Representation::OptionalInt64) ? Representation::OptionalInt64 : formOfParameter(type, forms.span()));
    Vector<Reference, 8> parametersOfSlots;
    if (objects) {
        for (const ObjectsThatMeet::Slot& slot : objects->slots) {
            if (slot.valueOnEveryWay)
                continue;
            TypeID typeOfSlot;
            forms.shrink(0);
            for (Reference value : slot.valuesOfWays) {
                typeOfSlot = typeOfSlot ? joinTypes(typeOfSlot, ownTypeOf(value)) : ownTypeOf(value);
                forms.append(representationOf(value));
            }
            parametersOfSlots.append(addParameter(continuationBlock, typeOfSlot, formOfParameter(typeOfSlot, forms.span())));
        }
    }
    Vector<FieldOfScope, 4> fieldsThatMeet;
    if (unitHasMadeScopes() || !m_fieldsOfScopesOfHeads.isEmpty() || !m_carriedResultRequests.isEmpty() || mayMeetUnmadeRecords(returns[0].state)) {
        Vector<const WalkState*, 4> states;
        for (const ReturnOfCopy& returned : returns)
            states.append(&returned.state);
        findFieldsOfScopesThatMeet(states.span(), fieldsThatMeet);
        for (FieldOfScope& field : fieldsThatMeet)
            field.parameter = addParameter(continuationBlock, field.type, field.form);
    }
    Vector<Reference, 8> arguments;
    for (size_t index = 0; index < returns.size() && !m_hasFailed; ++index) {
        m_block = returns[index].block;
        m_state = WTF::move(returns[index].state);
        arguments.shrink(0);
        arguments.append(returns[index].value);
        if (objects) {
            for (const ObjectsThatMeet::Slot& slot : objects->slots) {
                if (!slot.valueOnEveryWay)
                    arguments.append(slot.valuesOfWays[index]);
            }
        }
        for (const FieldOfScope& field : fieldsThatMeet)
            arguments.append(field.values[index]);
        jumpTo(continuationBlock, arguments.span());
        if (index + 1 < returns.size())
            leaveBlockPending();
    }
    if (objects && !m_hasFailed && !isUnreachable())
        noteObjectsThatHaveMetAsOne(*this, result, *objects, parametersOfSlots.span());
    if (!m_hasFailed)
        noteFieldsOfScopes(fieldsThatMeet.span());
    if (checksProofsOf(ProofSource::ResultOfCall) && !function.isCoroutine() && !returnsKey)
        checkProofOfResultOfCopy(result, linked.result, originOfCall, std::ranges::any_of(returns, [&](const ReturnOfCopy& returned) { return holdsNaNForUndefined(returned.value); }));
    bind(call.id(), guardNumericCallResult(*this, call, result, originOfCall));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
