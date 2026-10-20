/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRBuilder.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static bool hasContents(const Block& block)
{
    const Terminator& terminator = block.terminator;
    return terminator.kind != TerminatorKind::Unreachable || !terminator.targets.isEmpty() || !block.values.isEmpty() || !block.parameters.isEmpty()
        || block.handler || block.role != BlockRole::Ordinary || block.subject || block.frequency != BlockFrequency::Normal;
}

static Value makeCast(Opcode opcode, ValueID operand, TypeID castType)
{
    Value cast = IRBuilder::make(opcode, { operand });
    cast.setCastType(castType);
    return cast;
}

static TypeID partInside(const Program& program, TypeArena& types, TypeID narrower, TypeID type)
{
    if (!narrower)
        return { };
    TypeID part = types.intersectionOf(narrower, type);
    if (part == TypeTable::never || part == type || !types.at(part).isSubsetOf(types.at(type), program))
        return { };
    return part;
}

BlockID IRBuilder::splitEdge(Edge edge)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(!edge.isHandlerEdge());

    std::optional<Target> handler = std::exchange(m_handler, std::nullopt);
    BlockID added = addBlock(rarerOf(m_function.at(edge.from).frequency, m_function.at(targetOf(m_function, edge).block).frequency));
    m_handler = WTF::move(handler);

    const Program& program = m_context.program;
    uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
    uint32_t firstResult = firstEdgeResultParameter(m_function, edge.from, edge.targetIndex);
    Origin origin = m_function.at(edge.from).terminator.origin();

    Target& split = targetOf(m_function, edge);
    Target original = std::exchange(split, Target { added, { }, { } });
    split.proofs = WTF::move(original.proofs);

    // The parameter of the old target joins what every edge into it passes. This edge alone enters the added block.
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        const Value& result = m_function.at(m_function.at(original.block).parameters[firstResult + resultIndex]);
        Value parameter(Opcode::Parameter);
        parameter.type = typeOnEdge(program, m_function, { edge.from, edge.targetIndex, resultIndex }, m_types);
        parameter.setWritten(partInside(program, m_types, result.written(), parameter.type));
        parameter.setPrediction(partInside(program, m_types, result.prediction(), parameter.type));
        parameter.setOrigin(origin);
        ValueID id = addValue(WTF::move(parameter));
        m_function.at(added).parameters.append(id);
        original.arguments.append(id);
    }

    Terminator& jump = m_function.at(added).terminator;
    jump.become(TerminatorKind::Jump);
    jump.targets.append(Target { original.block, WTF::move(original.arguments), { } });
    jump.setOrigin(origin);
    didChangeControlFlow();
    return added;
}

BlockID IRBuilder::splitBlock(ProgramPoint point)
{
    return splitBlock(point.block, singleElementSpan(point.index)).first();
}

Vector<BlockID> IRBuilder::splitBlock(BlockID block, std::span<const uint32_t> ascendingIndexes)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(block != m_block);
    Vector<BlockID> parts;
    if (ascendingIndexes.empty())
        return parts;

    size_t numberOfValues = m_function.at(block).values.size();
    for (size_t i = 1; i < ascendingIndexes.size(); ++i)
        RELEASE_ASSERT(ascendingIndexes[i - 1] < ascendingIndexes[i]);
    RELEASE_ASSERT(ascendingIndexes.back() <= numberOfValues);

    BlockFrequency frequency = m_function.at(block).frequency;
    CodeShape shape = m_function.at(block).shape;
    std::optional<Target> handler = std::exchange(m_handler, m_function.at(block).handler);
    parts.reserveInitialCapacity(ascendingIndexes.size());
    for (size_t i = 0; i < ascendingIndexes.size(); ++i) {
        parts.append(addBlock(frequency));
        m_function.at(parts.last()).shape = shape;
    }
    m_handler = WTF::move(handler);

    auto& first = m_function.at(block);
    Terminator terminator = std::exchange(first.terminator, Terminator { });
    BlockID previous = block;
    for (size_t i = 0; i < parts.size(); ++i) {
        size_t begin = ascendingIndexes[i];
        size_t end = i + 1 < parts.size() ? ascendingIndexes[i + 1] : numberOfValues;
        m_function.at(parts[i]).values.append(first.values.subspan(begin, end - begin));
        auto& jump = m_function.at(previous).terminator;
        jump.become(TerminatorKind::Jump);
        jump.targets.append(Target { parts[i], { }, { } });
        jump.setOrigin(m_origin);
        previous = parts[i];
    }
    m_function.at(previous).terminator = WTF::move(terminator);
    first.values.shrink(ascendingIndexes.front());
    didChangeControlFlow();
    return parts;
}

void IRBuilder::redirectEdge(Edge edge, BlockID target, Arguments&& arguments)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(target != entryBlock);
    Target& redirected = targetOf(m_function, edge);
    redirected.block = target;
    redirected.arguments = WTF::move(arguments);
    didChangeControlFlow();
}

void IRBuilder::redirectEdge(Edge edge, Target&& target)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(target.block != entryBlock);
    targetOf(m_function, edge) = WTF::move(target);
    didChangeControlFlow();
}

void IRBuilder::convertBranchToJump(BlockID block, uint32_t targetIndex)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    auto& terminator = m_function.at(block).terminator;
    RELEASE_ASSERT(terminator.isConditionalBranch());

    Vector<ValueID, 2> results;
    uint32_t numberOfResults = numberOfEdgeResults(m_function, block, targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        EdgeResult result = edgeResultAt(m_function, { block, targetIndex, resultIndex });
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            results.append(result.subject);
            break;
        case EdgeResultKind::Guard: {
            Value guard(Opcode::AllGuards);
            prepare(guard);
            guard.setOrigin(terminator.origin());
            ValueID id = addValue(WTF::move(guard));
            m_function.at(block).values.append(id);
            results.append(id);
            break;
        }
        case EdgeResultKind::DeclaredResult:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Target target = WTF::move(terminator.targets[targetIndex]);
    target.arguments.append(results.span());
    target.proofs.clear();
    terminator.become(TerminatorKind::Jump);
    terminator.targets.append(WTF::move(target));
    didChangeControlFlow();
}

void IRBuilder::demoteReturnCall(BlockID block)
{
    Terminator& terminator = m_function.at(block).terminator;
    RELEASE_ASSERT(terminator.kind == TerminatorKind::ReturnCall && block != m_block && m_insertions.isEmpty());

    ValueID result = terminator.callOperand();
    // SiteExpansion returns the converted result of a call that it has marked itself.
    if (Value& call = m_function.at(result); FlagMask::isDemotedTailCall(call.opcode))
        call.setIsDemotedTailCall(true);
    Vector<TailCast, 2> casts;
    for (TailCast cast : terminator.casts())
        casts.append(cast);

    BlockID previousBlock = m_block;
    Origin previousOrigin = m_origin;
    setOrigin(terminator.origin());
    terminator = Terminator();
    moveToEndOf(block);

    TypingContext typing { m_context.program, m_function, m_types };
    bool canPredict = isLegalIn(Opcode::Predict, m_context.program.form());
    for (const TailCast& cast : casts) {
        TypeID castType = typing.typeIDOf(cast.declared);
        switch (cast.resolution) {
        case CastResolution::Unresolved: {
            Value request = make(Opcode::ClaimDeclared, { result });
            request.setDeclared(cast.declared);
            result = append(WTF::move(request));
            continue;
        }
        case CastResolution::Nothing:
            continue;
        case CastResolution::Upcast:
            result = appendUpcast(result, castType);
            break;
        case CastResolution::Claim: {
            Value claim = makeCast(Opcode::Claim, result, castType);
            claim.setSite(cast.declared.site);
            claim.setCause(CheckCause::MayFail);
            result = append(WTF::move(claim));
            break;
        }
        case CastResolution::Prediction:
            break;
        case CastResolution::Assume:
            if (m_context.build.validates()) {
                Value test = make(Opcode::ValidateType, { result });
                test.setValidatedType(castType);
                append(WTF::move(test));
            }
            result = append(makeCast(Opcode::AssumeType, result, castType));
            break;
        }

        TypeID predicted = typing.predicting().typeIDOf(cast.declared);
        if (!canPredict || predicted == castType)
            continue;
        Value predict = make(Opcode::Predict, { result });
        predict.setPredicted(predicted);
        result = append(WTF::move(predict));
    }
    emitReturn(result);

    m_origin = previousOrigin;
    m_block = previousBlock;
}

void IRBuilder::removeHandler(BlockID block)
{
    m_function.at(block).handler = std::nullopt;
    didChangeControlFlow();
}

size_t IRBuilder::emptyUnreachableBlocks()
{
    RELEASE_ASSERT(!m_block && m_insertions.isEmpty());
    std::optional<CFG> localCFG;
    const CFG& cfg = m_analyses ? m_analyses->cfg() : localCFG.emplace(m_function);
    size_t numberOfEmptiedBlocks = 0;
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        Block& block = m_function.at(BlockID(index));
        if (cfg.isReachable(BlockID(index)) || !hasContents(block))
            continue;
        block = Block { };
        ++numberOfEmptiedBlocks;
    }
    if (numberOfEmptiedBlocks)
        didChangeControlFlow();
    return numberOfEmptiedBlocks;
}

void IRBuilder::coverWithHandler(std::span<const BlockID> blocks, const Target& callerHandler)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(callerHandler.block != entryBlock);
    Target handler = callerHandler;
    for (BlockID block : blocks) {
        std::optional<Target>& covering = m_function.at(block).handler;
        if (!covering)
            covering = handler;
    }
    didChangeControlFlow();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
