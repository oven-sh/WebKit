/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkTables.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBodyIndex.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "AOTType.h"
#include <algorithm>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet looksInLinkCallbacks { Opcode::TypeTest, Opcode::ToBoolean, Opcode::TypeOf };
static constexpr OpcodeSet takersInLinkCallbacks { Opcode::Call, Opcode::CallDirect, Opcode::NewClosure };
static constexpr unsigned maxNamesInLinkCallbacks = 8;

static bool handsOnInLinkCallbacks(Opcode opcode, uint32_t operandIndex)
{
    return !operandIndex && inputPassThroughOpcodes.contains(opcode);
}

static bool firstMentionSuitsCallbackInLinkCallbacks(const BodyIndex& index, ValueID value)
{
    for (unsigned step = 0; step < maxNamesInLinkCallbacks; ++step) {
        std::optional<BodyUser> user = index.firstUser(value);
        if (!user || user->kind != BodyUseKind::Operand)
            return false;
        BodyValue taker = index.value(user->value);
        Opcode opcode = taker.opcode();
        if (takersInLinkCallbacks.contains(opcode))
            return true;
        if (looksInLinkCallbacks.contains(opcode))
            return true;
        if (!inputPassThroughOpcodes.contains(opcode) || taker.operand(0) != value)
            return false;
        value = user->value;
    }
    return false;
}

namespace {

struct NameInLinkCallbacks {
    ValueID value;
    uint32_t mentionsUnderstood { 0 };
};

struct HandingOnInLinkCallbacks {
    uint32_t to { 0 };
    uint32_t from { 0 };

    friend constexpr auto operator<=>(const HandingOnInLinkCallbacks&, const HandingOnInLinkCallbacks&) = default;
};

struct BodyReadInLinkCallbacks {
    const Program& program;
    const LinkTables& tables;
    const BodyIndex& index;
};

struct MentionsInLinkCallbacks {
    Vector<uint32_t, 4> placesHandedTo;
    bool isCaptured { false };
};

} // anonymous namespace

static FunctionID functionOfVariableThatIsCalledInLinkCallbacks(const BodyReadInLinkCallbacks& body, const BodyValue& call)
{
    const BodyIndex& index = body.index;
    ValueID read = index.stripCasts(call.calleeOperand());
    if (!index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.stripCasts(index.value(read).valueOperand());
    if (index.isParameter(read) || index.opcodeOf(read) != Opcode::GetModuleVariable)
        return { };
    VariableID variable = index.value(read).variable();
    const Variable& declaration = body.program.at(variable);
    if (declaration.owner || declaration.initialization != VariableInitialization::Function || variable.index() >= body.tables.variables.size() || body.tables.variable(variable).isStoredInto)
        return { };
    return declaration.function;
}

static FunctionID functionInSightThatIsEnteredInLinkCallbacks(const BodyReadInLinkCallbacks& body, const BodyValue& call)
{
    return call.opcode() == Opcode::CallDirect ? call.function() : functionOfVariableThatIsCalledInLinkCallbacks(body, call);
}

static std::optional<uint32_t> placeThatTakesOperandInLinkCallbacks(const BodyReadInLinkCallbacks& body, const BodyValue& call, uint32_t operandIndex)
{
    Opcode opcode = call.opcode();
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(opcode);
    if (operandIndex < numberOfNamedOperands || isHeldOperand(call, operandIndex))
        return std::nullopt;
    uint32_t position = operandIndex - numberOfNamedOperands;
    FunctionID callee = functionInSightThatIsEnteredInLinkCallbacks(body, call);
    if (!callee || callee.index() >= body.tables.functions.size())
        return std::nullopt;
    const Function& entered = body.program.at(callee);
    uint32_t firstParameter = entered.firstParameterIndex();
    uint32_t entryParameter = opcode == Opcode::CallDirect ? position : firstParameter + position;
    bool isCapture = entryParameter < entered.captures.size();
    if (!isCapture && (entryParameter < firstParameter || entryParameter - firstParameter >= entered.numberOfPositionalParameters()))
        return std::nullopt;
    const LinkedFunction& linked = body.tables.function(callee);
    if (entryParameter >= linked.numberOfParameters)
        return std::nullopt;
    return linked.firstParameter + entryParameter;
}

static bool listPlacesThatTakeCaptureInLinkCallbacks(const BodyReadInLinkCallbacks& body, const BodyValue& closure, uint32_t operandIndex, Vector<uint32_t, 4>& placesHandedTo)
{
    const BodyIndex& index = body.index;
    FunctionID made = closure.function();
    uint32_t capture = operandIndex - numberOfNamedOperandsOf(Opcode::NewClosure);
    if (made.index() >= body.tables.functions.size() || capture >= body.program.at(made).captures.size() || capture >= body.tables.function(made).numberOfParameters)
        return false;

    ValueID name = closure.id();
    ValueID taker = index.onlyUsingValue(name);
    while (taker && inputPassThroughOpcodes.contains(index.opcodeOf(taker)) && index.value(taker).operand(0) == name) {
        name = taker;
        taker = index.onlyUsingValue(name);
    }
    if (!taker || (index.opcodeOf(taker) != Opcode::Call && index.opcodeOf(taker) != Opcode::CallDirect) || index.blockOf(taker) != index.blockOf(closure.id()))
        return false;
    std::optional<BodyUser> userOfCall = index.onlyUser(taker);
    if (userOfCall && userOfCall->kind == BodyUseKind::TerminatorOperand && index.terminator(userOfCall->block).kind() == TerminatorKind::ReturnCall)
        return false;

    BodyValue call = index.value(taker);
    std::span<const ValueID> arguments = call.argumentsOperands();
    size_t position = std::ranges::find(arguments, name) - arguments.begin();
    if (position == arguments.size())
        return false;
    std::optional<uint32_t> placeOfClosure = placeThatTakesOperandInLinkCallbacks(body, call, numberOfNamedOperandsOf(call.opcode()) + static_cast<uint32_t>(position));
    if (!placeOfClosure)
        return false;
    placesHandedTo.append(*placeOfClosure);
    placesHandedTo.append(body.tables.function(made).firstParameter + capture);
    return true;
}

static bool isHeldAgainstBuiltinInLinkCallbacks(const BodyIndex& index, const BodyValue& comparison, uint32_t operandIndex)
{
    if (comparison.opcode() != Opcode::StrictEq)
        return false;
    ValueID other = comparison.operand(operandIndex ? 0 : 1);
    return other && !index.isParameter(other) && index.opcodeOf(other) == Opcode::Builtin;
}

static bool isOnlyCalledInLinkCallbacks(const BodyReadInLinkCallbacks& body, ValueID parameter, MentionsInLinkCallbacks& found)
{
    const BodyIndex& index = body.index;
    if (index.isUnused(parameter))
        return true;
    if (!firstMentionSuitsCallbackInLinkCallbacks(index, parameter))
        return false;

    Vector<NameInLinkCallbacks, maxNamesInLinkCallbacks> names;
    names.append({ parameter });
    uint32_t mentionsNotSeen = index.useCount(parameter);
    auto find = [&](ValueID value) -> NameInLinkCallbacks* {
        for (NameInLinkCallbacks& name : names) {
            if (name.value == value)
                return &name;
        }
        return nullptr;
    };
    bool doesMore = false;
    auto addName = [&](ValueID value) {
        mentionsNotSeen += index.useCount(value);
        if (names.size() == maxNamesInLinkCallbacks)
            doesMore = true;
        else
            names.append({ value });
    };

    std::optional<BodyUser> firstUser = index.firstUser(parameter);
    BlockID blockOfFirstUser = firstUser->kind == BodyUseKind::Operand ? index.blockOf(firstUser->value) : firstUser->block;
    for (uint32_t blockIndex = blockOfFirstUser.index(); blockIndex < index.numberOfBlocks() && mentionsNotSeen && !doesMore; ++blockIndex) {
        BodyBlock block = index.block(BlockID(blockIndex));
        for (BodyValue value : block.values()) {
            Opcode opcode = value.opcode();
            bool handsOn = false;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                NameInLinkCallbacks* name = operand ? find(operand) : nullptr;
                if (!name)
                    return;
                ++name->mentionsUnderstood;
                --mentionsNotSeen;
                if (opcode == Opcode::Call && operandIndex == OperandIndex::Call::callee)
                    return;
                if (opcode == Opcode::NarrowByTest && operandIndex)
                    return;
                if (opcode == Opcode::Call || opcode == Opcode::CallDirect) {
                    std::optional<uint32_t> place = placeThatTakesOperandInLinkCallbacks(body, value, operandIndex);
                    if (place)
                        found.placesHandedTo.append(*place);
                    else
                        doesMore = true;
                } else if (opcode == Opcode::NewClosure) {
                    found.isCaptured = true;
                    doesMore |= !listPlacesThatTakeCaptureInLinkCallbacks(body, value, operandIndex, found.placesHandedTo);
                } else if (handsOnInLinkCallbacks(opcode, operandIndex))
                    handsOn = true;
                else if (!looksInLinkCallbacks.contains(opcode) && !isHeldAgainstBuiltinInLinkCallbacks(index, value, operandIndex))
                    doesMore = true;
            });
            if (handsOn)
                addName(value.id());
        }

        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            doesMore |= operand && find(operand);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            uint32_t position = 0;
            for (ValueID argument : target.arguments()) {
                ValueID joined = index.block(target.block()).parameter(position++);
                NameInLinkCallbacks* name = argument ? find(argument) : nullptr;
                if (!name)
                    continue;
                ++name->mentionsUnderstood;
                --mentionsNotSeen;
                if (!index.isUnused(joined) && !find(joined))
                    addName(joined);
            }
        });
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
                std::optional<BodyEdgeResult> result = index.edgeResult(terminator, targetIndex, target, proofIndex);
                NameInLinkCallbacks* subject = result && result->subject ? find(result->subject) : nullptr;
                if (!subject)
                    continue;
                ++subject->mentionsUnderstood;
                --mentionsNotSeen;
                if (!index.isUnused(result->parameter))
                    addName(result->parameter);
            }
            ++targetIndex;
        }
    }
    if (doesMore)
        return false;
    return std::ranges::all_of(names, [&](const NameInLinkCallbacks& name) {
        uint32_t mentions = index.useCount(name.value);
        return mentions < BodyIndex::maxCountedUses && mentions == name.mentionsUnderstood;
    });
}

static FunctionID functionThatTakesAtInLinkCallbacks(const LinkTables& tables, uint32_t place)
{
    auto behind = std::ranges::upper_bound(tables.functions, place, { }, &LinkedFunction::firstParameter);
    return FunctionID(static_cast<uint32_t>(behind - tables.functions.begin()) - 1);
}

void findParametersThatAreOnlyCalled(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, const ParallelLoops& loops, Mechanisms mechanisms, bool shouldAssumeCalleesRetainNothing, LinkTables& tables)
{
    if (!mechanisms.contains(Mechanism::CellsInFrames))
        return;

    TypeReader types(program);
    auto mayBeFunction = [&](TypeID type) {
        return type && !holdsOnlyPrimitives(type) && types.at(type).mayBeFunction();
    };
    auto findBodyThatMayOnlyCall = [&](FunctionID id) -> const BodyIndex* {
        const Function& function = program.at(id);
        if (!function.hasBody() || function.isCoroutine() || function.isModuleBody())
            return nullptr;
        const ThinSummary& summary = summaries.at(id);
        if (summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval))
            return nullptr;
        const BodyIndex* index = bodies.find(id);
        return index && index->numberOfBlocks() ? index : nullptr;
    };
    auto judgeEntryParameter = [&](FunctionID id, const BodyIndex& index, unsigned entryParameter, Vector<HandingOnInLinkCallbacks>& handingsFound) {
        uint32_t place = tables.function(id).firstParameter + entryParameter;
        MentionsInLinkCallbacks mentions;
        if (!isOnlyCalledInLinkCallbacks({ program, tables, index }, index.block(entryBlock).parameter(entryParameter), mentions) && !shouldAssumeCalleesRetainNothing)
            return;
        LinkedParameter& arriving = tables.parameters[place];
        arriving.isOnlyCalled = true;
        arriving.isCapturedWhereOnlyCalled = mentions.isCaptured;
        for (uint32_t to : mentions.placesHandedTo) {
            if (to != place)
                handingsFound.append({ to, place });
        }
    };
    Lock lockOfHandings;
    Vector<HandingOnInLinkCallbacks> handings;

    size_t numberOfFunctions = std::min<size_t>(program.functions.size(), tables.functions.size());
    auto judge = [&](size_t number, Vector<HandingOnInLinkCallbacks>& handingsOfRange) {
        FunctionID id(static_cast<uint32_t>(number));
        const Function& function = program.at(id);
        const BodyIndex* index = function.numberOfPositionalParameters() ? findBodyThatMayOnlyCall(id) : nullptr;
        if (!index)
            return;

        const LinkedFunction& linked = tables.function(id);
        BodyBlock entry = index->block(entryBlock);
        unsigned firstParameter = function.firstParameterIndex();
        for (unsigned argument = 0; argument < function.numberOfPositionalParameters(); ++argument) {
            unsigned entryParameter = firstParameter + argument;
            if (entryParameter >= linked.numberOfParameters || entryParameter >= entry.numberOfParameters())
                return;
            const LinkedParameter& arriving = tables.parameters[linked.firstParameter + entryParameter];
            if (!mayBeFunction(arriving.proven) && !mayBeFunction(arriving.writtenOrSeenToArrive()))
                continue;
            judgeEntryParameter(id, *index, entryParameter, handingsOfRange);
        }
    };
    forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t first, size_t end) {
        Vector<HandingOnInLinkCallbacks> handingsOfRange;
        for (size_t number = first; number < end; ++number)
            judge(number, handingsOfRange);
        if (handingsOfRange.isEmpty())
            return;
        Locker locker { lockOfHandings };
        handings.appendVector(handingsOfRange);
    });

    BitVector capturesJudged(tables.parameters.size());
    for (size_t next = 0; next < handings.size(); ++next) {
        uint32_t place = handings[next].to;
        FunctionID taker = functionThatTakesAtInLinkCallbacks(tables, place);
        uint32_t entryParameter = place - tables.function(taker).firstParameter;
        if (entryParameter >= program.at(taker).captures.size() || capturesJudged.quickSet(place))
            continue;
        const BodyIndex* index = findBodyThatMayOnlyCall(taker);
        if (index && entryParameter < index->block(entryBlock).numberOfParameters())
            judgeEntryParameter(taker, *index, entryParameter, handings);
    }

    std::ranges::sort(handings);
    auto forEachThatHandsTo = [&](uint32_t place, const auto& functor) {
        auto first = std::ranges::lower_bound(handings, HandingOnInLinkCallbacks { place, 0 });
        for (; first != handings.end() && first->to == place; ++first)
            functor(first->from);
    };
    auto forEachPlaceHandedTo = [&](const auto& functor) {
        for (size_t index = 0; index < handings.size(); ++index) {
            if (!index || handings[index].to != handings[index - 1].to)
                functor(handings[index].to);
        }
    };

    Vector<uint32_t> takenBack;
    auto takeBackFromAllThatHandTo = [&](uint32_t place) {
        forEachThatHandsTo(place, [&](uint32_t from) {
            LinkedParameter& giver = tables.parameters[from];
            if (!std::exchange(giver.isOnlyCalled, false))
                return;
            giver.isCapturedWhereOnlyCalled = false;
            takenBack.append(from);
        });
    };
    forEachPlaceHandedTo([&](uint32_t place) {
        if (!tables.parameters[place].isOnlyCalled)
            takeBackFromAllThatHandTo(place);
    });
    while (!takenBack.isEmpty())
        takeBackFromAllThatHandTo(takenBack.takeLast());
}

void carryExpectationOfThisIntoCaptures(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, LinkTables& tables)
{
    size_t numberOfFunctions = std::min<size_t>({ program.functions.size(), tables.functions.size(), summaries.size() });
    for (uint32_t number = 0; number < numberOfFunctions; ++number) {
        FunctionID maker(number);
        const ThinSummary& summary = summaries.at(maker);
        std::optional<unsigned> ownThis = summary.sitesThatHandOverCaptures.isEmpty() ? std::nullopt : program.at(maker).thisParameterIndex();
        if (!ownThis || *ownThis >= tables.function(maker).numberOfParameters)
            continue;
        TypeID written = tables.parametersOf(maker)[*ownThis].written;
        const BodyIndex* index = written ? bodies.find(maker) : nullptr;
        if (!index || !index->numberOfBlocks())
            continue;
        ValueID held = index->block(entryBlock).parameter(*ownThis);
        for (ValueID site : summary.sitesThatHandOverCaptures) {
            BodyValue value = index->value(site);
            if (value.opcode() != Opcode::NewClosure)
                continue;
            FunctionID made = value.as<Opcode::NewClosure>().function();
            if (made.index() >= tables.functions.size() || program.at(made).kind == FunctionKind::Arrow)
                continue;
            std::span<const ValueID> operands = value.as<Opcode::NewClosure>().capturesOperands();
            size_t numberOfCaptures = std::min<size_t>({ operands.size(), program.at(made).captures.size(), tables.function(made).numberOfParameters });
            for (size_t capture = 0; capture < numberOfCaptures; ++capture) {
                if (!operands[capture] || index->stripCasts(operands[capture]) != held)
                    continue;
                LinkedParameter& captured = tables.parameters[tables.function(made).firstParameter + capture];
                captured.written = written;
                captured.seenToArrive = { };
            }
        }
    }
}

static constexpr OpcodeSet looksInLinkLooks { Opcode::TypeTest, Opcode::ToBoolean, Opcode::TypeOf, Opcode::StrictEq };
static constexpr OpcodeSet makersOfObjectsInLinkLooks { Opcode::NewObject, Opcode::Data };
static_assert(!OperandIndex::PropGet::object);

namespace {

struct HandingOnInLinkLooks {
    uint32_t to { 0 };
    uint32_t from { 0 };
    friend constexpr auto operator<=>(const HandingOnInLinkLooks&, const HandingOnInLinkLooks&) = default;
};

} // anonymous namespace

static bool isNameThatOnlyFieldsAnswerInLinkLooks(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && !isOnObjectPrototype(program.nameClassOf(key));
}

static ValueID valueBehindWhatHandsOnInLinkLooks(const BodyIndex& index, ValueID id)
{
    while (!index.isParameter(id)) {
        BodyValue definition = index.value(id);
        if (!inputPassThroughOpcodes.contains(definition.opcode()) || !definition.operand(0))
            break;
        id = definition.operand(0);
    }
    return id;
}

static bool isOnlyLookedAtInLinkLooks(const Program& program, const LinkTables& tables, const BodyIndex& index, ValueID parameter, Vector<uint32_t, 4>& placesHandedTo)
{
    BitVector names(index.numberOfValues());
    uint32_t mentionsNotSeen = 0;
    bool doesMore = false;
    auto addName = [&](ValueID value) {
        uint32_t mentions = index.useCount(value);
        if (mentions == BodyIndex::maxCountedUses) {
            doesMore = true;
            return;
        }
        names.quickSet(value.index());
        mentionsNotSeen += mentions;
    };
    auto isName = [&](ValueID value) {
        return value && names.quickGet(value.index());
    };

    addName(parameter);
    std::optional<BodyUser> firstUser = index.firstUser(parameter);
    BlockID blockOfFirstUser = !firstUser ? entryBlock : firstUser->kind == BodyUseKind::Operand ? index.blockOf(firstUser->value) : firstUser->block;
    for (uint32_t blockIndex = blockOfFirstUser.index(); blockIndex < index.numberOfBlocks() && mentionsNotSeen && !doesMore; ++blockIndex) {
        BodyBlock block = index.block(BlockID(blockIndex));
        for (BodyValue value : block.values()) {
            Opcode opcode = value.opcode();
            bool handsOn = false;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!isName(operand))
                    return;
                --mentionsNotSeen;
                if (looksInLinkLooks.contains(opcode) || (opcode == Opcode::PropGet && !operandIndex && isNameThatOnlyFieldsAnswerInLinkLooks(program, value.key())) || (opcode == Opcode::NarrowByTest && operandIndex))
                    return;
                if (opcode == Opcode::Call || opcode == Opcode::CallDirect) {
                    if (std::optional<uint32_t> place = placeThatTakesOperandInLinkCallbacks({ program, tables, index }, value, operandIndex))
                        placesHandedTo.append(*place);
                    else
                        doesMore = true;
                } else if (!operandIndex && inputPassThroughOpcodes.contains(opcode))
                    handsOn = true;
                else
                    doesMore = true;
            });
            if (handsOn)
                addName(value.id());
        }

        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            doesMore |= isName(operand);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            uint32_t position = 0;
            for (ValueID argument : target.arguments()) {
                ValueID joined = index.block(target.block()).parameter(position++);
                if (!isName(argument))
                    continue;
                --mentionsNotSeen;
                if (!index.isUnused(joined) && !isName(joined))
                    addName(joined);
            }
        });
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
                std::optional<BodyEdgeResult> result = index.edgeResult(terminator, targetIndex, target, proofIndex);
                if (!result || !isName(result->subject))
                    continue;
                --mentionsNotSeen;
                if (!index.isUnused(result->parameter) && !isName(result->parameter))
                    addName(result->parameter);
            }
            ++targetIndex;
        }
    }
    return !doesMore && !mentionsNotSeen;
}

void findParametersThatAreOnlyLookedAt(const Program& program, const BodyIndexTable& bodies, const ThinSummaries& summaries, Mechanisms mechanisms, bool shouldAssumeCalleesRetainNothing, LinkTables& tables)
{
    if (!mechanisms.contains(Mechanism::CellsInFrames))
        return;

    BitVector asked(tables.parameters.size());
    Vector<uint32_t> placesAsked;
    auto ask = [&](uint32_t place) {
        if (!asked.quickSet(place))
            placesAsked.append(place);
    };

    size_t numberOfFunctions = std::min<size_t>(program.functions.size(), tables.functions.size());
    for (size_t number = 0; number < numberOfFunctions; ++number) {
        FunctionID id(static_cast<uint32_t>(number));
        if (!tables.function(id).isReached || !program.at(id).hasBody() || program.at(id).isCoroutine())
            continue;
        const ThinSummary& summary = summaries.at(id);
        const BodyIndex* index = nullptr;
        for (const SummaryCall& call : summary.calls) {
            if (!call.numberOfArguments || (call.callee != SummaryCall::Callee::Value && call.callee != SummaryCall::Callee::Binding) || !call.site || call.isConstruction || call.passesList || call.isMadeByProtocol)
                continue;
            bool takesCellMadeHere = std::ranges::any_of(summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments), [&](uint32_t node) {
                return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Allocation;
            });
            if (!takesCellMadeHere)
                continue;
            if (!index)
                index = bodies.find(id);
            if (!index)
                break;
            BodyValue taker = index->value(call.site);
            if (taker.opcode() != Opcode::Call && taker.opcode() != Opcode::CallDirect)
                continue;
            taker.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!operand)
                    return;
                ValueID made = valueBehindWhatHandsOnInLinkLooks(*index, operand);
                if (index->isParameter(made) || !makersOfObjectsInLinkLooks.contains(index->opcodeOf(made)))
                    return;
                if (std::optional<uint32_t> place = placeThatTakesOperandInLinkCallbacks({ program, tables, *index }, taker, operandIndex))
                    ask(*place);
            });
        }
    }

    Vector<HandingOnInLinkLooks> handings;
    for (size_t next = 0; next < placesAsked.size(); ++next) {
        uint32_t place = placesAsked[next];
        FunctionID taker = functionThatTakesAtInLinkCallbacks(tables, place);
        const Function& function = program.at(taker);
        if (!function.hasBody() || function.isCoroutine() || function.isModuleBody())
            continue;
        const ThinSummary& summary = summaries.at(taker);
        if (summary.has(SummaryTrait::UsesArguments) || summary.has(SummaryTrait::HasDirectEval))
            continue;
        const BodyIndex* index = bodies.find(taker);
        uint32_t entryParameter = place - tables.function(taker).firstParameter;
        if (!index || !index->numberOfBlocks() || entryParameter >= index->block(entryBlock).numberOfParameters())
            continue;

        Vector<uint32_t, 4> placesHandedTo;
        if (!shouldAssumeCalleesRetainNothing && !isOnlyLookedAtInLinkLooks(program, tables, *index, index->block(entryBlock).parameter(entryParameter), placesHandedTo))
            continue;
        tables.parameters[place].isOnlyLookedAt = true;
        for (uint32_t to : placesHandedTo) {
            if (to == place)
                continue;
            handings.append({ to, place });
            ask(to);
        }
    }

    std::ranges::sort(handings);
    Vector<uint32_t> takenBack;
    auto takeBackFromAllThatHandTo = [&](uint32_t place) {
        for (auto giver = std::ranges::lower_bound(handings, HandingOnInLinkLooks { place, 0 }); giver != handings.end() && giver->to == place; ++giver) {
            if (std::exchange(tables.parameters[giver->from].isOnlyLookedAt, false))
                takenBack.append(giver->from);
        }
    };
    for (size_t index = 0; index < handings.size(); ++index) {
        uint32_t place = handings[index].to;
        if ((!index || place != handings[index - 1].to) && !tables.parameters[place].isOnlyLookedAt)
            takeBackFromAllThatHandTo(place);
    }
    while (!takenBack.isEmpty())
        takeBackFromAllThatHandTo(takenBack.takeLast());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
