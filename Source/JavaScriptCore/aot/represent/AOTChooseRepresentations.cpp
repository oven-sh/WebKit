/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConversions.h"
#include "AOTRepresentationWebs.h"
#include <algorithm>
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The inserter asks for the lists of uses, and many a function needs no conversion of its making.
struct ConversionWalk {
    ConversionInserter& inserter() const
    {
        if (!madeInserter)
            madeInserter.emplace(pass, builder);
        return *madeInserter;
    }

    const RepresentationInputs& inputs;
    Function& function;
    FunctionPassContext& pass;
    IRBuilder& builder;
    std::optional<ConversionInserter>& madeInserter;
};

} // anonymous namespace

static void uncoverEntryBlock(FunctionPassContext& pass, IRBuilder& builder)
{
    const Block& entry = pass.function.at(entryBlock);
    if (!entry.handler)
        return;

    builder.setOrigin(entry.terminator.origin());
    builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    pass.analyses.didChangeControlFlow();
}

static constexpr unsigned maxCopiesFollowed = 4;

static bool isMadeBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, unsigned copiesFollowed = 0)
{
    const Function& function = inputs.function;
    const Value& value = function.at(id);
    if (!picksOwnRepresentation(function, id))
        return resultRepresentationOf(inputs, value) == Representation::JSValue;
    if (copiesFollowed == maxCopiesFollowed || constantsInFormOfUses.contains(value.opcode) || value.opcode == Opcode::Unspecified)
        return false;

    if (value.opcode != Opcode::Parameter) {
        bool isBoxed = true;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (isForwardedOperand(value, operandIndex))
                isBoxed = isBoxed && isMadeBoxed(pass, inputs, operand, copiesFollowed + 1);
        });
        return isBoxed;
    }

    ValueLocation location = pass.analyses.useLists().locationOf(id);
    for (Edge edge : pass.analyses.cfg().incomingEdges(location.block)) {
        if (edge.isHandlerEdge())
            return false;
        IncomingValue incoming = incomingValueOf(function, edge, location.index);
        ValueID source = incoming.argument;
        if (incoming.kind == IncomingValue::Kind::EdgeResult) {
            EdgeResult result = edgeResultAt(function, incoming.result);
            if (result.kind != EdgeResultKind::NarrowedOperand)
                return false;
            source = result.subject;
        }
        if (!isMadeBoxed(pass, inputs, source, copiesFollowed + 1))
            return false;
    }
    return true;
}

namespace {

struct UsesOfCopies {
    Vector<ValueID, 16> copies;
    uint64_t votesForBoxed { 0 };
    uint64_t votesForOthers { 0 };
};

} // anonymous namespace

static void countUsesOfCopies(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id, UsesOfCopies& uses)
{
    constexpr size_t maxCopies = 16;
    const Function& function = inputs.function;
    if (uses.copies.size() == maxCopies || uses.copies.contains(id))
        return;
    uses.copies.append(id);

    auto count = [&](Representation wanted, BlockID block) {
        if (hasRegister(wanted))
            (wanted == Representation::JSValue ? uses.votesForBoxed : uses.votesForOthers) += votesOfConversionIn(pass.analyses, block);
    };
    for (const Use& use : pass.analyses.useLists().usesOf(id)) {
        switch (use.kind) {
        case Use::Kind::Operand: {
            const Value& user = function.at(use.user);
            if (isForwardedOperand(user, use.operandIndex))
                countUsesOfCopies(pass, inputs, use.user, uses);
            else
                count(wantedRepresentationOf(inputs, user, use.operandIndex), use.block);
            break;
        }
        case Use::Kind::TerminatorOperand:
            count(wantedRepresentationOf(inputs, function.at(use.block).terminator, use.operandIndex), use.block);
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument: {
            ValueID parameter = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
            if (defaultRepresentationOf(inputs.typeOf(parameter)) == Representation::JSValue)
                count(Representation::JSValue, use.block);
            else
                countUsesOfCopies(pass, inputs, parameter, uses);
            break;
        }
        case Use::Kind::BlockSubject:
            break;
        }
    }
}

static bool isWantedBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs, ValueID id)
{
    UsesOfCopies uses;
    countUsesOfCopies(pass, inputs, id, uses);
    return uses.votesForBoxed > uses.votesForOthers;
}

template<typename Predicate>
static bool everyOperandBesideInt32(const RepresentationInputs& inputs, const Value& sum, const Predicate& holds)
{
    bool hasSuchOperand = false;
    bool holdsForAll = true;
    sum.forEachOperand([&](ValueID operand, uint32_t) {
        if (inputs.typeOf(operand).isInt32())
            return;
        hasSuchOperand = true;
        holdsForAll = holdsForAll && holds(operand);
    });
    return hasSuchOperand && holdsForAll;
}

static Vector<ValueID, 8> keepSumsOfBoxedNumbersBoxed(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Function& function = pass.function;
    Vector<ValueID, 8> kept;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (!sumsOfNumbers.contains(value.opcode) || value.form() != OperatorForm::Numbers)
                continue;
            bool takesBoxedNumbers = everyOperandBesideInt32(inputs, value, [&](ValueID operand) {
                return inputs.typeOf(operand).primitives & Primitive::Integer && isMadeBoxed(pass, inputs, operand);
            });
            if (takesBoxedNumbers && isWantedBoxed(pass, inputs, id))
                kept.append(id);
        }
    }
    for (ValueID id : kept)
        function.edit(id).representation = Representation::JSValue;
    return kept;
}

static void settleSumsKeptBoxed(const RepresentationInputs& inputs, Function& function, std::span<const ValueID> sumsKeptBoxed)
{
    for (ValueID id : sumsKeptBoxed) {
        bool operandsStayedBoxed = everyOperandBesideInt32(inputs, function.at(id), [&](ValueID operand) {
            return function.at(operand).representation == Representation::JSValue;
        });
        if (!operandsStayedBoxed)
            function.edit(id).representation = Representation::Double;
    }
}

static void writeRepresentations(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    Vector<ValueID, 8> sumsKeptBoxed = keepSumsOfBoxedNumbersBoxed(pass, inputs);
    RepresentationWebs webs(inputs, pass.analyses);
    webs.build();
    webs.choose();

    Function& function = pass.function;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters)
            function.edit(parameter).representation = webs.representationOf(parameter);
        for (ValueID value : block.values)
            function.edit(value).representation = webs.representationOf(value);
    }

    settleSumsKeptBoxed(inputs, function, sumsKeptBoxed.span());
}

static bool needsConversion(const Function& function, ValueID value, Representation wanted)
{
    return hasRegister(wanted) && function.at(value).representation != wanted;
}

static void convertValueOperands(const ConversionWalk& walk, BlockID block)
{
    Function& function = walk.function;
    const BlockValues& values = function.at(block).values;
    for (uint32_t index = 0; index < values.size(); ++index) {
        ValueID user = values[index];
        uint32_t numberOfOperands = function.at(user).numberOfOperands();
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID operand = function.at(user).operand(operandIndex);
            if (!operand)
                continue;
            Representation wanted = wantedRepresentationOf(walk.inputs, function.at(user), operandIndex);
            if (needsConversion(function, operand, wanted))
                walk.inserter().convertOperand(user, operandIndex, wanted, { block, index });
        }
    }
}

static void convertTerminatorOperands(const ConversionWalk& walk, BlockID block)
{
    const Terminator& terminator = walk.function.at(block).terminator;
    for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
        ValueID operand = terminator.operand(operandIndex);
        if (!operand)
            continue;
        Representation wanted = wantedRepresentationOf(walk.inputs, terminator, operandIndex);
        if (needsConversion(walk.function, operand, wanted))
            walk.inserter().convertTerminatorOperand(block, operandIndex, wanted);
    }
}

static void convertEdgeResults(const ConversionWalk& walk, Edge edge)
{
    const Function& function = walk.function;
    const CompactList<ValueID>& parameters = function.at(targetOf(function, edge).block).parameters;
    uint32_t firstResult = firstEdgeResultParameter(function, edge.from, edge.targetIndex);
    uint32_t numberOfResults = numberOfEdgeResults(function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        uint32_t parameterIndex = firstResult + resultIndex;
        Representation passed = edgeResultRepresentationOf(walk.inputs, { edge.from, edge.targetIndex, resultIndex });
        Representation wanted = function.at(parameters[parameterIndex]).representation;
        if (hasRegister(wanted) && !isSameWord(passed, wanted))
            walk.inserter().convertEdgeResult(edge, parameterIndex, passed, wanted);
    }
}

static void convertEdgeArguments(const ConversionWalk& walk, Edge edge)
{
    const Function& function = walk.function;
    const Target& target = targetOf(function, edge);
    const CompactList<ValueID>& parameters = function.at(target.block).parameters;
    for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
        ValueID argument = target.arguments[argumentIndex];
        if (!argument)
            continue;
        Representation wanted = function.at(parameters[argumentIndex]).representation;
        if (hasRegister(wanted) && !isSameWord(function.at(argument).representation, wanted))
            walk.inserter().convertEdgeArgument(edge, argumentIndex, wanted);
    }
}

static void insertConversions(const ConversionWalk& walk, std::span<const BlockID> reversePostOrder)
{
    for (BlockID block : reversePostOrder) {
        convertValueOperands(walk, block);
        convertTerminatorOperands(walk, block);
        forEachOutgoingEdge(walk.function, block, [&](Edge edge, const Target&) {
            if (!edge.isHandlerEdge())
                convertEdgeResults(walk, edge);
            convertEdgeArguments(walk, edge);
        });
    }
    walk.inserter().finish();
}

// The code generator hands an operation each of its operands by the id of the value, in one form.
template<typename Operation>
static bool takesOneValueInTwoForms(const ConversionWalk& walk, const Operation& user)
{
    constexpr uint32_t maxOperandsComparedInPairs = 6;
    uint32_t numberOfOperands = user.numberOfOperands();
    if (numberOfOperands < 2)
        return false;

    auto formTakenAt = [&](uint32_t operandIndex) {
        ValueID operand = user.operand(operandIndex);
        Representation wanted = wantedRepresentationOf(walk.inputs, user, operandIndex);
        return needsConversion(walk.function, operand, wanted) ? wanted : walk.function.at(operand).representation;
    };
    if (numberOfOperands <= maxOperandsComparedInPairs) {
        for (uint32_t later = 1; later < numberOfOperands; ++later) {
            ValueID operand = user.operand(later);
            for (uint32_t earlier = 0; operand && earlier < later; ++earlier) {
                if (user.operand(earlier) == operand && formTakenAt(earlier) != formTakenAt(later))
                    return true;
            }
        }
        return false;
    }

    Vector<std::pair<ValueID, uint32_t>, 16> operands;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (ValueID operand = user.operand(operandIndex))
            operands.append({ operand, operandIndex });
    }
    std::ranges::sort(operands);
    for (size_t index = 1; index < operands.size(); ++index) {
        if (operands[index - 1].first == operands[index].first && formTakenAt(operands[index - 1].second) != formTakenAt(operands[index].second))
            return true;
    }
    return false;
}

static void insertConversionsThatCannotWait(const ConversionWalk& walk, std::span<const BlockID> reversePostOrder)
{
    Function& function = walk.function;
    for (BlockID block : reversePostOrder) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID user = values[index];
            if (!takesOneValueInTwoForms(walk, function.at(user)))
                continue;
            uint32_t numberOfOperands = function.at(user).numberOfOperands();
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                ValueID operand = function.at(user).operand(operandIndex);
                if (!operand)
                    continue;
                Representation wanted = wantedRepresentationOf(walk.inputs, function.at(user), operandIndex);
                if (needsConversion(function, operand, wanted))
                    walk.inserter().convertOperand(user, operandIndex, wanted, { block, index });
            }
        }
        if (takesOneValueInTwoForms(walk, function.at(block).terminator))
            convertTerminatorOperands(walk, block);
    }
    if (walk.madeInserter)
        walk.madeInserter->finish();
}

static void countRepresentations(FunctionPassContext& pass, uint32_t numberOfConversions)
{
    const Function& function = pass.function;
    std::array<uint64_t, numberOfEnumerators<Representation>> numberOfValuesIn { };
    auto countValue = [&](ValueID id) {
        ++numberOfValuesIn[std::to_underlying(function.at(id).representation)];
    };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            countValue(parameter);
        for (ValueID value : block.values)
            countValue(value);
    }

    auto count = [&](StatisticID statistic, uint64_t amount) {
        if (amount)
            pass.build.statistics().count(statistic, amount);
    };
    count(StatisticID::ConversionsInserted, numberOfConversions);
    count(StatisticID::ValuesInInt32, numberOfValuesIn[std::to_underlying(Representation::Int32)]);
    count(StatisticID::ValuesInInt64, numberOfValuesIn[std::to_underlying(Representation::Int64)]);
    count(StatisticID::ValuesInDouble, numberOfValuesIn[std::to_underlying(Representation::Double)]);
    count(StatisticID::ValuesBoxed, numberOfValuesIn[std::to_underlying(Representation::JSValue)]);
    count(StatisticID::ReferencesCompressed, numberOfValuesIn[std::to_underlying(Representation::Cell32)]);
}

PassOutcome chooseRepresentations(FunctionPassContext& pass)
{
    RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions && !pass.function.representationsAreChosen);
    Function& function = pass.function;
    size_t numberOfBlocksBefore = function.blocks.size();

    IRBuilder builder(pass.analyses.context(), function, pass.types);
    uncoverEntryBlock(pass, builder);

    RepresentationInputs inputs { pass.program, pass.id, function, *pass.inputs.placement, *pass.inputs.callingConventions };
    function.operandsAreConvertedAtFetch = leavesConversionsToCodeGenerator(pass.build, pass.inputs.heat, pass.program, pass.id);
    writeRepresentations(pass, inputs);

    std::optional<ConversionInserter> inserter;
    ConversionWalk walk { inputs, function, pass, builder, inserter };
    if (function.operandsAreConvertedAtFetch)
        insertConversionsThatCannotWait(walk, pass.analyses.cfg().reversePostOrder());
    else
        insertConversions(walk, pass.analyses.cfg().reversePostOrder());

    // No analysis reads a representation, and only a block that is added changes an edge.
    if (function.blocks.size() != numberOfBlocksBefore)
        pass.analyses.didChangeControlFlow();
    else if (inserter && inserter->mayHaveChangedUses()) {
        pass.analyses.didChangeValues();
        pass.keptAnalyses = controlFlowAnalyses;
    } else
        pass.keptAnalyses = allAnalyses;

    function.representationsAreChosen = true;
    countRepresentations(pass, inserter ? inserter->numberOfConversions() : 0);

    if (pass.build.options().verifiesProgram && !function.operandsAreConvertedAtFetch) {
        Vector<InternalError> errors;
        checkRepresentations(inputs, errors);
        if (!errors.isEmpty())
            pass.build.diagnostics().append(WTF::move(errors));
    }
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
