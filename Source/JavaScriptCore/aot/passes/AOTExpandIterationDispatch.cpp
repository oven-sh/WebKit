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
#include "AOTFunctionAnalyses.h"
#include "AOTRegionCloner.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <memory>
#include <tuple>
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

class ArmDispatcher {
    WTF_MAKE_NONCOPYABLE(ArmDispatcher);
public:
    ArmDispatcher(FunctionPassContext&, IRBuilder&, const Iteration&);

    void run();

private:
    void splitAfterOpen();
    void splitAfterLastCloses();
    OperationsAfterDone findOperationsAfterDone();
    BitVector findBlocksEnteredAfterDone(ValueID resume, BlockID stepBlock);
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

    void expandOperations();
    Vector<OperationSite, 8> collectOperationSites();
    void expandOperation(const OperationSite&);
    void skipStepWhenDone();
    void skipCloseWhenDone();
    void defineDoneAfterStep(const IRBuilder::Expansion&);
    void emitOperation(const Value& original, ArmState&);
    void emitOperationByForm(const Value& original);

    void finishCleanupBlocks();
    bool closingHasNoEffectIn(unsigned copyIndex) const;
    void eraseOpen();

    ValueID addParameterLike(BlockID, ValueID model);
    ValueID appendUnspecifiedLike(ValueID model);

    FunctionPassContext& m_pass;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder& m_builder;
    const Iteration& m_iteration;
    IterationPlan m_plan;
    SSARepair m_repair;
    IterationExpansion m_expansion;
    BlockID m_head;
    uint32_t m_openIndex { 0 };
    BlockID m_tail;
    Vector<BlockID> m_region;
    ArmDispatch m_dispatch { ArmDispatch::SingleArm };
    Vector<PatternRead, 2> m_patternReads;
    Vector<ArmCopy, 2> m_copies;
    Vector<std::unique_ptr<RegionCloner>, 1> m_cloners;
    Vector<ArmState, 2> m_states;
    Vector<BlockID, 2> m_ends;
    ValueID m_form;
    std::optional<StateVariable> m_done;
};

ArmDispatcher::ArmDispatcher(FunctionPassContext& pass, IRBuilder& builder, const Iteration& iteration)
    : m_pass(pass)
    , m_function(pass.function)
    , m_analyses(pass.analyses)
    , m_builder(builder)
    , m_iteration(iteration)
    , m_plan(planIteration(pass, iteration))
    , m_repair(builder, pass.analyses)
    , m_expansion(pass, builder, m_repair, iteration.origin)
{
}

void ArmDispatcher::run()
{
    bool isDestructuring = m_function.at(m_iteration.open).isDestructuring();
    m_builder.setOrigin(m_function.at(m_iteration.open).origin());

    splitAfterOpen();
    OperationsAfterDone operationsAfterDone = findOperationsAfterDone();
    m_region = findLiveRegion(m_pass, m_iteration.open);
    if (m_plan.arms.size() > 1)
        splitAfterLastCloses();
    m_dispatch = chooseArmDispatch(m_pass, m_iteration, m_plan, m_region.span());
    bool hasOneCopyPerArm = m_dispatch != ArmDispatch::ByForm;
    for (const IterationArm& arm : m_plan.arms)
        m_patternReads.append(isDestructuring && hasOneCopyPerArm ? choosePatternRead(m_pass, m_iteration, arm) : PatternRead::BySteps);

    cloneRegionPerArm();
    reopenHead();
    if (operationsAfterDone == OperationsAfterDone::Unknown && m_patternReads.contains(PatternRead::BySteps))
        m_done = m_expansion.addVariable(m_builder.appendBoolean(false));
    emitTestsAndOpens();
    connectArms();
    expandOperations();
    finishCleanupBlocks();
    eraseOpen();
    m_repair.run();
    m_builder.emptyUnreachableBlocks();

    m_pass.build.statistics().count(m_plan.hasClassArm() ? StatisticID::IterationsExpandedByClass : StatisticID::IterationsExpandedGenerically);
    if (m_iteration.kind == IteratorKind::Sync && !m_plan.hasClassArm())
        m_pass.addDecline(Wire::DeclineKind::IterationNotExpandedByClass, m_iteration.open);
}

void ArmDispatcher::splitAfterOpen()
{
    ValueLocation location = m_analyses.useLists().locationOf(m_iteration.open);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    m_head = location.block;
    m_openIndex = location.index;
    m_tail = m_builder.splitBlock(ProgramPoint { m_head, m_openIndex + 1 });
}

void ArmDispatcher::splitAfterLastCloses()
{
    BitVector isInRegion(m_function.blocks.size());
    for (BlockID block : m_region)
        isInRegion.quickSet(block.index());

    // A block that was merged with its successor goes on with the code after the iteration, which no copy needs and no limit on copies should count.
    Vector<ProgramPoint, 2> cuts;
    {
        const CFG& cfg = m_analyses.cfg();
        const UseLists& useLists = m_analyses.useLists();
        auto isLastUseOfOpen = [&](ValueLocation close) {
            bool isLiveAtTail = std::ranges::any_of(cfg.successors(close.block), [&](BlockID successor) {
                return isInRegion.quickGet(successor.index());
            });
            return !isLiveAtTail && std::ranges::none_of(useLists.usesOf(m_iteration.open), [&](const Use& use) {
                return use.kind == Use::Kind::Operand && use.block == close.block && useLists.locationOf(use.user).index > close.index;
            });
        };
        for (ValueID close : m_iteration.closes) {
            ValueLocation location = useLists.locationOf(close);
            if (location.index + 1 < m_function.at(location.block).values.size() && isLastUseOfOpen(location))
                cuts.append(ProgramPoint { location.block, location.index + 1 });
        }
    }

    for (ProgramPoint cut : cuts)
        m_builder.splitBlock(cut);
}

OperationsAfterDone ArmDispatcher::findOperationsAfterDone()
{
    const UseLists& useLists = m_analyses.useLists();
    Vector<ValueLocation, 8> operations;
    for (ValueID resume : m_iteration.resumes)
        operations.append(useLists.locationOf(resume));
    for (ValueID close : m_iteration.closes)
        operations.append(useLists.locationOf(close));

    for (size_t index = 0; index < m_iteration.resumes.size(); ++index) {
        ValueLocation step = operations[index];
        BitVector enteredBlocks = findBlocksEnteredAfterDone(m_iteration.resumes[index], step.block);
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

BitVector ArmDispatcher::findBlocksEnteredAfterDone(ValueID resume, BlockID stepBlock)
{
    // CFG::blocksReachableFrom stops at blocks. What ends this walk is an edge: the one taken while the step is not done, and any edge back to the open.
    BitVector enteredBlocks(m_function.blocks.size());
    Vector<BlockID, 16> worklist { stepBlock };
    while (!worklist.isEmpty()) {
        forEachOutgoingEdge(m_function, worklist.takeLast(), [&](Edge edge, const Target& target) {
            if (target.block == m_head || provesNotDone(m_function, edge, resume))
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

    for (size_t arm = 1; arm < m_plan.arms.size(); ++arm) {
        if (m_region.isEmpty()) {
            m_copies.append(ArmCopy { .entry = m_tail });
            continue;
        }

        auto cloner = makeUniqueWithoutFastMallocCheck<RegionCloner>(m_builder, m_function);
        cloner->cloneBlocks(m_region.span(), [](ValueID, Value&) { });

        ArmCopy copy { .entry = cloner->cloneOf(m_tail) };
        RELEASE_ASSERT(copy.entry);
        for (ValueID resume : m_iteration.resumes)
            copy.resumes.append(cloner->cloneOf(resume));
        for (ValueID close : m_iteration.closes)
            copy.closes.append(cloner->cloneOf(close));
        for (BlockID cleanupBlock : m_iteration.cleanupBlocks)
            copy.cleanupBlocks.append(cloner->cloneOf(cleanupBlock));
        m_copies.append(WTF::move(copy));

        if (m_plan.arms[arm].form == IterationForm::Protocol) {
            for (BlockID block : cloner->clonedBlocks())
                m_function.at(block).frequency = rarerOf(m_function.at(block).frequency, BlockFrequency::Unlikely);
            m_analyses.didChangeControlFlow();
        }
        m_cloners.append(WTF::move(cloner));
    }
}

void ArmDispatcher::reopenHead()
{
    m_function.at(m_head).terminator = Terminator();
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
        m_builder.emitJump(protocolEntry, { subject });
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
    check.setNullishAccess(NullishAccess::Iterate);
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
        RegionCloner::repairSSA(m_analyses, m_cloners.span());
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
        return;
    }

    arguments.append(appendUnspecifiedLike(state.subject));
    if (state.nextMethod)
        arguments.append(appendUnspecifiedLike(state.nextMethod));
    for (const std::optional<StateVariable>& variable : { state.position, state.table }) {
        if (variable)
            m_expansion.define(*variable, appendUnspecifiedLike(variable->anyDefinition));
    }
}

void ArmDispatcher::expandOperations()
{
    for (size_t index = 0; index < m_copies.size(); ++index) {
        const ArmCopy& copy = m_copies[index];
        if (!copy.isReadAtOnce)
            continue;
        Iteration operations { m_iteration.open, m_iteration.kind, m_iteration.origin, copy.resumes, copy.closes, copy.cleanupBlocks };
        expandPatternInOneRead(m_pass, m_builder, operations, m_states[index].subject);
    }

    for (const OperationSite& site : collectOperationSites())
        expandOperation(site);
}

Vector<OperationSite, 8> ArmDispatcher::collectOperationSites()
{
    const UseLists& useLists = m_analyses.useLists();
    Vector<OperationSite, 8> sites;
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
            sites.append(WTF::move(site));
        };
        for (ValueID resume : copy.resumes)
            addSite(resume);
        for (ValueID close : copy.closes)
            addSite(close);
    }

    std::sort(sites.begin(), sites.end(), [](const OperationSite& a, const OperationSite& b) {
        return std::tie(a.point.block, a.point.index) > std::tie(b.point.block, b.point.index);
    });
    return sites;
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

void ArmDispatcher::finishCleanupBlocks()
{
    for (unsigned copyIndex = 0; copyIndex < m_copies.size(); ++copyIndex) {
        const ArmCopy& copy = m_copies[copyIndex];
        if (copy.cleanupBlocks.isEmpty())
            continue;
        if (closingHasNoEffectIn(copyIndex))
            dropCleanupRegions(m_pass, m_builder, copy.cleanupBlocks.span());
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
        return closingHasNoEffect(m_plan.arms[copyIndex].form);
    case ArmDispatch::ByForm:
        return std::ranges::all_of(m_plan.arms, [](const IterationArm& arm) {
            return closingHasNoEffect(arm.form);
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
    TypeID type = m_function.at(model).type;
    TypeID predicted = m_function.at(model).predictionOrType();
    ValueID parameter = m_builder.addParameter(block, type);
    if (predicted != type) {
        m_function.at(parameter).setPrediction(predicted);
        m_analyses.didChangeValues();
    }
    return parameter;
}

ValueID ArmDispatcher::appendUnspecifiedLike(ValueID model)
{
    return m_expansion.appendUnspecified(m_function.at(model).type);
}

} // anonymous namespace

Vector<BlockID> findLiveRegion(FunctionPassContext& pass, ValueID open)
{
    const CFG& cfg = pass.analyses.cfg();
    BitVector trackedValues(pass.function.values.size());
    trackedValues.set(open.index());
    Liveness liveness(pass.function, cfg, pass.analyses.useLists(), { .trackedValues = &trackedValues });

    Vector<BlockID> region;
    for (BlockID block : cfg.reversePostOrder()) {
        if (liveness.isLiveAtHead(open, block))
            region.append(block);
    }
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

void expandOneIteration(FunctionPassContext& pass, IRBuilder& builder, const Iteration& iteration)
{
    ArmDispatcher(pass, builder, iteration).run();
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
