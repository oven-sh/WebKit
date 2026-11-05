/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTBuiltinTable.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkTextBuilders.h"
#include "AOTWalkNumberForms.h"
#include "AOTTypeText.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned mostLooksAtOneLoop = 3;

static ValueID valueHandedOnInLoops(const BodyIndex& index, ValueID value)
{
    for (;;) {
        value = index.valueBehindCasts(value);
        ValueID tested = index.valueNarrowedInto(value);
        if (!tested)
            return value;
        value = tested;
    }
}

static Vector<bool, 8> parametersHandedRoundUnchangedInLoops(const BodyIndex& index, LoopID loop)
{
    const BodyLoop& record = index.loop(loop);
    BodyBlock head = index.block(record.head);
    Vector<bool, 8> isUnchanged;
    isUnchanged.fill(!index.isEnteredByHandlerEdge(record.head), head.numberOfParameters());
    if (isUnchanged.isEmpty())
        return isUnchanged;

    for (BlockID block : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(block).targets()) {
            if (target.block() != record.head)
                continue;
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t parameter = 0; parameter < isUnchanged.size(); ++parameter)
                isUnchanged[parameter] = isUnchanged[parameter] && parameter < arguments.size() && valueHandedOnInLoops(index, arguments[parameter]) == head.parameter(parameter);
        }
    }
    return isUnchanged;
}

static TypeID widenedInLoops(TypeArena& types, TypeID assumed, TypeID united)
{
    Type record = types.at(united);
    if (!record.isJavaScriptValue())
        return united;
    united = types.withoutBoundsOfFractions(types.withLiteralStringsWidened(united));
    if (!(record.primitives & Primitive::Integer))
        return united;
    if (record.primitives & Primitive::WholeNumber || !record.integers.isSubsetOf(Range::exact()))
        return types.withPrimitives(types.withIntegers(united, Range::full()), Primitive::WholeNumber);

    if (record.integers == Range::int32() || record.integers == Range::uint32())
        return united;

    Range before = types.at(assumed).integers;
    Range widened = record.integers;
    if (before.isEmpty() || widened.minimum < before.minimum)
        widened.minimum = Range::exact().minimum;
    if (before.isEmpty() || widened.maximum > before.maximum)
        widened.maximum = Range::exact().maximum;
    return types.withIntegers(united, widened);
}

TypeID topsOfKindsInLoops(TypeArena& types, TypeID type)
{
    Type record = types.at(type);
    if (!record.isJavaScriptValue())
        return type;
    TypeID tops = types.withLiteralStringsWidened(types.withoutObjects(type));
    if (record.primitives & Primitive::Number)
        tops = types.unionOf(tops, TypeTable::number);
    if (record.primitives & Primitive::Boolean)
        tops = types.unionOf(tops, TypeTable::boolean);
    if (record.primitives & Primitive::AnySymbol)
        tops = types.unionOf(tops, TypeTable::symbol);
    return record.hasObjects() ? types.unionOf(tops, TypeTable::object) : tops;
}

static TypeID topsOfKindsWithoutNewFractionsInLoops(TypeArena& types, TypeID type)
{
    TypeID tops = topsOfKindsInLoops(types, type);
    Type record = types.at(type);
    if (!record.isJavaScriptValue() || !(record.primitives & Primitive::Number) || record.primitives & Primitive::FractionalNumber)
        return tops;
    return types.withoutPrimitives(tops, Primitive::FractionalNumber);
}

static constexpr OpcodeSet callsTypedByResultOfCalleeInLoops { Opcode::Call, Opcode::CallDirect, Opcode::Construct, Opcode::SuperConstruct };
static constexpr OpcodeSet operationsLookedAtByTypesAloneInLoops = opcodesThatStateTypes | allNumericOperators | allRelations | allEqualities
    | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::Boolean, Opcode::Undefined, Opcode::Null, Opcode::String, Opcode::LoopHint, Opcode::LogicalNot, Opcode::ToBoolean, Opcode::CheckInitialized,
        Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::GetModuleVariable, Opcode::PutModuleVariable };
static constexpr OpcodeSet readsOutOfFreshObjectsInLoops { Opcode::PropGet, Opcode::ElemGet, Opcode::ElemHas };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);
static_assert(OperandIndex::Construct::callee == OperandIndex::Call::callee);

static bool endsInReturnInLoops(const BodyTerminator& end)
{
    return end.kind() == TerminatorKind::Return || end.kind() == TerminatorKind::ReturnCall;
}

std::optional<std::array<BodyEdgeResult, 2>> boundsProvenOnEdge(const BodyIndex& index, const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (!terminator.isConditionalBranch() || index.isParameter(terminator.operand(0)))
        return std::nullopt;
    BodyValue relation = index.value(terminator.operand(0));
    if (provesOf(relation.opcode()) != Proof::IntegerBound)
        return std::nullopt;

    std::array<BodyEdgeResult, 2> bounds;
    for (uint32_t operandIndex = 0; operandIndex < bounds.size(); ++operandIndex) {
        bounds[operandIndex].proof = Proof::IntegerBound;
        bounds[operandIndex].test = relation.id();
        bounds[operandIndex].subject = relation.operand(operandIndex);
        bounds[operandIndex].operandIndex = operandIndex;
        bounds[operandIndex].isOnTakenEdge = !targetIndex;
    }
    return bounds;
}

ValueID relationThatNarrowsBehind(const Walker& walker, const BodyTerminator& terminator)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::TypedOperations) || !boundsProvenOnEdge(index, terminator, 0))
        return { };

    BodyValue relation = index.value(terminator.operand(0));
    std::array<TypeID, 2> types;
    for (uint32_t operandIndex = 0; operandIndex < types.size(); ++operandIndex) {
        Reference built = walker.referenceOf(relation.operand(operandIndex));
        types[operandIndex] = built && built.isRecord() ? walker.code().at(built.id()).type : TypeID(TypeTable::anything);
    }
    OperandTypes operands { types };
    for (uint32_t operandIndex = 0; operandIndex < types.size(); ++operandIndex) {
        for (EdgeSide side : { EdgeSide::Taken, EdgeSide::NotTaken }) {
            if (typeOfEdgeResult(walker.typing(), relation, operands, operandIndex, side) != types[operandIndex])
                return relation.id();
        }
    }
    return { };
}

class LooksAtLoops {
    WTF_MAKE_NONCOPYABLE(LooksAtLoops);
public:
    explicit LooksAtLoops(Walker&, LoopID outermost = { });
    ~LooksAtLoops() { giveBackWhatIsKept(); }

    std::optional<LoopPreview> preview(std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround);
    TypeID widestTypeOf(const BodyValue&);

private:
    static constexpr uint32_t noBound = UINT32_MAX;
    static constexpr uint32_t boundsOfDominator = UINT32_MAX - 1;

    struct Bound {
        ValueID value;
        TypeID type;
        uint32_t next { noBound };
    };

    struct FreeValue {
        ValueID value;
        TypeID type;
    };
    struct WayOut {
        BlockID from;
        uint32_t targetIndex { 0 };
        BlockID to;
    };

    struct OpenLoop {
        BlockID head;
        Vector<TypeID, 8> typesThatCameRound;
        LoopID loop;
        uint32_t firstValue { 0 };
        bool followsFromTypesAlone { true };
        bool loopsInsideCountAsSettled { false };
        Vector<FreeValue, 4> freeValues;
        Vector<WayOut, 2> waysOut;
        Vector<bool, 8> isSteppedOnDoubles;
    };

    struct KeptOfLoop {
        Vector<BlockID, 8> blocks;
        bool isKept { false };
        bool opensGuesses { false };
        uint32_t takenInPreview { 0 };
        Vector<Vector<TypeID, 8>, 2> starts;
        Vector<TypeID, 8> settled;
        Vector<TypeID, 8> cameRound;
        Vector<bool, 8> isSteppedOnDoubles;
        Vector<FreeValue, 4> freeValues;
        Vector<WayOut, 2> waysOut;
        Effects effects { Effects::none() };
    };
    struct Kept final : Walker::KeptForBuild {
        const BodyIndex* index { nullptr };
        LoopID outermost;
        uint32_t firstValue { 0 };
        uint32_t firstBlock { 0 };
        Vector<TypeID> typesOfValues;
        Vector<uint32_t> firstBoundOfBlocks;
        Vector<Bound, 16> bounds;
        BitVector valuesWithBounds;
        Vector<KeptOfLoop> loops;
        uint32_t numberOfPreviews { 0 };
        bool frameAwaitsTypes { false };
        bool frameMayHoldOtherTypes { false };
    };
    friend void noteTypesOfLooksLieInFrame(Walker&);

    struct Stretch {
        const BodyIndex* index { nullptr };
        FunctionID function;
        LoopID outermost;
        uint32_t firstValue { 0 };
        uint32_t endOfValues { 0 };
        Vector<TypeID> typesOfValues;
        uint32_t firstBlock { 0 };
        Vector<uint32_t> firstBoundOfBlocks;
        Vector<OpenLoop, 4> openLoops;
        HashMap<uint32_t, Vector<TypeID, 8>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> settledParametersOfLoops;
        TypeID returned;
    };

    void settle(LoopID);
    void takeWhatIsKept();
    void giveBackWhatIsKept();
    KeptOfLoop& keptOf(LoopID loop) { return m_kept->loops[loop.index() - m_kept->outermost.index()]; }
    bool takeWhatIsKeptOf(LoopID, KeptOfLoop&, BodyBlock head, std::span<const TypeID> assumed);
    void keep(LoopID, KeptOfLoop&, BodyBlock head, OpenLoop&&, Vector<TypeID, 8>&& start, const Effects&, bool isLost, bool lastLookAssumedWhatIsSettled);
    void noteFreeValue(ValueID);
    void doNotKeep();
    size_t placeOfOpenLoopWithHead(BlockID) const;
    uint32_t firstBoundInheritedBy(BlockID);
    TypeID lost(TypeID);
    void loseWhatIsMadeOf(LoopID, Vector<bool, 8>& isLost);
    void forgetParametersInside(LoopID);
    void lookAtBlocksOf(LoopID);
    void lookAtTurnsWrittenOut(LoopID);
    TypeID typeReadOutOfListOfCall(const BodyValue&, OperandTypes);
    TypeID typeShownOfEarlierReadInTurn(const BodyValue& read);
    void lookAtBlock(BodyBlock);
    void lookAtOperation(const BodyValue&);
    void noteStepMadeOnDoubles(const BodyValue&, OperandTypes, TypeID result);
    void keepIntegersThatTurnsTakeAsIntegers();
    void addEffectsOf(const BodyValue&, OperandTypes, TypeID result, FunctionID* halfRunBehindLooks = nullptr);
    void lookAtOperationAsTyped(const BodyValue&);
    void followEdge(BodyTarget, const BodyTerminator*, uint32_t targetIndex);
    bool isEdgeThatNoRunTakes(const BodyTerminator&, uint32_t targetIndex);
    bool leavesOutWhatNoRunEnters() const { return m_leavesOutWhatNoRunEnters && m_calleesLookedAt.isEmpty(); }

    const BodyIndex& index() const { return *m_stretch.index; }
    bool isLookingAtCopy() const { return !m_calleesLookedAt.isEmpty(); }
    std::span<TypeID> typesOfParametersOf(BodyBlock block) { return m_stretch.typesOfValues.mutableSpan().subspan(block.firstParameter().index() - m_stretch.firstValue, block.numberOfParameters()); }
    TypeID typeOf(ValueID);
    ValueID valueBehindCasts(ValueID);
    uint32_t& firstBoundOf(BlockID block) { return m_stretch.firstBoundOfBlocks[block.index() - m_stretch.firstBlock]; }
    BlockID headOfLoopInFrontOf(BlockID) const;
    TypeID joined(TypeID held, TypeID arriving);
    OperandTypes collectOperandTypesOfOperation(const BodyValue&);
    bool collectOperandTypesOfRead(const BodyValue&);
    TypeID typeByRow(const BodyValue&, OperandTypes);
    TypeID typeOfResultAt(const BodyValue&);
    bool iteratesOverListOfCall(const BodyValue&);
    TypeID withWhatCopyReturns(const BodyValue& call, OperandTypes, TypeID byRow);
    TypeID withWhatCopyOfGetterReturns(const BodyValue& read, OperandTypes, TypeID byRow);
    TypeID withWhatCopyOfHalfRunBehindLooksReturns(const BodyValue& access, FunctionID half, OperandTypes, TypeID byRow);
    TypeID typeReturnedByCopyOf(FunctionID callee, const BodyIndex&, std::span<const TypeID> typesOfEntryParameters);
    TypeID typeLaidDownAtEntryOf(FunctionID callee, unsigned entryParameter);
    TypeID typeOfProof(const BodyTerminator&, uint32_t targetIndex, BodyTarget, uint32_t proofIndex);
    template<typename Entry, typename Holder> OperandTypes collectOperandTypes(Entry, const Holder&, Vector<TypeID, 16>&);
    void copyIntoDraft(const BodyValue&);

    Walker& m_walker;
    TypeArena& m_arena;
    const Families& m_families;
    TypingContext m_typing;
    TypeID m_anything;

    Stretch m_stretch;
    Vector<FunctionID, 4> m_calleesLookedAt;
    struct ListOfCallLookedAt {
        FunctionID callee;
        ValueID list;
        Vector<TypeID, 8> elements;
        TextOfCopyRead text;
    };
    Vector<ListOfCallLookedAt, 1> m_listsOfCallsLookedAt;
    Vector<Bound, 16> m_bounds;
    BitVector m_valuesWithBounds;
    BitVector m_blocksEntered;
    bool m_leavesOutWhatNoRunEnters { false };
    Kept* m_kept { nullptr };
    bool m_hasChangedWhatIsKept { false };
    bool m_hasMadeLooks { false };
    uint32_t m_firstValueOfInnermostLoop { 0 };
    BlockID m_blockLookedAt;
    bool m_unknownCodeRunsAround { false };
    uint32_t m_firstBound { noBound };
    Effects m_effects { Effects::none() };
    Vector<TypeID, 8> m_typesThatCameRoundOutermost;
    Vector<bool, 8> m_isLostOutermost;
    Vector<bool, 8> m_isSteppedOnDoublesOutermost;
    bool m_mentionsLetLeave { true };
    Vector<Reference, 4> m_objectsLeftAsTheyAre;
    bool m_hasLostTypes { false };
    bool m_countsOnListsTestedInFront { false };
    bool m_countsOnTurnsThatBeginAgain { false };
    bool m_nothingWasDoneInTurn { false };
    bool m_takesEffectsOfCopiesFromTheirOperations { false };
    bool m_asksOnlyWhetherTurnsRunOtherCode { false };
    bool m_hasFoundOtherCode { false };
    ValueID m_firstOperationThatRunsOtherCode;
    bool m_otherCodeRunsInLoopInside { false };
    Vector<GuessCountedOnByLooks, 4> m_guessesCountedOn;
    bool m_lastOperationMayRunOtherCode { false };
    bool m_hasLookedAtCopy { false };
    Vector<ValueID, 16> m_expectationsRemembered;
    Vector<TypeID, 16> m_operandTypes;
    Vector<TypeID, 16> m_operandTypesOfOtherOperation;
    Draft m_draft { Opcode::Undefined };
};

LooksAtLoops::LooksAtLoops(Walker& walker, LoopID outermost)
    : m_walker(walker)
    , m_arena(walker.types())
    , m_families(walker.facts().families())
    , m_typing { walker.program(), walker.types(), walker.positions(), TypeLevel::Proven }
    , m_anything(walker.types().anything(UnknownSeed::LostByWalk))
{
    m_stretch.index = &walker.currentBodyIndex();
    m_stretch.function = walker.currentFunction();
    m_stretch.outermost = outermost;
    m_stretch.firstValue = m_stretch.endOfValues = index().numberOfValues();
    if (!outermost)
        return;
    m_stretch.firstValue = index().block(index().loop(outermost).head).firstParameter().index();
    m_stretch.endOfValues = index().block(index().loop(outermost).last).endOfValues().index();
    m_stretch.typesOfValues.fill(TypeID(), m_stretch.endOfValues - m_stretch.firstValue);
    m_stretch.firstBlock = index().loop(outermost).head.index();
    m_stretch.firstBoundOfBlocks.fill(boundsOfDominator, index().loop(outermost).last.index() + 1 - m_stretch.firstBlock);
    m_leavesOutWhatNoRunEnters = walker.has(Mechanism::Folding);
    if (!walker.looksCountOnTurnsThatBeginAgain(index().loop(outermost).head))
        takeWhatIsKept();
}

void LooksAtLoops::takeWhatIsKept()
{
    LoopID outermost = m_stretch.outermost;
    std::unique_ptr<Walker::KeptForBuild>& held = m_walker.looksKeptInFrame();
    Kept* kept = static_cast<Kept*>(held.get());
    if (!kept || kept->index != &index() || !index().isInLoop(index().loop(outermost).head, kept->outermost)) {
        held = nullptr;
        if (index().loop(outermost).numberOfLoopsWithin == 1)
            return;
        uint32_t lastLoop = outermost.index();
        for (BlockID member : index().membersOf(outermost))
            lastLoop = std::max(lastLoop, index().loopOf(member).index());
        auto made = makeUniqueWithoutFastMallocCheck<Kept>();
        kept = made.get();
        kept->index = &index();
        kept->outermost = outermost;
        kept->firstValue = m_stretch.firstValue;
        kept->firstBlock = m_stretch.firstBlock;
        kept->typesOfValues = m_stretch.typesOfValues;
        kept->firstBoundOfBlocks = m_stretch.firstBoundOfBlocks;
        kept->loops.grow(lastLoop + 1 - outermost.index());
        for (BlockID member : index().membersOf(outermost)) {
            LoopID loop = index().loopOf(member);
            kept->loops[loop.index() - outermost.index()].blocks.append(member);
            if (loop != outermost && index().loop(loop).head == member)
                kept->loops[index().loop(loop).parent.index() - outermost.index()].blocks.append(member);
        }
        held = WTF::move(made);
    }
    m_kept = kept;
    ++kept->numberOfPreviews;
    kept->frameMayHoldOtherTypes |= kept->frameAwaitsTypes;
    std::ranges::copy(kept->typesOfValues.span().subspan(m_stretch.firstValue - kept->firstValue, m_stretch.typesOfValues.size()), m_stretch.typesOfValues.begin());
    std::ranges::copy(kept->firstBoundOfBlocks.span().subspan(m_stretch.firstBlock - kept->firstBlock, m_stretch.firstBoundOfBlocks.size()), m_stretch.firstBoundOfBlocks.begin());
    m_bounds = WTF::move(kept->bounds);
    m_valuesWithBounds = WTF::move(kept->valuesWithBounds);
}

void LooksAtLoops::giveBackWhatIsKept()
{
    Kept* kept = std::exchange(m_kept, nullptr);
    if (!kept)
        return;
    std::ranges::copy(m_stretch.typesOfValues, kept->typesOfValues.mutableSpan().subspan(m_stretch.firstValue - kept->firstValue).begin());
    std::ranges::copy(m_stretch.firstBoundOfBlocks, kept->firstBoundOfBlocks.mutableSpan().subspan(m_stretch.firstBlock - kept->firstBlock).begin());
    kept->bounds = WTF::move(m_bounds);
    kept->valuesWithBounds = WTF::move(m_valuesWithBounds);
    kept->frameAwaitsTypes |= m_hasMadeLooks;
    if (!m_hasChangedWhatIsKept)
        return;
    for (LoopID around = index().loop(m_stretch.outermost).parent; around && around.index() >= kept->outermost.index(); around = index().loop(around).parent)
        kept->loops[around.index() - kept->outermost.index()].isKept = false;
}

void LooksAtLoops::doNotKeep()
{
    if (m_firstValueOfInnermostLoop)
        m_stretch.openLoops.last().followsFromTypesAlone = false;
}

void LooksAtLoops::noteFreeValue(ValueID value)
{
    OpenLoop& innermost = m_stretch.openLoops.last();
    bool isListed = innermost.freeValues.containsIf([&](const FreeValue& free) {
        return free.value == value;
    });
    if (!isListed)
        innermost.freeValues.append(FreeValue { value, { } });
}

size_t LooksAtLoops::placeOfOpenLoopWithHead(BlockID block) const
{
    if (m_stretch.openLoops.isEmpty() || !index().isLoopHead(block))
        return notFound;
    uint32_t depth = index().loop(index().loopOf(block)).depth;
    uint32_t depthOfOutermost = index().loop(index().loopOf(m_stretch.openLoops[0].head)).depth;
    if (depth < depthOfOutermost || depth - depthOfOutermost >= m_stretch.openLoops.size() || m_stretch.openLoops[depth - depthOfOutermost].head != block)
        return notFound;
    return depth - depthOfOutermost;
}

bool LooksAtLoops::takeWhatIsKeptOf(LoopID loop, KeptOfLoop& kept, BodyBlock head, std::span<const TypeID> assumed)
{
    if (!kept.isKept || kept.opensGuesses != m_walker.opensGuesses())
        return false;
    bool hasBegunSo = kept.starts.containsIf([&](const Vector<TypeID, 8>& start) {
        return std::ranges::equal(start, assumed);
    });
    if (!hasBegunSo)
        return false;
    uint32_t atHead = firstBoundInheritedBy(head.id());
    m_firstBound = atHead;
    for (const FreeValue& free : kept.freeValues) {
        if (typeOf(free.value) != free.type)
            return false;
    }

    std::ranges::copy(kept.settled, typesOfParametersOf(head).begin());
    firstBoundOf(head.id()) = atHead;
    m_stretch.settledParametersOfLoops.set(loop.index(), kept.settled);
    m_effects.merge(kept.effects);
    for (const WayOut& way : kept.waysOut) {
        BodyTerminator terminator = index().terminator(way.from);
        m_blockLookedAt = way.from;
        m_firstBound = firstBoundOf(way.from);
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            if (targetIndex == way.targetIndex)
                followEdge(target, &terminator, targetIndex);
            ++targetIndex;
        }
    }
    kept.takenInPreview = m_kept->numberOfPreviews;
    if (m_stretch.openLoops.isEmpty()) {
        m_typesThatCameRoundOutermost = kept.cameRound;
        m_isLostOutermost.fill(false, head.numberOfParameters());
        m_isSteppedOnDoublesOutermost = kept.isSteppedOnDoubles;
    }
    return true;
}

void LooksAtLoops::keep(LoopID loop, KeptOfLoop& kept, BodyBlock head, OpenLoop&& closed, Vector<TypeID, 8>&& start, const Effects& effects, bool isLost, bool lastLookAssumedWhatIsSettled)
{
    OpenLoop* around = m_stretch.openLoops.isEmpty() ? nullptr : &m_stretch.openLoops.last();
    bool isKept = closed.followsFromTypesAlone && !isLost;
    if (around) {
        around->followsFromTypesAlone &= isKept;
        for (const WayOut& way : closed.waysOut) {
            if (!index().isInLoop(way.to, around->loop))
                around->waysOut.append(way);
        }
    }
    if (!isKept) {
        m_hasChangedWhatIsKept = true;
        kept.isKept = false;
        return;
    }
    m_firstBound = firstBoundInheritedBy(head.id());
    for (FreeValue& free : closed.freeValues)
        free.type = typeOf(free.value);

    std::span<const TypeID> settled = typesOfParametersOf(head);
    bool isAsBefore = kept.isKept && kept.opensGuesses == m_walker.opensGuesses() && std::ranges::equal(kept.settled, settled)
        && std::ranges::equal(kept.freeValues, closed.freeValues, [](const FreeValue& first, const FreeValue& second) {
            return first.value == second.value && first.type == second.type;
        });
    if (!isAsBefore) {
        m_hasChangedWhatIsKept = true;
        kept.starts.shrink(0);
    }
    kept.isKept = true;
    auto hasBegunWith = [&](std::span<const TypeID> types) {
        return kept.starts.containsIf([&](const Vector<TypeID, 8>& known) {
            return std::ranges::equal(known, types);
        });
    };
    if (!hasBegunWith(start.span()))
        kept.starts.append(WTF::move(start));
    if (lastLookAssumedWhatIsSettled && !hasBegunWith(settled))
        kept.starts.append(Vector<TypeID, 8>(settled));
    kept.opensGuesses = m_walker.opensGuesses();
    kept.settled = Vector<TypeID, 8>(settled);
    kept.cameRound = WTF::move(closed.typesThatCameRound);
    kept.isSteppedOnDoubles = WTF::move(closed.isSteppedOnDoubles);
    kept.freeValues = WTF::move(closed.freeValues);
    kept.waysOut = WTF::move(closed.waysOut);
    kept.effects = effects;
    UNUSED_PARAM(loop);
}

std::optional<LoopPreview> LooksAtLoops::preview(std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround)
{
    BodyBlock head = index().block(index().loop(m_stretch.outermost).head);
    auto typeThatEnters = [&](uint32_t parameter) {
        return parameter < entryTypes.size() && entryTypes[parameter] ? entryTypes[parameter] : m_anything;
    };

    std::span<TypeID> atHead = typesOfParametersOf(head);
    for (uint32_t parameter = 0; parameter < atHead.size(); ++parameter)
        atHead[parameter] = typeThatEnters(parameter);
    m_countsOnTurnsThatBeginAgain = m_walker.looksCountOnTurnsThatBeginAgain(head.id());
    m_asksOnlyWhetherTurnsRunOtherCode = m_walker.looksAskOnlyWhetherTurnsRunOtherCode(head.id());
    if (m_countsOnTurnsThatBeginAgain)
        m_walker.setExpectationsRememberedForLook(&m_expectationsRemembered);
    settle(m_stretch.outermost);
    m_walker.setLooksTakeFormsBehindLooksAsBuilt(false);
    if (m_countsOnTurnsThatBeginAgain) {
        m_walker.setExpectationsRememberedForLook(nullptr);
        for (ValueID value : m_expectationsRemembered)
            m_walker.rememberedExpectationOf(value) = { };
    }
    if (m_hasFoundOtherCode)
        return std::nullopt;

    if (m_countsOnListsTestedInFront && !leavesListsTestedInFrontOfLoopAsTheyAre(m_effects))
        return std::nullopt;
    bool turnsEndElementsOfFreshObjects = !m_effects.mayRunOtherCode() && m_effects.writes.containsAny(AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage });
    if (m_hasLostTypes || turnsEndElementsOfFreshObjects) {
        for (Reference object : m_objectsLeftAsTheyAre)
            m_walker.letLeave(object);
    }

    LoopPreview result;
    result.effects = m_effects;
    result.guessesCountedOn = WTF::move(m_guessesCountedOn);
    result.firstOperationThatRunsOtherCode = m_firstOperationThatRunsOtherCode;
    result.otherCodeRunsInLoopInside = m_otherCodeRunsInLoopInside;
    result.hasLetMentionsLeave = true;
    keepIntegersThatTurnsTakeAsIntegers();
    result.parametersSteppedOnDoubles = WTF::move(m_isSteppedOnDoublesOutermost);
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        TypeID cameRound = m_typesThatCameRoundOutermost[parameter];
        if (m_isLostOutermost[parameter])
            result.typesOfParameters.append(atHead[parameter]);
        else
            result.typesOfParameters.append(cameRound ? joined(typeThatEnters(parameter), cameRound) : typeThatEnters(parameter));
    }
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (TypeID least = m_walker.leastTypeOfHeadParameter(m_stretch.function, head.id(), parameter))
            result.typesOfParameters[parameter] = joined(result.typesOfParameters[parameter], least);
    }
    for (uint32_t parameter = 0; parameter < atHead.size() && parameter < settledAround.size(); ++parameter) {
        if (!m_arena.isSubset(result.typesOfParameters[parameter], settledAround[parameter]))
            return std::nullopt;
    }
    result.typesOfValuesLieInFrame = m_kept && !m_hasMadeLooks && !m_kept->frameMayHoldOtherTypes;
    giveBackWhatIsKept();
    result.firstValue = m_stretch.firstValue;
    result.typesOfValues = WTF::move(m_stretch.typesOfValues);
    return result;
}

TypeID LooksAtLoops::widestTypeOf(const BodyValue& operation)
{
    OperandTypes operands = collectOperandTypesOfOperation(operation);
    return withWhatCopyReturns(operation, operands, typeByRow(operation, operands));
}

void LooksAtLoops::settle(LoopID loop)
{
    BodyBlock head = index().block(index().loop(loop).head);
    uint32_t numberOfParameters = head.numberOfParameters();

    size_t depth = m_stretch.openLoops.size();
    KeptOfLoop* kept = m_kept ? &keptOf(loop) : nullptr;
    Vector<TypeID, 8> assumed = m_stretch.settledParametersOfLoops.take(loop.index());
    if (kept && assumed.isEmpty() && depth && m_stretch.openLoops[depth - 1].loopsInsideCountAsSettled && kept->isKept)
        assumed = kept->settled;
    bool wasSettled = !assumed.isEmpty();
    assumed.grow(numberOfParameters);
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        TypeID entering = typesOfParametersOf(head)[parameter];
        if (!entering)
            entering = m_anything;
        assumed[parameter] = wasSettled ? joined(assumed[parameter], entering) : entering;
        if (TypeID least = m_walker.leastTypeOfHeadParameter(m_stretch.function, head.id(), parameter))
            assumed[parameter] = joined(assumed[parameter], least);
    }

    if (kept && takeWhatIsKeptOf(loop, *kept, head, assumed.span()))
        return;

    m_hasMadeLooks = true;
    m_stretch.openLoops.append(OpenLoop { head.id(), { } });
    Vector<TypeID, 8> start = assumed;
    Effects effectsAround = Effects::none();
    bool unknownCodeRanAround = m_unknownCodeRunsAround;
    if (kept) {
        OpenLoop& open = m_stretch.openLoops[depth];
        open.loop = loop;
        open.firstValue = head.firstParameter().index();
        open.loopsInsideCountAsSettled = kept->takenInPreview == m_kept->numberOfPreviews || (depth && m_stretch.openLoops[depth - 1].loopsInsideCountAsSettled);
        m_firstValueOfInnermostLoop = open.firstValue;
        effectsAround = std::exchange(m_effects, Effects::none());
        m_unknownCodeRunsAround |= Effects::unknownCode().isSubsetOf(effectsAround);
    }
    Vector<bool, 8> isLost;
    isLost.fill(false, numberOfParameters);
    for (unsigned look = 0; look < mostLooksAtOneLoop; ++look) {
        forgetParametersInside(loop);
        m_stretch.openLoops[depth].freeValues.shrink(0);
        m_stretch.openLoops[depth].waysOut.shrink(0);
        std::ranges::copy(assumed, typesOfParametersOf(head).begin());
        m_stretch.openLoops[depth].typesThatCameRound.fill(TypeID(), numberOfParameters);
        m_nothingWasDoneInTurn = !depth && m_countsOnTurnsThatBeginAgain;
        if (!depth)
            m_guessesCountedOn.shrink(0);
        lookAtBlocksOf(loop);
        m_nothingWasDoneInTurn = false;
        if (m_hasFoundOtherCode)
            break;

        bool holds = true;
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
            TypeID cameRound = m_stretch.openLoops[depth].typesThatCameRound[parameter];
            if (!cameRound || m_arena.isSubset(cameRound, assumed[parameter]))
                continue;
            TypeID united = joined(assumed[parameter], cameRound);
            TypeID grown = look ? topsOfKindsWithoutNewFractionsInLoops(m_arena, united) : widenedInLoops(m_arena, assumed[parameter], united);
            if (grown == assumed[parameter])
                continue;
            holds = false;
            isLost[parameter] = look + 1 == mostLooksAtOneLoop;
            assumed[parameter] = grown;
        }
        if (holds)
            break;
    }
    if (isLost.contains(true)) {
        std::ranges::copy(assumed, typesOfParametersOf(head).begin());
        loseWhatIsMadeOf(loop, isLost);
        for (size_t place = 0; kept && place < m_kept->loops.size(); ++place) {
            if (!m_kept->loops[place].blocks.isEmpty() && index().isInLoop(m_kept->loops[place].blocks[0], loop))
                m_kept->loops[place].isKept = false;
        }
    }
    if (m_walker.isLoopThatMayDoAnything(m_stretch.function, head.id()))
        m_effects.merge(Effects::unknownCode());
    bool lastLookAssumedWhatIsSettled = true;
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        if (isLost[parameter])
            continue;
        TypeID cameRound = m_stretch.openLoops[depth].typesThatCameRound[parameter];
        TypeID settled = cameRound ? joined(start[parameter], cameRound) : start[parameter];
        lastLookAssumedWhatIsSettled &= settled == assumed[parameter];
        typesOfParametersOf(head)[parameter] = settled;
    }
    m_stretch.settledParametersOfLoops.set(loop.index(), Vector<TypeID, 8>(typesOfParametersOf(head)));
    if (!depth)
        m_typesThatCameRoundOutermost = m_stretch.openLoops[depth].typesThatCameRound;
    if (kept) {
        if (!depth)
            m_isSteppedOnDoublesOutermost = m_stretch.openLoops[depth].isSteppedOnDoubles;
        OpenLoop closed = WTF::move(m_stretch.openLoops[depth]);
        m_stretch.openLoops.shrink(depth);
        m_firstValueOfInnermostLoop = depth ? m_stretch.openLoops[depth - 1].firstValue : 0;
        closed.followsFromTypesAlone &= !m_unknownCodeRunsAround;
        m_unknownCodeRunsAround = unknownCodeRanAround;
        Effects effects = std::exchange(m_effects, effectsAround);
        m_effects.merge(effects);
        keep(loop, *kept, head, WTF::move(closed), WTF::move(start), effects, isLost.contains(true), lastLookAssumedWhatIsSettled);
    }
    if (!depth)
        m_isLostOutermost = WTF::move(isLost);
    m_stretch.openLoops.shrink(depth);
}

TypeID LooksAtLoops::lost(TypeID type)
{
    if (!type || (!holdsOnlyPrimitives(type) && !m_arena.at(type).isJavaScriptValue()))
        return type;
    return joinOf(m_arena, m_families, m_anything, type);
}

void LooksAtLoops::loseWhatIsMadeOf(LoopID loop, Vector<bool, 8>& isLost)
{
    const BodyLoop& record = index().loop(loop);
    BodyBlock head = index().block(record.head);
    uint32_t numberOfValues = m_stretch.endOfValues - m_stretch.firstValue;
    uint32_t placeOfAllParameters = numberOfValues;
    BitVector isMadeOfLost(numberOfValues + 1);
    auto isInStretch = [&](ValueID value) {
        return value.index() >= m_stretch.firstValue && value.index() < m_stretch.endOfValues;
    };
    auto isMarked = [&](ValueID value) {
        return isInStretch(value) && isMadeOfLost.quickGet(value.index() - m_stretch.firstValue);
    };

    struct LostWith {
        uint32_t place { 0 };
        uint32_t next { UINT32_MAX };
    };
    Vector<uint32_t> firstLostWith;
    firstLostWith.fill(UINT32_MAX, numberOfValues + 1);
    Vector<LostWith> lostWith;
    auto placeIsLostWithPlace = [&](uint32_t made, uint32_t taken) {
        lostWith.append(LostWith { made, firstLostWith[taken] });
        firstLostWith[taken] = lostWith.size() - 1;
    };
    auto isLostWith = [&](ValueID made, ValueID taken) {
        if (isInStretch(taken))
            placeIsLostWithPlace(made.index() - m_stretch.firstValue, taken.index() - m_stretch.firstValue);
    };
    for (uint32_t parameter = 0; parameter < isLost.size(); ++parameter)
        placeIsLostWithPlace(head.parameter(parameter).index() - m_stretch.firstValue, placeOfAllParameters);
    auto follow = [&](BodyTarget target, std::span<const ValueID> takenByTest) {
        if (!index().isInLoop(target.block(), loop))
            return;
        BodyBlock to = index().block(target.block());
        uint32_t parameter = 0;
        for (ValueID argument : target.arguments()) {
            if (parameter < to.numberOfParameters())
                isLostWith(to.parameter(parameter), argument);
            ++parameter;
        }
        for (; parameter < to.numberOfParameters(); ++parameter) {
            for (ValueID taken : takenByTest)
                isLostWith(to.parameter(parameter), taken);
        }
    };
    Vector<ValueID, 8> takenByTest;
    for (BlockID member : index().membersOf(loop)) {
        BodyBlock block = index().block(member);
        if (block.hasHandler())
            follow(block.handler(), { });
        for (BodyValue operation : block.values()) {
            if (resultRuleOf(operation.opcode()) == ResultRule::None)
                continue;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isLostWith(operation.id(), operand);
            });
        }
        BodyTerminator terminator = block.terminator();
        takenByTest.shrink(0);
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            takenByTest.append(operand);
        });
        if (terminator.isConditionalBranch() && !index().isParameter(terminator.operand(0))) {
            index().value(terminator.operand(0)).forEachOperand([&](ValueID operand, uint32_t) {
                takenByTest.append(operand);
            });
        }
        for (BodyTarget target : terminator.targets())
            follow(target, takenByTest.span());
        std::optional<std::array<BodyEdgeResult, 2>> bounds = boundsProvenOnEdge(index(), terminator, 0);
        for (size_t side = 0; bounds && side < bounds->size(); ++side) {
            ValueID operand = valueBehindCasts((*bounds)[side].subject);
            for (ValueID taken : takenByTest) {
                if (operand.index() >= m_stretch.firstValue)
                    isLostWith(operand, taken);
                else if (isInStretch(taken))
                    placeIsLostWithPlace(placeOfAllParameters, taken.index() - m_stretch.firstValue);
            }
        }
    }

    Vector<uint32_t> found;
    auto mark = [&](uint32_t place) {
        if (!isMadeOfLost.quickSet(place))
            found.append(place);
    };
    for (uint32_t parameter = 0; parameter < isLost.size(); ++parameter) {
        if (isLost[parameter])
            mark(head.parameter(parameter).index() - m_stretch.firstValue);
    }
    while (!found.isEmpty()) {
        for (uint32_t entry = firstLostWith[found.takeLast()]; entry != UINT32_MAX; entry = lostWith[entry].next)
            mark(lostWith[entry].place);
    }
    isMadeOfLost.quickClear(placeOfAllParameters);

    for (size_t place : isMadeOfLost)
        m_stretch.typesOfValues[place] = lost(m_stretch.typesOfValues[place]);
    for (uint32_t parameter = 0; parameter < isLost.size(); ++parameter)
        isLost[parameter] = isMarked(head.parameter(parameter));
    m_effects = Effects();
    m_hasLostTypes = true;
}

void LooksAtLoops::forgetParametersInside(LoopID loop)
{
    const BodyLoop& record = index().loop(loop);
    firstBoundOf(record.head) = boundsOfDominator;
    for (BlockID member : (m_kept ? keptOf(loop).blocks.span() : index().membersOf(loop)).subspan(1)) {
        std::ranges::fill(typesOfParametersOf(index().block(member)), TypeID());
        firstBoundOf(member) = boundsOfDominator;
        if (leavesOutWhatNoRunEnters())
            m_blocksEntered.clear(member.index());
    }
}

void LooksAtLoops::lookAtBlocksOf(LoopID loop)
{
    BlockID head = index().loop(loop).head;
    for (BlockID block : m_kept ? keptOf(loop).blocks.span() : index().membersOf(loop)) {
        if (m_hasFoundOtherCode)
            return;
        LoopID innermost = index().loopOf(block);
        if (leavesOutWhatNoRunEnters() && block != head && !m_blocksEntered.get(block.index()))
            continue;
        if (innermost == loop)
            lookAtBlock(index().block(block));
        else if (innermost && index().loop(innermost).parent == loop && index().loop(innermost).head == block)
            settle(innermost);
    }
}

BlockID LooksAtLoops::headOfLoopInFrontOf(BlockID block) const
{
    BlockID dominator = index().immediateDominator(block);
    BlockID head;
    for (LoopID loop = dominator ? index().loopOf(dominator) : LoopID(); loop && !index().isInLoop(block, loop); loop = index().loop(loop).parent)
        head = index().loop(loop).head;
    return head;
}

uint32_t LooksAtLoops::firstBoundInheritedBy(BlockID block)
{
    BlockID head = headOfLoopInFrontOf(block);
    BlockID dominator = head ? head : index().immediateDominator(block);
    uint32_t first = dominator && dominator.index() >= m_stretch.firstBlock ? firstBoundOf(dominator) : noBound;
    return first == boundsOfDominator ? noBound : first;
}

void LooksAtLoops::lookAtBlock(BodyBlock block)
{
    m_firstBound = noBound;
    m_blockLookedAt = block.id();
    bool hasHandler = block.hasHandler() && (isLookingAtCopy() || !m_walker.throwsPastCleanupOf(block));
    if (block.hasHandler())
        doNotKeep();
    if (hasHandler) {
        followEdge(block.handler(), nullptr, 0);
        m_nothingWasDoneInTurn = false;
    }

    if (m_nothingWasDoneInTurn && (!isLookingAtCopy() || m_takesEffectsOfCopiesFromTheirOperations) && block.terminator().kind() == TerminatorKind::Throw && !block.hasHandler())
        return;

    uint32_t& first = firstBoundOf(block.id());
    if (first == boundsOfDominator)
        first = firstBoundInheritedBy(block.id());
    m_firstBound = first;

    for (BodyValue operation : block.values()) {
        lookAtOperation(operation);
        if (m_asksOnlyWhetherTurnsRunOtherCode && (!isLookingAtCopy() || m_takesEffectsOfCopiesFromTheirOperations) && m_effects.mayRunOtherCode()) {
            m_hasFoundOtherCode = true;
            return;
        }
    }

    BodyTerminator terminator = block.terminator();
    if (isLookingAtCopy()) {
        if (endsInReturnInLoops(terminator)) {
            TypeID returned = terminator.operand(0) ? typeOf(terminator.operand(0)) : TypeID(TypeTable::undefined);
            m_stretch.returned = m_stretch.returned ? joined(m_stretch.returned, returned) : returned;
        }
        if (m_takesEffectsOfCopiesFromTheirOperations)
            m_effects.merge(effectsOfTerminator(m_typing, terminator, collectOperandTypes(terminator.kind(), terminator, m_operandTypes)));
    } else if (terminator.numberOfTargets() || hasHandler) {
        Effects ofTerminator = effectsOfTerminator(m_typing, terminator, collectOperandTypes(terminator.kind(), terminator, m_operandTypes));
        if (m_nothingWasDoneInTurn)
            m_nothingWasDoneInTurn = ofTerminator.mayBeRunAgainUnseen();
        m_effects.merge(ofTerminator);
    }
    uint32_t targetIndex = 0;
    const TextOfCopyRead* text = !m_listsOfCallsLookedAt.isEmpty() && m_listsOfCallsLookedAt.last().callee == m_stretch.function ? &m_listsOfCallsLookedAt.last().text : nullptr;
    for (BodyTarget target : terminator.targets()) {
        bool isNeverTaken = text && (targetIndex ? text->leadsToTakenAlone : text->leadsToNotTakenAlone).get(block.id().index());
        if (!isNeverTaken)
            followEdge(target, &terminator, targetIndex);
        ++targetIndex;
    }
}

void LooksAtLoops::lookAtOperationAsTyped(const BodyValue& operation)
{
    if (!operationsLookedAtByTypesAloneInLoops.contains(operation.opcode()))
        doNotKeep();
    bool countsOnGuesses = m_nothingWasDoneInTurn && (!isLookingAtCopy() || m_takesEffectsOfCopiesFromTheirOperations);
    m_walker.setLooksTakeFormsBehindLooksAsBuilt(countsOnGuesses);
    bool isRead = m_stretch.outermost && readsOutOfFreshObjectsInLoops.contains(operation.opcode());
    bool leavesFreshObjectAsItIs = isRead && collectOperandTypesOfRead(operation);
    OperandTypes operands = isRead ? OperandTypes { m_operandTypes.span() } : collectOperandTypesOfOperation(operation);
    TypeID byRow = typeByRow(operation, operands);
    if (TypeID element = operation.opcode() == Opcode::ElemGet ? typeReadOutOfListTestedInFrontOfLoop(m_walker, index(), operation, operands[OperandIndex::ElemGet::object], operands[OperandIndex::ElemGet::property]) : TypeID()) {
        m_countsOnListsTestedInFront = true;
        byRow = element;
    }
    if (operation.opcode() == Opcode::PropGet && m_walker.has(Mechanism::TypedOperations) && !operation.isFreeName() && operation.key() == m_walker.program().keyOf(BuiltinNames::length)) {
        TypeID holder = operands[OperandIndex::PropGet::object];
        if (holder && hasKind(m_arena, holder, OperandKind::Array))
            byRow = resultTypeOf(m_typing, Draft(Opcode::GetArrayLength), OperandTypes { std::span<const TypeID>(&holder, 1) });
        else if (TypeID text = holder && holdsOnlyPrimitives(holder) ? m_arena.withoutNullish(holder) : TypeID(); text && !isNeverType(text) && m_arena.at(text).isAnyString())
            byRow = resultTypeOf(m_typing, Draft(Opcode::GetStringLength), OperandTypes { std::span<const TypeID>(&text, 1) });
    }
    if (operation.opcode() == Opcode::PropGet && m_walker.has(Mechanism::TypedOperations) && !operation.isFreeName() && !isLookingAtCopy() && operation.key() == m_walker.program().keyOf(BuiltinNames::length) && readsOffEntryParameterThatStaysAsMade(m_walker, operation)) {
        TypeID holder = operands[OperandIndex::PropGet::object];
        const ClassPart* onlyClass = holder && m_arena.at(holder).isObject() ? singleBuiltinClassOf(m_arena, m_arena.at(holder)) : nullptr;
        if (onlyClass && isTypedView(m_walker.program().builtins().typedArrayTypeOf(onlyClass->builtinClass)))
            byRow = resultTypeOf(m_typing, Draft(Opcode::GetTypedArrayLength), OperandTypes { std::span<const TypeID>(&holder, 1) });
    }
    if (TypeID known = !m_listsOfCallsLookedAt.isEmpty() ? typeReadOutOfListOfCall(operation, operands) : TypeID())
        byRow = known;
    if (TypeID shown = countsOnGuesses ? typeShownOfEarlierReadInTurn(operation) : TypeID())
        byRow = shown;
    if (countsOnGuesses && operation.opcode() == Opcode::PropGet && !operation.isFreeName()) {
        if (TypeID function = typeThatLookAtCalledSlotWouldProve(m_walker, operation, operands[OperandIndex::PropGet::object]))
            byRow = function;
    }
    noteStepMadeOnDoubles(operation, operands, byRow);
    if (leavesFreshObjectAsItIs)
        m_effects.merge(declaredBoundOf(Opcode::GetArrayElement));
    else if (!m_countsOnTurnsThatBeginAgain) {
        if (!isLookingAtCopy()) {
            bool ranOtherCode = m_firstOperationThatRunsOtherCode || m_effects.mayRunOtherCode();
            addEffectsOf(operation, operands, byRow);
            if (!ranOtherCode && m_effects.mayRunOtherCode()) {
                m_firstOperationThatRunsOtherCode = operation.id();
                m_otherCodeRunsInLoopInside = m_stretch.openLoops.size() > 1;
            }
        }
    } else if (!isLookingAtCopy() || m_takesEffectsOfCopiesFromTheirOperations) {
        Effects ofOperationsInFront = std::exchange(m_effects, Effects::none());
        FunctionID halfRunBehindLooks;
        addEffectsOf(operation, operands, byRow, &halfRunBehindLooks);
        Effects ofOperation = std::exchange(m_effects, Effects::none());
        bool copyWasLookedAtBefore = std::exchange(m_hasLookedAtCopy, false);
        TypeID given;
        {
            SetForScope fromOperations(m_takesEffectsOfCopiesFromTheirOperations, m_takesEffectsOfCopiesFromTheirOperations || countsOnGuesses);
            given = halfRunBehindLooks ? withWhatCopyOfHalfRunBehindLooksReturns(operation, halfRunBehindLooks, operands, byRow) : withWhatCopyReturns(operation, operands, byRow);
        }
        if (m_hasLookedAtCopy && (m_takesEffectsOfCopiesFromTheirOperations || countsOnGuesses) && (halfRunBehindLooks || callsTypedByResultOfCalleeInLoops.contains(operation.opcode())))
            ofOperation = m_effects;
        m_hasLookedAtCopy = copyWasLookedAtBefore;
        m_nothingWasDoneInTurn &= ofOperation.mayBeRunAgainUnseen();
        m_lastOperationMayRunOtherCode = ofOperation.mayRunOtherCode();
        m_effects = ofOperationsInFront;
        m_effects.merge(ofOperation);
        m_stretch.typesOfValues[operation.id().index() - m_stretch.firstValue] = given;
        return;
    }
    m_stretch.typesOfValues[operation.id().index() - m_stretch.firstValue] = withWhatCopyReturns(operation, operands, byRow);
}

TypeID LooksAtLoops::typeShownOfEarlierReadInTurn(const BodyValue& read)
{
    bool readsByName = read.opcode() == Opcode::PropGet && !read.isFreeName();
    if (read.opcode() != Opcode::GetEnvironmentField && read.opcode() != Opcode::GetModuleVariable && !readsByName)
        return { };
    for (uint32_t place = m_firstBound; place != noBound; place = m_bounds[place].next) {
        ValueID earlier = m_bounds[place].value;
        while (earlier && !index().isParameter(earlier) && index().opcodeOf(earlier) == Opcode::CheckInitialized)
            earlier = valueBehindCasts(index().value(earlier).operand(0));
        if (!earlier || earlier.index() < m_stretch.firstValue || earlier >= read.id() || index().isParameter(earlier) || index().opcodeOf(earlier) != read.opcode())
            continue;
        BodyValue first = index().value(earlier);
        bool readsSamePlace = read.opcode() == Opcode::GetModuleVariable ? first.variable() == read.variable()
            : readsByName ? !first.isFreeName() && first.key() == read.key() && valueBehindCasts(first.objectOperand()) == valueBehindCasts(read.objectOperand())
            : first.layout() == read.layout() && first.field() == read.field() && valueBehindCasts(first.environmentOperand()) == valueBehindCasts(read.environmentOperand());
        if (readsSamePlace)
            return m_bounds[place].type;
    }
    return { };
}

TypeID LooksAtLoops::typeReadOutOfListOfCall(const BodyValue& read, OperandTypes operands)
{
    const ListOfCallLookedAt& known = m_listsOfCallsLookedAt.last();
    if (known.callee != m_stretch.function || !known.list)
        return { };
    if (read.opcode() == Opcode::PropGet) {
        if (read.isFreeName() || read.key() != m_walker.program().keyOf(BuiltinNames::length) || valueBehindCasts(read.operand(OperandIndex::PropGet::object)) != known.list)
            return { };
        return m_arena.integer(Range::one(static_cast<int64_t>(known.elements.size())));
    }
    if (read.opcode() != Opcode::ElemGet || valueBehindCasts(read.operand(OperandIndex::ElemGet::object)) != known.list)
        return { };
    TypeID key = operands[OperandIndex::ElemGet::property];
    if (!key || !m_arena.at(key).isInteger() || known.elements.isEmpty())
        return { };
    Range positions = m_arena.at(key).integers;
    if (!positions.isSubsetOf(Range { 0, static_cast<int64_t>(known.elements.size()) - 1 }))
        return { };
    TypeID element;
    for (int64_t position = positions.minimum; position <= positions.maximum; ++position)
        element = element ? joined(element, known.elements[position]) : known.elements[position];
    return element;
}

void LooksAtLoops::lookAtTurnsWrittenOut(LoopID loop)
{
    BodyBlock head = index().block(index().loop(loop).head);
    uint32_t numberOfParameters = head.numberOfParameters();
    uint32_t stays = m_listsOfCallsLookedAt.last().text.turnsOfLoops[loop.index()];
    Vector<TypeID> inSomeTurn;
    inSomeTurn.fill(TypeID(), m_stretch.typesOfValues.size());
    auto noteTypesOf = [&](BodyBlock block) {
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value) {
            TypeID type = m_stretch.typesOfValues[value - m_stretch.firstValue];
            TypeID& held = inSomeTurn[value - m_stretch.firstValue];
            if (type)
                held = held ? joined(held, type) : type;
        }
    };

    size_t depth = m_stretch.openLoops.size();
    m_stretch.openLoops.append(OpenLoop { head.id(), { } });
    for (uint32_t turn = 0; turn < stays; ++turn) {
        forgetParametersInside(loop);
        m_stretch.openLoops[depth].typesThatCameRound.fill(TypeID(), numberOfParameters);
        lookAtBlocksOf(loop);
        for (BlockID member : index().membersOf(loop))
            noteTypesOf(index().block(member));
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
            TypeID cameRound = m_stretch.openLoops[depth].typesThatCameRound[parameter];
            typesOfParametersOf(head)[parameter] = cameRound ? cameRound : m_anything;
        }
    }
    forgetParametersInside(loop);
    m_stretch.openLoops[depth].typesThatCameRound.fill(TypeID(), numberOfParameters);
    lookAtBlock(head);
    noteTypesOf(head);
    m_stretch.openLoops.shrink(depth);

    for (BlockID member : index().membersOf(loop)) {
        BodyBlock block = index().block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            m_stretch.typesOfValues[value - m_stretch.firstValue] = inSomeTurn[value - m_stretch.firstValue];
    }
}

void LooksAtLoops::noteStepMadeOnDoubles(const BodyValue& step, OperandTypes operands, TypeID result)
{
    constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    if (!steps.contains(step.opcode()) || !m_stretch.outermost || isLookingAtCopy() || !result || !operands[0])
        return;
    Type made = m_arena.at(result);
    if (!made.isInteger() || (made.integers.maximum < maxExactInteger && made.integers.minimum > -maxExactInteger))
        return;
    uint32_t numberOfOperands = numberOfNamedOperandsOf(step.opcode());
    if (numberOfOperands == 2 && !operands[1])
        return;
    if (chooseArithmeticForm(m_walker.program(), m_arena, step.opcode(), OperatorTypes { operands[0], numberOfOperands == 2 ? operands[1] : TypeID() }, result) != OperatorForm::CheckedIntegers)
        return;
    BodyBlock head = index().block(index().loop(m_stretch.outermost).head);
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID stepped = valueBehindCasts(step.operand(operandIndex));
        if (!index().isParameter(stepped))
            continue;
        if (m_kept) {
            size_t place = placeOfOpenLoopWithHead(index().blockOf(stepped));
            if (place == notFound)
                continue;
            BodyBlock headOfStepped = index().block(m_stretch.openLoops[place].head);
            if (m_stretch.openLoops[place].isSteppedOnDoubles.isEmpty())
                m_stretch.openLoops[place].isSteppedOnDoubles.fill(false, headOfStepped.numberOfParameters());
            m_stretch.openLoops[place].isSteppedOnDoubles[stepped.index() - headOfStepped.firstParameter().index()] = true;
            for (size_t inside = place + 1; inside < m_stretch.openLoops.size(); ++inside)
                m_stretch.openLoops[inside].followsFromTypesAlone = false;
            continue;
        }
        if (index().blockOf(stepped) != head.id())
            continue;
        if (m_isSteppedOnDoublesOutermost.isEmpty())
            m_isSteppedOnDoublesOutermost.fill(false, head.numberOfParameters());
        m_isSteppedOnDoublesOutermost[stepped.index() - head.firstParameter().index()] = true;
    }
}

void LooksAtLoops::keepIntegersThatTurnsTakeAsIntegers()
{
    if (m_isSteppedOnDoublesOutermost.isEmpty())
        return;
    BodyBlock head = index().block(index().loop(m_stretch.outermost).head);
    if (head.numberOfParameters() > 32) {
        m_isSteppedOnDoublesOutermost.clear();
        return;
    }

    constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    Vector<uint32_t> madeOf;
    madeOf.fill(0, m_stretch.endOfValues - m_stretch.firstValue);
    auto placeOf = [&](ValueID value) {
        return value.index() - m_stretch.firstValue;
    };
    auto partsOf = [&](ValueID value) -> uint32_t {
        return value && value.index() >= m_stretch.firstValue && value.index() < m_stretch.endOfValues ? madeOf[placeOf(value)] : 0;
    };
    auto isIntegerInLooks = [&](ValueID value) {
        TypeID type;
        if (value.index() >= m_stretch.firstValue)
            type = value.index() < m_stretch.endOfValues ? m_stretch.typesOfValues[placeOf(value)] : TypeID();
        else if (Reference reference = m_walker.referenceOf(value); reference && reference.isRecord())
            type = m_walker.code().at(reference.id()).type;
        return type && m_arena.at(type).isInteger();
    };
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (m_isSteppedOnDoublesOutermost[parameter])
            madeOf[placeOf(head.parameter(parameter))] = 1u << parameter;
    }

    uint32_t takenAsIntegers = 0;
    for (BlockID member : index().membersOf(m_stretch.outermost)) {
        BodyBlock block = index().block(member);
        for (BodyValue operation : block.values()) {
            uint32_t parts = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                parts |= partsOf(operand);
            });
            if (!parts)
                continue;
            Opcode opcode = operation.opcode();
            TypeID result = m_stretch.typesOfValues[placeOf(operation.id())];
            if (opcodesThatStateTypes.contains(opcode) || (steps.contains(opcode) && result && m_arena.at(result).isNumber()))
                madeOf[placeOf(operation.id())] = parts;
            else if (!allRelations.contains(opcode) || (isIntegerInLooks(operation.operand(0)) && isIntegerInLooks(operation.operand(1))))
                takenAsIntegers |= parts;
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            takenAsIntegers |= partsOf(operand);
        });
        for (BodyTarget target : end.targets()) {
            bool isJoinBehind = target.block().index() > member.index() && index().isInLoop(target.block(), m_stretch.outermost) && !index().isLoopHead(target.block());
            BodyBlock to = index().block(target.block());
            uint32_t place = 0;
            for (ValueID argument : target.arguments()) {
                uint32_t parts = partsOf(argument);
                if (parts && isJoinBehind && place < to.numberOfParameters())
                    madeOf[placeOf(to.parameter(place))] |= parts;
                else if (parts && target.block() != head.id() && index().isInLoop(target.block(), m_stretch.outermost))
                    takenAsIntegers |= parts;
                ++place;
            }
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                takenAsIntegers |= partsOf(argument);
        }
    }
    for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter) {
        if (takenAsIntegers & (1u << parameter))
            m_isSteppedOnDoublesOutermost[parameter] = false;
    }
}

void LooksAtLoops::addEffectsOf(const BodyValue& operation, OperandTypes operands, TypeID result, FunctionID* halfRunBehindLooks)
{
    Opcode opcode = operation.opcode();
    if (familyOf(opcode) == OperationFamily::Iteration && iteratesOverListOfCall(operation))
        return;
    if (familyOf(opcode) == OperationFamily::Iteration && isPartOfIterationOverListLiteral(m_walker, index(), operation))
        return;
    if (std::optional<Effects> ofStep = opcode == Opcode::IteratorResume && !isLookingAtCopy() ? effectsOfStepOverListAhead(m_walker, index(), operation, m_countsOnListsTestedInFront) : std::nullopt) {
        m_effects.merge(*ofStep);
        return;
    }
    if (opcode == Opcode::Builtin && firstReadOfNamedBuiltinIsSeen(m_walker, operation))
        m_effects.merge(declaredBoundOf(Opcode::InitializeBuiltin));
    if (opcode == Opcode::ForInStep)
        m_effects.merge(declaredBoundOf(Opcode::PutHeapFrameWord));
    if (opcode == Opcode::GetModuleVariable && isVariableFilledAtFirstRead(m_walker.plan(), operation.variable())) {
        Draft store(Opcode::PutModuleVariable);
        store.setVariable(operation.variable());
        std::array<TypeID, 1> stored { m_anything };
        m_effects.merge(effectsOf(m_typing, store, OperandTypes { std::span<const TypeID>(stored) }, { }, nullptr));
        m_effects.merge(declaredBoundOf(m_walker.plan().deferredDefinitions().findDeferred(operation.variable())->opcode));
    }
    if (m_unknownCodeRunsAround || Effects::unknownCode().isSubsetOf(m_effects)) {
        if (maySuspendAsWritten(operation))
            m_effects.maySuspend = true;
        return;
    }

    Effects effectsOfCallee;
    const Effects* foundByLink = nullptr;
    if (effectRuleOf(opcode) == EffectRule::Call) {
        const LinkTables& linked = m_walker.facts().linked();
        FunctionID callee = linked.calleeAt(m_stretch.function, operation.id());
        if (!callee && m_nothingWasDoneInTurn && (opcode == Opcode::Call || opcode == Opcode::Construct) && operands[OperandIndex::Call::callee] && m_arena.at(operands[OperandIndex::Call::callee]).isObject())
            callee = singleFunctionOf(m_arena, m_arena.at(operands[OperandIndex::Call::callee]));
        if (callee && callee.index() < linked.functions.size()) {
            effectsOfCallee = linked.function(callee).effectsOnReturn;
            effectsOfCallee.merge(linked.function(callee).effectsOnThrow);
            foundByLink = &effectsOfCallee;
        }
    }
    auto typeOfInput = [&](ValueID input) {
        return typeOf(input);
    };
    std::optional<Effects> ofTypedForm = effectRuleOf(opcode) == EffectRule::Access ? effectsOfTypedFormAhead(m_walker, operation, TypesOfInputs(typeOfInput), halfRunBehindLooks) : std::nullopt;
    if (opcode == Opcode::Call && !foundByLink && m_walker.has(Mechanism::BuiltinCalls) && result && m_arena.at(result).hasOnlyPrimitives(Primitive::Number | Primitive::Boolean)) {
        bool takesPrimitivesAlone = true;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operandIndex != OperandIndex::Call::callee)
                takesPrimitivesAlone &= m_arena.at(typeOf(operand)).isJavaScriptValue() && !m_arena.at(typeOf(operand)).hasObjects();
        });
        if (takesPrimitivesAlone)
            ofTypedForm = effectsOfCallByOneRowAhead(m_walker, operation, TypesOfInputs(typeOfInput));
    }
    Effects effects = effectsOf(m_typing, operation, operands, result, ofTypedForm ? &*ofTypedForm : foundByLink);
    effects.maySuspend &= maySuspendAsWritten(operation);
    if (operationsThatJoinTexts.contains(opcode) && result && m_arena.at(result).isAnyString()) {
        doNotKeep();
        if (mayKeepTextsInStringBuilders(m_walker))
            effects.merge(declaredBoundOf(Opcode::StringBuilderAppendValue));
    }
    m_effects.merge(effects);
}

void LooksAtLoops::lookAtOperation(const BodyValue& operation)
{
    if (!m_nothingWasDoneInTurn || (isLookingAtCopy() && !m_takesEffectsOfCopiesFromTheirOperations)) {
        lookAtOperationAsTyped(operation);
        return;
    }
    Effects ofOperationsInFront = m_effects;
    m_lastOperationMayRunOtherCode = false;
    lookAtOperationAsTyped(operation);
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    bool isOperator = operators.contains(operation.opcode());
    if (!m_lastOperationMayRunOtherCode && !isOperator)
        return;

    auto typeOfInput = [&](ValueID input) {
        return typeOf(input);
    };
    struct Left {
        ValueID operand;
        TypeID expected;
        TypeID proven;
    };
    Vector<Left, 2> leftByTests;
    forEachTypeThatGuessWouldProveAt(m_walker, operation, TypesOfInputs(typeOfInput), scopedLambda<void(ValueID, TypeID, TypeID)>([&](ValueID operand, TypeID expected, TypeID proven) {
        leftByTests.append({ operand, expected, proven });
    }));
    if (leftByTests.isEmpty())
        return;
    auto isClosed = [&] {
        return m_lastOperationMayRunOtherCode || (isOperator && typedFormOf(m_walker, operation, TypesOfInputs(typeOfInput)) == TypedForm::None);
    };
    if (!isClosed())
        return;

    uint32_t firstBound = m_firstBound;
    size_t numberOfGuesses = m_guessesCountedOn.size();
    for (const Left& left : leftByTests) {
        ValueID behindCasts = valueBehindCasts(left.operand);
        m_bounds.append(Bound { behindCasts, left.proven, m_firstBound });
        m_valuesWithBounds.set(behindCasts.index());
        m_firstBound = m_bounds.size() - 1;
        if (!isLookingAtCopy())
            m_guessesCountedOn.append(GuessCountedOnByLooks { operation.id(), left.operand, left.expected });
    }
    firstBoundOf(m_blockLookedAt) = m_firstBound;
    Effects withOperationAsItStands = std::exchange(m_effects, ofOperationsInFront);
    m_nothingWasDoneInTurn = true;
    lookAtOperationAsTyped(operation);
    if (!isClosed())
        return;
    m_firstBound = firstBound;
    firstBoundOf(m_blockLookedAt) = firstBound;
    m_guessesCountedOn.shrink(numberOfGuesses);
    m_effects.merge(withOperationAsItStands);
    lookAtOperationAsTyped(operation);
}

bool LooksAtLoops::isEdgeThatNoRunTakes(const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (!leavesOutWhatNoRunEnters() || terminator.kind() != TerminatorKind::Branch)
        return false;
    ValueID asked = terminator.operand(0);
    if (!index().isParameter(asked) && index().opcodeOf(asked) == Opcode::ToBoolean)
        asked = index().value(asked).operand(0);
    asked = index().valueBehindCasts(asked);
    if (index().isParameter(asked) || index().opcodeOf(asked) != Opcode::Call)
        return false;
    BodyValue call = index().value(asked);
    ValueID callee = call.calleeOperand();
    if (!callee || index().isParameter(callee) || index().opcodeOf(callee) != Opcode::Builtin || index().value(callee).as<Opcode::Builtin>().builtin() != Builtins::ArrayIsArray)
        return false;
    if (call.argumentsOperands().size() != 1 || valueHandedOnInLoops(index(), call.argumentsOperands()[0]).index() >= m_stretch.firstValue)
        return false;
    TypeID condition = typeOf(terminator.operand(0));
    return condition == TypeArena::fromPrimitives(targetIndex ? Primitive::True : Primitive::False);
}

void LooksAtLoops::followEdge(BodyTarget target, const BodyTerminator* terminator, uint32_t targetIndex)
{
    if (terminator && isEdgeThatNoRunTakes(*terminator, targetIndex))
        return;
    if (leavesOutWhatNoRunEnters())
        m_blocksEntered.set(target.block().index());
    if (m_firstValueOfInnermostLoop && !index().isInLoop(target.block(), m_stretch.openLoops.last().loop))
        m_stretch.openLoops.last().waysOut.append(WayOut { m_blockLookedAt, targetIndex, target.block() });

    std::span<TypeID> held;
    size_t open = placeOfOpenLoopWithHead(target.block());
    if (open != notFound)
        held = m_stretch.openLoops[open].typesThatCameRound.mutableSpan();
    else if (!m_stretch.outermost || index().isInLoop(target.block(), m_stretch.outermost))
        held = typesOfParametersOf(index().block(target.block()));
    else
        return;

    std::array<ValueID, 2> operandsOfRelation;
    std::array<TypeID, 2> leftByRelation;
    if (std::optional<std::array<BodyEdgeResult, 2>> bounds = terminator && m_walker.has(Mechanism::TypedOperations) ? boundsProvenOnEdge(index(), *terminator, targetIndex) : std::nullopt) {
        BodyValue relation = index().value((*bounds)[0].test);
        OperandTypes operands = collectOperandTypes(relation.opcode(), relation, m_operandTypesOfOtherOperation);
        for (const BodyEdgeResult& bound : *bounds) {
            TypeID left = typeOfEdgeResult(m_typing, relation, operands, bound.operandIndex, bound.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
            if (left == operands[bound.operandIndex])
                continue;
            operandsOfRelation[bound.operandIndex] = valueBehindCasts(bound.subject);
            leftByRelation[bound.operandIndex] = left;
        }
    }
    auto typeOnEdge = [&](ValueID value) {
        if (leftByRelation[0] || leftByRelation[1]) {
            ValueID behindCasts = valueBehindCasts(value);
            for (size_t side = 0; side < leftByRelation.size(); ++side) {
                if (leftByRelation[side] && operandsOfRelation[side] == behindCasts)
                    return leftByRelation[side];
            }
        }
        return typeOf(value);
    };

    size_t parameter = 0;
    auto arrive = [&](TypeID type) {
        if (parameter < held.size())
            held[parameter] = held[parameter] ? joined(held[parameter], type) : type;
        ++parameter;
    };
    for (ValueID argument : target.arguments())
        arrive(typeOnEdge(argument));
    for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex)
        arrive(terminator ? typeOfProof(*terminator, targetIndex, target, proofIndex) : m_anything);
    while (parameter < held.size())
        arrive(m_anything);

    if (open != notFound || !terminator || index().numberOfIncomingEdges(target.block()) != 1 || index().isLoopHead(target.block()))
        return;
    uint32_t& first = firstBoundOf(target.block());
    if (headOfLoopInFrontOf(target.block())) {
        first = boundsOfDominator;
        return;
    }
    first = m_firstBound;
    for (size_t side = 0; side < leftByRelation.size(); ++side) {
        if (!leftByRelation[side])
            continue;
        m_bounds.append(Bound { operandsOfRelation[side], leftByRelation[side], first });
        m_valuesWithBounds.set(operandsOfRelation[side].index());
        first = m_bounds.size() - 1;
    }
}

TypeID LooksAtLoops::typeOf(ValueID value)
{
    if (value.index() < m_firstValueOfInnermostLoop)
        noteFreeValue(value);
    if (m_firstBound != noBound) {
        ValueID behindCasts = valueBehindCasts(value);
        for (uint32_t place = m_valuesWithBounds.get(behindCasts.index()) ? m_firstBound : noBound; place != noBound; place = m_bounds[place].next) {
            if (m_bounds[place].value == behindCasts)
                return m_bounds[place].type;
        }
    }

    TypeID type;
    if (value.index() < m_stretch.firstValue) {
        if (Reference reference = m_walker.referenceOf(value); reference && reference.isRecord()) {
            if (m_stretch.outermost && m_mentionsLetLeave)
                m_walker.letLeave(reference);
            type = m_walker.code().at(reference.id()).type;
            if (TypeID seenByLooks = typeOfRecordForLooks(m_walker, type); seenByLooks != type) {
                doNotKeep();
                m_effects.merge(declaredBoundOf(Opcode::StringBuilderToString));
                type = seenByLooks;
            }
        }
    } else if (value.index() < m_stretch.endOfValues)
        type = m_stretch.typesOfValues[value.index() - m_stretch.firstValue];
    return type ? type : m_anything;
}

ValueID LooksAtLoops::valueBehindCasts(ValueID value)
{
    while (!index().isParameter(value)) {
        BodyValue definition = index().value(value);
        if (!opcodesThatStateTypes.contains(definition.opcode()) || !definition.operand(0))
            break;
        if (definition.opcode() == Opcode::Convert && definition.as<Opcode::Convert>().declared().has(PositionFlags::IsClaim))
            break;
        value = definition.operand(0);
    }
    return value;
}

TypeID LooksAtLoops::joined(TypeID held, TypeID arriving)
{
    if (held == arriving)
        return held;
    TypeID united = joinOf(m_arena, m_families, held, arriving);
    if (!holdsOnlyPrimitives(held) && !m_arena.at(held).isJavaScriptValue() && m_arena.at(united).isJavaScriptValue())
        return held;
    return united;
}

OperandTypes LooksAtLoops::collectOperandTypesOfOperation(const BodyValue& operation)
{
    OperandTypes operands = collectOperandTypes(operation.opcode(), operation, m_operandTypes);
    if (operation.opcode() == Opcode::NewObject && operands[OperandIndex::NewObject::prototype]) {
        ValueID read = valueBehindCasts(operation.operand(OperandIndex::NewObject::prototype));
        TypeID newTarget = !index().isParameter(read) && index().opcodeOf(read) == Opcode::GetConstructPrototype ? typeOf(index().value(read).as<Opcode::GetConstructPrototype>().newTargetOperand()) : TypeID();
        m_operandTypes[OperandIndex::NewObject::prototype] = newTarget ? newTarget : TypeID(TypeTable::object);
    }
    return operands;
}

bool LooksAtLoops::collectOperandTypesOfRead(const BodyValue& read)
{
    {
        SetForScope withoutLeaving(m_mentionsLetLeave, false);
        collectOperandTypes(read.opcode(), read, m_operandTypes);
    }
    TypeID typeOfKey = read.opcode() == Opcode::PropGet ? TypeID() : m_operandTypes[OperandIndex::ElemGet::property];
    bool leavesFreshObjectAsItIs = false;
    read.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        Reference reference = operand.index() < m_stretch.firstValue ? m_walker.referenceOf(operand) : Reference();
        if (!reference || !reference.isRecord())
            return;
        if (readLeavesFreshObjectAsItIs(m_walker, read, operandIndex, reference, typeOfKey)) {
            leavesFreshObjectAsItIs = true;
            m_objectsLeftAsTheyAre.append(reference);
        } else
            m_walker.letLeave(reference);
    });
    return leavesFreshObjectAsItIs;
}

TypeID LooksAtLoops::typeByRow(const BodyValue& operation, OperandTypes operands)
{
    Opcode opcode = operation.opcode();
    if (opcode == Opcode::NarrowByTest)
        return operands[OperandIndex::NarrowByTest::left];
    if (opcodesThatStateTypes.contains(opcode))
        return operands[0];
    if (resultRuleOf(opcode) == ResultRule::None)
        return { };
    if (opcode == Opcode::ResultAt)
        return typeOfResultAt(operation);

    copyIntoDraft(operation);
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(m_typing.program, m_draft))
        return TypeTable::undefined;
    if (opcode != Opcode::ElemGet)
        return resultTypeOf(m_typing, m_draft, operands);
    TypeID before = m_walker.exchangeResultOfReadBeingTyped(m_walker.facts().linked().resultOfReadByKey(m_stretch.function, operation.id()));
    TypeID type = resultTypeOf(m_typing, m_draft, operands);
    m_walker.exchangeResultOfReadBeingTyped(before);
    return type;
}

bool LooksAtLoops::iteratesOverListOfCall(const BodyValue& operation)
{
    if (m_listsOfCallsLookedAt.isEmpty() || m_listsOfCallsLookedAt.last().callee != m_stretch.function || !m_listsOfCallsLookedAt.last().list)
        return false;
    ValueID open = operation.opcode() == Opcode::IteratorOpen ? operation.id() : operation.operand(0);
    if (!open || index().isParameter(open) || index().opcodeOf(open) != Opcode::IteratorOpen)
        return false;
    return valueBehindCasts(index().value(open).operand(0)) == m_listsOfCallsLookedAt.last().list;
}

TypeID LooksAtLoops::typeOfResultAt(const BodyValue& operation)
{
    auto resultAt = operation.as<Opcode::ResultAt>();
    ValueID results = resultAt.resultsOperand();
    if (!results || index().isParameter(results))
        return m_anything;

    BodyValue producer = index().value(results);
    if (producer.opcode() == Opcode::IteratorResume && resultAt.index() == static_cast<uint32_t>(IteratorResumeResult::Value)) {
        if (iteratesOverListOfCall(producer)) {
            const Vector<TypeID, 8>& elements = m_listsOfCallsLookedAt.last().elements;
            std::optional<uint32_t> stepIndex = producer.as<Opcode::IteratorResume>().stepIndex();
            if (stepIndex)
                return *stepIndex < elements.size() ? elements[*stepIndex] : TypeID(TypeTable::undefined);
            TypeID element;
            for (TypeID listed : elements)
                element = element ? joined(element, listed) : listed;
            return element ? element : TypeID(TypeTable::undefined);
        }
        if (std::optional<StepOfListLiteral> step = stepOfListLiteral(m_walker, index(), producer))
            return step->element ? typeOf(step->element) : TypeID(TypeTable::undefined);
        auto typeOfInput = [&](ValueID input) {
            SetForScope withoutLeaving(m_mentionsLetLeave, false);
            return typeOf(input);
        };
        if (TypeID unit = isLookingAtCopy() ? TypeID() : typeOfStepOverTextAhead(m_walker, index(), producer, TypesOfInputs(typeOfInput)))
            return unit;
        if (TypeID element = isLookingAtCopy() ? TypeID() : typeOfStepOverListAhead(m_walker, index(), producer, m_countsOnListsTestedInFront))
            return element;
    }
    OperandTypes operands = collectOperandTypes(producer.opcode(), producer, m_operandTypesOfOtherOperation);
    copyIntoDraft(producer);
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_typing.program, m_draft);
    if (!numberOfResults || resultAt.index() >= *numberOfResults)
        return m_anything;
    return resultTypeOf(m_typing, m_draft, operands, { }, resultAt.index());
}

TypeID LooksAtLoops::withWhatCopyReturns(const BodyValue& call, OperandTypes operands, TypeID byRow)
{
    Opcode opcode = call.opcode();
    if (opcode == Opcode::PropGet || opcode == Opcode::SuperGet)
        return withWhatCopyOfGetterReturns(call, operands, byRow);
    if (!callsTypedByResultOfCalleeInLoops.contains(opcode) || !byRow || (m_arena.at(byRow).isAnything() && !m_takesEffectsOfCopiesFromTheirOperations))
        return byRow;

    if (opcode == Opcode::Call) {
        auto typeOfInput = [&](ValueID input) {
            return typeOf(input);
        };
        if (TypeID quotient = typeOfCallOnNumbersAhead(m_walker, index(), call, TypesOfInputs(typeOfInput)))
            return quotient;
        if (TypeID inside = typeOfReadInsideTextAhead(m_walker, index(), call, TypesOfInputs(typeOfInput)))
            return inside;
        if (TypeID answer = typeOfIsArrayAhead(m_walker, index(), call, TypesOfInputs(typeOfInput)))
            return answer;
    }

    if (FunctionID entered = opcode == Opcode::Call ? findFunctionBehindCompiledBuiltin(m_walker, operands[OperandIndex::Call::callee]) : FunctionID()) {
        Draft direct(Opcode::CallDirect);
        direct.setFunction(entered);
        direct.setRole(m_walker.plan().conventions().directEntryOf(entered).role);
        return joined(byRow, resultTypeOf(m_typing, direct, OperandTypes { }));
    }

    FunctionID callee;
    if (opcode == Opcode::CallDirect)
        callee = m_draft.function();
    else if (opcode == Opcode::Call) {
        callee = onlyCalleeOf(m_typing, operands[OperandIndex::Call::callee]);
        if (!callee)
            callee = findLibraryFunctionBehindBuiltin(m_walker, operands[OperandIndex::Call::callee]);
        if (FunctionID behindBuiltin = findLibraryFunctionBehindBuiltin(m_walker, operands[OperandIndex::Call::callee])) {
            Draft direct(Opcode::CallDirect);
            direct.setFunction(behindBuiltin);
            direct.setRole(m_walker.plan().conventions().directEntryOf(behindBuiltin).role);
            byRow = joined(byRow, resultTypeOf(m_typing, direct, OperandTypes { }));
        } else if (!callee) {
            LibraryFunctionsBehindBuiltin behind = findLibraryFunctionsBehindBuiltin(m_walker, operands[OperandIndex::Call::callee]);
            PlaceOfCall placeOfCopy { m_stretch.function, isLookingAtCopy() || m_stretch.outermost || m_walker.isInLoop(), &index(), call.id() };
            for (FunctionID named : behind.named) {
                Draft direct(Opcode::CallDirect);
                direct.setFunction(named);
                direct.setRole(m_walker.plan().conventions().directEntryOf(named).role);
                byRow = joined(byRow, resultTypeOf(m_typing, direct, OperandTypes { }));
                if ((behind.enteredByValue && named != behind.enteredByValue) || m_calleesLookedAt.contains(named) || !mayCopyCallee(m_walker, m_draft, named, placeOfCopy))
                    continue;
                if (behind.enteredByValue && (isLookingAtCopy() || m_walker.depthOfCopies() || call.argumentsOperands().empty() || !mayBeLeafHandedToMethodOfLists(m_walker, typeOf(call.argumentsOperands()[0]))))
                    continue;
                m_nothingWasDoneInTurn = false;
                return m_anything;
            }
        }
    }
    else if (TypeID constructor = operands[opcode == Opcode::SuperConstruct ? OperandIndex::SuperConstruct::parent : OperandIndex::Construct::callee]; constructor && m_arena.at(constructor).isObject())
        callee = singleFunctionOf(m_arena, m_arena.at(constructor));
    PlaceOfCall place { m_stretch.function, isLookingAtCopy() || m_stretch.outermost || m_walker.isInLoop(), &index(), call.id() };
    if (!callee || m_calleesLookedAt.contains(callee) || !mayCopyCallee(m_walker, m_draft, callee, place))
        return byRow;

    const BodyIndex& body = m_walker.facts().bodies().at(callee);
    std::optional<TurnsOfCopyWrittenOut> turns = body.hasLoops() ? turnsOfCopyWrittenOutAt(m_walker, m_draft, callee, place) : std::nullopt;
    if (body.hasLoops() && !turns) {
        m_nothingWasDoneInTurn = false;
        return m_anything;
    }

    const Function& function = m_walker.program().at(callee);
    Vector<Reference, 8> handedOver = operandsHandedToCopy(m_draft, function);
    if (handedOver.size() != function.numberOfEntryParameters())
        return byRow;
    Vector<TypeID, 8> typesOfEntryParameters;
    for (unsigned entryParameter = 0; entryParameter < handedOver.size(); ++entryParameter)
        typesOfEntryParameters.append(handedOver[entryParameter] ? typeOf(handedOver[entryParameter].id()) : typeLaidDownAtEntryOf(callee, entryParameter));

    if (turns) {
        ListOfCallLookedAt list;
        list.callee = callee;
        if (turns->entryParameterOfList) {
            list.list = body.block(entryBlock).parameter(*turns->entryParameterOfList);
            for (Reference argument : m_draft.argumentsOperands().subspan(turns->firstArgumentListed))
                list.elements.append(typeOf(argument.id()));
        }
        list.text = WTF::move(turns->text);
        m_listsOfCallsLookedAt.append(WTF::move(list));
    }
    TypeID returned = typeReturnedByCopyOf(callee, body, typesOfEntryParameters.span());
    if (turns)
        m_listsOfCallsLookedAt.removeLast();
    if (returned && m_takesEffectsOfCopiesFromTheirOperations && m_hasLookedAtCopy)
        return returned;
    return returned ? joined(byRow, returned) : byRow;
}

TypeID LooksAtLoops::withWhatCopyOfGetterReturns(const BodyValue& read, OperandTypes operands, TypeID byRow)
{
    if (!byRow || m_arena.at(byRow).isAnything())
        return byRow;
    bool isThroughSuper = read.opcode() == Opcode::SuperGet;
    PropertyKeyID key = isThroughSuper ? read.as<Opcode::SuperGet>().key() : read.as<Opcode::PropGet>().key();
    FunctionID getter = findGetterOfRead(m_walker, read.opcode(), operands[isThroughSuper ? OperandIndex::SuperGet::base : OperandIndex::PropGet::object], key);
    if (!getter || m_calleesLookedAt.contains(getter))
        return byRow;

    const Function& function = m_walker.program().at(getter);
    Vector<Reference, 8> handedOver;
    handedOver.fill(Reference(), function.numberOfEntryParameters());
    Draft site(Opcode::CallDirect);
    site.setFunction(getter);
    site.setVariadicOperands(handedOver.span());
    PlaceOfCall place { m_stretch.function, isLookingAtCopy() || m_stretch.outermost || m_walker.isInLoop() };
    if (!mayCopyCallee(m_walker, site, getter, place))
        return byRow;
    const BodyIndex& body = m_walker.facts().bodies().at(getter);
    if (body.hasLoops())
        return m_anything;

    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    TypeID receiver = operands[isThroughSuper ? OperandIndex::SuperGet::receiver : OperandIndex::PropGet::object];
    Vector<TypeID, 8> typesOfEntryParameters;
    for (unsigned entryParameter = 0; entryParameter < function.numberOfEntryParameters(); ++entryParameter)
        typesOfEntryParameters.append(thisParameter == entryParameter ? receiver : typeLaidDownAtEntryOf(getter, entryParameter));
    TypeID returned = typeReturnedByCopyOf(getter, body, typesOfEntryParameters.span());
    return returned ? joined(byRow, returned) : byRow;
}

TypeID LooksAtLoops::withWhatCopyOfHalfRunBehindLooksReturns(const BodyValue& access, FunctionID half, OperandTypes operands, TypeID byRow)
{
    if (m_calleesLookedAt.contains(half))
        return byRow;
    const Function& function = m_walker.program().at(half);
    Vector<Reference, 8> handedOver;
    handedOver.fill(Reference(), function.numberOfEntryParameters());
    Draft site(Opcode::CallDirect);
    site.setFunction(half);
    site.setIsNeverInlined(function.isHiddenInStackTraces());
    site.setVariadicOperands(handedOver.span());
    PlaceOfCall place { m_stretch.function, isLookingAtCopy() || m_stretch.outermost || m_walker.isInLoop() };
    if (!mayCopyCallee(m_walker, site, half, place))
        return byRow;
    const BodyIndex& body = m_walker.facts().bodies().at(half);
    if (body.hasLoops())
        return byRow;

    static_assert(OperandIndex::PropGet::object == OperandIndex::PropSet::object);
    bool isStore = access.opcode() == Opcode::PropSet;
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    std::optional<unsigned> parameterOfStored = isStore && function.numberOfPositionalParameters() ? std::optional(function.firstParameterIndex()) : std::nullopt;
    Vector<TypeID, 8> typesOfEntryParameters;
    for (unsigned entryParameter = 0; entryParameter < function.numberOfEntryParameters(); ++entryParameter) {
        if (thisParameter == entryParameter)
            typesOfEntryParameters.append(operands[OperandIndex::PropGet::object]);
        else if (parameterOfStored == entryParameter)
            typesOfEntryParameters.append(operands[OperandIndex::PropSet::value]);
        else
            typesOfEntryParameters.append(typeLaidDownAtEntryOf(half, entryParameter));
    }
    TypeID returned = typeReturnedByCopyOf(half, body, typesOfEntryParameters.span());
    return !isStore && byRow && returned ? joined(byRow, returned) : byRow;
}

TypeID LooksAtLoops::typeReturnedByCopyOf(FunctionID callee, const BodyIndex& body, std::span<const TypeID> typesOfEntryParameters)
{
    Stretch copy;
    copy.index = &body;
    copy.function = callee;
    copy.endOfValues = body.numberOfValues();
    copy.typesOfValues.fill(TypeID(), body.numberOfValues());
    copy.firstBoundOfBlocks.fill(boundsOfDominator, body.numberOfBlocks());
    Stretch caller = std::exchange(m_stretch, WTF::move(copy));
    uint32_t firstBoundAtCall = m_firstBound;
    BlockID blockLookedAtAtCall = m_blockLookedAt;
    uint32_t firstValueOfInnermostLoopAtCall = std::exchange(m_firstValueOfInnermostLoop, 0);
    Kept* keptAtCall = std::exchange(m_kept, nullptr);
    m_calleesLookedAt.append(callee);
    bool laysFrame = m_takesEffectsOfCopiesFromTheirOperations && m_walker.layFrameForLookIntoCopy(callee, body);
    SetForScope fromOperations(m_takesEffectsOfCopiesFromTheirOperations, laysFrame);
    m_hasLookedAtCopy = laysFrame;
    size_t numberRememberedOfCaller = m_expectationsRemembered.size();

    std::ranges::copy(typesOfEntryParameters, typesOfParametersOf(body.block(entryBlock)).begin());
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFoundOtherCode; ++number) {
        bool isPassedOver = !m_listsOfCallsLookedAt.isEmpty() && m_listsOfCallsLookedAt.last().callee == callee && !m_listsOfCallsLookedAt.last().text.isEntered.get(number);
        if (!body.isReachable(BlockID(number)) || isPassedOver)
            continue;
        LoopID loop = body.loopOf(BlockID(number));
        if (!loop)
            lookAtBlock(body.block(BlockID(number)));
        else if (body.loop(loop).head == BlockID(number))
            lookAtTurnsWrittenOut(loop);
    }

    m_calleesLookedAt.removeLast();
    if (laysFrame) {
        m_walker.takeAwayFrameForLookIntoCopy();
        m_expectationsRemembered.shrink(numberRememberedOfCaller);
    }
    m_blockLookedAt = blockLookedAtAtCall;
    m_firstBound = firstBoundAtCall;
    m_firstValueOfInnermostLoop = firstValueOfInnermostLoopAtCall;
    m_kept = keptAtCall;
    return std::exchange(m_stretch, WTF::move(caller)).returned;
}

TypeID LooksAtLoops::typeLaidDownAtEntryOf(FunctionID callee, unsigned entryParameter)
{
    TypingContext withoutFacts { m_typing.program, m_arena, PositionTypes::knowingNothing(), TypeLevel::Proven };
    const DeclaredType* declared = m_typing.program.at(callee).declaredEntryParameter(entryParameter);
    TypeID type = declared ? m_typing.program.provenTypeOf(*declared) : declaredEntryTypeOf(withoutFacts, callee, entryParameter);
    return type ? type : m_anything;
}

TypeID LooksAtLoops::typeOfProof(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, uint32_t proofIndex)
{
    std::optional<BodyEdgeResult> result = index().edgeResult(terminator, targetIndex, target, proofIndex);
    if (!result)
        return m_anything;

    TypeID subject = typeOf(result->subject);
    TypeID narrowed;
    if (!result->test)
        narrowed = typeOfCaseResult(m_typing, terminator, subject, result->caseIndex.value_or(targetIndex));
    else if (!index().isParameter(result->test)) {
        BodyValue test = index().value(result->test);
        OperandTypes operands = collectOperandTypes(test.opcode(), test, m_operandTypesOfOtherOperation);
        narrowed = typeOfEdgeResult(m_typing, test, operands, result->operandIndex, result->isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
    }
    if (!narrowed)
        return subject;
    bool isTestOfType = result->test && !index().isParameter(result->test) && index().opcodeOf(result->test) == Opcode::TypeTest;
    if (isTestOfType && isLookAtCellInFrontOfIsArray(m_walker, index(), index().value(result->test)) && !cellAnswersWhetherValueIsArray(m_walker, subject))
        return subject;
    if (isTestOfType && !isLookingAtCopy() && m_walker.has(Mechanism::TypedOperations) && m_arena.isSubset(narrowed, subject))
        return narrowed;
    return joined(subject, narrowed);
}

template<typename Entry, typename Holder>
OperandTypes LooksAtLoops::collectOperandTypes(Entry entry, const Holder& holder, Vector<TypeID, 16>& types)
{
    types.fill(TypeID(), numberOfNamedOperandsOf(entry) + holder.variadicOperands().size());
    holder.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        types[operandIndex] = typeOf(operand);
    });
    return { types.span() };
}

void LooksAtLoops::copyIntoDraft(const BodyValue& operation)
{
    m_draft.become(operation.opcode());
    m_draft.flags = static_cast<uint8_t>(operation.flags());
    copyFields(operation, m_draft, [](ValueID operand) {
        return Reference::value(operand);
    });
}

static LoopPreview previewThatClaimsNothingOfLoops(Walker& walker, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    Vector<bool, 8> isUnchanged;
    if (index.isLoopHead(inputHead))
        isUnchanged = parametersHandedRoundUnchangedInLoops(index, index.loopOf(inputHead));

    LoopPreview preview;
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        TypeID entering = parameter < entryTypes.size() ? entryTypes[parameter] : TypeID();
        bool staysAsItEnters = entering && ((parameter < isUnchanged.size() && isUnchanged[parameter]) || !types.at(entering).isJavaScriptValue());
        if (staysAsItEnters)
            preview.typesOfParameters.append(entering);
        else
            preview.typesOfParameters.append(parameter < settledAround.size() ? settledAround[parameter] : types.anything(UnknownSeed::LostByWalk));
        if (TypeID least = walker.leastTypeOfHeadParameter(walker.currentFunction(), inputHead, parameter))
            preview.typesOfParameters.last() = types.unionOf(preview.typesOfParameters.last(), least);
    }
    return preview;
}

LoopPreview previewLoop(Walker& walker, BlockID inputHead, std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.hasIrreducibleFlow() && index.isLoopHead(inputHead)) {
        if (std::optional<LoopPreview> preview = LooksAtLoops(walker, index.loopOf(inputHead)).preview(entryTypes, settledAround))
            return WTF::move(*preview);
    }
    return previewThatClaimsNothingOfLoops(walker, inputHead, entryTypes, settledAround);
}

void noteTypesOfLooksLieInFrame(Walker& walker)
{
    if (auto* kept = static_cast<LooksAtLoops::Kept*>(walker.looksKeptInFrame().get()))
        kept->frameAwaitsTypes = false;
}

TypeID widestTypeOfOperation(Walker& walker, const BodyValue& operation)
{
    return LooksAtLoops(walker).widestTypeOf(operation);
}

TypeID Walker::leastTypeOfHeadParameter(FunctionID function, BlockID inputHead, uint32_t parameter) const
{
    for (const LoopOfWalkGivenUp& loop : m_loopsOfWalksGivenUp) {
        if (loop.function == function && loop.inputHead == inputHead)
            return parameter < loop.leastTypes.size() ? loop.leastTypes[parameter] : TypeID();
    }
    return { };
}

bool Walker::isLoopThatMayDoAnything(FunctionID function, BlockID inputHead) const
{
    return std::ranges::any_of(m_loopsOfWalksGivenUp, [&](const LoopOfWalkGivenUp& loop) {
        return loop.mayDoAnything && loop.function == function && loop.inputHead == inputHead;
    });
}

Walker::LoopOfWalkGivenUp& Walker::loopOfWalkGivenUp(FunctionID function, BlockID inputHead)
{
    for (LoopOfWalkGivenUp& loop : m_loopsOfWalksGivenUp) {
        if (loop.function == function && loop.inputHead == inputHead)
            return loop;
    }
    m_loopsOfWalksGivenUp.append(LoopOfWalkGivenUp { function, inputHead, false, { }, { } });
    return m_loopsOfWalksGivenUp.last();
}

void Walker::giveUpWalkOfUnit()
{
    m_walksUnitAgain = true;
    m_hasFailed = true;
    endAsUnreachable(m_origin);
}

bool Walker::walkUnitAgainWithWiderHead(BlockID inputHead, uint32_t parameter, TypeID previewed, TypeID found)
{
    if (m_hasFailed || m_inputs.options.verifiesCode || !m_types.at(previewed).isJavaScriptValue() || !m_types.at(found).isJavaScriptValue())
        return false;
    LoopOfWalkGivenUp& loop = loopOfWalkGivenUp(currentFunction(), inputHead);
    while (loop.leastTypes.size() <= parameter) {
        loop.leastTypes.append(TypeID());
        loop.numbersOfWalksGivenUp.append(0);
    }
    TypeID united = m_types.unionOf(previewed, found);
    TypeID wider;
    switch (loop.numbersOfWalksGivenUp[parameter]) {
    case 0:
        wider = widenedInLoops(m_types, previewed, united);
        break;
    case 1:
        wider = topsOfKindsInLoops(m_types, united);
        break;
    default:
        wider = m_types.unionOf(m_types.anything(UnknownSeed::LostByWalk), united);
        break;
    }
    if (m_types.isSubset(wider, previewed))
        return false;
    if (loop.numbersOfWalksGivenUp[parameter] < 2)
        ++loop.numbersOfWalksGivenUp[parameter];
    loop.leastTypes[parameter] = wider;
    dataLogLn("AOT: f", unit().function.index(), " ", nameOf(unit().role), " is walked again: parameter ", parameter, " of the head b", inputHead.index(), " of f", currentFunction().index(), " was previewed as ", TypeInText { m_types, previewed }, ", the walk brings ", TypeInText { m_types, found }, ", and it now holds ", TypeInText { m_types, wider });
    giveUpWalkOfUnit();
    return true;
}

bool Walker::walkUnitAgainWithWiderHeadOfCode(BlockID head, size_t parameter, TypeID previewed, TypeID found)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = currentBodyIndex();
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID inputHead(number);
        if (!index.isReachable(inputHead) || !index.isLoopHead(inputHead) || inputBlock(frameIndex, inputHead).head != head)
            continue;
        return parameter < index.block(inputHead).numberOfParameters() && walkUnitAgainWithWiderHead(inputHead, static_cast<uint32_t>(parameter), previewed, found);
    }
    return false;
}

bool Walker::looksAtLoopsAroundHaveCounted(const Draft& draft, const Effects& effects)
{
    if (looksAtLoopsAroundHaveSeen(effects))
        return true;
    const LinkTables& linked = facts().linked();
    if (effectRuleOf(draft.opcode) != EffectRule::Call || !draft.inputValueGiven || linked.calls.isEmpty())
        return false;
    FunctionID callee = linked.calleeAt(currentFunction(), draft.inputValueGiven);
    if (!callee)
        return false;
    Effects ofCallee = linked.function(callee).effectsOnReturn;
    ofCallee.merge(linked.function(callee).effectsOnThrow);
    return looksAtLoopsAroundHaveSeen(effectsOf(typing(), draft, typesOfOperands(draft), { }, &ofCallee));
}

bool Walker::walkUnitAgainWithLoopsThatMayDoAnything()
{
    if (m_hasFailed || m_inputs.options.verifiesCode)
        return false;
    bool isNews = false;
    for (const OpenLoop& open : m_openLoops) {
        if (open.effects.mayRunOtherCode())
            continue;
        LoopOfWalkGivenUp& loop = loopOfWalkGivenUp(m_frames[open.frame].function, m_frames[open.frame].index->loop(open.loop).head);
        isNews |= !std::exchange(loop.mayDoAnything, true);
    }
    if (!isNews)
        return false;
    dataLogLn("AOT: f", unit().function.index(), " ", nameOf(unit().role), " is walked again: in f", currentFunction().index(), ", where a ", nameOf(m_opcodeBeingWalked), " is written in a loop, something is built that does what the looks have not counted on, and the loops around it now count on any code");
    giveUpWalkOfUnit();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
