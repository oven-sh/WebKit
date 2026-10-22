/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCopy.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct InlineCopy {
    const Value call;
    const Function& callee;
    const InliningSummary& summary;
    InlinedPart part;
    BlockFrequency callBlockFrequency;
    uint32_t firstFrame;
    std::optional<Terminator> siteReturnCall;
    IRBuilder::Expansion expansion;
    Vector<BlockID> region;
    BlockID fullCallBlock;
};

} // anonymous namespace

size_t inlineFrameCountOf(const Function& function)
{
    return std::max<size_t>(function.inlineFrames.size(), 1);
}

uint32_t appendInlineFrames(Function& caller, const InlineFrame& callFrame, const Function& callee)
{
    Vector<InlineFrame>& frames = caller.inlineFrames;
    if (frames.isEmpty())
        frames.append(InlineFrame { });

    uint32_t firstFrame = frames.size();
    frames.append(callFrame);
    for (size_t index = 1; index < callee.inlineFrames.size(); ++index) {
        InlineFrame frame = callee.inlineFrames[index];
        frame.parent += firstFrame;
        frames.append(frame);
    }
    return firstFrame;
}

ValueID upcastToParameterType(IRBuilder& builder, const Value& parameter, ValueID argument)
{
    const Program& program = builder.context().program;
    // A cast takes a JavaScript value, and a raw word names the value it was made from as its base.
    if (!program.at(parameter.type).isJavaScriptValue())
        return argument;

    ValueID converted = builder.appendUpcast(argument, parameter.type);
    TypeID expected = parameter.expected();
    if (!expected || expected == builder.function().at(converted).expected())
        return converted;

    if (program.typeOf(builder.function(), converted).isSubsetOf(program.at(expected), program))
        return converted;

    // Of two expectations that exclude each other the one nearer to the use stands.
    TypeArena& types = builder.types();
    TypeID proven = builder.function().at(converted).type;
    TypeID narrowed = types.intersectionOf(builder.function().at(converted).expectedOrType(), expected);
    if (narrowed == TypeTable::never)
        narrowed = types.intersectionOf(proven, expected);
    // An intersection may keep a part of one side that the subset test does not find in the other.
    if (narrowed == TypeTable::never || narrowed == proven || !types.at(narrowed).isSubsetOf(types.at(proven), program))
        return converted;

    // What the copy expects of the value stands on a cast that changes nothing else: the caller's other uses of the argument expect what the caller does.
    Value cast = IRBuilder::make(Opcode::Upcast, { converted });
    cast.setCastType(proven);
    cast.setExpected(narrowed);
    return builder.append(WTF::move(cast));
}

void cloneBlocksIntoInlineFrame(IRBuilder& builder, RegionCloner& cloner, std::span<const BlockID> region, uint32_t firstFrame)
{
    cloner.cloneBlocks(region, [&](ValueID, Value& clone) {
        clone.inlineFrame += firstFrame;
    });
    for (BlockID id : cloner.clonedBlocks())
        builder.function().edit(id, BlockEdit::Terminator).terminator.inlineFrame += firstFrame;
}

static bool hasSignatureMismatch(const FunctionPassContext& context, const InliningSummary& summary, const Value& call, const Function& callee)
{
    const Program& program = context.program;
    if (call.numberOfOperands() != callee.numberOfEntryParameters())
        return true;
    if (callee.results.size() != 1)
        return summary.hasBareReturn;

    TypeID resultType = TypingContext { program, callee, context.types }.typeIDOf(callee.result());
    if (resultType != call.type && !program.at(resultType).isSubsetOf(program.at(call.type), program))
        return true;
    return summary.hasBareReturn && !program.at(TypeTable::undefined).isSubsetOf(program.at(resultType), program);
}

static bool namesFunctionToCall(const FunctionPassContext& context, const InliningSummary& summary, const Value& call)
{
    for (size_t entryParameter : summary.calledParameters) {
        if (entryParameter < call.numberOfOperands() && context.program.typeOf(context.function, call.operand(static_cast<uint32_t>(entryParameter))).singleFunction())
            return true;
    }
    return false;
}

static bool isCopyFor(const FunctionPassContext& context, const InlineFrame& copy, const Value& call)
{
    auto functionOf = [&](uint32_t frame) {
        return frame ? context.function.inlineFrames[frame].callee : context.id;
    };
    return copy.callPosition == call.position && functionOf(copy.parent) == functionOf(call.inlineFrame);
}

// A function that calls what it is given does something else with each function it is given: a traversal in the callback of the same traversal repeats nothing.
// Its body is copied once for each call in the text, so a chain of copies ends.
static bool closesCycle(const FunctionPassContext& context, const InliningSummary& summary, const Value& call)
{
    FunctionID callee = call.function();
    if (callee == context.id)
        return true;
    bool differsByCall = namesFunctionToCall(context, summary, call);
    const Vector<InlineFrame>& frames = context.function.inlineFrames;
    for (uint32_t frame = call.inlineFrame; frame; frame = frames[frame].parent) {
        if (frames[frame].callee == callee && (!differsByCall || isCopyFor(context, frames[frame], call)))
            return true;
    }
    return false;
}

InlineDecision canInline(const FunctionPassContext& context, const InliningSummary& summary, ValueID callID)
{
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    if (summary.barrier == InlineBarrier::None) {
        const Function& callee = context.program.at(call.function());
        if (hasSignatureMismatch(context, summary, call, callee))
            return InlineDecision::SignatureMismatch;
        if (context.build.exceeds(LimitID::InlineFramesPerFunction, inlineFrameCountOf(caller) + inlineFrameCountOf(callee), context.subject(callID)))
            return InlineDecision::CallerSizeLimit;
    }
    if (closesCycle(context, summary, call))
        return InlineDecision::ClosesCycle;
    return InlineDecision::Unknown;
}

static std::optional<Terminator> prepareTailCall(IRBuilder& builder, const InlineRequest& request)
{
    BlockID block = request.point.block;
    const Terminator& terminator = builder.function().at(block).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall || terminator.callOperand() != request.call)
        return std::nullopt;

    Terminator returnCall = terminator;
    builder.demoteReturnCall(block);
    return returnCall;
}

static void mapArguments(IRBuilder& builder, RegionCloner& cloner, const InlineCopy& copy)
{
    const auto& parameters = copy.callee.at(entryBlock).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index)
        cloner.mapValue(parameters[index], upcastToParameterType(builder, copy.callee.at(parameters[index]), copy.call.operand(index)));
}

static void cloneBody(IRBuilder& builder, RegionCloner& cloner, InlineCopy& copy)
{
    CFG cfg(copy.callee);
    switch (copy.part) {
    case InlinedPart::WholeBody:
        copy.region.append(cfg.reversePostOrder());
        break;
    case InlinedPart::Head: {
        const BitVector& headBlocks = copy.summary.headBlocks;
        copy.fullCallBlock = builder.addBlock(rarerOf(BlockFrequency::Unlikely, copy.callBlockFrequency));
        for (BlockID block : cfg.reversePostOrder()) {
            if (!headBlocks.get(block.index()))
                continue;
            copy.region.append(block);
            for (const Target& target : copy.callee.at(block).terminator.targets) {
                if (!headBlocks.get(target.block.index()) && !cloner.cloneOf(target.block))
                    cloner.mapBlock(target.block, copy.fullCallBlock);
            }
        }
        break;
    }
    }

    cloneBlocksIntoInlineFrame(builder, cloner, copy.region.span(), copy.firstFrame);
    builder.emitJump(cloner.cloneOf(entryBlock));
}

static void replaceReturn(IRBuilder& builder, BlockID block, const InlineCopy& copy)
{
    Function& caller = builder.function();
    ValueID returned = std::exchange(caller.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { }).valueOperand();
    builder.moveToEndOf(block);
    if (copy.callee.results.size() == 1) {
        builder.endExpansion(copy.expansion, returned ? returned : builder.appendUndefined());
        return;
    }

    RELEASE_ASSERT(returned);
    Vector<ValueID, 4> results;
    if (caller.at(returned).opcode == Opcode::Results) {
        results.append(caller.at(returned).valuesOperands());
        const BlockValues& values = caller.at(block).values;
        if (!values.isEmpty() && values.last() == returned) {
            BitVector indexes;
            indexes.set(values.size() - 1);
            builder.eraseValues(block, indexes);
        }
    } else {
        for (uint32_t index = 0; index < copy.expansion.results.size(); ++index) {
            Value result = IRBuilder::make(Opcode::ResultAt, { returned });
            result.setIndex(index);
            results.append(builder.append(WTF::move(result)));
        }
    }
    builder.endExpansion(copy.expansion, results.span());
}

static void appendCasts(Terminator& returnCall, const Terminator& siteReturnCall)
{
    Vector<TailCast, 4> casts;
    for (TailCast cast : returnCall.casts())
        casts.append(cast);
    for (TailCast cast : siteReturnCall.casts())
        casts.append(cast);
    returnCall.setCasts(casts.span());
}

static void finishClonedTerminator(IRBuilder& builder, BlockID clone, const InlineCopy& copy)
{
    switch (builder.function().at(clone).terminator.kind) {
    case TerminatorKind::Return:
        replaceReturn(builder, clone, copy);
        return;
    case TerminatorKind::ReturnCall:
        if (copy.siteReturnCall) {
            appendCasts(builder.function().edit(clone, BlockEdit::Terminator).terminator, *copy.siteReturnCall);
            return;
        }
        builder.demoteReturnCall(clone);
        replaceReturn(builder, clone, copy);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void finishClonedBlocks(IRBuilder& builder, const RegionCloner& cloner, const InlineCopy& copy)
{
    Function& caller = builder.function();
    builder.setOrigin(copy.call.origin());
    std::span<const BlockID> clones = cloner.clonedBlocks();
    for (size_t index = 0; index < clones.size(); ++index) {
        Block& block = caller.edit(clones[index], { BlockEdit::Arguments, BlockEdit::Rest });
        block.frequency = rarerOf(block.frequency, copy.callBlockFrequency);
        if (copy.summary.noReturnBlocks.get(copy.region[index].index()))
            block.frequency = BlockFrequency::Rare;
        for (Target& target : block.terminator.targets) {
            if (target.block != copy.fullCallBlock)
                continue;
            target.arguments.clear();
            target.proofs.clear();
        }
        finishClonedTerminator(builder, clones[index], copy);
    }
}

static void appendFullCall(IRBuilder& builder, const InlineCopy& copy)
{
    builder.setOrigin(copy.call.origin());
    builder.moveToEndOf(copy.fullCallBlock);
    Value call = copy.call;
    call.setIsAfterInlinedHead(true);
    // The ReturnCall that prepareTailCall() demoted owns this copy of the call.
    if (copy.siteReturnCall)
        call.setIsDemotedTailCall(false);
    ValueID result = builder.append(WTF::move(call));
    if (!copy.siteReturnCall) {
        builder.endExpansion(copy.expansion, result);
        return;
    }

    Terminator returnCall = *copy.siteReturnCall;
    returnCall.setCallOperand(result);
    builder.emitTerminator(WTF::move(returnCall));
}

void copyIn(FunctionPassContext& context, IRBuilder& builder, const InlineRequest& request)
{
    Function& caller = context.function;
    RELEASE_ASSERT(&builder.function() == &caller);
    RELEASE_ASSERT(caller.at(request.point.block).values[request.point.index] == request.call);

    std::optional<Terminator> siteReturnCall = prepareTailCall(builder, request);

    Value call = caller.at(request.call);
    RELEASE_ASSERT(call.opcode == Opcode::CallDirect);
    FunctionID calleeID = call.function();
    const Function& callee = context.program.at(calleeID);
    BlockFrequency callBlockFrequency = caller.at(request.point.block).frequency;
    uint32_t firstFrame = appendInlineFrames(caller, InlineFrame {
        .callee = calleeID,
        .callPosition = call.position,
        .parent = call.inlineFrame,
        .isTailCall = call.isDemotedTailCall(),
        .isConstruction = call.isConstruction(),
        .module = callee.module,
    }, callee);

    InlineCopy copy {
        WTF::move(call),
        callee,
        request.callee,
        request.part,
        callBlockFrequency,
        firstFrame,
        WTF::move(siteReturnCall),
        request.deferredResultReads ? builder.beginExpansion(request.point, *request.deferredResultReads) : builder.beginExpansion(request.point),
        { },
        { },
    };
    RegionCloner cloner(builder, callee);
    mapArguments(builder, cloner, copy);
    cloneBody(builder, cloner, copy);
    finishClonedBlocks(builder, cloner, copy);
    if (copy.fullCallBlock)
        appendFullCall(builder, copy);

    builder.setHandler(std::nullopt);
    context.analyses.didChangeControlFlow();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
