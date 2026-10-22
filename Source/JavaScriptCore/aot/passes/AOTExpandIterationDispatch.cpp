/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCoroutines.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFuseGeneratorLoops.h"
#include "AOTRegionCloner.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static Value makeNamesTest(const IterationArm& arm, PatternRead patternRead, ValueID subject)
{
    if (arm.form == IterationForm::Generator)
        return IRBuilder::make(Opcode::IsUnmodifiedGenerator, { subject });
    if (arm.form == IterationForm::AsyncGenerator)
        return IRBuilder::make(Opcode::IsUnmodifiedAsyncGenerator, { subject });
    OptionSet<PristineAspects> aspects { PristineAspects::Names };
    if (patternRead == PatternRead::ByIndex)
        aspects.add(PristineAspects::Storage);
    return makePristineTest(subject, arm.builtinClass, aspects);
}

static bool provesNotDone(const Function& function, Edge edge, ValueID resume)
{
    constexpr uint32_t notTakenTargetIndex = 1;
    if (edge.isHandlerEdge() || edge.targetIndex != notTakenTargetIndex)
        return false;
    const Terminator& terminator = function.at(edge.from).terminator;
    if (!terminator.isConditionalBranch())
        return false;
    const Value& condition = function.at(terminator.conditionOperand());
    return condition.opcode == Opcode::ResultAt && condition.resultsOperand() == resume && condition.index() == static_cast<uint32_t>(IteratorResumeResult::Done);
}

// An arm that awaits each value stands for the wrapper around a synchronous iterator, and closing the wrapper takes a turn of the job queue.
static bool closingHasNoEffect(const IterationArm& arm)
{
    return closingHasNoEffect(arm.form) && !arm.awaitsEachValue;
}

static bool isExpandedWithoutTest(const IterationPlan& plan)
{
    return plan.arms.size() == 1 && plan.armSources.isEmpty() && plan.arms.first().needsNoTest() && closingHasNoEffect(plan.arms.first());
}

static bool awaitsCallOnString(FunctionPassContext& pass, const Iteration& iteration)
{
    static_assert(!OperandIndex::IteratorOpen::iterable, "findSingleUseCastChain starts at operand 0.");
    const Function& function = pass.function;
    Vector<ValueID, 2> chain = findSingleUseCastChain(function, pass.analyses.useLists(), iteration.open);
    if (chain.isEmpty())
        return false;
    const Value& call = function.at(chain.first());
    if (call.opcode != Opcode::Call || !call.calleeKey() || !call.thisValueOperand())
        return false;
    return pass.program.typeOf(function, call.thisValueOperand()).isAnyString();
}

static BitVector blockSetOf(const Function& function, std::span<const BlockID> blocks)
{
    BitVector set(function.blocks.size());
    for (BlockID block : blocks)
        set.quickSet(block.index());
    return set;
}

static bool isLastUseOfOpen(FunctionAnalyses& analyses, ValueID open, const BitVector& liveRegion, ValueLocation close)
{
    const UseLists& useLists = analyses.useLists();
    bool isLiveAtTail = std::ranges::any_of(analyses.cfg().successors(close.block), [&](BlockID successor) {
        return liveRegion.quickGet(successor.index());
    });
    return !isLiveAtTail && std::ranges::none_of(useLists.usesOf(open), [&](const Use& use) {
        return use.kind == Use::Kind::Operand && use.block == close.block && useLists.locationOf(use.user).index > close.index;
    });
}

static bool isFollowedByIterationOperation(const Function& function, ValueLocation location)
{
    return std::ranges::any_of(function.at(location.block).values.span().subspan(location.index + 1), [&](ValueID id) {
        return removedByOf(function.at(id).opcode) == PassID::ExpandIteration;
    });
}

namespace {

enum class OperationsAfterDone : uint8_t { Unknown, None };

struct ArmCopy {
    BlockID entry;
    Vector<ValueID, 2> resumes;
    Vector<ValueID, 2> closes;
    Vector<BlockID, 1> cleanupBlocks;
    bool isReadAtOnce { false };
};

struct OperationSite {
    ProgramPoint point;
    ValueID value;
    unsigned copy;
    Vector<ValueID, 2> resultReads;
};

// One iteration of a round. Every step is taken for all iterations of the round before the next step is: the steps that only read the function share what the analyses have computed.
class ArmDispatcher {
    WTF_MAKE_NONCOPYABLE(ArmDispatcher);
public:
    ArmDispatcher(FunctionPassContext&, IRBuilder&, SSARepair&, const Iteration&);

    BlockID head() const { return m_head; }
    std::span<const BlockID> region() const { return m_region.span(); }
    const IterationPlan& plan() const { return m_plan; }

    void planArms();
    void splitAfterLastCloses();
    void chooseForms();
    void emitOpens();
    void addVariablesOfCopies();
    void collectPatternsReadAtOnce(Vector<PatternInOneRead>&);
    void collectOperationSites();
    void expandOperations();
    void finishCleanupBlocks(Vector<BlockID>& cleanupBlocksToDrop);
    void eraseOpen();
    void report();

private:
    void findCutsAfterLastCloses(const BitVector& liveRegion);
    OperationsAfterDone findOperationsAfterDone(const BitVector& liveRegion);
    BitVector findBlocksEnteredAfterDone(ValueID resume, BlockID stepBlock, const BitVector& liveRegion);
    void cloneRegionPerArm();
    void reopenHead();

    void emitTestsAndOpens();
    ValueID emitClassArm(const IterationArm&, ValueID value, BlockID protocolEntry);
    void finishFailingEdge(ValueID rest, BlockID protocolEntry);
    void emitProtocolArm(ValueID iterable);

    void emitOpensByArmSource();
    void defineAtArmSources(std::optional<StateVariable>& chosenArm, Vector<std::optional<StateVariable>, 2>& subjects);
    void emitClassArmWithoutTest(const IterationArm&, ValueID subject);

    void connectArms();
    void joinArmsByForm();
    void appendJoinArguments(Arguments&, const ArmState&, bool isOwnEdge);

    void expandOperation(const OperationSite&);
    void skipStepWhenDone();
    void skipCloseWhenDone();
    void defineDoneAfterStep(const IRBuilder::Expansion&);
    void emitOperation(const Value& original, ArmState&);
    void emitOperationByForm(const Value& original);
    void emitStepOfWrappedIterator(ArmState&);
    void emitThrowOrReturnOfWrappedIterator(const Value& original, ArmState&);
    void emitCloseOfWrappedIterator();

    bool closingHasNoEffectIn(unsigned copyIndex) const;

    ValueID addParameterLike(BlockID, ValueID model);
    ValueID appendUnspecifiedLike(ValueID model);

    FunctionPassContext& m_pass;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder& m_builder;
    SSARepair& m_repair;
    Iteration m_iteration;
    IterationExpansion m_expansion;
    IterationPlan m_plan;
    BlockID m_head;
    uint32_t m_openIndex { 0 };
    BlockID m_tail;
    Vector<BlockID> m_region;
    OperationsAfterDone m_operationsAfterDone { OperationsAfterDone::Unknown };
    Vector<ProgramPoint, 2> m_cutsAfterLastCloses;
    ArmDispatch m_dispatch { ArmDispatch::SingleArm };
    Vector<PatternRead, 2> m_patternReads;
    Vector<ArmCopy, 2> m_copies;
    Vector<ValueID> m_definitionsInRegion;
    Vector<Vector<ValueID>, 1> m_copiesOfDefinitions;
    Vector<ArmState, 2> m_states;
    Vector<BlockID, 2> m_ends;
    ValueID m_form;
    std::optional<StateVariable> m_done;
    Vector<OperationSite, 8> m_sites;
    const IRBuilder::Expansion* m_cut { nullptr };
};

ArmDispatcher::ArmDispatcher(FunctionPassContext& pass, IRBuilder& builder, SSARepair& repair, const Iteration& iteration)
    : m_pass(pass)
    , m_function(pass.function)
    , m_analyses(pass.analyses)
    , m_builder(builder)
    , m_repair(repair)
    , m_iteration(iteration)
    , m_expansion(pass, builder, repair, iteration.origin)
    , m_region(findLiveRegion(pass, iteration.open))
{
    ValueLocation location = m_analyses.useLists().locationOf(m_iteration.open);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    m_head = location.block;
    m_openIndex = location.index;

    const Target& next = m_function.at(m_head).terminator.next();
    RELEASE_ASSERT(next.arguments.isEmpty());
    m_tail = next.block;
}

void ArmDispatcher::planArms()
{
    m_plan = planIteration(m_pass, m_iteration);
    BitVector liveRegion = blockSetOf(m_function, m_region.span());
    m_operationsAfterDone = findOperationsAfterDone(liveRegion);
    if (m_plan.arms.size() > 1)
        findCutsAfterLastCloses(liveRegion);
}

// A block that was merged with its successor goes on with the code after the iteration, which no copy needs and no limit on copies should count.
void ArmDispatcher::findCutsAfterLastCloses(const BitVector& liveRegion)
{
    const UseLists& useLists = m_analyses.useLists();
    for (ValueID close : m_iteration.closes) {
        ValueLocation location = useLists.locationOf(close);
        if (location.index + 1 < m_function.at(location.block).values.size() && isLastUseOfOpen(m_analyses, m_iteration.open, liveRegion, location))
            m_cutsAfterLastCloses.append(ProgramPoint { location.block, location.index + 1 });
    }
}

void ArmDispatcher::splitAfterLastCloses()
{
    m_builder.setOrigin(m_function.at(m_iteration.open).origin());
    for (ProgramPoint cut : m_cutsAfterLastCloses)
        m_builder.splitBlock(cut);
}

void ArmDispatcher::chooseForms()
{
    bool isDestructuring = m_function.at(m_iteration.open).isDestructuring();
    m_dispatch = chooseArmDispatch(m_pass, m_iteration, m_plan, m_region.span());
    bool hasOneCopyPerArm = m_dispatch != ArmDispatch::ByForm;
    for (const IterationArm& arm : m_plan.arms)
        m_patternReads.append(isDestructuring && hasOneCopyPerArm ? choosePatternRead(m_pass, m_iteration, arm, m_region.span()) : PatternRead::BySteps);
}

void ArmDispatcher::emitOpens()
{
    m_builder.setOrigin(m_function.at(m_iteration.open).origin());
    cloneRegionPerArm();
    reopenHead();
    if (m_operationsAfterDone == OperationsAfterDone::Unknown && m_patternReads.contains(PatternRead::BySteps))
        m_done = m_expansion.addVariable(m_builder.appendBoolean(false));
    emitTestsAndOpens();
    connectArms();
}

void ArmDispatcher::report()
{
    m_pass.build.statistics().count(m_plan.hasClassArm() ? StatisticID::IterationsExpandedByClass : StatisticID::IterationsExpandedGenerically);
    if (m_iteration.kind == IteratorKind::Sync && !m_plan.hasClassArm())
        m_pass.addDecline(Wire::DeclineKind::IterationNotExpandedByClass, m_iteration.open);
}

OperationsAfterDone ArmDispatcher::findOperationsAfterDone(const BitVector& liveRegion)
{
    const UseLists& useLists = m_analyses.useLists();
    Vector<ValueLocation, 8> operations;
    for (ValueID resume : m_iteration.resumes)
        operations.append(useLists.locationOf(resume));
    for (ValueID close : m_iteration.closes)
        operations.append(useLists.locationOf(close));

    for (size_t index = 0; index < m_iteration.resumes.size(); ++index) {
        ValueLocation step = operations[index];
        BitVector enteredBlocks = findBlocksEnteredAfterDone(m_iteration.resumes[index], step.block, liveRegion);
        bool isFollowed = operations.containsIf([&](const ValueLocation& operation) {
            if (enteredBlocks.get(operation.block.index()))
                return true;
            return operation.block == step.block && operation.index > step.index;
        });
        if (isFollowed)
            return OperationsAfterDone::Unknown;
    }
    return OperationsAfterDone::None;
}

BitVector ArmDispatcher::findBlocksEnteredAfterDone(ValueID resume, BlockID stepBlock, const BitVector& liveRegion)
{
    // CFG::blocksReachableFrom stops at blocks. What ends this walk is an edge: the one taken while the step is not done, and any edge to a block in which the iterator is dead, from where only the open leads back.
    BitVector enteredBlocks(m_function.blocks.size());
    Vector<BlockID, 16> worklist { stepBlock };
    while (!worklist.isEmpty()) {
        forEachOutgoingEdge(m_function, worklist.takeLast(), [&](Edge edge, const Target& target) {
            if (!liveRegion.quickGet(target.block.index()) || provesNotDone(m_function, edge, resume))
                return;
            if (!enteredBlocks.set(target.block.index()))
                worklist.append(target.block);
        });
    }
    return enteredBlocks;
}

void ArmDispatcher::cloneRegionPerArm()
{
    m_copies.append(ArmCopy { m_tail, m_iteration.resumes, m_iteration.closes, m_iteration.cleanupBlocks });
    switch (m_dispatch) {
    case ArmDispatch::SingleArm:
    case ArmDispatch::ByForm:
        return;
    case ArmDispatch::Versioned:
        break;
    }

    for (BlockID block : m_region) {
        m_definitionsInRegion.append(m_function.at(block).parameters.span());
        m_definitionsInRegion.append(m_function.at(block).values.span());
    }

    for (size_t arm = 1; arm < m_plan.arms.size(); ++arm) {
        if (m_region.isEmpty()) {
            m_copies.append(ArmCopy { .entry = m_tail });
            continue;
        }

        RegionCloner cloner(m_builder, m_function);
        cloner.cloneBlocks(m_region.span(), [](ValueID, Value&) { });

        ArmCopy copy { .entry = cloner.cloneOf(m_tail) };
        RELEASE_ASSERT(copy.entry);
        for (ValueID resume : m_iteration.resumes)
            copy.resumes.append(cloner.cloneOf(resume));
        for (ValueID close : m_iteration.closes)
            copy.closes.append(cloner.cloneOf(close));
        for (BlockID cleanupBlock : m_iteration.cleanupBlocks)
            copy.cleanupBlocks.append(cloner.cloneOf(cleanupBlock));
        m_copies.append(WTF::move(copy));

        if (m_plan.arms[arm].form == IterationForm::Protocol) {
            for (BlockID block : cloner.clonedBlocks())
                m_function.edit(block).frequency = rarerOf(m_function.at(block).frequency, BlockFrequency::Unlikely);
            m_analyses.didChangeControlFlow();
        }

        // A cloner has a word for every value and every block of the function, so it does not wait for the repair.
        m_copiesOfDefinitions.append(m_definitionsInRegion.map([&](ValueID definition) {
            return cloner.cloneOf(definition);
        }));
    }
}

// One variable holds a definition and all of its copies: an edge out of one copy carries no definition of a variable that knows only another copy.
void ArmDispatcher::addVariablesOfCopies()
{
    if (m_copiesOfDefinitions.isEmpty())
        return;

    const UseLists& useLists = m_analyses.useLists();
    auto isUsedOutsideBlock = [&](ValueID definition) {
        BlockID block = useLists.blockOf(definition);
        return std::ranges::any_of(useLists.usesOf(definition), [&](const Use& use) {
            return use.block != block;
        });
    };
    for (size_t index = 0; index < m_definitionsInRegion.size(); ++index) {
        ValueID definition = m_definitionsInRegion[index];
        bool needsRepair = isUsedOutsideBlock(definition) || std::ranges::any_of(m_copiesOfDefinitions, [&](const Vector<ValueID>& copies) {
            return isUsedOutsideBlock(copies[index]);
        });
        if (!needsRepair)
            continue;
        SSARepair::Variable variable = m_repair.addVariable(definition);
        for (const Vector<ValueID>& copies : m_copiesOfDefinitions)
            m_repair.addDefinition(variable, copies[index]);
    }
}

void ArmDispatcher::reopenHead()
{
    m_function.edit(m_head).terminator = Terminator();
    m_analyses.didChangeControlFlow();
    m_builder.setHandler(m_function.at(m_head).handler);
    m_builder.moveToEndOf(m_head);
}

void ArmDispatcher::emitTestsAndOpens()
{
    if (!m_plan.armSources.isEmpty()) {
        emitOpensByArmSource();
        return;
    }

    BlockID protocolEntry;
    ValueID protocolIterable = m_plan.source;
    if (m_plan.hasProtocolArm() && m_plan.hasClassArm()) {
        protocolEntry = m_builder.addBlock(rarerOf(m_function.at(m_head).frequency, BlockFrequency::Unlikely));
        protocolIterable = addParameterLike(protocolEntry, m_plan.source);
    }

    size_t numberOfClassArms = m_plan.arms.size() - m_plan.hasProtocolArm();
    ValueID rest = m_plan.source;
    for (const IterationArm& arm : m_plan.arms.span().first(numberOfClassArms)) {
        RELEASE_ASSERT(rest);
        rest = emitClassArm(arm, rest, protocolEntry);
    }
    if (numberOfClassArms && rest)
        finishFailingEdge(rest, protocolEntry);

    if (!m_plan.hasProtocolArm())
        return;
    if (protocolEntry)
        m_builder.moveToEndOf(protocolEntry);
    emitProtocolArm(protocolIterable);
}

void ArmDispatcher::emitProtocolArm(ValueID iterable)
{
    m_states.append(ArmState { .arm = m_plan.arms.last(), .patternRead = m_patternReads.last(), .subject = iterable });
    if (m_patternReads.last() == PatternRead::InOneRead)
        m_copies.last().isReadAtOnce = true;
    else
        emitProtocolOpen(m_expansion, m_states.last(), m_iteration.kind);
    m_ends.append(m_builder.currentBlock());
}

void ArmDispatcher::emitOpensByArmSource()
{
    size_t numberOfClassArms = m_plan.arms.size() - m_plan.hasProtocolArm();
    std::optional<StateVariable> chosenArm;
    Vector<std::optional<StateVariable>, 2> subjects(numberOfClassArms);
    defineAtArmSources(chosenArm, subjects);

    if (m_plan.arms.size() == 1) {
        emitClassArmWithoutTest(m_plan.arms.first(), subjects.first()->anyDefinition);
        return;
    }

    BlockFrequency frequency = m_function.at(m_head).frequency;
    Vector<BlockID, 4> entries;
    for (const IterationArm& arm : m_plan.arms)
        entries.append(m_builder.addBlock(arm.form == IterationForm::Protocol ? rarerOf(frequency, BlockFrequency::Unlikely) : frequency));

    Vector<std::pair<int64_t, Target>> cases;
    for (size_t arm = 0; arm + 1 < entries.size(); ++arm)
        cases.constructAndAppend(static_cast<int64_t>(arm), Target { entries[arm], { }, { } });
    m_builder.emitSwitch(chosenArm->anyDefinition, WTF::move(cases), Target { entries.last(), { }, { } });

    for (size_t arm = 0; arm < numberOfClassArms; ++arm) {
        m_builder.moveToEndOf(entries[arm]);
        emitClassArmWithoutTest(m_plan.arms[arm], subjects[arm]->anyDefinition);
    }
    if (m_plan.hasProtocolArm()) {
        m_builder.moveToEndOf(entries.last());
        emitProtocolArm(m_plan.source);
    }
}

// Every definition of a subject has the type of its arm: an operation is typed from the definition it names, before SSARepair picks the one that reaches it.
void ArmDispatcher::defineAtArmSources(std::optional<StateVariable>& chosenArm, Vector<std::optional<StateVariable>, 2>& subjects)
{
    auto define = [&](std::optional<StateVariable>& variable, ProgramPoint point, Value&& definition) {
        ValueID id = m_builder.insertBefore(point, WTF::move(definition));
        if (variable)
            m_expansion.define(*variable, id);
        else
            variable = m_expansion.addVariable(id);
        return id;
    };
    auto makeUnspecified = [](TypeID type) {
        Value unspecified = IRBuilder::make(Opcode::Unspecified);
        unspecified.setValueType(type);
        return unspecified;
    };

    for (const ArmSource& source : m_plan.armSources) {
        ProgramPoint point = pointBeforeTerminatorOf(m_function, source.block);
        if (m_plan.arms.size() > 1) {
            Value arm = IRBuilder::make(Opcode::Integer);
            arm.setInteger(source.arm);
            define(chosenArm, point, WTF::move(arm));
        }

        for (size_t index = 0; index < subjects.size(); ++index) {
            TypeID type = m_plan.arms[index].testedType;
            if (index != source.arm) {
                define(subjects[index], point, makeUnspecified(type));
                continue;
            }
            // SSARepair rewrites every use of a definition, and the program's own uses of the receiver have to keep naming it.
            Value subject = IRBuilder::make(Opcode::Upcast, { source.subject });
            subject.setCastType(type);
            define(subjects[index], point, WTF::move(subject));
        }

        if (source.passedIterator) {
            ValueID passed = usedValueOf(m_function, *source.passedIterator);
            replaceUse(m_function, *source.passedIterator, m_builder.insertBefore(point, makeUnspecified(m_function.at(passed).type)));
        }
    }
    m_builder.applyInsertions();
}

void ArmDispatcher::emitClassArmWithoutTest(const IterationArm& arm, ValueID subject)
{
    m_states.append(ArmState { .arm = arm, .patternRead = m_patternReads[m_states.size()], .subject = subject });
    emitArmOpen(m_expansion, m_states.last());
    m_ends.append(m_builder.currentBlock());
}

ValueID ArmDispatcher::emitClassArm(const IterationArm& arm, ValueID value, BlockID protocolEntry)
{
    PatternRead patternRead = m_patternReads[m_states.size()];
    ValueID subject = value;
    BlockID failingBlock;
    ValueID rest;
    if (!arm.classIsProven) {
        ValueID test = m_builder.appendTypeTest(value, arm.testedType);
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        subject = branch.provenOnTaken;
        failingBlock = branch.notTaken;
        rest = branch.provenOnNotTaken;
    }

    if (!arm.namesAreProven) {
        RELEASE_ASSERT(protocolEntry);
        ValueID test = m_builder.append(makeNamesTest(arm, patternRead, subject));
        IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
        m_builder.moveToEndOf(branch.notTaken);
        m_builder.emitJump(protocolEntry, { chooseArgumentOfProtocolEntry(m_pass, protocolEntry, subject, value) });
        m_builder.moveToEndOf(branch.taken);
    }

    m_states.append(ArmState { .arm = arm, .patternRead = patternRead, .subject = subject });
    emitArmOpen(m_expansion, m_states.last());
    m_ends.append(m_builder.currentBlock());

    if (failingBlock)
        m_builder.moveToEndOf(failingBlock);
    return rest;
}

void ArmDispatcher::finishFailingEdge(ValueID rest, BlockID protocolEntry)
{
    if (protocolEntry) {
        m_builder.emitJump(protocolEntry, { rest });
        return;
    }

    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { rest });
    check.setNullishAccess(m_iteration.origin == IterationOrigin::Spread ? NullishAccess::Spread : NullishAccess::Iterate);
    m_builder.append(WTF::move(check));
    m_builder.emitUnreachable();
}

void ArmDispatcher::connectArms()
{
    switch (m_dispatch) {
    case ArmDispatch::SingleArm:
    case ArmDispatch::Versioned:
        RELEASE_ASSERT(m_ends.size() == m_copies.size());
        for (size_t arm = 0; arm < m_ends.size(); ++arm) {
            m_builder.moveToEndOf(m_ends[arm]);
            m_builder.emitJump(m_copies[arm].entry);
        }
        return;
    case ArmDispatch::ByForm:
        joinArmsByForm();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ArmDispatcher::joinArmsByForm()
{
    size_t numberOfArms = m_states.size();
    BlockID join = m_builder.addBlock(m_function.at(m_head).frequency);
    m_form = m_builder.addParameter(join, m_pass.types.intern(Type::integer({ 0, static_cast<int64_t>(numberOfArms) - 1 })));

    Vector<ArmState, 2> joinedStates = m_states;
    for (ArmState& state : joinedStates) {
        state.subject = addParameterLike(join, state.subject);
        if (state.nextMethod)
            state.nextMethod = addParameterLike(join, state.nextMethod);
        if (state.isSteppedWithoutCall)
            state.isSteppedWithoutCall = addParameterLike(join, state.isSteppedWithoutCall);
    }

    for (size_t arm = 0; arm < numberOfArms; ++arm) {
        m_builder.moveToEndOf(m_ends[arm]);
        Arguments arguments { m_builder.appendInteger(static_cast<int64_t>(arm)) };
        for (size_t other = 0; other < numberOfArms; ++other)
            appendJoinArguments(arguments, m_states[other], other == arm);
        m_builder.emitJump(join, WTF::move(arguments));
    }

    m_states = WTF::move(joinedStates);
    m_builder.moveToEndOf(join);
    m_builder.emitJump(m_tail);
}

void ArmDispatcher::appendJoinArguments(Arguments& arguments, const ArmState& state, bool isOwnEdge)
{
    if (isOwnEdge) {
        arguments.append(state.subject);
        if (state.nextMethod)
            arguments.append(state.nextMethod);
        if (state.isSteppedWithoutCall)
            arguments.append(state.isSteppedWithoutCall);
        return;
    }

    arguments.append(appendUnspecifiedLike(state.subject));
    if (state.nextMethod)
        arguments.append(appendUnspecifiedLike(state.nextMethod));
    if (state.isSteppedWithoutCall)
        arguments.append(appendUnspecifiedLike(state.isSteppedWithoutCall));
    for (const std::optional<StateVariable>& variable : { state.position, state.table }) {
        if (variable)
            m_expansion.define(*variable, appendUnspecifiedLike(variable->anyDefinition));
    }
}

void ArmDispatcher::collectPatternsReadAtOnce(Vector<PatternInOneRead>& patterns)
{
    for (size_t index = 0; index < m_copies.size(); ++index) {
        const ArmCopy& copy = m_copies[index];
        if (!copy.isReadAtOnce)
            continue;
        patterns.append(PatternInOneRead { { m_iteration.open, m_iteration.kind, m_iteration.origin, copy.resumes, copy.closes, copy.cleanupBlocks }, m_states[index].subject });
    }
}

void ArmDispatcher::collectOperationSites()
{
    const UseLists& useLists = m_analyses.useLists();
    for (unsigned copyIndex = 0; copyIndex < m_copies.size(); ++copyIndex) {
        const ArmCopy& copy = m_copies[copyIndex];
        if (copy.isReadAtOnce)
            continue;
        // An operation that nothing reaches any more goes away with its block.
        auto addSite = [&](ValueID operation) {
            ValueLocation location = useLists.locationOf(operation);
            if (location.placement != ValuePlacement::InBlock)
                return;
            OperationSite site { { location.block, location.index }, operation, copyIndex, { } };
            for (const Use& use : useLists.usesOf(operation)) {
                if (use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == Opcode::ResultAt)
                    site.resultReads.append(use.user);
            }
            m_sites.append(WTF::move(site));
        };
        for (ValueID resume : copy.resumes)
            addSite(resume);
        for (ValueID close : copy.closes)
            addSite(close);
    }

    std::sort(m_sites.begin(), m_sites.end(), [](const OperationSite& a, const OperationSite& b) {
        return std::tie(a.point.block, a.point.index) > std::tie(b.point.block, b.point.index);
    });
}

void ArmDispatcher::expandOperations()
{
    for (const OperationSite& site : m_sites)
        expandOperation(site);
}

void ArmDispatcher::expandOperation(const OperationSite& site)
{
    RELEASE_ASSERT(m_function.at(site.point.block).values[site.point.index] == site.value);

    Value original = m_function.at(site.value);
    bool isStep = original.opcode == Opcode::IteratorResume;
    m_builder.setOrigin(original.origin());
    // The overload without reads looks them up in the whole function and erases them wherever they stand, which moves the points of the sites still to come.
    IRBuilder::Expansion cut = isStep ? m_builder.beginExpansion(site.point, site.resultReads.span()) : m_builder.beginExpansion(site.point);
    m_expansion.beginOperation(cut);
    m_cut = &cut;

    bool tracksDone = m_done && isStep && (m_dispatch == ArmDispatch::ByForm || m_states[site.copy].patternRead == PatternRead::BySteps);
    if (tracksDone)
        skipStepWhenDone();
    else if (m_done && !isStep && !closingHasNoEffectIn(site.copy))
        skipCloseWhenDone();

    switch (m_dispatch) {
    case ArmDispatch::SingleArm:
    case ArmDispatch::Versioned:
        emitOperation(original, m_states[site.copy]);
        break;
    case ArmDispatch::ByForm:
        emitOperationByForm(original);
        break;
    }

    if (tracksDone)
        defineDoneAfterStep(cut);
}

void ArmDispatcher::skipStepWhenDone()
{
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(m_done->anyDefinition);
    m_expansion.finishStepAsDone();

    // A step that throws leaves the state done: this is the definition its handler sees.
    m_builder.moveToEndOf(blocks.notTaken);
    m_expansion.define(*m_done, m_builder.appendBoolean(true));
}

void ArmDispatcher::skipCloseWhenDone()
{
    IRBuilder::BranchBlocks blocks = m_builder.emitBranch(m_done->anyDefinition);
    m_expansion.finishClose();
    m_builder.moveToEndOf(blocks.notTaken);
}

void ArmDispatcher::defineDoneAfterStep(const IRBuilder::Expansion& cut)
{
    // SSARepair rewrites every use of a definition, and the program's own uses of this step's result have to keep naming it.
    Value done = IRBuilder::make(Opcode::Upcast, { cut.results[static_cast<size_t>(IteratorResumeResult::Done)] });
    done.setCastType(TypeTable::boolean);
    m_expansion.define(*m_done, m_builder.insertBefore(ProgramPoint::atHeadOf(cut.continuation), WTF::move(done)));
    m_builder.applyInsertions();
}

void ArmDispatcher::emitOperation(const Value& original, ArmState& state)
{
    if (state.arm.awaitsEachValue) {
        if (original.opcode == Opcode::IteratorResume) {
            if (original.resumeMode() == ResumeMode::Next)
                emitStepOfWrappedIterator(state);
            else
                emitThrowOrReturnOfWrappedIterator(original, state);
            return;
        }
        RELEASE_ASSERT(original.opcode == Opcode::IteratorClose);
        emitCloseOfWrappedIterator();
        return;
    }
    if (original.opcode == Opcode::IteratorResume && state.patternRead == PatternRead::ByIndex) {
        emitArrayStepByIndex(m_expansion, state, *original.stepIndex());
        return;
    }
    if (original.opcode == Opcode::IteratorResume) {
        emitArmStep(m_expansion, state, StepRequest { original.iteratorKind(), original.resumeMode(), original.sentOperand(), original.readsValueWhenDone() });
        return;
    }
    RELEASE_ASSERT(original.opcode == Opcode::IteratorClose);
    emitArmClose(m_expansion, state, original.iteratorKind(), original.completion());
}

static bool isAsyncGenerator(const Function& function)
{
    if (function.role == FunctionRole::CoroutineBody)
        return coroutineOfBody(function).kind == CoroutineKind::AsyncGenerator;
    return function.has(FunctionFlags::IsAsync) && function.has(FunctionFlags::IsGenerator);
}

// %AsyncFromSyncIteratorPrototype%.next rejects its promise with what the step throws, and the loop awaits that promise: the exception arrives one turn of the job queue later.
// It resolves its promise in a reaction to the value, done or not: the value arrives one turn later than from an await of the value itself.
// Closing the iterator of such an arm has no effect, so nobody can tell that a value which rejects closes it a turn before the loop hears of the rejection.
void ArmDispatcher::emitStepOfWrappedIterator(ArmState& state)
{
    BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
    std::optional<Target> outer = m_builder.handler();

    IRBuilder::Expansion step { m_builder.addBlock(frequency), { }, outer };
    step.results.append(m_builder.addParameter(step.continuation, TypeTable::boolean));
    step.results.append(m_builder.addParameter(step.continuation, TypeTable::anything));

    BlockID thrown = m_builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
    m_builder.setHandler(Target { thrown, { }, { } });
    BlockID attempt = m_builder.addBlock(frequency);
    m_builder.emitJump(attempt);

    m_builder.moveToEndOf(attempt);
    m_expansion.beginOperation(step);
    emitArmStep(m_expansion, state, StepRequest { });
    m_expansion.beginOperation(*m_cut);
    m_builder.setHandler(WTF::move(outer));

    m_builder.moveToEndOf(thrown);
    ValueID exception = m_builder.append(Opcode::CatchException, { });
    appendAwait(m_builder, m_builder.appendUndefined());
    m_builder.emitRethrow(exception);

    m_builder.moveToEndOf(step.continuation);
    ValueID done = step.results[static_cast<size_t>(IteratorResumeResult::Done)];
    if (!isAsyncGenerator(m_function)) {
        ValueID value = appendAwait(m_builder, step.results[static_cast<size_t>(IteratorResumeResult::Value)]);
        m_function.edit(value).setTakesExtraTurn(true);
        m_expansion.finishStep(done, value);
        return;
    }

    outer = m_builder.handler();
    m_builder.setHandler(Target { thrown, { }, { } });
    BlockID awaiting = m_builder.addBlock(frequency);
    m_builder.setHandler(WTF::move(outer));
    BlockID awaited = m_builder.addBlock(frequency);
    ValueID value = m_builder.addParameter(awaited, TypeTable::anything);
    m_builder.emitJump(awaiting);

    m_builder.moveToEndOf(awaiting);
    m_builder.emitJump(awaited, { appendAwait(m_builder, step.results[static_cast<size_t>(IteratorResumeResult::Value)]) });

    m_builder.moveToEndOf(awaited);
    appendAwait(m_builder, m_builder.appendUndefined());
    m_expansion.finishStep(done, value);
}

void ArmDispatcher::emitThrowOrReturnOfWrappedIterator(const Value& original, ArmState& state)
{
    ArmState protocol { .arm = IterationArm { }, .subject = openWrappedIterator(m_expansion, state.subject) };
    emitProtocolStep(m_expansion, protocol, StepRequest { IteratorKind::Async, original.resumeMode(), original.sentOperand(), original.readsValueWhenDone() });
}

// %AsyncFromSyncIteratorPrototype%.return finds no method to call on the iterator of such an arm and answers with a fulfilled promise, which the close awaits.
void ArmDispatcher::emitCloseOfWrappedIterator()
{
    appendAwait(m_builder, m_builder.appendUndefined());
    m_expansion.finishClose();
}

void ArmDispatcher::emitOperationByForm(const Value& original)
{
    BlockFrequency frequency = m_function.at(m_builder.currentBlock()).frequency;
    Vector<BlockID, 4> blocks;
    for (const ArmState& state : m_states) {
        bool isProtocol = state.arm.form == IterationForm::Protocol;
        blocks.append(m_builder.addBlock(rarerOf(frequency, isProtocol ? BlockFrequency::Unlikely : BlockFrequency::Normal)));
    }

    Vector<std::pair<int64_t, Target>> cases;
    for (size_t arm = 0; arm + 1 < blocks.size(); ++arm)
        cases.constructAndAppend(static_cast<int64_t>(arm), Target { blocks[arm], { }, { } });
    m_builder.emitSwitch(m_form, WTF::move(cases), Target { blocks.last(), { }, { } });

    for (size_t arm = 0; arm < blocks.size(); ++arm) {
        m_builder.moveToEndOf(blocks[arm]);
        emitOperation(original, m_states[arm]);
    }
}

void ArmDispatcher::finishCleanupBlocks(Vector<BlockID>& cleanupBlocksToDrop)
{
    for (unsigned copyIndex = 0; copyIndex < m_copies.size(); ++copyIndex) {
        const ArmCopy& copy = m_copies[copyIndex];
        if (copy.cleanupBlocks.isEmpty())
            continue;
        if (closingHasNoEffectIn(copyIndex))
            cleanupBlocksToDrop.append(copy.cleanupBlocks.span());
        else
            releaseCleanupBlocks(m_pass, copy.cleanupBlocks.span());
    }
}

bool ArmDispatcher::closingHasNoEffectIn(unsigned copyIndex) const
{
    if (m_copies[copyIndex].isReadAtOnce)
        return true;

    switch (m_dispatch) {
    case ArmDispatch::SingleArm:
    case ArmDispatch::Versioned:
        return closingHasNoEffect(m_plan.arms[copyIndex]);
    case ArmDispatch::ByForm:
        return std::ranges::all_of(m_plan.arms, [](const IterationArm& arm) {
            return closingHasNoEffect(arm);
        });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ArmDispatcher::eraseOpen()
{
    const BlockValues& values = m_function.at(m_head).values;
    RELEASE_ASSERT(values[m_openIndex] == m_iteration.open);
    BitVector indexes;
    indexes.set(m_openIndex);
    for (uint32_t index = 0; index < m_openIndex; ++index) {
        if (m_plan.replacedValues.contains(values[index]))
            indexes.set(index);
    }
    RELEASE_ASSERT(indexes.bitCount() == m_plan.replacedValues.size() + 1);
    m_builder.eraseValues(m_head, indexes);
}

ValueID ArmDispatcher::addParameterLike(BlockID block, ValueID model)
{
    TypeID expected = m_function.at(model).expected();
    ValueID parameter = m_builder.addParameter(block, m_function.at(model).type);
    if (expected) {
        m_function.edit(parameter).setExpected(expected);
        m_analyses.didChangeValues();
    }
    return parameter;
}

ValueID ArmDispatcher::appendUnspecifiedLike(ValueID model)
{
    return m_expansion.appendUnspecified(m_function.at(model).type);
}

// The iterations of a function that have no block in common are expanded together: the analyses are computed a fixed number of times for all of them, and SSA is repaired once.
// The blocks in which an iterator is live hold the iterations nested in it, so an iteration waits for the rounds that expand those, and its copies hold no open.
class IterationRound {
    WTF_MAKE_NONCOPYABLE(IterationRound);
public:
    IterationRound(FunctionPassContext&, IRBuilder&, std::span<const ValueID> opensOfSpreads, ExpandedIterations, BitVector& separatedOpens);

    bool run();
    PassOutcome outcome() const { return m_outcome; }

private:
    struct Cut {
        ProgramPoint point;
        Origin origin;
    };

    Vector<Iteration> collectWantedIterations();
    void separate(std::span<const Iteration>);
    void selectByBlocks(std::span<const Iteration>);
    void selectByPlans();

    void forEachDispatcher(NOESCAPE const Invocable<void(ArmDispatcher&)> auto& functor)
    {
        for (const std::unique_ptr<ArmDispatcher>& dispatcher : m_dispatchers)
            functor(*dispatcher);
    }

    FunctionPassContext& m_pass;
    IRBuilder& m_builder;
    std::span<const ValueID> m_opensOfSpreads;
    ExpandedIterations m_expandedIterations;
    BitVector& m_separatedOpens;
    SSARepair m_repair;
    Vector<std::unique_ptr<ArmDispatcher>> m_dispatchers;
    PassOutcome m_outcome { PassOutcome::Unchanged };
};

IterationRound::IterationRound(FunctionPassContext& pass, IRBuilder& builder, std::span<const ValueID> opensOfSpreads, ExpandedIterations expandedIterations, BitVector& separatedOpens)
    : m_pass(pass)
    , m_builder(builder)
    , m_opensOfSpreads(opensOfSpreads)
    , m_expandedIterations(expandedIterations)
    , m_separatedOpens(separatedOpens)
    , m_repair(builder, pass.analyses)
{
}

bool IterationRound::run()
{
    Vector<Iteration> iterations = collectWantedIterations();
    if (iterations.isEmpty())
        return false;

    separate(iterations.span());
    selectByBlocks(iterations.span());
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.planArms(); });
    selectByPlans();
    if (m_dispatchers.isEmpty())
        return false;
    m_outcome = PassOutcome::Changed;

    passParametersOfCleanupBlocksOn(m_pass, m_builder);
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.splitAfterLastCloses(); });
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.chooseForms(); });
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.emitOpens(); });
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.addVariablesOfCopies(); });
    Vector<PatternInOneRead> patternsReadAtOnce;
    forEachDispatcher([&](ArmDispatcher& dispatcher) { dispatcher.collectPatternsReadAtOnce(patternsReadAtOnce); });
    expandPatternsInOneRead(m_pass, m_builder, patternsReadAtOnce.span());
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.collectOperationSites(); });
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.expandOperations(); });

    Vector<BlockID> cleanupBlocksToDrop;
    forEachDispatcher([&](ArmDispatcher& dispatcher) { dispatcher.finishCleanupBlocks(cleanupBlocksToDrop); });
    dropCleanupRegions(m_pass, m_builder, cleanupBlocksToDrop.span());

    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.eraseOpen(); });
    m_builder.setHandler(std::nullopt);
    m_repair.run();
    m_builder.emptyUnreachableBlocks();
    forEachDispatcher([](ArmDispatcher& dispatcher) { dispatcher.report(); });
    return true;
}

Vector<Iteration> IterationRound::collectWantedIterations()
{
    Vector<Iteration> iterations = collectIterations(m_pass);
    for (Iteration& iteration : iterations) {
        if (std::ranges::contains(m_opensOfSpreads, iteration.open))
            iteration.origin = IterationOrigin::Spread;
    }

    switch (m_expandedIterations) {
    case ExpandedIterations::All:
        break;
    case ExpandedIterations::OnlyProven:
        iterations.removeAllMatching([&](const Iteration& iteration) {
            return awaitsCallOnString(m_pass, iteration) || !isExpandedWithoutTest(planIteration(m_pass, iteration));
        });
        break;
    }
    return iterations;
}

// Every open ends its block. A close that is followed by an operation of another iteration ends its block as well, so iterations that stand one behind the other in straight-line code have no block in common.
void IterationRound::separate(std::span<const Iteration> iterations)
{
    const Function& function = m_pass.function;
    Vector<Cut, 16> cuts;
    {
        const UseLists& useLists = m_pass.analyses.useLists();
        for (const Iteration& iteration : iterations) {
            Origin origin = function.at(iteration.open).origin();
            if (!m_separatedOpens.set(iteration.open.index())) {
                ValueLocation open = useLists.locationOf(iteration.open);
                cuts.append(Cut { { open.block, open.index + 1 }, origin });
            }

            std::optional<BitVector> liveRegion;
            for (ValueID close : iteration.closes) {
                ValueLocation location = useLists.locationOf(close);
                if (!isFollowedByIterationOperation(function, location))
                    continue;
                if (!liveRegion)
                    liveRegion = blockSetOf(function, findLiveRegion(m_pass, iteration.open).span());
                if (isLastUseOfOpen(m_pass.analyses, iteration.open, *liveRegion, location))
                    cuts.append(Cut { { location.block, location.index + 1 }, origin });
            }
        }
    }
    if (cuts.isEmpty())
        return;

    // From the last cut of a block to the first: what stands in front of a cut stays where it is.
    std::ranges::sort(cuts, std::ranges::greater { }, [](const Cut& cut) {
        return std::pair { cut.point.block, cut.point.index };
    });
    for (const Cut& cut : cuts) {
        m_builder.setOrigin(cut.origin);
        m_builder.splitBlock(cut.point);
    }
    m_outcome = PassOutcome::Changed;
}

// From the last iteration to the first, so that an iteration is asked after the ones nested in it.
void IterationRound::selectByBlocks(std::span<const Iteration> iterations)
{
    BitVector takenBlocks(m_pass.function.blocks.size());
    for (const Iteration& iteration : iterations | std::views::reverse) {
        auto dispatcher = makeUniqueWithoutFastMallocCheck<ArmDispatcher>(m_pass, m_builder, m_repair, iteration);
        bool sharesBlock = takenBlocks.quickSet(dispatcher->head().index());
        for (BlockID block : dispatcher->region())
            sharesBlock |= takenBlocks.quickSet(block.index());
        if (!sharesBlock)
            m_dispatchers.append(WTF::move(dispatcher));
    }
}

// What an arm defines at the end of a block that passes an iterator on has to find that block as the plan saw it.
void IterationRound::selectByPlans()
{
    constexpr unsigned nobody = 0;
    Vector<unsigned> ownerOfBlock(FillWith { }, m_pass.function.blocks.size(), nobody);
    for (unsigned index = 0; index < m_dispatchers.size(); ++index) {
        ownerOfBlock[m_dispatchers[index]->head().index()] = index + 1;
        for (BlockID block : m_dispatchers[index]->region())
            ownerOfBlock[block.index()] = index + 1;
    }

    BitVector isPostponed(m_dispatchers.size());
    bool isAnyLeftOut = false;
    for (unsigned index = 0; index < m_dispatchers.size(); ++index) {
        const IterationPlan& plan = m_dispatchers[index]->plan();
        if (m_expandedIterations == ExpandedIterations::OnlyProven && !isExpandedWithoutTest(plan)) {
            m_dispatchers[index] = nullptr;
            isAnyLeftOut = true;
            continue;
        }

        bool sharesBlock = std::ranges::any_of(plan.armSources, [&](const ArmSource& source) {
            unsigned owner = ownerOfBlock[source.block.index()];
            return owner != nobody && owner != index + 1;
        });
        if (sharesBlock) {
            isPostponed.quickSet(index);
            continue;
        }
        for (const ArmSource& source : plan.armSources)
            ownerOfBlock[source.block.index()] = index + 1;
    }

    // Alone, the last iteration shares no block with anybody.
    if (!isAnyLeftOut && !m_dispatchers.isEmpty() && isPostponed.bitCount() == m_dispatchers.size())
        isPostponed.quickClear(0);
    for (size_t index : isPostponed)
        m_dispatchers[index] = nullptr;
    m_dispatchers.removeAllMatching([](const std::unique_ptr<ArmDispatcher>& dispatcher) {
        return !dispatcher;
    });
}

} // anonymous namespace

// An iterator is the base of no interior pointer and no test narrows it, so it is live from its open to its uses.
Vector<BlockID> findLiveRegion(FunctionPassContext& pass, ValueID open)
{
    const CFG& cfg = pass.analyses.cfg();
    const UseLists& useLists = pass.analyses.useLists();
    BlockID blockOfOpen = useLists.blockOf(open);

    Vector<BlockID> region;
    BitVector isLiveAtHead(pass.function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (const Use& use : useLists.usesOf(open))
        worklist.append(use.block);
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (block == blockOfOpen || !cfg.isReachable(block) || isLiveAtHead.quickSet(block.index()))
            continue;
        region.append(block);
        worklist.append(cfg.predecessors(block));
    }
    std::ranges::sort(region, { }, [&](BlockID block) {
        return cfg.reversePostOrderIndex(block);
    });
    return region;
}

ArmDispatch chooseArmDispatch(FunctionPassContext& pass, const Iteration& iteration, const IterationPlan& plan, std::span<const BlockID> liveRegion)
{
    if (plan.arms.size() == 1)
        return ArmDispatch::SingleArm;
    if (liveRegion.empty())
        return ArmDispatch::Versioned;
    if (RegionCloner::checkCanDuplicate(pass.analyses, liveRegion) != CloneCheck::CanClone)
        return ArmDispatch::ByForm;

    uint64_t bytes = 0;
    for (BlockID block : liveRegion)
        bytes += estimatedCodeBytes(pass.program, pass.function, pass.function.at(block));
    if (pass.exceeds(LimitID::MaxVersionedIterationBytes, bytes * (plan.arms.size() - 1), iteration.open))
        return ArmDispatch::ByForm;
    return ArmDispatch::Versioned;
}

PassOutcome expandIterations(FunctionPassContext& pass, IRBuilder& builder, std::span<const ValueID> opensOfSpreads, ExpandedIterations expandedIterations)
{
    BitVector separatedOpens;
    PassOutcome outcome = PassOutcome::Unchanged;
    for (;;) {
        IterationRound round(pass, builder, opensOfSpreads, expandedIterations, separatedOpens);
        bool didExpand = round.run();
        outcome |= round.outcome();
        if (!didExpand)
            return outcome;
    }
}

void emitArmOpen(IterationExpansion& expansion, ArmState& state)
{
    switch (state.arm.form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::String:
    case IterationForm::SplitString:
        emitIndexedOpen(expansion, state);
        return;
    case IterationForm::OwnEntries:
        emitOwnEntriesOpen(expansion, state);
        return;
    case IterationForm::Map:
    case IterationForm::Set:
        emitCollectionOpen(expansion, state);
        return;
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        return;
    case IterationForm::Protocol:
        emitProtocolOpen(expansion, state, IteratorKind::Sync);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitStepWithoutMethod(IterationExpansion& expansion, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder();
    switch (request.mode) {
    case ResumeMode::Return: {
        ValueID value = request.sent ? request.sent : builder.appendUndefined();
        expansion.finishStep(builder.appendBoolean(true), value);
        return;
    }
    case ResumeMode::Throw: {
        Value error = IRBuilder::make(Opcode::NewRuntimeError);
        error.setRuntimeError(RuntimeErrorKind::IteratorWithoutThrowMethod);
        builder.emitThrow(builder.append(WTF::move(error)));
        return;
    }
    case ResumeMode::Next:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitArmStep(IterationExpansion& expansion, ArmState& state, const StepRequest& request)
{
    if (closingHasNoEffect(state.arm.form) && request.mode != ResumeMode::Next) {
        emitStepWithoutMethod(expansion, request);
        return;
    }

    switch (state.arm.form) {
    case IterationForm::Array:
        emitArrayStep(expansion, state);
        return;
    case IterationForm::TypedArray:
        emitTypedArrayStep(expansion, state);
        return;
    case IterationForm::String:
        emitStringStep(expansion, state);
        return;
    case IterationForm::SplitString:
        emitSplitStringStep(expansion, state);
        return;
    case IterationForm::OwnEntries:
        emitOwnEntriesStep(expansion, state);
        return;
    case IterationForm::Map:
    case IterationForm::Set:
        emitCollectionStep(expansion, state);
        return;
    case IterationForm::Generator:
        emitGeneratorStep(expansion, state, request);
        return;
    case IterationForm::AsyncGenerator:
        emitAsyncGeneratorStep(expansion, state, request);
        return;
    case IterationForm::Protocol:
        emitProtocolStep(expansion, state, request);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitArmClose(IterationExpansion& expansion, ArmState& state, IteratorKind kind, IteratorCompletion completion)
{
    switch (state.arm.form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::String:
    case IterationForm::SplitString:
    case IterationForm::OwnEntries:
    case IterationForm::Map:
    case IterationForm::Set:
        expansion.finishClose();
        return;
    case IterationForm::Generator:
        emitGeneratorClose(expansion, state, completion);
        return;
    case IterationForm::AsyncGenerator:
    case IterationForm::Protocol:
        emitProtocolClose(expansion, state, kind, completion);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
