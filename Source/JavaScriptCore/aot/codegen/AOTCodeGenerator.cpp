/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinTypes.h"
#include "AOTAccessEmitter.h"
#include "AOTArrayStorageForms.h"
#include "AOTCallEmitter.h"
#include "AOTCallingConventions.h"
#include "AOTCodeRecords.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectsNet.h"
#include "AOTEffectsOfOperations.h"
#include "AOTElementAccessEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTEntryEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTOperatorEmitter.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStringBuilderEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTSweep.h"
#include "AOTThrowEmitter.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeIDs.h"
#include <algorithm>
#include <bit>
#include <ranges>
#include <type_traits>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned longestWayUpSinglePredecessors = 8;
static constexpr unsigned longestWayUpDominators = 32;
static constexpr BlockID enteredOtherwise = BlockID(BlockID::hashTableDeletedIndex);

template<typename T>
static std::span<T> allocateSpan(Machine::Arena& arena, size_t count)
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    return unsafeMakeSpan(static_cast<T*>(arena.allocate(count * sizeof(T), alignof(T))), count);
}

template<typename T>
static std::span<T> allocateZeroedSpan(Machine::Arena& arena, size_t count)
{
    std::span<T> span = allocateSpan<T>(arena, count);
    zeroSpan(span);
    return span;
}

static_assert(BlockID().bits() == UINT32_MAX);
template<typename T>
static std::span<T> allocateSpanOfAllBitsSet(Machine::Arena& arena, size_t count)
{
    std::span<T> span = allocateSpan<T>(arena, count);
    memsetSpan(span, 0xFF);
    return span;
}

static constexpr int maxUnlikelyMaskBits = 4;

static Machine::EdgeClass edgeClassLeadingTo(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return Machine::EdgeClass::Normal;
    case BlockFrequency::Unlikely:
        return Machine::EdgeClass::Unlikely;
    case BlockFrequency::Rare:
        return Machine::EdgeClass::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOfGeneratedOperation(const OperationContext& context)
{
    if (context.valueID())
        return nameOf(static_cast<const ValueContext&>(context).opcode());
    return nameOf(static_cast<const TerminatorContext&>(context).kind());
}

static String describeFormsOfGeneratedOperand(Representation found, Representation wanted)
{
    return makeString("an operand kept as "_s, nameOf(found), " is wanted as "_s, nameOf(wanted));
}

GenerationResult CodeGenerator::run()
{
    RELEASE_ASSERT(code().isSwept() && !code().entries().empty());
    if (!recordSitesWithoutCode())
        return GenerationResult::Failed;

    m_values = allocateZeroedSpan<LValue>(m_proc.arena(), code().numberOfValues());
    m_looksBehindEachOperation = m_checksAllocation || code().someBarrierIsCountedOn();
    m_storesBehindWhichOwnerIsRemembered.clearAll();
    m_storesWithLaterOrderWord.clearAll();
    m_lastFirstStores.shrink(0);
    m_regionsAtHeads.shrink(0);
    m_carriedSubstrings.shrink(0);
    m_hasKeysAskedTwice = false;
    orderBlocks();
    createBlocks();
    followNumbersOutOfBytes();
    lookForCarriedSums();
    createPhis();
    if (unit().role == CodeRole::CoroutineBody && code().hasSuspensions()) {
        m_keepsValuesInHeapFrame = emitter<SavedFrameEmitter>().layOutHeapFrame();
        m_looksBehindEachOperation |= m_keepsValuesInHeapFrame;
    }

    if (code().hasLoops() && code().opcodesOfValues().contains(Opcode::IsElementPresent) && code().opcodesOfValues().contains(Opcode::IsPristine))
        findRowGuards();
    if (code().hasLoops() && code().opcodesOfValues().contains(Opcode::HasArrayStorage))
        findKindsTried();
    findRopeTolerantStrings();
    findLoopCarriedNumbers();
    findLoopCarriedTables();
    findCarriedListStorages();
    findCarriedSubstrings();
    findKeysCheckedTwice();
    if (!m_parametersThatMaySum.isEmpty()) [[unlikely]]
        carrySums();
    m_notesHeadsOfBlocks = m_hasKeysAskedTwice || m_rows || m_kindsTried;
    m_watchesTerminators = m_checksAllocation || m_checksEffects || m_rows || m_kindsTried;
    m_watchesCalls = m_understatedEffects || m_rows;
    m_leavesBoxesToHandlers = false;
    if (code().hasBoxesForHandlers() && !m_keepsValuesInHeapFrame) [[unlikely]]
        findBoxesLeftToHandlers();

    m_generatesWatched = m_watchesOperations || m_looksBehindEachOperation;
    definePinnedValues(m_out, m_pinned);
    emitter<EntryEmitter>().generateEntries();
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (m_hasFailed)
            return GenerationResult::Failed;
        BlockID id = m_order[position];
        m_positionInOrder = position;
        if (!m_loopExits.isEmpty()) [[unlikely]]
            noteScopeOfLoopExit(id);
        if (leavesOutBlockThatNothingLeadsTo(id))
            continue;
        if (!m_loopExits.isEmpty()) [[unlikely]]
            enterScopeOfLoopExit(id);
        const Block& block = code().at(id);
        if (block.isLoopHead && !carriesBesideParameters(id) && (!m_carriesSums || !carriesSum(id)) && (!m_keepsValuesInHeapFrame || emitter<SavedFrameEmitter>().liesInLoopInsideStretch(id))) {
            if (std::optional<LoopDistribution> distribution = code().opcodesOfValues().contains(Opcode::PutTypedArrayElement) ? findLoopDistribution(position) : std::nullopt) [[unlikely]] {
                distributeLoop(*distribution);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            LBasicBlock entryOfLoop = m_blocks[id.index()];
            if (code().opcodesOfValues().contains(Opcode::NewArrayOfGiven)) {
                tryEmitArrayAllocationLoop(id);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            if (code().opcodesOfValues().contains(Opcode::PutDoubleIndex) || (m_rows && code().opcodesOfValues().contains(Opcode::PutArrayElement))) {
                tryEmitRowLoop(id);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            if (m_blocks[id.index()] == entryOfLoop)
                tryEmitMoveLoop(id);
            if (m_hasFailed)
                return GenerationResult::Failed;
            if (code().opcodesOfValues().contains(Opcode::GetArrayElement) && (code().opcodesOfValues().contains(Opcode::StrictEq) || code().opcodesOfValues().contains(Opcode::Eq))) {
                tryEmitSearchLoop(id);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            static constexpr OpcodeSet elementReads { Opcode::GetTypedArrayElement, Opcode::GetArrayElement };
            static constexpr OpcodeSet elementStores { Opcode::PutTypedArrayElement, Opcode::PutArrayElement };
            if (code().opcodesOfValues().intersects(elementStores) && code().opcodesOfValues().intersects(elementReads)) {
                tryEmitSumNest(id);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            bool mayHoldSteppedStore = code().opcodesOfValues().contains(Opcode::PutArrayElement) || code().opcodesOfValues().contains(Opcode::PutTypedArrayElement);
            if (std::optional<SteppedStore> stepped = mayHoldSteppedStore ? findSteppedStore(position) : std::nullopt) [[unlikely]]
                layOutSteppedStore(*stepped);
            std::optional<CountedLoop> loop = findCountedLoopAt(position);
            bool mayRotate = (!loop || loop->isTooLongToUnroll) && code().opcodesOfValues().contains(Opcode::BitXor);
            if (std::optional<RotatingLoop> rotating = mayRotate ? findRotatingLoop(position) : std::nullopt) [[unlikely]] {
                unrollRotatingLoop(*rotating);
                if (m_hasFailed)
                    return GenerationResult::Failed;
                loop = std::nullopt;
            }
            if (loop && loop->mayComeBack && isShortRecursiveLoop(*loop)) {
                build().statistics().count(StatisticID::RecursiveLoopsNotUnrolled);
                loop = std::nullopt;
            }
            if (loop && !findSharedViewKinds(*loop))
                loop = std::nullopt;
            if (std::optional<WideLoop> wide = loop && !loop->rowCountsAsked ? findWideLoop(*loop) : std::nullopt) {
                layOutWide(*loop, *wide);
                if (loop->isTooLongToUnroll)
                    loop = std::nullopt;
            }
            if (std::optional<LanePlan> lanes = loop && !loop->rowCountsAsked ? findLanes(*loop) : std::nullopt) {
                layOutLanes(*loop, *lanes);
                loop = std::nullopt;
            }
            if (loop && m_keepsValuesInHeapFrame) {
                for (BlockID inTurn : loop->turn) {
                    if (!emitter<SavedFrameEmitter>().liesInLoopInsideStretch(inTurn)) {
                        loop = std::nullopt;
                        break;
                    }
                }
            }
            if (std::optional<Interleaving> nest = loop && !loop->rowCountsAsked ? findInterleaving(*loop) : std::nullopt) {
                layOutInterleaved(*loop, *nest);
                if (m_hasFailed)
                    return GenerationResult::Failed;
            }
            if (loop && loop->storesIntoSlots)
                build().statistics().count(StatisticID::UnrolledLoopsWithSlotStores);
            if (loop) {
                findLoopExits(*loop);
                emitFirstConditions(*loop);
                startFromConstant(*loop);
            }
            if (loop && !loop->viewsAskedForKind.isEmpty())
                layOutTurnsForEachKindOfView(*loop);
            else if (loop) {
                auto layOutForEveryKind = [&](CountedLoop& loop) {
                    loop.turnsForWideTextsFollow = asksTextsOfLoopForWidth(loop);
                    if (!loop.mayComeBack && !loop.secondTurnsGoByKindsRead && !loop.turnsForWideTextsFollow && loop.asksListsForAnotherKind())
                        m_layoutsInRow = LayoutsInRow { loop.asksListsReadAndListsStoredInto ? 4u : 2u, nullptr, { } };
                    else if (!loop.mayComeBack && !loop.secondTurnsGoByKindsRead && loop.turnsForWideTextsFollow && loop.listsAskedForKind.isEmpty())
                        m_layoutsInRow = LayoutsInRow { 2u, nullptr, { } };
                    layOutTurns(loop);
                    if (std::exchange(loop.turnsForWideTextsFollow, false) && !m_hasFailed) {
                        loop.asksTextsForWide = true;
                        layOutTurns(loop);
                    }
                    if (loop.asksListsForAnotherKind() && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop);
                    if (loop.asksListsReadAndListsStoredInto && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop, false);
                    if (loop.asksListsReadAndListsStoredInto && !m_hasFailed)
                        layOutTurnsForOtherKindOfStorage(loop);
                    RELEASE_ASSERT(m_hasFailed || !m_ringOfLoopLaidOut);
                    RELEASE_ASSERT(m_hasFailed || !m_layoutsInRow);
                };
                if (std::ranges::any_of(loop->listsAskedForKind, &ListAskedForKind::otherKindsAreNotAsked)) {
                    const Edge& back = code().at(code().at(loop->turn.last()).terminator.next());
                    auto findSumsAnew = [&](CountedLoop& changed) {
                        changed.sumsOfIntegers.shrink(0);
                        changed.sumsAreTakenOverBehindStretches = false;
                        findSumsOfIntegers(changed, back);
                    };
                    bool onlyTruthIsAsked = std::ranges::all_of(loop->turn, [&](BlockID inTurn) {
                        std::span<const ValueID> values = code().valuesOf(inTurn);
                        return std::ranges::all_of(values, [&](ValueID read) {
                            const Operation& operation = code().at(read);
                            if (operation.opcode != Opcode::GetArrayElement || !std::ranges::any_of(loop->listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.otherKindsAreNotAsked && asked.list == operation.arrayOperand(); }))
                                return true;
                            ValueID test = code().at(inTurn).terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(inTurn, code().at(inTurn).terminator) : ValueID();
                            return test && values.size() >= 2 && values.back() == test && values[values.size() - 2] == read && opcodeOf(test) == Opcode::ToBoolean && code().at(test).form() == OperatorForm::Generic && code().at(test).valueOperand() == read && code().useCountOf(read) == 1;
                        });
                    });
                    if (onlyTruthIsAsked) {
                        CountedLoop forWholeNumbers = *loop;
                        for (ListAskedForKind& asked : forWholeNumbers.listsAskedForKind) {
                            if (asked.otherKindsAreNotAsked)
                                asked.kind = ArrayStorage::Int32;
                        }
                        forWholeNumbers.mayComeBack = false;
                        forWholeNumbers.boundsAskedInFront.shrink(0);
                        findSumsAnew(forWholeNumbers);
                        build().statistics().count(StatisticID::UnrolledLoopsForInt32Truthiness);
                        layOutForEveryKind(forWholeNumbers);
                    }
                    CountedLoop forDoubles = *loop;
                    forDoubles.mayComeBack = loop->mayComeBack && loop->boundsAskedInFront.isEmpty();
                    forDoubles.layoutWithoutQuestionFollows = forDoubles.mayComeBack;
                    forDoubles.boundsAskedInFront.shrink(0);
                    findSumsAnew(forDoubles);
                    if (forDoubles.mayComeBack)
                        layOutTurns(forDoubles);
                    else
                        layOutForEveryKind(forDoubles);
                    loop->listsAskedForKind.removeAllMatching([](const ListAskedForKind& asked) {
                        return asked.otherKindsAreNotAsked;
                    });
                    versionIterationsByKind(*loop);
                    findSumsAnew(*loop);
                }
                if (!m_hasFailed && !loop->boundsAskedInFront.isEmpty()) {
                    CountedLoop behindBounds = *loop;
                    behindBounds.mayComeBack = false;
                    build().statistics().count(StatisticID::UnrolledLoopsForMaskedIndexes);
                    layOutForEveryKind(behindBounds);
                    loop->boundsAskedInFront.shrink(0);
                    loop->sumsOfIntegers.shrink(0);
                    loop->sumsAreTakenOverBehindStretches = false;
                    findSumsOfIntegers(*loop, code().at(code().at(loop->turn.last()).terminator.next()));
                }
                if (!m_hasFailed)
                    layOutForEveryKind(*loop);
            }
            RELEASE_ASSERT(m_hasFailed || !m_layoutsInRow);
            if (m_hasFailed)
                return GenerationResult::Failed;
        }
        [[clang::always_inline]] generateBlock(id, block);
        new (NotNull, &m_scopeAtEndOfBlock[id.index()]) ScopeFork(m_out.forkScope());
    }
    for (LBasicBlock leftOut : m_blocksLeftOut)
        RELEASE_ASSERT(m_hasFailed || !leftOut->numberOfIncomingEdges());
    RELEASE_ASSERT(m_hasFailed || !m_limitOfWrittenLoop);
    RELEASE_ASSERT(m_hasFailed || !emitter<TruthinessEmitter>().hasDeferredTruthTests());
    return m_hasFailed ? GenerationResult::Failed : GenerationResult::Generated;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::leavesOutBlockThatNothingLeadsTo(BlockID id)
{
    if (m_nextBlockGoesOnHere)
        return false;
    LBasicBlock block = m_blocks[id.index()];
    if (!block)
        return true;
    if (block->numberOfIncomingEdges() || block->isEntry()) [[likely]]
        return false;
    m_blocksLeftOut.append(block);
    return true;
}

const Function& CodeGenerator::function() const
{
    return program().at(unit().function);
}

FormAtPlace CodeGenerator::sayFormNamedFor(const OperationContext& context, StateOfKnowledge state) const
{
    bool standsInLoop = context.block() && usesLoopForms(context.block());
    if (!context.valueID())
        return formByLoopAlone(static_cast<const TerminatorContext&>(context).kind(), state, standsInLoop);
    FormAtPlace named = formByLoopAlone(static_cast<const ValueContext&>(context).opcode(), state, standsInLoop);
    if (m_operation == &context)
        m_records.formsSaid.say(named.form, state, named.place);
    return named;
}

bool CodeGenerator::takesOpenForm(const OperationContext& context, StateOfKnowledge state) const
{
    return sayFormNamedFor(context, state).form == Form::Open;
}

bool CodeGenerator::takesFormOfLoops(const OperationContext& context, StateOfKnowledge state) const
{
    return sayFormNamedFor(context, state).place == Place::Loop;
}

const PlacedSlot* CodeGenerator::slotOfScopeReadInPlaceOfClosureSlow(ValueID id, const Operation& read) const
{
    if (code().useCountOf(id) != 1 || !code().onlyUserOf(id))
        return nullptr;
    const Operation& capture = code().at(code().onlyUserOf(id));
    if (capture.opcode != Opcode::GetCapture || capture.index() || capture.representation != Representation::Cell)
        return nullptr;
    const PlacedSlot* scope = nullptr;
    for (LayoutID member : membersOf(inputs().types, read.receivers())) {
        if (!plan().isInUse(member))
            continue;
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, read.key());
        std::optional<FieldIndex> fieldOfScope = plan().layout(member).environmentOfDeferredClosures;
        if (!field || !fieldOfScope)
            return nullptr;
        const PlacedSlot& slot = plan().slot(member, *field);
        if (slot.representation != SlotRepresentation::DeferredClosure || slot.storage != SlotStorage::Inline || !slot.deferredClosureCapturesEnvironment || slot.deferredFunction != capture.function())
            return nullptr;
        const PlacedSlot& found = plan().slot(member, *fieldOfScope);
        if (found.representation != SlotRepresentation::Cell32 || (scope && !scope->hasLocationOf(found)))
            return nullptr;
        scope = &found;
    }
    return scope;
}

bool CodeGenerator::isDeferredClosureSlow(const Operation& named) const
{
    if (named.representation != Representation::Void)
        return false;
    const Operation* definition = &named;
    while (definition->opcode == Opcode::Narrowed)
        definition = &code().at(definition->valueOperand());
    return definition->opcode == Opcode::NewClosure && definition->closureForm() == ClosureForm::Deferred;
}

void CodeGenerator::computeEffectsOf(const OperationContext& context) const
{
    std::span<const ValueID> operands = context.instruction().operands();
    std::array<TypeID, 8> typesOfFewOperands;
    Vector<TypeID> typesOfManyOperands;
    std::span<TypeID> typesOfOperands(typesOfFewOperands);
    if (operands.size() > typesOfFewOperands.size()) [[unlikely]] {
        typesOfManyOperands.grow(operands.size());
        typesOfOperands = typesOfManyOperands.mutableSpan();
    }
    for (size_t index = 0; index < operands.size(); ++index) {
        ValueID operand = operands[index];
        typesOfOperands[index] = operand ? typeIDOf(operand) : TypeID();
    }
    OperandTypes operandTypes { typesOfOperands.first(operands.size()) };
    if (!context.valueID()) {
        new (NotNull, &context.m_effects) Effects(effectsOfTerminator(m_typing, static_cast<const TerminatorContext&>(context).terminator(), operandTypes));
        context.m_hasEffects = true;
        return;
    }

    const Operation& operation = static_cast<const ValueContext&>(context).value();
    Effects& effects = *new (NotNull, &context.m_effects) Effects(Typed::effectsOf(m_typing, operation, operandTypes, operation.type));
    context.m_hasEffects = true;
    EffectTraits traits = operation.traits();
    if (!traits.mayThrow())
        effects.mayThrow = false;
    else if (!traits.mayThrowByRule())
        effects.mayThrow &= ThrowEffect::onlyForLackOfMemory();
    if (!traits.mayCollect())
        effects.mayCollect = false;
    if (!traits.mayCallUnknownCode())
        effects.mayCallUnknownCode = false;
    if (!traits.maySuspend())
        effects.maySuspend = false;
    if (!traits.mayBeTerminated())
        effects.mayBeTerminated = false;
    if (m_understatedEffects) [[unlikely]]
        UnderstatedEffects::decode(m_understatedEffects).cut(KindOfEffectsRow::Operation, static_cast<unsigned>(operation.opcode), effects);
}

bool CodeGenerator::recordSitesWithoutCode()
{
    Subject subject { .function = unit().function };
    std::span<const InlineFrame> inlineFrames = code().inlineFrames();
    if (inlineFrames.size() > build().limit(LimitID::InlineFramesPerFunction)) {
        failAtLimit(LimitID::InlineFramesPerFunction, inlineFrames.size(), subject);
        return false;
    }

    SiteRecords& sites = m_records.sites;
    for (const InlineFrame& frame : inlineFrames) {
        bool isFunctionItself = sites.inlineCallSites.isEmpty();
        if (isFunctionItself) {
            sites.inlineCallSites.append(0);
            continue;
        }
        sites.inlineCallSites.append(sites.addSite({
            .origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isPartOfConstruction = frame.callIsPartOfConstruction || frame.isConstruction },
            .kind = frame.isConstruction ? CallSiteKind::Construct : CallSiteKind::Plain,
        }));
    }
    sites.numberOfSitesWithoutCode = sites.positions.size();

    size_t numberOfPositions = sites.positions.size() + 1;
    if (numberOfPositions > build().limit(LimitID::SitesPerFunction)) {
        failAtLimit(LimitID::SitesPerFunction, numberOfPositions, subject);
        return false;
    }
    return true;
}

void CodeGenerator::orderBlocks()
{
    static_assert(BlockFrequency::Normal < BlockFrequency::Unlikely && BlockFrequency::Unlikely < BlockFrequency::Rare);
    enum class StepKind : uint8_t { EdgeOfTerminator, OtherWayIn, EndOfBlock };
    struct Step {
        BlockID block;
        BlockID from;
        StepKind kind;
    };

    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfBlocks = code().numberOfBlocks();
    m_singlePredecessors = allocateSpanOfAllBitsSet<BlockID>(arena, numberOfBlocks);
    m_scopeAtEndOfBlock = allocateZeroedSpan<ScopeFork>(arena, numberOfBlocks);
    Machine::UncheckedSpan<BlockID> order = allocateSpan<BlockID>(arena, numberOfBlocks);
    uint32_t numberOrdered = 0;
    Vector<Step, 64> steps;

    auto endBlock = [&](BlockID block) {
        order[numberOfBlocks - ++numberOrdered] = block;
    };

    static constexpr BlockFrequency fromLikeliest[] = { BlockFrequency::Normal, BlockFrequency::Unlikely, BlockFrequency::Rare };
    auto enter = [&](Step& step) -> bool {
        BlockID id = step.block;
        RELEASE_ASSERT(id);
        BlockID& predecessor = m_singlePredecessors[id.index()];
        if (predecessor) {
            m_firstBlockHasOtherWaysIn |= id == m_onlyEntryBlock;
            if (predecessor != enteredOtherwise)
                m_waysIntoJoins.append({ id, predecessor });
            m_waysIntoJoins.append({ id, step.from });
            predecessor = enteredOtherwise;
            return false;
        }
        if (!code().isReached(id))
            return false;
        if (step.kind == StepKind::EdgeOfTerminator)
            predecessor = step.from;
        else {
            predecessor = enteredOtherwise;
            m_waysIntoJoins.append({ id, step.from });
        }

        const Block& block = code().at(id);
        const Terminator& terminator = block.terminator;
        EdgeID handler = block.handler ? code().handlerOf(id) : EdgeID();
        uint32_t numberOfSuccessors = terminator.numberOfEdges + (handler ? 1 : 0);
        if (!numberOfSuccessors) {
            endBlock(id);
            return false;
        }
        steps.append({ id, BlockID(), StepKind::EndOfBlock });
        auto successorAt = [&](uint32_t index) -> Step {
            if (index < terminator.numberOfEdges)
                return { code().at(terminator.edge(index)).to, id, StepKind::EdgeOfTerminator };
            return { code().at(handler).to, id, StepKind::OtherWayIn };
        };
        if (numberOfSuccessors == 1) {
            step = successorAt(0);
            return true;
        }
        if (numberOfSuccessors == 2) {
            Step first = successorAt(0);
            Step second = successorAt(1);
            bool firstIsRarer = code().at(first.block).frequency > code().at(second.block).frequency;
            steps.append(firstIsRarer ? second : first);
            step = firstIsRarer ? first : second;
            return true;
        }

        struct Successor {
            Step step;
            BlockFrequency frequency;
        };
        Vector<Successor, 8> successors;
        for (uint32_t index = 0; index < numberOfSuccessors; ++index) {
            Step successor = successorAt(index);
            successors.append({ successor, code().at(successor.block).frequency });
        }
        for (BlockFrequency frequency : fromLikeliest) {
            for (const Successor& successor : successors) {
                if (successor.frequency == frequency)
                    steps.append(successor.step);
            }
        }
        return false;
    };
    auto takeStep = [&](Step& step) -> bool {
        while (!steps.isEmpty()) {
            step = steps.takeLast();
            if (step.kind != StepKind::EndOfBlock)
                return true;
            endBlock(step.block);
        }
        return false;
    };

    std::span<const EntryOfCode> entries = code().entries();
    if (entries.size() == 1)
        m_onlyEntryBlock = entries[0].block;
    for (size_t entry = entries.size(); entry--;) {
        Step step { entries[entry].block, BlockID(), StepKind::OtherWayIn };
        while (enter(step) || takeStep(step)) { }
    }
    m_order = order.span().last(numberOrdered);
    for (const WayIntoBlock& way : m_waysIntoJoins)
        m_singlePredecessors[way.to.index()] = BlockID();
}

void CodeGenerator::findDominators()
{
    constexpr uint32_t unknown = UINT32_MAX;
    constexpr uint32_t noWay = UINT32_MAX;
    constexpr uint32_t root = 0;
    Machine::Arena& arena = m_proc.arena();
    uint32_t numberOfPlaces = static_cast<uint32_t>(m_order.size() + 1);

    m_places = allocateZeroedSpan<uint32_t>(arena, code().numberOfBlocks());
    for (uint32_t place = 1; place < numberOfPlaces; ++place)
        m_places[m_order[place - 1].index()] = place;

    m_dominatorsByPlace = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    m_dominatorsByPlace[root] = root;
    Machine::UncheckedSpan<uint32_t> firstWaysOut = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    Machine::UncheckedSpan<uint32_t> nextWaysOut = allocateSpan<uint32_t>(arena, m_waysIntoJoins.size());
    for (uint32_t index = 0; index < m_waysIntoJoins.size(); ++index) {
        const WayIntoBlock& way = m_waysIntoJoins[index];
        if (!way.from) {
            m_dominatorsByPlace[m_places[way.to.index()]] = root;
            continue;
        }
        uint32_t& first = firstWaysOut[m_places[way.from.index()]];
        nextWaysOut[index] = first;
        first = index;
    }

    Machine::UncheckedSpan<uint32_t> firstWaysIn = allocateSpanOfAllBitsSet<uint32_t>(arena, numberOfPlaces);
    Machine::UncheckedSpan<uint32_t> nextWaysIn = allocateSpan<uint32_t>(arena, m_waysIntoJoins.size());
    Vector<uint32_t, 16> targetsOfWaysThatComeRound;
    for (uint32_t place = 1; place < numberOfPlaces; ++place) {
        for (uint32_t index = firstWaysOut[place]; index != noWay; index = nextWaysOut[index]) {
            uint32_t target = m_places[m_waysIntoJoins[index].to.index()];
            if (target <= place && (firstWaysIn[target] == noWay || m_places[m_waysIntoJoins[firstWaysIn[target]].from.index()] < target))
                targetsOfWaysThatComeRound.append(target);
            nextWaysIn[index] = firstWaysIn[target];
            firstWaysIn[target] = index;
        }
    }
    auto goThroughPlaces = [&]<bool climbsMayBeLong>() {
        constexpr int64_t stepsOfShortClimbsForEachPlace = 4;
        [[maybe_unused]] int64_t stepsLeftForShortClimbs = stepsOfShortClimbsForEachPlace * numberOfPlaces;
        [[maybe_unused]] std::span<uint32_t> depths;
        [[maybe_unused]] std::span<uint32_t> leaps;
        [[maybe_unused]] uint32_t numberOfPlacesHung = 1;
        [[maybe_unused]] uint32_t numberOfPlacesWithDominatorOfRound = 1;
        [[maybe_unused]] bool dominatorHasMovedBehindRound = false;
        auto lastOnEveryWayToBoth = [&](uint32_t found, uint32_t other) {
            if (!climbsMayBeLong || stepsLeftForShortClimbs >= 0 || dominatorHasMovedBehindRound) [[likely]] {
                while (found != other) {
                    if (found > other)
                        found = m_dominatorsByPlace[found];
                    else
                        other = m_dominatorsByPlace[other];
                    if constexpr (climbsMayBeLong)
                        --stepsLeftForShortClimbs;
                }
                return found;
            }
            if (leaps.empty()) {
                depths = allocateSpan<uint32_t>(arena, numberOfPlaces);
                leaps = allocateSpan<uint32_t>(arena, numberOfPlaces);
                depths[root] = 0;
                leaps[root] = root;
            }
            for (; numberOfPlacesHung < numberOfPlacesWithDominatorOfRound; ++numberOfPlacesHung) {
                uint32_t above = m_dominatorsByPlace[numberOfPlacesHung];
                uint32_t leap = leaps[above];
                depths[numberOfPlacesHung] = depths[above] + 1;
                leaps[numberOfPlacesHung] = depths[above] - depths[leap] == depths[leap] - depths[leaps[leap]] ? leaps[leap] : above;
            }
            if (depths[found] < depths[other])
                std::swap(found, other);
            uint32_t depth = depths[other];
            while (depths[found] > depth)
                found = depths[leaps[found]] >= depth ? leaps[found] : m_dominatorsByPlace[found];
            while (found != other) {
                bool leapsDiffer = leaps[found] != leaps[other];
                found = leapsDiffer ? leaps[found] : m_dominatorsByPlace[found];
                other = leapsDiffer ? leaps[other] : m_dominatorsByPlace[other];
            }
            return found;
        };
        auto takeWaysInto = [&](uint32_t target, bool thatComeRound) {
            uint32_t known = m_dominatorsByPlace[target];
            if (known == root)
                return false;
            uint32_t found = unknown;
            for (uint32_t index = firstWaysIn[target]; index != noWay; index = nextWaysIn[index]) {
                uint32_t source = m_places[m_waysIntoJoins[index].from.index()];
                if ((source >= target) != thatComeRound) {
                    if (thatComeRound)
                        break;
                    continue;
                }
                found = found == unknown ? source : lastOnEveryWayToBoth(found, source);
            }
            if (found == unknown)
                return false;
            if (known != unknown)
                found = lastOnEveryWayToBoth(found, known);
            m_dominatorsByPlace[target] = found;
            return found != known;
        };

        for (bool changed = true; changed;) {
            changed = false;
            numberOfPlacesHung = 1;
            dominatorHasMovedBehindRound = false;
            for (uint32_t place = 1; place < numberOfPlaces; ++place) {
                if (BlockID predecessor = m_singlePredecessors[m_order[place - 1].index()])
                    m_dominatorsByPlace[place] = m_places[predecessor.index()];
                else {
                    numberOfPlacesWithDominatorOfRound = place;
                    takeWaysInto(place, false);
                }
            }
            numberOfPlacesWithDominatorOfRound = numberOfPlaces;
            for (uint32_t target : targetsOfWaysThatComeRound) {
                if (takeWaysInto(target, true)) {
                    changed = true;
                    dominatorHasMovedBehindRound = true;
                }
            }
        }
    };
    constexpr uint32_t mostPlacesOfBodyWithoutLongClimbs = 512;
    if (numberOfPlaces > mostPlacesOfBodyWithoutLongClimbs)
        goThroughPlaces.template operator()<true>();
    else
        goThroughPlaces.template operator()<false>();
}

void CodeGenerator::numberDominatorTree()
{
    Machine::Arena& arena = m_proc.arena();
    size_t numberOfPlaces = m_dominatorsByPlace.size();
    m_subtreesByPlace = allocateZeroedSpan<SubtreeOfDominators>(arena, numberOfPlaces);
    for (size_t place = numberOfPlaces; --place;) {
        uint32_t size = ++m_subtreesByPlace[place].size;
        m_subtreesByPlace[m_dominatorsByPlace[place]].size += size;
    }
    Machine::UncheckedSpan<uint32_t> nextNumbersBelow = allocateSpan<uint32_t>(arena, numberOfPlaces);
    nextNumbersBelow[0] = 0;
    for (size_t place = 1; place < numberOfPlaces; ++place) {
        uint32_t& nextNumberBelowDominator = nextNumbersBelow[m_dominatorsByPlace[place]];
        uint32_t first = nextNumberBelowDominator;
        nextNumberBelowDominator += m_subtreesByPlace[place].size;
        nextNumbersBelow[place] = first + 1;
        m_subtreesByPlace[place].first = first;
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::dominates(BlockID home, BlockID user)
{
    if (m_dominatorsByPlace.empty()) {
        BlockID above = user;
        for (unsigned step = 0; step < longestWayUpSinglePredecessors; ++step) {
            above = m_singlePredecessors[above.index()];
            if (!above)
                break;
            if (above == home)
                return true;
        }
        findDominators();
    }

    uint32_t placeOfHome = m_places[home.index()];
    uint32_t placeOfUser = m_places[user.index()];
    if (!placeOfHome || !placeOfUser)
        return false;
    if (m_subtreesByPlace.empty()) {
        uint32_t place = placeOfUser;
        for (unsigned step = 0; step < longestWayUpDominators; ++step) {
            if (place <= placeOfHome)
                return place == placeOfHome;
            place = m_dominatorsByPlace[place];
        }
        numberDominatorTree();
    }
    const SubtreeOfDominators& subtreeOfHome = m_subtreesByPlace[placeOfHome];
    return m_subtreesByPlace[placeOfUser].first - subtreeOfHome.first < subtreeOfHome.size;
}

std::optional<CodeGenerator::PushesInLoop> CodeGenerator::findPushesInLoop(BlockID block, ValueID array)
{
    if (m_keepsValuesInHeapFrame || !usesLoopForms(block))
        return std::nullopt;
    if (m_dominatorsByPlace.empty())
        findDominators();
    uint32_t placeOfBlock = m_places[block.index()];
    BlockID head;
    Vector<BlockID, 4> ends;
    for (uint32_t place = placeOfBlock; place && !head; place = m_dominatorsByPlace[place]) {
        BlockID above = m_order[place - 1];
        if (!code().at(above).isLoopHead)
            continue;
        bool isInLoop = false;
        code().forEachIncomingEdgeUnsealed(above, [&](const Edge& edge) {
            if (edge.to != above || !code().isReached(edge.from) || !dominates(above, edge.from))
                return;
            ends.append(edge.from);
            isInLoop |= m_places[edge.from.index()] >= placeOfBlock;
        });
        if (isInLoop)
            head = above;
        else
            ends.shrink(0);
    }
    if (!head || !std::ranges::all_of(ends, [&](BlockID end) { return dominates(block, end); }))
        return std::nullopt;

    static constexpr OpcodeSet tests { Opcode::Less, Opcode::LessEq };
    auto withoutNarrowing = [&](ValueID value) {
        while (!code().isConstant(value) && (operationsThatStandAmongNarrowedValues.contains(opcodeOf(value)) || valuePreservingOpcodes.contains(opcodeOf(value))))
            value = code().at(value).operand(0);
        return value;
    };
    auto isIncremented = [&](ValueID parameter) {
        size_t place = std::ranges::find(code().at(head).parameters(), parameter) - code().at(head).parameters().begin();
        bool isIncrementedOnEveryEdge = true;
        code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
            if (edge.to != head || !std::ranges::contains(ends, edge.from))
                return;
            ValueID next = place < edge.arguments().size() ? withoutNarrowing(edge.arguments()[place]) : ValueID();
            isIncrementedOnEveryEdge &= next && !code().isConstant(next) && opcodeOf(next) == Opcode::Increment && withoutNarrowing(code().at(next).operand(0)) == parameter;
        });
        return isIncrementedOnEveryEdge;
    };
    std::optional<PushesInLoop> found;
    BlockID testBlock;
    for (uint32_t place = m_dominatorsByPlace[placeOfBlock]; place >= m_places[head.index()]; place = m_dominatorsByPlace[place]) {
        const Terminator& terminator = code().at(m_order[place - 1]).terminator;
        if (terminator.kind != TerminatorKind::Branch || !tests.contains(opcodeOf(terminator.conditionOperand())))
            continue;
        const Operation& test = code().at(terminator.conditionOperand());
        ValueID counter = withoutNarrowing(test.leftOperand());
        if (!std::ranges::contains(code().at(head).parameters(), counter) || !isIncremented(counter))
            continue;
        found = PushesInLoop { counter, test.rightOperand(), test.opcode == Opcode::LessEq };
        testBlock = m_order[place - 1];
    }
    static constexpr RepresentationSet readableForms { Representation::Int32, Representation::Int64, Representation::Double, Representation::JSValue };
    auto isAvailable = [&](ValueID value) {
        return code().isConstant(value) || (isGenerated(value) && readableForms.contains(representationOf(value)) && !findBoxed(value));
    };
    if (!found || !isAvailable(found->counter) || !isAvailable(found->bound))
        return std::nullopt;

    Vector<BlockID, 16> blocks { head };
    BitVector isInLoop(code().numberOfBlocks());
    isInLoop.quickSet(head.index());
    auto add = [&](BlockID member) {
        if (code().isReached(member) && !isInLoop.quickSet(member.index()))
            blocks.append(member);
    };
    for (BlockID end : ends)
        add(end);
    for (size_t place = 1; place < blocks.size(); ++place) {
        code().forEachIncomingEdgeUnsealed(blocks[place], [&](const Edge& edge) {
            add(edge.from);
        });
    }
    static constexpr OpcodeSet reads {
        Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetArrayElement, Opcode::GetValueAtSiteOrAbsent,
        Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::GetGlobalBinding, Opcode::GetGlobalProperty, Opcode::GetGlobalPropertyAddress, Opcode::CheckInitialized
    };
    auto isSameInEveryIteration = [&](auto& self, ValueID value, unsigned depth) -> bool {
        BlockID home = !value || code().isConstant(value) ? BlockID() : code().blockOf(value);
        if (!home || !isInLoop.quickGet(home.index()))
            return true;
        if (!depth)
            return false;
        const Operation& operation = code().at(value);
        if (operation.opcode != Opcode::Parameter)
            return (reads.contains(operation.opcode) || valuePreservingOpcodes.contains(operation.opcode) || operationsThatStandAmongNarrowedValues.contains(operation.opcode)) && std::ranges::all_of(operation.operands(), [&](ValueID operand) { return self(self, operand, depth - 1); });
        if (home == head)
            return false;
        size_t place = std::ranges::find(code().at(home).parameters(), value) - code().at(home).parameters().begin();
        ValueID joined;
        bool isJoined = true;
        code().forEachIncomingEdgeUnsealed(home, [&](const Edge& edge) {
            if (edge.to != home || !code().isReached(edge.from) || code().at(edge.from).frequency == BlockFrequency::Rare)
                return;
            ValueID argument = place < edge.arguments().size() ? edge.arguments()[place] : ValueID();
            isJoined &= argument && (!joined || joined == argument);
            joined = argument;
        });
        return isJoined && joined && self(self, joined, depth - 1);
    };
    constexpr unsigned longestWayToVariable = 8;
    if (!isSameInEveryIteration(isSameInEveryIteration, array, longestWayToVariable))
        return std::nullopt;
    if (BlockID home = code().isConstant(array) ? BlockID() : code().blockOf(array); home && isInLoop.quickGet(home.index())) {
        size_t place = std::ranges::find(code().at(head).parameters(), found->counter) - code().at(head).parameters().begin();
        bool startsAlike = true;
        code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
            if (edge.to != head || !code().isReached(edge.from) || std::ranges::contains(ends, edge.from))
                return;
            ValueID start = place < edge.arguments().size() ? edge.arguments()[place] : ValueID();
            startsAlike &= start && (!found->startOfCounter || found->startOfCounter == start);
            found->startOfCounter = start;
        });
        if (!startsAlike || !found->startOfCounter || !isAvailable(found->startOfCounter))
            return std::nullopt;
    }
    for (BlockID member : blocks) {
        const Terminator& terminator = code().at(member).terminator;
        for (uint32_t edge = 0; member != testBlock && edge < terminator.numberOfEdges; ++edge) {
            BlockID target = code().at(terminator.edge(edge)).to;
            if (!isInLoop.quickGet(target.index()) && code().at(target).frequency != BlockFrequency::Rare)
                return std::nullopt;
        }
        bool pushesOntoArray = terminator.kind == TerminatorKind::TryCallInline && terminator.inlineBuiltin() == InlineBuiltin::ArrayPush && terminator.receiverOperand() == array;
        if (pushesOntoArray && std::ranges::all_of(ends, [&](BlockID end) { return dominates(member, end); }))
            found->valuesPerIteration += terminator.argumentsOperands().size();
    }
    return found->valuesPerIteration ? found : std::nullopt;
}

bool CodeGenerator::hasArrayStorageTest(ValueID array)
{
    if (!m_arraysTestedForStorage) {
        m_arraysTestedForStorage.emplace();
        for (BlockID block : m_order) {
            for (ValueID value : code().valuesOf(block)) {
                if (opcodeOf(value) != Opcode::HasArrayStorage)
                    continue;
                ValueID tested = code().at(value).arrayOperand();
                while (valuePreservingOpcodes.contains(opcodeOf(tested)))
                    tested = code().at(tested).operand(0);
                if (!m_arraysTestedForStorage->contains(tested))
                    m_arraysTestedForStorage->append(tested);
            }
        }
    }
    return m_arraysTestedForStorage->contains(array);
}

bool CodeGenerator::isPoppedInLoop(ValueID array)
{
    auto rootOf = [&](ValueID value) {
        while (valuePreservingOpcodes.contains(opcodeOf(value)))
            value = code().at(value).operand(0);
        return value;
    };
    if (!m_arraysPoppedInLoops) {
        m_arraysPoppedInLoops.emplace();
        for (BlockID block : m_order) {
            const Terminator& terminator = code().at(block).terminator;
            if (!isInLoop(block) || terminator.kind != TerminatorKind::TryCallInline || terminator.inlineBuiltin() != InlineBuiltin::ArrayPop)
                continue;
            ValueID popped = rootOf(terminator.receiverOperand());
            if (!m_arraysPoppedInLoops->contains(popped))
                m_arraysPoppedInLoops->append(popped);
        }
    }
    return !m_arraysPoppedInLoops->isEmpty() && m_arraysPoppedInLoops->contains(rootOf(array));
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::goesOnBehindJump(size_t positionInOrder) const
{
    if (!positionInOrder)
        return false;
    BlockID id = m_order[positionInOrder];
    BlockID inFront = m_order[positionInOrder - 1];
    if (m_singlePredecessors[id.index()] != inFront)
        return false;
    const Block& previousBlock = code().at(inFront);
    const Block& block = code().at(id);
    if (previousBlock.terminator.kind != TerminatorKind::Jump || previousBlock.frequency != block.frequency)
        return false;
    return std::ranges::none_of(block.parameters(), [&](ValueID parameter) {
        return code().isNeeded(parameter) && hasRegister(representationOf(parameter));
    });
}

void CodeGenerator::createBlocks()
{
    m_blocks = allocateZeroedSpan<LBasicBlock>(m_proc.arena(), code().numberOfBlocks());
    for (size_t position = 0; position < m_order.size(); ++position) {
        if (!goesOnBehindJump(position) && !goesOnInBlockOfEntry(m_order[position]))
            m_blocks[m_order[position].index()] = m_out.newBlock();
    }
}

static constexpr OpcodeSet sumsThatMayStayBoxed { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };

size_t CodeGenerator::placeAmongJumpArguments(BlockID home, ValueID parameter)
{
    static constexpr size_t fewArguments = 16;
    std::span<const ValueID> arguments = code().at(code().at(home).terminator.edge(0)).arguments();
    if (arguments.size() <= fewArguments) {
        auto found = std::ranges::find(arguments, parameter);
        return found == arguments.end() ? notFound : found - arguments.begin();
    }
    m_blocksOfJumpArguments.ensureSize(code().numberOfBlocks());
    if (!m_blocksOfJumpArguments.quickSet(home.index())) {
        for (size_t place = 0; place < arguments.size(); ++place) {
            if (arguments[place] && !code().isConstant(arguments[place]) && code().blockOf(arguments[place]) == home)
                m_placesAmongJumpArguments.add(arguments[place], place);
        }
    }
    auto found = m_placesAmongJumpArguments.find(parameter);
    return found == m_placesAmongJumpArguments.end() ? notFound : found->value;
}

bool CodeGenerator::isReadByBoxAlone(BlockID home, ValueID parameter, unsigned stepsLeft)
{
    if (code().useCountOf(parameter) != 1)
        return false;
    for (const auto& entry : code().entries()) {
        if (entry.block == home)
            return false;
    }
    if (ValueID user = code().onlyUserOf(parameter)) {
        const Operation& box = code().at(user);
        std::span<const ValueID> values = code().valuesOf(home);
        return box.opcode == Opcode::Box && box.representation == Representation::JSValue && !values.empty() && values.front() == user;
    }
    const Terminator& terminator = code().at(home).terminator;
    if (!stepsLeft || terminator.kind != TerminatorKind::Jump)
        return false;
    size_t place = placeAmongJumpArguments(home, parameter);
    if (place == notFound)
        return false;
    BlockID target = code().at(terminator.edge(0)).to;
    ValueID next = code().at(target).parameters()[place];
    return representationOf(next) == Representation::Double && code().isNeeded(next) && isReadByBoxAlone(target, next, stepsLeft - 1);
}

bool CodeGenerator::unboxesNumber(const Operation& operation) const
{
    return operation.opcode == Opcode::Unbox && operation.representation == Representation::Double && representationOf(operation.valueOperand()) == Representation::JSValue && typeOf(operation.valueOperand()).isNumber() && !isCarriedUnboxed(operation.valueOperand());
}

ValueID CodeGenerator::boxPassedToJoin(BlockID from, ValueID argument) const
{
    const Operation& unboxing = code().at(argument);
    if (!m_carriesParametersBoxed || code().isConstant(argument) || !unboxesNumber(unboxing) || unboxing.block() != from || code().useCountOf(argument) != 1)
        return { };
    const Terminator& terminator = code().at(from).terminator;
    if (terminator.kind != TerminatorKind::Jump)
        return { };
    const Edge& edge = code().at(terminator.next());
    size_t place = std::ranges::find(edge.arguments(), argument) - edge.arguments().begin();
    return place < edge.arguments().size() && isCarriedBoxed(code().at(edge.to).parameters()[place]) ? unboxing.valueOperand() : ValueID();
}

bool CodeGenerator::isReadByBoxedSumAlone(BlockID home, ValueID parameter) const
{
    const Block& block = code().at(home);
    if (code().useCountOf(parameter) != 1 || block.isLoopHead || block.isEntry || block.isHandlerBlock() || m_keepsValuesInHeapFrame || (unit().role == CodeRole::CoroutineBody && code().hasSuspensions()))
        return false;
    ValueID sum = code().onlyUserOf(parameter);
    if (!sum || !sumsThatMayStayBoxed.contains(opcodeOf(sum)) || !typeOf(parameter).isNumber())
        return false;
    const Operation& operation = code().at(sum);
    ValueID box = code().onlyUserOf(sum);
    if (operation.form() != OperatorForm::Numbers || operation.representation != Representation::Double || operation.block() != home || code().useCountOf(sum) != 1)
        return false;
    if (!box || opcodeOf(box) != Opcode::Box || code().at(box).representation != Representation::JSValue || code().at(box).block() != home)
        return false;
    auto isInt32 = [&](ValueID id) {
        return code().isConstant(id) && opcodeOf(id) == Opcode::Integer && isRepresentableAs<int32_t>(code().at(id).integer());
    };
    if (operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub) {
        if (!isInt32(operation.leftOperand() == parameter ? operation.rightOperand() : operation.leftOperand()))
            return false;
    }
    size_t place = std::ranges::find(block.parameters(), parameter) - block.parameters().begin();
    bool passesBox = false;
    for (EdgeID id = block.firstEdgeIn(); id; id = code().at(id).nextInto()) {
        const Edge& edge = code().at(id);
        std::span<const ValueID> arguments = edge.arguments();
        if (place >= arguments.size() || !arguments[place])
            return false;
        if (isInt32(arguments[place]))
            continue;
        const Operation& argument = code().at(arguments[place]);
        if (code().isConstant(arguments[place]) || !unboxesNumber(argument) || argument.block() != edge.from || code().useCountOf(arguments[place]) != 1 || code().at(edge.from).terminator.kind != TerminatorKind::Jump)
            return false;
        passesBox = true;
    }
    return passesBox;
}

bool CodeGenerator::isFunctionCalledRepeatedly() const
{
    if (m_isFunctionCalledRepeatedly)
        return *m_isFunctionCalledRepeatedly;
    const FunctionID id = code().function();
    auto readsOwnName = [&] {
        if (function().kind != FunctionKind::Method || !code().opcodesOfValues().contains(Opcode::PropGet))
            return false;
        for (uint32_t index = 0; index < code().numberOfValues(); ++index) {
            const Operation& operation = code().at(ValueID(index));
            if (operation.opcode != Opcode::PropGet || operation.isFreeName())
                continue;
            const PropertyKey& key = program().at(operation.key());
            if (key.kind == PropertyKeyKind::Text && key.text == function().name)
                return true;
        }
        return false;
    };
    m_isFunctionCalledRepeatedly = id && (facts().linked().function(id).isIterationCallback || m_isTrapOfProxy || code().callsItself() || readsOwnName());
    return *m_isFunctionCalledRepeatedly;
}

bool CodeGenerator::isStoredIntoByName(ValueID made) const
{
    BlockID block = code().blockOf(made);
    for (unsigned stepsLeft = 8; stepsLeft--;) {
        ValueID boxed;
        for (ValueID id : code().valuesOf(block)) {
            const Operation& operation = code().at(id);
            if (operation.opcode == Opcode::Box && operation.operand(0) == made)
                boxed = id;
            if (operation.opcode == Opcode::PropSet && (operation.objectOperand() == made || (boxed && operation.objectOperand() == boxed)))
                return true;
        }
        const Terminator& terminator = code().at(block).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        const Edge& edge = code().at(terminator.next());
        std::span<const ValueID> arguments = edge.arguments();
        std::span<const ValueID> parameters = code().at(edge.to).parameters();
        size_t position = std::ranges::find_if(arguments, [&](ValueID argument) {
            return argument == made || (boxed && argument == boxed);
        }) - arguments.begin();
        if (position < std::min(arguments.size(), parameters.size()))
            made = parameters[position];
        block = edge.to;
    }
    return false;
}

BlockID CodeGenerator::usualEntranceOf(BlockID id) const
{
    BlockID above = m_singlePredecessors[id.index()];
    if (!above)
        return id;
    bool otherPathsAreUnlikely = true;
    code().forEachEdgeOf(above, [&](EdgeID, const Edge& edge) {
        otherPathsAreUnlikely = otherPathsAreUnlikely && (edge.to == id || code().at(edge.to).frequency == BlockFrequency::Rare);
    });
    return otherPathsAreUnlikely ? above : id;
}

void CodeGenerator::findKeysCheckedTwice()
{
    static constexpr OpcodeSet accessesByKey { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement };
    if (m_keepsValuesInHeapFrame || !code().hasLoops() || !code().opcodesOfValues().intersects(accessesByKey))
        return;
    m_blocksOfFirstAccesses.clear();
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.frequency == BlockFrequency::Rare)
            continue;
        for (ValueID value : code().valuesOf(block)) {
            const Operation& access = code().at(value);
            if (!accessesByKey.contains(access.opcode) || access.keyIsIntegral())
                continue;
            ValueID key = access.positionOperand();
            Representation representation = representationOf(key);
            if ((representation != Representation::Double && representation != Representation::OptionalDouble) || code().isConstant(key))
                continue;
            BlockID entrance = usualEntranceOf(id);
            auto first = m_blocksOfFirstAccesses.add(key.index(), entrance);
            if (first.isNewEntry || (first.iterator->value != entrance && !dominates(first.iterator->value, entrance)))
                continue;
            if (!std::exchange(m_hasKeysAskedTwice, true)) {
                m_looksBehindEachOperation = true;
                m_keysAskedTwice.clearAll();
                m_keysAskedTwice.ensureSize(code().numberOfValues());
            }
            m_keysAskedTwice.quickSet(key.index());
        }
    }
}

std::optional<BitVector> CodeGenerator::blocksOfLoop(BlockID head)
{
    BitVector found(code().numberOfBlocks());
    Vector<BlockID, 16> pending;
    auto add = [&](BlockID block) {
        if (code().isReached(block) && !found.quickSet(block.index()))
            pending.append(block);
    };
    found.quickSet(head.index());
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        if (code().isReached(edge.from) && blockDominates(head, edge.from))
            add(edge.from);
    });
    if (pending.isEmpty())
        return std::nullopt;
    while (!pending.isEmpty()) {
        code().forEachIncomingEdgeUnsealed(pending.takeLast(), [&](const Edge& edge) {
            add(edge.from);
        });
    }
    return found;
}

bool CodeGenerator::leadsToInnerLoop(BlockID id)
{
    static constexpr unsigned mostSteps = 24;
    BlockID at = id;
    for (unsigned step = 0; step < mostSteps; ++step) {
        const Terminator& end = code().at(at).terminator;
        if (end.kind != TerminatorKind::Branch && end.kind != TerminatorKind::Jump)
            return false;
        at = code().at(end.kind == TerminatorKind::Branch ? end.taken() : end.next()).to;
        if (code().at(at).isLoopHead)
            return !blockDominates(at, id);
    }
    return false;
}

bool CodeGenerator::isHeaderTested(ValueID test) const
{
    LBasicBlock passed = m_headerTests.get(test.index()).passed;
    return passed && passed == m_out.m_block;
}

ValueID CodeGenerator::findNextHeaderTest(ValueID test, Opcode opcode) const
{
    BlockID id = code().at(test).block();
    const Block& block = code().at(id);
    if (!block.isInLoop || m_keepsValuesInHeapFrame || testReadByBranchAlone(id, block.terminator) != test)
        return { };
    BlockID to = code().at(block.terminator.taken()).to;
    const Block& next = code().at(to);
    if (m_singlePredecessors[to.index()] != id || !next.parameters().empty() || next.isLoopHead || next.isEntry || next.isHandlerBlock())
        return { };
    ValueID later = testReadByBranchAlone(to, next.terminator);
    if (!later || opcodeOf(later) != opcode)
        return { };
    ValueID array = withoutNames(code().at(test).operand(0));
    bool onlyNames = std::ranges::all_of(code().valuesOf(to), [&](ValueID value) {
        return value == later || ((opcodeOf(value) == Opcode::Unbox || opcodeOf(value) == Opcode::Narrowed) && withoutNames(value) == array);
    });
    if (!onlyNames || withoutNames(code().at(later).operand(0)) != array)
        return { };
    auto destination = [&](EdgeID edge) {
        const Edge* left = &code().at(edge);
        const Block& failed = code().at(left->to);
        bool onlyJumps = left->arguments().empty() && code().valuesOf(left->to).empty() && failed.parameters().empty() && failed.terminator.kind == TerminatorKind::Jump && !failed.isLoopHead && !failed.isEntry && !failed.isHandlerBlock();
        return onlyJumps ? &code().at(failed.terminator.next()) : left;
    };
    const Edge* one = destination(block.terminator.notTaken());
    const Edge* other = destination(next.terminator.notTaken());
    return one->to == other->to && std::ranges::equal(one->arguments(), other->arguments()) ? later : ValueID();
}

ValueID CodeGenerator::findPristineTestAfter(ValueID typeTest) const
{
    ValueID test = findNextHeaderTest(typeTest, Opcode::IsPristine);
    if (!test || code().at(test).builtinClass() != BuiltinClassID(BuiltinClasses::Array) || program().admitsReplacedBuiltins)
        return { };
    OptionSet<PristineAspects> aspects = code().at(test).aspects();
    aspects.remove({ PristineAspects::Storage, PristineAspects::Names });
    return aspects.isEmpty() ? test : ValueID();
}

ValueID CodeGenerator::findStorageTestAfter(ValueID pristineTest, bool followsTypeTest) const
{
    ValueID test = findNextHeaderTest(pristineTest, Opcode::HasArrayStorage);
    if (!test)
        return { };
    const Operation& storage = code().at(test);
    if ((!followsTypeTest && storage.arrayStorage() != ArrayStorage::Double && !storage.isWritable()) || !findArrayStorageForm(storage.arrayStorage()) || storage.admitsInt32Storage() || storage.data())
        return { };
    return test;
}

std::optional<CodeGenerator::RowGuard> CodeGenerator::findRowGuardAt(BlockID id, RowKind kind)
{
    const Block& block = code().at(id);
    ValueID present = block.isInLoop && block.frequency == BlockFrequency::Normal ? testReadByBranchAlone(id, block.terminator) : ValueID();
    if (!present || opcodeOf(present) != Opcode::IsElementPresent)
        return std::nullopt;
    const Operation& presence = code().at(present);
    RowGuard guard;
    guard.presenceBlock = id;
    guard.array = presence.arrayOperand();
    guard.position = presence.positionOperand();
    if (presence.arrayStorage() != ArrayStorage::Contiguous || code().isConstant(guard.array) || code().isConstant(guard.position) || representationOf(guard.array) != Representation::Cell)
        return std::nullopt;
    if (representationOf(guard.position) != Representation::Int32 && (representationOf(guard.position) != Representation::Int64 || typeOf(guard.position).integers.minimum < 0))
        return std::nullopt;

    static constexpr unsigned mostChecks = 5;
    bool isArray = false;
    bool isPristine = false;
    bool isNumeric = false;
    RowCount& asked = guard.asked;
    constexpr unsigned shiftOfType = 8 * (JSCell::typeInfoTypeOffset() - JSCell::indexingTypeAndMiscOffset());
    constexpr unsigned shiftOfFlags = 8 * (JSCell::typeInfoFlagsOffset() - JSCell::indexingTypeAndMiscOffset());
    asked.mask = 0xffu << shiftOfType | static_cast<uint32_t>(JSC::IsPristine) << shiftOfFlags;
    asked.header = static_cast<uint32_t>(ArrayType) << shiftOfType | static_cast<uint32_t>(JSC::IsPristine) << shiftOfFlags;
    auto isRow = [&](ValueID value) {
        return guard.names.contains(value);
    };
    for (BlockID from = id; kind == RowKind::Any ? !isPristine : !isNumeric;) {
        BlockID to = code().at(code().at(from).terminator.taken()).to;
        const Block& next = code().at(to);
        if (guard.checkBlocks.size() == mostChecks || m_singlePredecessors[to.index()] != from || !next.parameters().empty() || next.isLoopHead || next.isEntry || next.isHandlerBlock() || next.frequency != BlockFrequency::Normal)
            return std::nullopt;
        ValueID test = testReadByBranchAlone(to, next.terminator);
        if (!test)
            return std::nullopt;
        for (ValueID value : code().valuesOf(next)) {
            const Operation& operation = code().at(value);
            switch (operation.opcode) {
            case Opcode::GetArrayElement:
                if (guard.row || operation.arrayOperand() != guard.array || operation.positionOperand() != guard.position || operation.arrayStorage() != ArrayStorage::Contiguous || !operation.isPresent() || operation.representation != Representation::JSValue)
                    return std::nullopt;
                guard.row = value;
                guard.names.append(value);
                break;
            case Opcode::Narrowed:
            case Opcode::Unbox:
                if (!isRow(operation.valueOperand()) || (operation.representation != Representation::JSValue && operation.representation != Representation::Cell))
                    return std::nullopt;
                guard.names.append(value);
                break;
            case Opcode::CheckProof:
                if (!isRow(operation.valueOperand()))
                    return std::nullopt;
                break;
            case Opcode::TypeTest:
                if (value != test || !isRow(operation.valueOperand()) || !hasKind(inputs().types, operation.testedType(), OperandKind::Array))
                    return std::nullopt;
                isArray = true;
                break;
            case Opcode::IsPristine: {
                OptionSet<PristineAspects> aspects = operation.aspects();
                aspects.remove({ PristineAspects::Storage, PristineAspects::Names });
                if (value != test || !isRow(operation.valueOperand()) || operation.builtinClass() != BuiltinClasses::Array || !aspects.isEmpty() || !isArray)
                    return std::nullopt;
                isPristine = true;
                break;
            }
            case Opcode::HasArrayStorage: {
                if (value != test || !isRow(operation.arrayOperand()) || operation.arrayStorage() != ArrayStorage::Double || operation.admitsInt32Storage() != (kind == RowKind::Numbers) || operation.data() || !isPristine)
                    return std::nullopt;
                static constexpr uint32_t bitOfDoubles = DoubleShape & ~Int32Shape;
                uint32_t ignored = kind == RowKind::Numbers ? bitOfDoubles : 0;
                asked.mask |= (operation.isWritable() ? 0xff : IndexingShapeMask) & ~ignored;
                asked.header |= (operation.isWritable() ? ArrayWithDouble : DoubleShape) & ~ignored;
                isNumeric = true;
                break;
            }
            default:
                return std::nullopt;
            }
        }
        if (!guard.row)
            return std::nullopt;
        guard.checkBlocks.append(to);
        from = to;
    }
    guard.continuation = code().at(code().at(guard.checkBlocks.last()).terminator.taken()).to;
    const Block& continuation = code().at(guard.continuation);
    if (m_singlePredecessors[guard.continuation.index()] != guard.checkBlocks.last() || !continuation.parameters().empty() || continuation.isLoopHead || continuation.isEntry || continuation.isHandlerBlock())
        return std::nullopt;
    if (!findLoopOfRows(guard))
        return std::nullopt;

    asked.array = withoutNames(guard.array);
    return guard;
}

bool CodeGenerator::findLoopOfRows(RowGuard& guard)
{
    static constexpr OpcodeSet conversions { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::NumberToInteger };
    auto withoutConversions = [&](ValueID value) {
        while (!code().isConstant(value) && conversions.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    ValueID counter = withoutConversions(guard.position);
    if (code().isConstant(counter) || opcodeOf(counter) != Opcode::Parameter)
        return false;
    BlockID head = code().blockOf(counter);
    if (!head || !code().at(head).isLoopHead)
        return false;
    std::span<const ValueID> parameters = code().at(head).parameters();
    size_t place = std::ranges::find(parameters, counter) - parameters.begin();
    bool isCounted = place < parameters.size();
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        ValueID passed = place < edge.arguments().size() ? edge.arguments()[place] : ValueID();
        if (!isCounted || !passed || !code().isReached(edge.from)) {
            isCounted = false;
            return;
        }
        const Operation& operation = code().at(withoutConversions(passed));
        if (operation.opcode == Opcode::Integer) {
            isCounted = !operation.integer() && !guard.preheader && code().at(edge.from).terminator.kind == TerminatorKind::Jump && code().at(edge.from).isInLoop;
            guard.preheader = edge.from;
            return;
        }
        if (operation.opcode == Opcode::Increment)
            isCounted = (operation.form() == OperatorForm::Integers || operation.form() == OperatorForm::CheckedIntegers) && withoutConversions(operation.valueOperand()) == counter;
        else
            isCounted = operation.opcode == Opcode::Add && operation.form() == OperatorForm::Integers && withoutConversions(operation.leftOperand()) == counter && opcodeOf(operation.rightOperand()) == Opcode::Integer && code().at(operation.rightOperand()).integer() == 1;
    });
    if (!isCounted || !guard.preheader)
        return false;
    const Terminator* end = &code().at(head).terminator;
    if (end->kind == TerminatorKind::Jump)
        end = &code().at(code().at(end->next()).to).terminator;
    if (end->kind != TerminatorKind::Branch)
        return false;
    const Operation& relation = code().at(end->conditionOperand());
    if (relation.opcode != Opcode::Less || relation.form() != OperatorForm::Integers || withoutConversions(relation.leftOperand()) != counter)
        return false;
    guard.bound = relation.rightOperand();
    auto standsInFront = [&](ValueID value) {
        return code().isConstant(value) || (code().blockOf(value) && code().blockOf(value) != head && blockDominates(code().blockOf(value), guard.preheader));
    };
    return representationOf(guard.bound) == representationOf(counter) && standsInFront(guard.bound) && standsInFront(guard.array);
}

bool CodeGenerator::isReadAfterFailure(const KindTried& tried)
{
    BlockID passed = code().at(code().at(tried.start).terminator.taken()).to;
    std::span<const ValueID> tests = code().valuesOf(tried.start);
    auto isOfStart = [&](ValueID value) {
        return std::ranges::contains(tests, value);
    };
    for (BlockID id : m_order) {
        if (id == tried.start || !blockDominates(tried.start, id) || blockDominates(passed, id))
            continue;
        bool isRead = std::ranges::any_of(code().at(id).terminator.operands(), isOfStart);
        for (ValueID value : code().valuesOf(id))
            isRead |= std::ranges::any_of(code().at(value).operands(), isOfStart);
        code().forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            isRead |= std::ranges::any_of(edge.arguments(), isOfStart);
        });
        if (isRead)
            return true;
    }
    return false;
}

std::optional<CodeGenerator::KindTried> CodeGenerator::findKindTriedAt(BlockID test)
{
    static constexpr OpcodeSet looks {
        Opcode::HasStructureOfClass, Opcode::ShapeTest, Opcode::HasLayoutPrototype, Opcode::AreMembersUnchanged, Opcode::GetPropertyOfHeir, Opcode::TypeTest, Opcode::IsPristine, Opcode::HasArrayStorage, Opcode::GetArrayStorage,
        Opcode::GetArrayLength, Opcode::IsElementPresent, Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::NarrowInteger, Opcode::WidenInteger, Opcode::NumberToInteger, Opcode::Less,
    };
    KindTried tried;
    tried.other = code().at(code().at(test).terminator.notTaken()).to;
    tried.start = test;
    for (BlockID before = test; before;) {
        const Block& block = code().at(before);
        if (block.terminator.kind != TerminatorKind::Branch || code().at(block.terminator.notTaken()).to != tried.other || (before != test && code().at(block.terminator.taken()).to != tried.start))
            break;
        bool onlyLooks = std::ranges::all_of(code().valuesOf(before), [&](ValueID value) {
            const Operation& operation = code().at(value);
            return looks.contains(operation.opcode) || (operation.opcode == Opcode::GetArrayElement && operation.level() == KnowledgeLevel::Proven && operation.isPresent());
        });
        if (!onlyLooks || !block.parameters().empty())
            break;
        tried.start = before;
        before = m_singlePredecessors[before.index()];
    }
    const Block& other = code().at(tried.other);
    if (tried.start == test || !other.parameters().empty() || other.isLoopHead || other.isEntry || other.isHandlerBlock() || m_singlePredecessors[tried.other.index()] || isReadAfterFailure(tried))
        return std::nullopt;

    BlockID headAround;
    std::optional<BitVector> loopAround;
    for (BlockID head : m_order) {
        if (!code().at(head).isLoopHead || !blockDominates(head, tried.start) || (headAround && !blockDominates(headAround, head)))
            continue;
        std::optional<BitVector> blocks = blocksOfLoop(head);
        if (!blocks || !blocks->quickGet(tried.start.index()))
            continue;
        headAround = head;
        loopAround = WTF::move(blocks);
    }
    if (!headAround)
        return std::nullopt;
    code().forEachIncomingEdgeUnsealed(headAround, [&](const Edge& edge) {
        if (code().isReached(edge.from) && !loopAround->quickGet(edge.from.index()) && !tried.preheaders.contains(edge.from))
            tried.preheaders.append(edge.from);
    });
    Vector<BlockID, 4> testsForDoubles;
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        if (block.terminator.kind != TerminatorKind::Branch || !loopAround->quickGet(id.index()) || !blockDominates(tried.other, id))
            continue;
        ValueID condition = testReadByBranchAlone(id, block.terminator);
        if (condition && opcodeOf(condition) == Opcode::HasArrayStorage && code().at(condition).arrayStorage() == ArrayStorage::Double)
            testsForDoubles.append(id);
    }
    for (BlockID id : testsForDoubles) {
        BlockID failure = code().at(code().at(id).terminator.notTaken()).to;
        const Block& block = code().at(failure);
        bool leadsToOther = std::ranges::any_of(testsForDoubles, [&](BlockID other) { return blockDominates(failure, other); });
        if (!leadsToOther && !block.isLoopHead && !block.isEntry && !block.isHandlerBlock() && !tried.failures.contains(failure))
            tried.failures.append(failure);
    }
    if (tried.preheaders.isEmpty() || tried.failures.isEmpty())
        return std::nullopt;
    return tried;
}

void CodeGenerator::findKindsTried()
{
    if (m_keepsValuesInHeapFrame || code().hasSuspensions())
        return;
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.frequency != BlockFrequency::Normal || block.terminator.kind != TerminatorKind::Branch)
            continue;
        ValueID test = block.terminator.conditionOperand();
        if (code().isConstant(test) || opcodeOf(test) != Opcode::HasArrayStorage || code().at(test).arrayStorage() != ArrayStorage::Int32 || !testReadByBranchAlone(id, block.terminator))
            continue;
        BlockID other = code().at(block.terminator.notTaken()).to;
        size_t known = m_kindsTried ? m_kindsTried->kinds.findIf([&](const KindTried& tried) { return tried.other == other; }) : notFound;
        if (known != notFound) {
            m_kindsTried->kinds[known].tests.append(id);
            continue;
        }
        std::optional<KindTried> tried = findKindTriedAt(id);
        if (!tried)
            continue;
        if (!m_kindsTried)
            m_kindsTried = std::make_unique<KindsTried>();
        tried->tests.append(id);
        expectOtherJumps(tried->other);
        build().statistics().count(StatisticID::KindsTriedFirstByIterations);
        m_kindsTried->kinds.append(WTF::move(*tried));
    }
    if (m_kindsTried)
        m_kindsTried->slot = m_proc.addStackSlot(safeCast<unsigned>(m_kindsTried->kinds.size() * sizeof(uint64_t)));
}

TypedPointer CodeGenerator::kindTriedAddress(size_t place)
{
    return m_out.address(heaps().frameSlots, m_out.slotBase(m_kindsTried->slot), place * sizeof(uint64_t));
}

void CodeGenerator::enterBlockWithKindsTried(BlockID id)
{
    if (m_headOfTurnsLaidOut || m_interleaving)
        return;
    for (size_t place = 0; place < m_kindsTried->kinds.size(); ++place) {
        const KindTried& tried = m_kindsTried->kinds[place];
        if (tried.failures.contains(id))
            m_out.store32(m_out.constInt32(static_cast<int32_t>(TriedFirst::Int32sAlways)), kindTriedAddress(place));
        if (tried.start != id)
            continue;
        LBasicBlock begins = m_out.newBlock();
        m_out.branch(m_out.equal(m_out.load32(kindTriedAddress(place)), m_out.constInt32(static_cast<int32_t>(TriedFirst::Doubles))), normalEdge(blockFor(tried.other)), normalEdge(begins));
        m_out.appendTo(begins);
    }
}

void CodeGenerator::beginKindsTried(const TerminatorContext& terminator)
{
    BlockID id = terminator.block();
    for (size_t place = 0; place < m_kindsTried->kinds.size(); ++place) {
        KindTried& tried = m_kindsTried->kinds[place];
        if (tried.preheaders.contains(id))
            m_out.store32(m_out.int32Zero, kindTriedAddress(place));
        if (!tried.tests.contains(id) || m_headOfTurnsLaidOut || m_interleaving)
            continue;
        tried.failed = m_out.newBlock();
        tried.blockOfOther = blockFor(tried.other);
        m_blocks[tried.other.index()] = tried.failed;
        m_kindsTried->testing = place;
    }
}

void CodeGenerator::endKindTried()
{
    size_t place = std::exchange(m_kindsTried->testing, notFound);
    KindTried& tried = m_kindsTried->kinds[place];
    m_blocks[tried.other.index()] = tried.blockOfOther;
    m_out.appendTo(tried.failed);
    m_out.store32(m_out.bitOr(m_out.load32(kindTriedAddress(place)), m_out.constInt32(static_cast<int32_t>(TriedFirst::Doubles))), kindTriedAddress(place));
    m_out.jump(normalEdge(tried.blockOfOther));
}

bool CodeGenerator::isReadAfterRejoin(const RowGuard& guard)
{
    BitVector isReached(code().numberOfBlocks());
    Vector<BlockID, 16> pending { guard.continuation };
    isReached.quickSet(guard.continuation.index());
    while (!pending.isEmpty()) {
        code().forEachEdgeOf(pending.takeLast(), [&](EdgeID, const Edge& edge) {
            if (edge.to != guard.checkBlocks[0] && code().isReached(edge.to) && !isReached.quickSet(edge.to.index()))
                pending.append(edge.to);
        });
    }
    bool isRead = false;
    auto read = [&](ValueID value) {
        isRead |= guard.names.contains(value);
    };
    for (BlockID id : m_order) {
        if (!isReached.quickGet(id.index()) || blockDominates(guard.continuation, id))
            continue;
        for (ValueID value : code().valuesOf(id)) {
            for (ValueID operand : code().at(value).operands())
                read(operand);
        }
        for (ValueID operand : code().at(id).terminator.operands())
            read(operand);
        code().forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments())
                read(argument);
        });
        if (isRead)
            return true;
    }
    return false;
}

void CodeGenerator::findRowGuards()
{
    RELEASE_ASSERT(!m_rows);
    if (m_keepsValuesInHeapFrame || code().hasSuspensions() || program().admitsReplacedBuiltins)
        return;
    static constexpr unsigned mostCounts = 4;
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.terminator.kind != TerminatorKind::Branch || opcodeOf(block.terminator.conditionOperand()) != Opcode::IsElementPresent)
            continue;
        for (RowKind kind : { RowKind::Numbers, RowKind::Doubles, RowKind::Any }) {
            std::optional<RowGuard> guard = findRowGuardAt(id, kind);
            if (!guard || isReadAfterRejoin(*guard))
                continue;
            if (!m_rows)
                m_rows = std::make_unique<CheckedRows>();
            size_t count = m_rows->counts.findIf([&](const RowCount& other) {
                return other.array == guard->asked.array && other.mask == guard->asked.mask && other.header == guard->asked.header;
            });
            if (count == notFound) {
                if (m_rows->counts.size() == mostCounts)
                    break;
                count = m_rows->counts.size();
                m_rows->counts.append(guard->asked);
            }
            guard->count = count;
            if (kind == RowKind::Any && !m_rows->guards.isEmpty() && m_rows->guards.last().presenceBlock == id) {
                m_rows->guards.last().precedesOther = true;
                m_rows->guards.last().numberOfSharedNames = guard->names.size();
            }
            m_rows->guards.append(WTF::move(*guard));
            if (kind != RowKind::Doubles)
                break;
        }
    }
    if (!m_rows)
        return;

    m_rows->slotOfCounts = m_proc.addStackSlot(safeCast<unsigned>(m_rows->counts.size() * RowCount::size));
    BitVector isBeforeGuards(code().numberOfBlocks());
    m_rows->changingBlocks = BitVector(code().numberOfBlocks());
    m_rows->blocksBeforeChangingLoops = BitVector(code().numberOfBlocks());
    Vector<BlockID, 16> pending;
    auto add = [&](BlockID block) {
        if (code().isReached(block) && !isBeforeGuards.quickSet(block.index()))
            pending.append(block);
    };
    for (const RowGuard& guard : m_rows->guards) {
        add(guard.presenceBlock);
        add(guard.preheader);
        expectOtherJumps(guard.continuation);
        build().statistics().count(StatisticID::RowsCountedByChecks);
    }
    while (!pending.isEmpty()) {
        code().forEachIncomingEdgeUnsealed(pending.takeLast(), [&](const Edge& edge) {
            add(edge.from);
        });
    }
    for (BlockID id : m_order) {
        if (id == m_order[0] || code().at(id).isEntry || (isBeforeGuards.quickGet(id.index()) && mayChangeRows(id)))
            m_rows->changingBlocks.quickSet(id.index());
    }
    Vector<std::pair<BlockID, BitVector>, 8> loops;
    for (BlockID head : m_order) {
        std::optional<BitVector> blocks = code().at(head).isLoopHead && isBeforeGuards.quickGet(head.index()) ? blocksOfLoop(head) : std::nullopt;
        if (blocks)
            loops.append({ head, WTF::move(*blocks) });
    }
    auto changesRowsInPlace = [&](const std::pair<BlockID, BitVector>& loop) {
        return std::ranges::any_of(m_order, [&](BlockID member) {
            return loop.second.quickGet(member.index()) && code().at(member).frequency == code().at(loop.first).frequency && m_rows->changingBlocks.quickGet(member.index()) && std::ranges::none_of(code().valuesOf(member), [&](ValueID value) { return code().at(value).traits().mayRunOtherCode(); });
        });
    };
    for (const auto& loop : loops) {
        bool changesRows = changesRowsInPlace(loop);
        code().forEachIncomingEdgeUnsealed(loop.first, [&](const Edge& edge) {
            if (loop.second.quickGet(edge.from.index()))
                return;
            size_t around = loops.size();
            for (size_t place = 0; place < loops.size(); ++place) {
                if (loops[place].second.quickGet(edge.from.index()))
                    around = place;
            }
            if (changesRows || (around < loops.size() && changesRowsInPlace(loops[around])))
                m_rows->blocksBeforeChangingLoops.quickSet(edge.from.index());
        });
    }
}

ValueID CodeGenerator::withoutNames(ValueID cell) const
{
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::Box };
    while (!code().isConstant(cell) && names.contains(opcodeOf(cell)))
        cell = code().at(cell).operand(0);
    return cell;
}

bool CodeGenerator::isArrayWithoutRows(ValueID array)
{
    static constexpr OpcodeSet allocations { Opcode::NewArray, Opcode::NewArrayOfGiven, Opcode::NewArrayLike };
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::Box };
    static constexpr OpcodeSet accesses {
        Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex, Opcode::HasArrayStorage, Opcode::IsPristine, Opcode::IsElementPresent,
        Opcode::GetArrayLength, Opcode::GetArrayStorage, Opcode::GetArrayCapacity, Opcode::ConvertToDoubleStorage, Opcode::ReserveArrayCapacity, Opcode::TypeTest,
    };
    array = withoutNames(array);
    if (code().isConstant(array) || !allocations.contains(opcodeOf(array)) || std::ranges::any_of(m_rows->counts, [&](const RowCount& count) { return count.array == array; }))
        return false;
    for (const auto& [known, isUnescaped] : m_rows->unescapedArrays) {
        if (known == array)
            return isUnescaped;
    }
    bool isUnescaped = true;
    auto isOfArray = [&](ValueID value) {
        return value && withoutNames(value) == array;
    };
    for (BlockID id : m_order) {
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            std::span<const ValueID> operands = operation.operands();
            for (size_t place = 0; place < operands.size(); ++place) {
                if (isOfArray(operands[place]))
                    isUnescaped &= !place && (names.contains(operation.opcode) || accesses.contains(operation.opcode));
            }
        }
        for (ValueID operand : code().at(id).terminator.operands())
            isUnescaped &= !isOfArray(operand);
        code().forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments())
                isUnescaped &= !isOfArray(argument);
        });
        if (!isUnescaped)
            break;
    }
    m_rows->unescapedArrays.append({ array, isUnescaped });
    return isUnescaped;
}

bool CodeGenerator::mayChangeRows(BlockID block, ValueID id, const Operation& operation)
{
    if (operation.traits().isPure() || operation.opcode == Opcode::PutDoubleIndex || operation.opcode == Opcode::ConvertToDoubleStorage || (operation.opcode == Opcode::PutContiguousIndex && operation.hasNumericStorage()))
        return false;
    if (operation.opcode == Opcode::PutContiguousIndex && isArrayWithoutRows(operation.arrayOperand()))
        return false;
    ValueContext context(block, id, operation, handlerOf(block));
    return !changesRowsByCalls(context) && mayChangeRows(context);
}

bool CodeGenerator::storesNumberInPlace(const OperationContext& context) const
{
    if (!context.valueID() || opcodeOf(context.valueID()) != Opcode::PutArrayElement)
        return false;
    const auto& value = static_cast<const ValueContext&>(context);
    return !value->isInBounds() && typeOf(value->valueOperand()).isNumber() && GuessedAccessEmitter::testsForItself(value) && storeTakesIndexOfProvenList(inputs().types, typeIDOf(value->arrayOperand()), typeIDOf(value->positionOperand()));
}

bool CodeGenerator::changesRowsByCalls(const OperationContext& context)
{
    static constexpr OpcodeSet reads { Opcode::GetArrayElement, Opcode::PropGet, Opcode::ElemGet, Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Increment, Opcode::Decrement, Opcode::ToNumber };
    if (!context.valueID())
        return false;
    const Operation& operation = code().at(context.valueID());
    return reads.contains(operation.opcode) || (operation.opcode == Opcode::PutArrayElement && (storesNumberInPlace(context) || isArrayWithoutRows(operation.arrayOperand())));
}

void CodeGenerator::zeroCountsAtCallSlow(const Effects& effects)
{
    if (m_operation && mayChangeRows(effects) && changesRowsByCalls(*m_operation))
        zeroRowCounts();
}

bool CodeGenerator::mayChangeRows(const Effects& effects)
{
    static constexpr AbstractHeapSet heapsUnderRows { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths, AbstractHeap::Elements, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks };
    return effects.mayRunOtherCode() || effects.writes.containsAny(heapsUnderRows);
}

bool CodeGenerator::mayChangeRows(const OperationContext& context) const
{
    return mayChangeRows(effectsOf(context));
}

bool CodeGenerator::mayChangeRows(BlockID id)
{
    static constexpr std::array terminatorsWithoutEffects { TerminatorKind::Jump, TerminatorKind::Branch, TerminatorKind::Switch, TerminatorKind::Unreachable };
    const Terminator& terminator = code().at(id).terminator;
    if (!std::ranges::contains(terminatorsWithoutEffects, terminator.kind) && mayChangeRows(TerminatorContext(id, terminator, handlerOf(id))))
        return true;
    return std::ranges::any_of(code().valuesOf(id), [&](ValueID value) { return mayChangeRows(id, value, code().at(value)); });
}

TypedPointer CodeGenerator::rowCountAddress(unsigned count, ptrdiff_t offset)
{
    return m_out.address(heaps().frameSlots, m_out.slotBase(m_rows->slotOfCounts), count * RowCount::size + offset);
}

void CodeGenerator::zeroRowCounts()
{
    for (unsigned count = 0; count < m_rows->counts.size(); ++count)
        m_out.store32(m_out.int32Zero, rowCountAddress(count));
}

const CodeGenerator::RowGuard* CodeGenerator::findCheckingGuardSlow(BlockID id) const
{
    for (const RowGuard& guard : m_rows->guards) {
        if (guard.checkBlocks.contains(id))
            return &guard;
    }
    return nullptr;
}

CodeGenerator::RowGuard* CodeGenerator::findGuardContinuingIn(BlockID id)
{
    if (!m_rows)
        return nullptr;
    for (RowGuard& guard : m_rows->guards) {
        if (guard.continuation == id && guard.joinedRow)
            return &guard;
    }
    return nullptr;
}

void CodeGenerator::enterBlockWithRows(BlockID id)
{
    if (m_rows->changingBlocks.quickGet(id.index()))
        zeroRowCounts();
    for (RowGuard& guard : m_rows->guards | std::views::reverse) {
        if (guard.checkedNames.isEmpty())
            continue;
        bool isBehind = blockDominates(guard.continuation, id);
        for (size_t place = isBehind ? 0 : guard.numberOfSharedNames; place < guard.names.size(); ++place)
            m_values[guard.names[place].index()] = isBehind ? guard.joinedRow : guard.checkedNames[place];
    }
}

void CodeGenerator::joinAfterChecks(RowGuard& guard)
{
    m_blocks[guard.continuation.index()] = guard.joinBlock;
    for (ValueID name : guard.names)
        guard.checkedNames.append(m_values[name.index()]);
    LBasicBlock isNext = m_out.newBlock();
    LBasicBlock hasPassed = m_out.newBlock();
    m_out.appendTo(guard.checked);
    LValue position = get(guard.position).value;
    if (position->type() == Machine::Int64)
        position = m_out.castToInt32(position);
    m_out.branch(m_out.equal(position, m_out.load32(rowCountAddress(guard.count))), normalEdge(isNext), normalEdge(hasPassed));
    m_out.appendTo(isNext);
    m_out.store32(m_out.add(position, m_out.int32One), rowCountAddress(guard.count));
    m_out.jump(normalEdge(hasPassed));
    m_out.appendTo(hasPassed);
    m_out.upsilon(m_values[guard.row.index()], guard.joinedRow);
    m_out.jump(normalEdge(guard.joinBlock));
}

void CodeGenerator::beginTerminatorWatched(const TerminatorContext& terminator)
{
    if (m_kindsTried)
        beginKindsTried(terminator);
    if (m_rows)
        beginRowGuards(terminator);
    if (m_checksEffects && terminator.handler())
        emitter<RuntimeCallEmitter>().saveDeedsForHandler({ });
}

void CodeGenerator::endTerminatorWatched()
{
    if (RowGuard* guard = std::exchange(m_guardChecked, nullptr))
        joinAfterChecks(*guard);
    if (m_kindsTried && m_kindsTried->testing != notFound)
        endKindTried();
    if (m_checksAllocation)
        m_out.clearDeclaredAccesses();
}

void CodeGenerator::beginRowGuards(const TerminatorContext& terminator)
{
    BlockID id = terminator.block();
    if (m_rows->blocksBeforeChangingLoops.quickGet(id.index()))
        zeroRowCounts();
    if (m_headOfTurnsLaidOut || m_interleaving)
        return;
    for (size_t index = 0; index < m_rows->guards.size(); ++index) {
        RowGuard& guard = m_rows->guards[index];
        bool isEnsured = std::ranges::any_of(m_rows->guards.span().first(index), [&](const RowGuard& earlier) {
            return earlier.preheader == id && earlier.count == guard.count;
        });
        if (guard.preheader != id || isEnsured)
            continue;
        LValue array = get(guard.array).value;
        LBasicBlock isOtherArray = m_out.newBlock();
        LBasicBlock isSameArray = m_out.newBlock();
        m_out.branch(m_out.equal(m_out.load64(rowCountAddress(guard.count, RowCount::offsetOfArray)), array), normalEdge(isSameArray), rareEdge(isOtherArray));
        m_out.appendTo(isOtherArray);
        m_out.store64(array, rowCountAddress(guard.count, RowCount::offsetOfArray));
        m_out.store32(m_out.int32Zero, rowCountAddress(guard.count));
        m_out.jump(normalEdge(isSameArray));
        m_out.appendTo(isSameArray);
    }
    for (RowGuard& guard : m_rows->guards) {
        if (guard.presenceBlock != id)
            continue;
        guard.scope = m_out.forkScope();
        guard.joinedRow = m_out.phiIn(blockFor(guard.continuation), machineTypeOf(Representation::JSValue));
        LBasicBlock hasPassed = m_out.newBlock();
        LBasicBlock isUncounted = m_out.newBlock();
        LValue passed = m_out.load32(rowCountAddress(guard.count));
        LValue position = get(guard.position).value;
        m_out.branch(m_out.below(position, position->type() == Machine::Int64 ? m_out.zeroExt(passed, Machine::Int64) : passed), normalEdge(hasPassed), guard.precedesOther ? normalEdge(isUncounted) : rareEdge(isUncounted));
        m_out.appendTo(hasPassed);
        generate(ValueContext(id, guard.row, code().at(guard.row), handlerOf(id)));
        LValue row = std::exchange(m_values[guard.row.index()], nullptr);
        m_out.upsilon(row, guard.joinedRow);
        m_out.jump(normalEdge(blockFor(guard.continuation)));
        m_out.appendTo(isUncounted);
    }
    for (RowGuard& guard : m_rows->guards) {
        if (guard.checkBlocks.last() != id || !guard.joinedRow)
            continue;
        guard.joinBlock = blockFor(guard.continuation);
        guard.checked = m_out.newBlock();
        m_blocks[guard.continuation.index()] = guard.checked;
        m_guardChecked = &guard;
        return;
    }
}

void CodeGenerator::noteIndexOfKey(ValueID id)
{
    if (!m_keysAskedTwice.quickGet(id.index()))
        return;
    LValue number = m_values[id.index()];
    if (number && number->type() == Machine::Double && number->owner() == m_out.m_block)
        emitter<ConversionEmitter>().noteIndexOfKey(Number(number));
}

void CodeGenerator::findRopeTolerantStrings()
{
    if (m_keepsValuesInHeapFrame || !code().opcodesOfValues().intersects(OpcodeSet { Opcode::FlattenString, Opcode::OpenString }))
        return;
    struct ReadOfString {
        ValueID string;
        BlockID block;
    };
    Vector<ReadOfString, 8> reads;
    Vector<ValueID, 4> candidates;
    Vector<ValueID, 4> readOtherwise;
    Vector<size_t, 8> positionsOfHeads;
    auto isCandidate = [&](ValueID value) {
        return value && (opcodeOf(value) == Opcode::OpenString || (opcodeOf(value) == Opcode::FlattenString && code().at(value).mayStayRope()));
    };
    for (BlockID id : m_order) {
        for (ValueID value : code().valuesOf(id)) {
            if (isCandidate(value))
                candidates.append(value);
        }
    }
    if (candidates.isEmpty())
        return;
    auto noteOtherRead = [&](ValueID value) {
        if (isCandidate(value) && opcodeOf(value) != Opcode::OpenString && !readOtherwise.contains(value))
            readOtherwise.append(value);
    };
    for (size_t position = 0; position < m_order.size(); ++position) {
        BlockID id = m_order[position];
        const Block& block = code().at(id);
        if (block.isLoopHead)
            positionsOfHeads.append(position);
        for (ValueID user : code().valuesOf(id)) {
            const Operation& operation = code().at(user);
            if (operation.opcode != Opcode::GetStringCodeUnit)
                operation.forEachChild(noteOtherRead);
            else if (isCandidate(operation.stringOperand()))
                reads.append({ operation.stringOperand(), id });
        }
        block.terminator.forEachChild(noteOtherRead);
        for (uint32_t edge = 0; edge < block.terminator.numberOfEdges; ++edge) {
            for (ValueID argument : code().at(block.terminator.edge(edge)).arguments())
                noteOtherRead(argument);
        }
    }
    for (ValueID string : candidates) {
        if (readOtherwise.contains(string))
            continue;
        BlockID home = code().blockOf(string);
        Vector<size_t, 2> positionsOfCarriers;
        bool isReadInUnrolledLoop = false;
        for (const ReadOfString& read : reads) {
            if (read.string != string)
                continue;
            std::optional<size_t> nearest;
            for (size_t position : positionsOfHeads) {
                BlockID head = m_order[position];
                if (head != home && dominates(home, head) && dominates(head, read.block))
                    nearest = position;
            }
            if (!nearest || positionsOfCarriers.contains(*nearest))
                continue;
            isReadInUnrolledLoop |= !!findCountedLoopAt(*nearest);
            positionsOfCarriers.append(*nearest);
        }
        if (isReadInUnrolledLoop)
            continue;
        m_ropeTolerantStrings.append(string);
        build().statistics().count(StatisticID::StringsResolvedOnRead);
        std::ranges::sort(positionsOfCarriers);
        for (size_t position : positionsOfCarriers) {
            BlockID head = m_order[position];
            bool isPassedArguments = std::ranges::any_of(code().at(head).parameters(), [&](ValueID parameter) { return !!m_values[parameter.index()]; });
            if (isPassedArguments)
                m_loopCarriedStrings.append({ string, head, m_out.phiIn(blockFor(head), machineTypeOf(Pointer::representation)), m_out.phiIn(blockFor(head), machineTypeOf(Word32::representation)) });
        }
    }
}

void CodeGenerator::findLoopCarriedNumbers()
{
    m_loopCarriedNumbers.shrink(0);
    m_firstLoopCarriedNumbers.clear();
    m_phisOfLoopCarriedNumbers.clear();
    m_rotatesUnrolledIterationNumbers = false;
    m_numbersOfIntegers.shrink(0);
    if (m_keepsValuesInHeapFrame || !code().hasLoops() || !code().opcodesOfValues().contains(Opcode::IntegerToNumber) || code().hasSuspensions())
        return;
    Vector<std::pair<LoopCarriedNumber, size_t>, 4> candidates;
    for (size_t position = 0; position < m_order.size(); ++position) {
        BlockID head = m_order[position];
        const Block& block = code().at(head);
        if (!block.isLoopHead)
            continue;
        std::span<const ValueID> parameters = block.parameters();
        for (size_t place = 0; place < parameters.size(); ++place) {
            ValueID parameter = parameters[place];
            if (!m_values[parameter.index()] || representationOf(parameter) != Representation::Int32 || code().useCountOf(parameter) != 1)
                continue;
            ValueID conversion = code().onlyUserOf(parameter);
            if (conversion && opcodeOf(conversion) == Opcode::IntegerToNumber && code().isNeeded(conversion) && usesLoopForms(code().blockOf(conversion)))
                candidates.append({ { parameter, head, conversion, place, nullptr }, position });
        }
    }
    if (candidates.isEmpty())
        return;

    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> blocksOfConversions;
    auto forEachBackEdge = [&](const LoopCarriedNumber& carried, const auto& functor) {
        code().forEachIncomingEdgeUnsealed(carried.head, [&](const Edge& edge) {
            if (code().isReached(edge.from) && dominates(carried.head, edge.from))
                functor(edge, edge.arguments()[carried.place]);
        });
    };
    for (const auto& [carried, position] : candidates) {
        forEachBackEdge(carried, [&](const Edge&, ValueID next) {
            if (!code().isConstant(next))
                blocksOfConversions.add(next.index(), BlockID());
        });
    }
    for (BlockID id : m_order) {
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            if (operation.opcode != Opcode::IntegerToNumber || operation.representation != Representation::Double)
                continue;
            auto found = blocksOfConversions.find(operation.valueOperand().index());
            if (found != blocksOfConversions.end() && !found->value)
                found->value = id;
        }
    }
    static constexpr size_t mostNumbersCarried = isX86_64() ? 8 : 16;
    size_t countedLoopCheckPosition = notFound;
    bool isCountedLoop = false;
    size_t numbersCarried = 0;
    for (auto& [carried, position] : candidates) {
        bool everyNextIsConverted = true;
        forEachBackEdge(carried, [&](const Edge& edge, ValueID next) {
            if (code().isConstant(next))
                return;
            BlockID converted = blocksOfConversions.get(next.index());
            everyNextIsConverted &= converted && representationOf(next) == Representation::Int32 && blockDominates(converted, edge.from);
        });
        if (!everyNextIsConverted)
            continue;
        if (countedLoopCheckPosition != position) {
            countedLoopCheckPosition = position;
            isCountedLoop = !!findCountedLoopAt(position);
            std::span<const ValueID> parameters = code().at(carried.head).parameters();
            numbersCarried = std::ranges::count_if(parameters, [&](ValueID parameter) { return m_values[parameter.index()] && m_values[parameter.index()]->type() == Machine::Double; });
        }
        if (isCountedLoop || numbersCarried >= mostNumbersCarried)
            continue;
        ++numbersCarried;
        build().statistics().count(StatisticID::LoopIntegersCarriedAsNumbers);
        carried.phi = m_out.phiIn(blockFor(carried.head), Machine::Double);
        m_firstLoopCarriedNumbers.add(carried.head.index(), m_loopCarriedNumbers.size());
        m_phisOfLoopCarriedNumbers.add(carried.conversion.index(), carried.phi);
        m_loopCarriedNumbers.append(carried);
    }
}

static constexpr OpcodeSet stepsOverTables { Opcode::MapNext, Opcode::SetNext };

static std::optional<ValueID> listWhoseEndMoves(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::TryCallInline)
        return std::nullopt;
    if (terminator.inlineBuiltin() != InlineBuiltin::ArrayPop && (terminator.inlineBuiltin() != InlineBuiltin::ArrayPush || terminator.argumentsOperands().size() != 1))
        return std::nullopt;
    return terminator.receiverOperand();
}

bool CodeGenerator::isJoinWithRareCode(BlockID join) const
{
    const Block& block = code().at(join);
    if (block.isLoopHead || block.isEntry || block.isHandlerBlock())
        return false;
    unsigned edges = 0;
    unsigned rareEdges = 0;
    for (EdgeID id = block.firstEdgeIn(); id; id = code().at(id).nextInto()) {
        const Block& from = code().at(code().at(id).from);
        if (from.terminator.kind != TerminatorKind::Jump || !code().isReached(code().at(id).from))
            return false;
        ++edges;
        rareEdges += from.frequency == BlockFrequency::Rare;
    }
    return edges == 2 && rareEdges == 1;
}

bool CodeGenerator::isBackEdgeOfSimpleLoop(BlockID from, BlockID head)
{
    const Block& block = code().at(head);
    if (!block.isLoopHead || block.isEntry || block.isHandlerBlock())
        return false;
    unsigned edges = 0;
    BlockID entering;
    for (EdgeID id = block.firstEdgeIn(); id; id = code().at(id).nextInto()) {
        BlockID source = code().at(id).from;
        if (code().at(source).terminator.kind != TerminatorKind::Jump || !code().isReached(source))
            return false;
        ++edges;
        if (source != from)
            entering = source;
    }
    return edges == 2 && entering && blockDominates(head, from) && !blockDominates(head, entering);
}

bool CodeGenerator::isKnownAtEveryEdgeInto(BlockID block, ValueID list)
{
    BlockID home = code().blockOf(list);
    for (EdgeID id = code().at(block).firstEdgeIn(); id; id = code().at(id).nextInto()) {
        if (!blockDominates(home, code().at(id).from))
            return false;
    }
    return true;
}

void CodeGenerator::carryListStorageFrom(uint32_t storage, BlockID start, UncheckedKeyHashSet<uint64_t>& joinsOfLists)
{
    constexpr unsigned mostBlocks = 10;
    auto leavesListsAlone = [&](ValueID id) {
        const Operation& operation = code().at(id);
        EffectTraits traits = operation.traits();
        return operation.opcode == Opcode::LoopHint || (!traits.mayCollect() && !traits.mayRunOtherCode() && !traits.mayBeTerminated() && !traits.mayWriteObservableState());
    };
    const ValueID list = m_listStorages[storage].list;
    const size_t storagesBefore = m_listStorages.size();
    size_t storagesRead = storagesBefore;
    auto noteRead = [&] {
        storagesRead = m_listStorages.size();
    };
    auto joinOfList = [&](BlockID join) {
        return static_cast<uint64_t>(join.index()) << 32 | list.index();
    };
    auto isCarriedInto = [&](BlockID join) {
        return joinsOfLists.contains(joinOfList(join)) || std::ranges::any_of(m_listStorages.span().subspan(storagesBefore), [&](const ListStorage& other) { return other.block == join; });
    };
    BlockID block = start;
    for (unsigned steps = 0; steps < mostBlocks; ++steps) {
        bool isChanged = false;
        for (ValueID id : code().valuesOf(block)) {
            if (code().at(id).opcode == Opcode::GetArrayLength && code().at(id).arrayOperand() == list) {
                m_lengthsOfListStorages.append({ id, storage });
                noteRead();
            }
            isChanged = !leavesListsAlone(id);
            if (isChanged)
                break;
        }
        const Terminator& terminator = code().at(block).terminator;
        if (isChanged)
            break;
        if (listWhoseEndMoves(terminator) == list) {
            m_callsOnListStorages.append({ block, storage });
            noteRead();
            break;
        }
        BlockID next;
        if (terminator.kind == TerminatorKind::Jump)
            next = code().at(terminator.next()).to;
        else if (terminator.kind == TerminatorKind::Branch && m_listStorages.size() > storagesBefore) {
            BlockID taken = code().at(terminator.taken()).to;
            BlockID notTaken = code().at(terminator.notTaken()).to;
            next = paysForCodeInLine(taken) ? taken : notTaken;
        } else
            break;
        if (!paysForCodeInLine(next) || code().at(next).frequency == BlockFrequency::Rare)
            break;
        if (m_singlePredecessors[next.index()] != block) {
            bool mayMeet = terminator.kind == TerminatorKind::Jump && (isJoinWithRareCode(next) || isBackEdgeOfSimpleLoop(block, next)) && isKnownAtEveryEdgeInto(next, list);
            if (!mayMeet || isCarriedInto(next))
                break;
            ListStorage met;
            met.list = list;
            met.block = next;
            met.usualPredecessor = block;
            met.usualSource = storage;
            storage = m_listStorages.size();
            m_listStorages.append(met);
        }
        block = next;
    }
    m_listStorages.shrink(storagesRead);
    for (size_t index = storagesBefore; index < m_listStorages.size(); ++index) {
        ListStorage& met = m_listStorages[index];
        joinsOfLists.add(joinOfList(met.block));
        met.butterfly = m_out.phiIn(blockFor(met.block), Machine::Int64);
        met.length = m_out.phiIn(blockFor(met.block), Machine::Int64);
        met.mode = m_out.phiIn(blockFor(met.block), Machine::Int32);
        build().statistics().count(StatisticID::ListStoragesCarried);
    }
}

void CodeGenerator::findCarriedListStorages()
{
    m_listStorages.shrink(0);
    m_listStoragesByBlock.shrink(0);
    m_lengthsOfListStorages.shrink(0);
    m_callsOnListStorages.shrink(0);
    if (m_keepsValuesInHeapFrame || !code().hasLoops() || code().hasSuspensions())
        return;
    UncheckedKeyHashSet<uint64_t> joinsOfLists;
    for (BlockID call : m_order) {
        const Block& block = code().at(call);
        if (!paysForCodeInLine(call) || block.frequency == BlockFrequency::Rare)
            continue;
        std::optional<ValueID> list = listWhoseEndMoves(block.terminator);
        if (!list || code().isConstant(*list) || representationOf(*list) != Representation::Cell)
            continue;
        BlockID usualEnd = code().at(block.terminator.done()).to;
        if (m_singlePredecessors[usualEnd.index()] != call)
            continue;
        ListStorage behind;
        behind.list = *list;
        behind.block = call;
        behind.isBehindCall = true;
        size_t reads = m_lengthsOfListStorages.size() + m_callsOnListStorages.size();
        uint32_t index = m_listStorages.size();
        m_listStorages.append(behind);
        carryListStorageFrom(index, usualEnd, joinsOfLists);
        if (m_lengthsOfListStorages.size() + m_callsOnListStorages.size() == reads)
            m_listStorages.shrink(index);
    }
    m_listStoragesByBlock = Vector<std::pair<BlockID, uint32_t>, 4>(m_listStorages.size(), [&](size_t index) { return std::pair { m_listStorages[index].block, static_cast<uint32_t>(index) }; });
    std::ranges::stable_sort(m_listStoragesByBlock, { }, &std::pair<BlockID, uint32_t>::first);
    std::ranges::stable_sort(m_lengthsOfListStorages, { }, &std::pair<ValueID, uint32_t>::first);
    std::ranges::stable_sort(m_callsOnListStorages, { }, &std::pair<BlockID, uint32_t>::first);
}

CodeGenerator::ListStorage* CodeGenerator::listStorageBehindCall(BlockID call)
{
    for (auto [block, index] : std::ranges::equal_range(m_listStoragesByBlock, call, { }, &std::pair<BlockID, uint32_t>::first)) {
        if (m_listStorages[index].isBehindCall)
            return &m_listStorages[index];
    }
    return nullptr;
}

const CodeGenerator::ListStorage* CodeGenerator::listStorageForLength(ValueID length) const
{
    auto found = std::ranges::lower_bound(m_lengthsOfListStorages, length, { }, &std::pair<ValueID, uint32_t>::first);
    if (found == m_lengthsOfListStorages.end() || found->first != length || !m_listStorages[found->second].butterfly)
        return nullptr;
    return &m_listStorages[found->second];
}

const CodeGenerator::ListStorage* CodeGenerator::listStorageForCall(BlockID call) const
{
    auto found = std::ranges::lower_bound(m_callsOnListStorages, call, { }, &std::pair<BlockID, uint32_t>::first);
    if (found == m_callsOnListStorages.end() || found->first != call || !m_listStorages[found->second].butterfly)
        return nullptr;
    return &m_listStorages[found->second];
}

void CodeGenerator::passListStorages(const Edge& edge)
{
    for (auto [block, index] : std::ranges::equal_range(m_listStoragesByBlock, edge.to, { }, &std::pair<BlockID, uint32_t>::first)) {
        const ListStorage& storage = m_listStorages[index];
        if (storage.isBehindCall)
            continue;
        const ListStorage& source = m_listStorages[storage.usualSource];
        if (edge.from == storage.usualPredecessor && source.butterfly) {
            m_out.upsilon(source.butterfly, storage.butterfly);
            m_out.upsilon(source.length, storage.length);
            m_out.upsilon(source.mode, storage.mode);
            continue;
        }
        Machine::DeclaredAccessesSuspension suspension(m_out);
        ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
        ArrayElements held = elements.elementsOf(get(storage.list).as<CellPointer>());
        LValue length = elements.publicLength(held).value();
        LValue mode = elements.indexingMode(held.array, IndexingShapeAndWritabilityMask).value();
        m_out.upsilon(held.butterfly.value(), storage.butterfly);
        m_out.upsilon(length, storage.length);
        m_out.upsilon(mode, storage.mode);
    }
}

bool CodeGenerator::loopMaySuspend(BlockID head, const BitVector& suspendingBlocks)
{
    BitVector seen;
    seen.set(head.index());
    Vector<BlockID, 8> worklist { head };
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (suspendingBlocks.get(block.index()))
            return true;
        code().forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            bool isInLoop = code().isReached(edge.from) && (block != head || dominates(head, edge.from));
            if (isInLoop && !seen.set(edge.from.index()))
                worklist.append(edge.from);
        });
    }
    return false;
}

void CodeGenerator::findSuspensionFreeRegions()
{
    const uint32_t numberOfBlocks = code().numberOfBlocks();
    m_regionsAtHeads.fill(0, numberOfBlocks);
    m_regionsAtEnds.fill(0, numberOfBlocks);
    m_regionsBehindSuspensions.fill(0, code().numberOfValues());
    BitVector suspensions;
    BitVector suspendingBlocks;
    for (const LiveAtSuspension& record : code().suspensions()) {
        suspensions.set(record.suspension.index());
        suspendingBlocks.set(code().blockOf(record.suspension).index());
    }
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        const uint32_t own = id.index() + 1;
        uint32_t region = 0;
        if (block.isEntry || block.isHandlerBlock() || (block.isLoopHead && loopMaySuspend(id, suspendingBlocks)))
            region = own;
        else {
            code().forEachIncomingEdgeUnsealed(id, [&](const Edge& edge) {
                if (!code().isReached(edge.from) || (block.isLoopHead && dominates(id, edge.from)))
                    return;
                uint32_t entering = m_regionsAtEnds[edge.from.index()];
                region = entering && (!region || region == entering) ? entering : own;
            });
        }
        if (!region)
            region = own;
        m_regionsAtHeads[id.index()] = region;
        if (suspendingBlocks.get(id.index())) {
            for (ValueID value : code().valuesOf(id)) {
                if (region > numberOfBlocks)
                    m_regionsBehindSuspensions[value.index()] = region;
                if (suspensions.get(value.index()))
                    region = numberOfBlocks + value.index() + 1;
            }
        }
        m_regionsAtEnds[id.index()] = region;
    }
}

uint32_t CodeGenerator::suspensionFreeRegionAt(const OperationContext& context)
{
    if (!code().hasSuspensions())
        return 0;
    if (m_regionsAtHeads.isEmpty())
        findSuspensionFreeRegions();
    const size_t block = context.block().index();
    if (!context.valueID())
        return m_regionsAtEnds[block];
    uint32_t region = m_regionsBehindSuspensions[context.valueID().index()];
    return region ? region : m_regionsAtHeads[block];
}

void CodeGenerator::findLoopCarriedTables()
{
    m_loopCarriedTables.shrink(0);
    m_formsOfCarriedTables.shrink(0);
    m_keysOfCheckedTables.shrink(0);
    if (m_keepsValuesInHeapFrame || !code().hasLoops() || code().hasSuspensions() || !code().opcodesOfValues().intersects(stepsOverTables))
        return;
    for (BlockID id : m_order) {
        for (ValueID step : code().valuesOf(id)) {
            const Operation& operation = code().at(step);
            if (!stepsOverTables.contains(operation.opcode) || !usesLoopForms(id))
                continue;
            const ValueID table = operation.tableOperand();
            const Operation& carried = code().at(table);
            if (carried.opcode != Opcode::Parameter || !m_values[table.index()] || !code().at(carried.block()).isLoopHead || keysOfCarriedTable(table))
                continue;
            const BlockID head = carried.block();
            auto placeOf = [&](ValueID parameter) {
                std::span<const ValueID> parameters = code().at(code().at(parameter).block()).parameters();
                return static_cast<size_t>(std::ranges::find(parameters, parameter) - parameters.begin());
            };
            Vector<ValueID, 4> tables { table };
            bool isReadAfterStep = false;
            for (size_t next = 0; next < tables.size(); ++next) {
                const size_t place = placeOf(tables[next]);
                code().forEachIncomingEdgeUnsealed(code().at(tables[next]).block(), [&](const Edge& edge) {
                    if (!code().isReached(edge.from) || !dominates(head, edge.from) || place >= edge.arguments().size())
                        return;
                    const ValueID passed = edge.arguments()[place];
                    if (!passed || code().isConstant(passed))
                        return;
                    const Operation& operation = code().at(passed);
                    isReadAfterStep |= operation.opcode == Opcode::CollectionTable;
                    if (operation.opcode == Opcode::Parameter && m_values[passed.index()] && !code().at(operation.block()).isLoopHead && !code().at(operation.block()).isHandlerBlock() && !tables.contains(passed))
                        tables.append(passed);
                });
            }
            const size_t position = std::ranges::find(m_order, head) - m_order.begin();
            if (!isReadAfterStep || findCountedLoopAt(position))
                continue;
            build().statistics().count(StatisticID::CollectionTablesCheckedPerLoop);
            size_t group = m_formsOfCarriedTables.size();
            for (ValueID parameter : tables) {
                auto carriedBefore = std::ranges::find(m_loopCarriedTables, parameter, &LoopCarriedTable::table);
                if (carriedBefore != m_loopCarriedTables.end())
                    group = carriedBefore->group;
            }
            m_formsOfCarriedTables.grow(std::max(group + 1, m_formsOfCarriedTables.size()));
            for (ValueID parameter : tables) {
                if (!keysOfCarriedTable(parameter))
                    m_loopCarriedTables.append({ parameter, code().at(parameter).block(), placeOf(parameter), m_out.phiIn(blockFor(code().at(parameter).block()), Machine::Int64), group });
            }
        }
    }
}

static constexpr OpcodeSet looksAtCarriedSubstrings { Opcode::RegExpMatchLiteral, Opcode::RegExpMatchLiteralInParts, Opcode::GetStringCodeUnit, Opcode::StringStartsWithLiteral };
static constexpr OpcodeSet readsOfCarriedSubstrings = looksAtCarriedSubstrings | OpcodeSet { Opcode::GetStringLength, Opcode::ToBoolean, Opcode::Substring, Opcode::StringRangeEqualsLiteral, Opcode::StringRangeToNumber, Opcode::StringRangeParseInt };

bool CodeGenerator::isLeftOfCarriedSubstring(ValueID value, BlockID head, Vector<CarriedSubstring, 4>& strings)
{
    if (strings.containsIf([&](const CarriedSubstring& string) { return string.string == value; }))
        return true;
    if (!value || code().isConstant(value) || representationOf(value) != Representation::Cell)
        return false;
    const BlockID block = code().blockOf(value);
    if (!dominates(head, block))
        return false;
    const Operation& operation = code().at(value);
    if (operation.opcode == Opcode::Substring) {
        const ValueID end = operation.endOperand();
        if (opcodeOf(end) != Opcode::GetStringLength || code().at(end).stringOperand() != operation.stringOperand() || !isLeftOfCarriedSubstring(operation.stringOperand(), head, strings))
            return false;
        strings.append({ value, head });
        strings.first().cut = value;
        return true;
    }
    const Block& join = code().at(block);
    if (operation.opcode != Opcode::Parameter || !m_values[value.index()] || block == head || join.isLoopHead || join.isEntry || join.isHandlerBlock())
        return false;
    const size_t place = join.placeOfParameter(value);
    bool isLeft = true;
    code().forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
        if (code().isReached(edge.from))
            isLeft = isLeft && place < edge.arguments().size() && dominates(head, edge.from) && isLeftOfCarriedSubstring(edge.arguments()[place], head, strings);
    });
    strings.append({ value, head, block, place });
    return isLeft;
}

bool CodeGenerator::canCarrySubstring(BlockID head, size_t place, Vector<CarriedSubstring, 4>& strings)
{
    strings.append({ code().at(head).parameters()[place], head, head, place });
    bool isCarriedAround = true;
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        if (!code().isReached(edge.from))
            return;
        if (place >= edge.arguments().size()) {
            isCarriedAround = false;
            return;
        }
        if (dominates(head, edge.from))
            isCarriedAround = isCarriedAround && isLeftOfCarriedSubstring(edge.arguments()[place], head, strings);
    });
    if (!strings.first().cut || !isCarriedAround)
        return false;

    auto isCarried = [&](ValueID value) {
        return strings.containsIf([&](const CarriedSubstring& string) { return string.string == value; });
    };
    bool isLookedAt = false;
    for (BlockID id : m_order) {
        const Block& block = code().at(id);
        if (!dominates(head, id))
            continue;
        if (block.subject && isCarried(block.subject))
            return false;
        for (ValueID user : code().valuesOf(id)) {
            const Operation& operation = code().at(user);
            unsigned count = 0;
            operation.forEachChild([&](ValueID child) {
                count += isCarried(child);
            });
            if (!count)
                continue;
            if (count > 1 || !readsOfCarriedSubstrings.contains(operation.opcode) || !isCarried(operation.opcode == Opcode::ToBoolean ? operation.valueOperand() : operation.stringOperand()))
                return false;
            isLookedAt |= looksAtCarriedSubstrings.contains(operation.opcode);
        }
        bool isReadByTerminator = false;
        block.terminator.forEachChild([&](ValueID child) {
            isReadByTerminator |= isCarried(child);
        });
        if (isReadByTerminator)
            return false;
        EdgeID handler = code().handlerOf(id);
        if (handler && std::ranges::any_of(code().at(handler).arguments(), isCarried))
            return false;
        for (uint32_t number = 0; number < block.terminator.numberOfEdges; ++number) {
            const Edge& edge = code().at(block.terminator.edge(number));
            std::span<const ValueID> parameters = code().at(edge.to).parameters();
            for (size_t index = 0; index < edge.arguments().size(); ++index) {
                if (!isCarried(edge.arguments()[index]) || !m_values[parameters[index].index()] || isCarried(parameters[index]))
                    continue;
                Representation wanted = representationOf(parameters[index]);
                if (edge.to == head || handler || usesLoopForms(edge.to) || (wanted != Representation::Cell && wanted != Representation::JSValue))
                    return false;
            }
        }
    }
    return isLookedAt;
}

void CodeGenerator::findCarriedSubstrings()
{
    m_carriedSubstrings.shrink(0);
    if (m_keepsValuesInHeapFrame || !code().hasLoops() || code().hasSuspensions() || !code().opcodesOfValues().contains(Opcode::Substring) || !code().opcodesOfValues().intersects(looksAtCarriedSubstrings))
        return;
    for (BlockID head : m_order) {
        const Block& block = code().at(head);
        if (!block.isLoopHead || block.isEntry || !usesLoopForms(head))
            continue;
        std::span<const ValueID> parameters = block.parameters();
        for (size_t place = 0; place < parameters.size(); ++place) {
            const ValueID parameter = parameters[place];
            Vector<CarriedSubstring, 4> strings;
            if (!m_values[parameter.index()] || representationOf(parameter) != Representation::Cell || findCarriedSubstring(parameter) || !canCarrySubstring(head, place, strings))
                continue;
            build().statistics().count(StatisticID::SubstringsCarriedByPosition);
            for (CarriedSubstring& string : strings) {
                string.cut = strings.first().cut;
                if (string.block) {
                    string.start = m_out.phiIn(blockFor(string.block), Machine::Int32);
                    string.length = m_out.phiIn(blockFor(string.block), Machine::Int32);
                }
                m_carriedSubstrings.append(string);
            }
        }
    }
}

bool CodeGenerator::mayCarryNarrowInteger(BlockID home, ValueID parameter)
{
    const Block& block = code().at(home);
    if (block.isLoopHead || !SlotEmitter::isNarrowIntegerOrOneMarker(typeOf(parameter)) || (unit().role == CodeRole::CoroutineBody && code().hasSuspensions()))
        return false;
    size_t place = std::ranges::find(block.parameters(), parameter) - block.parameters().begin();
    bool readsNarrowInteger = false;
    for (EdgeID id = block.firstEdgeIn(); id; id = code().at(id).nextInto()) {
        std::span<const ValueID> arguments = code().at(id).arguments();
        if (place >= arguments.size() || !arguments[place])
            return false;
        if (code().isConstant(arguments[place]))
            continue;
        if (!emitter<SlotAccessEmitter>().readsNarrowInteger(code().at(arguments[place])))
            return false;
        readsNarrowInteger = true;
    }
    return readsNarrowInteger;
}

bool CodeGenerator::isReachedWithoutCalls(BlockID from, BlockID to) const
{
    constexpr unsigned mostBlocks = 4;
    for (unsigned steps = 0; steps < mostBlocks; ++steps) {
        if (from == to)
            return true;
        const Terminator& terminator = code().at(from).terminator;
        bool isCalled = std::ranges::any_of(code().valuesOf(from), [&](ValueID id) {
            return code().at(id).traits().mayCollect();
        });
        if (isCalled || (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::TryCallInline))
            return false;
        from = code().at(terminator.kind == TerminatorKind::Jump ? terminator.next() : terminator.done()).to;
        if (code().at(from).frequency == BlockFrequency::Rare || code().at(from).isLoopHead)
            return false;
    }
    return false;
}

bool CodeGenerator::isUnboxedBeforeJoin(BlockID home, size_t place, Representation form) const
{
    if (!paysForCodeInLine(home) || code().at(home).frequency == BlockFrequency::Rare)
        return false;
    constexpr unsigned mostEdges = 3;
    unsigned edges = 0;
    unsigned usualEdges = 0;
    for (EdgeID id = code().at(home).firstEdgeIn(); id; id = code().at(id).nextInto()) {
        const Edge& edge = code().at(id);
        const Block& from = code().at(edge.from);
        if (++edges > mostEdges || place >= edge.arguments().size() || from.terminator.kind != TerminatorKind::Jump)
            return false;
        ValueID argument = edge.arguments()[place];
        if (!argument || code().isConstant(argument) || representationOf(argument) != Representation::JSValue || isNeverType(code().at(argument).type) || !isLegalRepresentation(typeOf(argument), form))
            return false;
        usualEdges += from.frequency != BlockFrequency::Rare;
    }
    return edges > 1 && usualEdges == 1;
}

bool CodeGenerator::isReadByUnboxAlone(BlockID home, ValueID parameter) const
{
    const Block& block = code().at(home);
    ValueID reader = code().onlyUserOf(parameter);
    if (block.isLoopHead || block.isEntry || block.isHandlerBlock() || m_keepsValuesInHeapFrame || (unit().role == CodeRole::CoroutineBody && code().hasSuspensions()))
        return false;
    static constexpr RepresentationSet unboxedNumbers { Representation::Int32, Representation::Int64, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble };
    Representation form = representationOf(reader);
    if (!unboxedNumbers.contains(form) || waitsForBoxOfSum(reader, code().at(reader)))
        return false;
    static constexpr OpcodeSet constantsCarried { Opcode::Integer, Opcode::Double, Opcode::Undefined, Opcode::Null };
    size_t place = std::ranges::find(block.parameters(), parameter) - block.parameters().begin();
    if (isUnboxedBeforeJoin(home, place, form) && isReachedWithoutCalls(home, code().blockOf(reader)))
        return true;
    bool isPassed = false;
    for (EdgeID id = block.firstEdgeIn(); id; id = code().at(id).nextInto()) {
        std::span<const ValueID> arguments = code().at(id).arguments();
        if (place >= arguments.size() || !arguments[place] || !code().isConstant(arguments[place]) || !constantsCarried.contains(opcodeOf(arguments[place])) || !isLegalRepresentation(typeOf(arguments[place]), form))
            return false;
        isPassed = true;
    }
    return isPassed;
}

void CodeGenerator::widenCarriedIntegers(const Block& block)
{
    for (ValueID parameter : block.parameters()) {
        LValue& lowered = m_values[parameter.index()];
        if (isCarriedNarrow(representationOf(parameter), lowered))
            lowered = emitter<SlotEmitter>().widenBySelect(Word32(lowered), typeOf(parameter)).value();
    }
}

struct CodeGenerator::CarriedSum {
    static constexpr size_t mostNames = 24;

    Vector<ValueID, 8> names;
    Vector<BlockID, 4> heads;
    Vector<std::pair<ValueID, double>, 4> addends;
    double greatestStart { 0 };
    bool truncatesAddend { false };
};

static std::optional<double> greatestOfIntegers(const Type& type, uint16_t alsoZero)
{
    if (!type.hasOnlyPrimitives(Primitive::Integer | alsoZero) || type.integers.isEmpty())
        return std::nullopt;
    return std::max(std::abs(static_cast<double>(type.integers.minimum)), std::abs(static_cast<double>(type.integers.maximum)));
}

bool CodeGenerator::followCarriedSum(CarriedSum& sum) const
{
    auto isName = [&](ValueID value) {
        if (code().isConstant(value) || representationOf(value) != Representation::Double || !typeOf(value).isNumber())
            return false;
        const Operation& operation = code().at(value);
        return operation.opcode == Opcode::Parameter || ((operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub) && operation.form() == OperatorForm::Numbers);
    };
    auto follow = [&](ValueID name) {
        if (!sum.names.contains(name))
            sum.names.append(name);
    };
    for (size_t index = 0; index < sum.names.size(); ++index) {
        if (sum.names.size() > CarriedSum::mostNames)
            return false;
        ValueID name = sum.names[index];
        const Operation& operation = code().at(name);
        if (operation.opcode != Opcode::Parameter) {
            bool leftIsName = isName(operation.operand(0));
            bool rightIsName = isName(operation.operand(1));
            if (leftIsName == rightIsName || (rightIsName && operation.opcode == Opcode::Sub))
                return false;
            std::optional<double> greatest = greatestOfIntegers(typeOf(operation.operand(leftIsName ? 1 : 0)), Primitive::Null);
            if (!greatest)
                return false;
            sum.addends.append({ name, *greatest });
            ValueID addend = operation.operand(leftIsName ? 1 : 0);
            bool isUnboxed = !code().isConstant(addend) && opcodeOf(addend) == Opcode::Unbox && code().useCountOf(addend) == 1 && code().blockOf(addend) == operation.block();
            sum.truncatesAddend |= !code().isConstant(addend) && !isUnboxed && opcodeOf(addend) != Opcode::IntegerToNumber;
            follow(operation.operand(leftIsName ? 0 : 1));
            continue;
        }
        BlockID home = operation.block();
        const Block& block = code().at(home);
        if (!code().isNeeded(name) || block.isEntry || block.isHandlerBlock() || block.subject || isCarriedBoxed(name) || isCarriedSum(name))
            return false;
        if (block.isLoopHead && !sum.heads.contains(home))
            sum.heads.append(home);
        size_t place = block.placeOfParameter(name);
        bool isFollowed = true;
        code().forEachIncomingEdgeUnsealed(home, [&](const Edge& edge) {
            if (!code().isReached(edge.from))
                return;
            ValueID argument = place < edge.arguments().size() ? edge.arguments()[place] : ValueID();
            std::optional<double> greatest;
            if (!argument)
                isFollowed = false;
            else if (isName(argument))
                follow(argument);
            else if (greatest = greatestOfIntegers(typeOf(argument), 0); greatest)
                sum.greatestStart = std::max(sum.greatestStart, *greatest);
            else
                isFollowed = false;
        });
        if (!isFollowed)
            return false;
    }
    return !sum.heads.isEmpty() && !sum.addends.isEmpty();
}

std::optional<double> CodeGenerator::greatestNumberOfTurns(BlockID head)
{
    const Block& block = code().at(head);
    for (ValueID counter : block.parameters()) {
        const Type& type = typeOf(counter);
        if (!type.isInteger() || type.integers.isEmpty())
            continue;
        size_t place = block.placeOfParameter(counter);
        bool isStepped = true;
        code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
            if (!code().isReached(edge.from) || !dominates(head, edge.from))
                return;
            ValueID next = place < edge.arguments().size() ? edge.arguments()[place] : ValueID();
            isStepped &= next && !code().isConstant(next) && opcodeOf(next) == Opcode::Increment && code().at(next).form() == OperatorForm::Integers && code().valueBehindNames<valuePreservingOpcodes>(code().at(next).operand(0)) == counter;
        });
        if (isStepped)
            return static_cast<double>(type.integers.maximum) - static_cast<double>(type.integers.minimum) + 1;
    }
    return std::nullopt;
}

uint32_t CodeGenerator::endOfLoop(BlockID head)
{
    uint32_t last = m_positionsInOrder[head.index()];
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        if (code().isReached(edge.from) && dominates(head, edge.from))
            last = std::max(last, m_positionsInOrder[edge.from.index()]);
    });
    return last + 1;
}

bool CodeGenerator::isReadInLoops(const CarriedSum& sum)
{
    auto isName = [&](ValueID value) {
        return value && sum.names.contains(value);
    };
    for (BlockID head : sum.heads) {
        for (uint32_t position = m_positionsInOrder[head.index()], end = endOfLoop(head); position < end; ++position) {
            BlockID id = m_order[position];
            for (ValueID value : code().valuesOf(id)) {
                bool isRead = false;
                code().at(value).forEachChild([&](ValueID child) { isRead |= isName(child); });
                if (isRead && !isName(value))
                    return true;
            }
            bool isTested = false;
            code().at(id).terminator.forEachChild([&](ValueID child) { isTested |= isName(child); });
            EdgeID handler = code().handlerOf(id);
            if (isTested || (handler && std::ranges::any_of(code().at(handler).arguments(), isName)))
                return true;
        }
    }
    return false;
}

bool CodeGenerator::callsWithin(BlockID head)
{
    static constexpr OpcodeSet calls { Opcode::CallDirect, Opcode::Call, Opcode::Construct };
    static constexpr OpcodeSet callsOfBuiltins { Opcode::CallBuiltin, Opcode::CallInline, Opcode::RegExpMatchLiteral, Opcode::AppendIterable };
    for (uint32_t position = m_positionsInOrder[head.index()], end = endOfLoop(head); position < end; ++position) {
        const Block& block = code().at(m_order[position]);
        if (block.frequency != BlockFrequency::Rare && std::ranges::any_of(code().valuesOf(block), [&](ValueID value) { return calls.contains(opcodeOf(value)) || (callsOfBuiltins.contains(opcodeOf(value)) && code().at(value).traits().mayCollect()); }))
            return true;
    }
    return false;
}

bool CodeGenerator::isUnboxedBySum(ValueID id, const Operation& unboxing) const
{
    if (unboxing.opcode != Opcode::Unbox || unboxing.representation != Representation::Double || code().useCountOf(id) != 1 || representationOf(unboxing.valueOperand()) != Representation::JSValue || !typeOf(unboxing.valueOperand()).isInteger() || isCarriedUnboxed(unboxing.valueOperand()))
        return false;
    ValueID sum = code().onlyUserOf(id);
    return sum && isCarriedSum(sum) && opcodeOf(sum) != Opcode::Parameter && code().blockOf(sum) == unboxing.block();
}

std::optional<double> CodeGenerator::maxIterationsForCarriedSums(BlockID head)
{
    return m_maxIterationsForCarriedSums.ensure(head.index(), [&] {
        return !findCountedLoopAt(m_positionsInOrder[head.index()]) ? greatestNumberOfTurns(head) : std::nullopt;
    }).iterator->value;
}

void CodeGenerator::lookForCarriedSums()
{
    if (std::exchange(m_carriesSums, false))
        m_carriedSums.clearAll();
    if (std::exchange(m_checksSums, false))
        m_checkedSums.clearAll();
    m_wordsOfCheckedSums.clear();
    m_integersOfCarriedSums.clear();
    m_parametersThatMaySum.shrink(0);
    m_positionsInOrder.shrink(0);
    m_maxIterationsForCarriedSums.clear();
    m_looksForCarriedSums = code().hasLoops() && !code().hasSuspensions() && !code().readsNumbersOutOfBytes() && code().opcodesOfValues().contains(Opcode::Add);
}

void CodeGenerator::tryCarrySum(ValueID parameter)
{
    static constexpr double mostSum = 0x1p52;
    CarriedSum sum;
    sum.names.append(parameter);
    if (!followCarriedSum(sum))
        return;
    if (m_positionsInOrder.isEmpty()) {
        m_positionsInOrder.fill(0, code().numberOfBlocks());
        for (uint32_t position = 0; position < m_order.size(); ++position)
            m_positionsInOrder[m_order[position].index()] = position;
    }
    Vector<double, 4> turns;
    bool hasUnknownTurns = false;
    for (BlockID head : sum.heads) {
        std::optional<double> greatest = maxIterationsForCarriedSums(head);
        if (!greatest && findCountedLoopAt(m_positionsInOrder[head.index()]))
            return;
        hasUnknownTurns |= !greatest;
        turns.append(greatest.value_or(0));
    }
    bool isCarriedAcrossCalls = std::ranges::any_of(sum.heads, [&](BlockID head) { return callsWithin(head); });
    if (sum.truncatesAddend && !isCarriedAcrossCalls)
        return;
    double greatest = sum.greatestStart;
    for (auto [name, addend] : sum.addends) {
        for (size_t index = 0; index < turns.size(); ++index)
            addend *= dominates(sum.heads[index], code().blockOf(name)) ? turns[index] : 1;
        greatest += addend;
    }
    if (hasUnknownTurns || greatest > mostSum) {
        static constexpr double greatestCheckedAddend = 0x1p32;
        bool hasSmallAddends = std::ranges::all_of(sum.addends, [&](const std::pair<ValueID, double>& addend) { return addend.second <= greatestCheckedAddend; });
        if (!isCarriedAcrossCalls || !hasSmallAddends || isReadInLoops(sum) || sum.greatestStart > mostSum || code().at(code().blockOf(parameter)).frequency == BlockFrequency::Rare)
            return;
        m_checksSums = true;
        m_checkedSums.ensureSize(code().numberOfValues());
        for (ValueID name : sum.names)
            m_checkedSums.set(name.index());
        build().statistics().count(StatisticID::CheckedSumsCarried);
    }
    m_carriesSums = true;
    m_carriedSums.ensureSize(code().numberOfValues());
    for (ValueID name : sum.names) {
        m_carriedSums.set(name.index());
        if (opcodeOf(name) != Opcode::Parameter)
            continue;
        if (m_followsMarkers)
            m_valuesHoldingAnyNaN.quickClear(name.index());
        m_out.retypePhi(m_values[name.index()], machineTypeOf(Representation::Int64));
    }
    build().statistics().count(StatisticID::SumsCarriedAsIntegers);
}

void CodeGenerator::carrySums()
{
    for (ValueID parameter : m_parametersThatMaySum) {
        if (!isCarriedSum(parameter))
            tryCarrySum(parameter);
    }
}

bool CodeGenerator::carriesSum(BlockID head) const
{
    return std::ranges::any_of(code().at(head).parameters(), [&](ValueID parameter) { return isCarriedSum(parameter); });
}

LValue CodeGenerator::numberOfCheckedWord(LValue word)
{
    LValue integer = m_out.sub(word, m_out.constInt64(biasOfCheckedSums));
    LValue number = m_out.select(m_out.belowOrEqual(word, m_out.constInt64(2 * biasOfCheckedSums)), m_out.intToDouble(integer), m_out.bitCast(integer, Machine::Double));
    m_wordsOfCheckedSums.add(number, word);
    return number;
}

LValue CodeGenerator::wordOfCheckedSum(ValueID id)
{
    LValue lowered = m_values[id.index()];
    RELEASE_ASSERT(lowered && isCheckedSum(id));
    LValue word = lowered->type() == Machine::Int64 ? lowered : m_wordsOfCheckedSums.get(lowered);
    RELEASE_ASSERT(word);
    return word;
}

LValue CodeGenerator::noteWordOfCheckedSum(LValue word)
{
    LValue unread = m_out.bitCast(word, Machine::Double);
    m_wordsOfCheckedSums.add(unread, word);
    return unread;
}

LValue CodeGenerator::wordOfNumberInCheckedSum(LValue number)
{
    if (LValue word = m_wordsOfCheckedSums.get(number))
        return word;
    LValue integer = integerOfCarriedSum(number);
    return integer->hasInt64() ? m_out.constInt64(integer->asInt64() + biasOfCheckedSums) : m_out.add(integer, m_out.constInt64(biasOfCheckedSums));
}

LValue CodeGenerator::integerOfCarriedSum(LValue number, bool isNotNegative)
{
    if (number->opcode() == Machine::IToD) {
        LValue integer = number->child(0);
        if (integer->type() != Machine::Int32)
            return integer;
        return isNotNegative ? m_out.extendWordThatIsNotNegative(integer) : m_out.signExt32To64(integer);
    }
    if (number->hasDouble())
        return m_out.constInt64(static_cast<int64_t>(number->asDouble()));
    return emitter<ConversionEmitter>().truncateUnchecked(Number(number)).value();
}

void CodeGenerator::takeIntegersOfCarriedSums(const Block& block)
{
    for (ValueID parameter : block.parameters()) {
        LValue& lowered = m_values[parameter.index()];
        if (!lowered || lowered->opcode() != Machine::Phi || !isCarriedSum(parameter) || isCheckedSum(parameter))
            continue;
        m_integersOfCarriedSums.set(parameter.index(), lowered);
        lowered = m_out.intToDouble(lowered);
    }
}

void CodeGenerator::createPhis()
{
    m_carriesNarrowIntegers = false;
    constexpr unsigned mostBlocksThatHandParameterOn = 4;
    const bool mayUnboxParameters = code().opcodesOfValues().contains(Opcode::Unbox);
    const bool mayConvertIntegers = code().opcodesOfValues().contains(Opcode::IntegerToNumber);
    for (BlockID block : m_order) {
        if (goesOnInBlockOfEntry(block))
            continue;
        for (ValueID parameter : code().at(block).parameters()) {
            Representation representation = representationOf(parameter);
            RELEASE_ASSERT(representation != Representation::Unchosen);
            if (!code().isNeeded(parameter) || !hasRegister(representation))
                continue;
            if (representation == Representation::Double && m_looksForCarriedSums && code().at(block).isLoopHead) [[unlikely]]
                m_parametersThatMaySum.append(parameter);
            if (representation == Representation::Double && (!m_followsNumbersOutOfBytes || m_followsMarkers) && (isReadByBoxAlone(block, parameter, mostBlocksThatHandParameterOn) || (mayUnboxParameters && isReadByBoxedSumAlone(block, parameter)))) [[unlikely]] {
                if (ValueID reader = code().onlyUserOf(parameter); reader && opcodeOf(reader) != Opcode::Box)
                    build().statistics().count(StatisticID::JoinsCarriedBoxed);
                if (!m_carriesParametersBoxed) {
                    m_carriesParametersBoxed = true;
                    m_parametersCarriedBoxed.ensureSize(code().numberOfValues());
                }
                m_parametersCarriedBoxed.quickSet(parameter.index());
                representation = Representation::JSValue;
            }
            if (representation == Representation::JSValue && mayUnboxParameters && code().useCountOf(parameter) == 1 && code().onlyUserOf(parameter) && opcodeOf(code().onlyUserOf(parameter)) == Opcode::Unbox && isReadByUnboxAlone(block, parameter)) [[unlikely]] {
                if (!m_carriesParametersUnboxed) {
                    m_carriesParametersUnboxed = true;
                    m_parametersCarriedUnboxed.ensureSize(code().numberOfValues());
                }
                m_parametersCarriedUnboxed.quickSet(parameter.index());
                representation = carriedRepresentationOf(parameter);
                build().statistics().count(StatisticID::JoinsCarriedUnboxed);
            }
            if (representation == Representation::OptionalInt64 && mayCarryNarrowInteger(block, parameter)) [[unlikely]] {
                m_carriesNarrowIntegers = true;
                representation = Representation::Int32;
            }
            m_out.setOrigin(machineOriginOf(parameter));
            LValue phi = m_out.phiIn(blockFor(block), machineTypeOf(representation));
            defineValue(parameter, representation, Operand(representation, phi));
            if (representation == Representation::Int64 && mayConvertIntegers && typeOf(parameter).isInteger())
                emitter<ConversionEmitter>().noteRangeOfParameter(Integer(phi), typeOf(parameter).integers);
        }
    }
    m_out.setOrigin(Machine::Origin());
}

void CodeGenerator::followNumbersOutOfBytes()
{
    if (!code().readsNumbersOutOfBytes()) {
        if (!code().hasOptionalNumberArithmetic() || m_keepsValuesInHeapFrame)
            return;
        m_followsMarkers = true;
    }

    m_followsNumbersOutOfBytes = true;
    uint32_t numberOfValues = code().numberOfValues();
    m_valuesHoldingAnyNaN.ensureSize(numberOfValues);
    m_valuesReadForBitsOfNaN.ensureSize(numberOfValues);
    m_blocksBegun.ensureSize(code().numberOfBlocks());

    Vector<ValueID> headsFed;
    headsFed.fill(ValueID(), numberOfValues);
    Vector<ValueID, 4> headsFedByBytes;
    auto isReadForBitsOfNaN = [&](ValueID value) {
        return m_valuesReadForBitsOfNaN.quickGet(value.index());
    };
    auto feed = [&](ValueID value, ValueID head) {
        if (head && !headsFed[value.index()])
            headsFed[value.index()] = head;
    };

    for (size_t position = m_order.size(); position--;) {
        BlockID id = m_order[position];
        const Block& block = code().at(id);
        auto noteReadForBits = [&](ValueID operand) {
            const Operation& definition = code().at(operand);
            bool mayHoldAnyNaN = readsNumberOutOfBytes(definition) || handsOnBitsOfNaN(definition) || (definition.opcode == Opcode::Parameter && definition.representation == Representation::Double);
            bool isPurifiedByReader = block.frequency == BlockFrequency::Rare && mayHoldAnyNaN && code().at(definition.block()).frequency != BlockFrequency::Rare;
            if (!isPurifiedByReader)
                m_valuesReadForBitsOfNaN.quickSet(operand.index());
        };
        block.terminator.forEachChild(noteReadForBits);
        if (block.subject)
            m_valuesReadForBitsOfNaN.quickSet(block.subject.index());
        EdgeID handler = code().handlerOf(id);
        code().forEachEdgeOf(id, [&](EdgeID edgeID, const Edge& edge) {
            std::span<const ValueID> parameters = code().at(edge.to).parameters();
            std::span<const ValueID> arguments = edge.arguments();
            size_t numberOfPairs = std::min(parameters.size(), arguments.size());
            for (size_t index = 0; index < numberOfPairs; ++index) {
                ValueID parameter = parameters[index];
                ValueID argument = arguments[index];
                if (!argument || !code().isNeeded(parameter))
                    continue;
                if (edgeID == handler || representationOf(parameter) != Representation::Double) {
                    noteReadForBits(argument);
                    continue;
                }
                if (code().at(edge.to).isLoopHead && parameter < argument) {
                    ValueID& fed = headsFed[argument.index()];
                    if (!fed || argument < fed || fed < parameter)
                        fed = parameter;
                } else if (code().at(edge.to).isLoopHead)
                    feed(argument, parameter);
                else if (!isReadForBitsOfNaN(parameter))
                    feed(argument, headsFed[parameter.index()]);
            }
        });

        std::span<const ValueID> values = code().valuesOf(id);
        for (size_t index = values.size(); index--;) {
            ValueID value = values[index];
            const Operation& operation = code().at(value);
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                constexpr OpcodeSet storesIntoBytes { Opcode::PutTypedArrayElement, Opcode::PutDataViewElement };
                bool isTaken = storesIntoBytes.contains(operation.opcode) ? takesAnyNaNIntoBytes(operation, operandIndex) : takesAnyNaN(operation, operandIndex);
                if (!isTaken)
                    noteReadForBits(operand);
            });
            ValueID head = headsFed[value.index()];
            if (!head || isReadForBitsOfNaN(value))
                continue;
            if (readsNumberOutOfBytes(operation) || readsNumberOrNothingOutOfBytes(operation))
                headsFedByBytes.append(head);
            else if (handsOnBitsOfNaN(operation)) {
                operation.forEachChild([&](ValueID operand) {
                    feed(operand, head);
                    if (m_followsMarkers && takesUndefinedForNaN(operand) && (headsFedByBytes.isEmpty() || headsFedByBytes.last() != head))
                        headsFedByBytes.append(head);
                });
            }
        }
    }

    for (ValueID head : headsFedByBytes) {
        while (head && !isReadForBitsOfNaN(head) && !m_valuesHoldingAnyNaN.quickSet(head.index()))
            head = headsFed[head.index()];
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CodeGenerator::testReadByBranchAlone(BlockID id, const Terminator& terminator) const
{
    if (terminator.kind != TerminatorKind::Branch)
        return { };
    ValueID condition = terminator.conditionOperand();
    const Operation& test = code().at(condition);
    if (test.block() != id || branchesOf(test.opcode) != Branches::May || code().useCountOf(condition) != 1)
        return { };
    if (test.representation != Representation::Boolean || isNeverType(test.type))
        return { };
    return condition;
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CodeGenerator::testReadByBranchBehindJumpAlone(BlockID id, const Block& block) const
{
    if (block.terminator.kind != TerminatorKind::Jump)
        return { };
    BlockID behind = code().at(block.terminator.next()).to;
    if (m_blocks[behind.index()] || m_singlePredecessors[behind.index()] != id || !code().valuesOf(behind).empty() || code().handlerOf(behind) != code().handlerOf(id))
        return { };
    return testReadByBranchAlone(id, code().at(behind).terminator);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CodeGenerator::isAwaitedAtOnce(const Operation& call, std::span<const ValueID> valuesOfBlock, size_t index) const
{
    if ((call.opcode != Opcode::Call && call.opcode != Opcode::CallDirect) || index + 1 == valuesOfBlock.size())
        return false;
    const Operation& next = code().at(valuesOfBlock[index + 1]);
    return next.opcode == Opcode::AwaitBySavingFrame && next.awaitedOperand() == valuesOfBlock[index];
}

ValueID CodeGenerator::sumBoxedBySlow(const Operation& box) const
{
    ValueID sum = box.valueOperand();
    const Operation& operation = code().at(sum);
    BlockID block = box.block();
    if (!sumsThatMayStayBoxed.contains(operation.opcode) || operation.form() != OperatorForm::Numbers)
        return { };
    if (operation.block() != block || code().useCountOf(sum) != 1)
        return { };

    bool someTermLiesBoxed = false;
    auto isTerm = [&](ValueID id) {
        const Operation& term = code().at(id);
        if (code().isConstant(id))
            return term.opcode == Opcode::Integer && isRepresentableAs<int32_t>(term.integer());
        if (term.block() != block || code().useCountOf(id) != 1)
            return false;
        if (term.opcode == Opcode::Parameter && isCarriedBoxed(id)) {
            someTermLiesBoxed = true;
            return true;
        }
        if (term.opcode == Opcode::IntegerToNumber)
            return term.representation == Representation::Double && representationOf(term.valueOperand()) == Representation::Int32;
        if (term.opcode != Opcode::Unbox || representationOf(term.valueOperand()) != Representation::JSValue || isCarriedUnboxed(term.valueOperand()))
            return false;
        const Type& boxed = typeOf(term.valueOperand());
        if (!(term.representation == Representation::Double && boxed.isNumber()) && !(term.representation == Representation::OptionalDouble && boxed.isNumberOrNullish()))
            return false;
        someTermLiesBoxed = true;
        return true;
    };
    bool isStep = operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement;
    bool hasTermsAlone = isStep ? isTerm(operation.valueOperand()) : isTerm(operation.leftOperand()) && isTerm(operation.rightOperand());
    return hasTermsAlone && someTermLiesBoxed ? sum : ValueID();
}

bool CodeGenerator::isLeftToBackEdge(ValueID id, const Operation& operation) const
{
    if (operation.form() != OperatorForm::Integers || code().useCountOf(id) != 1 || code().onlyUserOf(id) || !hasRegister(operation.representation))
        return false;
    if (m_keepsValuesInHeapFrame || m_headOfTurnsLaidOut || !m_loopExits.isEmpty() || !m_sumsOfIntegers.isEmpty())
        return false;
    bool addsOperand = operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub;
    if (addsOperand && (!code().isConstant(operation.operand(1)) || opcodeOf(operation.operand(1)) != Opcode::Integer))
        return false;
    ValueID counter = operation.operand(0);
    while (!code().isConstant(counter) && opcodeOf(counter) == Opcode::Narrowed)
        counter = code().at(counter).operand(0);
    const Operation& carried = code().at(counter);
    if (carried.opcode != Opcode::Parameter || carried.representation != operation.representation || !code().at(carried.block()).isLoopHead)
        return false;
    constexpr size_t mostBlocksSearched = 32;
    for (size_t position = m_positionInOrder; position < m_order.size() && position < m_positionInOrder + mostBlocksSearched; ++position) {
        const Block& block = code().at(m_order[position]);
        if (position != m_positionInOrder && block.isLoopHead)
            return false;
        if (block.terminator.kind != TerminatorKind::Jump || code().at(block.terminator.next()).to != carried.block())
            continue;
        std::span<const ValueID> arguments = code().at(block.terminator.next()).arguments();
        std::span<const ValueID> parameters = code().at(carried.block()).parameters();
        size_t place = std::ranges::find(arguments, id) - arguments.begin();
        if (place < arguments.size())
            return parameters[place] == counter;
    }
    return false;
}

bool CodeGenerator::isRemainderOfBox(ValueID id) const
{
    const Operation& remainder = code().at(id);
    static constexpr OpcodeSet numberUsers = allNumericOperators | allRelations | allEqualities | allBitwiseOperators | OpcodeSet { Opcode::ToBoolean, Opcode::NumberToInteger };
    if (remainder.opcode != Opcode::Mod || remainder.form() != OperatorForm::Numbers || remainder.representation != Representation::Double || (m_followsNumbersOutOfBytes && !m_followsMarkers) || !usesLoopForms(remainder.block()))
        return false;
    ValueID divisor = remainder.rightOperand();
    ValueID dividend = remainder.leftOperand();
    if (!code().isConstant(divisor) || opcodeOf(divisor) != Opcode::Integer || code().at(divisor).integer() < 2 || code().at(divisor).integer() > std::numeric_limits<int32_t>::max() || code().isConstant(dividend))
        return false;
    const Operation& unboxing = code().at(dividend);
    bool isUnboxed = unboxing.opcode == Opcode::Unbox && (unboxing.representation == Representation::Double || unboxing.representation == Representation::OptionalDouble) && unboxing.block() == remainder.block() && code().useCountOf(dividend) == 1;
    if (!isUnboxed || representationOf(unboxing.valueOperand()) != Representation::JSValue || isCarriedUnboxed(unboxing.valueOperand()))
        return false;
    const Type& boxed = typeOf(unboxing.valueOperand());
    if (!(boxed.primitives & Primitive::Integer) || !boxed.hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::UndefinedOrNull))
        return false;
    ValueID user = code().useCountOf(id) == 1 ? code().onlyUserOf(id) : ValueID();
    return user && !numberUsers.contains(opcodeOf(user));
}

bool CodeGenerator::waitsForBoxOfSum(ValueID id, const Operation& operation) const
{
    if (m_carriesSums && isUnboxedBySum(id, operation)) [[unlikely]]
        return true;
    ValueID sum = id;
    if (operation.opcode == Opcode::Unbox && boxPassedToJoin(operation.block(), id)) [[unlikely]]
        return true;
    if (operation.opcode == Opcode::Unbox) {
        if (ValueID remainder = code().onlyUserOf(id); remainder && isRemainderOfBox(remainder)) [[unlikely]]
            return true;
    }
    if (operation.opcode == Opcode::Unbox || operation.opcode == Opcode::IntegerToNumber)
        sum = code().onlyUserOf(id);
    else if (!sumsThatMayStayBoxed.contains(operation.opcode))
        return false;
    ValueID box = sum ? code().onlyUserOf(sum) : ValueID();
    return box && sumBoxedBy(code().at(box)) == sum;
}

NEVER_INLINE void CodeGenerator::dispatchWatched(const ValueContext& context)
{
    uint32_t siteOfUser = FormsSaid::noSite;
    if (m_notesForms)
        siteOfUser = m_records.formsSaid.enter(context.id(), context.opcode(), context.block(), m_proc.numberOfOperations());
    declareAccessesOf(context);

    if (m_checksEffects) {
        RuntimeCallEmitter::WatchOfEffects watch;
        RuntimeCallEmitter::DeedsInFrontOfQuietCall deedsInFrontOfQuietCall = emitter<RuntimeCallEmitter>().countDeedOf(context);
        if (context.handler() && context->traits().mayThrow())
            emitter<RuntimeCallEmitter>().saveDeedsForHandler(deedsInFrontOfQuietCall);
        emitter<RuntimeCallEmitter>().beginWatchOfOperation(context, watch);
        dispatch(context);
        if (deedsInFrontOfQuietCall.all && !m_hasFailed && currentBlockIsOpen())
            emitter<RuntimeCallEmitter>().putBackDeedsBehindQuietCall(deedsInFrontOfQuietCall);
        if (watch.isSet() && !m_hasFailed && currentBlockIsOpen() && !isNeverType(context->type))
            emitter<RuntimeCallEmitter>().endWatchOfEffects(context, watch, nullptr);
        else
            emitter<RuntimeCallEmitter>().abandonWatchOfOperation(watch);
    } else
        dispatch(context);

    if (m_notesForms)
        m_records.formsSaid.leave(siteOfUser, m_proc.numberOfOperations());
}

NEVER_INLINE void CodeGenerator::endScheduledLifetimes(const ValueContext& context)
{
    m_scheduledLifetimeEnds.removeAllMatching([&](const std::pair<ValueID, ValueID>& lent) {
        if (lent.second != context.id())
            return false;
        if (!m_hasFailed && currentBlockIsOpen())
            emitter<HintEmitter>().emitLifetimeEnd(context, lent.first);
        return true;
    });
}

NEVER_INLINE void CodeGenerator::generateWatched(const ValueContext& context)
{
    const OperationContext* user = std::exchange(m_operation, &context);
    Machine::Origin originOfUser = m_out.origin();
    m_out.setOrigin(machineOriginOf(context.id()));
    bool userOwesFenceBehindStores = std::exchange(m_owesFenceBehindStores, false);
    if (m_looksBehindEachOperation) [[unlikely]]
        m_storesBehindWhichOwnerIsRemembered.clear(context.id().index());
    if (m_watchesOperations) [[unlikely]]
        dispatchWatched(context);
    else
        dispatch(context);
    if (m_owesFenceBehindStores) [[unlikely]]
        fail(context, InternalErrorKind::MissingFenceAfterLateStores);
    m_owesFenceBehindStores = userOwesFenceBehindStores;
    if (!m_scheduledLifetimeEnds.isEmpty()) [[unlikely]]
        endScheduledLifetimes(context);
    m_out.setOrigin(originOfUser);
    m_operation = user;
    if (m_looksBehindEachOperation) [[unlikely]] {
        if (barrierIsCountedOn(context.id()) && !m_storesBehindWhichOwnerIsRemembered.get(context.id().index()) && !m_hasFailed)
            fail(context, InternalErrorKind::OpcodeNotInForm, "the barrier of an earlier store is left to this one, and the code of this one does not remember its owner"_s);
        if (m_checksAllocation && user)
            declareAccessesSlow(*user);
        else if (m_checksAllocation)
            m_out.clearDeclaredAccesses();
        if (m_hasKeysAskedTwice && !user && !m_hasFailed && currentBlockIsOpen())
            noteIndexOfKey(context.id());
        if (m_keepsValuesInHeapFrame && !user && !m_hasFailed && currentBlockIsOpen() && !context.isBranchCondition() && !context.returnCall()) {
            SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
            emitter<SavedFrameEmitter>().keepInHeapFrame(context);
        }
    }

    size_t index = context.id().index();
    if (m_values[index] || !hasRegister(context.representation())) [[likely]]
        return;
    if (m_hasFailed || m_definitionTaken || context.isBranchCondition() || context.returnCall() || isNeverType(context->type))
        return;
    RELEASE_ASSERT(hasResults(index));
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generate(const ValueContext& context)
{
    if (m_generatesWatched) [[unlikely]] {
        generateWatched(context);
        return;
    }
    const OperationContext* user = std::exchange(m_operation, &context);
    Machine::Origin originOfUser = m_out.origin();
    m_out.setOrigin(machineOriginOf(context.id()));
    bool userOwesFenceBehindStores = std::exchange(m_owesFenceBehindStores, false);
    dispatch(context);
    if (m_owesFenceBehindStores) [[unlikely]]
        fail(context, InternalErrorKind::MissingFenceAfterLateStores);
    m_owesFenceBehindStores = userOwesFenceBehindStores;
    if (!m_scheduledLifetimeEnds.isEmpty()) [[unlikely]]
        endScheduledLifetimes(context);
    m_out.setOrigin(originOfUser);
    m_operation = user;
    size_t index = context.id().index();
    if (m_values[index] || !hasRegister(context.representation())) [[likely]]
        return;
    if (m_hasFailed || m_definitionTaken || context.isBranchCondition() || context.returnCall() || isNeverType(context->type))
        return;
    RELEASE_ASSERT(hasResults(index));
}

static constexpr OpcodeSet opcodesOfOtherValues = sumsThatMayStayBoxed | OpcodeSet {
    Opcode::HasArrayStorage, Opcode::NewClosure, Opcode::Narrowed, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::GetDeferredClosureSlot, Opcode::NewArray,
};
static constexpr OpcodeSet opcodesMadeElsewhere = opcodesOfOtherValues | OpcodeSet { Opcode::ToString, Opcode::Mul, Opcode::StackStringBuilderFinish };

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generateValue(BlockID block, ValueID id, const Operation& operation, bool isAwaitedAtOnce, std::span<const ValueID> operationsBehind)
{
    if (!m_sumsOfIntegers.isEmpty() && addToSumOfIntegers(id)) [[unlikely]]
        return;
    if (!m_valuesLeftOutOfTurnsLaidOut.isEmpty() && isLeftOutOfTurn(block, id, operation)) [[unlikely]]
        return;
    const bool mayBeMadeElsewhere = opcodesMadeElsewhere.contains(operation.opcode);
    const bool isMadeByOperator = operation.opcode == Opcode::ToString || operation.opcode == Opcode::Mul;
    if (mayBeMadeElsewhere && !isMadeByOperator) {
        if (operation.opcode == Opcode::HasArrayStorage && leavesKindToTurnsLaidOut(block, id, operation)) [[unlikely]]
            return;
        if (isDeferredClosure(operation))
            return;
        if (sumsThatMayStayBoxed.contains(operation.opcode) && isLeftToBackEdge(id, operation)) [[unlikely]] {
            m_stepsLeftToBackEdge.append(id);
            return;
        }
        if (operation.opcode == Opcode::Box && isLeftToHandlers(id)) [[unlikely]]
            return;
        if ((operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble) && waitsForBoxOfSum(id, operation)) [[unlikely]]
            return;
        if (const PlacedSlot* scope = slotOfScopeReadInPlaceOfClosure(id, operation); scope && !operationsBehind.empty() && operationsBehind.front() == code().onlyUserOf(id)) {
            m_closureReadForItsScope = id;
            m_slotOfScopeOfClosureRead = scope;
            return;
        }
        if (operation.opcode == Opcode::NewArray && !operationsBehind.empty() && !m_keepsValuesInHeapFrame && ArrayAllocationEmitter(*this).isLeftToSpreadRightBehind(block, id, operation, operationsBehind)) [[unlikely]] {
            m_literalLeftToSpreadRightBehind = id;
            return;
        }
        if (operation.opcode == Opcode::StackStringBuilderFinish && !m_keepsValuesInHeapFrame && !m_checksEffects && emitter<StringBuilderEmitter>().leavesFinishToAppend(block, id, operation, operationsBehind)) [[unlikely]]
            return;
        if (operation.opcode == Opcode::NewArray && ArrayAllocationEmitter(*this).isLeftToListCall(block, id, operation)) [[unlikely]]
            return;
    }
    if (operation.traits().mayThrow())
        m_isAheadOfWhatMayThrow = false;
    const bool addsToEmptyString = operation.opcode == Opcode::Add && operation.form() == OperatorForm::MayBeString;
    if ((mayBeMadeElsewhere && isMadeByOperator) || addsToEmptyString) [[unlikely]] {
        if ((operation.opcode == Opcode::ToString || addsToEmptyString) && !m_keepsValuesInHeapFrame && !m_checksEffects && emitter<OperatorEmitter>().leavesConversionToConcatenation(block, id, operation, operationsBehind)) {
            m_conversionLeftToConcatenation = id;
            return;
        }
        if (operation.opcode == Opcode::Mul && !m_keepsValuesInHeapFrame && !m_checksEffects) {
            Vector<BlockID, 2> rareBlocksWithProduct;
            if (emitter<OperatorEmitter>().leavesProductToRemainder(block, id, operation, rareBlocksWithProduct)) [[unlikely]] {
                m_productsLeftToRemainders.set(id.index());
                for (BlockID rare : rareBlocksWithProduct)
                    m_productsMadeInRareBlocks.add(rare.index(), Vector<ValueID, 2>()).iterator->value.append(id);
                m_remakesValuesInRareBlocks = true;
                return;
            }
        }
    }
    ValueContext context(block, id, operation, handlerOf(block));
    context.m_isAwaitedAtOnce = isAwaitedAtOnce;
    context.m_operationsBehind = operationsBehind;
    generate(context);
}

size_t CodeGenerator::remakeValuesInRareBlock(BlockID block)
{
    if (auto products = m_productsMadeInRareBlocks.find(block.index()); products != m_productsMadeInRareBlocks.end()) {
        for (ValueID product : products->value)
            m_values[product.index()] = generateProductHere(block, product).value;
    }
    size_t numberOfBigIntsInFrame = m_bigIntsInFrame.size();
    if (!m_copiesBigIntsInRareBlocks)
        return numberOfBigIntsInFrame;
    OperatorEmitter& operators = emitter<OperatorEmitter>();
    for (const BigIntValues::CopyInUnlikelyBlock& copy : operators.bigIntValues().copiesInUnlikelyBlock(block)) {
        if (!m_values[copy.value.index()])
            continue;
        m_bigIntsInFrame.append({ copy.value, m_values[copy.value.index()] });
        ValueContext context(block, copy.producer, code().at(copy.producer), handlerOf(block));
        m_values[copy.value.index()] = operators.copyToHeap(context, get(copy.value)).value;
    }
    return numberOfBigIntsInFrame;
}

void CodeGenerator::forgetRemadeValues(BlockID block, size_t numberOfBigIntsInFrame)
{
    while (m_bigIntsInFrame.size() > numberOfBigIntsInFrame) {
        auto [value, inFrame] = m_bigIntsInFrame.takeLast();
        if (m_values[value.index()])
            m_values[value.index()] = inFrame;
    }
    if (auto products = m_productsMadeInRareBlocks.find(block.index()); products != m_productsMadeInRareBlocks.end()) {
        for (ValueID product : products->value)
            m_values[product.index()] = nullptr;
    }
}

Operand CodeGenerator::generateProductHere(BlockID block, ValueID product)
{
    const Operation& operation = code().at(product);
    m_isAheadOfWhatMayThrow = false;
    ValueContext context(block, product, operation, handlerOf(block));
    generate(context);
    LValue made = std::exchange(m_values[product.index()], nullptr);
    return made ? Operand(operation.representation, made) : zeroInPlaceOfValueOfFailedCode(operation.representation);
}

NEVER_INLINE bool CodeGenerator::isLeftOutOfTurn(BlockID block, ValueID id, const Operation& operation)
{
    if (m_rotatingIteration && generatesInRotatingIteration(id))
        return true;
    if (m_lanes && emitLanes(block, id))
        return true;
    if (m_interleaving && beginInterleavedOperation(id, operation))
        return true;
    return m_valuesLeftOutOfTurnsLaidOut.contains(id);
}

static uint32_t indexOfLiteralUsed(const std::pair<ValueID, ValueID>& use)
{
    return use.first.index();
}

void CodeGenerator::listUsersOfLiterals(BlockID block)
{
    if (m_blockWhoseUsersOfLiteralsAreListed == block)
        return;
    m_blockWhoseUsersOfLiteralsAreListed = block;
    m_usersOfLiteralsInBlock.shrink(0);
    m_literalsStoredIntoByName.shrink(0);
    m_literalsHeldAcrossSuspensions.shrink(0);
    Vector<ValueID, 8> literalsBeforeSpreads;
    auto isLiteral = [&](ValueID value) {
        return value && code().at(value).opcode == Opcode::NewArray;
    };
    for (ValueID user : code().valuesOf(block)) {
        const Operation& operation = code().at(user);
        for (ValueID operand : operation.operands()) {
            if (isLiteral(operand))
                m_usersOfLiteralsInBlock.append({ operand, user });
        }
        if (suspends()) {
            if (operation.traits().maySuspend()) {
                m_literalsHeldAcrossSuspensions.appendVector(literalsBeforeSpreads);
                literalsBeforeSpreads.shrink(0);
            } else if (operation.opcode == Opcode::NewArray)
                literalsBeforeSpreads.append(user);
            else if (operation.opcode == Opcode::AppendElements || operation.opcode == Opcode::AppendIterable) {
                if (size_t place = literalsBeforeSpreads.reverseFind(operation.arrayOperand()); place != notFound)
                    literalsBeforeSpreads.removeAt(place);
            }
        }
        if (operation.opcode != Opcode::PropSet)
            continue;
        ValueID object = operation.objectOperand();
        if (code().at(object).opcode == Opcode::Box)
            object = code().at(object).operand(0);
        if (isLiteral(object))
            m_literalsStoredIntoByName.append(object);
    }
    std::ranges::stable_sort(m_usersOfLiteralsInBlock, { }, indexOfLiteralUsed);
    std::ranges::sort(m_literalsStoredIntoByName, { }, &ValueID::index);
    std::ranges::sort(m_literalsHeldAcrossSuspensions, { }, &ValueID::index);
}

bool CodeGenerator::isLiteralHeldAcrossSuspension(BlockID block, ValueID literal)
{
    listUsersOfLiterals(block);
    return std::ranges::binary_search(m_literalsHeldAcrossSuspensions, literal.index(), { }, &ValueID::index);
}

std::span<const std::pair<ValueID, ValueID>> CodeGenerator::usersOfLiteralInBlock(BlockID block, ValueID literal)
{
    listUsersOfLiterals(block);
    auto users = std::ranges::equal_range(m_usersOfLiteralsInBlock, literal.index(), { }, indexOfLiteralUsed);
    return { users.begin(), users.end() };
}

bool CodeGenerator::isLiteralStoredIntoByName(BlockID block, ValueID literal)
{
    listUsersOfLiterals(block);
    return std::ranges::binary_search(m_literalsStoredIntoByName, literal.index(), { }, &ValueID::index);
}

bool CodeGenerator::pushesElements()
{
    if (!m_pushesElements) {
        m_pushesElements = std::ranges::any_of(m_order, [&](BlockID id) {
            const Terminator& terminator = code().at(id).terminator;
            return (terminator.kind == TerminatorKind::TryCallInline && terminator.inlineBuiltin() == InlineBuiltin::ArrayPush) || std::ranges::any_of(code().valuesOf(id), [&](ValueID value) {
                return opcodeOf(value) == Opcode::CallInline && code().at(value).inlineBuiltin() == InlineBuiltin::ArrayPush;
            });
        });
    }
    return *m_pushesElements;
}

LValue CodeGenerator::bearLiteralAsWritten(BlockID block, ValueID literal)
{
    generateValue(block, literal, code().at(literal), false, { });
    return m_values[literal.index()];
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::generateTerminator(BlockID id, const Block& block, bool generatesCondition)
{
    RELEASE_ASSERT(block.hasEnded() && !m_operation);
    const Terminator& terminator = block.terminator;
    m_isAheadOfWhatMayThrow = false;
    TerminatorContext context(id, terminator, handlerOf(id));
    context.m_generatesCondition = generatesCondition;

    m_out.setOrigin(m_proc.traits().notesRangesOfOrigins ? machineOriginOf(terminator.kind) : Machine::Origin());

    m_operation = &context;
    declareAccessesOf(context);
    if (m_watchesTerminators) [[unlikely]]
        beginTerminatorWatched(context);
    if (m_limitOfWrittenLoop && m_limitOfWrittenLoop->asking == id) [[unlikely]]
        askLimitOfWrittenLoop(context);
    else if (m_rotatingIteration) [[unlikely]]
        generateTerminatorOfRotatingIteration(context);
    else
        dispatch(context);
    if (std::exchange(m_owesFenceBehindStores, false)) [[unlikely]]
        fail(context, InternalErrorKind::MissingFenceAfterLateStores);
    if (m_watchesTerminators) [[unlikely]]
        endTerminatorWatched();
    m_operation = nullptr;
    RELEASE_ASSERT(m_hasFailed || currentBlockIsOpen() == m_nextBlockGoesOnHere);
}

void CodeGenerator::generateBranchCondition(const TerminatorContext& terminator, ValueID condition, BranchTargets targets)
{
    if (ValueID read = std::exchange(m_readLeftToBranch, ValueID()))
        condition = read;
    ValueContext context(terminator.block(), condition, code().at(condition), handlerOf(terminator.block()));
    context.m_branchTargets = targets;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

NEVER_INLINE ValueID CodeGenerator::readLeftToBranchOnItsTruthSlow(BlockID id, std::span<const ValueID> values, ValueID test)
{
    if (code().handlerOf(id))
        return { };
    const Operation& asked = code().at(test);
    ValueID read = values[values.size() - 2];
    if (m_headOfTurnsLaidOut) {
        bool asksTruthOfRead = asked.opcode == Opcode::ToBoolean && asked.form() == OperatorForm::Generic && asked.valueOperand() == read && code().useCountOf(read) == 1;
        return asksTruthOfRead && emitter<ElementAccessEmitter>().asksTruthOfRawElement(read) ? read : ValueID();
    }
    if (asked.opcode == Opcode::ToBoolean && (asked.form() == OperatorForm::Generic || asked.form() == OperatorForm::Numbers) && asked.valueOperand() == read && code().useCountOf(read) == 1 && emitter<ElementAccessEmitter>().testsArrayNumberTruth(read))
        return read;
    if (code().at(id).isInLoop)
        return { };
    if (code().useCountOf(read) == 1 && code().onlyUserOf(read) == test && emitter<AccessEmitter>().integerThatLengthIsAskedFor(id, read, test))
        return read;
    if (asked.opcode != Opcode::ToBoolean || asked.form() != OperatorForm::Generic || asked.valueOperand() != read || code().useCountOf(read) != 1)
        return { };
    return code().at(read).opcode == Opcode::PropGet && emitter<AccessEmitter>().readsTruthAtSite(id, read) ? read : ValueID();
}

std::optional<CodeGenerator::WaysOfTruth> CodeGenerator::waysOfTruthOf(const ValueContext& read) const
{
    BlockID id = read.block();
    const Block& block = code().at(id);
    std::span<const ValueID> behind = read.operationsBehind();
    if (block.isInLoop || m_headOfTurnsLaidOut || behind.size() != 1)
        return std::nullopt;
    static constexpr OpcodeSet tests { Opcode::ToBoolean, Opcode::TypeTest };
    auto testOfEnd = [&](BlockID tested, const Block& ended) -> const Operation* {
        ValueID test = testReadByBranchAlone(tested, ended.terminator);
        return test && tests.contains(opcodeOf(test)) && code().at(test).valueOperand() == read.id() ? &code().at(test) : nullptr;
    };
    auto wayAlong = [&](EdgeID along) -> std::optional<BlockEdge> {
        const Edge& edge = code().at(along);
        const Block& target = code().at(edge.to);
        if (!edge.arguments().empty() || !target.parameters().empty() || target.isLoopHead)
            return std::nullopt;
        return BlockEdge(blockFor(edge.to), edgeClassLeadingTo(target.frequency));
    };
    auto asksFor = [&](const Operation& test, TypeID asked) {
        TypeArena& types = inputs().types;
        return test.opcode == Opcode::TypeTest && types.isSubset(test.testedType(), asked) && types.isSubset(asked, test.testedType());
    };
    if (block.terminator.kind != TerminatorKind::Branch || !tests.contains(opcodeOf(behind[0])))
        return std::nullopt;
    const Operation* first = testOfEnd(id, block);
    if (!first || block.terminator.conditionOperand() != behind[0])
        return std::nullopt;
    std::optional<BlockEdge> whereTrue = wayAlong(block.terminator.taken());
    if (!whereTrue)
        return std::nullopt;
    if (first->opcode == Opcode::ToBoolean) {
        std::optional<BlockEdge> whereFalse = wayAlong(block.terminator.notTaken());
        if (first->form() != OperatorForm::Generic || code().useCountOf(read.id()) != 1 || !whereFalse)
            return std::nullopt;
        return WaysOfTruth { { *whereTrue, *whereFalse }, false };
    }

    BlockID others = code().at(block.terminator.notTaken()).to;
    const Block& blockOfOthers = code().at(others);
    if (!asksFor(*first, TypeTable::object) || code().useCountOf(read.id()) != 3 || m_singlePredecessors[others.index()] != id || code().valuesOf(others).size() != 1 || blockOfOthers.terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    const Operation* second = testOfEnd(others, blockOfOthers);
    if (!second || !asksFor(*second, TypeTable::nullish))
        return std::nullopt;
    BlockID rest = code().at(blockOfOthers.terminator.notTaken()).to;
    std::span<const ValueID> valuesOfRest = code().valuesOf(rest);
    if (m_singlePredecessors[rest.index()] != others || valuesOfRest.empty() || opcodeOf(valuesOfRest[0]) != Opcode::ToBoolean || code().at(valuesOfRest[0]).valueOperand() != read.id())
        return std::nullopt;
    std::optional<BlockEdge> whereNothing = wayAlong(blockOfOthers.terminator.taken());
    if (!whereNothing)
        return std::nullopt;
    return WaysOfTruth { { *whereTrue, *whereNothing }, true };
}

bool CodeGenerator::isReadAgainBehindItsOwnTruthAlone(const ValueContext& read) const
{
    const Block& block = code().at(read.block());
    if (block.terminator.kind != TerminatorKind::Branch)
        return false;
    ValueID test = block.terminator.conditionOperand();
    if (!test || opcodeOf(test) != Opcode::ToBoolean || code().at(test).valueOperand() != read.id() || !std::ranges::contains(read.operationsBehind(), test))
        return false;
    uint32_t uses = 1;
    BlockID inFront = read.block();
    for (EdgeID way = block.terminator.taken(); m_singlePredecessors[code().at(way).to.index()] == inFront;) {
        inFront = code().at(way).to;
        for (ValueID behindYes : code().valuesOf(inFront)) {
            code().at(behindYes).forEachOperand([&](ValueID operand, uint32_t) {
                uses += operand == read.id();
            });
        }
        const Terminator& end = code().at(inFront).terminator;
        if (end.kind != TerminatorKind::Jump)
            break;
        way = end.next();
    }
    return code().useCountOf(read.id()) == uses;
}

bool CodeGenerator::isGuardedByUndefinedTest(const ValueContext& read) const
{
    const Block& block = code().at(read.block());
    if (block.terminator.kind != TerminatorKind::Branch)
        return false;
    ValueID test = block.terminator.conditionOperand();
    if (!test || opcodeOf(test) != Opcode::TypeTest || code().at(test).valueOperand() != read.id() || !std::ranges::contains(read.operationsBehind(), test))
        return false;
    TypeArena& types = inputs().types;
    const Type& type = types.at(read->type);
    TypeID tested = code().at(test).testedType();
    if (type.primitives & (Primitive::Null | Primitive::Absent) || !types.isSubset(TypeTable::undefined, tested) || !types.isSubset(tested, TypeTable::nullish))
        return false;
    uint32_t uses = 1;
    BlockID behind = read.block();
    for (EdgeID way = block.terminator.notTaken(); m_singlePredecessors[code().at(way).to.index()] == behind;) {
        behind = code().at(way).to;
        for (ValueID id : code().valuesOf(behind)) {
            const Operation& narrowed = code().at(id);
            uses += narrowed.opcode == Opcode::Narrowed && !narrowed.isOnTakenEdge() && narrowed.testOperand() == test && narrowed.valueOperand() == read.id() && narrowed.representation == read->representation;
        }
        const Terminator& end = code().at(behind).terminator;
        if (end.kind != TerminatorKind::Jump)
            break;
        way = end.next();
    }
    return code().useCountOf(read.id()) == uses;
}

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), call, code().at(call), handlerOf(terminator.block()));
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

static constexpr OpcodeSet operationsThatThrowWhateverComes { Opcode::ThrowNewError, Opcode::ThrowModuleFailure, Opcode::ThrowNotIterable };

bool CodeGenerator::leavesHoleToTestBehind(const ValueContext& read)
{
    BlockID home = read.block();
    const Block& block = code().at(home);
    if (!isX86_64() || m_keepsValuesInHeapFrame || (m_headOfTurnsLaidOut && !m_rotatingIteration) || !block.isInLoop || block.handler)
        return false;
    ValueID test = testReadByBranchAlone(home, block.terminator);
    if (!test || code().at(test).opcode != Opcode::TypeTest || code().at(test).valueOperand() != read.id())
        return false;
    TypeArena& types = inputs().types;
    const Type& tested = types.at(code().at(test).testedType());
    bool testsNumber = !code().at(test).asksInFull() && tested.isNumber() && types.at(read->type).isAnything();
    if (!testsNumber && (!code().at(test).asksInFull() || tested.primitives))
        return false;
    std::span<const ValueID> values = code().valuesOf(block);
    size_t place = std::ranges::find(values, read.id()) - values.begin();
    for (ValueID behind : values.subspan(std::min(place + 1, values.size()))) {
        const Operation& operation = code().at(behind);
        if (behind != test && (operation.opcode != Opcode::CheckProof || !operation.isLeftToTestBehind() || operation.valueOperand() != read.id()))
            return false;
    }
    if (testsNumber) {
        BlockID passing = code().at(block.terminator.taken()).to;
        uint32_t uses = 1;
        for (ValueID id : code().valuesOf(passing)) {
            const Operation& narrowed = code().at(id);
            uses += narrowed.opcode == Opcode::Narrowed && narrowed.isOnTakenEdge() && narrowed.testOperand() == test && narrowed.valueOperand() == read.id();
        }
        if (m_singlePredecessors[passing.index()] != home || code().useCountOf(read.id()) != uses)
            return false;
        m_elementsWhoseHolesAreLeftToTestsBehind.set(read.id().index());
        return true;
    }
    const Edge& failing = code().at(block.terminator.notTaken());
    const Block& whereItFails = code().at(failing.to);
    if (m_singlePredecessors[failing.to.index()] != home || !failing.arguments().empty() || whereItFails.handler || whereItFails.terminator.kind != TerminatorKind::Unreachable)
        return false;
    m_elementsWhoseHolesAreLeftToTestsBehind.set(read.id().index());
    m_elementsMadePresentAtHeadsOfBlocks.add(failing.to.index(), read.id());
    return true;
}

bool CodeGenerator::deferCheckToTruthTest(const ValueContext& check)
{
    const Type& checked = inputs().types.at(check->checkedType());
    if (!(checked.primitives & Primitive::Boolean) || !checked.hasOnlyPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::Absent))
        return false;
    BlockID home = check.block();
    const Block& block = code().at(home);
    ValueID test = testReadByBranchAlone(home, block.terminator);
    if (!test || code().at(test).opcode != Opcode::ToBoolean || code().at(test).form() != OperatorForm::Bits || code().at(test).valueOperand() != check->valueOperand())
        return false;
    std::span<const ValueID> values = code().valuesOf(block);
    if (values.size() < 2 || values.back() != test || values[values.size() - 2] != check.id())
        return false;
    m_truthTestWithCheck = test;
    m_checkDeferredToTruthTest = check.id();
    return true;
}

bool CodeGenerator::throwsRuntimeErrorImmediately(const Block& block, std::span<const ValueID> errorAndFollowingOperations)
{
    if (block.terminator.kind != TerminatorKind::Throw || block.handler || m_keepsValuesInHeapFrame || errorAndFollowingOperations.size() > 2)
        return false;
    ValueID thrown = errorAndFollowingOperations.back();
    if (errorAndFollowingOperations.size() == 2 && (code().at(thrown).opcode != Opcode::Box || code().at(thrown).valueOperand() != errorAndFollowingOperations.front()))
        return false;
    return block.terminator.valueOperand() == thrown;
}

ValueID CodeGenerator::asyncResultReturnedByStub(const Block& block) const
{
    if (block.terminator.kind != TerminatorKind::Return || !isX86_64() || m_keepsValuesInHeapFrame || m_checksEffects)
        return { };
    ValueID result = block.terminator.valueOperand();
    std::span<const ValueID> values = code().valuesOf(block);
    if (!result || values.empty() || values.back() != result || code().at(result).opcode != Opcode::ReturnAsyncResult || code().useCountOf(result) != 1)
        return { };
    const CallingConventions& conventions = plan().conventions();
    if (conventions.numberOfResults(unit()) != 1 || conventions.resultRepresentation(unit()) != Representation::JSValue || conventions.spillsArgumentRegisters(unit()))
        return { };
    return result;
}

static constexpr size_t numberOfOperationsThatEnterBody = 3;

ValueID CodeGenerator::bodyEnteredAfterNewPromise(const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Return || block.handler || m_checksEffects)
        return { };
    ValueID started = block.terminator.valueOperand();
    std::span<const ValueID> values = code().valuesOf(block);
    if (!started || values.size() < numberOfOperationsThatEnterBody || values.back() != started || code().at(started).opcode != Opcode::CallDirect || code().useCountOf(started) != 1)
        return { };
    const Operation& start = code().at(started);
    const CodeUnit body { unit().function, CodeRole::CoroutineBody };
    ValueID boxed = values[values.size() - 2];
    ValueID promise = values[values.size() - 3];
    if (plan().conventions().unitEnteredBy(start) != body || start.argumentsOperands().empty() || start.argumentsOperands().back() != boxed)
        return { };
    if (code().at(boxed).opcode != Opcode::Box || code().at(boxed).valueOperand() != promise || code().useCountOf(boxed) != 1)
        return { };
    if (code().at(promise).opcode != Opcode::NewPromise || code().useCountOf(promise) != 1 || !emitter<CallEmitter>().findArgumentForPromiseBefore(body))
        return { };
    return started;
}

static constexpr size_t numberOfOperationsThatFulfillPromise = 3;

ValueID CodeGenerator::fulfilledPromiseReturnedByStub(const Block& block) const
{
    if (block.terminator.kind != TerminatorKind::Return || !code().opcodesOfValues().contains(Opcode::FulfillFreshPromise) || block.handler || !isX86_64() || m_keepsValuesInHeapFrame || m_checksEffects)
        return { };
    ValueID boxed = block.terminator.valueOperand();
    std::span<const ValueID> values = code().valuesOf(block);
    if (!boxed || values.size() < numberOfOperationsThatFulfillPromise || values.back() != boxed || code().at(boxed).opcode != Opcode::Box || code().useCountOf(boxed) != 1)
        return { };
    ValueID fulfillment = values[values.size() - 2];
    ValueID promise = values[values.size() - 3];
    if (code().at(boxed).valueOperand() != promise || code().at(promise).opcode != Opcode::NewPromise || code().useCountOf(promise) != 2)
        return { };
    if (code().at(fulfillment).opcode != Opcode::FulfillFreshPromise || code().at(fulfillment).promiseOperand() != promise || code().at(fulfillment).valueOperand() == boxed)
        return { };
    const CallingConventions& conventions = plan().conventions();
    if (conventions.numberOfResults(unit()) != 1 || conventions.resultRepresentation(unit()) != Representation::JSValue || conventions.spillsArgumentRegisters(unit()))
        return { };
    return boxed;
}

bool CodeGenerator::isWordBits(ValueID id) const
{
    static constexpr OpcodeSet opcodes { Opcode::BitAnd, Opcode::RShift, Opcode::URShift };
    const Operation& operation = code().at(id);
    if (!opcodes.contains(operation.opcode) || operation.representation != Representation::Int32 || !code().isConstant(operation.rightOperand()) || code().at(operation.rightOperand()).opcode != Opcode::Integer)
        return false;
    Representation word = representationOf(operation.leftOperand());
    return (word == Representation::Int32 || word == Representation::Int64) && !code().isConstant(operation.leftOperand()) && (operation.form() == OperatorForm::Integers || operation.form() == OperatorForm::Numbers);
}

static constexpr size_t fewestBitsOfOneBlock = 4;
static constexpr OpcodeSet opcodesOfBits { Opcode::BitAnd, Opcode::RShift, Opcode::URShift };

std::span<const ValueID> CodeGenerator::valuesWithBitsAtFirstUses(BlockID id, std::span<const ValueID> values, Vector<ValueID>& inOrderOfUses)
{
    if (!usesLoopForms(id) || m_keepsValuesInHeapFrame || (m_headOfTurnsLaidOut && !m_rotatesUnrolledIterationNumbers))
        return values;
    size_t numberOfBits = std::ranges::count_if(values, [&](ValueID value) { return opcodesOfBits.contains(code().at(value).opcode) && isWordBits(value); });
    if (numberOfBits < fewestBitsOfOneBlock)
        return values;
    inOrderOfUses.reserveInitialCapacity(values.size());
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> waiting;
    UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> conversionsAhead;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> converted;
    if (const Terminator& end = code().at(id).terminator; end.kind == TerminatorKind::Jump && code().at(code().at(end.next()).to).isLoopHead) {
        const Edge& back = code().at(end.next());
        std::span<const ValueID> parameters = code().at(back.to).parameters();
        UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> conversionsOfIntegers;
        for (ValueID value : values) {
            if (code().at(value).opcode == Opcode::IntegerToNumber)
                conversionsOfIntegers.add(code().at(value).valueOperand().index(), value);
        }
        for (size_t place = 0; place < parameters.size() && place < back.arguments().size(); ++place) {
            ValueID next = back.arguments()[place];
            if (representationOf(parameters[place]) != Representation::Int32 || code().useCountOf(parameters[place]) != 1)
                continue;
            ValueID conversion = code().onlyUserOf(parameters[place]);
            if (!conversion || opcodeOf(conversion) != Opcode::IntegerToNumber || code().blockOf(conversion) != id)
                continue;
            if (isWordBits(next) && code().blockOf(next) == id)
                conversionsAhead.add(next.index(), conversion);
            else if (ValueID conversionOfNext = conversionsOfIntegers.get(next.index()); conversionOfNext && (loopCarriedNumberOf(conversion) || (isARM64() && m_rotatesUnrolledIterationNumbers)))
                conversionsAhead.add(conversionOfNext.index(), conversion);
        }
    }
    auto appendConversionAhead = [&](ValueID value) {
        ValueID conversion = conversionsAhead.get(value.index());
        if (!conversion || !converted.add(conversion.index()).isNewEntry)
            return;
        inOrderOfUses.append(conversion);
        ValueID product = code().useCountOf(conversion) == 1 ? code().onlyUserOf(conversion) : ValueID();
        bool isProduct = product && opcodeOf(product) == Opcode::Mul && code().at(product).form() == OperatorForm::Numbers && code().blockOf(product) == id;
        ValueID factor = isProduct ? code().at(product).operand(code().at(product).operand(0) == conversion) : ValueID();
        if (factor && representationOf(factor) == Representation::Double && code().blockOf(factor) != id && converted.add(product.index()).isNewEntry)
            inOrderOfUses.append(product);
    };
    auto append = [&](ValueID value) {
        appendConversionAhead(value);
        Vector<ValueID, 4> chain { value };
        while (waiting.contains(code().at(chain.last()).leftOperand().index()))
            chain.append(code().at(chain.last()).leftOperand());
        while (!chain.isEmpty()) {
            waiting.remove(chain.last().index());
            inOrderOfUses.append(chain.takeLast());
        }
    };
    for (ValueID value : values) {
        if (converted.contains(value.index()))
            continue;
        if (isWordBits(value)) {
            waiting.add(value.index());
            continue;
        }
        appendConversionAhead(value);
        code().at(value).forEachChild([&](ValueID operand) {
            if (waiting.contains(operand.index()))
                append(operand);
        });
        inOrderOfUses.append(value);
        if (code().at(value).opcode == Opcode::IntegerToNumber || code().at(value).opcode == Opcode::Mul)
            converted.add(value.index());
    }
    for (ValueID value : values) {
        if (waiting.contains(value.index()))
            append(value);
    }
    RELEASE_ASSERT(inOrderOfUses.size() == values.size());
    return inOrderOfUses.span();
}

void CodeGenerator::generateBlock(BlockID id, const Block& block)
{
    std::span<const ValueID> values = code().valuesOf(block);
    LValue* elementMadePresent = nullptr;
    LValue elementAsRead = nullptr;
    auto takeElementAsReadAgain = makeScopeExit([&] {
        if (elementMadePresent)
            *elementMadePresent = elementAsRead;
    });
    m_blockOfCachedHandler = id;
    m_cachedHandler = handlerEdgeOf(id, block);
    if (m_followsNumbersOutOfBytes)
        m_blocksBegun.quickSet(id.index());
    if (std::exchange(m_nextBlockGoesOnHere, false))
        RELEASE_ASSERT(blockFor(id) == m_out.m_block && currentBlockIsOpen());
    else if (BlockID predecessor = m_singlePredecessors[id.index()]; !predecessor)
        m_out.appendTo(blockFor(id));
    else if (!m_blocksWithOtherJumps.get(id.index())) [[likely]]
        m_out.appendToSuccessor(blockFor(id), m_scopeAtEndOfBlock[predecessor.index()]);
    else if (RowGuard* guard = findGuardContinuingIn(id); guard && m_out.canRestoreScope(guard->scope))
        m_out.appendToJoin(blockFor(id), guard->scope);
    else
        m_out.appendTo(blockFor(id));
    m_out.setOrigin(Machine::Origin());
    m_isAheadOfWhatMayThrow = true;
    if (m_notesHeadsOfBlocks) [[unlikely]] {
        if (m_rows)
            enterBlockWithRows(id);
        if (m_kindsTried)
            enterBlockWithKindsTried(id);
        for (ValueID parameter : block.parameters()) {
            if (m_hasKeysAskedTwice)
                noteIndexOfKey(parameter);
        }
    }
    if (!m_elementsMadePresentAtHeadsOfBlocks.isEmpty()) [[unlikely]] {
        if (auto made = m_elementsMadePresentAtHeadsOfBlocks.find(id.index()); made != m_elementsMadePresentAtHeadsOfBlocks.end()) {
            elementMadePresent = &m_values[made->value.index()];
            elementAsRead = *elementMadePresent;
            *elementMadePresent = m_out.select(m_out.isZero64(elementAsRead), m_out.constInt64(JSValue::encode(jsUndefined())), elementAsRead);
        }
    }
    if (m_keepsValuesInHeapFrame) [[unlikely]]
        enterBlockOfBodyWithHeapFrame(id, block);
    if (m_carriesNarrowIntegers) [[unlikely]]
        widenCarriedIntegers(block);
    if (m_carriesSums) [[unlikely]]
        takeIntegersOfCarriedSums(block);
    const bool remakesValues = m_remakesValuesInRareBlocks && block.frequency == BlockFrequency::Rare;
    const size_t numberOfBigIntsInFrame = remakesValues ? remakeValuesInRareBlock(id) : 0;
    auto forgetValues = makeScopeExit([&] {
        if (remakesValues) [[unlikely]]
            forgetRemadeValues(id, numberOfBigIntsInFrame);
    });

    static constexpr OpcodeSet resultsReturnedByStubs { Opcode::ReturnAsyncResult, Opcode::NewPromise };
    ValueID owned = valueOwnedByTerminator(block.terminator);
    size_t numberOwned = 1;
    if (!owned && block.terminator.kind == TerminatorKind::Return && code().opcodesOfValues().intersects(resultsReturnedByStubs)) {
        owned = asyncResultReturnedByStub(block);
        if (!owned) {
            owned = bodyEnteredAfterNewPromise(block);
            numberOwned = numberOfOperationsThatEnterBody;
        }
        if (!owned) {
            owned = fulfilledPromiseReturnedByStub(block);
            numberOwned = numberOfOperationsThatFulfillPromise;
        }
    }
    if (owned) {
        RELEASE_ASSERT(values.size() >= numberOwned && values.back() == owned);
        values = values.first(values.size() - numberOwned);
    }
    Vector<ValueID> inOrderOfUses;
    if (block.isInLoop) {
        if (values.size() >= fewestBitsOfOneBlock && code().opcodesOfValues().intersects(opcodesOfBits))
            values = valuesWithBitsAtFirstUses(id, values, inOrderOfUses);
        if (!m_valuesOfEarlierPasses.isEmpty() && !m_headOfTurnsLaidOut) [[unlikely]]
            values = valuesOfLastPass(values, inOrderOfUses);
    }
    if (block.terminator.kind == TerminatorKind::Branch && code().opcodesOfValues().contains(Opcode::Mod) && inOrderOfUses.isEmpty()) [[unlikely]]
        values = emitter<OperatorEmitter>().valuesWithZerosAhead(id, values, inOrderOfUses);
    ValueID test = block.terminator.kind == TerminatorKind::Branch ? testReadByBranchAlone(id, block.terminator) : ValueID();
    bool testWaitsForBranch = false;
    m_readLeftToBranch = readLeftToBranchOnItsTruth(id, values, test);
    static constexpr OpcodeSet readsOfElements { Opcode::ElemGet, Opcode::GetArrayElement };
    const bool looksForElementMoves = mayMoveElements(id);
    std::optional<ElementMoves> moves;
    for (size_t index = 0; index < values.size(); ++index) {
        ValueID value = values[index];
        if (moves && index == moves->end) [[unlikely]]
            endElementMoves(moves);
        if (value == m_readLeftToBranch)
            continue;
        const Operation& operation = code().at(value);
        if (looksForElementMoves && !moves && !testWaitsForBranch && readsOfElements.contains(operation.opcode)) [[unlikely]] {
            if ((moves = findElementMoves(values, index)))
                beginElementMoves(id, *moves);
        }
        if (moves && moves->madeAhead.contains(value)) [[unlikely]]
            continue;
        if (value == test) {
            testWaitsForBranch = true;
            continue;
        }
        if (testWaitsForBranch && (!operation.traits().isPure() || isNeverType(operation.type))) {
            testWaitsForBranch = false;
            generateValue(id, test, code().at(test), false, values.subspan(index));
            if (m_hasFailed)
                return;
        }
        if (operation.opcode == Opcode::NewRuntimeError && throwsRuntimeErrorImmediately(block, values.subspan(index))) {
            ValueContext context(id, value, operation, nullptr);
            m_out.setOrigin(machineOriginOf(value));
            m_isAheadOfWhatMayThrow = false;
            SetForScope<const OperationContext*> whileThrowing(m_operation, &context);
            emitter<ThrowEmitter>().throwRuntimeError(context, operation.runtimeError(), operation.name());
            m_out.unreachable();
            return;
        }
        generateValue(id, value, operation, isAwaitedAtOnce(operation, values, index), testWaitsForBranch ? std::span<const ValueID>() : values.subspan(index + 1));
        if (m_hasFailed)
            return;
        if (!isNeverType(operation.type)) [[likely]]
            continue;
        if (!currentBlockIsOpen())
            return;
        if (operationsThatThrowWhateverComes.contains(operation.opcode)) {
            m_out.unreachable();
            return;
        }
        ValueContext context(id, value, operation, nullptr);
        SetForScope<const OperationContext*> whileStopping(m_operation, &context);
        emitter<ThrowEmitter>().crashOnFalseProof(context, std::nullopt, ProofSource::OtherOperation);
        return;
    }
    if (moves) [[unlikely]]
        endElementMoves(moves);
    if (m_interleaving && block.terminator.kind == TerminatorKind::Branch) [[unlikely]]
        beginInterleavedOperation(block.terminator.conditionOperand(), code().at(block.terminator.conditionOperand()));
    generateTerminator(id, block, testWaitsForBranch);
}

void CodeGenerator::enterBlockOfBodyWithHeapFrame(BlockID id, const Block& block)
{
    SavedFrameEmitter& frames = emitter<SavedFrameEmitter>();
    if (frames.goesOnInStretchAroundLoop(id))
        return;
    frames.beginStretchOfHeapFrame(id);
    if (id == m_order[0])
        frames.clearWordsInMachineFrame();
    for (ValueID parameter : block.parameters()) {
        if (!m_values[parameter.index()])
            continue;
        ValueContext context(id, parameter, code().at(parameter), nullptr);
        SetForScope<const OperationContext*> whileKeeping(m_operation, &context);
        frames.keepInHeapFrame(context);
    }
    if (ValueID neverStored = frames.readBackOutOfHeapFrame(id, ValueID()))
        fail(ValueContext(id, neverStored, code().at(neverStored), nullptr), InternalErrorKind::InvalidSuspension);
}

bool CodeGenerator::blockDominates(BlockID above, BlockID below)
{
    return above == below || dominates(above, below);
}

BlockID CodeGenerator::nearestDominatorOf(BlockID block)
{
    if (m_dominatorsByPlace.empty())
        findDominators();
    uint32_t above = m_dominatorsByPlace[m_places[block.index()]];
    return above ? m_order[above - 1] : BlockID();
}

static constexpr unsigned turnsLaidOut = 8;
static constexpr unsigned turnsLaidOutForEachKindOfView = 4;
static constexpr int32_t turnsBetweenPolls = 1 << 20;
static constexpr int64_t largestInvariantTerm = int64_t { 1 } << 31;
static constexpr int64_t largestWideTerm = int64_t { 1 } << 40;
static constexpr OpcodeSet stepOpcodes { Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet additiveOpcodes = stepOpcodes | OpcodeSet { Opcode::Add, Opcode::Sub };
static constexpr unsigned mostBlocksInTurnLaidOut = 4;
static constexpr unsigned placesLookedAtBehindHead = 32;
static constexpr unsigned placesLookedAtFirst = 8;
static constexpr unsigned mostBlocksInTurnFoundWide = 8;
static constexpr unsigned maxBlocksStoringBeforeTests = 16;
static constexpr int64_t fewestTurnsOfLoopComeBackTo = 64;
static constexpr int64_t turnsThatPayForComingBack = 12;
static constexpr int64_t turnsWrittenForEachTurnOwed = 2;
static constexpr int64_t mostTurnsOwed = 64;
static constexpr int64_t largestStepOfCounter = 16;
static constexpr unsigned mostOperationsInTurnLaidOut = 16;
static constexpr unsigned maxOperationsInWideIteration = 32;
static constexpr unsigned mostOperationsInLanes = 128;
static constexpr unsigned wideIterationsUnrolled = 4;
static constexpr unsigned mostOperationsInRotatingIteration = 96;
static constexpr unsigned rotatingIterationsInRow = 2;
static constexpr int64_t largestStepOfIndex = 16;
static constexpr int64_t largestOffsetOfIndex = 1 << 16;
static constexpr OpcodeSet operationsOfTurnsLaidOut {
    Opcode::Narrowed, Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32, Opcode::ToUint32,
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::DivideIntegers, Opcode::MultiplyIntegers, Opcode::MultiplyAddIntegers, Opcode::Mod, Opcode::RemainderByReciprocal, Opcode::RemainderByWholeDivisor, Opcode::Increment, Opcode::Decrement, Opcode::Negate,
    Opcode::ToNumber, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq, Opcode::TypeTest,
    Opcode::GetStringCodeUnit, Opcode::GetTypedArrayElement, Opcode::GetArrayElement, Opcode::IsElementPresent,
    Opcode::ShapeTest, Opcode::GetSlot,
};
static constexpr AbstractHeapSet heapsThatTurnsLaidOutReadOffObjects { AbstractHeap::ObjectFields, AbstractHeap::ShapeWords, AbstractHeap::NamedProperties };
static_assert(declaredBoundOf(Opcode::ShapeTest).reads.containsOnly(heapsThatTurnsLaidOutReadOffObjects) && declaredBoundOf(Opcode::GetSlot).reads.containsOnly(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::PutTypedArrayElement).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutContiguousIndex).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutDoubleIndex).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::PutModuleVariable).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects) && !declaredBoundOf(Opcode::PutEnvironmentField).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));
static_assert(!declaredBoundOf(Opcode::ExtendArrayLength).writes.containsAny(heapsThatTurnsLaidOutReadOffObjects));

bool CodeGenerator::leavesKindToTurnsLaidOut(BlockID block, ValueID id, const Operation& operation) const
{
    if (operation.arrayStorage() != ArrayStorage::Double || operation.admitsInt32Storage() || operation.isWritable() || operation.data() || m_keepsValuesInHeapFrame || m_headOfTurnsLaidOut)
        return false;
    if (ValueID user = code().onlyUserOf(id); m_order[m_positionInOrder] != block || (user && (opcodeOf(user) != Opcode::GetArrayElement || code().at(user).holdsDoublesOperand() != id)))
        return false;
    static constexpr size_t blocksLookedThroughForHead = 8;
    size_t next = m_positionInOrder + 1;
    size_t end = std::min(m_order.size(), next + blocksLookedThroughForHead);
    while (next < end && !code().at(m_order[next]).isLoopHead)
        ++next;
    if (next == end)
        return false;
    if (m_cachedLoopHeaderPosition != next || m_cachedLoopQueryPosition != m_positionInOrder) {
        m_cachedLoopHeaderPosition = next;
        m_cachedLoopQueryPosition = m_positionInOrder;
        m_cachedCountedLoop = findCountedLoopAt(next);
    }
    const std::optional<CountedLoop>& loop = m_cachedCountedLoop;
    if (!loop || loop->isTooLongToUnroll)
        return false;
    auto told = std::ranges::find(loop->kindsToldToReads, id, &KindToldToReads::holdsDoubles);
    if (told == loop->kindsToldToReads.end() || told->numberOfReads != code().useCountOf(id))
        return false;
    return loop->listsWhoseKindIsRead.contains(told->list) || std::ranges::any_of(loop->listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == told->list && !asked.isStoredInto; });
}

std::optional<CodeGenerator::CountedLoop> CodeGenerator::findCountedLoopAt(size_t positionOfHead) const
{
    BlockID head = m_order[positionOfHead];
    const Block& blockOfHead = code().at(head);
    if (blockOfHead.frequency != BlockFrequency::Normal || code().handlerOf(head) || blockOfHead.subject || blockOfHead.isEntry || blockOfHead.isHandlerBlock())
        return std::nullopt;

    CountedLoop loop { .positionOfHead = positionOfHead };
    BlockID asking = head;
    size_t positionOfAsking = positionOfHead;
    std::span<const ValueID> parameters = blockOfHead.parameters();
    static constexpr OpcodeSet relationsOfCounters { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    auto isTestOfInteger = [&](ValueID test) {
        const Operation& comparison = code().at(test);
        return (comparison.opcode == Opcode::ToBoolean || relationsOfCounters.contains(comparison.opcode)) && comparison.form() == OperatorForm::Integers;
    };
    auto isTestOfParameter = [&](ValueID test) {
        return test && isTestOfInteger(test) && std::ranges::contains(parameters, code().at(test).operand(0));
    };
    ValueID test;
    if (blockOfHead.terminator.kind == TerminatorKind::Jump) {
        const Edge& behindPoll = code().at(blockOfHead.terminator.next());
        if (!behindPoll.arguments().empty() || positionOfHead + 1 == m_order.size() || m_order[positionOfHead + 1] != behindPoll.to || m_singlePredecessors[behindPoll.to.index()] != head)
            return std::nullopt;
        const Block& block = code().at(behindPoll.to);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(behindPoll.to) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return std::nullopt;
        if (!block.parameters().empty())
            return std::nullopt;
        if (code().valuesOf(behindPoll.to).size() <= 1)
            test = code().valuesOf(behindPoll.to).empty() ? testReadByBranchBehindJumpAlone(head, blockOfHead) : testReadByBranchAlone(behindPoll.to, block.terminator);
        loop.testsAtEnd = !isTestOfParameter(test) || code().at(code().at(block.terminator.notTaken()).to).frequency == BlockFrequency::Rare;
        if (loop.testsAtEnd)
            test = ValueID();
        else {
            asking = behindPoll.to;
            ++positionOfAsking;
        }
    } else
        test = testReadByBranchAlone(head, blockOfHead.terminator);
    if (!loop.testsAtEnd && !isTestOfParameter(test))
        return std::nullopt;
    const Block& blockThatAsks = code().at(asking);
    for (ValueID value : code().valuesOf(head)) {
        if (value == test)
            continue;
        if (opcodeOf(value) != Opcode::LoopHint || loop.poll)
            return std::nullopt;
        loop.poll = value;
    }
    auto readTest = [&] {
        const Operation& comparison = code().at(test);
        bool testsTruth = comparison.opcode == Opcode::ToBoolean;
        loop.relation = testsTruth ? Opcode::Greater : comparison.opcode;
        loop.bound = testsTruth ? ValueID() : comparison.rightOperand();
    };
    auto isCounter = [&](ValueID counter) {
        Representation representation = representationOf(counter);
        return (representation == Representation::Int32 || representation == Representation::Int64) && (!loop.bound || (representationOf(loop.bound) == representation && code().blockOf(loop.bound) != head));
    };
    if (!loop.testsAtEnd) {
        readTest();
        loop.counter = code().at(test).operand(0);
        if (!isCounter(loop.counter))
            return std::nullopt;
    }
    if (std::ranges::any_of(parameters, [&](ValueID parameter) { return isCarriedBoxed(parameter) || isCarriedSum(parameter) || findCarriedSubstring(parameter); }))
        return std::nullopt;
    bool rotatesNumbers = false;
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        for (size_t place = 0; place < parameters.size() && place < edge.arguments().size(); ++place) {
            ValueID next = edge.arguments()[place];
            rotatesNumbers |= next && next != parameters[place] && m_values[parameters[place].index()] && representationOf(parameters[place]) == Representation::Double && !code().isConstant(next) && opcodeOf(next) == Opcode::Parameter && code().blockOf(next) == head && representationOf(next) == Representation::Double;
        }
    });
    const Edge& intoTurn = code().at(loop.testsAtEnd ? blockOfHead.terminator.next() : blockThatAsks.terminator.taken());
    if (!intoTurn.arguments().empty() || positionOfAsking + 1 == m_order.size() || m_order[positionOfAsking + 1] != intoTurn.to || m_singlePredecessors[intoTurn.to.index()] != asking || code().valuesOf(intoTurn.to).size() > mostOperationsInLanes + 1)
        return std::nullopt;

    unsigned numberOfPlaces = 0;
    bool mayEndInSwitch = false;
    auto blockAt = [&](unsigned place) {
        return m_order[positionOfAsking + 1 + place];
    };
    auto placeOf = [&](BlockID block) -> std::optional<unsigned> {
        for (unsigned place = 0; place < numberOfPlaces; ++place) {
            if (blockAt(place) == block)
                return place;
        }
        return std::nullopt;
    };
    auto forEachPlaceLedTo = [&](unsigned place, auto&& function) {
        const Terminator& terminator = code().at(blockAt(place)).terminator;
        uint32_t numberOfEdges = findCheckingGuard(blockAt(place)) ? 1 : terminator.numberOfEdges;
        for (uint32_t edge = 0; edge < numberOfEdges; ++edge) {
            if (std::optional<unsigned> target = placeOf(code().at(terminator.edge(edge)).to); target && *target > place)
                function(*target);
        }
    };
    auto leadsBack = [&](const Block& block) {
        return block.terminator.kind == TerminatorKind::Jump && code().at(block.terminator.next()).to == head;
    };
    auto findBackEdgeBlock = [&](BlockID tested) -> BlockID {
        static constexpr unsigned mostBlocks = 4;
        const Terminator& end = code().at(tested).terminator;
        if (!loop.testsAtEnd || end.kind != TerminatorKind::Branch || !isTestOfInteger(end.conditionOperand()))
            return { };
        BlockID from = tested;
        BlockID next = code().at(end.taken()).to;
        for (unsigned number = 0; number < mostBlocks; ++number) {
            const Block& block = code().at(next);
            if (m_singlePredecessors[next.index()] != from || block.terminator.kind != TerminatorKind::Jump || !block.parameters().empty())
                return { };
            if (leadsBack(block))
                return next;
            from = std::exchange(next, code().at(block.terminator.next()).to);
        }
        return { };
    };
    auto endsInTest = [&](const Block& block) {
        return loop.testsAtEnd && block.terminator.kind == TerminatorKind::Branch && !!findBackEdgeBlock(code().at(block.terminator.taken()).from);
    };
    auto endsIteration = [&](const Block& block) {
        return leadsBack(block) || endsInTest(block);
    };
    auto goesStraightToEnd = [&](const Block& block) {
        static constexpr unsigned mostJumps = 6;
        const Block* passed = &block;
        for (unsigned jumps = 0; jumps < mostJumps && passed->terminator.kind == TerminatorKind::Jump && !endsIteration(*passed); ++jumps)
            passed = &code().at(code().at(passed->terminator.next()).to);
        return endsIteration(*passed);
    };
    bool admitsStoresIntoSlots = true;
    const bool countsInNests = (positionOfHead && code().at(m_order[positionOfHead - 1]).isInLoop) || (code().function() && facts().linked().functionsFinishedInNestedIterations.get(code().function().index()));
    auto isCountedTruth = [&](ValueID id) {
        const Operation& truth = code().at(id);
        if (truth.opcode != Opcode::ToNumber || truth.form() != OperatorForm::Booleans || code().useCountOf(id) != 1)
            return false;
        ValueID number = code().onlyUserOf(id);
        if (number && opcodeOf(number) == Opcode::WidenInteger && code().useCountOf(number) == 1)
            number = code().onlyUserOf(number);
        return number && (opcodeOf(number) == Opcode::Add || opcodeOf(number) == Opcode::Sub) && code().at(number).form() == OperatorForm::Integers;
    };
    auto mayBelongToTurn = [&](BlockID id) {
        const Block& block = code().at(id);
        if (!block.isInLoop || block.isLoopHead || code().handlerOf(id) || block.subject || block.isEntry || block.isHandlerBlock() || block.frequency != blockOfHead.frequency)
            return false;
        if (std::ranges::any_of(block.parameters(), [&](ValueID parameter) { return isCarriedBoxed(parameter) || findCarriedSubstring(parameter); }) || findLiteralChain(id))
            return false;
        const RowGuard* checkingGuard = findCheckingGuard(id);
        for (ValueID value : code().valuesOf(id)) {
            const Operation& operation = code().at(value);
            static constexpr OpcodeSet checksOfRow { Opcode::IsPristine, Opcode::HasArrayStorage, Opcode::CheckProof };
            if (checkingGuard && checksOfRow.contains(operation.opcode)) [[unlikely]]
                continue;
            if (operation.opcode == Opcode::LoopHint && !operation.throwsOwedError() && (!loop.poll || loop.poll == value)) {
                loop.poll = value;
                continue;
            }
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayBeTerminated() || isNeverType(operation.type))
                return false;
            if ((operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement) && operation.typedArrayType() == NotTypedArray && !operation.isInsideView())
                return false;
            if (operation.opcode == Opcode::PutTypedArrayElement) {
                const Type& stored = typeOf(operation.valueOperand());
                if (!goesStraightToEnd(block) || stored.hasObjects() || stored.primitives & cellPrimitives)
                    return false;
                continue;
            }
            if (loop.storesAheadOfTests && operation.opcode == Opcode::GetOwnDoubleIndex)
                continue;
            if (operation.opcode == Opcode::PutContiguousIndex || operation.opcode == Opcode::PutDoubleIndex) {
                bool mayStore = goesStraightToEnd(block) || (loop.storesAheadOfTests && operation.opcode == Opcode::PutDoubleIndex);
                if (!mayStore || !typeOf(operation.valueOperand()).isNumber())
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::ExtendArrayLength) {
                if (!endsIteration(block))
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::PutModuleVariable || operation.opcode == Opcode::PutEnvironmentField) {
                if (!leadsBack(block) || opcodeOf(operation.valueOperand()) != Opcode::Box)
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::PutSlot) {
                if (!admitsStoresIntoSlots || operation.level() != KnowledgeLevel::Proven || !typeOf(operation.valueOperand()).isNumber())
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::IsSlotFilled && operation.level() == KnowledgeLevel::Proven)
                continue;
            bool takesTruth = operation.opcode == Opcode::ToNumber && operation.form() == OperatorForm::Booleans && (countsInNests || isCountedTruth(value));
            if (operation.opcode == Opcode::ToNumber && !takesTruth && (operation.form() == OperatorForm::Generic || !typeOf(operation.valueOperand()).isNumberOrNullish()))
                return false;
            auto isCountedNegation = [&] {
                if (operation.opcode != Opcode::LogicalNot || representationOf(operation.valueOperand()) != Representation::Boolean || code().useCountOf(value) != 1)
                    return false;
                ValueID user = code().onlyUserOf(value);
                return user && opcodeOf(user) == Opcode::ToNumber && code().at(user).form() == OperatorForm::Booleans && (countsInNests || isCountedTruth(user));
            };
            if (isCountedNegation())
                continue;
            if (operation.opcode == Opcode::CheckProof && operation.isLeftToTestBehind())
                continue;
            if (operation.opcode == Opcode::ToBoolean) {
                ValueID read = operation.valueOperand();
                const Operation& element = code().at(read);
                if (operation.form() == OperatorForm::Numbers || operation.form() == OperatorForm::Integers)
                    continue;
                if (operation.form() != OperatorForm::Generic || code().isConstant(read) || code().blockOf(read) != id || code().useCountOf(read) != 1 || element.opcode != Opcode::GetArrayElement)
                    return false;
                if (element.arrayStorage() != ArrayStorage::Other || element.level() != KnowledgeLevel::Proven || !element.isInBounds() || element.asksCellWhereNothingIsFound() || representationOf(read) != Representation::JSValue)
                    return false;
                continue;
            }
            if (operation.opcode == Opcode::CallInline && reckonsInLine(operation.inlineBuiltin()) && traits.isPure())
                continue;
            if (!operationsOfTurnsLaidOut.contains(operation.opcode) || traits.mayWriteObservableState())
                return false;
        }
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return true;
        bool endsInSmallSwitch = mayEndInSwitch && terminator.kind == TerminatorKind::Switch && terminator.numberOfCases() < Machine::minCasesForSwitchWindow && (representationOf(terminator.subjectOperand()) == Representation::Int32 || representationOf(terminator.subjectOperand()) == Representation::Int64);
        if (terminator.kind != TerminatorKind::Branch && !endsInSmallSwitch)
            return false;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            if (code().at(terminator.edge(edge)).to == head)
                return false;
        }
        return true;
    };
    Vector<ValueID, 4> listsReadInTurn;
    Vector<ValueID, 2> listsThatTakeRawDoubles;
    auto findTurn = [&](unsigned mostPlaces, bool switches) {
        numberOfPlaces = std::min<size_t>(mostPlaces, m_order.size() - positionOfAsking - 1);
        mayEndInSwitch = switches;
        loop.turn.shrink(0);
        loop.positionsOfTurn.shrink(0);
        loop.flatStrings.shrink(0);
        loop.listsAskedForKind.shrink(0);
        loop.listsWhoseKindIsRead.shrink(0);
        loop.kindsToldToReads.shrink(0);
        loop.viewsAskedForKind.shrink(0);
        loop.boundsAskedInFront.shrink(0);
        std::array<bool, placesLookedAtBehindHead> isComeTo { };
        std::array<bool, placesLookedAtBehindHead> isInTurn { };
        std::optional<unsigned> placeOfLast;
        isComeTo[0] = mayBelongToTurn(intoTurn.to);
        for (unsigned place = 0; place < numberOfPlaces; ++place) {
            if (!isComeTo[place])
                continue;
            if (leadsBack(code().at(blockAt(place)))) {
                placeOfLast = place;
                continue;
            }
            forEachPlaceLedTo(place, [&](unsigned target) {
                if (!isComeTo[target])
                    isComeTo[target] = mayBelongToTurn(blockAt(target));
            });
        }
        if (!placeOfLast)
            return false;
        isInTurn[*placeOfLast] = true;
        for (unsigned place = *placeOfLast; place--;) {
            if (!isComeTo[place] || leadsBack(code().at(blockAt(place))))
                continue;
            forEachPlaceLedTo(place, [&](unsigned target) {
                isInTurn[place] |= isInTurn[target];
            });
        }
        if (!isInTurn[0])
            return false;
        loop.numberOfOperations = 0;
        loop.holdsProductSum = false;
        loop.holesLeaveTurns = true;
        loop.numberOfSlotStores = 0;
        bool hasStoredIntoList = false;
        loop.storesIntoLists = false;
        loop.storesIntoSlots = false;
        listsReadInTurn.shrink(0);
        listsThatTakeRawDoubles.shrink(0);
        unsigned numberOfBlocksThatHoldOperations = 0;
        for (unsigned place = 0; place <= *placeOfLast; ++place) {
            if (!isInTurn[place])
                continue;
            const bool checksRow = findCheckingGuard(blockAt(place));
            numberOfBlocksThatHoldOperations += !code().valuesOf(blockAt(place)).empty() && !checksRow;
            if (!countsInNests)
                numberOfBlocksThatHoldOperations += std::ranges::count_if(code().valuesOf(blockAt(place)), isCountedTruth);
            if (numberOfBlocksThatHoldOperations > (loop.storesAheadOfTests ? maxBlocksStoringBeforeTests : switches ? mostBlocksInTurnFoundWide : mostBlocksInTurnLaidOut))
                return false;
            loop.turn.append(blockAt(place));
            loop.positionsOfTurn.append(positionOfAsking + 1 + place);
            for (ValueID value : code().valuesOf(blockAt(place))) {
                const Operation& operation = code().at(value);
                if (operation.opcode == Opcode::LoopHint)
                    continue;
                if (checksRow && operation.opcode != Opcode::GetArrayElement) [[unlikely]]
                    continue;
                loop.numberOfOperations += operation.opcode != Opcode::Narrowed;
                loop.holdsProductSum |= operation.opcode == Opcode::MultiplyAddIntegers;
                if (loop.numberOfOperations > mostOperationsInLanes)
                    return false;
                if (operation.opcode == Opcode::GetStringCodeUnit && opcodeOf(operation.stringOperand()) == Opcode::FlattenString && !loop.flatStrings.contains(operation.stringOperand()))
                    loop.flatStrings.append(operation.stringOperand());
                loop.holesLeaveTurns &= operation.opcode != Opcode::PutTypedArrayElement;
                loop.numberOfSlotStores += operation.opcode == Opcode::PutSlot;
                loop.storesIntoSlots |= operation.opcode == Opcode::PutSlot;
                bool storesIntoList = operation.opcode == Opcode::PutContiguousIndex || operation.opcode == Opcode::PutDoubleIndex;
                loop.storesIntoLists |= storesIntoList;
                hasStoredIntoList |= storesIntoList || operation.opcode == Opcode::ExtendArrayLength;
                if (operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::IsElementPresent) {
                    loop.holesLeaveTurns &= !hasStoredIntoList;
                    listsReadInTurn.append(operation.arrayOperand());
                }
                if (operation.opcode == Opcode::PutDoubleIndex)
                    listsThatTakeRawDoubles.append(operation.arrayOperand());
                if (operation.opcode == Opcode::PutContiguousIndex && operation.goesByKindOfStorage() && !code().isConstant(operation.arrayOperand())) {
                    ValueID list = operation.arrayOperand();
                    if (!std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list && asked.isStoredInto; }))
                        loop.listsAskedForKind.append({ list, typeOf(operation.valueOperand()).isInt32() ? ArrayStorage::Contiguous : ArrayStorage::Double, true });
                }
                if ((operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement) && operation.typedArrayType() == NotTypedArray) {
                    if (!loop.viewsAskedForKind.contains(operation.viewOperand()))
                        loop.viewsAskedForKind.append(operation.viewOperand());
                }
                if (operation.opcode == Opcode::GetArrayElement && operation.level() == KnowledgeLevel::Proven && !operation.isInBounds() && !operation.isPresent() && !operation.lengthOperands().empty()) {
                    if (std::optional<int64_t> most = mostOfKeyCutByWrittenMask(operation.positionOperand()))
                        loop.boundsAskedInFront.append({ value, operation.lengthOperands()[0], *most });
                }
                if (operation.opcode == Opcode::GetArrayElement && operation.arrayStorage() == ArrayStorage::Other && operation.level() == KnowledgeLevel::Proven && operation.isInBounds() && !operation.asksCellWhereNothingIsFound() && representationOf(value) == Representation::JSValue && (typeOf(value).primitives & Primitive::Number) && !code().isConstant(operation.arrayOperand())) {
                    if (!std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == operation.arrayOperand(); }))
                        loop.listsAskedForKind.append({ operation.arrayOperand(), ArrayStorage::Double, false, true, true });
                    continue;
                }
                if (operation.opcode != Opcode::GetArrayElement || !operation.admitsInt32Storage() || operation.arrayStorage() != ArrayStorage::Double || code().isConstant(operation.arrayOperand()))
                    continue;
                ValueID list = operation.arrayOperand();
                if (ValueID holdsDoubles = operation.holdsDoublesOperand()) {
                    auto told = std::ranges::find(loop.kindsToldToReads, holdsDoubles, &KindToldToReads::holdsDoubles);
                    if (told == loop.kindsToldToReads.end())
                        loop.kindsToldToReads.append({ list, holdsDoubles, 1 });
                    else
                        ++told->numberOfReads;
                }
                if (!operation.isInBounds() && !operation.isPresent()) {
                    if (!loop.listsWhoseKindIsRead.contains(list))
                        loop.listsWhoseKindIsRead.append(list);
                    continue;
                }
                if (std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list; }))
                    continue;
                bool mayHoldFraction = !!(typeOf(value).primitives & Primitive::Number & ~Primitive::NumberWithoutFraction);
                loop.listsAskedForKind.append({ list, mayHoldFraction ? ArrayStorage::Double : ArrayStorage::Int32 });
            }
            if (loop.storesIntoSlots && code().at(blockAt(place)).terminator.kind != TerminatorKind::Jump && !endsInTest(code().at(blockAt(place))))
                return false;
        }
        for (unsigned place = 0; loop.storesIntoSlots && place < *placeOfLast; ++place) {
            unsigned numberOfBlocksOfTurnLedTo = 0;
            if (isInTurn[place]) {
                forEachPlaceLedTo(place, [&](unsigned target) {
                    numberOfBlocksOfTurnLedTo += isInTurn[target];
                });
            }
            if (isInTurn[place] && numberOfBlocksOfTurnLedTo != 1)
                return false;
        }
        return true;
    };
    mayEndInSwitch = true;
    if (!mayBelongToTurn(intoTurn.to))
        return std::nullopt;
    if (!findTurn(placesLookedAtFirst, false) && !findTurn(placesLookedAtBehindHead, true)) {
        admitsStoresIntoSlots = false;
        if (!findTurn(placesLookedAtFirst, false) && !findTurn(placesLookedAtBehindHead, true)) {
            loop.storesAheadOfTests = code().opcodesOfValues().contains(Opcode::GetOwnDoubleIndex);
            loop.numberOfOperations = 0;
            if (!loop.storesAheadOfTests || !findTurn(placesLookedAtBehindHead, false))
                return std::nullopt;
        }
    }
    auto isAmong = [&](ValueID list, const auto& lists) {
        return std::ranges::any_of(lists, [&](ValueID other) { return other == list || (wordOf(list) && wordOf(other) == wordOf(list)); });
    };
    if (std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.isStoredInto && (isAmong(asked.list, listsThatTakeRawDoubles) || isAmong(asked.list, listsReadInTurn)); }))
        return std::nullopt;
    for (ListAskedForKind& asked : loop.listsAskedForKind) {
        if (!isAmong(asked.list, listsThatTakeRawDoubles))
            continue;
        asked.kind = ArrayStorage::Double;
        asked.hasOneKind = true;
    }
    auto isMadeInLoop = [&](ValueID value) {
        BlockID home = code().blockOf(value);
        return home == head || loop.turn.contains(home);
    };
    if (!loop.viewsAskedForKind.isEmpty()) {
        if (std::ranges::any_of(loop.viewsAskedForKind, isMadeInLoop) || std::ranges::any_of(loop.listsAskedForKind, &ListAskedForKind::isStoredInto))
            return std::nullopt;
        for (const ListAskedForKind& asked : std::exchange(loop.listsAskedForKind, { })) {
            if (!loop.listsWhoseKindIsRead.contains(asked.list))
                loop.listsWhoseKindIsRead.append(asked.list);
        }
        loop.boundsAskedInFront.shrink(0);
    }
    loop.flatStrings.removeAllMatching(isMadeInLoop);
    loop.boundsAskedInFront.removeAllMatching([&](const BoundAskedInFront& bound) {
        return isMadeInLoop(bound.length);
    });
    loop.relationsAskedInFront.shrink(0);
    for (BlockID block : loop.turn) {
        const Terminator& end = code().at(block).terminator;
        if (end.kind != TerminatorKind::Branch || !loop.turn.contains(code().at(end.edge(0)).to) || loop.turn.contains(code().at(end.edge(1)).to) || code().at(end.edge(1)).to == head)
            continue;
        const Operation& relation = code().at(end.conditionOperand());
        bool checksKindInPreheader = relation.opcode == Opcode::HasArrayStorage && !isMadeInLoop(end.conditionOperand());
        bool testsInvariantWord = relation.opcode == Opcode::TypeTest && relation.testedType() == TypeTable::int32 && !relation.asksInFull() && representationOf(relation.valueOperand()) == Representation::JSValue && typeOf(relation.valueOperand()).isNumber() && !isMadeInLoop(relation.valueOperand());
        if (!checksKindInPreheader && !testsInvariantWord && (relation.opcode != Opcode::Less || relation.form() != OperatorForm::Integers || isMadeInLoop(relation.operand(0)) || isMadeInLoop(relation.operand(1))))
            continue;
        loop.relationsAskedInFront.append(end.conditionOperand());
    }
    loop.listsWhoseKindIsRead.removeAllMatching([&](ValueID list) {
        return isMadeInLoop(list) || std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == list; });
    });
    loop.listsAskedForKind.removeAllMatching([&](const ListAskedForKind& asked) {
        return isMadeInLoop(asked.list);
    });
    loop.kindsToldToReads.removeAllMatching([&](const KindToldToReads& told) {
        return isMadeInLoop(told.holdsDoubles);
    });

    for (ListAskedForKind& asked : loop.listsAskedForKind) {
        if (loop.holdsProductSum && !asked.isStoredInto && !asked.hasOneKind && asked.kind == ArrayStorage::Double)
            asked.kind = ArrayStorage::Int32;
    }
    loop.asksListsReadAndListsStoredInto = std::ranges::any_of(loop.listsAskedForKind, &ListAskedForKind::isStoredInto) && std::ranges::any_of(loop.listsAskedForKind, [](const ListAskedForKind& asked) { return !asked.isStoredInto && !asked.hasOneKind; });
    const Edge& back = code().at(code().at(loop.turn.last()).terminator.next());
    auto integerHandedOnBy = [&](ValueID value) {
        static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
        while (handingIntegerOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        return value;
    };
    auto broughtInTurn = [&](ValueID value) {
        return findValuePassedInIteration(loop, value);
    };
    if (loop.testsAtEnd) {
        auto last = std::ranges::find_if(loop.turn, [&](BlockID block) { return findBackEdgeBlock(block) == loop.turn.last(); });
        if (last == loop.turn.end() || loop.turn.contains(code().at(code().at(*last).terminator.notTaken()).to))
            return std::nullopt;
        test = code().at(*last).terminator.conditionOperand();
        readTest();
        ValueID tested = integerHandedOnBy(code().at(test).operand(0));
        for (size_t index = 0; index < parameters.size() && !loop.counter; ++index) {
            if (tested == parameters[index] || (m_values[parameters[index].index()] && broughtInTurn(back.arguments()[index]) == tested))
                loop.counter = parameters[index];
        }
        if (!loop.counter || !isCounter(loop.counter) || (loop.bound && isMadeInLoop(loop.bound)))
            return std::nullopt;
    }
    ValueID stepped = broughtInTurn(back.arguments()[std::ranges::find(parameters, loop.counter) - parameters.begin()]);
    if (!stepped)
        return std::nullopt;
    const Operation& step = code().at(stepped);
    static constexpr OpcodeSet stepsOfCounters { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    if (!stepsOfCounters.contains(step.opcode) || step.form() != OperatorForm::Integers)
        return std::nullopt;
    ValueID steppedFrom;
    if (step.opcode == Opcode::Increment || step.opcode == Opcode::Decrement) {
        steppedFrom = step.valueOperand();
        loop.step = step.opcode == Opcode::Increment ? 1 : -1;
    } else {
        ValueID constant = step.rightOperand();
        if (!code().isConstant(constant) || opcodeOf(constant) != Opcode::Integer)
            return std::nullopt;
        steppedFrom = step.leftOperand();
        loop.step = step.opcode == Opcode::Add ? code().at(constant).integer() : -code().at(constant).integer();
    }
    bool countsUp = loop.relation == Opcode::Less || loop.relation == Opcode::LessEq;
    if (integerHandedOnBy(steppedFrom) != loop.counter || !loop.step || loop.step > 0 != countsUp || std::abs(loop.step) > largestStepOfCounter)
        return std::nullopt;
    if (loop.testsAtEnd && integerHandedOnBy(code().at(test).operand(0)) != loop.counter)
        loop.addedBeforeTest = loop.step;
    Range counters = typeOf(loop.counter).integers;
    Range bounds = loop.bound ? typeOf(loop.bound).integers : Range { 0, 0 };
    loop.mostTurns = (countsUp ? bounds.maximum - counters.minimum : counters.maximum - bounds.minimum) / std::abs(loop.step) + 1;
    if (loop.mostTurns < static_cast<int64_t>(turnsLaidOut))
        return std::nullopt;
    if (loop.testsAtEnd) {
        loop.relationsOfCounterBelowBound.append(test);
        auto isBound = [&](ValueID value) {
            return value == loop.bound || (code().isConstant(value) && opcodeOf(value) == Opcode::Integer && (loop.bound ? code().isConstant(loop.bound) && code().at(loop.bound).integer() == code().at(value).integer() : !code().at(value).integer()));
        };
        for (BlockID block : loop.turn) {
            const Terminator& end = code().at(block).terminator;
            if (end.kind != TerminatorKind::Branch || !loop.turn.contains(code().at(end.taken()).to) || !isTestOfInteger(end.conditionOperand()))
                continue;
            const Operation& guard = code().at(end.conditionOperand());
            if (loop.addedBeforeTest && guard.opcode == loop.relation && integerHandedOnBy(guard.operand(0)) == loop.counter && isBound(guard.operand(1)))
                loop.relationsOfCounterBelowBound.append(end.conditionOperand());
        }
    }
    bool isCounterBelowBound = loop.step == 1 && loop.relation == Opcode::Less && counters.minimum >= 0;
    if (isCounterBelowBound || (loop.step == 1 && loop.relation == Opcode::LessEq && loop.bound && !loop.testsAtEnd)) {
        auto holdAgainstBound = [&](ValueID length) {
            if (!loop.lengthsHeldAgainstBound.contains(length))
                loop.lengthsHeldAgainstBound.append(length);
        };
        auto isLengthFromInFront = [&](ValueID length) {
            return !isMadeInLoop(length) && typeOf(length).isInteger() && typeOf(length).integers.minimum >= 0 && (representationOf(length) == Representation::Int32 || representationOf(length) == Representation::Int64);
        };
        auto findStepped = [&](ValueID value, int64_t& added) -> ValueID {
            for (value = broughtInTurn(value); value; value = broughtInTurn(value)) {
                const Operation& sum = code().at(value);
                if (code().isConstant(value) || !stepsOfCounters.contains(sum.opcode) || sum.form() != OperatorForm::Integers)
                    return value;
                if (sum.opcode == Opcode::Increment || sum.opcode == Opcode::Decrement) {
                    added += sum.opcode == Opcode::Increment ? 1 : -1;
                    value = sum.valueOperand();
                } else {
                    ValueID constant = sum.rightOperand();
                    if (!code().isConstant(constant) || opcodeOf(constant) != Opcode::Integer || std::abs(code().at(constant).integer()) > largestOffsetOfIndex)
                        return value;
                    added += sum.opcode == Opcode::Add ? code().at(constant).integer() : -code().at(constant).integer();
                    value = sum.leftOperand();
                }
            }
            return { };
        };
        auto holdSteppedIndex = [&](ValueID position, ValueID length) {
            if (!loop.bound || loop.testsAtEnd || !typeOf(position).isInteger() || typeOf(position).integers.minimum < 0)
                return false;
            SteppedIndex held { 0, 0, 0, length };
            ValueID carried = findStepped(position, held.offset);
            held.indexOfParameter = std::ranges::find(parameters, carried) - parameters.begin();
            if (held.indexOfParameter == parameters.size() || !m_values[carried.index()] || (representationOf(carried) != Representation::Int32 && representationOf(carried) != Representation::Int64))
                return false;
            if (findStepped(back.arguments()[held.indexOfParameter], held.step) != carried) {
                held.step = 0;
                if (!isCounterBelowBound || held.offset || !carriesOnlyCounter(loop, back.arguments()[held.indexOfParameter], carried, 0))
                    return false;
                holdAgainstBound(length);
            }
            if (std::abs(held.step) > largestStepOfIndex || std::abs(held.offset) > largestOffsetOfIndex)
                return false;
            for (SteppedIndex& other : loop.steppedIndexes) {
                if (other.indexOfParameter == held.indexOfParameter && other.step == held.step && other.length == length) {
                    other.offset = std::max(other.offset, held.offset);
                    return true;
                }
            }
            loop.steppedIndexes.append(held);
            return true;
        };
        for (BlockID block : loop.turn) {
            for (ValueID value : code().valuesOf(block)) {
                const Operation& operation = code().at(value);
                bool testsItsPosition = operation.opcode == Opcode::IsElementPresent ? !operation.isInBounds() : operation.opcode == Opcode::GetArrayElement && operation.level() == KnowledgeLevel::Proven && !operation.isInBounds() && !operation.isPresent();
                if (!testsItsPosition || operation.lengthOperands().empty() || !isLengthFromInFront(operation.lengthOperands()[0]))
                    continue;
                ValueID length = operation.lengthOperands()[0];
                if (isCounterBelowBound && integerHandedOnBy(operation.positionOperand()) == loop.counter)
                    holdAgainstBound(length);
                else if (!holdSteppedIndex(operation.positionOperand(), length))
                    continue;
                loop.accessesUnderCounterBelowBound.append(value);
            }
            const Terminator& end = code().at(block).terminator;
            if (end.kind != TerminatorKind::Branch || !loop.turn.contains(code().at(end.edge(0)).to) || loop.turn.contains(code().at(end.edge(1)).to) || code().at(end.edge(1)).to == head)
                continue;
            const Operation& relation = code().at(end.conditionOperand());
            if (relation.opcode != Opcode::Less || relation.form() != OperatorForm::Integers || !isLengthFromInFront(relation.operand(1)))
                continue;
            if (isCounterBelowBound && integerHandedOnBy(relation.operand(0)) == loop.counter)
                holdAgainstBound(relation.operand(1));
            else if (!holdSteppedIndex(relation.operand(0), relation.operand(1)))
                continue;
            loop.relationsOfCounterBelowBound.append(end.conditionOperand());
        }
    }
    for (const RowGuard& guard : m_rows ? m_rows->guards.span() : std::span<const RowGuard>()) {
        if (!loop.turn.contains(guard.presenceBlock) && !std::ranges::any_of(guard.checkBlocks, [&](BlockID block) { return loop.turn.contains(block); }))
            continue;
        bool isUnderCounter = isCounterBelowBound && loop.bound && !loop.testsAtEnd && guard.bound == loop.bound && integerHandedOnBy(guard.position) == loop.counter && !isMadeInLoop(guard.array);
        if (!isUnderCounter || !loop.turn.contains(guard.presenceBlock) || !loop.turn.contains(guard.continuation) || !std::ranges::all_of(guard.checkBlocks, [&](BlockID block) { return loop.turn.contains(block); }))
            return std::nullopt;
        if (std::ranges::any_of(loop.turn, [&](BlockID block) { return m_rows->changingBlocks.quickGet(block.index()); }))
            return std::nullopt;
        loop.relationsOfCounterBelowBound.append(code().at(guard.presenceBlock).terminator.conditionOperand());
        for (BlockID block : guard.checkBlocks) {
            loop.relationsOfCounterBelowBound.append(code().at(block).terminator.conditionOperand());
        }
        bool isAskedByOther = std::ranges::any_of(m_rows->guards, [&](const RowGuard& other) { return other.precedesOther && !guard.precedesOther && other.presenceBlock == guard.presenceBlock; });
        if (!isAskedByOther)
            loop.rowCountsAsked |= 1 << guard.count;
    }
    if (loop.numberOfOperations > maxOperationsInWideIteration || (loop.numberOfOperations > mostOperationsInTurnLaidOut && loop.steppedIndexes.isEmpty())) {
        loop.rotatesNumbers = rotatesNumbers && loop.numberOfOperations <= mostOperationsInRotatingIteration && !loop.testsAtEnd && loop.viewsAskedForKind.isEmpty() && loop.listsAskedForKind.isEmpty() && loop.listsWhoseKindIsRead.isEmpty() && std::ranges::all_of(loop.turn, [&](BlockID block) {
            return code().at(block).terminator.kind == TerminatorKind::Jump;
        });
        loop.isTooLongToUnroll = !loop.rotatesNumbers;
    }
    if (loop.storesAheadOfTests) {
        loop.rotatesNumbers = false;
        loop.isTooLongToUnroll = true;
    }
    loop.asking = asking;
    if (!findStoresOfWhatComesRound(loop, back))
        return std::nullopt;
    unsigned numberOfSlotStoresLeftOut = std::ranges::count_if(loop.loopCarriedValueStores, [&](const LoopCarriedValueStore& store) { return opcodeOf(store.store) == Opcode::PutSlot; });
    loop.holesLeaveTurns &= numberOfSlotStoresLeftOut == loop.numberOfSlotStores;
    if (loop.isTooLongToUnroll && (loop.rowCountsAsked || (!findLanes(loop) && !findWideLoop(loop))))
        return std::nullopt;
    loop.carriesJoinedValue = std::ranges::any_of(back.arguments(), [&](ValueID handedOn) {
        return std::ranges::any_of(loop.turn, [&](BlockID block) { return block != head && std::ranges::contains(code().at(block).parameters(), handedOn); });
    });
    loop.mayComeBack = loop.bound && !loop.testsAtEnd && !m_keepsValuesInHeapFrame && loop.loopCarriedValueStores.isEmpty() && loop.mostTurns >= fewestTurnsOfLoopComeBackTo && (someUnusualTurnComesRound(loop) || someHoleLeavesTurn(loop)) && loop.viewsAskedForKind.isEmpty();
    loop.mayComeBack &= !loop.storesIntoLists && !loop.rowCountsAsked;
    if (loop.mayComeBack && loop.listsAskedForKind.isEmpty() && loop.listsWhoseKindIsRead.isEmpty()) {
        BlockID head = m_order[positionOfHead];
        code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
            if (edge.to == head && edge.from != head && m_scopeAtEndOfBlock[edge.from.index()].block && code().at(edge.from).isInLoop)
                loop.mayComeBack = false;
        });
    }
    versionIterationsByKind(loop);
    findSumsOfIntegers(loop, back);
    findExactCounters(loop, back);
    return loop;
}

ValueID CodeGenerator::findValuePassedInIteration(const CountedLoop& loop, ValueID value) const
{
    static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    while (true) {
        while (handingIntegerOn.contains(opcodeOf(value)))
            value = code().at(value).valueOperand();
        BlockID home = opcodeOf(value) == Opcode::Parameter ? code().blockOf(value) : BlockID();
        if (!home || home == m_order[loop.positionOfHead] || !loop.turn.contains(home))
            return value;
        std::span<const ValueID> taken = code().at(home).parameters();
        size_t place = std::ranges::find(taken, value) - taken.begin();
        ValueID brought;
        for (BlockID from : loop.turn) {
            const Terminator& end = code().at(from).terminator;
            for (uint32_t edge = 0; edge < end.numberOfEdges; ++edge) {
                const Edge& into = code().at(end.edge(edge));
                if (into.to != home)
                    continue;
                if (place >= into.arguments().size() || (brought && brought != into.arguments()[place]))
                    return { };
                brought = into.arguments()[place];
            }
        }
        if (!brought)
            return { };
        value = brought;
    }
}

bool CodeGenerator::carriesOnlyCounter(const CountedLoop& loop, ValueID value, ValueID carried, unsigned depth) const
{
    static constexpr unsigned mostJoins = 4;
    static constexpr OpcodeSet handingIntegerOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    while (handingIntegerOn.contains(opcodeOf(value)))
        value = code().at(value).valueOperand();
    if (value == carried || value == loop.counter)
        return true;
    BlockID home = opcodeOf(value) == Opcode::Parameter ? code().blockOf(value) : BlockID();
    if (!home || home == m_order[loop.positionOfHead] || !loop.turn.contains(home) || depth == mostJoins)
        return false;
    size_t place = std::ranges::find(code().at(home).parameters(), value) - code().at(home).parameters().begin();
    return std::ranges::all_of(loop.turn, [&](BlockID from) {
        const Terminator& end = code().at(from).terminator;
        for (uint32_t edge = 0; edge < end.numberOfEdges; ++edge) {
            const Edge& into = code().at(end.edge(edge));
            if (into.to == home && (place >= into.arguments().size() || !carriesOnlyCounter(loop, into.arguments()[place], carried, depth + 1)))
                return false;
        }
        return true;
    });
}

void CodeGenerator::versionIterationsByKind(CountedLoop& loop) const
{
    auto areReadAsDoubles = [&] {
        return std::ranges::all_of(loop.listsAskedForKind, [](const ListAskedForKind& asked) { return asked.kind == ArrayStorage::Double && !asked.isStoredInto && !asked.hasOneKind && !asked.otherKindsAreNotAsked; });
    };
    if (loop.listsWhoseKindIsRead.isEmpty()) {
        loop.secondTurnsGoByKindsRead = loop.listsAskedForKind.size() > 1 && loop.viewsAskedForKind.isEmpty() && !loop.storesIntoLists && areReadAsDoubles()
            && std::ranges::all_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return std::ranges::contains(loop.kindsToldToReads, asked.list, &KindToldToReads::list); });
        return;
    }
    auto onlyTestsIndexingType = [&](ValueID value) {
        const Operation& read = code().at(value);
        return read.opcode != Opcode::GetArrayElement || !loop.listsWhoseKindIsRead.contains(read.arrayOperand()) || loop.accessesUnderCounterBelowBound.contains(value);
    };
    bool areReadsBelowBound = loop.viewsAskedForKind.isEmpty() && std::ranges::all_of(loop.turn, [&](BlockID block) {
        return std::ranges::all_of(code().valuesOf(block), onlyTestsIndexingType);
    });
    if (!loop.listsAskedForKind.isEmpty()) {
        if (!areReadsBelowBound || !areReadAsDoubles() || loop.storesIntoLists)
            return;
    }
    if (!loop.mayComeBack && !areReadsBelowBound && loop.storesIntoLists)
        return;
    loop.secondTurnsGoByKindsRead = loop.listsAskedForKind.size() + loop.listsWhoseKindIsRead.size() > 1;
    for (ValueID list : std::exchange(loop.listsWhoseKindIsRead, { }))
        loop.listsAskedForKind.append({ list, ArrayStorage::Double });
}

static_assert(!operationsOfTurnsLaidOut.contains(Opcode::GetModuleVariable) && !operationsOfTurnsLaidOut.contains(Opcode::GetEnvironmentField));

bool CodeGenerator::isOnlySlotAccess(const CountedLoop& loop, ValueID store) const
{
    static constexpr OpcodeSet slotAccesses { Opcode::GetSlot, Opcode::PutSlot, Opcode::IsSlotFilled };
    const Operation& put = code().at(store);
    BlockID home = code().isConstant(put.objectOperand()) ? BlockID() : code().blockOf(put.objectOperand());
    if (home && (home == m_order[loop.positionOfHead] || loop.turn.contains(home)))
        return false;
    return std::ranges::none_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
            const Operation& access = code().at(value);
            return value != store && slotAccesses.contains(access.opcode) && access.key() == put.key();
        });
    });
}

bool CodeGenerator::mayIterationThrow(const CountedLoop& loop) const
{
    auto mayLeaveByThrow = [&](const Operation& operation) {
        if (!operation.traits().mayThrow())
            return false;
        if (operation.opcode != Opcode::GetArrayElement || operation.asksCellWhereNothingIsFound() || !hasArrayStorageForm(operation.arrayStorage()))
            return true;
        const Type& position = typeOf(operation.positionOperand());
        return !position.isInteger() || !position.integers.isSubsetOf(Range { 0, std::numeric_limits<uint32_t>::max() - 1 });
    };
    return std::ranges::any_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
            return mayLeaveByThrow(code().at(value));
        });
    });
}

bool CodeGenerator::addsIntegerToParameter(ValueID sum, ValueID parameter) const
{
    auto isIntegerTerm = [&](ValueID number) {
        const Operation& term = code().at(number);
        return (term.opcode == Opcode::IntegerToNumber && representationOf(term.valueOperand()) == Representation::Int32) || (term.opcode == Opcode::Integer && isInBounds<int32_t>(term.integer()));
    };
    for (ValueID last = sum;;) {
        const Operation& addition = code().at(sum);
        if (code().isConstant(sum) || !additiveOpcodes.contains(addition.opcode) || addition.form() != OperatorForm::Numbers || (sum != last && code().useCountOf(sum) != 1))
            return false;
        bool isStep = stepOpcodes.contains(addition.opcode);
        bool termIsRight = isStep || addition.opcode == Opcode::Sub || addition.leftOperand() == parameter || (addition.rightOperand() != parameter && isIntegerTerm(addition.rightOperand()));
        ValueID before = addition.operand(termIsRight ? 0 : 1);
        if (!isStep && !isIntegerTerm(addition.operand(termIsRight ? 1 : 0)))
            return false;
        if (before == parameter)
            return true;
        sum = before;
    }
}

std::optional<Vector<CodeGenerator::SumOfIntegers::EarlierTerm, 1>> CodeGenerator::findEarlierTerms(ValueID last, ValueID parameter) const
{
    auto termBeside = [&](ValueID sum, ValueID number) -> ValueID {
        const Operation& term = code().at(number);
        if (code().isConstant(number))
            return term.opcode == Opcode::Integer && isInBounds<int32_t>(term.integer()) ? number : ValueID();
        bool isConverted = term.opcode == Opcode::IntegerToNumber && term.representation == Representation::Double && code().useCountOf(number) == 1 && term.block() == code().blockOf(sum);
        bool isInteger = isConverted && (representationOf(term.valueOperand()) == Representation::Int32 || representationOf(term.valueOperand()) == Representation::Int64) && typeOf(term.valueOperand()).isInteger();
        return isInteger ? term.valueOperand() : ValueID();
    };
    auto isEarlierSum = [&](ValueID number) {
        if (code().isConstant(number) || code().blockOf(number) != code().blockOf(last) || code().useCountOf(number) != 1)
            return false;
        const Operation& addition = code().at(number);
        return additiveOpcodes.contains(addition.opcode) && addition.form() == OperatorForm::Numbers && addition.representation == Representation::Double;
    };
    Vector<SumOfIntegers::EarlierTerm, 1> terms;
    for (ValueID sum = last;;) {
        const Operation& addition = code().at(sum);
        bool isStep = stepOpcodes.contains(addition.opcode);
        bool isSubtraction = addition.opcode == Opcode::Sub || addition.opcode == Opcode::Decrement;
        ValueID left = addition.operand(0);
        ValueID right = isStep ? ValueID() : addition.operand(1);
        bool readsParameter = left == parameter || (right == parameter && !isSubtraction);
        ValueID before = isStep || isSubtraction || left == parameter || (!readsParameter && isEarlierSum(left)) ? left : right;
        if (!readsParameter && !isEarlierSum(before))
            return std::nullopt;
        if (sum != last) {
            ValueID termAsNumber = before == left ? right : left;
            ValueID term = isStep ? ValueID() : termBeside(sum, termAsNumber);
            if (!term && !isStep)
                return std::nullopt;
            terms.append({ sum, termAsNumber, term, isSubtraction });
        }
        if (readsParameter)
            return terms;
        sum = before;
    }
}

bool CodeGenerator::findStoresOfWhatComesRound(CountedLoop& loop, const Edge& back) const
{
    BlockID last = loop.turn.last();
    std::span<const ValueID> parameters = code().at(m_order[loop.positionOfHead]).parameters();
    for (BlockID block : loop.storesIntoSlots ? loop.turn.span() : std::span<const BlockID>()) {
        for (ValueID value : code().valuesOf(block)) {
            const Operation& store = code().at(value);
            if (store.opcode != Opcode::PutSlot)
                continue;
            ValueID box = opcodeOf(store.valueOperand()) == Opcode::Box ? store.valueOperand() : ValueID();
            ValueID sum = box ? code().at(box).valueOperand() : store.valueOperand();
            size_t index = std::ranges::find(back.arguments(), sum) - back.arguments().begin();
            Representation carried = index < parameters.size() ? representationOf(parameters[index]) : Representation::Void;
            bool isCarried = !code().isConstant(sum) && (carried == Representation::Double || (carried == Representation::Int64 && box && (opcodeOf(sum) == Opcode::NumberToInteger || (additiveOpcodes.contains(opcodeOf(sum)) && code().at(sum).form() == OperatorForm::Integers)))) && m_values[parameters[index].index()] && representationOf(sum) == carried;
            if (!isCarried || (!box && !addsIntegerToParameter(sum, parameters[index])) || code().barrierIsCountedOn(value) || !isOnlySlotAccess(loop, value) || !loop.turn.contains(code().blockOf(sum)))
                continue;
            if ((box && (!loop.turn.contains(code().blockOf(box)) || code().useCountOf(box) != 1)) || someUnusualTurnComesRound(loop) || mayIterationThrow(loop))
                continue;
            loop.loopCarriedValueStores.append({ index, sum, box, value });
        }
    }
    for (ValueID value : code().valuesOf(last)) {
        const Operation& store = code().at(value);
        if (store.opcode != Opcode::PutModuleVariable && store.opcode != Opcode::PutEnvironmentField)
            continue;
        if (code().barrierIsCountedOn(value))
            return false;
        bool storesIntoScope = store.opcode == Opcode::PutEnvironmentField;
        if (storesIntoScope) {
            BlockID home = code().blockOf(store.environmentOperand());
            if (home == m_order[loop.positionOfHead] || loop.turn.contains(home) || !plan().isInUse(store.layout()))
                return false;
        }
        ValueID box = store.valueOperand();
        const Operation& boxing = code().at(box);
        bool placeHoldsValues = storesIntoScope && plan().slot(store.layout(), store.field()).representation == SlotRepresentation::JSValue;
        bool isExactCounter = boxing.opcode == Opcode::Box && boxing.representation == Representation::OptionalInt64 && (opcodeOf(boxing.valueOperand()) == Opcode::NumberToInteger || (additiveOpcodes.contains(opcodeOf(boxing.valueOperand())) && code().at(boxing.valueOperand()).form() == OperatorForm::Integers));
        if (boxing.opcode != Opcode::Box || boxing.block() != last || (!isExactCounter && boxing.representation != (placeHoldsValues ? Representation::JSValue : Representation::OptionalDouble)) || code().useCountOf(box) != 1)
            return false;
        ValueID loopCarriedValue = boxing.valueOperand();
        Representation carried = isExactCounter ? Representation::Int64 : Representation::Double;
        if (representationOf(loopCarriedValue) != carried || !loop.turn.contains(code().blockOf(loopCarriedValue)))
            return false;
        size_t index = std::ranges::find(back.arguments(), loopCarriedValue) - back.arguments().begin();
        if (index == parameters.size() || !m_values[parameters[index].index()] || representationOf(parameters[index]) != carried)
            return false;
        for (const LoopCarriedValueStore& other : loop.loopCarriedValueStores) {
            const Operation& stored = code().at(other.store);
            if (stored.opcode != store.opcode)
                continue;
            if (storesIntoScope ? stored.layout() == store.layout() && stored.field() == store.field() : stored.variable() == store.variable())
                return false;
        }
        loop.loopCarriedValueStores.append({ index, loopCarriedValue, box, value });
    }
    if (loop.loopCarriedValueStores.isEmpty())
        return true;
    return !mayIterationThrow(loop);
}

void CodeGenerator::storeLoopCarriedValue(const CountedLoop& loop, const LoopCarriedValueStore& store, LValue value)
{
    Vector<ValueID, 4> leftOut = std::exchange(m_valuesLeftOutOfTurnsLaidOut, { });
    Vector<SumOfIntegers, 2> sums = std::exchange(m_sumsOfIntegers, { });
    LValue loopCarriedValue = std::exchange(m_values[store.loopCarriedValue.index()], value->type() == Machine::Int64 ? value : emitter<ConversionEmitter>().purified(Number(value)).value());
    LValue box = store.box ? std::exchange(m_values[store.box.index()], nullptr) : nullptr;
    if (store.box)
        generateValue(loop.turn.last(), store.box, code().at(store.box), false, { });
    generateValue(loop.turn.last(), store.store, code().at(store.store), false, { });
    m_values[store.loopCarriedValue.index()] = loopCarriedValue;
    if (store.box)
        m_values[store.box.index()] = box;
    m_sumsOfIntegers = WTF::move(sums);
    m_valuesLeftOutOfTurnsLaidOut = WTF::move(leftOut);
}

void CodeGenerator::findSumsOfIntegers(CountedLoop& loop, const Edge& back) const
{
    constexpr int64_t largestSum = int64_t { 1 } << 53;
    constexpr int64_t mostAddedInAll = largestSum - (int64_t { 1 } << 51);
    constexpr int64_t mostAddedInVectors = int64_t { 1 } << 52;
    BlockID head = m_order[loop.positionOfHead];
    std::span<const ValueID> parameters = code().at(head).parameters();
    std::span<const BlockID> turn = loop.turn.span();
    int64_t mostTurns = loop.mostTurns;
    auto isMadeInLoop = [&](ValueID value) {
        BlockID home = code().isConstant(value) ? BlockID() : code().blockOf(value);
        return home && (home == head || home == loop.asking || std::ranges::contains(turn, home));
    };
    auto liesInside = [&](ValueID read) {
        return code().at(read).isInBounds() || code().at(read).isPresent() || std::ranges::contains(loop.boundsAskedInFront, read, &BoundAskedInFront::read) || loop.accessesUnderCounterBelowBound.contains(read);
    };
    auto isWholeInvariant = [&](ValueID value) {
        if (code().isConstant(value) || (representationOf(value) != Representation::Double && representationOf(value) != Representation::OptionalDouble))
            return false;
        Type type = typeOf(value);
        if (!(type.primitives & Primitive::NumberWithoutFraction) || !type.hasOnlyPrimitives(Primitive::NumberWithoutFraction | Primitive::UndefinedOrNull))
            return false;
        if (!isMadeInLoop(value))
            return true;
        size_t place = std::ranges::find(parameters, value) - parameters.begin();
        return place < parameters.size() && m_values[value.index()] && back.arguments()[place] && findValuePassedInIteration(loop, back.arguments()[place]) == value;
    };
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        ValueID sum = back.arguments()[index];
        const Operation& addition = code().at(sum);
        if (!m_values[parameter.index()] || representationOf(parameter) != Representation::Double)
            continue;
        bool isStoredToo = loop.loopCarriedValueStores.containsIf([&](const LoopCarriedValueStore& store) { return store.loopCarriedValue == sum; });
        if (code().isConstant(sum) || !additiveOpcodes.contains(addition.opcode) || addition.form() != OperatorForm::Numbers || addition.representation != Representation::Double)
            continue;
        std::optional<Vector<SumOfIntegers::EarlierTerm, 1>> earlierTerms = findEarlierTerms(sum, parameter);
        if (!earlierTerms)
            continue;
        bool isStep = stepOpcodes.contains(addition.opcode);
        bool isSubtracted = addition.opcode == Opcode::Sub || addition.opcode == Opcode::Decrement;
        ValueID sumBefore = earlierTerms->isEmpty() ? parameter : earlierTerms->first().sum;
        ValueID termAsNumber = isStep ? ValueID() : addition.operand(0) == sumBefore ? addition.operand(1) : addition.operand(0);
        ValueID passed = isStep ? ValueID() : findValuePassedInIteration(loop, termAsNumber);
        if (!passed && !isStep)
            continue;
        bool termIsInvariant = passed && isWholeInvariant(passed);
        bool isPassedToJoin = passed != termAsNumber;
        const Operation& conversion = code().at(passed ? passed : sum);
        bool termIsConstant = isStep || (!isPassedToJoin && code().isConstant(passed) && conversion.opcode == Opcode::Integer && isInBounds<int32_t>(conversion.integer()));
        bool termIsOptional = conversion.opcode == Opcode::GetArrayElement && conversion.representation == Representation::OptionalDouble && !loop.carriesJoinedValue && !conversion.readsHoleAsNaN() && !conversion.isPresent() && !conversion.listHasNoHoles();
        bool termIsElementOfList = !isStep && !isPassedToJoin && loop.holesLeaveTurns && !loop.storesIntoLists && conversion.opcode == Opcode::GetArrayElement && (termIsOptional || (conversion.readsHoleAsNaN() && conversion.representation == Representation::Double))
            && conversion.level() == KnowledgeLevel::Proven && liesInside(passed)
            && (conversion.arrayStorage() == ArrayStorage::Int32 || std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == conversion.arrayOperand() && asked.kind == ArrayStorage::Int32; }));
        if (!termIsInvariant && !termIsConstant) {
            if (code().isConstant(passed) || code().useCountOf(passed) != 1 || code().useCountOf(termAsNumber) != 1 || (termIsElementOfList || isPassedToJoin ? !std::ranges::contains(turn, conversion.block()) : conversion.block() != addition.block()))
                continue;
            if (!termIsElementOfList && (conversion.opcode != Opcode::IntegerToNumber || conversion.representation != Representation::Double || (representationOf(conversion.valueOperand()) != Representation::Int32 && representationOf(conversion.valueOperand()) != Representation::Int64) || !typeOf(conversion.valueOperand()).isInteger()))
                continue;
        }
        Range terms = isStep ? Range::one(1) : termIsConstant ? Range::one(conversion.integer()) : termIsElementOfList || termIsInvariant ? Range { -largestInvariantTerm, largestInvariantTerm } : typeOf(conversion.valueOperand()).integers;
        auto rangeOf = [&](ValueID term) {
            return !term ? Range::one(1) : code().isConstant(term) ? Range::one(code().at(term).integer()) : typeOf(term).integers;
        };
        auto isNarrowEnough = [](const Range& range) {
            return range.isSubsetOf(Range { -largestWideTerm, largestWideTerm });
        };
        auto largestOf = [](const Range& range) {
            return std::max<int64_t>({ -range.minimum, range.maximum, 1 });
        };
        if (!isNarrowEnough(terms) || std::ranges::any_of(*earlierTerms, [&](const SumOfIntegers::EarlierTerm& earlier) { return !isNarrowEnough(rangeOf(earlier.term)); }))
            continue;
        int64_t largestTerm = largestOf(terms);
        for (const SumOfIntegers::EarlierTerm& earlier : *earlierTerms)
            largestTerm += largestOf(rangeOf(earlier.term));
        bool isTakenOverBehindStretches = largestTerm > mostAddedInAll / mostTurns;
        if (isTakenOverBehindStretches && (!loop.poll || mostTurns <= turnsBetweenPolls || largestTerm > mostAddedInAll / (turnsBetweenPolls + static_cast<int64_t>(turnsLaidOut))))
            continue;
        unsigned numberOfReads = 0;
        unsigned numberOfSumReads = 0;
        auto count = [&](ValueID read) {
            numberOfReads += read == parameter;
            numberOfSumReads += read == sum;
        };
        for (BlockID block : turn) {
            for (ValueID value : code().valuesOf(block))
                code().at(value).forEachChild(count);
            code().at(block).terminator.forEachChild(count);
            code().forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                if (edge.to != head && !std::ranges::contains(turn, edge.to))
                    return;
                for (ValueID argument : edge.arguments())
                    count(argument);
            });
        }
        bool isReadInIterations = numberOfReads != 1 || code().useCountOf(sum) != 1u + isStoredToo;
        if (isReadInIterations && (isTakenOverBehindStretches || numberOfSumReads != code().useCountOf(sum)))
            continue;
        ValueID term = termIsElementOfList || termIsInvariant || termIsConstant ? passed : conversion.valueOperand();
        int64_t mostAdded = largestTerm * (isTakenOverBehindStretches ? turnsBetweenPolls + static_cast<int64_t>(turnsLaidOut) : mostTurns);
        bool isVectorizable = !isReadInIterations && !termIsOptional && earlierTerms->isEmpty() && !isSubtracted && !termIsInvariant && !termIsConstant && !isPassedToJoin && mostAdded <= mostAddedInVectors && (termIsElementOfList || representationOf(term) == Representation::Int32);
        loop.sumsOfIntegers.append({ index, sum, termAsNumber, term, nullptr, termIsElementOfList, nullptr, nullptr, termIsInvariant, nullptr, isVectorizable, static_cast<double>(largestSum - mostAdded), nullptr, isSubtracted, *earlierTerms, isReadInIterations });
        loop.sumsAreTakenOverBehindStretches |= isTakenOverBehindStretches;
    }
    if (loop.sumsAreTakenOverBehindStretches)
        loop.sumsOfIntegers.removeAllMatching([](const SumOfIntegers& sum) { return sum.isReadInIterations; });
}

void CodeGenerator::findExactCounters(CountedLoop& loop, const Edge& back) const
{
    constexpr int64_t mostSteppedInAll = int64_t { 1 } << 52;
    constexpr unsigned mostJoinsPassed = 4;
    BlockID head = m_order[loop.positionOfHead];
    std::span<const ValueID> parameters = code().at(head).parameters();
    if (!code().opcodesOfValues().contains(Opcode::NumberToInteger) || loop.mostTurns <= 0 || loop.mostTurns > mostSteppedInAll)
        return;
    struct Step {
        ValueID integer;
        size_t indexOfParameter;
    };
    Vector<Step, 4> steps;
    auto isExact = [&](ValueID integer) {
        return representationOf(integer) == Representation::Int64 && typeOf(integer).isInteger() && typeOf(integer).integers.isSubsetOf(Range::exact());
    };
    auto parameterBehind = [&](auto& self, ValueID value, unsigned joinsPassed) -> std::optional<size_t> {
        while (!code().isConstant(value) && opcodeOf(value) == Opcode::Narrowed)
            value = code().at(value).operand(0);
        if (code().isConstant(value) || !isExact(value))
            return std::nullopt;
        if (auto step = std::ranges::find(steps, value, &Step::integer); step != steps.end())
            return step->indexOfParameter;
        if (opcodeOf(value) != Opcode::Parameter)
            return std::nullopt;
        BlockID home = code().blockOf(value);
        std::span<const ValueID> taken = code().at(home).parameters();
        size_t place = std::ranges::find(taken, value) - taken.begin();
        if (home == head)
            return place < taken.size() && m_values[value.index()] ? std::optional(place) : std::nullopt;
        if (!loop.turn.contains(home) || place == taken.size() || joinsPassed == mostJoinsPassed)
            return std::nullopt;
        std::optional<size_t> found;
        bool areOfOneParameter = true;
        code().forEachIncomingEdgeUnsealed(home, [&](const Edge& edge) {
            if (edge.to != home || !code().isReached(edge.from) || !areOfOneParameter)
                return;
            std::optional<size_t> behind = loop.turn.contains(edge.from) && place < edge.arguments().size() && edge.arguments()[place] ? self(self, edge.arguments()[place], joinsPassed + 1) : std::nullopt;
            areOfOneParameter = behind && (!found || *found == *behind);
            found = behind;
        });
        return areOfOneParameter ? found : std::nullopt;
    };
    for (BlockID block : loop.turn) {
        for (ValueID value : code().valuesOf(block)) {
            const Operation& conversion = code().at(value);
            bool isConverted = conversion.opcode == Opcode::NumberToInteger && !code().isConstant(conversion.operand(0));
            if ((!isConverted && !additiveOpcodes.contains(conversion.opcode)) || !isExact(value))
                continue;
            const Operation& step = isConverted ? code().at(conversion.operand(0)) : conversion;
            if (!additiveOpcodes.contains(step.opcode) || step.form() != (isConverted ? OperatorForm::CheckedIntegers : OperatorForm::Integers) || (isConverted && (step.representation != Representation::Double || !loop.turn.contains(step.block()))))
                continue;
            auto isOne = [&](ValueID term) {
                return code().isConstant(term) && opcodeOf(term) == Opcode::Integer && std::abs(code().at(term).integer()) == 1;
            };
            ValueID stepped = stepOpcodes.contains(step.opcode) || isOne(step.operand(1)) ? step.operand(0) : step.opcode == Opcode::Add && isOne(step.operand(0)) ? step.operand(1) : ValueID();
            if (std::optional<size_t> index = stepped ? parameterBehind(parameterBehind, stepped, 0) : std::nullopt)
                steps.append({ value, *index });
        }
    }
    for (size_t index = 0; index < parameters.size() && !steps.isEmpty(); ++index) {
        int64_t stepsOfTurn = std::ranges::count(steps, index, &Step::indexOfParameter);
        if (!stepsOfTurn)
            continue;
        if (stepsOfTurn > mostSteppedInAll / loop.mostTurns || parameterBehind(parameterBehind, back.arguments()[index], 0) != index) {
            steps.removeAllMatching([&](const Step& step) { return step.indexOfParameter == index; });
            continue;
        }
        loop.exactCounters.append({ index, stepsOfTurn * loop.mostTurns });
    }
    for (const Step& step : steps)
        loop.stepsOfExactCounters.append(step.integer);
}

LValue CodeGenerator::numberOfSum(const SumOfIntegers& sum)
{
    return m_out.intToDouble(m_out.add(sum.wordOfBase, sum.added));
}

bool CodeGenerator::addToSumOfIntegers(ValueID id)
{
    auto signExtendedTerm = [&](LValue word) {
        LValue extended = m_out.signExt32To64(word);
        return isARM64() ? m_out.opaque(extended) : extended;
    };
    auto addWord = [&](SumOfIntegers& sum, LValue word, bool isSubtracted) {
        sum.added = isSubtracted ? m_out.sub(sum.added, word) : m_out.add(sum.added, word);
    };
    auto addTerm = [&](SumOfIntegers& sum, ValueID termAdded, bool isSubtracted) {
        if (!termAdded || code().isConstant(termAdded)) {
            int64_t term = termAdded ? code().at(termAdded).integer() : 1;
            if (isSubtracted)
                term = -term;
            LValue before = sum.added;
            if (before->opcode() == Machine::Add && before->child(1)->hasInt64()) {
                term += before->child(1)->asInt64();
                before = before->child(0);
            }
            sum.added = m_out.add(before, m_out.constInt64(term));
            return;
        }
        LValue term = get(termAdded).value;
        addWord(sum, term->type() == Machine::Int64 ? term : typeOf(termAdded).integers.minimum >= 0 ? m_out.zeroExtPtr(term) : signExtendedTerm(term), isSubtracted);
    };
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        for (const SumOfIntegers::EarlierTerm& earlier : sum.earlierTerms) {
            if (id == earlier.sum)
                addTerm(sum, earlier.term, earlier.isSubtracted);
            if (id == earlier.sum || id == earlier.termAsNumber)
                return true;
        }
        if (id == sum.termAsNumber)
            return !sum.termIsElementOfList && !sum.termIsInvariant;
        if (id != sum.sum)
            continue;
        if (sum.termIsInvariant)
            addWord(sum, sum.invariantTerm, sum.isSubtracted);
        else if (sum.termIsElementOfList) {
            auto noted = std::ranges::find(m_wordsOfElementsInTurnLaidOut, sum.term, [](const auto& word) { return word.first; });
            RELEASE_ASSERT(noted != m_wordsOfElementsInTurnLaidOut.end());
            addWord(sum, signExtendedTerm(noted->second), sum.isSubtracted);
        } else
            addTerm(sum, sum.term, sum.isSubtracted);
        if (sum.isReadInIterations)
            m_values[id.index()] = numberOfSum(sum);
        return true;
    }
    return false;
}

bool CodeGenerator::liesInsideByQuestionInFrontOfTurnsLaidOut(ValueID read) const
{
    return std::ranges::contains(m_boundsAskedInFrontOfTurnsLaidOut, read, &BoundAskedInFront::read) || std::ranges::contains(m_accessesUnderCounterBelowBoundOfTurnsLaidOut, read);
}

std::optional<int64_t> CodeGenerator::mostOfKeyCutByWrittenMask(ValueID key) const
{
    Type type = typeOf(key);
    if (!type.isInteger() || type.integers.minimum < 0)
        return std::nullopt;
    while (conversionsOfForm.contains(opcodeOf(key)))
        key = code().at(key).operand(0);
    const Operation& cut = code().at(key);
    if (cut.opcode != Opcode::BitAnd || (!code().isConstant(cut.operand(0)) && !code().isConstant(cut.operand(1))))
        return std::nullopt;
    return type.integers.maximum;
}

std::optional<ArrayStorage> CodeGenerator::kindOfListFoundInFrontOfTurnsLaidOut(ValueID list) const
{
    for (const ListAskedForKind& asked : m_listsAskedForKindInFrontOfTurnsLaidOut) {
        if (asked.list == list)
            return asked.kind;
    }
    return std::nullopt;
}

std::optional<TypedArrayType> CodeGenerator::kindOfViewFoundInFrontOfTurnsLaidOut(ValueID view) const
{
    return std::ranges::contains(m_viewsAskedForKindInFrontOfTurnsLaidOut, view) ? std::optional(m_kindOfViewFoundInFrontOfTurnsLaidOut) : std::nullopt;
}

bool CodeGenerator::accessesViewRepeatedly(ValueID access)
{
    static constexpr OpcodeSet accesses { Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement };
    static constexpr size_t mostViews = 8;
    if (BlockID block = code().blockOf(access); std::exchange(m_blockOfViewAccesses, block) != block) {
        m_viewsAccessed.shrink(0);
        m_viewsAccessedRepeatedly.shrink(0);
        for (ValueID id : code().valuesOf(block)) {
            const Operation& operation = code().at(id);
            if (!accesses.contains(operation.opcode) || operation.isInsideView() || !operation.lengthOperands().empty())
                continue;
            ValueID view = operation.viewOperand();
            if (m_viewsAccessed.contains(view)) {
                if (!m_viewsAccessedRepeatedly.contains(view))
                    m_viewsAccessedRepeatedly.append(view);
            } else if (m_viewsAccessed.size() < mostViews)
                m_viewsAccessed.append(view);
        }
    }
    return m_viewsAccessedRepeatedly.contains(code().at(access).viewOperand());
}

LValue CodeGenerator::numberOfInteger(LValue integer)
{
    for (auto& [known, number] : m_numbersOfIntegers) {
        if (known == integer)
            return number;
    }
    LValue number = m_out.intToDouble(integer);
    if (m_rotatesUnrolledIterationNumbers)
        m_numbersOfIntegers.append({ integer, number });
    return number;
}

LValue CodeGenerator::holdsDoublesAsReadInFrontOfTurnsLaidOut(ValueID list) const
{
    for (auto& [read, holdsDoubles] : m_kindsReadInFrontOfTurnsLaidOut) {
        if (read == list)
            return holdsDoubles;
    }
    return nullptr;
}

LBasicBlock CodeGenerator::blockThatHoleLeavesTurnLaidOutBy()
{
    return m_holesLeaveTurnsLaidOut && m_headOfTurnsLaidOut ? m_blockLeavingTurnLaidOut() : nullptr;
}

LBasicBlock CodeGenerator::optionalNumberHoleExit()
{
    return m_optionalNumbersLeaveAtHoles ? blockThatHoleLeavesTurnLaidOutBy() : nullptr;
}

bool CodeGenerator::asksTextsOfLoopForWidth(const CountedLoop& loop)
{
    if (loop.flatStrings.isEmpty() || !loop.listsAskedForKind.isEmpty() || !loop.listsWhoseKindIsRead.isEmpty() || loop.asksListsReadAndListsStoredInto)
        return false;
    BlockID head = m_order[loop.positionOfHead];
    TerminatorContext context(head, code().at(head).terminator, nullptr);
    SetForScope<const OperationContext*> whileAsking(m_operation, &context);
    return std::ranges::any_of(loop.flatStrings, [&](ValueID string) {
        return emitter<StringEmitter>().widthIsFoundAtRun(string);
    });
}

void CodeGenerator::layOutTurnsForOtherKindOfStorage(CountedLoop& loop, bool ofListsStoredInto)
{
    if (loop.secondTurnsGoByKindsRead) {
        for (const ListAskedForKind& asked : std::exchange(loop.listsAskedForKind, { }))
            loop.listsWhoseKindIsRead.append(asked.list);
    }
    for (ListAskedForKind& asked : loop.listsAskedForKind) {
        if (!asked.hasOneKind && (ofListsStoredInto || !asked.isStoredInto))
            asked.kind = asked.kind != ArrayStorage::Double ? ArrayStorage::Double : asked.isStoredInto ? ArrayStorage::Contiguous : ArrayStorage::Int32;
    }
    loop.sumsOfIntegers.shrink(0);
    loop.sumsAreTakenOverBehindStretches = false;
    findSumsOfIntegers(loop, code().at(code().at(loop.turn.last()).terminator.next()));
    layOutTurns(loop);
}

uint32_t CodeGenerator::findSharedViewKinds(const CountedLoop& loop) const
{
    uint32_t kinds = UINT32_MAX;
    for (ValueID view : loop.viewsAskedForKind)
        kinds &= kindsOfViewsOfNumbersIn(program(), m_inputs.types, typeIDOf(view));
    return kinds;
}

void CodeGenerator::layOutTurnsForEachKindOfView(CountedLoop& loop)
{
    uint32_t kinds = findSharedViewKinds(loop);
    RELEASE_ASSERT(kinds);
    build().statistics().count(StatisticID::UnrolledLoopsForTypedArrays);
    m_layoutsInRow = LayoutsInRow { static_cast<unsigned>(std::popcount(kinds)), nullptr, { } };
    for (uint32_t kind = 32; kind-- && !m_hasFailed;) {
        if (!(kinds >> kind & 1))
            continue;
        loop.kindAskedOfView = static_cast<TypedArrayType>(kind);
        layOutTurns(loop);
    }
}

void CodeGenerator::keepArgumentsForNextTurn(const Edge& edge)
{
    std::span<const ValueID> parameters = code().at(edge.to).parameters();
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size());
    m_argumentsForNextTurn.shrink(0);
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_values[parameter.index()] || (m_lanes && m_lanes->pairWith(parameter)))
            m_argumentsForNextTurn.append(Operand());
        else if (holdsAnyNaN(arguments[index]) && letsAnyNaNInto(parameter))
            m_argumentsForNextTurn.append(getWithAnyNaN(arguments[index]));
        else
            m_argumentsForNextTurn.append(getIn(arguments[index], representationOf(parameter)));
    }
}

bool CodeGenerator::someHoleLeavesTurn(const CountedLoop& loop) const
{
    return loop.holesLeaveTurns && std::ranges::any_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID read) {
            return opcodeOf(read) == Opcode::GetArrayElement && (code().at(read).readsHoleAsNaN() || (!loop.carriesJoinedValue && !code().at(read).isPresent() && !code().at(read).listHasNoHoles() && representationOf(read) == Representation::OptionalDouble)) && (code().at(read).arrayStorage() == ArrayStorage::Int32 || code().at(read).admitsInt32Storage());
        });
    });
}

bool CodeGenerator::someUnusualTurnComesRound(const CountedLoop& loop) const
{
    BlockID head = m_order[loop.positionOfHead];
    auto isLedToByUnusualTurn = [&](BlockID target) {
        bool found = false;
        code().forEachIncomingEdgeUnsealed(target, [&](const Edge& edge) {
            BlockID from = edge.from;
            found |= edge.to == target && from != head && from != loop.asking && code().isReached(from) && code().at(from).frequency != BlockFrequency::Rare && !loop.turn.contains(from) && !m_scopeAtEndOfBlock[from.index()].block;
        });
        return found;
    };
    return isLedToByUnusualTurn(head) || std::ranges::any_of(loop.turn, isLedToByUnusualTurn);
}

LValue CodeGenerator::askCounter(Opcode relation, LValue counter, LValue limit)
{
    switch (relation) {
    case Opcode::Less:
        return m_out.lessThan(counter, limit);
    case Opcode::LessEq:
        return m_out.lessThanOrEqual(counter, limit);
    case Opcode::Greater:
        return m_out.greaterThan(counter, limit);
    default:
        return m_out.greaterThanOrEqual(counter, limit);
    }
}

void CodeGenerator::askLimitOfWrittenLoop(const TerminatorContext& terminator)
{
    LimitOfWrittenLoop loop = *std::exchange(m_limitOfWrittenLoop, std::nullopt);
    BranchTargets targets { edgeTo(terminator, 0), edgeTo(terminator, 1) };
    LValue counter = loop.phisOfWrittenHead[loop.indexOfCounter];
    LBasicBlock here = m_out.m_block;
    LBasicBlock limitIsReached = m_out.newBlock();
    LBasicBlock wayBack = m_out.newBlock();
    m_out.appendTo(limitIsReached);
    m_out.branch(askCounter(loop.relation, counter, loop.bound), normalEdge(wayBack), targets.notTaken);
    m_out.appendTo(wayBack);
    LValue price = m_out.constantOfType(counter->type(), turnsThatPayForComingBack * loop.step);
    LValue paid = m_out.add(loop.paidUpTo, price);
    LValue least = m_out.sub(counter, price);
    paid = m_out.select(loop.step > 0 ? m_out.lessThan(paid, least) : m_out.greaterThan(paid, least), least, paid);
    for (size_t index = 0; index < loop.phisOfEntry.size(); ++index) {
        if (loop.phisOfEntry[index])
            m_out.upsilon(loop.phisOfWrittenHead[index], loop.phisOfEntry[index]);
    }
    m_out.upsilon(paid, loop.paidAtEntry);
    m_out.jump(normalEdge(loop.entry));
    m_out.appendTo(here);
    m_out.branch(askCounter(loop.relation, counter, loop.limit), targets.taken, normalEdge(limitIsReached));
}

const CodeGenerator::LoopExit* CodeGenerator::findLoopExit(BlockID block) const
{
    if (m_loopExits.isEmpty()) [[likely]]
        return nullptr;
    auto exit = m_loopExitsByBlock.find(block.index());
    return exit == m_loopExitsByBlock.end() ? nullptr : &m_loopExits[exit->value];
}

void CodeGenerator::passValuesToLoopExit(BlockID block)
{
    const LoopExit* exit = findLoopExit(block);
    if (!exit) [[likely]]
        return;
    for (size_t index = 0; index < exit->values.size(); ++index)
        m_out.upsilon(m_values[exit->values[index].index()], exit->phis[index]);
}

bool CodeGenerator::isPassedToExit(const Edge& edge, ValueID parameter) const
{
    const LoopExit* exit = findLoopExit(edge.to);
    return std::ranges::contains(edge.arguments(), parameter) || (exit && exit->values.contains(parameter));
}

void CodeGenerator::leaveLoop(const Edge& edge, std::span<const LValue> valuesOfParameters)
{
    std::span<const ValueID> parameters = code().at(m_headOfTurnsLaidOut).parameters();
    Vector<LValue, 8> saved;
    for (size_t index = 0; index < parameters.size(); ++index) {
        LValue& value = m_values[parameters[index].index()];
        saved.append(value);
        if (valuesOfParameters[index])
            value = valuesOfParameters[index];
    }
    const Block& target = code().at(edge.to);
    passArguments(edge, target.parameters(), { });
    passValuesToLoopExit(edge.to);
    m_out.jump(BlockEdge(blockFor(edge.to), edgeClassLeadingTo(target.frequency)));
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = saved[index];
}

Vector<ValueID, 8> CodeGenerator::findValuesReadOutside(std::span<const BlockID> blocksOfLoop, std::span<const ValueID> candidates) const
{
    if (candidates.empty())
        return { };
    Vector<unsigned, 32> reads;
    reads.fill(0, candidates.size());
    auto count = [&](ValueID read) {
        if (read < candidates.front() || candidates.back() < read)
            return;
        auto found = std::ranges::lower_bound(candidates, read);
        if (*found == read)
            ++reads[found - candidates.begin()];
    };
    for (BlockID id : blocksOfLoop) {
        for (ValueID user : code().valuesOf(id))
            forEachOperandNeededBySweep(code().at(user), count);
        forEachValueReadAtEndOfSweptBlock(code().at(id), count);
        code().forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            std::span<const ValueID> parameters = code().at(edge.to).parameters();
            std::span<const ValueID> arguments = edge.arguments();
            for (size_t index = 0; index < std::min(parameters.size(), arguments.size()); ++index) {
                if (arguments[index] && code().isNeeded(parameters[index]))
                    count(arguments[index]);
            }
        });
    }
    Vector<ValueID, 8> found;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (reads[index] != code().useCountOf(candidates[index]))
            found.append(candidates[index]);
    }
    return found;
}

void CodeGenerator::findLoopParameterReaders()
{
    if (m_hasReadersOfLoopParameters)
        return;
    m_hasReadersOfLoopParameters = true;
    for (BlockID id : m_order) {
        auto note = [&](ValueID read) {
            const Operation& operation = code().at(read);
            if (operation.opcode == Opcode::Parameter && operation.block() != id && code().at(operation.block()).isLoopHead)
                m_readersOfLoopParameters.append({ read, id });
        };
        for (ValueID user : code().valuesOf(id))
            forEachOperandNeededBySweep(code().at(user), note);
        forEachValueReadAtEndOfSweptBlock(code().at(id), note);
        code().forEachEdgeOf(id, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments()) {
                if (argument)
                    note(argument);
            }
        });
    }
    std::ranges::stable_sort(m_readersOfLoopParameters, { }, &LoopParameterReader::parameter);
}

void CodeGenerator::findLoopExits(const CountedLoop& loop)
{
    m_exitsOfUnrolledLoop.shrink(0);
    if (m_keepsValuesInHeapFrame)
        return;
    BlockID head = m_order[loop.positionOfHead];
    Vector<BlockID, 16> blocks = collectLoopBlocks(loop);
    if (m_dominatorsByPlace.empty())
        findDominators();
    auto isInLoop = [&](BlockID block) {
        return std::ranges::binary_search(blocks, block);
    };
    auto isIteration = [&](BlockID block) {
        return block == head || block == loop.asking || loop.turn.contains(block);
    };
    auto canPassPhis = [&](BlockID id) {
        const Terminator& terminator = code().at(id).terminator;
        if (terminator.kind == TerminatorKind::Jump)
            return true;
        static constexpr OpcodeSet testsThatPassValues { Opcode::TypeTest, Opcode::IsElementPresent };
        return terminator.kind == TerminatorKind::Branch && !testsThatPassValues.contains(opcodeOf(terminator.conditionOperand()));
    };

    Vector<BlockID, 8> exits;
    Vector<BlockID, 4> exitsOfIterations;
    for (BlockID id : blocks) {
        if (code().handlerOf(id) || (id != head && (code().at(id).isEntry || !dominates(head, id))))
            return;
        const Terminator& terminator = code().at(id).terminator;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            BlockID target = code().at(terminator.edge(edge)).to;
            if (isInLoop(target))
                continue;
            if (!exits.contains(target))
                exits.append(target);
            if (isIteration(id) && !exitsOfIterations.contains(target))
                exitsOfIterations.append(target);
        }
    }

    Vector<BlockID, 8> dominatedExits;
    Vector<BlockID, 4> directDominatedExits;
    bool needsNoOtherPhis = true;
    for (BlockID exit : exits) {
        const Block& block = code().at(exit);
        bool isDominated = dominates(head, exit);
        bool isDirect = exitsOfIterations.contains(exit) && m_blocks[exit.index()] && !block.isEntry && !block.isHandlerBlock() && block.frequency != BlockFrequency::Rare && !(isDominated && (block.isLoopHead || block.terminator.hasCases()));
        bool hasOtherPredecessor = false;
        code().forEachIncomingEdgeUnsealed(exit, [&](const Edge& edge) {
            if (edge.to != exit || !code().isReached(edge.from))
                return;
            if (isDominated || isInLoop(edge.from))
                isDirect &= canPassPhis(edge.from);
            hasOtherPredecessor |= !isInLoop(edge.from) && edge.from != exit && !dominates(exit, edge.from);
        });
        if (isDominated)
            dominatedExits.append(exit);
        if (isDirect && isDominated)
            directDominatedExits.append(exit);
        else if (isDirect)
            m_exitsOfUnrolledLoop.append(exit);
        else if (isDominated)
            needsNoOtherPhis &= !hasOtherPredecessor;
    }
    if (!directDominatedExits.isEmpty() && needsNoOtherPhis && findValuesPassedToExits(loop, blocks.span(), dominatedExits.span(), directDominatedExits.span()))
        m_exitsOfUnrolledLoop.appendVector(directDominatedExits);
    if (!m_exitsOfUnrolledLoop.isEmpty())
        build().statistics().count(StatisticID::UnrolledLoopsWithDirectExits);
}

bool CodeGenerator::findValuesPassedToExits(const CountedLoop& loop, std::span<const BlockID> blocksOfLoop, std::span<const BlockID> exits, std::span<const BlockID> directExits)
{
    BlockID head = m_order[loop.positionOfHead];
    Vector<ValueID, 32> candidates;
    for (BlockID id : blocksOfLoop) {
        if (id != head && id != loop.asking && !loop.turn.contains(id))
            continue;
        candidates.append(code().at(id).parameters());
        candidates.append(code().valuesOf(id));
    }
    std::ranges::sort(candidates);
    Vector<ValueID, 4> values;
    for (ValueID value : findValuesReadOutside(blocksOfLoop, candidates.span())) {
        if (opcodeOf(value) != Opcode::Parameter || code().blockOf(value) != head)
            return false;
        if (m_values[value.index()])
            values.append(value);
    }
    if (values.isEmpty())
        return true;

    auto dominatingExit = [&](BlockID block) {
        for (BlockID exit : exits) {
            if (blockDominates(exit, block))
                return exit;
        }
        return BlockID();
    };
    struct EdgeBetweenExits {
        size_t from;
        size_t to;
    };
    Vector<EdgeBetweenExits, 4> edgesBetweenExits;
    Vector<LoopExit, 4> found;
    for (BlockID exit : directExits)
        found.append({ exit, std::numeric_limits<uint32_t>::max(), { }, { }, { } });
    auto placeOf = [&](BlockID exit) {
        return static_cast<size_t>(std::ranges::find(directExits, exit) - directExits.begin());
    };
    for (size_t to = 0; to < directExits.size(); ++to) {
        bool isAfterOneExit = true;
        code().forEachIncomingEdgeUnsealed(directExits[to], [&](const Edge& edge) {
            if (edge.to != directExits[to] || !code().isReached(edge.from) || std::ranges::binary_search(blocksOfLoop, edge.from))
                return;
            BlockID from = dominatingExit(edge.from);
            isAfterOneExit &= !!from;
            if (from && placeOf(from) < directExits.size())
                edgesBetweenExits.append({ placeOf(from), to });
        });
        if (!isAfterOneExit)
            return false;
    }
    findLoopParameterReaders();
    for (ValueID value : values) {
        auto readers = std::ranges::equal_range(m_readersOfLoopParameters, value, { }, &LoopParameterReader::parameter);
        for (const LoopParameterReader& reader : readers) {
            if (std::ranges::binary_search(blocksOfLoop, reader.block))
                continue;
            BlockID exit = dominatingExit(reader.block);
            if (!exit)
                return false;
            if (size_t place = placeOf(exit); place < found.size() && !found[place].values.contains(value))
                found[place].values.append(value);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (EdgeBetweenExits edge : edgesBetweenExits) {
            for (ValueID value : found[edge.to].values) {
                if (found[edge.from].values.contains(value))
                    continue;
                found[edge.from].values.append(value);
                changed = true;
            }
        }
    }
    for (LoopExit& exit : found) {
        if (exit.values.isEmpty())
            continue;
        for (ValueID value : exit.values)
            exit.phis.append(m_out.phiIn(m_blocks[exit.block.index()], m_values[value.index()]->type()));
        expectOtherJumps(exit.block);
        m_loopExitsByBlock.set(exit.block.index(), static_cast<uint32_t>(m_loopExits.size()));
        m_loopExits.append(WTF::move(exit));
    }
    return true;
}

void CodeGenerator::noteScopeOfLoopExit(BlockID id)
{
    if (m_scopesByPlace.isEmpty())
        m_scopesByPlace.fill(noLoopExit, m_dominatorsByPlace.size());
    uint32_t place = m_places[id.index()];
    auto exit = m_loopExitsByBlock.find(id.index());
    uint32_t enclosing = m_scopesByPlace[m_dominatorsByPlace[place]];
    uint32_t scope = exit == m_loopExitsByBlock.end() ? enclosing : exit->value;
    if (scope != enclosing)
        m_loopExits[scope].enclosing = enclosing;
    m_scopesByPlace[place] = scope;
}

void CodeGenerator::enterScopeOfLoopExit(BlockID id)
{
    uint32_t scope = m_scopesByPlace[m_places[id.index()]];
    if (scope == m_currentLoopExit)
        return;
    while (m_currentLoopExit != noLoopExit && !blockDominates(m_loopExits[m_currentLoopExit].block, id)) {
        LoopExit& left = m_loopExits[m_currentLoopExit];
        for (size_t index = 0; index < left.values.size(); ++index)
            m_values[left.values[index].index()] = left.outerValues[index];
        m_currentLoopExit = left.enclosing;
    }
    Vector<uint32_t, 8> entered;
    for (uint32_t next = scope; next != m_currentLoopExit; next = m_loopExits[next].enclosing)
        entered.append(next);
    while (!entered.isEmpty()) {
        m_currentLoopExit = entered.takeLast();
        LoopExit& current = m_loopExits[m_currentLoopExit];
        current.outerValues.shrink(0);
        for (size_t index = 0; index < current.values.size(); ++index) {
            LValue& value = m_values[current.values[index].index()];
            current.outerValues.append(value);
            value = current.phis[index];
        }
    }
}

uint32_t CodeGenerator::findLoopExitThatRenames(std::span<const ValueID> values) const
{
    if (m_loopExits.isEmpty()) [[likely]]
        return noLoopExit;
    for (uint32_t exit = m_currentLoopExit; exit != noLoopExit; exit = m_loopExits[exit].enclosing) {
        bool renames = std::ranges::any_of(values, [&](ValueID value) {
            return value && m_loopExits[exit].values.contains(isLeftToHandlers(value) ? code().at(value).valueOperand() : value);
        });
        if (renames)
            return exit;
    }
    return noLoopExit;
}

static LValue constantForwardedTo(LValue phi)
{
    Machine::Block* entering = phi->opcode() == Machine::Phi ? phi->owner()->singlePredecessor() : nullptr;
    if (!entering)
        return phi;
    for (LValue upsilon : entering->upsilons()) {
        if (upsilon->phi() == phi && upsilon->child(0)->isConstant())
            return upsilon->child(0);
    }
    return phi;
}

void CodeGenerator::startFromConstant(const CountedLoop& loop)
{
    if (!loop.bound || code().isConstant(loop.bound))
        return;
    LValue& counter = m_values[loop.counter.index()];
    LValue start = constantForwardedTo(counter);
    if (start == counter)
        return;
    build().statistics().count(StatisticID::UnrolledLoopsFromConstants);
    counter = start;
}

Range CodeGenerator::boundsOnEntry(const CountedLoop& loop, Range bounds, Range counters)
{
    if (!loop.conditionIsTestedOnEntry)
        return bounds;
    bool differs = loop.relation == Opcode::Less || loop.relation == Opcode::Greater;
    if (loop.step > 0)
        bounds.minimum = std::max(bounds.minimum, counters.minimum + differs);
    else
        bounds.maximum = std::min(bounds.maximum, counters.maximum - differs);
    return bounds;
}

void CodeGenerator::emitFirstConditions(CountedLoop& loop)
{
    BlockID head = m_order[loop.positionOfHead];
    const Terminator& end = code().at(loop.asking).terminator;
    if (!loop.bound || loop.testsAtEnd || !m_exitsOfUnrolledLoop.contains(code().at(end.notTaken()).to))
        return;
    auto hasDirectExit = [&](BlockID id) {
        const Terminator& terminator = code().at(id).terminator;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            if (m_exitsOfUnrolledLoop.contains(code().at(terminator.edge(edge)).to))
                return true;
        }
        return false;
    };
    size_t numberOfBlocks = 0;
    for (size_t index = 0; index < loop.turn.size(); ++index) {
        bool isCheap = std::ranges::all_of(code().valuesOf(loop.turn[index]), [&](ValueID value) {
            const Operation& operation = code().at(value);
            static constexpr OpcodeSet comparisons { Opcode::StrictEq, Opcode::Eq, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
            if (value == loop.poll)
                return true;
            if (comparisons.contains(operation.opcode) && operation.form() != OperatorForm::Integers && operation.form() != OperatorForm::Numbers)
                return false;
            return !operation.traits().mayWriteObservableState();
        });
        if (!isCheap)
            break;
        if (hasDirectExit(loop.turn[index]))
            numberOfBlocks = index + 1;
    }
    if (!numberOfBlocks && std::ranges::any_of(loop.turn, hasDirectExit))
        return;
    std::span<const ValueID> parameters = code().at(head).parameters();
    bool peelsIteration = numberOfBlocks && loop.loopCarriedValueStores.isEmpty() && !m_carriesNarrowIntegers && std::ranges::all_of(parameters, [&](ValueID parameter) {
        LValue phi = m_values[parameter.index()];
        return !phi || phi->opcode() == Machine::Phi;
    });
    std::span<const BlockID> blocks = peelsIteration ? loop.turn.span() : loop.turn.span().first(numberOfBlocks);

    build().statistics().count(peelsIteration ? StatisticID::UnrolledLoopsPeeled : StatisticID::UnrolledLoopConditionsCopied);
    LBasicBlock front = m_out.newBlock();
    Vector<LValue, 8> phisOfHead;
    Vector<LValue, 8> phisOfFront;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfHead.append(phi);
        if (peelsIteration)
            phisOfFront.append(phi ? m_out.phiIn(front, phi->type()) : nullptr);
    }
    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    {
        TerminatorContext context(loop.asking, end, nullptr);
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LBasicBlock begins = blocks.empty() ? front : m_out.newBlock();
        BlockEdge exit = edgeTo(context, 1);
        LValue counter = get(loop.counter).value;
        LValue bound = get(loop.bound).value;
        m_out.branch(askCounter(loop.relation, bound->isConstant() ? counter : constantForwardedTo(counter), bound), normalEdge(begins), exit);
        if (!blocks.empty())
            m_out.appendTo(begins);
        loop.conditionIsTestedOnEntry = true;
    }
    if (!blocks.empty()) {
        Vector<LBasicBlock, mostBlocksInTurnLaidOut> blocksOfWrittenTurn;
        Vector<std::pair<ValueID, LValue>, 8> phisOfWrittenTurn;
        for (BlockID block : blocks) {
            blocksOfWrittenTurn.append(m_blocks[block.index()]);
            for (ValueID parameter : code().at(block).parameters())
                phisOfWrittenTurn.append({ parameter, m_values[parameter.index()] });
        }
        m_headOfTurnsLaidOut = head;
        m_iterationMayThrowInPlace = true;
        m_turnLaidOut = blocks;
        LBasicBlock restart = nullptr;
        m_blockLeavingTurnLaidOut = [&] {
            if (!peelsIteration)
                return front;
            if (!restart) {
                LBasicBlock here = m_out.m_block;
                restart = m_out.newBlock();
                m_out.appendTo(restart);
                for (size_t index = 0; index < parameters.size(); ++index) {
                    if (phisOfFront[index])
                        m_out.upsilon(phisOfHead[index], phisOfFront[index]);
                }
                m_out.jump(normalEdge(front));
                m_out.appendTo(here);
            }
            return restart;
        };
        if (LValue& counter = m_values[loop.counter.index()]; peelsIteration && !code().isConstant(loop.bound))
            counter = constantForwardedTo(counter);
        m_blockLeavingLoop = [&](const Edge& edge) {
            LBasicBlock here = m_out.m_block;
            LBasicBlock blockOnEdge = m_out.newBlock();
            m_out.appendTo(blockOnEdge);
            leaveLoop(edge, phisOfHead.span());
            m_out.appendTo(here);
            return blockOnEdge;
        };
        for (unsigned index = 0; index < blocks.size(); ++index) {
            LBasicBlock& block = m_blocks[blocks[index].index()];
            if (!index) {
                block = m_out.m_block;
                m_nextBlockGoesOnHere = true;
            } else
                block = goesOnBehindJump(loop.positionsOfTurn[index]) ? nullptr : m_out.newBlock();
        }
        for (auto [parameter, phi] : phisOfWrittenTurn) {
            if (phi)
                m_values[parameter.index()] = m_out.phiIn(blockFor(code().blockOf(parameter)), phi->type());
        }
        if (loop.poll)
            m_valuesLeftOutOfTurnsLaidOut.append(loop.poll);
        for (BlockID block : blocks) {
            generateBlock(block);
            if (m_hasFailed)
                return;
            m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
        }
        m_valuesLeftOutOfTurnsLaidOut.shrink(0);
        if (peelsIteration) {
            RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
            m_nextBlockGoesOnHere = false;
            for (size_t index = 0; index < parameters.size(); ++index) {
                if (phisOfFront[index])
                    m_out.upsilon(m_argumentsForNextTurn[index].value, phisOfFront[index]);
            }
            m_out.jump(normalEdge(front));
            for (size_t index = 0; index < parameters.size(); ++index)
                m_values[parameters[index].index()] = phisOfFront[index];
        }
        RELEASE_ASSERT(!m_nextBlockGoesOnHere && !currentBlockIsOpen());
        m_headOfTurnsLaidOut = BlockID();
        m_iterationMayThrowInPlace = false;
        m_turnLaidOut = { };
        m_blockLeavingTurnLaidOut = nullptr;
        m_blockLeavingLoop = nullptr;
        emitter<StringEmitter>().forgetWidthsFound();
        for (unsigned index = 0; index < blocks.size(); ++index) {
            for (ValueID value : code().valuesOf(blocks[index]))
                m_values[value.index()] = nullptr;
            m_blocks[blocks[index].index()] = blocksOfWrittenTurn[index];
            m_scopeAtEndOfBlock[blocks[index].index()] = ScopeFork();
        }
        for (auto [parameter, phi] : phisOfWrittenTurn)
            m_values[parameter.index()] = phi;
    }
    m_out.setOrigin(Machine::Origin());
    m_blocks[head.index()] = front;
}

void CodeGenerator::layOutTurns(const CountedLoop& loop)
{
    build().statistics().count(StatisticID::UnrolledLoopsEmitted);
    for (size_t stores = loop.loopCarriedValueStores.size(); stores--;)
        build().statistics().count(StatisticID::UnrolledLoopStoresSunk);
    for (size_t sums = loop.sumsOfIntegers.size(); sums--;)
        build().statistics().count(StatisticID::UnrolledLoopIntegerSums);
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    std::span<const BlockID> turn = loop.turn.span();
    unsigned turnsInRow = loop.rotatesNumbers ? rotatingIterationsInRow : !loop.viewsAskedForKind.isEmpty() || loop.asksListsReadAndListsStoredInto ? turnsLaidOutForEachKindOfView : loop.numberOfOperations > mostOperationsInTurnLaidOut || loop.holdsProductSum ? wideIterationsUnrolled : turnsLaidOut;
    if (!loop.viewsAskedForKind.isEmpty())
        build().statistics().count(StatisticID::UnrolledIterationsForTypedArrays, turnsInRow);

    auto forEachParameterInTurn = [&](auto&& function) {
        for (BlockID block : turn) {
            for (ValueID parameter : code().at(block).parameters())
                function(block, parameter);
        }
    };
    auto forgetTurn = [&] {
        for (BlockID block : turn) {
            for (ValueID parameter : code().at(block).parameters())
                m_values[parameter.index()] = nullptr;
            for (ValueID value : code().valuesOf(block))
                m_values[value.index()] = nullptr;
        }
    };

    Vector<LBasicBlock, mostBlocksInTurnLaidOut> blocksOfWrittenTurn;
    Vector<LValue, 8> phisOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    forEachParameterInTurn([&](BlockID, ValueID parameter) {
        phisOfWrittenTurn.append(m_values[parameter.index()]);
    });

    bool comesBack = loop.mayComeBack;
    RELEASE_ASSERT(!comesBack || (loop.loopCarriedValueStores.isEmpty() && !m_keepsValuesInHeapFrame));
    std::optional<RingOfLoopLaidOut> ring = std::exchange(m_ringOfLoopLaidOut, std::nullopt);
    RELEASE_ASSERT(!ring || (comesBack && ring->phisOfEntry.size() == parameters.size()));
    if (comesBack)
        build().statistics().count(ring ? StatisticID::UnrolledLoopsWithSharedReentry : StatisticID::UnrolledLoopsWithReentry);
    LBasicBlock entry = ring ? ring->entry : comesBack ? m_out.newBlock() : blockFor(head);
    LBasicBlock turns = m_out.newBlock();
    bool emitsSingleIterations = loop.bound && loop.loopCarriedValueStores.isEmpty() && loop.viewsAskedForKind.isEmpty() && (!loop.listsAskedForKind.isEmpty() || !loop.listsWhoseKindIsRead.isEmpty());
    LBasicBlock single = emitsSingleIterations ? m_out.newBlock() : nullptr;
    RELEASE_ASSERT(!m_layoutsInRow || !comesBack);
    bool isFirstInRow = m_layoutsInRow && !m_layoutsInRow->headBehindAll;
    bool isLastInRow = m_layoutsInRow && !--m_layoutsInRow->left;
    if (isFirstInRow)
        m_layoutsInRow->headBehindAll = m_out.newBlock();
    LBasicBlock writtenHead = ring ? ring->writtenHead : isLastInRow ? m_layoutsInRow->headBehindAll : m_out.newBlock();
    LBasicBlock leaving = comesBack ? m_out.newBlock() : nullptr;
    LBasicBlock intoWritten = ring ? ring->intoWritten : comesBack ? m_out.newBlock() : writtenHead;
    LBasicBlock otherKind = comesBack && (loop.layoutWithoutQuestionFollows || ((!ring || ring->nextMayOpenAnother) && (loop.asksListsForAnotherKind() || loop.turnsForWideTextsFollow))) ? m_out.newBlock() : nullptr;
    using Values = Vector<LValue, 8>;
    Values phisOfEntry;
    Values phisOfTurns;
    Values phisOfWrittenHead;
    Values phisOfHead;
    Values phisOfLeaving;
    if (ring) {
        phisOfHead = ring->phisOfHead;
        phisOfEntry = ring->phisOfEntry;
        phisOfWrittenHead = ring->phisOfWrittenHead;
        phisOfTurns.fill(nullptr, parameters.size());
    } else {
        for (ValueID parameter : parameters) {
            LValue phi = m_values[parameter.index()];
            phisOfHead.append(phi);
            phisOfEntry.append(phi && comesBack ? m_out.phiIn(entry, phi->type()) : phi);
            phisOfTurns.append(nullptr);
            if (isFirstInRow)
                m_layoutsInRow->phisBehindAll.append(phi ? m_out.phiIn(m_layoutsInRow->headBehindAll, phi->type()) : nullptr);
            phisOfWrittenHead.append(isLastInRow ? m_layoutsInRow->phisBehindAll[phisOfWrittenHead.size()] : phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
        }
    }
    LBasicBlock headOfRest = m_layoutsInRow ? m_layoutsInRow->headBehindAll : writtenHead;
    Values phisOfRest = m_layoutsInRow ? m_layoutsInRow->phisBehindAll : phisOfWrittenHead;
    if (headOfRest != writtenHead)
        build().statistics().count(StatisticID::UnrolledLoopRemaindersShared);
    bool noTurnComesToNextFront = m_layoutsInRow && !loop.viewsAskedForKind.isEmpty();
    bool countIsAsked = noTurnComesToNextFront && !isFirstInRow;
    bool tripCountIsChecked = countIsAsked || (ring && ring->tripCountIsChecked);
    if (countIsAsked)
        build().statistics().count(StatisticID::UnrolledLoopTripChecksReused);
    if (isLastInRow)
        m_layoutsInRow = std::nullopt;
    m_sumsOfIntegers = loop.sumsOfIntegers;
    bool hasInvariantTerm = std::ranges::any_of(m_sumsOfIntegers, &SumOfIntegers::termIsInvariant);
    for (SumOfIntegers& sum : m_sumsOfIntegers) {
        sum.added = m_out.phiIn(turns, Machine::Int64);
        phisOfTurns[sum.indexOfParameter] = sum.added;
        sum.base = loop.sumsAreTakenOverBehindStretches ? m_out.phiIn(turns, Machine::Double) : phisOfEntry[sum.indexOfParameter];
    }
    auto comesRoundWide = [&](size_t index) {
        const Type& type = typeOf(parameters[index]);
        ValueID next = code().at(code().at(loop.turn.last()).terminator.next()).arguments()[index];
        if (parameters[index] == loop.counter || representationOf(parameters[index]) != Representation::Int32 || !type.isInteger() || type.integers.isEmpty() || type.integers.minimum < 0 || code().isConstant(next) || opcodeOf(next) != Opcode::DivideIntegers)
            return false;
        const Type& carried = typeOf(next);
        return representationOf(next) == Representation::Int32 && carried.isInteger() && !carried.integers.isEmpty() && carried.integers.minimum >= 0 && representationOf(code().at(next).operand(0)) == Representation::Int64;
    };
    Vector<size_t, 2> indexesOfWide;
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (!phisOfEntry[index] || phisOfTurns[index])
            continue;
        bool isWide = isX86_64() && single && phisOfEntry[index]->type() == Machine::Int32 && code().at(loop.turn.last()).terminator.kind == TerminatorKind::Jump && comesRoundWide(index);
        if (isWide)
            indexesOfWide.append(index);
        phisOfTurns[index] = m_out.phiIn(turns, isWide ? Machine::Int64 : phisOfEntry[index]->type());
    }
    auto asCarriedByLoop = [&](size_t index, LValue value) -> LValue {
        if (!indexesOfWide.contains(index) || value->type() != Machine::Int32)
            return value;
        return value->opcode() == Machine::Trunc && value->child(0)->type() == Machine::Int64 ? value->child(0) : m_out.zeroExt(value, Machine::Int64);
    };
    if (loop.rotatesNumbers)
        build().statistics().count(StatisticID::UnrolledLoopsThatRotateNumbers);
    Vector<std::pair<size_t, LValue>, 4> numbersOfTurns;
    if (loop.rotatesNumbers && isARM64()) {
        const Edge& back = code().at(code().at(loop.turn.last()).terminator.next());
        auto isConvertedInIteration = [&](ValueID integer) {
            return std::ranges::any_of(loop.turn, [&](BlockID block) {
                return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
                    return opcodeOf(value) == Opcode::IntegerToNumber && representationOf(value) == Representation::Double && code().at(value).valueOperand() == integer;
                });
            });
        };
        for (size_t index = 0; index < parameters.size(); ++index) {
            ValueID next = back.arguments()[index];
            if (!phisOfTurns[index] || parameters[index] == loop.counter || std::ranges::contains(m_sumsOfIntegers, index, &SumOfIntegers::indexOfParameter) || representationOf(parameters[index]) != Representation::Int32 || code().useCountOf(parameters[index]) != 1 || !isConvertedInIteration(parameters[index]))
                continue;
            if (!code().isConstant(next) && representationOf(next) == Representation::Int32 && isConvertedInIteration(next))
                numbersOfTurns.append({ index, m_out.phiIn(turns, Machine::Double) });
        }
    }
    Values phisOfSingle;
    for (size_t index = 0; index < parameters.size() && single; ++index)
        phisOfSingle.append(phisOfEntry[index] ? m_out.phiIn(single, phisOfEntry[index]->type()) : nullptr);
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    Values phisIntoWritten = phisOfWrittenHead;
    LType typeOfCounter = phisOfEntry[indexOfCounter]->type();
    LValue bound = nullptr;
    LValue limit = nullptr;
    LValue limitOfWay = nullptr;
    LValue paidUpTo = nullptr;
    LValue paidIntoWritten = nullptr;
    LValue paidOfWay = nullptr;
    if (comesBack) {
        for (size_t index = 0; index < parameters.size(); ++index) {
            LValue left = single ? phisOfSingle[index] : phisOfTurns[index];
            phisOfLeaving.append(left ? m_out.phiIn(leaving, left->type()) : nullptr);
            for (SumOfIntegers& sum : m_sumsOfIntegers) {
                if (sum.indexOfParameter == index && !single && hasInvariantTerm)
                    sum.baseAtExit = m_out.phiIn(leaving, Machine::Double);
            }
            phisIntoWritten[index] = ring ? ring->phisIntoWritten[index] : phisOfWrittenHead[index] ? m_out.phiIn(intoWritten, phisOfWrittenHead[index]->type()) : nullptr;
        }
        limit = ring ? ring->limit : m_out.phiIn(intoWritten, typeOfCounter);
        paidIntoWritten = ring ? ring->paidIntoWritten : m_out.phiIn(intoWritten, typeOfCounter);
        paidUpTo = ring ? ring->paidUpTo : m_out.phiIn(entry, typeOfCounter);
        paidOfWay = phisOfHead[indexOfCounter];
    }
    auto passSumToExit = [&](const SumOfIntegers& sum, LValue added) {
        if (sum.baseAtExit)
            m_out.upsilon(sum.base, sum.baseAtExit);
        return added;
    };
    auto handOn = [&](LValue value, LValue phi) {
        m_out.upsilon(value, phi);
    };
    auto forwardIterationNumbers = [&](const Values& values) {
        for (auto& [index, phi] : numbersOfTurns)
            m_out.upsilon(numberOfInteger(values[index]), phi);
    };
    bool leavesTurnsLaidOut = false;
    Vector<LValue, 2> wordsWidenedBeforeIterations;
    auto blockHandingOver = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock blockOnEdge = m_out.newBlock();
        m_out.appendTo(blockOnEdge);
        Values values;
        for (size_t index = 0; index < phis.size(); ++index)
            values.append(phis[index] ? valueAt(index) : nullptr);
        if (leavesTurnsLaidOut && target != turns && target != single && !loop.loopCarriedValueStores.isEmpty()) {
            LBasicBlock behindStores = nullptr;
            if (values[indexOfCounter] == phisOfTurns[indexOfCounter]) {
                LBasicBlock someTurnHasRun = m_out.newBlock();
                behindStores = m_out.newBlock();
                m_out.branch(m_out.equal(values[indexOfCounter], phisOfEntry[indexOfCounter]), normalEdge(behindStores), normalEdge(someTurnHasRun));
                m_out.appendTo(someTurnHasRun);
            }
            for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores)
                storeLoopCarriedValue(loop, store, values[store.indexOfParameter]);
            if (behindStores) {
                m_out.jump(normalEdge(behindStores));
                m_out.appendTo(behindStores);
            }
        }
        if (target == turns)
            forwardIterationNumbers(values);
        for (size_t index = 0; index < phis.size(); ++index) {
            if (!phis[index])
                continue;
            bool isCounterHandedOn = index == indexOfCounter && (values[index] == phisOfTurns[index] || (single && values[index] == phisOfSingle[index]) || (comesBack && values[index] == phisOfLeaving[index]));
            if (values[index] == phisOfHead[index] || values[index] == phisOfEntry[index] || isCounterHandedOn)
                handOn(values[index], phis[index]);
            else
                m_out.upsilon(values[index], phis[index]);
        }
        RELEASE_ASSERT(!comesBack || target != writtenHead);
        if (comesBack && target == intoWritten) {
            m_out.upsilon(limitOfWay ? limitOfWay : bound, limit);
            m_out.upsilon(paidOfWay, paidIntoWritten);
        }
        m_out.jump(normalEdge(target));
        m_out.appendTo(here);
        return blockOnEdge;
    };
    auto sumAt = [&](size_t indexOfParameter) -> const SumOfIntegers* {
        for (const SumOfIntegers& sum : m_sumsOfIntegers) {
            if (sum.indexOfParameter == indexOfParameter)
                return &sum;
        }
        return nullptr;
    };
    bool pollsBetweenStretches = loop.poll && loop.mostTurns > turnsBetweenPolls;
    bool countsUp = loop.step > 0;
    auto holdsOf = [&](LValue counter, LValue limit) {
        return askCounter(loop.relation, counter, limit);
    };
    LValue endOfStretch = pollsBetweenStretches ? m_out.phiIn(turns, phisOfEntry[indexOfCounter]->type()) : nullptr;
    auto endOfStretchFrom = [&](LValue counter, LValue lastStart) {
        LValue behindStretch = m_out.add(counter, m_out.constantOfType(counter->type(), turnsBetweenPolls * loop.step));
        return m_out.select(countsUp ? m_out.lessThan(behindStretch, lastStart) : m_out.greaterThan(behindStretch, lastStart), behindStretch, lastStart);
    };

    m_out.appendTo(ring ? ring->otherKind : comesBack ? blockFor(head) : entry);
    m_out.setOrigin(Machine::Origin());
    bool comparesLastStartLate = single;
    LValue lastStart = nullptr;
    {
        TerminatorContext context(head, blockOfHead.terminator, nullptr);
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LValue counter = phisOfEntry[indexOfCounter];
        bound = ring ? ring->bound : loop.bound ? get(loop.bound).value : m_out.constantOfType(counter->type(), 0);
        RELEASE_ASSERT(bound && counter && bound->type() == counter->type());
        int64_t reach = (turnsInRow - 1) * loop.step + loop.addedBeforeTest;
        auto findLastStart = [&] {
            return m_out.sub(bound, m_out.constantOfType(bound->type(), reach));
        };
        if (!comparesLastStartLate)
            lastStart = ring && ring->lastStart ? ring->lastStart : findLastStart();
        auto handToWritten = [&](const Values& phis) {
            return blockHandingOver(phisIntoWritten, intoWritten, [&](size_t index) {
                return phis[index];
            });
        };
        auto handToTurns = [&] {
            return blockHandingOver(phisOfTurns, turns, [&](size_t index) {
                if (index == indexOfCounter && endOfStretch)
                    m_out.upsilon(endOfStretchFrom(counter, lastStart), endOfStretch);
                if (const SumOfIntegers* sum = sumAt(index); sum && loop.sumsAreTakenOverBehindStretches)
                    m_out.upsilon(phisOfEntry[index], sum->base);
                return sumAt(index) ? m_out.constInt64(0) : asCarriedByLoop(index, phisOfEntry[index]);
            });
        };
        LBasicBlock written = comesBack ? nullptr : handToWritten(phisOfHead);
        LBasicBlock laidOut = comesBack || comparesLastStartLate ? nullptr : handToTurns();
        bool shouldSkipEntryChecks = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrollEntryChecks);
        auto answerTo = [&](LValue answer) {
            return shouldSkipEntryChecks ? m_out.opaque(m_out.booleanTrue) : answer;
        };
        auto tooFewForAnyLayout = [&] {
            return headOfRest == writtenHead ? written : blockHandingOver(phisOfRest, headOfRest, [&](size_t index) {
                return phisOfHead[index];
            });
        };
        Range bounds = boundsOnEntry(loop, loop.bound ? typeOf(loop.bound).integers : Range { 0, 0 }, typeOf(loop.counter).integers);
        if (bound->type() == Machine::Int32 && !ring && !countIsAsked) {
            int64_t nearest = (countsUp ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max()) + reach;
            if (countsUp ? bounds.minimum < nearest : bounds.maximum > nearest) {
                if (comesBack)
                    written = handToWritten(phisOfHead);
                LValue limit = m_out.constInt32(static_cast<int32_t>(nearest));
                LBasicBlock doesNotWrap = m_out.newBlock();
                m_out.branch(countsUp ? m_out.lessThan(bound, limit) : m_out.greaterThan(bound, limit), unlikelyEdge(tooFewForAnyLayout()), normalEdge(doesNotWrap));
                m_out.appendTo(doesNotWrap);
            }
        }
        if (comesBack) {
            if (!ring) {
                for (size_t index = 0; index < parameters.size(); ++index) {
                    if (phisOfEntry[index])
                        handOn(phisOfHead[index], phisOfEntry[index]);
                }
                m_out.upsilon(phisOfHead[indexOfCounter], paidUpTo);
                m_out.jump(normalEdge(entry));
                m_out.appendTo(entry);
            }
            paidOfWay = paidUpTo;
            written = handToWritten(phisOfEntry);
            if (!comparesLastStartLate)
                laidOut = handToTurns();
        }
        auto branchOnTripCount = [&](LBasicBlock tooFewAreLeft) {
            LBasicBlock enoughAreLeft = m_out.newBlock();
            bool shouldSkipTripCountCheck = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrollTripCountCheck);
            m_out.branch(shouldSkipTripCountCheck ? m_out.opaque(m_out.booleanTrue) : holdsOf(counter, lastStart), normalEdge(enoughAreLeft), normalEdge(tooFewAreLeft));
            m_out.appendTo(enoughAreLeft);
        };
        if (!tripCountIsChecked && !single)
            branchOnTripCount(tooFewForAnyLayout());
        for (size_t place = 0; place < loop.boundsAskedInFront.size() && !ring; ++place) {
            const BoundAskedInFront& bound = loop.boundsAskedInFront[place];
            bool isAskedAlready = std::ranges::any_of(loop.boundsAskedInFront.span().first(place), [&](const BoundAskedInFront& earlier) {
                return earlier.length == bound.length && earlier.most >= bound.most;
            });
            if (isAskedAlready)
                continue;
            LValue length = get(bound.length).value;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(m_out.lessThan(m_out.constantOfType(length->type(), bound.most), length)), normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        }
        for (ValueID relation : loop.relationsAskedInFront) {
            if (ring || countIsAsked)
                break;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(emitter<ControlEmitter>().hoistedResult(relation).value()), normalEdge(holds), normalEdge(tooFewForAnyLayout()));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopComparisonsHoisted);
        }
        if (ring || countIsAsked) {
            RELEASE_ASSERT(std::ranges::all_of(loop.lengthsHeldAgainstBound, [&](ValueID length) {
                return m_lengthsHeldAgainstBoundByLayoutThatAsked.contains(length);
            }));
        } else
            m_lengthsHeldAgainstBoundByLayoutThatAsked = loop.lengthsHeldAgainstBound;
        for (ValueID asked : loop.lengthsHeldAgainstBound) {
            if (ring || countIsAsked)
                break;
            LBasicBlock holds = m_out.newBlock();
            LValue length = get(asked).value;
            LValue fallsShort = bound->type() == Machine::Int32 && length->type() == Machine::Int32 && bounds.minimum >= 0 ? m_out.below(length, bound) : m_out.lessThan(m_out.zeroExt(length, Machine::Int64), bound->type() == Machine::Int64 ? bound : bounds.minimum >= 0 ? m_out.zeroExt(bound, Machine::Int64) : m_out.signExt32To64(bound));
            m_out.branch(shouldSkipEntryChecks ? m_out.opaque(m_out.booleanFalse) : fallsShort, normalEdge(tooFewForAnyLayout()), normalEdge(holds));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopLengthChecksHoisted);
        }
        for (unsigned count = 0; count < (loop.rowCountsAsked ? m_rows->counts.size() : 0); ++count) {
            if (!(loop.rowCountsAsked >> count & 1))
                continue;
            LBasicBlock holds = m_out.newBlock();
            LValue passed = m_out.load32(rowCountAddress(count));
            m_out.branch(answerTo(m_out.belowOrEqual(bound, bound->type() == Machine::Int64 ? m_out.zeroExt(passed, Machine::Int64) : passed)), normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopsOverCountedRows);
        }
        for (const SteppedIndex& held : loop.steppedIndexes) {
            if (ring || countIsAsked)
                break;
            auto wide = [&](LValue value) { return value->type() == Machine::Int32 ? m_out.signExt32To64(value) : value; };
            LValue greatest = wide(phisOfEntry[held.indexOfParameter]);
            if (held.step > 0) {
                LValue distance = m_out.sub(wide(bound), wide(phisOfEntry[indexOfCounter]));
                greatest = m_out.add(greatest, held.step == 1 ? distance : m_out.mul(distance, m_out.constInt64(held.step)));
            }
            int64_t added = held.offset - (held.step > 0 && loop.relation == Opcode::Less ? held.step : 0);
            if (added)
                greatest = m_out.add(greatest, m_out.constInt64(added));
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(m_out.lessThan(greatest, m_out.zeroExt(get(held.length).value, Machine::Int64))), normalEdge(holds), normalEdge(tooFewForAnyLayout()));
            m_out.appendTo(holds);
            build().statistics().count(StatisticID::UnrolledLoopLengthChecksHoisted);
        }
        for (ValueID string : loop.flatStrings) {
            std::optional<Condition> hasWidth = emitter<StringEmitter>().findWidth(string, !loop.asksTextsForWide);
            if (!hasWidth || hasWidth->value()->hasInt())
                continue;
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(hasWidth->value()), normalEdge(holds), otherKind ? normalEdge(otherKind) : unlikelyEdge(written));
            m_out.appendTo(holds);
        }
        for (const ListAskedForKind& asked : loop.listsAskedForKind) {
            LBasicBlock holds = m_out.newBlock();
            auto known = std::ranges::find_if(loop.kindsToldToReads, [&](const KindToldToReads& told) {
                return told.list == asked.list && asked.kind == ArrayStorage::Double && !asked.isStoredInto && m_values[told.holdsDoubles.index()];
            });
            if (known != loop.kindsToldToReads.end()) {
                m_out.branch(answerTo(get(known->holdsDoubles).value), normalEdge(holds), normalEdge(otherKind ? otherKind : written));
                m_out.appendTo(holds);
                build().statistics().count(StatisticID::KnownKindsCheckedAtLoopEntry);
                continue;
            }
            Condition hasKind = asked.isStoredInto ? emitter<ElementAccessEmitter>().listTakesStoresInArrayStorage(asked.list, asked.kind) : asked.otherKindsAreNotAsked ? emitter<ElementAccessEmitter>().listHasArrayStorage(asked.list, asked.kind) : emitter<ElementAccessEmitter>().numericListHasArrayStorage(asked.list, asked.kind);
            m_out.branch(answerTo(hasKind.value()), normalEdge(holds), normalEdge(otherKind ? otherKind : written));
            m_out.appendTo(holds);
        }
        for (ValueID view : loop.viewsAskedForKind) {
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(answerTo(emitter<ElementAccessEmitter>().viewIsOfKind(view, loop.kindAskedOfView).value()), normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        }
        for (ValueID list : loop.listsWhoseKindIsRead) {
            LValue holdsDoubles = (loop.viewsAskedForKind.isEmpty() ? emitter<ElementAccessEmitter>().numericListHasArrayStorage(list, ArrayStorage::Double) : emitter<ElementAccessEmitter>().listHasArrayStorage(list, ArrayStorage::Double)).value();
            m_kindsReadInFrontOfTurnsLaidOut.append({ list, shouldSkipEntryChecks ? m_out.bitXor(holdsDoubles, m_out.booleanTrue) : holdsDoubles });
            build().statistics().count(StatisticID::UnrolledLoopStorageChecksHoisted);
        }
        for (const ExactCounter& exact : loop.exactCounters) {
            LValue entering = phisOfEntry[exact.indexOfParameter];
            const Range& range = typeOf(parameters[exact.indexOfParameter]).integers;
            LBasicBlock isFarAbove = m_out.newBlock();
            LBasicBlock isFarBelow = m_out.newBlock();
            if (range.minimum < -maxExactInteger + exact.mostStepped)
                m_out.branch(answerTo(m_out.greaterThanOrEqual(entering, m_out.constInt64(-maxExactInteger + exact.mostStepped))), normalEdge(isFarAbove), unlikelyEdge(written));
            else
                m_out.jump(normalEdge(isFarAbove));
            m_out.appendTo(isFarAbove);
            m_out.branch(answerTo(m_out.lessThanOrEqual(entering, m_out.constInt64(maxExactInteger - exact.mostStepped))), normalEdge(isFarBelow), unlikelyEdge(written));
            m_out.appendTo(isFarBelow);
            build().statistics().count(StatisticID::ExactCountersAskedOnce);
        }
        if (loop.holdsProductSum)
            widenFactorsBeforeIterations(loop, wordsWidenedBeforeIterations);
        if (single) {
            if (comparesLastStartLate)
                lastStart = findLastStart();
            branchOnTripCount(blockHandingOver(phisOfSingle, single, [&](size_t index) {
                return phisOfEntry[index];
            }));
        }
        bool someTurnMayLeave = std::ranges::any_of(loop.turn, [&](BlockID id) {
            const Terminator& terminator = code().at(id).terminator;
            for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
                BlockID target = code().at(terminator.edge(edge)).to;
                if (target != head && !loop.turn.contains(target))
                    return true;
            }
            return false;
        });
        bool sumsMayLeaveAtEntry = hasInvariantTerm && comesBack && !single;
        for (SumOfIntegers& sum : m_sumsOfIntegers) {
            if (!someTurnMayLeave && !sumsMayLeaveAtEntry && !sum.subtractsSomeTerm() && !sum.isReadInIterations)
                continue;
            LBasicBlock isNotMinusZero = m_out.newBlock();
            m_out.branch(m_out.equal(m_out.bitCast(phisOfEntry[sum.indexOfParameter], Machine::Int64), m_out.constInt64(std::numeric_limits<int64_t>::min())), unlikelyEdge(written), normalEdge(isNotMinusZero));
            m_out.appendTo(isNotMinusZero);
        }
        for (SumOfIntegers& sum : m_sumsOfIntegers) {
            if (sum.termIsInvariant) {
                size_t place = std::ranges::find(parameters, sum.term) - parameters.begin();
                LValue number = place < parameters.size() ? phisOfEntry[place] : get(sum.term).value;
                LBasicBlock isSmall = m_out.newBlock();
                LBasicBlock isWhole = m_out.newBlock();
                LBasicBlock isOther = comesBack ? blockHandingOver(phisOfLeaving, leaving, [&](size_t index) -> LValue {
                    const SumOfIntegers* left = single ? nullptr : sumAt(index);
                    if (left)
                        m_out.upsilon(phisOfEntry[index], left->baseAtExit);
                    return left ? m_out.constInt64(0) : phisOfEntry[index];
                }) : written;
                m_out.branch(m_out.doubleLessThanOrEqual(m_out.doubleAbs(number), m_out.constDouble(largestInvariantTerm)), normalEdge(isSmall), unlikelyEdge(isOther));
                m_out.appendTo(isSmall);
                sum.invariantTerm = emitter<ConversionEmitter>().truncateUnchecked(Number(number)).value();
                m_out.branch(m_out.equal(m_out.bitCast(m_out.intToDouble(sum.invariantTerm), Machine::Int64), m_out.bitCast(number, Machine::Int64)), normalEdge(isWhole), unlikelyEdge(isOther));
                m_out.appendTo(isWhole);
            }
            requireWholeNumber(phisOfEntry[sum.indexOfParameter], parameters[sum.indexOfParameter], written, sum.largestBase);
            if (sum.isReadInIterations)
                sum.wordOfBase = emitter<ConversionEmitter>().truncateUnchecked(Number(phisOfEntry[sum.indexOfParameter])).value();
        }
        m_out.jump(normalEdge(laidOut ? laidOut : handToTurns()));
    }

    m_out.appendTo(turns);
    m_headOfTurnsLaidOut = head;
    m_iterationMayThrowInPlace = loop.loopCarriedValueStores.isEmpty();
    m_stepsOfExactCounters = loop.stepsOfExactCounters;
    m_rotatesUnrolledIterationNumbers = loop.rotatesNumbers;
    for (auto& [index, number] : numbersOfTurns)
        m_numbersOfIntegers.append({ phisOfTurns[index], number });
    leavesTurnsLaidOut = true;
    if (comesBack || (headOfRest != writtenHead && !noTurnComesToNextFront))
        m_unrolledLoopBlocks = collectLoopBlocks(loop);
    if (loop.poll && code().blockOf(loop.poll) != head)
        m_valuesLeftOutOfTurnsLaidOut.append(loop.poll);
    for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores) {
        if (store.box)
            m_valuesLeftOutOfTurnsLaidOut.append(store.box);
        m_valuesLeftOutOfTurnsLaidOut.append(store.store);
    }
    Vector<size_t, 4> takingAnyNaNInTurnsAlone;
    if (m_followsNumbersOutOfBytes) {
        m_blocksBegun.quickSet(head.index());
        for (size_t index = 0; index < parameters.size() && !m_followsMarkers; ++index) {
            if (!phisOfTurns[index] || sumAt(index) || representationOf(parameters[index]) != Representation::Double || holdsAnyNaN(parameters[index]))
                continue;
            m_valuesHoldingAnyNaN.quickSet(parameters[index].index());
            takingAnyNaNInTurnsAlone.append(index);
        }
    }
    auto forWrittenLoop = [&](size_t index, LValue value) {
        return takingAnyNaNInTurnsAlone.contains(index) ? emitter<ConversionEmitter>().purified(Number(value)).value() : value;
    };
    for (size_t index = 0; index < parameters.size(); ++index) {
        const SumOfIntegers* sum = sumAt(index);
        m_values[parameters[index].index()] = !sum ? phisOfTurns[index] : sum->isReadInIterations ? numberOfSum(*sum) : nullptr;
    }
    for (size_t index : indexesOfWide) {
        LValue word = m_out.castToInt32(phisOfTurns[index]);
        emitter<ConversionEmitter>().noteSignExtended(Word32(word), Integer(phisOfTurns[index]));
        m_values[parameters[index].index()] = word;
    }
    unsigned numberOfExtendedIterations = turnsInRow;
    LValue firstCounter = phisOfTurns[indexOfCounter];
    bool counterIsExtended = firstCounter->type() == Machine::Int32 && typeOf(loop.counter).integers.minimum >= 0 && countsUp;
    LValue extensionForAllTurns = counterIsExtended && isARM64() ? m_out.extendWordThatIsNotNegative(firstCounter) : nullptr;
    auto counterBehind = [&](unsigned numberOfTurns) -> LValue {
        if (counterIsExtended && numberOfTurns < numberOfExtendedIterations)
            return m_out.castToInt32(m_out.add(extensionForAllTurns ? extensionForAllTurns : m_out.extendWordThatIsNotNegative(firstCounter), m_out.constInt64(numberOfTurns * loop.step)));
        return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), numberOfTurns * loop.step));
    };
    auto wordCutAtExit = [&](LValue value) -> LValue {
        return value->opcode() == Machine::Trunc && value->child(0)->type() == Machine::Int64 ? m_out.castToInt32(value->child(0)) : value;
    };
    auto recomputedAtExit = [&](size_t index, LValue ahead) -> LValue {
        bool isStepped = ahead && ahead->opcode() == Machine::Add && ahead->child(0) == phisOfTurns[index] && ahead->child(1)->hasInt();
        return isStepped ? m_out.add(ahead->child(0), ahead->child(1)) : ahead ? wordCutAtExit(ahead) : nullptr;
    };
    auto recomputedAfterIterations = [&](LValue behind, LValue first) -> LValue {
        bool isStepped = behind->opcode() == Machine::Add && behind->child(0) == first && behind->child(1)->hasInt();
        return isStepped ? m_out.add(behind->child(0), behind->child(1)) : wordCutAtExit(behind);
    };
    m_turnLaidOut = turn;
    Values valuesAheadOfTurn;
    unsigned iteration = 0;
    if (!m_exitsOfUnrolledLoop.isEmpty()) {
        m_blockLeavingLoop = [&](const Edge& edge) {
            for (size_t index : takingAnyNaNInTurnsAlone)
                m_valuesHoldingAnyNaN.quickClear(parameters[index].index());
            LBasicBlock here = m_out.m_block;
            LBasicBlock blockOnEdge = m_out.newBlock();
            m_out.appendTo(blockOnEdge);
            auto valueAtExit = [&](size_t index) -> LValue {
                LValue ahead = valuesAheadOfTurn[index];
                if (const SumOfIntegers* sum = sumAt(index))
                    return m_out.doubleAdd(sum->base, m_out.intToDouble(ahead));
                if (index == indexOfCounter && iteration)
                    return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), iteration * loop.step));
                return forWrittenLoop(index, recomputedAtExit(index, ahead));
            };
            Values values;
            for (size_t index = 0; index < parameters.size(); ++index) {
                bool isStored = std::ranges::contains(loop.loopCarriedValueStores, index, &LoopCarriedValueStore::indexOfParameter);
                values.append(valuesAheadOfTurn[index] && (isStored || isPassedToExit(edge, parameters[index])) ? valueAtExit(index) : nullptr);
            }
            LBasicBlock behindStores = nullptr;
            if (!loop.loopCarriedValueStores.isEmpty() && !iteration) {
                LBasicBlock someIterationHasRun = m_out.newBlock();
                behindStores = m_out.newBlock();
                m_out.branch(m_out.equal(valuesAheadOfTurn[indexOfCounter], phisOfEntry[indexOfCounter]), normalEdge(behindStores), normalEdge(someIterationHasRun));
                m_out.appendTo(someIterationHasRun);
            }
            for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores)
                storeLoopCarriedValue(loop, store, values[store.indexOfParameter]);
            if (behindStores) {
                m_out.jump(normalEdge(behindStores));
                m_out.appendTo(behindStores);
            }
            leaveLoop(edge, values.span());
            m_out.appendTo(here);
            for (size_t index : takingAnyNaNInTurnsAlone)
                m_valuesHoldingAnyNaN.quickSet(parameters[index].index());
            return blockOnEdge;
        };
    }
    m_listsAskedForKindInFrontOfTurnsLaidOut = loop.listsAskedForKind.span();
    m_boundsAskedInFrontOfTurnsLaidOut = loop.boundsAskedInFront.span();
    m_relationsAskedInFrontOfTurnsLaidOut = loop.relationsAskedInFront;
    m_relationsAskedInFrontOfTurnsLaidOut.appendVector(loop.relationsOfCounterBelowBound);
    for (const RowGuard& guard : loop.rowCountsAsked ? m_rows->guards.span() : std::span<const RowGuard>()) {
        for (BlockID block : guard.checkBlocks) {
            for (ValueID value : loop.turn.contains(block) ? code().valuesOf(block) : std::span<const ValueID>()) {
                if (opcodeOf(value) == Opcode::CheckProof)
                    m_valuesLeftOutOfTurnsLaidOut.append(value);
            }
        }
    }
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = loop.accessesUnderCounterBelowBound.span();
    m_viewsAskedForKindInFrontOfTurnsLaidOut = loop.viewsAskedForKind.span();
    m_kindOfViewFoundInFrontOfTurnsLaidOut = loop.kindAskedOfView;
    m_holesLeaveTurnsLaidOut = loop.holesLeaveTurns;
    auto isComparedInt32 = [&](ValueID read) {
        return std::ranges::any_of(loop.listsAskedForKind, [&](const ListAskedForKind& asked) { return asked.list == code().at(read).arrayOperand() && asked.kind == ArrayStorage::Int32; });
    };
    bool onlyComparesInt32s = loop.carriesJoinedValue && !loop.listsAskedForKind.isEmpty() && std::ranges::all_of(turn, [&](BlockID block) {
        return std::ranges::all_of(code().valuesOf(block), [&](ValueID read) {
            if (opcodeOf(read) != Opcode::GetArrayElement)
                return true;
            ValueID user = isComparedInt32(read) ? code().onlyUserOf(read) : ValueID();
            return user && allRelations.contains(opcodeOf(user)) && std::ranges::all_of(code().at(user).operands(), [&](ValueID operand) {
                return !code().isConstant(operand) && opcodeOf(operand) == Opcode::GetArrayElement && std::ranges::contains(turn, code().blockOf(operand));
            });
        });
    });
    m_optionalNumbersLeaveAtHoles = loop.holesLeaveTurns && (!loop.carriesJoinedValue || onlyComparesInt32s);
    m_asksElementsBehindTurns = loop.holesLeaveTurns && !loop.storesIntoLists && std::ranges::all_of(turn, [&](BlockID block) {
        return code().at(block).terminator.kind == TerminatorKind::Jump && std::ranges::all_of(code().valuesOf(block), [&](ValueID read) {
            return opcodeOf(read) != Opcode::GetArrayElement || !code().at(read).readsHoleAsNaN() || isTermOfSumOfElements(read);
        });
    });
    if (!loop.listsAskedForKind.isEmpty())
        build().statistics().count(StatisticID::UnrolledLoopsForIndexingType);
    build().statistics().count(StatisticID::UnrolledLoopIntegerElementSums, std::ranges::count_if(m_sumsOfIntegers, [](const SumOfIntegers& sum) { return sum.termIsElementOfList; }));
    auto forgetWidenedWords = makeScopeExit([&] {
        for (LValue word : wordsWidenedBeforeIterations)
            emitter<ConversionEmitter>().forgetSignExtended(Word32(word));
    });
    auto emitIterations = [&](unsigned numberOfIterations) -> bool {
        for (unsigned turnLaidOut = 0; turnLaidOut < numberOfIterations; ++turnLaidOut) {
            forgetTurn();
            m_wordsOfElementsInTurnLaidOut.shrink(0);
            iteration = turnLaidOut;
            valuesAheadOfTurn.shrink(0);
            for (size_t index = 0; index < parameters.size(); ++index) {
                const SumOfIntegers* sum = sumAt(index);
                valuesAheadOfTurn.append(sum ? sum->added : m_values[parameters[index].index()]);
            }
            LBasicBlock blockLeavingTurn = nullptr;
            m_blockLeavingTurnLaidOut = [&] {
                if (!blockLeavingTurn) {
                    blockLeavingTurn = blockHandingOver(comesBack ? phisOfLeaving : noTurnComesToNextFront ? phisOfRest : phisOfWrittenHead, comesBack ? leaving : noTurnComesToNextFront ? headOfRest : writtenHead, [&](size_t index) -> LValue {
                        if (const SumOfIntegers* sum = sumAt(index))
                            return comesBack && !single ? passSumToExit(*sum, valuesAheadOfTurn[index]) : m_out.doubleAdd(sum->base, m_out.intToDouble(valuesAheadOfTurn[index]));
                        if (index == indexOfCounter && turnLaidOut)
                            return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), turnLaidOut * loop.step));
                        LValue ahead = recomputedAtExit(index, valuesAheadOfTurn[index]);
                        return comesBack ? ahead : forWrittenLoop(index, ahead);
                    });
                }
                return blockLeavingTurn;
            };
            LBasicBlock blockEndingLoop = nullptr;
            m_blockEndingLoop = [&] {
                if (!blockEndingLoop) {
                    blockEndingLoop = blockHandingOver(comesBack ? phisIntoWritten : phisOfRest, comesBack ? intoWritten : headOfRest, [&](size_t index) -> LValue {
                        if (const SumOfIntegers* sum = sumAt(index))
                            return m_out.doubleAdd(sum->base, m_out.intToDouble(valuesAheadOfTurn[index]));
                        if (index == indexOfCounter && turnLaidOut)
                            return m_out.add(firstCounter, m_out.constantOfType(firstCounter->type(), turnLaidOut * loop.step));
                        return forWrittenLoop(index, recomputedAtExit(index, valuesAheadOfTurn[index]));
                    });
                }
                return blockEndingLoop;
            };
            for (unsigned index = 0; index < turn.size(); ++index) {
                LBasicBlock& block = m_blocks[turn[index].index()];
                if (!index) {
                    block = m_out.m_block;
                    m_nextBlockGoesOnHere = true;
                } else
                    block = goesOnBehindJump(loop.positionsOfTurn[index]) ? nullptr : m_out.newBlock();
            }
            size_t indexOfPhi = 0;
            forEachParameterInTurn([&](BlockID block, ValueID parameter) {
                if (LValue phi = phisOfWrittenTurn[indexOfPhi++])
                    m_values[parameter.index()] = m_out.phiIn(blockFor(block), phi->type());
            });
            for (BlockID block : turn) {
                generateBlock(block);
                if (m_hasFailed)
                    return false;
                m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
            }
            RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
            for (size_t index = 0; index < parameters.size(); ++index) {
                const SumOfIntegers* sum = sumAt(index);
                if (phisOfTurns[index] && (!sum || sum->isReadInIterations))
                    m_values[parameters[index].index()] = index == indexOfCounter ? counterBehind(turnLaidOut + 1) : m_argumentsForNextTurn[index].value;
            }
        }
        return true;
    };
    if (!emitIterations(turnsInRow))
        return;
    m_asksElementsBehindTurns = false;
    if (LValue words = std::exchange(m_boxedWordsOfElementsInTurns, nullptr)) {
        LBasicBlock someIsAbsent = blockHandingOver(comesBack ? phisOfLeaving : noTurnComesToNextFront ? phisOfRest : phisOfWrittenHead, comesBack ? leaving : noTurnComesToNextFront ? headOfRest : writtenHead, [&](size_t index) -> LValue {
            const SumOfIntegers* sum = sumAt(index);
            if (sum && !(comesBack && !single))
                return m_out.doubleAdd(sum->base, m_out.intToDouble(phisOfTurns[index]));
            if (sum)
                return passSumToExit(*sum, phisOfTurns[index]);
            LValue left = indexesOfWide.contains(index) ? m_out.castToInt32(phisOfTurns[index]) : phisOfTurns[index];
            return comesBack ? left : forWrittenLoop(index, left);
        });
        LBasicBlock everyOneIsThere = m_out.newBlock();
        m_out.branch(emitter<ElementAccessEmitter>().areInt32Elements(words).value(), normalEdge(everyOneIsThere), rareEdge(someIsAbsent));
        m_out.appendTo(everyOneIsThere);
    }
    LBasicBlock endsIterations = m_out.newBlock();
    m_out.jump(normalEdge(endsIterations));
    m_out.appendTo(endsIterations);
    Values valuesBehindTurns;
    for (size_t index = 0; index < parameters.size(); ++index) {
        LValue behind = m_values[parameters[index].index()];
        valuesBehindTurns.append(behind && phisOfTurns[index] && !sumAt(index) ? recomputedAfterIterations(behind, phisOfTurns[index]) : behind);
    }
    if (single) {
        LBasicBlock endOfTurns = m_out.m_block;
        Vector<SumOfIntegers, 2> sumsOfTurns = std::exchange(m_sumsOfIntegers, { });
        firstCounter = phisOfSingle[indexOfCounter];
        extensionForAllTurns = nullptr;
        numberOfExtendedIterations = 0;
        iteration = 0;
        m_out.appendTo(single);
        valuesAheadOfTurn = phisOfSingle;
        for (size_t index = 0; index < parameters.size(); ++index)
            m_values[parameters[index].index()] = phisOfSingle[index];
        LBasicBlock begins = m_out.newBlock();
        LBasicBlock ended = nullptr;
        if (m_blockLeavingLoop && !loop.testsAtEnd && m_exitsOfUnrolledLoop.contains(code().at(code().at(loop.asking).terminator.notTaken()).to)) {
            TerminatorContext context(loop.asking, code().at(loop.asking).terminator, nullptr);
            SetForScope<const OperationContext*> whileLeaving(m_operation, &context);
            ended = m_blockLeavingLoop(code().at(code().at(loop.asking).terminator.notTaken()));
        } else {
            ended = blockHandingOver(comesBack ? phisIntoWritten : phisOfRest, comesBack ? intoWritten : headOfRest, [&](size_t index) {
                return forWrittenLoop(index, phisOfSingle[index]);
            });
        }
        LValue boundOfSingle = loop.addedBeforeTest ? m_out.sub(bound, m_out.constantOfType(bound->type(), loop.addedBeforeTest)) : bound;
        m_out.branch(holdsOf(firstCounter, boundOfSingle), normalEdge(begins), normalEdge(ended));
        m_out.appendTo(begins);
        if (!emitIterations(1))
            return;
        m_out.jump(normalEdge(blockHandingOver(phisOfSingle, single, [&](size_t index) {
            return recomputedAfterIterations(m_values[parameters[index].index()], phisOfSingle[index]);
        })));
        m_sumsOfIntegers = WTF::move(sumsOfTurns);
        m_out.appendTo(endOfTurns);
    }
    for (size_t index : takingAnyNaNInTurnsAlone)
        m_valuesHoldingAnyNaN.quickClear(parameters[index].index());
    m_headOfTurnsLaidOut = BlockID();
    m_iterationMayThrowInPlace = false;
    m_stepsOfExactCounters.shrink(0);
    m_turnLaidOut = { };
    m_listsAskedForKindInFrontOfTurnsLaidOut = { };
    m_boundsAskedInFrontOfTurnsLaidOut = { };
    m_relationsAskedInFrontOfTurnsLaidOut.shrink(0);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = { };
    m_viewsAskedForKindInFrontOfTurnsLaidOut = { };
    m_kindsReadInFrontOfTurnsLaidOut.shrink(0);
    m_holesLeaveTurnsLaidOut = false;
    m_optionalNumbersLeaveAtHoles = false;
    m_wordsOfElementsInTurnLaidOut.shrink(0);
    m_blockLeavingTurnLaidOut = nullptr;
    m_blockEndingLoop = nullptr;
    m_blockLeavingLoop = nullptr;
    m_unrolledLoopBlocks.shrink(0);
    m_nextBlockGoesOnHere = false;
    emitter<StringEmitter>().forgetWidthsFound();
    m_out.setOrigin(Machine::Origin());
    LValue counterBehindTurns = valuesBehindTurns[indexOfCounter];
    RELEASE_ASSERT(counterBehindTurns->type() != Machine::Int32 || Machine::clearsUpperHalf(counterBehindTurns->opcode()));
    auto goingRound = [&](LValue nextEndOfStretch) {
        return blockHandingOver(phisOfTurns, turns, [&](size_t index) {
            if (index == indexOfCounter && endOfStretch)
                m_out.upsilon(nextEndOfStretch ? nextEndOfStretch : endOfStretchFrom(m_out.opaque(counterBehindTurns), lastStart), endOfStretch);
            const SumOfIntegers* sum = sumAt(index);
            if (sum && loop.sumsAreTakenOverBehindStretches) {
                m_out.upsilon(nextEndOfStretch ? sum->base : sum->takenOver, sum->base);
                if (!nextEndOfStretch)
                    return m_out.constInt64(0);
            }
            return sum ? sum->added : asCarriedByLoop(index, valuesBehindTurns[index]);
        });
    };
    if (pollsBetweenStretches) {
        LBasicBlock stretchHasEnded = m_out.newBlock();
        m_out.branch(holdsOf(counterBehindTurns, endOfStretch), normalEdge(goingRound(endOfStretch)), unlikelyEdge(stretchHasEnded));
        m_out.appendTo(stretchHasEnded);
        for (const LoopCarriedValueStore& store : loop.loopCarriedValueStores) {
            const SumOfIntegers* sum = sumAt(store.indexOfParameter);
            storeLoopCarriedValue(loop, store, sum ? m_out.doubleAdd(sum->base, m_out.intToDouble(sum->added)) : forWrittenLoop(store.indexOfParameter, valuesBehindTurns[store.indexOfParameter]));
        }
        Vector<ValueID, 4> leftOut = std::exchange(m_valuesLeftOutOfTurnsLaidOut, { });
        generateValue(code().blockOf(loop.poll), loop.poll, code().at(loop.poll), false, { });
        m_valuesLeftOutOfTurnsLaidOut = WTF::move(leftOut);
        m_out.setOrigin(Machine::Origin());
    }
    RELEASE_ASSERT(pollsBetweenStretches || !loop.sumsAreTakenOverBehindStretches);
    if (loop.sumsAreTakenOverBehindStretches) {
        for (SumOfIntegers& sum : m_sumsOfIntegers)
            sum.takenOver = m_out.doubleAdd(sum.base, m_out.intToDouble(sum.added));
    }
    LBasicBlock again = goingRound(nullptr);
    LBasicBlock tooFewAreLeft = blockHandingOver(single ? phisOfSingle : comesBack ? phisIntoWritten : phisOfRest, single ? single : comesBack ? intoWritten : headOfRest, [&](size_t index) {
        const SumOfIntegers* sum = sumAt(index);
        if (sum)
            return sum->takenOver ? sum->takenOver : m_out.doubleAdd(sum->base, m_out.intToDouble(sum->added));
        LValue behind = wordCutAtExit(valuesBehindTurns[index]);
        return single ? behind : forWrittenLoop(index, behind);
    });
    m_rotatesUnrolledIterationNumbers = false;
    m_numbersOfIntegers.shrink(0);
    if (loop.sumsAreTakenOverBehindStretches) {
        LBasicBlock enoughAreLeft = m_out.newBlock();
        m_out.branch(holdsOf(counterBehindTurns, lastStart), normalEdge(enoughAreLeft), normalEdge(tooFewAreLeft));
        m_out.appendTo(enoughAreLeft);
        for (const SumOfIntegers& sum : m_sumsOfIntegers) {
            LBasicBlock isSmall = m_out.newBlock();
            m_out.branch(m_out.doubleLessThanOrEqual(m_out.doubleAbs(sum.takenOver), m_out.constDouble(sum.largestBase)), normalEdge(isSmall), unlikelyEdge(tooFewAreLeft));
            m_out.appendTo(isSmall);
        }
        m_out.jump(normalEdge(again));
    } else
        m_out.branch(holdsOf(counterBehindTurns, lastStart), normalEdge(again), normalEdge(tooFewAreLeft));
    if (comesBack && leaving->numberOfIncomingEdges()) {
        m_out.appendTo(leaving);
        LValue left = phisOfLeaving[indexOfCounter];
        auto steps = [&](int64_t turns) {
            return m_out.constantOfType(typeOfCounter, turns * loop.step);
        };
        int64_t oneTurn = loop.relation == Opcode::Less ? 1 : loop.relation == Opcode::Greater ? -1 : 0;
        LValue paid = m_out.add(paidUpTo, steps(turnsThatPayForComingBack));
        LValue isPaid = countsUp ? m_out.greaterThanOrEqual(left, paid) : m_out.lessThanOrEqual(left, paid);
        LBasicBlock nothingIsPaid = m_out.newBlock();
        LBasicBlock handsOver = m_out.newBlock();
        limitOfWay = m_out.phiIn(handsOver, typeOfCounter);
        paidOfWay = m_out.phiIn(handsOver, typeOfCounter);
        m_out.upsilon(m_out.add(left, m_out.constantOfType(typeOfCounter, oneTurn)), limitOfWay);
        m_out.upsilon(paidUpTo, paidOfWay);
        m_out.branch(isPaid, normalEdge(handsOver), normalEdge(nothingIsPaid));
        m_out.appendTo(nothingIsPaid);
        auto onward = [&](LValue from, LValue distance) {
            return countsUp ? m_out.add(from, distance) : m_out.sub(from, distance);
        };
        LValue most = m_out.constantOfType(typeOfCounter, mostTurnsOwed * std::abs(loop.step));
        LValue owed = countsUp ? m_out.sub(paid, left) : m_out.sub(left, paid);
        owed = m_out.select(m_out.below(owed, most), owed, most);
        LValue stay = m_out.mul(owed, m_out.constantOfType(typeOfCounter, turnsWrittenForEachTurnOwed));
        LValue end = onward(left, stay);
        LValue room = countsUp ? m_out.sub(bound, left) : m_out.sub(left, bound);
        m_out.upsilon(m_out.select(m_out.below(room, stay), bound, end), limitOfWay);
        m_out.upsilon(m_out.sub(onward(end, owed), steps(turnsThatPayForComingBack)), paidOfWay);
        m_out.jump(normalEdge(handsOver));
        m_out.appendTo(handsOver);
        m_out.jump(normalEdge(blockHandingOver(phisIntoWritten, intoWritten, [&](size_t index) -> LValue {
            const SumOfIntegers* sum = single ? nullptr : sumAt(index);
            return sum ? m_out.doubleAdd(sum->baseAtExit ? sum->baseAtExit : sum->base, m_out.intToDouble(phisOfLeaving[index])) : forWrittenLoop(index, phisOfLeaving[index]);
        })));
        limitOfWay = nullptr;
        paidOfWay = paidUpTo;
    }
    m_sumsOfIntegers.shrink(0);
    m_valuesLeftOutOfTurnsLaidOut.shrink(0);

    forgetTurn();
    for (unsigned index = 0; index < turn.size(); ++index)
        m_blocks[turn[index].index()] = blocksOfWrittenTurn[index];
    size_t indexOfPhi = 0;
    forEachParameterInTurn([&](BlockID, ValueID parameter) {
        m_values[parameter.index()] = phisOfWrittenTurn[indexOfPhi++];
    });
    m_blocks[head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWrittenHead[index];
    if (comesBack && !ring) {
        m_out.appendTo(intoWritten);
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (phisOfWrittenHead[index])
                handOn(phisIntoWritten[index], phisOfWrittenHead[index]);
        }
        m_out.jump(normalEdge(writtenHead));
        m_limitOfWrittenLoop = LimitOfWrittenLoop { loop.asking, loop.relation, loop.step, indexOfCounter, bound, limit, paidIntoWritten, paidUpTo, entry, phisOfEntry, phisOfWrittenHead };
    }
    if (otherKind)
        m_ringOfLoopLaidOut = RingOfLoopLaidOut { entry, otherKind, intoWritten, writtenHead, bound, comparesLastStartLate ? nullptr : lastStart, limit, paidIntoWritten, paidUpTo, phisOfHead, phisOfEntry, phisIntoWritten, phisOfWrittenHead, loop.layoutWithoutQuestionFollows, tripCountIsChecked || !single };
}

void CodeGenerator::declareAccessesSlow(const OperationContext& context)
{
    if (m_initializationDepth) {
        m_out.clearDeclaredAccesses();
        return;
    }

    Effects effects = effectsOf(context);
    HeapList reads;
    HeapList writes;
    heaps().appendHeaps(effects.reads, reads);
    heaps().appendHeaps(effects.writes, writes);
    heaps().appendHeapsOfConvertedCells(reads);
    if (effects.mayCollect)
        heaps().appendHeapsWrittenByCollection(writes);
    if (AOTHeaps::mayConvertCells(effects))
        heaps().appendHeapsOfConvertedCells(writes);
    m_out.setDeclaredAccesses(m_proc.heapSet(reads.span()), m_proc.heapSet(writes.span()));
}

LBasicBlock CodeGenerator::blockFor(BlockID id) const
{
    LBasicBlock block = m_blocks[id.index()];
    RELEASE_ASSERT(block);
    return block;
}

BlockEdge CodeGenerator::edgeTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Edge& edge = code().at(terminator->edge(targetIndex));
    const Block& block = code().at(edge.to);
    if (block.parameters().empty() && !m_headOfTurnsLaidOut && m_loopExits.isEmpty() && !m_keepsValuesInHeapFrame) [[likely]]
        return BlockEdge(blockFor(edge.to), edgeClassLeadingTo(block.frequency));
    return edgeThatMayPassArguments(terminator, edge, declaredResults);
}

NEVER_INLINE BlockEdge CodeGenerator::edgeThatMayPassArguments(const TerminatorContext& terminator, const Edge& edge, std::span<const Operand> declaredResults)
{
    const Block& blockAtEnd = code().at(edge.to);
    if (leavesTurnLaidOut(edge.to)) [[unlikely]]
        return edgeLeavingTurnLaidOut(edge);
    BlockEdge target(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
    for (ValueID parameter : blockAtEnd.parameters()) {
        if (m_values[parameter.index()])
            return edgeThatPassesArguments(terminator, edge, declaredResults);
    }
    if (findLoopExit(edge.to)) [[unlikely]]
        return edgeThatPassesArguments(terminator, edge, declaredResults);
    if (m_keepsValuesInHeapFrame && emitter<SavedFrameEmitter>().clearsWordsOnWayBackAlong(edge)) [[unlikely]]
        return edgeThatPassesArguments(terminator, edge, declaredResults);
    return target;
}

std::optional<uint32_t> CodeGenerator::constantMask(ValueID value) const
{
    auto operationMask = [&](ValueID id) -> std::optional<uint32_t> {
        const Operation& operation = code().at(id);
        if (operation.opcode != Opcode::BitAnd)
            return std::nullopt;
        for (ValueID operand : operation.operands()) {
            if (opcodeOf(operand) == Opcode::Integer)
                return static_cast<uint32_t>(code().at(operand).integer());
        }
        return std::nullopt;
    };
    if (std::optional<uint32_t> mask = operationMask(value))
        return mask;
    if (opcodeOf(value) != Opcode::Parameter)
        return std::nullopt;
    BlockID block = code().at(value).block();
    if (!block)
        return std::nullopt;
    std::span<const ValueID> parameters = code().at(block).parameters();
    size_t index = std::ranges::find(parameters, value) - parameters.begin();
    if (index == parameters.size())
        return std::nullopt;
    std::optional<uint32_t> mask;
    for (EdgeID id = code().at(block).firstEdgeIn(); id; id = code().at(id).nextInto()) {
        const Edge& edge = code().at(id);
        std::optional<uint32_t> passed = index < edge.arguments().size() ? operationMask(edge.arguments()[index]) : std::nullopt;
        if (!passed || (mask && *mask != *passed))
            return std::nullopt;
        mask = passed;
    }
    return mask;
}

bool CodeGenerator::isInEqualityChain(ValueID test, bool isNegated) const
{
    const Operation& asked = code().at(test);
    auto comparesSameValue = [&](BlockID id) {
        const Terminator& terminator = code().at(id).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            return false;
        ValueID condition = terminator.conditionOperand();
        while (opcodeOf(condition) == Opcode::LogicalNot)
            condition = code().at(condition).operand(0);
        const Operation& other = code().at(condition);
        return condition != test && other.opcode == asked.opcode && std::ranges::any_of(other.operands(), [&](ValueID operand) {
            return !code().isConstant(operand) && std::ranges::contains(asked.operands(), operand);
        });
    };
    BlockID block = asked.block();
    if (!block || code().at(block).terminator.kind != TerminatorKind::Branch)
        return false;
    const Terminator& terminator = code().at(block).terminator;
    if (comparesSameValue(code().at(isNegated ? terminator.taken() : terminator.notTaken()).to))
        return true;
    BlockID above = singlePredecessorOf(block);
    return above && comparesSameValue(above);
}

bool CodeGenerator::isReadOffLinkedEntry(ValueID compared) const
{
    auto entryOf = [&](ValueID value) -> ValueID {
        ValueID read = underlyingCellOf(code(), value);
        return opcodeOf(read) == Opcode::GetSlot ? underlyingCellOf(code(), code().at(read).operand(0)) : ValueID();
    };
    ValueID entry = entryOf(compared);
    if (!entry || opcodeOf(entry) != Opcode::Parameter)
        return false;
    BlockID head = code().at(entry).block();
    if (!head || !code().at(head).isLoopHead)
        return false;
    size_t place = code().at(head).placeOfParameter(entry);
    for (EdgeID edge = code().at(head).firstEdgeIn(); edge; edge = code().at(edge).nextInto()) {
        std::span<const ValueID> arguments = code().at(edge).arguments();
        if (place < arguments.size() && entryOf(arguments[place]) == entry)
            return true;
    }
    return false;
}

bool CodeGenerator::comparesLinkedEntry(ValueID test) const
{
    while (opcodeOf(test) == Opcode::LogicalNot)
        test = code().at(test).operand(0);
    const Operation& asked = code().at(test);
    return (asked.opcode == Opcode::StrictEq || asked.opcode == Opcode::Eq) && (isReadOffLinkedEntry(asked.operand(0)) || isReadOffLinkedEntry(asked.operand(1)));
}

static bool mostlyDiffer(Opcode left, Opcode right)
{
    static constexpr OpcodeSet oftenEqual = primitiveConstants | OpcodeSet { Opcode::GetArrayLength };
    return !oftenEqual.contains(left) && !oftenEqual.contains(right);
}

std::optional<bool> CodeGenerator::unlikelyAnswer(ValueID test, LooksBehindJoins looksBehindJoins) const
{
    bool isNegated = false;
    for (; opcodeOf(test) == Opcode::LogicalNot; test = code().at(test).operand(0))
        isNegated = !isNegated;
    const Operation& asked = code().at(test);
    std::optional<bool> unusual;
    switch (asked.opcode) {
    case Opcode::ToBoolean:
        if (std::optional<uint32_t> mask = constantMask(asked.operand(0)))
            unusual = std::popcount(*mask) <= maxUnlikelyMaskBits;
        break;
    case Opcode::StrictEq:
    case Opcode::Eq:
        if (bool firstIsInteger = opcodeOf(asked.operand(0)) == Opcode::Integer; firstIsInteger != (opcodeOf(asked.operand(1)) == Opcode::Integer) && !constantMask(asked.operand(firstIsInteger)))
            unusual = true;
        else if (code().at(asked.block()).isInLoop && mostlyDiffer(opcodeOf(asked.operand(0)), opcodeOf(asked.operand(1))))
            unusual = true;
        else if (isInEqualityChain(test, isNegated))
            unusual = true;
        break;
    case Opcode::Parameter:
        if (looksBehindJoins == LooksBehindJoins::Yes)
            unusual = unlikelyJoinResult(test);
        break;
    case Opcode::IsSlotFilled:
        if (code().at(asked.block()).isInLoop || (code().function() && facts().linked().functionsReachedByRecursion.get(code().function().index())))
            unusual = false;
        break;
    default:
        break;
    }
    if (unusual && isNegated)
        unusual = !*unusual;
    return unusual;
}

std::optional<bool> CodeGenerator::unlikelyJoinResult(ValueID parameter) const
{
    BlockID block = code().at(parameter).block();
    if (!block || code().at(parameter).representation != Representation::Boolean)
        return std::nullopt;
    std::span<const ValueID> parameters = code().at(block).parameters();
    size_t index = std::ranges::find(parameters, parameter) - parameters.begin();
    if (index == parameters.size())
        return std::nullopt;
    std::optional<bool> unusual;
    std::array<bool, 2> comesAlongUsualWay { };
    for (EdgeID id = code().at(block).firstEdgeIn(); id; id = code().at(id).nextInto()) {
        const Edge& edge = code().at(id);
        if (index >= edge.arguments().size())
            return std::nullopt;
        if (code().at(edge.from).frequency == BlockFrequency::Rare)
            continue;
        ValueID passed = edge.arguments()[index];
        if (!passed)
            return std::nullopt;
        if (opcodeOf(passed) == Opcode::Boolean) {
            comesAlongUsualWay[code().at(passed).boolean()] |= !isOnUnlikelySide(edge.from);
            continue;
        }
        std::optional<bool> answer = unlikelyAnswer(passed, LooksBehindJoins::No);
        if (!answer || (unusual && *unusual != *answer))
            return std::nullopt;
        unusual = answer;
    }
    if (!unusual || comesAlongUsualWay[*unusual])
        return std::nullopt;
    return unusual;
}

bool CodeGenerator::isOnUnlikelySide(BlockID block) const
{
    static constexpr unsigned mostJumps = 4;
    for (unsigned step = 0; step < mostJumps; ++step) {
        BlockID above = singlePredecessorOf(block);
        if (!above)
            return false;
        const Terminator& terminator = code().at(above).terminator;
        if (terminator.kind == TerminatorKind::Branch) {
            std::optional<bool> unlikely = unlikelyAnswer(terminator.conditionOperand(), LooksBehindJoins::No);
            return unlikely && code().at(*unlikely ? terminator.taken() : terminator.notTaken()).to == block;
        }
        if (terminator.kind != TerminatorKind::Jump)
            return false;
        block = above;
    }
    return false;
}

void CodeGenerator::markUnlikelySide(const TerminatorContext& terminator, BranchTargets& targets) const
{
    if (targets.taken.edgeClass() != Machine::EdgeClass::Normal || targets.notTaken.edgeClass() != Machine::EdgeClass::Normal)
        return;
    std::optional<bool> unlikely = unlikelyAnswer(terminator->conditionOperand(), LooksBehindJoins::Yes);
    if (!unlikely)
        return;
    const Block& asking = code().at(terminator.block());
    if (asking.isInLoop && code().at(code().at(*unlikely ? terminator->taken() : terminator->notTaken()).to).isInLoop && comparesLinkedEntry(terminator->conditionOperand()))
        unlikely = !*unlikely;
    const Block& unusual = code().at(code().at(*unlikely ? terminator->taken() : terminator->notTaken()).to);
    const Block& usual = code().at(code().at(*unlikely ? terminator->notTaken() : terminator->taken()).to);
    if (&unusual == &usual)
        return;
    if (asking.isLoopHead || unusual.isLoopHead || (asking.isInLoop && unusual.isInLoop && !usual.isInLoop))
        return;
    BlockEdge& edge = *unlikely ? targets.taken : targets.notTaken;
    edge = unlikelyEdge(edge.block());
}

std::optional<BlockEdge> CodeGenerator::edgeThatBringsNothing(EdgeID id)
{
    const Edge& edge = code().at(id);
    const Block& blockAtEnd = code().at(edge.to);
    LBasicBlock block = m_blocks[edge.to.index()];
    if (!block || isLayingOutTurns() || m_keepsValuesInHeapFrame || !edge.arguments().empty() || !blockAtEnd.parameters().empty() || findLoopExit(edge.to))
        return std::nullopt;
    return BlockEdge(block, edgeClassLeadingTo(blockAtEnd.frequency));
}

static constexpr OpcodeSet opcodesBehindStep { Opcode::ResultAt, Opcode::Increment };

void CodeGenerator::findStepsReadElsewhere()
{
    BitVector& found = m_stepsReadElsewhere.emplace(code().numberOfBlocks());
    BitVector branchesOnStep(code().numberOfBlocks());
    BitVector stepsTestingForEntry(code().numberOfBlocks());
    for (BlockID id : order()) {
        const Terminator& terminator = code().at(id).terminator;
        if (terminator.kind != TerminatorKind::Branch || opcodeOf(terminator.conditionOperand()) != Opcode::ResultAt)
            continue;
        branchesOnStep.quickSet(id.index());
        if (stepsOverTables.contains(opcodeOf(code().at(terminator.conditionOperand()).resultsOperand())))
            stepsTestingForEntry.quickSet(id.index());
    }
    for (BlockID user : order()) {
        auto noteRead = [&](ValueID value) {
            if (!value)
                return;
            const Operation& read = code().at(value);
            BlockID home = read.block();
            if (!home || !branchesOnStep.quickGet(home.index()) || found.quickGet(home.index()) || home == user)
                return;
            const Terminator& branch = code().at(home).terminator;
            if ((opcodesBehindStep.contains(read.opcode) || read.opcode == Opcode::ForInStep || stepsOverTables.contains(read.opcode)) && !dominates(code().at(stepsTestingForEntry.quickGet(home.index()) ? branch.taken() : branch.notTaken()).to, user))
                found.quickSet(home.index());
        };
        const Block& block = code().at(user);
        std::ranges::for_each(block.terminator.operands(), noteRead);
        for (ValueID value : code().valuesOf(user))
            std::ranges::for_each(code().at(value).operands(), noteRead);
        code().forEachEdgeOf(user, [&](EdgeID, const Edge& edge) {
            std::ranges::for_each(edge.arguments(), noteRead);
        });
        if (EdgeID handler = code().handlerOf(user); handler && branchesOnStep.quickGet(user.index())) {
            bool passesOwnValue = std::ranges::any_of(code().at(handler).arguments(), [&](ValueID value) {
                return value && code().at(value).block() == user;
            });
            if (passesOwnValue)
                found.quickSet(user.index());
        }
    }
}

std::optional<BlockEdge> CodeGenerator::edgeAfterLastStep(const ValueContext& step, uint32_t indexOfTest, StepTest tests)
{
    BlockID id = step.block();
    const Block& block = code().at(id);
    if (block.terminator.kind != TerminatorKind::Branch || isLayingOutTurns() || m_keepsValuesInHeapFrame)
        return std::nullopt;
    ValueID done = block.terminator.conditionOperand();
    const Operation& test = code().at(done);
    if (test.opcode != Opcode::ResultAt || test.block() != id || code().useCountOf(done) != 1 || test.resultsOperand() != step.id() || test.index() != indexOfTest)
        return std::nullopt;

    const Edge& end = code().at(tests == StepTest::IsDone ? block.terminator.taken() : block.terminator.notTaken());
    const Block& blockAtEnd = code().at(end.to);
    BlockID others = code().at(tests == StepTest::IsDone ? block.terminator.notTaken() : block.terminator.taken()).to;
    if (!end.arguments().empty() || !blockAtEnd.parameters().empty() || blockAtEnd.isLoopHead || findLoopExit(end.to) || m_singlePredecessors[others.index()] != id)
        return std::nullopt;

    for (ValueID value : step.operationsBehind()) {
        if (!opcodesBehindStep.contains(opcodeOf(value)))
            return std::nullopt;
    }
    if (!m_stepsReadElsewhere)
        findStepsReadElsewhere();
    if (m_stepsReadElsewhere->quickGet(id.index()))
        return std::nullopt;
    return edgeWithoutArguments(end);
}

std::optional<EdgeID> CodeGenerator::findEdgeIfAbsent(const ValueContext& lookup)
{
    if (isLayingOutTurns())
        return std::nullopt;
    ValueID found = lookup.id();
    BlockID id = lookup.block();
    std::span<const ValueID> behind = lookup.operationsBehind();
    auto skipProofs = [&] {
        while (!behind.empty() && opcodeOf(behind[0]) == Opcode::CheckProof && code().at(behind[0]).valueOperand() == found)
            behind = behind.subspan(1);
    };
    skipProofs();
    while (behind.empty() && code().at(id).terminator.kind == TerminatorKind::Jump) {
        const Edge& edge = code().at(code().at(id).terminator.next());
        const Block& next = code().at(edge.to);
        if (!edge.arguments().empty() || !next.parameters().empty() || next.isLoopHead || m_singlePredecessors[edge.to.index()] != id)
            return std::nullopt;
        id = edge.to;
        behind = code().valuesOf(id);
        skipProofs();
    }
    const Terminator& terminator = code().at(id).terminator;
    if (behind.size() != 1 || terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != behind[0])
        return std::nullopt;
    ValueID testID = behind[0];
    const Operation& test = code().at(testID);
    if (test.opcode != Opcode::TypeTest || test.valueOperand() != found || code().useCountOf(testID) != 1)
        return std::nullopt;

    bool isTakenIfAbsent = inputs().types.isSubset(TypeTable::undefined, test.testedType());
    EdgeID absentID = isTakenIfAbsent ? terminator.taken() : terminator.notTaken();
    const Edge* absent = &code().at(absentID);
    BlockID blockOfTest = id;
    BlockID other = code().at(isTakenIfAbsent ? terminator.notTaken() : terminator.taken()).to;
    BlockID blockOfProofs;
    auto holdsProofsAlone = [&](BlockID block) {
        std::span<const ValueID> values = code().valuesOf(block);
        return !values.empty() && code().at(block).terminator.kind == TerminatorKind::Jump && std::ranges::all_of(values, [&](ValueID value) {
            return opcodeOf(value) == Opcode::CheckProof && code().at(value).valueOperand() == found;
        });
    };
    if (absent->arguments().empty() && m_singlePredecessors[absent->to.index()] == id && holdsProofsAlone(absent->to)) {
        blockOfProofs = absent->to;
        absentID = code().at(blockOfProofs).terminator.next();
        absent = &code().at(absentID);
    }
    while (!m_blocks[absent->to.index()] && absent->arguments().empty() && code().valuesOf(absent->to).empty()) {
        const Block& next = code().at(absent->to);
        if (!next.parameters().empty() || next.terminator.kind != TerminatorKind::Jump)
            return std::nullopt;
        absentID = next.terminator.next();
        absent = &code().at(absentID);
    }
    const Block& blockIfAbsent = code().at(absent->to);
    const bool continuesLoop = blockIfAbsent.isLoopHead;
    if (continuesLoop && (blockIfAbsent.isEntry || blockIfAbsent.isHandlerBlock() || m_keepsValuesInHeapFrame || !m_stepsLeftToBackEdge.isEmpty() || !blockDominates(absent->to, lookup.block())))
        return std::nullopt;
    if (findLoopExit(absent->to) || !m_blocks[absent->to.index()] || m_singlePredecessors[other.index()] != blockOfTest || other == absent->to)
        return std::nullopt;
    if (m_keepsValuesInHeapFrame && (!absent->arguments().empty() || emitter<SavedFrameEmitter>().clearsWordsOnWayBackAlong(*absent)))
        return std::nullopt;
    if (m_keepsValuesInHeapFrame && (!emitter<SavedFrameEmitter>().liesInLoopInsideStretch(lookup.block()) || !emitter<SavedFrameEmitter>().liesInLoopInsideStretch(absent->to)))
        return std::nullopt;
    if (std::ranges::any_of(absent->arguments(), [&](ValueID passed) { return passed == found || passed == testID; }))
        return std::nullopt;

    auto isPassed = [&](const Edge& edge) { return std::ranges::contains(edge.arguments(), found); };
    for (BlockID user : order()) {
        if (user == blockOfProofs)
            continue;
        bool isRead = false;
        for (ValueID value : code().valuesOf(user))
            isRead |= value != testID && !(opcodeOf(value) == Opcode::CheckProof && (user == lookup.block() || user == blockOfTest)) && std::ranges::contains(code().at(value).operands(), found);
        isRead |= std::ranges::contains(code().at(user).terminator.operands(), found);
        if (user != blockOfTest)
            code().forEachEdgeOf(user, [&](EdgeID, const Edge& edge) { isRead |= isPassed(edge); });
        if (EdgeID handler = code().handlerOf(user))
            isRead |= isPassed(code().at(handler));
        if (isRead && !blockDominates(other, user))
            return std::nullopt;
    }
    expectOtherJumps(absent->to);
    if (continuesLoop)
        build().statistics().count(StatisticID::AbsentKeysContinueLoops);
    return absentID;
}

void CodeGenerator::jumpIfAbsent(EdgeID id)
{
    const Edge& edge = code().at(id);
    passArguments(edge, code().at(edge.to).parameters(), { });
    if (!m_listStorages.isEmpty()) [[unlikely]]
        passListStorages(edge);
    m_out.jump(BlockEdge(blockFor(edge.to), edgeClassLeadingTo(code().at(edge.to).frequency)));
}

BlockEdge CodeGenerator::edgeWithoutArguments(const Edge& edge)
{
    const Block& blockAtEnd = code().at(edge.to);
    RELEASE_ASSERT(edge.arguments().empty() && blockAtEnd.parameters().empty() && !findLoopExit(edge.to));
    return BlockEdge(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
}

Vector<BlockID, 16> CodeGenerator::collectLoopBlocks(const CountedLoop& loop) const
{
    BlockID head = m_order[loop.positionOfHead];
    Vector<BlockID, 16> found { head };
    Vector<BlockID, 16> pending;
    BitVector isFound(code().numberOfBlocks());
    isFound.quickSet(head.index());
    auto add = [&](BlockID block) {
        if (code().isReached(block) && !isFound.quickSet(block.index()))
            pending.append(block);
    };
    for (BlockID block : loop.turn)
        add(block);
    code().forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        if (!m_scopeAtEndOfBlock[edge.from.index()].block)
            add(edge.from);
    });
    while (!pending.isEmpty()) {
        BlockID block = pending.takeLast();
        found.append(block);
        code().forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            add(edge.from);
        });
    }
    std::ranges::sort(found);
    return found;
}

bool CodeGenerator::isShortRecursiveLoop(const CountedLoop& loop) const
{
    if (!code().opcodesOfValues().contains(Opcode::CallDirect))
        return false;
    bool readsMemory = std::ranges::any_of(loop.turn, [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
            return value != loop.poll && code().at(value).traits().mayTouchMemory();
        });
    });
    if (readsMemory)
        return false;
    return std::ranges::any_of(collectLoopBlocks(loop), [&](BlockID block) {
        return std::ranges::any_of(code().valuesOf(block), [&](ValueID value) {
            return opcodeOf(value) == Opcode::CallDirect && code().at(value).function() == unit().function;
        });
    });
}

void CodeGenerator::widenFactorsBeforeIterations(const CountedLoop& loop, Vector<LValue, 2>& words)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly))
        return;
    BlockID head = m_order[loop.positionOfHead];
    for (BlockID block : loop.turn) {
        for (ValueID id : code().valuesOf(block)) {
            const Operation& product = code().at(id);
            if (product.opcode != Opcode::MultiplyAddIntegers)
                continue;
            for (ValueID factor : { product.multiplicandOperand(), product.multiplierOperand() }) {
                bool isOfIteration = code().isConstant(factor) || code().blockOf(factor) == head || loop.turn.contains(code().blockOf(factor));
                if (isOfIteration || representationOf(factor) != Representation::Int32 || typeOf(factor).integers.minimum < 0)
                    continue;
                Word32 word = get(factor).as<Word32>();
                if (conversions.signExtendedIntegerOf(word))
                    continue;
                conversions.noteSignExtended(word, Integer(m_out.extendWordThatIsNotNegative(word.value())));
                words.append(word.value());
            }
        }
    }
}

LBasicBlock CodeGenerator::exitEdgeBlock(const Edge& edge)
{
    if (m_blockLeavingLoop && m_exitsOfUnrolledLoop.contains(edge.to))
        return m_blockLeavingLoop(edge);
    return m_unrolledLoopBlocks.isEmpty() || std::ranges::binary_search(m_unrolledLoopBlocks, edge.to) ? m_blockLeavingTurnLaidOut() : m_blockEndingLoop();
}

NEVER_INLINE BlockEdge CodeGenerator::edgeLeavingTurnLaidOut(const Edge& edge)
{
    bool isUsual = code().at(edge.to).frequency == BlockFrequency::Normal;
    return BlockEdge(exitEdgeBlock(edge), isUsual ? Machine::EdgeClass::Normal : Machine::EdgeClass::Rare);
}

NEVER_INLINE BlockEdge CodeGenerator::edgeThatPassesArguments(const TerminatorContext& terminator, const Edge& edge, std::span<const Operand> declaredResults)
{
    const Block& blockAtEnd = code().at(edge.to);
    BlockEdge target(blockFor(edge.to), edgeClassLeadingTo(blockAtEnd.frequency));
    std::span<const ValueID> parameters = blockAtEnd.parameters();
    if (terminator.kind() == TerminatorKind::Jump) {
        passArguments(edge, parameters, declaredResults);
        passValuesToLoopExit(edge.to);
        return target;
    }

    LBasicBlock here = m_out.m_block;
    LBasicBlock blockOnEdge = m_out.newBlock();
    m_out.appendTo(blockOnEdge);
    passArguments(edge, parameters, declaredResults);
    passValuesToLoopExit(edge.to);
    m_out.jump(target);
    m_out.appendTo(here);
    return BlockEdge(blockOnEdge, target.edgeClass());
}

void CodeGenerator::jumpTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Edge& edge = code().at(terminator->edge(targetIndex));
    if (edge.to == m_headOfTurnsLaidOut) [[unlikely]] {
        keepArgumentsForNextTurn(edge);
        m_nextBlockGoesOnHere = true;
        return;
    }
    if (leavesTurnLaidOut(edge.to)) [[unlikely]] {
        m_out.jump(BlockEdge(exitEdgeBlock(edge), edgeClassLeadingTo(code().at(edge.to).frequency)));
        return;
    }
    LBasicBlock& machineBlock = m_blocks[edge.to.index()];
    if (!machineBlock) {
        if (m_rotatingIteration) [[unlikely]]
            passToContinuation(edge);
        machineBlock = m_out.m_block;
        m_nextBlockGoesOnHere = true;
        return;
    }
    const Block& blockAtEnd = code().at(edge.to);
    passArguments(edge, blockAtEnd.parameters(), declaredResults);
    if (!m_listStorages.isEmpty()) [[unlikely]]
        passListStorages(edge);
    if (!m_loopExits.isEmpty()) [[unlikely]]
        passValuesToLoopExit(edge.to);
    m_out.jump(BlockEdge(machineBlock, edgeClassLeadingTo(blockAtEnd.frequency)));
}

bool CodeGenerator::canBeValueTableJoin(BlockID block) const
{
    if (!m_blocks[block.index()] || findLoopExit(block) || code().at(block).isHandlerBlock() || !m_stepsLeftToBackEdge.isEmpty())
        return false;
    return std::ranges::none_of(m_carriedSubstrings, [&](const CarriedSubstring& carried) {
        return carried.block == block;
    });
}

void CodeGenerator::jumpToJoin(BlockID join, std::span<const Operand> passed)
{
    const Block& blockAtJoin = code().at(join);
    std::span<const ValueID> parameters = blockAtJoin.parameters();
    RELEASE_ASSERT(passed.size() == parameters.size());
    for (size_t place = 0; place < parameters.size(); ++place) {
        if (m_values[parameters[place].index()])
            passArgument(parameters[place], passed[place]);
    }
    m_out.jump(BlockEdge(blockFor(join), edgeClassLeadingTo(blockAtJoin.frequency)));
}

void CodeGenerator::findBoxesLeftToHandlers()
{
    static constexpr uint8_t usesAreNotCounted = std::numeric_limits<uint8_t>::max();
    m_boxesLeftToHandlers.clearAll();
    UncheckedKeyHashMap<uint32_t, uint32_t> numberOfReads;
    for (BlockID id : m_order) {
        EdgeID handler = code().at(id).isInLoop ? code().handlerOf(id) : EdgeID();
        if (!handler)
            continue;
        const Edge& edge = code().at(handler);
        std::span<const ValueID> parameters = code().at(edge.to).parameters();
        for (size_t index = 0; index < parameters.size(); ++index) {
            ValueID argument = edge.arguments()[index];
            if (!argument || !code().isNeeded(parameters[index]))
                continue;
            const Operation& box = code().at(argument);
            if (box.opcode != Opcode::Box || box.representation != Representation::JSValue || !box.block() || !code().at(box.block()).isInLoop || sumBoxedBy(box))
                continue;
            Representation boxed = representationOf(box.valueOperand());
            if (numberRepresentations.contains(boxed) && !code().isConstant(box.valueOperand()) && code().useCountOf(argument) != usesAreNotCounted && ++numberOfReads.add(argument.index(), 0).iterator->value == code().useCountOf(argument)) {
                m_boxesLeftToHandlers.set(argument.index());
                m_leavesBoxesToHandlers = true;
            }
        }
    }
}

Operand CodeGenerator::boxForHandler(ValueID box)
{
    const Operation& operation = code().at(box);
    Operand made;
    SetForScope<Operand*> definitionTaken(m_definitionTaken, &made);
    generate(ValueContext(operation.block(), box, operation, nullptr));
    return made;
}

void CodeGenerator::passHandlerArguments(const Edge& handler)
{
    SetForScope<bool> whilePassing(m_isPassingHandlerArguments, true);
    passArguments(handler, code().at(handler.to).parameters(), { });
}

NEVER_INLINE void CodeGenerator::failToPassArgument(ValueID parameter, Representation found, Representation wanted)
{
    String forms = describeFormsOfGeneratedOperand(found, wanted);
    if (m_operation)
        fail(*m_operation, InternalErrorKind::UnsupportedConversion, forms);
    else
        fail(ValueContext(code().blockOf(parameter), parameter, code().at(parameter), nullptr), InternalErrorKind::UnsupportedConversion, forms);
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::passArgument(ValueID parameter, Operand argument)
{
    Representation representationOfParameter = carriedRepresentationOf(parameter);
    if (!isSameWord(argument.representation, representationOfParameter)) [[unlikely]] {
        failToPassArgument(parameter, argument.representation, representationOfParameter);
        return;
    }
    LValue phi = m_values[parameter.index()];
    if (phi && phi->opcode() != Machine::Phi) [[unlikely]]
        phi = m_keepsValuesInHeapFrame ? emitter<SavedFrameEmitter>().phiOfKeptParameter(parameter) : isCarriedSum(parameter) ? m_integersOfCarriedSums.get(parameter.index()) : nullptr;
    if (m_carriesNarrowIntegers && isCarriedNarrow(representationOfParameter, phi)) [[unlikely]]
        argument = emitter<SlotEmitter>().narrowOptionalInt32(argument.as<OptionalInteger>(), typeOf(parameter).primitives);
    RELEASE_ASSERT(phi && phi->opcode() == Machine::Phi && argument.value);
    if (argument.value->type() != phi->type()) [[unlikely]] {
        RELEASE_ASSERT(isCarriedSum(parameter));
        argument.value = isCheckedSum(parameter) ? wordOfNumberInCheckedSum(argument.value) : integerOfCarriedSum(argument.value);
    }
    m_out.upsilon(argument.value, phi);
}

void CodeGenerator::passArguments(const Edge& edge, std::span<const ValueID> parameters, std::span<const Operand> declaredResults)
{
    std::span<const ValueID> arguments = edge.arguments();
    RELEASE_ASSERT(parameters.size() == arguments.size() + declaredResults.size());
    if (m_keepsValuesInHeapFrame && !m_isPassingHandlerArguments) [[unlikely]]
        emitter<SavedFrameEmitter>().clearWordsOnWayBackAlong(edge);
    if (parameters.empty())
        return;

    if (!m_stepsLeftToBackEdge.isEmpty()) [[unlikely]] {
        for (size_t index = 0; index < arguments.size(); ++index) {
            ValueID argument = arguments[index];
            if (argument && m_values[parameters[index].index()] && m_stepsLeftToBackEdge.removeFirst(argument))
                generate(ValueContext(code().blockOf(argument), argument, code().at(argument), handlerOf(code().blockOf(argument))));
        }
    }
    Vector<Operand, 8> passed;
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_values[parameter.index()])
            passed.append(Operand());
        else if (m_leavesBoxesToHandlers && index < arguments.size() && isLeftToHandlers(arguments[index])) [[unlikely]]
            passed.append(boxForHandler(arguments[index]));
        else if (isCarriedBoxed(parameter)) [[unlikely]] {
            RELEASE_ASSERT(m_operation);
            bool isArgument = index < arguments.size();
            if (ValueID box = isArgument ? boxPassedToJoin(edge.from, arguments[index]) : ValueID()) [[unlikely]] {
                passed.append(get(box));
                continue;
            }
            Operand operand = isArgument ? get(arguments[index]) : declaredResults[index - arguments.size()];
            passed.append(emitter<ConversionEmitter>().box(*m_operation, operand, typeOf(isArgument ? arguments[index] : parameter)));
        } else if (const CarriedSubstring* carried = index < arguments.size() ? findCarriedSubstring(arguments[index]) : nullptr; carried && !findCarriedSubstring(parameter)) [[unlikely]]
            passed.append(Operand(representationOf(parameter), emitter<StringEmitter>().makeCarriedSubstring(contextOf(edge.from, carried->cut), *carried).value()));
        else if (m_carriesParametersUnboxed && index < arguments.size() && isCarriedUnboxed(parameter) && !code().isConstant(arguments[index])) [[unlikely]]
            passed.append(emitter<ConversionEmitter>().unbox(getIn(arguments[index], Representation::JSValue).as<Boxed>(), typeOf(arguments[index]), carriedRepresentationOf(parameter)));
        else if (index < arguments.size() && holdsAnyNaN(arguments[index]) && letsAnyNaNInto(parameter))
            passed.append(getWithAnyNaN(arguments[index]));
        else if (index < arguments.size())
            passed.append(getIn(arguments[index], carriedRepresentationOf(parameter)));
        else
            passed.append(declaredResults[index - arguments.size()]);
    }
    Vector<LValue, 4> lengthsOfSubstrings;
    for (const CarriedSubstring& carried : m_carriedSubstrings) {
        if (carried.block != edge.to)
            continue;
        Machine::DeclaredAccessesSuspension suspension(m_out);
        const CarriedSubstring* handedOn = findCarriedSubstring(arguments[carried.place]);
        lengthsOfSubstrings.append(handedOn ? handedOn->length : emitter<StringEmitter>().lengthOf(passed[carried.place].as<CellPointer>()).value());
    }
    size_t numberOfSubstring = 0;
    for (const CarriedSubstring& carried : m_carriedSubstrings) {
        if (carried.block != edge.to)
            continue;
        const CarriedSubstring* handedOn = findCarriedSubstring(arguments[carried.place]);
        m_out.upsilon(handedOn ? handedOn->start : m_out.constInt32(0), carried.start);
        m_out.upsilon(lengthsOfSubstrings[numberOfSubstring++], carried.length);
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (m_values[parameters[index].index()])
            passArgument(parameters[index], passed[index]);
    }
    for (const LoopCarriedTable& carried : m_loopCarriedTables) {
        if (carried.block != edge.to)
            continue;
        const ValueID table = carried.place < arguments.size() ? arguments[carried.place] : ValueID();
        auto checked = std::ranges::find(m_keysOfCheckedTables, table, &std::pair<ValueID, LValue>::first);
        LValue keys = checked != m_keysOfCheckedTables.end() ? checked->second : keysOfCarriedTable(table);
        m_out.upsilon(keys ? keys : m_out.constInt64(0), carried.keys);
    }
    for (const LoopCarriedString& carried : m_loopCarriedStrings) {
        if (carried.head == edge.to)
            emitter<StringEmitter>().passOpenedRope(carried.string, edge.from, carried.characters, carried.implFlags);
    }
    if (!m_loopCarriedNumbers.isEmpty()) [[unlikely]] {
        auto first = m_firstLoopCarriedNumbers.find(edge.to.index());
        for (size_t index = first == m_firstLoopCarriedNumbers.end() ? m_loopCarriedNumbers.size() : first->value; index < m_loopCarriedNumbers.size() && m_loopCarriedNumbers[index].head == edge.to; ++index)
            m_out.upsilon(m_out.intToDouble(passed[m_loopCarriedNumbers[index].place].as<Word32>().value()), m_loopCarriedNumbers[index].phi);
    }
}

NEVER_INLINE Operand CodeGenerator::getSlow(ValueID id, const Operation& operation)
{
    Representation representation = operation.representation;
    RELEASE_ASSERT(representation != Representation::Unchosen);
    if (representation == Representation::Void)
        return { };
    BlockID home = operation.block();
    if (!home)
        return makeConstant(id, operation);
    if (BlockID user = blockBeingGenerated(); user && user != home && home != m_onlyEntryBlock && !isProvenToDominate(home, user))
        verifyThatDefinitionDominates(id, home, user);
    if (m_keepsValuesInHeapFrame) [[unlikely]]
        verifyThatKeptValueIsNamedInThisStretch(id);
    LValue lowered = m_values[id.index()];
    if (!lowered) [[unlikely]]
        return zeroInPlaceOfValueOfFailedCode(representation);
    if (m_valuesDefinedBoxed.get(id.index())) [[unlikely]] {
        reportBoxedNumberUse(id);
        return zeroInPlaceOfValueOfFailedCode(representation);
    }
    if (m_checksSums && isCheckedSum(id)) [[unlikely]]
        lowered = numberOfCheckedWord(wordOfCheckedSum(id));
    if (representation == Representation::Double && emitter<ConversionEmitter>().isHeldAsIndex(lowered)) [[unlikely]]
        lowered = emitter<ConversionEmitter>().numberAsIndex(Number(lowered)).value();
    if (representation == Representation::Double && (m_carriesParametersBoxed || m_followsNumbersOutOfBytes)) [[unlikely]]
        return getNumberInFormKept(id, lowered);
    if (representation == Representation::OptionalDouble && holdsAnyNaN(id)) [[unlikely]]
        failAtNumberOrNothingReadForItsBits(id);
    return { representation, lowered };
}

Operand CodeGenerator::zeroInPlaceOfValueOfFailedCode(Representation representation)
{
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(representation);
}

Operand CodeGenerator::getNumberInFormKept(ValueID id, LValue lowered)
{
    if (m_carriesParametersBoxed && m_parametersCarriedBoxed.quickGet(id.index()))
        return { Representation::JSValue, lowered };
    if (m_followsNumbersOutOfBytes && m_valuesHoldingAnyNaN.quickGet(id.index()))
        return settledNaN(Number(lowered));
    return { Representation::Double, lowered };
}

void CodeGenerator::failAtNumberOrNothingReadForItsBits(ValueID id)
{
    fail(*m_operation, InternalErrorKind::UnsupportedConversion, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") is kept as its reader left it, for users that take it so, and is read by one that does not"_s));
}

void CodeGenerator::reportBoxedNumberUse(ValueID id)
{
    ASSERT_WITH_MESSAGE(!m_valuesDefinedBoxed.get(id.index()), "v%u is defined boxed for its only user, a Box", id.index());
    fail(*m_operation, InternalErrorKind::UnsupportedConversion, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") is defined boxed for its only user, a Box, and another operation reads it"_s));
}

void CodeGenerator::verifyThatKeptValueIsNamedInThisStretch(ValueID id)
{
    if (emitter<SavedFrameEmitter>().isReadAsNamedInAnotherStretch(id))
        fail(*m_operation, InternalErrorKind::InvalidSuspension, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") has a word in the machine frame and is read as another stretch named it"_s));
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::verifyThatDefinitionDominates(ValueID id, BlockID home, BlockID user)
{
    if (dominates(home, user)) [[likely]] {
        if (m_userOfHomesProven != user) {
            m_userOfHomesProven = user;
            m_homesProven.fill(BlockID());
        }
        m_homesProven[home.index() % m_homesProven.size()] = home;
        return;
    }
    if (!m_dominators)
        m_dominators.emplace(code(), order());
    if (code().isReached(home) && m_dominators->dominates(home, user))
        return;
    fail(*m_operation, InternalErrorKind::UseNotDominated, makeString('v', id.index(), " ("_s, nameOf(opcodeOf(id)), ") is used in b"_s, user.index(), ", which b"_s, home.index(), ", where it is defined, does not dominate"_s));
}

Operand CodeGenerator::getWithAnyNaN(ValueID id)
{
    if (!holdsAnyNaN(id) || m_valuesDefinedBoxed.get(id.index()))
        return get(id);
    return { Representation::Double, m_values[id.index()] };
}

Number CodeGenerator::settledNaN(Number number)
{
    return m_followsMarkers ? emitter<ConversionEmitter>().withoutPayload(number) : emitter<ConversionEmitter>().purified(number);
}

Number CodeGenerator::settleNaNOf(const ValueContext& value, Number number)
{
    if (!m_followsNumbersOutOfBytes || m_definitionTaken || m_valuesReadForBitsOfNaN.quickGet(value.id().index()))
        return settledNaN(number);
    m_valuesHoldingAnyNaN.quickSet(value.id().index());
    return number;
}

void CodeGenerator::noteThatNumberIsNoNaN(const ValueContext& value)
{
    RELEASE_ASSERT(!m_values[value.id().index()]);
    if (m_followsNumbersOutOfBytes)
        m_valuesHoldingAnyNaN.quickClear(value.id().index());
}

bool CodeGenerator::takesNothingForNaN(const ValueContext& value)
{
    bool readsNumberOrNothing = m_followsMarkers ? value.opcode() == Opcode::GetSlotOrUndefined && takesUndefinedForNaN(value.id()) : readsNumberOrNothingOutOfBytes(value.value());
    if (!readsNumberOrNothing || !m_followsNumbersOutOfBytes || m_definitionTaken || m_valuesReadForBitsOfNaN.quickGet(value.id().index()))
        return false;
    m_valuesHoldingAnyNaN.quickSet(value.id().index());
    return true;
}

bool CodeGenerator::takesUndefinedForNaN(ValueID id) const
{
    if (!m_followsMarkers || representationOf(id) != Representation::OptionalDouble || code().isConstant(id))
        return false;
    const Type& type = typeOf(id);
    return type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && type.primitives & Primitive::Undefined;
}

bool CodeGenerator::carriesAnyNaN(BlockID head) const
{
    return m_followsNumbersOutOfBytes && std::ranges::any_of(code().at(head).parameters(), [&](ValueID parameter) { return holdsAnyNaN(parameter); });
}

bool CodeGenerator::letsAnyNaNInto(ValueID parameter)
{
    if (holdsAnyNaN(parameter))
        return true;
    if (representationOf(parameter) != Representation::Double || (m_followsMarkers && isCarriedSum(parameter)) || m_valuesReadForBitsOfNaN.quickGet(parameter.index()) || m_blocksBegun.quickGet(code().blockOf(parameter).index()))
        return false;
    m_valuesHoldingAnyNaN.quickSet(parameter.index());
    return true;
}

Operand CodeGenerator::makeInWantedForm(ValueID id, const Operation& operation, Representation wanted)
{
    RELEASE_ASSERT(m_operation);
    bool canBeMadeInWantedForm = code().isConstant(id) && constantsInFormOfUses.contains(operation.opcode);
    if (canBeMadeInWantedForm) {
        Type type = typeOf(id);
        canBeMadeInWantedForm = isLegalRepresentation(type, wanted) && !(wanted == Representation::Cell32 && type.hasCells());
    }
    if (!canBeMadeInWantedForm) {
        fail(*m_operation, InternalErrorKind::UnsupportedConversion, describeFormsOfGeneratedOperand(operation.representation, wanted));
        return emitter<ConstantEmitter>().zeroOf(wanted);
    }

    Operation inWantedForm = operation;
    inWantedForm.representation = wanted;
    BlockID block = m_operation->block();
    Operand made;
    {
        SetForScope<Operand*> definitionTaken(m_definitionTaken, &made);
        generate(ValueContext(block, id, inWantedForm, handlerOf(block)));
    }
    if (made.representation == wanted)
        return made;
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(wanted);
}

ALWAYS_INLINE_INTO_OTHER_FILES Operand CodeGenerator::makeConstant(ValueID id, const Operation& constant)
{
    static constexpr OpcodeSet constantsWithoutCode { Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double };

    RELEASE_ASSERT(m_operation);
    bool isKeptForLaterUses = !m_notesForms && !constant.block() && constantsWithoutCode.contains(constant.opcode);
    if (isKeptForLaterUses) {
        if (LValue kept = m_values[id.index()])
            return { constant.representation, kept };
    }
    BlockID block = m_operation->block();
    Operand made;
    {
        SetForScope<Operand*> definitionTaken(m_definitionTaken, &made);
        generate(ValueContext(block, id, constant, handlerOf(block)));
    }
    if (made.representation == constant.representation) {
        if (isKeptForLaterUses && made.value && made.value->isConstant())
            m_values[id.index()] = made.value;
        return made;
    }
    RELEASE_ASSERT(m_hasFailed);
    return emitter<ConstantEmitter>().zeroOf(constant.representation);
}

void CodeGenerator::defineTest(const ValueContext& value, Condition result)
{
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
    if (m_resultRanges.empty())
        m_resultRanges = m_proc.arena().createSpan<ResultRange>(code().numberOfValues());
    ResultRange& range = m_resultRanges[value.id().index()];
    RELEASE_ASSERT(!range);
    range = ResultRange { static_cast<uint32_t>(m_results.size()), static_cast<uint32_t>(results.size()) };
    m_results.append(results);
}

Operand CodeGenerator::resultAt(ValueID producer, uint32_t index) const
{
    RELEASE_ASSERT(hasResults(producer.index()));
    const ResultRange& range = m_resultRanges[producer.index()];
    RELEASE_ASSERT(index < range.count);
    return m_results[range.first + index];
}

void CodeGenerator::defineParameter(ValueID parameter, Operand operand)
{
    RELEASE_ASSERT(opcodeOf(parameter) == Opcode::Parameter);
    if (m_values[parameter.index()])
        passArgument(parameter, operand);
}

ALWAYS_INLINE_INTO_OTHER_FILES void CodeGenerator::defineParameterAsPassed(ValueID parameter, Operand passed)
{
    RELEASE_ASSERT(opcodeOf(parameter) == Opcode::Parameter);
    Representation representation = representationOf(parameter);
    RELEASE_ASSERT(representation != Representation::Unchosen);
    if (!code().isNeeded(parameter) || !hasRegister(representation))
        return;
    if (!isSameWord(passed.representation, representation)) [[unlikely]] {
        failToPassArgument(parameter, passed.representation, representation);
        return;
    }
    defineValue(parameter, representation, Operand(representation, passed.value));
}

void CodeGenerator::goOnInBlockOfEntry(BlockID block)
{
    RELEASE_ASSERT(goesOnInBlockOfEntry(block) && !m_blocks[block.index()] && currentBlockIsOpen());
    m_blocks[block.index()] = m_out.m_block;
    m_nextBlockGoesOnHere = true;
}

Subject CodeGenerator::subjectOf(const OperationContext& context) const
{
    Subject subject { .function = unit().function, .location = code().locationOf(program(), context.origin()) };
    if (!hasModule(subject.location))
        subject.location = function().declarationLocation();
    if (hasModule(subject.location)) {
        subject.block = context.block();
        subject.value = context.valueID();
    }
    return subject;
}

void CodeGenerator::fail(const OperationContext& context, InternalErrorKind kind, const String& detail)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    Subject subject = subjectOf(context);
    ASCIILiteral operation = nameOfGeneratedOperation(context);
    String ending = detail;
    if (!hasModule(subject.location))
        ending = detail.isNull() ? String(operation) : makeString(operation, ": "_s, detail);
    build().diagnostics().add(InternalError { .kind = kind, .function = subject.function, .block = subject.block, .value = subject.value, .location = subject.location, .detail = ending, .operation = operation });
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

bool CodeGenerator::currentBlockIsOpen() const
{
    return !m_out.m_block->isTerminated();
}

CodeGenerator::InitializationScope::InitializationScope(CodeGenerator& generator, FillsCellOfHeap fillsCellOfHeap)
    : m_generator(generator)
{
    if (generator.m_initializationDepth++)
        return;
    generator.m_owesFenceBehindStores |= fillsCellOfHeap == FillsCellOfHeap::Yes;
    if (generator.m_proc.traits().checksAllocation)
        generator.m_out.clearDeclaredAccesses();
}

CodeGenerator::InitializationScope::~InitializationScope()
{
    if (!--m_generator.m_initializationDepth && m_generator.m_operation)
        m_generator.declareAccessesOf(*m_generator.m_operation);
}

DominatorsOfCode::DominatorsOfCode(const Code& code, std::span<const BlockID> order)
{
    constexpr uint32_t unknown = UINT32_MAX;
    struct Node {
        uint32_t dominator;
        uint32_t sizeOfSubtree;
        uint32_t nextNumberBelow;
    };
    const uint32_t numberOfBlocks = order.size();
    const uint32_t root = numberOfBlocks;
    m_sizeOfSubtree.fill(0, code.numberOfBlocks());
    m_firstNumberOfSubtree.fill(unknown, code.numberOfBlocks());
    Vector<uint32_t>& numbers = m_firstNumberOfSubtree;
    for (uint32_t number = 0; number < numberOfBlocks; ++number)
        numbers[order[number].index()] = number;
    Vector<Node, 64> nodes;
    nodes.fill(Node { unknown, 1, 0 }, numberOfBlocks + 1);
    nodes[root].dominator = root;
    for (const EntryOfCode& entry : code.entries())
        nodes[numbers[entry.block.index()]].dominator = root;

    auto handTo = [&](uint32_t number, EdgeID edge) -> bool {
        uint32_t target = numbers[code.at(edge).to.index()];
        uint32_t known = nodes[target].dominator;
        if (known == root)
            return false;
        uint32_t found = known;
        uint32_t other = number;
        if (found != unknown) {
            while (found != other) {
                if (other == root || (found != root && found > other))
                    found = nodes[found].dominator;
                else
                    other = nodes[other].dominator;
            }
        }
        if (known == other)
            return false;
        nodes[target].dominator = other;
        return target <= number;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t number = 0; number < numberOfBlocks; ++number) {
            BlockID block = order[number];
            const Terminator& end = code.at(block).terminator;
            for (uint32_t index = 0; index < end.numberOfEdges; ++index)
                changed |= handTo(number, end.edge(index));
            if (EdgeID handler = code.handlerOf(block))
                changed |= handTo(number, handler);
        }
    }

    for (uint32_t number = numberOfBlocks; number--;)
        nodes[nodes[number].dominator].sizeOfSubtree += nodes[number].sizeOfSubtree;
    for (uint32_t number = 0; number < numberOfBlocks; ++number) {
        Node& node = nodes[number];
        uint32_t& nextNumberBelowDominator = nodes[node.dominator].nextNumberBelow;
        uint32_t first = nextNumberBelowDominator;
        nextNumberBelowDominator += node.sizeOfSubtree;
        node.nextNumberBelow = first + 1;
        m_firstNumberOfSubtree[order[number].index()] = first;
        m_sizeOfSubtree[order[number].index()] = node.sizeOfSubtree;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
