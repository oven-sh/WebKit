/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AwaitedYield {
    BlockID block;
    uint32_t awaitIndex { 0 };
    uint32_t yieldIndex { 0 };
};

class CoroutineBodyBuilder {
    WTF_MAKE_NONCOPYABLE(CoroutineBodyBuilder);
public:
    CoroutineBodyBuilder(const AnalysisContext&, TypeArena&, const CreatorRecord::Coroutine&, Function&& unsplit);

    Function run(TypeID driverType);

private:
    void prepareHeader(TypeID driverType);
    void markRareModeArms();
    void mergeAwaitsIntoYields();
    void moveProofsOfSentValueBehindMode(BlockID, ValueID yield);
    void replaceYields();
    void replaceSteps();
    void replaceAwaits();
    void expandTypedAwait(ProgramPoint);
    bool headMayChangePrototype(ProgramPoint initialYield);
    bool headHasNoEffect(ProgramPoint initialYield);
    void expandInitialYield();
    void replaceReturns();

    void rewriteInPlace(BlockID, ValueID, Opcode, std::initializer_list<ValueID> operands);
    void rewriteInPlace(BlockID, ValueID, Opcode, std::initializer_list<ValueID> operands, const ReplacedEffects&);
    ReplacedEffects replacedEffectsOf(std::initializer_list<ValueID>) const;
    void checkSuccessor(BlockID, ValueID, const ReplacedEffects&);
    bool canKeepType(const Value& successor, TypeID storedType);
    bool canExpect(const Value& successor, TypeID expected);

    const AnalysisContext& m_context;
    TypeArena& m_types;
    const CreatorRecord::Coroutine& m_record;
    Function m_body;
    FunctionAnalyses m_analyses;
    IRBuilder m_builder;
    ValueID m_driver;
};

} // anonymous namespace

static std::optional<ProgramPoint> findInitialYield(const Function& function)
{
    for (uint32_t block = 0; block < function.blocks.size(); ++block) {
        auto& values = function.at(BlockID(block)).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::InitialYield)
                return ProgramPoint { BlockID(block), index };
        }
    }
    return std::nullopt;
}

static ValueID appendResultAt(IRBuilder& builder, ValueID producer, YieldResult result)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { producer });
    read.setIndex(static_cast<uint32_t>(result));
    return builder.append(WTF::move(read));
}

static void markModeArmsRare(Function& function, const UseLists& useLists, ValueID yield)
{
    for (const Use& read : useLists.usesOf(yield)) {
        if (read.kind != Use::Kind::Operand)
            continue;
        const Value& value = function.at(read.user);
        if (value.opcode != Opcode::ResultAt || value.index() != static_cast<uint32_t>(YieldResult::Mode))
            continue;
        for (const Use& use : useLists.usesOf(read.user)) {
            const Terminator& terminator = function.at(use.block).terminator;
            if (use.kind != Use::Kind::TerminatorOperand || terminator.kind != TerminatorKind::Switch)
                continue;
            BlockID otherwise = terminator.otherwise().block;
            for (const Target& target : terminator.caseTargets()) {
                if (target.block != otherwise)
                    function.edit(target.block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
            }
        }
    }
}

static std::optional<uint32_t> findYieldToMerge(const Function& function, const UseLists& useLists, ProgramPoint point)
{
    const BlockValues& values = function.at(point.block).values;
    const Value& await = function.at(values[point.index]);
    if (await.opcode != Opcode::Await || await.takesExtraTurn())
        return std::nullopt;

    for (uint32_t index = point.index; index + 1 < values.size(); ++index) {
        std::optional<Use> use = useLists.singleUse(values[index]);
        if (!use || use->kind != Use::Kind::Operand || use->user != values[index + 1])
            return std::nullopt;
        Opcode user = function.at(use->user).opcode;
        if (user == Opcode::Yield && use->operandIndex == OperandIndex::Yield::value)
            return index + 1;
        if (!isCastWithoutTest(user))
            return std::nullopt;
    }
    return std::nullopt;
}

static Vector<AwaitedYield> findYieldsToMerge(const Function& function, const UseLists& useLists)
{
    Vector<AwaitedYield> merges;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            std::optional<uint32_t> yieldIndex = findYieldToMerge(function, useLists, ProgramPoint { block, index });
            if (!yieldIndex)
                continue;
            merges.append(AwaitedYield { block, index, *yieldIndex });
            index = *yieldIndex;
        }
    }
    return merges;
}

CoroutineBodyBuilder::CoroutineBodyBuilder(const AnalysisContext& context, TypeArena& types, const CreatorRecord::Coroutine& record, Function&& unsplit)
    : m_context(context)
    , m_types(types)
    , m_record(record)
    , m_body(WTF::move(unsplit))
    , m_analyses(context, m_body)
    , m_builder(context, m_body, types, &m_analyses)
{
}

Function CoroutineBodyBuilder::run(TypeID driverType)
{
    prepareHeader(driverType);
    m_builder.setOrigin(Origin { m_body.position, 0 });
    m_driver = m_builder.addParameter(entryBlock, driverType);

    switch (m_record.kind) {
    case CoroutineKind::Generator:
        markRareModeArms();
        replaceYields();
        expandInitialYield();
        break;
    case CoroutineKind::AsyncGenerator:
        markRareModeArms();
        mergeAwaitsIntoYields();
        replaceYields();
        replaceSteps();
        replaceAwaits();
        expandInitialYield();
        break;
    case CoroutineKind::AsyncFunction:
        replaceSteps();
        replaceAwaits();
        break;
    }

    replaceReturns();
    m_builder.setHandler(std::nullopt);
    return WTF::move(m_body);
}

void CoroutineBodyBuilder::prepareHeader(TypeID driverType)
{
    unsigned sourceParameters = m_body.numberOfEntryParameters();

    if (m_body.runsOnlyBehindFailedTest())
        m_body.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    m_body.role = FunctionRole::CoroutineBody;
    m_body.creator = m_record;
    m_body.text = { };

    // The driver has to be the last entry parameter, so a hidden rest in front of it becomes an ordinary parameter.
    if (std::optional<unsigned> hiddenRest = m_body.hiddenRestParameterIndex()) {
        TypingContext typing { m_context.program, m_body, m_types };
        TypeID restType = declaredEntryTypeOf(typing, m_body, *hiddenRest);
        m_body.flags.remove(FunctionFlags::HasHiddenRest);
        m_body.parameters.append(DeclaredType { .type = restType });
    }
    m_body.flags.remove(FunctionFlags::HasRestParameter);
    m_body.parameters.append(DeclaredType { .type = driverType });
    RELEASE_ASSERT(m_record.promiseParameter == sourceParameters && m_body.numberOfEntryParameters() == sourceParameters + 1);

    m_body.results = Vector<DeclaredType> { DeclaredType { .type = TypeTable::anything } };

    Vector<ParameterOrigin> parameterMap;
    parameterMap.reserveInitialCapacity(sourceParameters + 1);
    for (uint32_t parameter = 0; parameter < sourceParameters; ++parameter)
        parameterMap.append(ParameterOrigin { parameter });
    parameterMap.append(ParameterOrigin { std::nullopt });
    m_body.derivation = Derivation { .source = m_record.ramp, .parameterMap = WTF::move(parameterMap) };

    m_body.capabilities = initialCapabilitiesOf(m_body);
}

ReplacedEffects CoroutineBodyBuilder::replacedEffectsOf(std::initializer_list<ValueID> replacedValues) const
{
    ReplacedEffects replaced;
    for (ValueID id : replacedValues)
        replaced.add(m_analyses.effectContext(), m_body, m_body.at(id));
    return replaced;
}

void CoroutineBodyBuilder::checkSuccessor(BlockID block, ValueID id, const ReplacedEffects& replaced)
{
    checkReplacement(m_analyses.effectContext(), m_body, { m_record.body, block, id }, m_body.at(id), replaced, m_context.build.diagnostics());
}

bool CoroutineBodyBuilder::canKeepType(const Value& successor, TypeID storedType)
{
    const Type& successorType = m_types.at(resultTypeOf(m_context.program, m_body, successor, m_types));
    return successorType.isSubsetOf(m_types.at(storedType), m_context.program);
}

// An expected type is set only where it says more than the type.
bool CoroutineBodyBuilder::canExpect(const Value& successor, TypeID expected)
{
    TypeID type = resultTypeOf(m_context.program, m_body, successor, m_types);
    return expected && expected != type && m_types.at(expected).isSubsetOf(m_types.at(type), m_context.program);
}

void CoroutineBodyBuilder::rewriteInPlace(BlockID block, ValueID id, Opcode opcode, std::initializer_list<ValueID> operands)
{
    rewriteInPlace(block, id, opcode, operands, replacedEffectsOf({ id }));
}

void CoroutineBodyBuilder::rewriteInPlace(BlockID block, ValueID id, Opcode opcode, std::initializer_list<ValueID> operands, const ReplacedEffects& replaced)
{
    Value& value = m_body.edit(id);
    value.become(opcode);
    uint32_t operandIndex = 0;
    for (ValueID operand : operands)
        value.setOperand(operandIndex++, operand);
    m_analyses.didChangeValues();
    checkSuccessor(block, id, replaced);
}

void CoroutineBodyBuilder::markRareModeArms()
{
    const UseLists& useLists = m_analyses.useLists();
    for (const Block& block : m_body.blocks) {
        for (ValueID id : block.values) {
            if (m_body.at(id).opcode == Opcode::Yield)
                markModeArmsRare(m_body, useLists, id);
        }
    }
    m_analyses.invalidate(FunctionAnalysis::Frequencies);
}

// What the merged await rejects with arrives at the yield as a value that is sent in to be thrown.
void CoroutineBodyBuilder::moveProofsOfSentValueBehindMode(BlockID block, ValueID yield)
{
    const Terminator& terminator = m_body.at(block).terminator;
    if (terminator.kind != TerminatorKind::Switch)
        return;

    const BlockValues& values = m_body.at(block).values;
    ValueID sent;
    Vector<ValueID, 4> proofs;
    BitVector proofIndexes(values.size());
    for (uint32_t index = 0; index < values.size(); ++index) {
        const Value& value = m_body.at(values[index]);
        if (value.opcode == Opcode::ResultAt && value.operand(0) == yield && value.index() == static_cast<uint32_t>(YieldResult::Sent)) {
            if (sent)
                return;
            sent = values[index];
            continue;
        }
        if (!sent || (!isCastWithoutTest(value.opcode) && value.opcode != Opcode::ValidateType))
            continue;
        if (value.valueOperand() != sent && !proofs.contains(value.valueOperand()))
            continue;
        proofs.append(values[index]);
        proofIndexes.quickSet(index);
    }
    if (proofs.isEmpty())
        return;

    BlockID throwingArm;
    Vector<BlockID, 2> otherArms;
    auto addArm = [&](BlockID arm) {
        if (!otherArms.contains(arm))
            otherArms.append(arm);
    };
    std::span<const Target> caseTargets = terminator.caseTargets();
    for (size_t index = 0; index < caseTargets.size(); ++index) {
        if (terminator.cases()[index] == static_cast<int64_t>(ResumeMode::Throw))
            throwingArm = caseTargets[index].block;
        else
            addArm(caseTargets[index].block);
    }
    addArm(terminator.otherwise().block);
    if (!throwingArm || otherArms.contains(throwingArm))
        return;
    const CFG& cfg = m_analyses.cfg();
    if (!cfg.singleIncomingEdge(throwingArm))
        return;
    Vector<Edge, 2> edgesIntoOtherArms;
    for (BlockID arm : otherArms) {
        std::optional<Edge> edge = cfg.singleIncomingEdge(arm);
        if (!edge)
            return;
        edgesIntoOtherArms.append(*edge);
    }

    struct UseInArm {
        Use use;
        uint32_t proof;
        // None for the arm that throws.
        std::optional<uint32_t> arm;
    };
    Vector<UseInArm, 8> uses;
    Dominators& dominators = m_analyses.dominators();
    const UseLists& useLists = m_analyses.useLists();
    for (uint32_t proof = 0; proof < proofs.size(); ++proof) {
        for (const Use& use : useLists.usesOf(proofs[proof])) {
            if (use.kind == Use::Kind::Operand && proofs.contains(use.user))
                continue;
            if (use.kind == Use::Kind::BlockSubject)
                return;
            if (dominators.dominates(throwingArm, use.block)) {
                uses.append({ use, proof, std::nullopt });
                continue;
            }
            size_t arm = otherArms.findIf([&](BlockID candidate) {
                return dominators.dominates(candidate, use.block);
            });
            // Somebody reads it where several arms meet, or in front of the switch.
            if (arm == notFound)
                return;
            uses.append({ use, proof, static_cast<uint32_t>(arm) });
        }
    }

    // A value that is put into a block is available neither at the head of the block, where the arguments of its handler are read, nor in the handler.
    for (size_t index = 0; index < otherArms.size(); ++index) {
        if (m_body.at(otherArms[index]).handler)
            otherArms[index] = m_builder.splitEdge(edgesIntoOtherArms[index]);
    }

    Vector<Vector<ValueID, 4>, 2> copies;
    for (BlockID arm : otherArms) {
        Vector<ValueID, 4> copiesInArm;
        for (ValueID proof : proofs) {
            Value copy = m_body.at(proof);
            size_t operand = proofs.find(copy.valueOperand());
            if (operand != notFound)
                copy.setValueOperand(copiesInArm[operand]);
            m_builder.setOrigin(copy.origin());
            copiesInArm.append(m_builder.insertBefore(ProgramPoint::atHeadOf(arm), WTF::move(copy)));
        }
        copies.append(WTF::move(copiesInArm));
    }
    for (const UseInArm& use : uses)
        replaceUse(m_body, use.use, use.arm ? copies[*use.arm][use.proof] : sent);
    m_builder.applyInsertions();
    m_builder.eraseValues(block, proofIndexes);
    m_analyses.didChangeValues();
}

void CoroutineBodyBuilder::mergeAwaitsIntoYields()
{
    Vector<AwaitedYield> merges = findYieldsToMerge(m_body, m_analyses.useLists());
    Vector<std::pair<BlockID, ValueID>, 8> mergedYields;
    for (size_t next = 0; next < merges.size();) {
        BlockID block = merges[next].block;
        const BlockValues& values = m_body.at(block).values;
        BitVector mergedIndexes(values.size());
        for (; next < merges.size() && merges[next].block == block; ++next) {
            const AwaitedYield& merge = merges[next];
            ValueID await = values[merge.awaitIndex];
            ValueID yield = values[merge.yieldIndex];
            rewriteInPlace(block, yield, Opcode::AsyncGeneratorYield, { m_driver, m_body.at(await).valueOperand() }, replacedEffectsOf({ await, yield }));
            for (uint32_t index = merge.awaitIndex; index < merge.yieldIndex; ++index)
                mergedIndexes.quickSet(index);
            mergedYields.append({ block, yield });
        }
        m_builder.eraseValues(block, mergedIndexes);
    }
    for (auto [block, yield] : mergedYields)
        moveProofsOfSentValueBehindMode(block, yield);
}

void CoroutineBodyBuilder::replaceYields()
{
    for (uint32_t blockIndex = 0; blockIndex < m_body.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : m_body.at(block).values) {
            const Value& value = m_body.at(id);
            if (value.opcode == Opcode::Yield)
                rewriteInPlace(block, id, Opcode::YieldBySavingFrame, { m_driver, value.valueOperand() });
        }
    }
}

// A step suspends this frame where the generator does not answer at once, and whoever finishes the step resumes the frame through its driver.
void CoroutineBodyBuilder::replaceSteps()
{
    for (const Block& block : m_body.blocks) {
        for (ValueID id : block.values) {
            Value& value = m_body.edit(id);
            if (value.opcode == Opcode::StepAsyncGenerator)
                value.setDriverOperand(m_driver);
        }
    }
    m_analyses.didChangeValues();
}

void CoroutineBodyBuilder::replaceAwaits()
{
    Vector<ProgramPoint> typedAwaits;
    for (uint32_t blockIndex = 0; blockIndex < m_body.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = m_body.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = m_body.at(values[index]);
            if (value.opcode != Opcode::Await)
                continue;
            ValueID awaited = value.valueOperand();
            bool takesExtraTurn = value.takesExtraTurn();
            if (canKeepType(IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, m_driver }), value.type)) {
                rewriteInPlace(block, values[index], Opcode::AwaitBySavingFrame, { awaited, m_driver });
                m_body.edit(values[index], ValueEdit::Definition).setTakesExtraTurn(takesExtraTurn);
            } else
                typedAwaits.append(ProgramPoint { block, index });
        }
    }

    while (!typedAwaits.isEmpty())
        expandTypedAwait(typedAwaits.takeLast());
}

void CoroutineBodyBuilder::expandTypedAwait(ProgramPoint point)
{
    ValueID id = m_body.at(point.block).values[point.index];
    ValueID awaited = m_body.at(id).valueOperand();
    TypeID type = m_body.at(id).type;
    TypeID expected = m_body.at(id).expected();
    bool takesExtraTurn = m_body.at(id).takesExtraTurn();
    ReplacedEffects replaced = replacedEffectsOf({ id });

    IRBuilder::Expansion expansion = m_builder.beginExpansion(point);
    Value successor = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, m_driver });
    successor.setTakesExtraTurn(takesExtraTurn);
    if (canExpect(successor, expected))
        successor.setExpected(expected);
    ValueID result = m_builder.append(WTF::move(successor));
    checkSuccessor(point.block, result, replaced);
    m_builder.endExpansion(expansion, convertTo(m_builder, result, DeclaredType { .type = type }));
}

// The ramp allocates the generator, which reads the property `prototype` of the function, and only then does the head of the body evaluate the parameters.
bool CoroutineBodyBuilder::headMayChangePrototype(ProgramPoint initialYield)
{
    if (initialYield.block != entryBlock)
        return true;
    return std::ranges::any_of(m_body.at(entryBlock).values.span().first(initialYield.index), [&](ValueID id) {
        Effects effects = m_analyses.effectsOf(id).onAnyEdge();
        return effects.mayCallUnknownCode || effects.writes.contains(AbstractHeap::NamedProperties);
    });
}

// Nothing tells when such a head has run, so the call that starts the generator may be left to its first step.
bool CoroutineBodyBuilder::headHasNoEffect(ProgramPoint initialYield)
{
    if (initialYield.block != entryBlock)
        return false;
    return std::ranges::all_of(m_body.at(entryBlock).values.span().first(initialYield.index), [&](ValueID id) {
        return m_analyses.isUnobservable(id);
    });
}

void CoroutineBodyBuilder::expandInitialYield()
{
    std::optional<ProgramPoint> point = findInitialYield(m_body);
    if (!point)
        return;

    ReplacedEffects replaced = replacedEffectsOf({ m_body.at(point->block).values[point->index] });
    bool takesPrototypeAgain = headMayChangePrototype(*point);

    IRBuilder::Expansion expansion = m_builder.beginExpansion(*point);
    RELEASE_ASSERT(expansion.results.isEmpty());
    if (m_record.kind == CoroutineKind::Generator && headHasNoEffect(*point)) {
        // Whoever takes the first step by calling the body has marked the generator as running, and the body does not suspend here.
        IRBuilder::BranchBlocks blocks = m_builder.emitBranch(m_builder.append(Opcode::IsGeneratorRunning, { m_driver }));
        m_builder.emitJump(expansion.continuation);
        m_builder.moveToEndOf(blocks.notTaken);
    }
    if (takesPrototypeAgain) {
        std::optional<unsigned> calleeParameter = m_body.calleeParameterIndex();
        RELEASE_ASSERT(calleeParameter);
        m_builder.append(Opcode::TakeGeneratorPrototype, { m_driver, m_body.at(entryBlock).parameters[*calleeParameter] });
    }
    ValueID yield = m_builder.append(Opcode::YieldBySavingFrame, { m_driver, m_builder.appendUndefined() });
    checkSuccessor(point->block, yield, replaced);
    ValueID sent = appendResultAt(m_builder, yield, YieldResult::Sent);
    ValueID mode = appendResultAt(m_builder, yield, YieldResult::Mode);

    BlockID throwArm = m_builder.addBlock(BlockFrequency::Rare);
    BlockID returnArm = m_builder.addBlock(BlockFrequency::Rare);
    Vector<std::pair<int64_t, Target>> cases;
    cases.append(std::pair<int64_t, Target> { static_cast<int64_t>(ResumeMode::Throw), Target { throwArm, { }, { } } });
    cases.append(std::pair<int64_t, Target> { static_cast<int64_t>(ResumeMode::Return), Target { returnArm, { }, { } } });
    m_builder.emitSwitch(mode, WTF::move(cases), Target { expansion.continuation, { }, { } });

    m_builder.moveToEndOf(throwArm);
    m_builder.emitThrow(sent);

    m_builder.moveToEndOf(returnArm);
    m_builder.emitReturn(sent);
}

void CoroutineBodyBuilder::replaceReturns()
{
    bool isAsyncFunction = m_record.kind == CoroutineKind::AsyncFunction;
    for (uint32_t blockIndex = 0; blockIndex < m_body.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = m_body.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        if (terminator.kind != TerminatorKind::Return)
            continue;

        ValueID value = terminator.valueOperand();
        if (value && !isAsyncFunction)
            continue;

        m_builder.setOrigin(terminator.origin());
        terminator = Terminator();
        m_builder.moveToEndOf(block);
        if (!value)
            value = m_builder.appendUndefined();
        if (isAsyncFunction)
            value = m_builder.append(Opcode::ReturnAsyncResult, { m_driver, value });
        m_builder.emitReturn(value);
    }
    m_analyses.didChangeValues();
}

Function buildCoroutineBody(const AnalysisContext& context, TypeArena& types, const CoroutineTypes& coroutineTypes, Function&& unsplit, const CoroutinePlan& plan)
{
    CoroutineBodyBuilder builder(context, types, plan.record, WTF::move(unsplit));
    return builder.run(coroutineTypes.driverParameterOf(plan.record.kind));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
