/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTCodeRecords.h"
#include "AOTEdgeResults.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeValidationEmitter.h"
#include "B3UpsilonValue.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static void setOrigin(Output& out, ValueID id)
{
    out.setOrigin(b3OriginOf(id).dfgOrigin());
}

static bool hasParameterWithRegister(const Function& function, BlockID block)
{
    return std::ranges::any_of(function.at(block).parameters, [&](ValueID parameter) {
        return hasRegister(function.at(parameter).representation);
    });
}

static Operand takeIncomingOperand(const CodeGenerator& generator, Edge edge, uint32_t parameterIndex, std::span<const Operand>& declaredResults)
{
    IncomingValue incoming = incomingValueOf(generator.function(), edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return generator.get(incoming.argument);
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(generator.function(), incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return generator.get(result.subject);
    case EdgeResultKind::Guard:
        return { };
    case EdgeResultKind::DeclaredResult: {
        RELEASE_ASSERT(!declaredResults.empty());
        Operand operand = declaredResults.front();
        declaredResults = declaredResults.subspan(1);
        return operand;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

GenerationResult CodeGenerator::run()
{
    RELEASE_ASSERT(function().representationsAreChosen);
    if (!recordSitesWithoutCode())
        return GenerationResult::Failed;

    m_values.fill(nullptr, function().values.size());
    m_resultRanges.grow(function().values.size());
    createBlocks();

    m_out.initializeConstants(m_proc, m_out.m_block);
    definePinnedValues(m_out, m_proc, m_inputs.program.instanceSizes, m_pinned);
    emitter<EntryEmitter>().generateEntry();
    if (m_hasFailed)
        return GenerationResult::Failed;
    createPhis();
    m_out.jump(blockFor(entryBlock));

    for (BlockID block : analyses().cfg().reversePostOrder()) {
        generateBlock(block);
        if (m_hasFailed)
            return GenerationResult::Failed;
    }
    publishInductionVariables();
    finishProcedure();
    return GenerationResult::Generated;
}

bool CodeGenerator::recordSitesWithoutCode()
{
    Subject subject { .function = m_inputs.id };
    size_t numberOfInlineFrames = function().inlineFrames.size();
    if (numberOfInlineFrames > build().limit(LimitID::InlineFramesPerFunction)) {
        failAtLimit(LimitID::InlineFramesPerFunction, numberOfInlineFrames, subject);
        return false;
    }

    SiteRecords& sites = m_records.sites;
    for (auto& frame : function().inlineFrames) {
        bool isSelfFunction = sites.inlineCallSites.isEmpty();
        if (isSelfFunction) {
            sites.inlineCallSites.append(0);
            continue;
        }
        sites.inlineCallSites.append(sites.addSite({ .origin = { frame.callPosition, frame.parent }, .constructs = frame.isConstruction, .isTailCall = frame.isTailCall }));
    }
    sites.numberOfSitesWithoutCode = sites.positions.size();

    size_t numberOfPositions = sites.positions.size() + 1;
    if (numberOfPositions > build().limit(LimitID::SitesPerFunction)) {
        failAtLimit(LimitID::SitesPerFunction, numberOfPositions, subject);
        return false;
    }
    return true;
}

void CodeGenerator::createBlocks()
{
    m_out.initialize(m_heaps);
    LBasicBlock root = m_out.newBlock();
    RELEASE_ASSERT(!root->index());
    m_blocks.fill(nullptr, function().blocks.size());
    for (BlockID block : analyses().cfg().reversePostOrder())
        m_blocks[block.index()] = m_out.newBlock();
    m_out.appendTo(root);
}

void CodeGenerator::createPhis()
{
    for (BlockID id : analyses().cfg().reversePostOrder()) {
        if (id == entryBlock)
            continue;
        LBasicBlock block = blockFor(id);
        for (ValueID parameter : function().at(id).parameters) {
            Representation representation = representationOf(parameter);
            if (!hasRegister(representation))
                continue;
            LValue& lowered = m_values[parameter.index()];
            RELEASE_ASSERT(!lowered);
            lowered = block->appendNew<B3::Value>(m_proc, B3::Phi, b3TypeOf(representation), b3OriginOf(parameter));
            publishKnownProperties(parameter, lowered);
        }
    }
}

void CodeGenerator::generateBlock(BlockID id)
{
    const Block& block = function().at(id);
    m_out.appendTo(blockFor(id));
    setOrigin(m_out, { });
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateParameters(id, block);

    std::span<const ValueID> values = block.values.span();
    if (ValueID owned = valueOwnedByTerminator(block.terminator)) {
        RELEASE_ASSERT(!values.empty() && values.back() == owned);
        values = values.first(values.size() - 1);
    }
    for (ValueID value : values) {
        generateValue(id, block, value);
        if (m_hasFailed)
            return;
        if (typeOf(value).isNever()) {
            if (currentBlockIsOpen()) {
                if (m_validates)
                    emitter<RuntimeValidationEmitter>().reportUnreachable(value);
                m_out.unreachable();
            }
            return;
        }
    }
    generateTerminator(id, block);
}

void CodeGenerator::generateValue(BlockID blockID, const Block& block, ValueID id)
{
    // The one user of a deferred closure is a store that writes nothing: the slot already names the function.
    if (isDeferredClosure(function(), id))
        return;
    ValueContext context(blockID, block, id, function().at(id), analyses().effectsOf(id));
    generate(context);
}

void CodeGenerator::generate(const ValueContext& context)
{
    setOrigin(m_out, context.id());
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateOperands(context);

    auto begin = m_heaps.markDecorations();
    size_t numberOfEnclosingExemptions = m_exemptStores.size();
    dispatch(context);
    checkInlineStores(context, begin);
    m_exemptStores.shrink(numberOfEnclosingExemptions);

    if (context.isBranchCondition() || context.returnCall()) {
        // The terminator that owns this value declares none of its stores.
        m_exemptStores.append({ begin, m_heaps.markDecorations() });
        return;
    }
    if (m_hasFailed || typeOf(context.id()).isNever())
        return;

    size_t index = context.id().index();
    RELEASE_ASSERT(context.representation() == Representation::Void || m_values[index] || m_resultRanges[index]);
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateResult(context);
}

void CodeGenerator::generateTerminator(BlockID blockID, const Block& block)
{
    setOrigin(m_out, { });
    TerminatorContext context(blockID, block, analyses().effectsOfTerminator(blockID));
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateExit(context);

    auto begin = m_heaps.markDecorations();
    dispatch(context);
    checkInlineStores(context, begin);
    m_exemptStores.shrink(0);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateBranchCondition(const TerminatorContext& terminator, ValueID condition, BranchTargets targets)
{
    ValueContext context(terminator.block(), function().at(terminator.block()), condition, function().at(condition), analyses().effectsOf(condition));
    context.m_branchTargets = targets;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), function().at(terminator.block()), call, function().at(call), analyses().effectsOf(call));
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::finishProcedure()
{
    m_heaps.computeRangesAndDecorateInstructions();
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
}

LBasicBlock CodeGenerator::blockFor(BlockID id) const
{
    LBasicBlock block = m_blocks[id.index()];
    RELEASE_ASSERT(block);
    return block;
}

B3::FrequentedBlock CodeGenerator::edgeTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Target& target = terminator->targets[targetIndex];
    Edge edge { terminator.block(), targetIndex };
    B3::FrequencyClass frequency = b3FrequencyClassOf(analyses().blockFrequencies().classOf(edge));
    if (build().recordedOptions().has(TestingSwitch::IgnoreBranchHints))
        frequency = B3::FrequencyClass::Normal;

    LBasicBlock block = blockFor(target.block);
    if (!hasParameterWithRegister(function(), target.block))
        return { block, frequency };
    if (terminator->targets.size() == 1) {
        passArguments(edge, target, declaredResults);
        return { block, frequency };
    }

    LBasicBlock here = m_out.m_block;
    LBasicBlock edgeBlock = m_out.newBlock();
    m_out.appendTo(edgeBlock);
    passArguments(edge, target, declaredResults);
    m_out.jump(block);
    m_out.appendTo(here);
    return { edgeBlock, frequency };
}

void CodeGenerator::jumpTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Target& target = terminator->targets[targetIndex];
    passArguments(Edge { terminator.block(), targetIndex }, target, declaredResults);
    m_out.jump(blockFor(target.block));
}

void CodeGenerator::passArguments(Edge edge, const Target& target, std::span<const Operand> declaredResults)
{
    auto& parameters = function().at(target.block).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (hasRegister(representationOf(parameters[index])))
            passArgument(parameters[index], takeIncomingOperand(*this, edge, index, declaredResults));
    }
    RELEASE_ASSERT(declaredResults.empty());
}

void CodeGenerator::passHandlerArguments(const Target& handler)
{
    auto& parameters = function().at(handler.block).parameters;
    RELEASE_ASSERT(handler.arguments.size() == parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (hasRegister(representationOf(parameters[index])))
            passArgument(parameters[index], get(handler.arguments[index]));
    }
}

void CodeGenerator::passArgument(ValueID parameter, Operand argument)
{
    RELEASE_ASSERT(argument.representation == representationOf(parameter));
    LValue phi = m_values[parameter.index()];
    RELEASE_ASSERT(phi && phi->opcode() == B3::Phi);
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), argument.value, phi);
}

Operand CodeGenerator::get(ValueID id) const
{
    Representation representation = representationOf(id);
    if (representation == Representation::Void)
        return { };
    LValue lowered = m_values[id.index()];
    RELEASE_ASSERT(lowered);
    return { representation, lowered };
}

void CodeGenerator::defineValue(ValueID id, Operand operand)
{
    RELEASE_ASSERT(operand.representation == representationOf(id));
    if (operand.representation == Representation::Void)
        return;
    LValue& lowered = m_values[id.index()];
    RELEASE_ASSERT(!lowered && operand.value && operand.value->type() == b3TypeOf(operand.representation));
    lowered = operand.value;
    publishKnownProperties(id, lowered);
}

void CodeGenerator::define(const ValueContext& value, Operand result)
{
    RELEASE_ASSERT(!value.isBranchCondition());
    defineValue(value.id(), result);
}

void CodeGenerator::defineTest(const ValueContext& value, Condition result)
{
    if (shouldDropGuard(value))
        result = Condition(m_out.constInt32(1));
    if (!value.isBranchCondition()) {
        define(value, result);
        return;
    }
    LValue& lowered = m_values[value.id().index()];
    RELEASE_ASSERT(!lowered);
    lowered = result.value();
    m_out.branch(result.value(), value.branchTargets());
}

void CodeGenerator::defineResults(const ValueContext& value, std::span<const Operand> results)
{
    std::optional<ResultRange>& range = m_resultRanges[value.id().index()];
    RELEASE_ASSERT(!range);
    range = ResultRange { static_cast<uint32_t>(m_results.size()), static_cast<uint32_t>(results.size()) };
    m_results.append(results);
}

Operand CodeGenerator::resultAt(ValueID producer, uint32_t index) const
{
    const std::optional<ResultRange>& range = m_resultRanges[producer.index()];
    RELEASE_ASSERT(range && index < range->count);
    return m_results[range->first + index];
}

void CodeGenerator::defineParameter(ValueID parameter, Operand operand)
{
    RELEASE_ASSERT(function().at(parameter).opcode == Opcode::Parameter);
    defineValue(parameter, operand);
}

Subject CodeGenerator::subjectOf(const OperationContext& context) const
{
    return { .function = m_inputs.id, .block = context.block(), .value = context.valueID(), .location = function().locationOf(context.origin()) };
}

void CodeGenerator::fail(const OperationContext& context, InternalErrorKind kind)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    Subject subject = subjectOf(context);
    build().diagnostics().add(InternalError { .kind = kind, .function = subject.function, .block = subject.block, .value = subject.value, .location = subject.location });
}

void CodeGenerator::failAtLimit(const OperationContext& context, LimitID limit, uint64_t needed)
{
    failAtLimit(limit, needed, subjectOf(context));
}

void CodeGenerator::failAtLimit(LimitID limit, uint64_t needed, const Subject& subject)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    build().reportLimitExceeded(limit, needed, subject);
}

bool CodeGenerator::shouldDropGuard(const ValueContext& value)
{
    bool isGuard = value.opcode() == Opcode::TypeTest || familyOf(value.opcode()) == OperationFamily::Guard;
    if (!m_droppedGuard || !isGuard || m_droppedGuard->opcode != value.opcode())
        return false;
    return m_numberOfGuardsSeen++ == m_droppedGuard->ordinal;
}

bool CodeGenerator::currentBlockIsOpen() const
{
    LBasicBlock block = m_out.m_block;
    return !block->size() || !block->last()->effects().terminal;
}

ValueID valueOwnedByTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::TestAndBranch:
        return terminator.conditionOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CodeGenerator::InitializationScope::InitializationScope(CodeGenerator& generator)
    : m_generator(generator)
    , m_begin(generator.m_heaps.markDecorations())
{
    ++generator.m_initializationDepth;
}

CodeGenerator::InitializationScope::~InitializationScope()
{
    --m_generator.m_initializationDepth;
    m_generator.m_exemptStores.append({ m_begin, m_generator.m_heaps.markDecorations() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
