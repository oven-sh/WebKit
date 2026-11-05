/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// previewLoop() is never withheld.
// At a suspension: the row of what suspends declares any code, so the effects of the preview write everything.
// At a throw: in a look an edge to a handler is an edge like any other.

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

// By parameter of the head: every edge that comes round hands the parameter itself, through casts and tests at most.
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

// `united`: what was assumed in a look, joined with what came round and lay outside it.
// The bounds of fractions and a list of texts may move with every turn, so they go. A range runs on the side on which it grew to the end of what a double holds exactly, which a step of one stays within.
// A comparison in the loop cuts it, and the head gets what then came round, not what was assumed: `i` that enters as 0 and is stepped behind `i < n` with an int32 is 0 to the largest int32.
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

    // `| 0` and `>>> 0` give all of int32 and all of uint32 and no more, whatever they take: `h = (h * 31 + c) | 0`. What passes through a loop inside unchanged would keep a wider range for good.
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

// Every number, every text, both booleans, every symbol, any object.
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

static constexpr OpcodeSet callsTypedByResultOfCalleeInLoops { Opcode::Call, Opcode::CallDirect, Opcode::Construct, Opcode::SuperConstruct };
// What a look finds of such an operation follows from the types of its operands, from the program and from Link. Of any other the walk is asked what it would build, and that may hang on where the walk stands.
static constexpr OpcodeSet operationsLookedAtByTypesAloneInLoops = opcodesThatStateTypes | allNumericOperators | allRelations | allEqualities
    | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::Boolean, Opcode::Undefined, Opcode::Null, Opcode::String, Opcode::LoopHint, Opcode::LogicalNot, Opcode::ToBoolean, Opcode::CheckInitialized,
        Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::GetModuleVariable, Opcode::PutModuleVariable };
// What readLeavesFreshObjectAsItIs() speaks of.
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
    // No loop: each value of the body has the type of what the walk has built for it, and nothing is looked at but a callee.
    explicit LooksAtLoops(Walker&, LoopID outermost = { });
    ~LooksAtLoops() { giveBackWhatIsKept(); }

    // Nothing: the invariant that the looks found does not lie within `settledAround`.
    std::optional<LoopPreview> preview(std::span<const TypeID> entryTypes, std::span<const TypeID> settledAround);
    // Null: the operation has no result.
    TypeID widestTypeOf(const BodyValue&);

private:
    static constexpr uint32_t noBound = UINT32_MAX;
    static constexpr uint32_t boundsOfDominator = UINT32_MAX - 1;

    // As Walker::Frame::NarrowerName: what a relation leaves of an operand on the one edge that enters a block holds in all that the block dominates.
    struct Bound {
        // Behind its casts.
        ValueID value;
        TypeID type;
        uint32_t next { noBound };
    };

    // A value from outside a loop that something in the loop names, and the type under which the head of the loop sees it.
    struct FreeValue {
        ValueID value;
        TypeID type;
    };
    // An edge that leaves a loop.
    struct WayOut {
        BlockID from;
        uint32_t targetIndex { 0 };
        BlockID to;
    };

    struct OpenLoop {
        BlockID head;
        // By parameter of the head. Null: no edge has come round.
        Vector<TypeID, 8> typesThatCameRound;
        // The rest: where m_kept.
        LoopID loop;
        uint32_t firstValue { 0 };
        // All that the looks have found in the loop follows from what the head was assumed to hold and from the types of the free values.
        bool followsFromTypesAlone { true };
        // A look at this loop that was not made in this preview, because what it would have found was kept, would have settled the loops inside.
        bool loopsInsideCountAsSettled { false };
        // Of the look being made.
        Vector<FreeValue, 4> freeValues;
        Vector<WayOut, 2> waysOut;
        // By parameter of the head. Empty: none.
        Vector<bool, 8> isSteppedOnDoubles;
    };

    // What settle() has left of a loop. The types of its values, and the bounds of its blocks, lie where the looks have written them.
    struct KeptOfLoop {
        // The blocks of the loop itself and the heads of the loops right inside it, in the order of the stream. Its own head comes first.
        Vector<BlockID, 8> blocks;
        bool isKept { false };
        bool opensGuesses { false };
        // The number of the last preview that took what is kept in the place of looks.
        uint32_t takenInPreview { 0 };
        // What the head was assumed to hold when settle() began, each time that it ended as below.
        Vector<Vector<TypeID, 8>, 2> starts;
        Vector<TypeID, 8> settled;
        Vector<TypeID, 8> cameRound;
        Vector<bool, 8> isSteppedOnDoubles;
        Vector<FreeValue, 4> freeValues;
        Vector<WayOut, 2> waysOut;
        Effects effects { Effects::none() };
    };
    // A look at a loop looks at the loops inside it, and the walk asks for looks at each of them again when it comes to its head. Where all that a look finds in a loop follows from types alone,
    // the same types give the same once more: it is kept, so that a loop is looked at as often as something new arrives at it, however many loops stand around it.
    struct Kept final : Walker::KeptForBuild {
        const BodyIndex* index { nullptr };
        LoopID outermost;
        uint32_t firstValue { 0 };
        uint32_t firstBlock { 0 };
        Vector<TypeID> typesOfValues;
        Vector<uint32_t> firstBoundOfBlocks;
        Vector<Bound, 16> bounds;
        BitVector valuesWithBounds;
        // By loop, less the outermost.
        Vector<KeptOfLoop> loops;
        uint32_t numberOfPreviews { 0 };
        // A preview has made looks, and the walk has not said yet that it has laid their types into the frame.
        bool frameAwaitsTypes { false };
        // It never said so of some preview.
        bool frameMayHoldOtherTypes { false };
    };
    friend void noteTypesOfLooksLieInFrame(Walker&);

    // What is looked at: a loop of the body that the walk reads, or the whole body of a callee that the walk may copy.
    struct Stretch {
        const BodyIndex* index { nullptr };
        FunctionID function;
        // Null: the whole body.
        LoopID outermost;
        // The values looked at have the numbers from the first up to the end.
        uint32_t firstValue { 0 };
        uint32_t endOfValues { 0 };
        // By value, less firstValue. Null: not looked at, or without a result.
        Vector<TypeID> typesOfValues;
        // By block, less firstBlock: into m_bounds. Until a look comes to the block: boundsOfDominator, unless the one edge that enters it has narrowed something.
        uint32_t firstBlock { 0 };
        Vector<uint32_t> firstBoundOfBlocks;
        Vector<OpenLoop, 4> openLoops;
        // By loop of the body: the types at the parameters of its head as settle() has left them. None: it was not settled yet.
        // A body may have thousands of loops in a row, and the looks come to one of them and to those inside it.
        HashMap<uint32_t, Vector<TypeID, 8>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> settledParametersOfLoops;
        // Of a callee. Null: the look has come to no return.
        TypeID returned;
    };

    // The scratch holds at the parameters of the head what enters. Behind it: an invariant of the loop, and at every value of the loop a type that holds under it.
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
    // Of a loop of a copy whose turns the walk writes out: Walker::writeOutTurnsOfLoop(). A look for each turn, each with what the turn before has brought round.
    void lookAtTurnsWrittenOut(LoopID);
    // Of a read in such a copy out of the list that the call makes. Null: it is no such read.
    TypeID typeReadOutOfListOfCall(const BodyValue&, OperandTypes);
    TypeID typeShownOfEarlierReadInTurn(const BodyValue& read);
    void lookAtBlock(BodyBlock);
    void lookAtOperation(const BodyValue&);
    void noteStepMadeOnDoubles(const BodyValue&, OperandTypes, TypeID result);
    void keepIntegersThatTurnsTakeAsIntegers();
    void addEffectsOf(const BodyValue&, OperandTypes, TypeID result, FunctionID* halfRunBehindLooks = nullptr);
    // lookAtOperation(), in a turn that may begin again: the walk proves at the operation what is expected of its operands, and what fails begins the turn again:
    // what the tests leave of the operands holds from there on in all that the block dominates. This one looks under the types as they stand.
    void lookAtOperationAsTyped(const BodyValue&);
    // No terminator: the edge to a handler.
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
    // Into m_operandTypes. True: it reads out of a fresh object from in front of the loop, which stays as it is.
    bool collectOperandTypesOfRead(const BodyValue&);
    // Null: the operation has no result.
    TypeID typeByRow(const BodyValue&, OperandTypes);
    TypeID typeOfResultAt(const BodyValue&);
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
    // From the call in the body that the walk reads inward.
    Vector<FunctionID, 4> m_calleesLookedAt;
    // The copies among them whose turns the walk writes out. The list of the other arguments is made for the copy and only read there, so a look knows it as the walk does: by what the call lists.
    struct ListOfCallLookedAt {
        FunctionID callee;
        // The entry parameter of the callee's body.
        ValueID list;
        Vector<TypeID, 8> elements;
        TextOfCopyRead text;
    };
    Vector<ListOfCallLookedAt, 1> m_listsOfCallsLookedAt;
    Vector<Bound, 16> m_bounds;
    // By value. Set: some bound may be of the value.
    BitVector m_valuesWithBounds;
    // By block, of the look being made at the loop that holds it: an edge that some run may take leads to it. Of a loop of the body that the walk reads, under Folding.
    BitVector m_blocksEntered;
    bool m_leavesOutWhatNoRunEnters { false };
    // Null: the loop has no loop inside it and stands in none that was looked at.
    Kept* m_kept { nullptr };
    bool m_hasChangedWhatIsKept { false };
    bool m_hasMadeLooks { false };
    // Of the innermost open loop. Zero: nothing is kept, or no loop is open.
    uint32_t m_firstValueOfInnermostLoop { 0 };
    BlockID m_blockLookedAt;
    // Some loop around the innermost open one runs code that nobody knows, so the effects of an operation add nothing.
    bool m_unknownCodeRunsAround { false };
    // Of the block being looked at.
    uint32_t m_firstBound { noBound };
    Effects m_effects { Effects::none() };
    Vector<TypeID, 8> m_typesThatCameRoundOutermost;
    Vector<bool, 8> m_isLostOutermost;
    Vector<bool, 8> m_isSteppedOnDoublesOutermost;
    // False while the operands of a read are typed, which lets leave what it names by itself.
    bool m_mentionsLetLeave { true };
    // Some look has found that every mention of them so far is a read that leaves them as they are.
    Vector<Reference, 4> m_objectsLeftAsTheyAre;
    bool m_hasLostTypes { false };
    // Some read was typed and weighed as a read out of a list that a row in front of a loop has tested, which holds in every turn only where no turn writes a list.
    bool m_countsOnListsTestedInFront { false };
    // Walker::looksCountOnTurnsThatBeginAgain() of the outermost loop.
    bool m_countsOnTurnsThatBeginAgain { false };
    // Of the look being made at the outermost loop, in the order of the stream: no operation so far does what may not be run again unseen. The walk does no more than the looks count on, and comes to an operation only past operations in front of it in the stream.
    bool m_nothingWasDoneInTurn { false };
    bool m_takesEffectsOfCopiesFromTheirOperations { false };
    // Walker::looksAskOnlyWhetherTurnsRunOtherCode() of the outermost loop, and: some operation does. Nothing more is looked at, and there is no preview.
    bool m_asksOnlyWhetherTurnsRunOtherCode { false };
    bool m_hasFoundOtherCode { false };
    // Of looks that count on nothing: LoopPreview::firstOperationThatRunsOtherCode.
    ValueID m_firstOperationThatRunsOtherCode;
    bool m_otherCodeRunsInLoopInside { false };
    // Of the look being made at the outermost loop.
    Vector<GuessCountedOnByLooks, 4> m_guessesCountedOn;
    // Of the operation that was looked at last in the body that the walk reads.
    bool m_lastOperationMayRunOtherCode { false };
    // typeReturnedByCopyOf() was entered.
    bool m_hasLookedAtCopy { false };
    // Walker::expectationsRememberedForLook(), of the frame on top.
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
    // What is kept was found by looks that count on nothing, and is left to them as it is.
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
    // What is kept of a loop around holds the types of the values in here.
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

// The loops that are open stand one in another from the outermost on.
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

// True: the looks that settle() would make now were made before, and all is as they would leave it.
bool LooksAtLoops::takeWhatIsKeptOf(LoopID loop, KeptOfLoop& kept, BodyBlock head, std::span<const TypeID> assumed)
{
    if (!kept.isKept || kept.opensGuesses != m_walker.opensGuesses())
        return false;
    bool hasBegunSo = kept.starts.containsIf([&](const Vector<TypeID, 8>& start) {
        return std::ranges::equal(start, assumed);
    });
    if (!hasBegunSo)
        return false;
    // Whatever names a value from in front of the outermost loop lets it leave, and is a free value of the loop around where it comes from outside that too: typeOf().
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

// The loop is closed: the innermost open loop is the one around it.
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
    // What comes from outside the loop around as well is a free value of that: typeOf().
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
    // Under what holds, one look finds that it holds. Where the last look assumed more, that one look is still to be made: it types the values of the loop more narrowly.
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

    // The head then drops the note, and the walk builds reads that test for themselves: neither the types nor the effects of these looks hold.
    if (m_countsOnListsTestedInFront && !leavesListsTestedInFrontOfLoopAsTheyAre(m_effects))
        return std::nullopt;
    // The reads were asked about under the types of the looks. Where those do not hold, or a turn ends what the state knows of the elements of a fresh object, the walk may build something else for them.
    bool turnsEndElementsOfFreshObjects = !m_effects.mayRunOtherCode() && m_effects.writes.containsAny(AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage });
    if (m_hasLostTypes || turnsEndElementsOfFreshObjects) {
        for (Reference object : m_objectsLeftAsTheyAre)
            m_walker.letLeave(object);
    }

    // What came round in the last look came round under an assumption that holds this type, so it comes round under this type too.
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
    // The looks around count on what they have settled on for this head. The walk types the turns by what it is handed here, so that is what has to lie within it, and not what the last look assumed:
    // `h = (h * 31 + s.charCodeAt(i)) | 0` brings 0 to 65535 first, is widened upward alone, brings an int32 and goes to the top of its kind, and all that ever comes round is an int32.
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

// Look 1 assumes what enters. What grew is widened, and look 2 checks that. What still grows goes to the tops of its kinds, and look 3 checks that.
// A parameter that grows even then is lost, with all that is made of it. Whatever is made of nothing that is lost has held in look 3.
void LooksAtLoops::settle(LoopID loop)
{
    BodyBlock head = index().block(index().loop(loop).head);
    uint32_t numberOfParameters = head.numberOfParameters();

    // Each look at the loop around settles this loop anew. It begins with what the loop was left with, joined with what enters now: that holds at once unless more enters than before,
    // so a loop costs a look for each look at the loop around it and one for each time that something grows, and not the looks of all the loops around it multiplied.
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
        // A turn of a loop inside is no part of the turn that is run once.
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
            TypeID grown = look ? topsOfKindsInLoops(m_arena, united) : widenedInLoops(m_arena, assumed[parameter], united);
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
        // It has written over the types of the loops inside.
        for (size_t place = 0; kept && place < m_kept->loops.size(); ++place) {
            if (!m_kept->loops[place].blocks.isEmpty() && index().isInLoop(m_kept->loops[place].blocks[0], loop))
                m_kept->loops[place].isKept = false;
        }
    }
    if (m_walker.isLoopThatMayDoAnything(m_stretch.function, head.id()))
        m_effects.merge(Effects::unknownCode());
    // What came round in the last look came round under an assumption that holds what the looks began with and what came round, so it comes round under that too: the head holds no more.
    // Whatever stands behind the loop and names a parameter of its head sees that, and so does the next look at the loop, which begins with it. The values in the loop keep the types of the last look, which hold more.
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
        // Where the effects were passed over they are not all here.
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

// An environment and an opaque value stay what they are: no operation that takes one has a form for anything else.
TypeID LooksAtLoops::lost(TypeID type)
{
    if (!type || (!holdsOnlyPrimitives(type) && !m_arena.at(type).isJavaScriptValue()))
        return type;
    return joinOf(m_arena, m_families, m_anything, type);
}

// `isLost`: by parameter of the head. The types of the last look rest on an assumption about those parameters that does not hold. So does the type of whatever takes one of them, and so on round the loop and the loops inside.
// Each such value is lost, a parameter of the head among them too. The effects of an operation follow from the types of its operands, so the loop is taken to do anything.
void LooksAtLoops::loseWhatIsMadeOf(LoopID loop, Vector<bool, 8>& isLost)
{
    const BodyLoop& record = index().loop(loop);
    BodyBlock head = index().block(record.head);
    uint32_t numberOfValues = m_stretch.endOfValues - m_stretch.firstValue;
    // The place behind the last value stands for all the parameters of the head.
    uint32_t placeOfAllParameters = numberOfValues;
    BitVector isMadeOfLost(numberOfValues + 1);
    auto isInStretch = [&](ValueID value) {
        return value.index() >= m_stretch.firstValue && value.index() < m_stretch.endOfValues;
    };
    auto isMarked = [&](ValueID value) {
        return isInStretch(value) && isMadeOfLost.quickGet(value.index() - m_stretch.firstValue);
    };

    // What is made of what, read off the loop once: each value of the stretch has a list of those that are lost with it. `v0 = v1; v1 = v2; ..` loses one more variable with each way round the loop,
    // so rounds over the loop until nothing grows are as many as it has variables.
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
        // What a test leaves of a value follows from the types of all that the test takes.
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
        // Behind a relation an operand has a narrower type under its own name. An operand from in front of the loop has no place here: then nothing of the loop is kept.
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
    // A loop inside forgets its own where it is looked at, and where it is not, what lies there is what is kept of it. Its head is told anew what enters.
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

// The walk may build a loop twice (AOTWalkLoopsBuiltTwice.cpp), and behind it the two copies meet. So what a relation in a loop has left of a value holds in the loop alone, and behind the loop holds what held at its head.
// The head of the outermost loop that holds the dominator of the block and not the block. Null: there is none.
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

// The effects of a call are Link's whether or not the walk copies the callee, so a look at a callee adds none.
void LooksAtLoops::lookAtBlock(BodyBlock block)
{
    // The walk hands a handler what bind() was handed.
    m_firstBound = noBound;
    m_blockLookedAt = block.id();
    if (block.hasHandler()) {
        doNotKeep();
        followEdge(block.handler(), nullptr, 0);
        // Under a handler the walk begins no turn again: Walker::mayBeginTurnAgainInClosedCopy().
        m_nothingWasDoneInTurn = false;
    }

    // The walk begins the turn again where a block that only throws is entered: Walker::walkBlock().
    if (m_nothingWasDoneInTurn && (!isLookingAtCopy() || m_takesEffectsOfCopiesFromTheirOperations) && block.terminator().kind() == TerminatorKind::Throw && !block.hasHandler())
        return;

    uint32_t& first = firstBoundOf(block.id());
    if (first == boundsOfDominator)
        first = firstBoundInheritedBy(block.id());
    m_firstBound = first;

    for (BodyValue operation : block.values()) {
        lookAtOperation(operation);
        // In a copy whose operations are asked what they do, m_effects holds what the copy has done so far.
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
    } else if (terminator.numberOfTargets() || block.hasHandler()) {
        // Behind a terminator that leads nowhere no turn follows.
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
    // The length of a list is a property of its own that nobody can replace: the walk builds GetArrayLength wherever the holder is proven a list. The row of the read by name says nothing of it.
    if (operation.opcode() == Opcode::PropGet && m_walker.has(Mechanism::TypedOperations) && !operation.isFreeName() && operation.key() == m_walker.program().keyOf(BuiltinNames::length)) {
        TypeID holder = operands[OperandIndex::PropGet::object];
        if (holder && hasKind(m_arena, holder, OperandKind::Array))
            byRow = resultTypeOf(m_typing, Draft(Opcode::GetArrayLength), OperandTypes { std::span<const TypeID>(&holder, 1) });
        // So is that of a text. Off undefined and null the read throws: splitNullishOffReceiverInWalkTyped().
        else if (TypeID text = holder && holdsOnlyPrimitives(holder) ? m_arena.withoutNullish(holder) : TypeID(); text && !isNeverType(text) && m_arena.at(text).isAnyString())
            byRow = resultTypeOf(m_typing, Draft(Opcode::GetStringLength), OperandTypes { std::span<const TypeID>(&text, 1) });
    }
    // The length of a view that stays as made: readsOffEntryParameterThatStaysAsMade(). The walk builds GetTypedArrayLength there, so `i < view.length` is a relation of two integers, which runs no code.
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
    // What the walk builds for such a read at the most.
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
        // Link speaks for every caller of the function. The copy is typed by what this call hands over.
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
    // It looks at other operations, so the types of the operands are gone behind it.
    m_stretch.typesOfValues[operation.id().index() - m_stretch.firstValue] = withWhatCopyReturns(operation, operands, byRow);
}

// `while (pos < end && text.charCodeAt(pos) === 32) pos++`, with `pos` shared by closures: each mention is a read. While the turn has written nothing and run no other code, each finds what the first has found,
// so what a test in front of the first user shows holds of all. The walk answers the later reads from its state, and the state has the checked name: proveExpectedTypeOrBeginTurnAgain().
TypeID LooksAtLoops::typeShownOfEarlierReadInTurn(const BodyValue& read)
{
    // `source.position < source.text.length && source.text.charCodeAt(source.position) === 32`: so with the slots of one object. buildReadForTypedAccess() binds the later read to what the state holds of the slot.
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
    // An edge may leave in any turn, so what stands behind the loop sees of a value of the loop what it is in some turn.
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
    // The test that fails.
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

// The types of the looks only grow, so a step that has left the integers in one look leaves them in the next, or its operand is no integer any more, and then neither is the parameter.
void LooksAtLoops::noteStepMadeOnDoubles(const BodyValue& step, OperandTypes operands, TypeID result)
{
    constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    if (!steps.contains(step.opcode()) || !m_stretch.outermost || isLookingAtCopy() || !result || !operands[0])
        return;
    // Where the true result may lie further out the row gives no integer, and then the parameter is none either.
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
        // Each loop is told of its own parameters, for the preview that begins at it. What a loop inside says of a loop around is no part of what is kept of it.
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

// An index, an operand of a bitwise operator, an argument, a value that is stored: each wants the integer or its box in every turn, and both are dearer to make of a double than the step is on integers.
// So a parameter comes round as a double where the turns only step it, compare it with what is no integer, and hand it on. One pass over the loop, and only where a step was noted.
// Bit i of a value: it is made of parameter i by casts, steps and joins alone. A join stands behind the edges that lead to it, but for the head of a loop.
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
    if (familyOf(opcode) == OperationFamily::Iteration && isPartOfIterationOverListLiteral(m_walker, index(), operation))
        return;
    // buildFirstReadOfNamedBuiltin()
    if (opcode == Opcode::Builtin && firstReadOfNamedBuiltinIsSeen(m_walker, operation))
        m_effects.merge(declaredBoundOf(Opcode::InitializeBuiltin));
    // stepForInInChoices(): the position goes round the loop in a word of the heap, which each turn reads and stores. Over a text the step itself lets nothing else run.
    if (opcode == Opcode::ForInStep)
        m_effects.merge(declaredBoundOf(Opcode::PutHeapFrameWord));
    // buildReadOfDeferredVariable(): the read that finds the mark makes the object and stores it.
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
        // What is called has passed the test for the closures of one function.
        if (!callee && m_nothingWasDoneInTurn && (opcode == Opcode::Call || opcode == Opcode::Construct) && operands[OperandIndex::Call::callee] && m_arena.at(operands[OperandIndex::Call::callee]).isObject())
            callee = singleFunctionOf(m_arena, m_arena.at(operands[OperandIndex::Call::callee]));
        if (callee && callee.index() < linked.functions.size()) {
            effectsOfCallee = linked.function(callee).effectsOnReturn;
            effectsOfCallee.merge(linked.function(callee).effectsOnThrow);
            foundByLink = &effectsOfCallee;
        }
    }
    // Where the types of the look prove the typed form of an access, no turn does more than that form.
    auto typeOfInput = [&](ValueID input) {
        return typeOf(input);
    };
    std::optional<Effects> ofTypedForm = effectRuleOf(opcode) == EffectRule::Access ? effectsOfTypedFormAhead(m_walker, operation, TypesOfInputs(typeOfInput), halfRunBehindLooks) : std::nullopt;
    // `text.charCodeAt(pos)`: what is called is proven a built-in, and these operands reach one row of it: no turn does more than callBuiltin() hands build() for that row.
    // Of a call that takes texts, numbers and the like and gives a number or a boolean. Where an object is among them the walk has forms of its own, which do other things than the row says:
    // `Object.hasOwn(box, "a")` is a PropHasOwn, `JSON.stringify(record)` writes into a builder; and a text that is given may be laid into a builder where it is made.
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
    // The walk may build a join that gives a text as appends to a builder.
    if (operationsThatJoinTexts.contains(opcode) && result && m_arena.at(result).isAnyString()) {
        doNotKeep();
        if (mayKeepTextsInStringBuilders(m_walker))
            effects.merge(declaredBoundOf(Opcode::StringBuilderAppendValue));
    }
    m_effects.merge(effects);
}

void LooksAtLoops::lookAtOperation(const BodyValue& operation)
{
    // In a copy: while the walker has the callee's frame, which is while the copy's operations are asked what they do.
    if (!m_nothingWasDoneInTurn || (isLookingAtCopy() && !m_takesEffectsOfCopiesFromTheirOperations)) {
        lookAtOperationAsTyped(operation);
        return;
    }
    Effects ofOperationsInFront = m_effects;
    m_lastOperationMayRunOtherCode = false;
    lookAtOperationAsTyped(operation);
    // `total += n`, where total may be a text: the operator runs no other code, and goes by stub.
    static constexpr OpcodeSet operators = opcodesOfFamily(OperationFamily::Operator);
    bool isOperator = operators.contains(operation.opcode());
    if (!m_lastOperationMayRunOtherCode && !isOperator)
        return;

    // What the tests for what is expected of the operands would leave of them. The cheapest question first: of most operands nothing is expected that is not known.
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
    // As it stands the operation is typed: no test in front of it makes the turn do less.
    if (!isClosed())
        return;

    uint32_t firstBound = m_firstBound;
    size_t numberOfGuesses = m_guessesCountedOn.size();
    for (const Left& left : leftByTests) {
        ValueID behindCasts = valueBehindCasts(left.operand);
        m_bounds.append(Bound { behindCasts, left.proven, m_firstBound });
        m_valuesWithBounds.set(behindCasts.index());
        m_firstBound = m_bounds.size() - 1;
        // In a copy the walk opens the guess by itself: in a loop one typed operation pays for it, and its failing side begins the turn again as well.
        if (!isLookingAtCopy())
            m_guessesCountedOn.append(GuessCountedOnByLooks { operation.id(), left.operand, left.expected });
    }
    firstBoundOf(m_blockLookedAt) = m_firstBound;
    Effects withOperationAsItStands = std::exchange(m_effects, ofOperationsInFront);
    m_nothingWasDoneInTurn = true;
    lookAtOperationAsTyped(operation);
    if (!isClosed())
        return;
    // The tests buy nothing: the operation has no typed form behind them, so the walk builds none of them.
    m_firstBound = firstBound;
    firstBoundOf(m_blockLookedAt) = firstBound;
    m_guessesCountedOn.shrink(numberOfGuesses);
    m_effects.merge(withOperationAsItStands);
    lookAtOperationAsTyped(operation);
}

// `Array.isArray(x)` behind the look at the cell: the walk writes the answer where the type of `x` decides it, and a branch on a constant is a jump: onlyTargetKnownToFolding().
// ONLY OF AN `x` FROM IN FRONT OF THE LOOPS THAT ARE LOOKED AT: what is known of it is the same in every look, so an edge that is left out in one is left out in all.
// Of a condition that is made of what a loop carries no edge is left out: a later look may open it, the third look is the last, and loseWhatIsMadeOf() follows what values are made of, not which blocks are entered.
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

    // What the relation that the Branch asks leaves of its operands on this edge. Null: all of the operand.
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

    // The block that the edge leaves dominates the target. A look at a loop may come to an edge that leaves the loop several times in one look at the loop around: the last time counts.
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

// Of a value in front of the loop: its own type. What the state knows beyond that may end in some turn.
// Whatever a turn names it may store into or hand on, and the next turn comes to the head with that: no effect ends what is known of a fresh object, so it leaves in front of the loop.
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
                // Whoever takes the name gets a string, which is made where he stands.
                doNotKeep();
                m_effects.merge(declaredBoundOf(Opcode::StringBuilderToString));
                type = seenByLooks;
            }
        }
    } else if (value.index() < m_stretch.endOfValues)
        type = m_stretch.typesOfValues[value.index() - m_stretch.firstValue];
    return type ? type : m_anything;
}

// The casts that the walk binds to what it has built for their operands. For a claim it builds a test, and what passes has a record of its own.
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

// An environment or an opaque value joined with what a look lost track of reads as anything, and no operation that takes such a value has a form for that.
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
    // The rule of an allocation reads the place of its prototype as new.target, as Walker::typeOfNewTargetBehind() has it. Any object says that nothing is known of new.target.
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
    // Closed, it is its operand: nothing is tested.
    if (opcodesThatStateTypes.contains(opcode))
        return operands[0];
    if (resultRuleOf(opcode) == ResultRule::None)
        return { };
    if (opcode == Opcode::ResultAt)
        return typeOfResultAt(operation);

    copyIntoDraft(operation);
    // It is read through ResultAt alone, which types the result that it asks for.
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(m_typing.program, m_draft))
        return TypeTable::undefined;
    if (opcode != Opcode::ElemGet)
        return resultTypeOf(m_typing, m_draft, operands);
    TypeID before = m_walker.exchangeResultOfReadBeingTyped(m_walker.facts().linked().resultOfReadByKey(m_stretch.function, operation.id()));
    TypeID type = resultTypeOf(m_typing, m_draft, operands);
    m_walker.exchangeResultOfReadBeingTyped(before);
    return type;
}

TypeID LooksAtLoops::typeOfResultAt(const BodyValue& operation)
{
    auto resultAt = operation.as<Opcode::ResultAt>();
    ValueID results = resultAt.resultsOperand();
    if (!results || index().isParameter(results))
        return m_anything;

    BodyValue producer = index().value(results);
    if (producer.opcode() == Opcode::IteratorResume && resultAt.index() == static_cast<uint32_t>(IteratorResumeResult::Value)) {
        if (std::optional<StepOfListLiteral> step = stepOfListLiteral(m_walker, index(), producer))
            return step->element ? typeOf(step->element) : TypeID(TypeTable::undefined);
        // The question names what is iterated and takes nothing of it: a list that the state knows stays as it is known.
        auto typeOfInput = [&](ValueID input) {
            SetForScope withoutLeaving(m_mentionsLetLeave, false);
            return typeOf(input);
        };
        if (TypeID unit = isLookingAtCopy() ? TypeID() : typeOfStepOverTextAhead(m_walker, index(), producer, TypesOfInputs(typeOfInput)))
            return unit;
    }
    OperandTypes operands = collectOperandTypes(producer.opcode(), producer, m_operandTypesOfOtherOperation);
    copyIntoDraft(producer);
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_typing.program, m_draft);
    if (!numberOfResults || resultAt.index() >= *numberOfResults)
        return m_anything;
    return resultTypeOf(m_typing, m_draft, operands, { }, resultAt.index());
}

// The row of a call of one known function answers with what Link has proven of the function's result. In the place of the call the walk may put a copy of the callee, and what the copy returns is typed by the rows of the callee's operations.
// Link proves by rules of its own, so the one need not lie within the other. A closed arm beside the copy calls, so the call may give either.
// `byRow` says anything: it holds whatever a copy returns. m_draft holds the call wherever it says less.
TypeID LooksAtLoops::withWhatCopyReturns(const BodyValue& call, OperandTypes operands, TypeID byRow)
{
    Opcode opcode = call.opcode();
    if (opcode == Opcode::PropGet || opcode == Opcode::SuperGet)
        return withWhatCopyOfGetterReturns(call, operands, byRow);
    // Of a callee whose result nothing is proven of, no look at its copy narrows anything. In a turn that may begin again the look is after what the copy does: behind the tests of its own guesses it may run no other code.
    if (!callsTypedByResultOfCalleeInLoops.contains(opcode) || !byRow || (m_arena.at(byRow).isAnything() && !m_takesEffectsOfCopiesFromTheirOperations))
        return byRow;

    // `i = Math.floor(i / 2)`: what is built is a division of integers.
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

    // The row of the Call answers with what the table of built-ins says. What is built is a direct call of the function that stands behind the built-in.
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
        // Where the function is not copied it is called directly: callLibraryFunctionBehindRows(). The row of that call answers with what Link has proven of the function.
        if (FunctionID behindBuiltin = findLibraryFunctionBehindBuiltin(m_walker, operands[OperandIndex::Call::callee])) {
            Draft direct(Opcode::CallDirect);
            direct.setFunction(behindBuiltin);
            direct.setRole(m_walker.plan().conventions().directEntryOf(behindBuiltin).role);
            byRow = joined(byRow, resultTypeOf(m_typing, direct, OperandTypes { }));
        } else if (!callee) {
            // Which of the functions that the rows name the walk enters hangs on what it has seen of the operands where the call stands: callLibraryFunctionBehindRows(). So each of them counts, called or copied.
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

    // The walk previews a loop of a copy when it comes to it, by itself.
    const BodyIndex& body = m_walker.facts().bodies().at(callee);
    std::optional<TurnsOfCopyWrittenOut> turns = body.hasLoops() ? turnsOfCopyWrittenOutAt(m_walker, m_draft, callee, place) : std::nullopt;
    if (body.hasLoops() && !turns) {
        // At the head of a loop inside something counts as done: Walker::noteHeadOfTurn().
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
    // In a turn that may begin again no closed arm stands beside the copy.
    if (returned && m_takesEffectsOfCopiesFromTheirOperations && m_hasLookedAtCopy)
        return returned;
    return returned ? joined(byRow, returned) : byRow;
}

// A read that finds a getter is built as a direct call of it or as a copy: callAccessor(). The row answers with what Link has proven of the getter's result.
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

// `gauge.level`, `gauge.level = v` in a turn that may begin again, where the half of an accessor is run behind looks: callAccessor(). A look that fails begins the turn again, so the access does what the operations of the copy do.
// Where the half is called, or no look goes into it, m_hasLookedAtCopy stays false, and the access may do anything.
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
    // At the head of a loop inside something counts as done: Walker::noteHeadOfTurn().
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

// Null: no way through the callee returns.
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
    // Whoever is asked what an operation of the copy does reads the body of the frame on top.
    bool laysFrame = m_takesEffectsOfCopiesFromTheirOperations && m_walker.layFrameForLookIntoCopy(callee, body);
    SetForScope fromOperations(m_takesEffectsOfCopiesFromTheirOperations, laysFrame);
    m_hasLookedAtCopy = laysFrame;
    size_t numberRememberedOfCaller = m_expectationsRemembered.size();

    std::ranges::copy(typesOfEntryParameters, typesOfParametersOf(body.block(entryBlock)).begin());
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFoundOtherCode; ++number) {
        // What the count of the arguments and the integers among them decide the walk folds: no way leads there.
        bool isPassedOver = !m_listsOfCallsLookedAt.isEmpty() && m_listsOfCallsLookedAt.last().callee == callee && !m_listsOfCallsLookedAt.last().text.isEntered.get(number);
        if (!body.isReachable(BlockID(number)) || isPassedOver)
            continue;
        // A body with loops is looked at only where the walk writes their turns out.
        LoopID loop = body.loopOf(BlockID(number));
        if (!loop)
            lookAtBlock(body.block(BlockID(number)));
        else if (body.loop(loop).head == BlockID(number))
            lookAtTurnsWrittenOut(loop);
    }

    m_calleesLookedAt.removeLast();
    // What was remembered of the values of the copy goes with its frame.
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

// What holds of an entry parameter whoever calls: Walker::takeEntryParameters(). Whatever a copy makes for a parameter that the call hands nothing to lies within it.
TypeID LooksAtLoops::typeLaidDownAtEntryOf(FunctionID callee, unsigned entryParameter)
{
    TypingContext withoutFacts { m_typing.program, m_arena, PositionTypes::knowingNothing(), TypeLevel::Proven };
    const DeclaredType* declared = m_typing.program.at(callee).declaredEntryParameter(entryParameter);
    TypeID type = declared ? m_typing.program.provenTypeOf(*declared) : declaredEntryTypeOf(withoutFacts, callee, entryParameter);
    return type ? type : m_anything;
}

// Whether the walk hands on the value that was tested or what the test leaves of it, it hands on no more than both. What a test leaves need not lie inside what it was asked of: narrowByTest().
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
    // narrowOnEdge(): behind a test of a type stands a Narrowed, of a type that lies within this one. Where the walk has none to build after all, what comes round tells: walkUnitAgainWithWiderHeadOfCode().
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

// The row's defaults are the closed form. A rule asks of an operand whether it is there, and its type. It never follows the reference.
void LooksAtLoops::copyIntoDraft(const BodyValue& operation)
{
    m_draft.become(operation.opcode());
    m_draft.flags = static_cast<uint8_t>(operation.flags());
    copyFields(operation, m_draft, [](ValueID operand) {
        return Reference::value(operand);
    });
}

// Every effect. Each parameter that is not handed round unchanged gets what the look at the loop around has settled on, and `anything` where there is none.
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

// Where some cycle has two ways in, a look in the order of the stream comes to a block before it has seen all that arrives there.
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

// ---- A walk that is given up: AOTWalk.h

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

// As fail(), but for the words.
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

// The edge that comes round leaves a block of the body whose frame is the newest. A parameter behind those of the input is the walk's own, and no look types it.
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
