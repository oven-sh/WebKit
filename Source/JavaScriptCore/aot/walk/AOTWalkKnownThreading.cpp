/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding, TypedOperations, EscapeAnalysis: ways that are about to meet walk a few operations apart where one of them knows more than all of them together.
// A small join is walked once for the ways that decide each target of its terminator. The head of a join, and what stands behind a copy with several returns, is walked once for each way
// on which an operation has a form that it lacks behind the meeting. An arm walks the branch of its block. Every operation is walked as it is written: what is asked ahead says what pays, never what is right.
// At a suspension: what may suspend is walked once. At a throw: a block under a handler is walked once, and no handler reads what the ways call by different names.
// Withheld: the ways meet where the body has them meet.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTTypeNarrowing.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

// In operations of the body. So many at most are asked ahead on a way, and so many at most are built a second time for all the ways of one block together:
// a way that decides a test saves the test and its branch, one that has a typed form saves the call of a stub, and an operation costs about what either saves.
static constexpr uint32_t mostOperationsOfJoinWalkedForEachWay = 8;

static constexpr OpcodeSet testsThatTypesDecideInKnownThreading = opcodesWhere([](Opcode opcode) {
    Proof proof = provesOf(opcode);
    bool narrowsType = proof == Proof::TestedType || proof == Proof::ExactFunction || proof == Proof::Truthiness || proof == Proof::Equality || proof == Proof::IntegerBound;
    return narrowsType && hasRuleForFolding(opcode);
});

// Where an iteration stands the walk keeps beside the state, and what is bound to its frame, its function or its site stands once in a unit.
static constexpr OpcodeSet operationsWalkedOnceInKnownThreading = opcodesOfFamily(OperationFamily::Iteration) | opcodesOfFamily(OperationFamily::ForIn) | opcodesOfFamily(OperationFamily::Coroutine)
    | opcodesOfFamily(OperationFamily::Exception) | opcodesWhere([](Opcode opcode) { return boundToOf(opcode) != BoundTo::Nothing; }) | OpcodeSet { Opcode::LoopHint };

static constexpr OpcodeSet callsOfValuesInKnownThreading { Opcode::Call, Opcode::Construct, Opcode::CallList };
// Closed, each calls a stub. A bitwise operator converts a number to int32, which costs nothing where the number is an integer.
static constexpr OpcodeSet operationsTypedByWayInKnownThreading = allRelations | allBitwiseOperators
    | OpcodeSet { Opcode::ToBoolean, Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet, Opcode::Call, Opcode::Construct };
static constexpr OpcodeSet callsThatLayOutListsInKnownThreading { Opcode::CallList, Opcode::ConstructList };
static constexpr OpcodeSet operationsThatWaysDecideInKnownThreading = operationsTypedByWayInKnownThreading | callsThatLayOutListsInKnownThreading | operationsThatJoinTexts | OpcodeSet { Opcode::CopyDataProperties };
static constexpr OpcodeSet constructionsCopiedInKnownThreading { Opcode::Construct, Opcode::SuperConstruct };
static constexpr OpcodeSet operationsThatBuildNothingInKnownThreading = opcodesThatStateTypes | primitivesWrittenForFolding;

// ---- What is asked of the body alone

static bool mayBeWalkedAgainInKnownThreading(const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (operationsWalkedOnceInKnownThreading.contains(opcode) || maySuspendAsWritten(operation))
        return false;
    // The int32 error judges what enters such a position by the types that all the ways together bring.
    return opcode != Opcode::Convert || !operation.as<Opcode::Convert>().declared().has(PositionFlags::IsInt32Promise);
}

template<typename User>
static bool namesByDefinitionInKnownThreading(const User& user, ValueID value)
{
    bool namesByDefinition = false;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        namesByDefinition |= operand == value && mustBeDefinition(user.operandKind(operandIndex));
    });
    return namesByDefinition;
}

// No parameter of a block can stand for it. Whatever gives several results is named so by every user, so the first one tells.
static bool mustBeNamedByDefinitionInKnownThreading(const BodyIndex& index, ValueID value)
{
    std::optional<BodyUser> user = index.firstUser(value);
    if (!user)
        return false;
    switch (user->kind) {
    case BodyUseKind::Operand:
        return namesByDefinitionInKnownThreading(index.value(user->value), value);
    case BodyUseKind::TerminatorOperand:
        return namesByDefinitionInKnownThreading(index.terminator(user->block), value);
    case BodyUseKind::BlockSubject:
        return true;
    case BodyUseKind::EdgeArgument:
    case BodyUseKind::EdgeProof:
    case BodyUseKind::HandlerArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// coroutines-5 keeps what lives across a suspension in words of the heap. It stores them where the body of the coroutine is walked at the outermost level, each block once, and what arms define where the arms have met.
// So in that body no block is walked again, and no arm leaves by the terminator: a word would stay as it was on the second way. A callee that is walked in line has no such words.
static bool bodyKeepsValuesInWordsOfHeapInKnownThreading(const Walker& walker)
{
#if __has_include("AOTWalkCoroutineHeapWordPlan.h")
    return walker.unit().role == CodeRole::CoroutineBody && !walker.depthOfCopies();
#else
    UNUSED_PARAM(walker);
    return false;
#endif
}

// ---- What the rows and the links of the chain answer ahead

static PrimitiveKnownToFolding onlyValueOfTypeInKnownThreading(const TypeArena& types, TypeID id)
{
    if (!id)
        return { };
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };

    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

// ---- The ways through one block of the body being read

class KnownThreading {
    WTF_MAKE_NONCOPYABLE(KnownThreading);
public:
    KnownThreading(Walker& walker, BlockID inputBlock)
        : m_walker(walker)
        , m_frameIndex(walker.m_frames.size() - 1)
        , m_index(*walker.m_frames.last().index)
        , m_block(m_index.block(inputBlock))
    {
    }

    bool endArmWithBranch();
    bool walkApartFromClosedWays();
    bool wouldBeWalkedApartFor(Reference value);

private:
    using Name = std::pair<ValueID, Reference>;

    struct ValueOnWay {
        // What stands for the value on the way. Null: the stretch computes it.
        Reference reference;
        PrimitiveKnownToFolding known;
        TypeID type;

        bool isDecided() const { return reference || known; }
    };

    // One of the ways that are about to meet in front of a stretch of the block.
    struct Way {
        // For each value that every way brings for itself.
        std::span<const Reference> brought;
        // OpenEdge::names.
        std::span<const Name> names;
        WalkState* state { nullptr };

        // Asked ahead. The target of the terminator that it decides, and how many operations it builds on the way there.
        std::optional<uint32_t> target;
        uint32_t numberOfOperationsBuilt { 0 };
        // A bit for each place from the begin of the stretch on: the operation there has a form on this way that it lacks where the ways have met.
        uint32_t placesDecided { 0 };
    };

    // The stretch from its begin to the end of the block as it would run on one way. The state of the walk is what holds on the way. It builds nothing but constants, which lie in no block.
    class Evaluation {
    public:
        Evaluation(KnownThreading& threading, const Way& way)
            : m_threading(threading)
            , m_walker(threading.m_walker)
            , m_way(way)
        {
        }

        std::optional<uint32_t> decidedTarget();
        uint32_t numberOfOperationsBuilt();

    private:
        ValueOnWay at(ValueID);
        ValueOnWay valueOf(Reference);
        ValueOnWay evaluate(const BodyValue&);
        std::optional<bool> answerThatTypesDecide(const BodyValue& test);
        ValueOnWay answerOfEqualTestBuilt(const BodyValue& test);
        ValueOnWay valueReadOffObjectInSight(const BodyValue& read);

        KnownThreading& m_threading;
        Walker& m_walker;
        const Way& m_way;
        std::array<ValueOnWay, mostOperationsOfJoinWalkedForEachWay> m_values { };
        uint32_t m_evaluated { 0 };
    };

    // The types of the values of the stretch by the rows of their operations, given the types of what is brought.
    struct TypesOfStretch {
        Vector<TypeID, 4> brought;
        std::array<TypeID, mostOperationsOfJoinWalkedForEachWay> byRow { };
        uint32_t known { 0 };
    };

    // A way that has come through a stretch.
    struct Arm {
        // Left open.
        BlockID block;
        WalkState state;
        // What it has for each value that is read behind the stretch.
        Vector<Reference, 8> values;
    };

    Walker::Frame& frame() { return m_walker.m_frames[m_frameIndex]; }
    uint32_t firstValue() const { return m_block.firstValue().index(); }
    std::optional<uint32_t> positionAmongBrought(ValueID value) const
    {
        uint32_t position = value.index() - m_firstBrought.index();
        return value.index() >= m_firstBrought.index() && position < m_numberBrought ? std::optional(position) : std::nullopt;
    }

    bool operationsMayBeWalkedAgain(uint32_t begin, uint32_t end);
    bool terminatorMayBeWalkedAgain(bool targetsLieInLoopOfBlock);
    bool letTerminatorBeWalkedForEachWay(ValueID firstBuiltOnEachWay);
    bool mayBeWalkedApartFromClosedWays();
    bool terminatorMayBeWalkedByWaysApart();
    template<typename Functor> bool looksAtValueFromInFront(const BodyValue& operation, const Functor& isLeanedOn);
    template<typename Functor> bool looksAtValueFromInFrontHereOrBehindJumps(const Functor& isLeanedOn);
    bool leansOnWhatWaysHaveProven(std::span<const uint32_t> passing, std::span<const uint32_t> closed);
    // For arms that have walked the block up to `begin`. True: each of them may walk what is left of it and its terminator, and the block is marked so. False: nothing was done.
    bool letArmsWalkEndOfBlockFrom(uint32_t begin);
    void forgetValuesThatNobodyReadsAnyMore();
    void forgetValuesFrom(ValueID first);

    uint32_t numberOfOperationsBuiltIn(uint32_t begin, uint32_t end);

    void walkBlockOnWays(std::span<const uint32_t> places, uint32_t namesAtHead);
    void meet(Vector<Arm, 4>&, std::span<const ValueID> values);

    Walker& m_walker;
    size_t m_frameIndex;
    const BodyIndex& m_index;
    BodyBlock m_block;
    // Among the operations of the block: where the ways begin to walk for themselves.
    uint32_t m_begin { 0 };
    // The values that every way brings for itself: the parameters of the block, or the operation in front of the stretch.
    ValueID m_firstBrought;
    uint32_t m_numberBrought { 0 };
};

// ---- One way, asked ahead

KnownThreading::ValueOnWay KnownThreading::Evaluation::valueOf(Reference reference)
{
    if (!reference || !reference.isRecord())
        return { };
    while (Reference checked = m_walker.state().checkedNameOf(reference))
        reference = checked;

    TypeID type = m_walker.typeOf(reference);
    // No type tells two doubles apart, and none names a made text: those two are read off the constant.
    const Operation& record = m_walker.code().at(reference.id());
    if (record.opcode == Opcode::Double)
        return { reference, PrimitiveKnownToFolding::number(record.number()), type };
    if (record.opcode == Opcode::String)
        return { reference, PrimitiveKnownToFolding::text(record.string()), type };
    return { reference, onlyValueOfTypeInKnownThreading(m_walker.types(), type), type };
}

KnownThreading::ValueOnWay KnownThreading::Evaluation::at(ValueID value)
{
    if (!value)
        return { };
    if (std::optional<uint32_t> position = m_threading.positionAmongBrought(value))
        return valueOf(m_way.brought[*position]);

    uint32_t first = m_threading.firstValue() + m_threading.m_begin;
    if (value.index() >= first && value.index() < m_threading.m_block.endOfValues().index()) {
        uint32_t position = value.index() - first;
        if (!(m_evaluated >> position & 1)) {
            m_evaluated |= 1u << position;
            m_values[position] = evaluate(m_walker.asWalked(m_threading.m_index.value(value)));
        }
        return m_values[position];
    }

    for (auto [named, name] : m_way.names) {
        if (named == value)
            return valueOf(name);
    }
    return valueOf(m_walker.referenceOf(value));
}

// As answerThatTypesDecideInFolding(): an operand that one side of the test leaves nothing of decides it.
std::optional<bool> KnownThreading::Evaluation::answerThatTypesDecide(const BodyValue& test)
{
    std::array<TypeID, 2> typesOfOperands;
    uint32_t numberOfOperands = 0;
    for (; numberOfOperands < typesOfOperands.size(); ++numberOfOperands) {
        ValueID operand = test.operand(numberOfOperands);
        if (!operand)
            break;
        TypeID type = at(operand).type;
        if (!type || isNeverType(type) || m_walker.types().at(type).mayBeUnspecified())
            return std::nullopt;
        typesOfOperands[numberOfOperands] = type;
    }

    TypingContext context { m_walker.program(), m_walker.types(), PositionTypes::knowingNothing() };
    OperandTypes operands { std::span<const TypeID>(typesOfOperands.data(), numberOfOperands) };
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (isNeverType(typeOfEdgeResult(context, test, operands, operandIndex, EdgeSide::Taken)))
            return false;
        if (isNeverType(typeOfEdgeResult(context, test, operands, operandIndex, EdgeSide::NotTaken)))
            return true;
    }
    return std::nullopt;
}

// `if (a === b) ..; if (a === b) ..`: the way has come by a branch on the same test of the same values.
KnownThreading::ValueOnWay KnownThreading::Evaluation::answerOfEqualTestBuilt(const BodyValue& test)
{
    Draft draft(test.opcode(), m_walker.originOf(test));
    draft.flags = static_cast<uint8_t>(test.flags());
    bool lacksOperand = false;
    copyFields(test, draft, [&](ValueID operand) {
        Reference reference = at(operand).reference;
        lacksOperand |= !reference;
        return reference;
    });
    if (lacksOperand)
        return { };
    m_walker.chooseConservativeFields(draft);
    return valueOf(findEqualOperationBuilt(m_walker, draft, declaredBoundInTable(test.opcode())));
}

// `while (!(r = step()).done)`: the way brings a literal, and the state knows what it holds.
KnownThreading::ValueOnWay KnownThreading::Evaluation::valueReadOffObjectInSight(const BodyValue& read)
{
    auto operation = read.as<Opcode::PropGet>();
    if (operation.isFreeName())
        return { };
    ValueOnWay object = at(operation.objectOperand());
    if (!object.reference || !object.type || isNeverType(object.type))
        return { };
    TypeArena& types = m_walker.types();
    LayoutID layout = singleLayoutOf(types, types.at(object.type));
    std::optional<FieldReference> field = layout ? findField(m_walker.program(), layout, operation.key()) : std::nullopt;
    ValueOnWay held = valueOf(field ? m_walker.state().slot(object.reference, *field) : Reference());
    return held.type && !(types.at(held.type).primitives & Primitive::Absent) ? held : ValueOnWay();
}

KnownThreading::ValueOnWay KnownThreading::Evaluation::evaluate(const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (opcodesThatStateTypes.contains(opcode))
        return at(operation.operand(0));
    if (primitivesWrittenForFolding.contains(opcode)) {
        Draft written = m_walker.draftOf(operation);
        m_walker.chooseConservativeFields(written);
        return valueOf(m_walker.constant(WTF::move(written)));
    }

    ValueOnWay result;
    if (operationsOnPrimitivesInFolding.contains(opcode)) {
        result.known = primitiveMadeOfConstants(m_walker.program(), operation, scopedLambda<PrimitiveKnownToFolding(ValueID)>([&](ValueID operand) {
            // It reads texts out of the table of the program, where a text that the walk has made does not lie.
            PrimitiveKnownToFolding known = at(operand).known;
            return known.isText() && liesInThread(known.asText()) ? PrimitiveKnownToFolding() : known;
        }));
    }
    if (!result.known && testsThatTypesDecideInKnownThreading.contains(opcode)) {
        if (std::optional<bool> answer = answerThatTypesDecide(operation))
            result.known = PrimitiveKnownToFolding::boolean(*answer);
    }
    if (!result.known && (opcode == Opcode::ToBoolean || opcode == Opcode::LogicalNot)) {
        Reference operand = at(operation.operand(0)).reference;
        if (std::optional<bool> outcome = operand ? outcomeKnownOf(m_walker, operand) : std::nullopt)
            result.known = PrimitiveKnownToFolding::boolean(*outcome == (opcode == Opcode::ToBoolean));
    }
    if (!result.known && branchesOf(opcode) != Branches::No)
        return answerOfEqualTestBuilt(operation);
    if (!result.known && opcode == Opcode::PropGet)
        return valueReadOffObjectInSight(operation);
    return result;
}

std::optional<uint32_t> KnownThreading::Evaluation::decidedTarget()
{
    BodyTerminator terminator = m_threading.m_block.terminator();
    ValueOnWay operand = at(terminator.operand(0));
    // onlyTargetKnownToFolding() goes by a constant. What the stretch computes and this knows, Folding binds to one.
    if (!operand.reference || operand.reference.isConstant()) {
        if (std::optional<uint32_t> target = onlyTargetWith(m_walker.types(), terminator, operand.known))
            return target;
    }
    if (terminator.kind() != TerminatorKind::Branch || !operand.reference)
        return std::nullopt;
    std::optional<bool> outcome = outcomeKnownOf(m_walker, operand.reference);
    return outcome ? std::optional<uint32_t>(*outcome ? 0 : 1) : std::nullopt;
}

uint32_t KnownThreading::Evaluation::numberOfOperationsBuilt()
{
    uint32_t number = 0;
    for (uint32_t id = m_threading.firstValue() + m_threading.m_begin; id < m_threading.m_block.endOfValues().index(); ++id) {
        if (!operationsThatBuildNothingInKnownThreading.contains(m_threading.m_index.opcodeOf(ValueID(id))))
            number += !at(ValueID(id)).isDecided();
    }
    return number;
}

// ---- What may be walked more than once

bool KnownThreading::operationsMayBeWalkedAgain(uint32_t begin, uint32_t end)
{
    for (uint32_t place = begin; place < end; ++place) {
        if (!mayBeWalkedAgainInKnownThreading(m_index.value(ValueID(firstValue() + place))))
            return false;
    }
    return true;
}

// Each walk of the terminator leads an edge of its own to a target, and the edges meet there as at any join.
bool KnownThreading::terminatorMayBeWalkedAgain(bool targetsLieInLoopOfBlock)
{
    // The looks at a loop have promised what a relation leaves of its operands in all that a target dominates which one edge enters as written: LooksAtLoops::followEdge().
    // The walk keeps that by a name which it notes only where the edge enters alone, and what comes round without it lies outside what was previewed.
    if (m_walker.isInLoop() && boundsProvenOnEdge(m_index, m_block.terminator(), 0))
        return false;
    for (BodyTarget target : m_block.terminator().targets()) {
        BlockID to = target.block();
        if (to == m_block.id() || m_index.isLoopHead(to) || m_index.isEnteredByHandlerEdge(to) || m_index.block(to).isHandlerBlock() || m_walker.inputBlock(m_frameIndex, to).isEntered)
            return false;
        if (targetsLieInLoopOfBlock && m_index.loopOf(to) != m_index.loopOf(m_block.id()))
            return false;
    }
    return true;
}

// The blocks are walked in the order of the stream, but for the members of a loop whose copies are being walked. So what is read for the last time in front of the block is read no more,
// and no edge that is led from here on has to be asked whether it brings a name for it. A hint may still mention it: it finds nothing, and is dropped.
void KnownThreading::forgetValuesThatNobodyReadsAnyMore()
{
    if (!frame().loopsBeingBuiltTwice.isEmpty())
        return;
    uint32_t here = m_block.firstParameter().index();
    frame().valuesBuiltTwice.removeAllMatching([&](ValueID value) {
        if (m_walker.m_liveness.lastUse(value).index() >= here)
            return false;
        frame().references[value.index()] = Reference();
        return true;
    });
}

void KnownThreading::forgetValuesFrom(ValueID first)
{
    for (uint32_t value = first.index(); value < m_block.endOfValues().index(); ++value)
        frame().references[value] = Reference();
}

// In front of the first of several walks of the terminator. What the block holds and a later block reads has another name on each way, so every edge from here on brings what stands for it: Frame::valuesBuiltTwice.
// `firstBuiltOnEachWay`: what stands in front of it in the block was bound once, in front of the ways, and keeps its one name: where arms of a guess walk the end of their block, the parameters of the block.
// False: some such value cannot travel so, and nothing was done.
bool KnownThreading::letTerminatorBeWalkedForEachWay(ValueID firstBuiltOnEachWay)
{
    if (frame().blocksWalkedForEachWay.contains(m_block.id()))
        return true;
    // What is live at the heads of a body grows as the body does since 11-03 17:49:55, and it is made once a build.
    BodyLiveness& liveness = m_walker.m_liveness;
    if (!liveness.isComputedFor(m_index))
        liveness.compute(m_index);

    Vector<ValueID, 8> readBehind;
    for (BodyTarget target : m_block.terminator().targets()) {
        liveness.forEachLiveAtHead(target.block(), firstBuiltOnEachWay, m_block.endOfValues(), [&](ValueID value) {
            readBehind.append(value);
        });
    }
    std::ranges::sort(readBehind);
    readBehind.shrink(std::ranges::unique(readBehind).begin() - readBehind.begin());

    for (ValueID value : readBehind) {
        if (!m_index.isParameter(value) && mustBeNamedByDefinitionInKnownThreading(m_index, value))
            return false;
    }
    // A handler edge brings the arguments that are written and no name.
    if (!readBehind.isEmpty()) {
        std::optional<Vector<BlockID, 4>>& handlerBlocks = frame().handlerBlocks;
        if (!handlerBlocks) {
            handlerBlocks.emplace();
            for (uint32_t number = 0; number < m_index.numberOfBlocks(); ++number) {
                if (m_index.block(BlockID(number)).isHandlerBlock())
                    handlerBlocks->append(BlockID(number));
            }
        }
        // Whatever reads a value stands behind it in the stream, and no further than its last use.
        uint32_t lastUse = 0;
        for (ValueID value : readBehind)
            lastUse = std::max(lastUse, liveness.lastUse(value).index());
        for (auto handlerBlock = std::ranges::upper_bound(*handlerBlocks, m_block.id()); handlerBlock != handlerBlocks->end() && m_index.block(*handlerBlock).firstParameter().index() <= lastUse; ++handlerBlock) {
            for (ValueID value : readBehind) {
                if (liveness.isLiveAtHead(value, *handlerBlock))
                    return false;
            }
        }
    }

    forgetValuesThatNobodyReadsAnyMore();
    for (ValueID value : readBehind) {
        if (!frame().valuesBuiltTwice.contains(value))
            frame().valuesBuiltTwice.append(value);
    }
    // A frame walks one block at a time.
    frame().blocksWalkedForEachWay.shrink(0);
    frame().blocksWalkedForEachWay.append(m_block.id());
    return true;
}

bool KnownThreading::letArmsWalkEndOfBlockFrom(uint32_t begin)
{
    uint32_t numberOfOperations = m_block.numberOfValues();
    if (begin > numberOfOperations || numberOfOperations - begin > mostOperationsOfJoinWalkedForEachWay)
        return false;
    if (!goesByOneOperand(m_block.terminator().kind()) || m_block.hasHandler() || m_index.hasIrreducibleFlow())
        return false;
    // No value is live for the sake of a hint, so a later block would let go of what the arm that came last has built.
    if (m_index.holdsParametersReadByReleaseHintsAlone() || m_index.opcodes().contains(Opcode::ReleaseHint))
        return false;
    return operationsMayBeWalkedAgain(begin, numberOfOperations) && terminatorMayBeWalkedAgain(false) && letTerminatorBeWalkedForEachWay(m_block.firstValue());
}

// ---- Operations that a way decides

uint32_t KnownThreading::numberOfOperationsBuiltIn(uint32_t begin, uint32_t end)
{
    uint32_t number = 0;
    for (uint32_t place = begin; place < end; ++place)
        number += !operationsThatBuildNothingInKnownThreading.contains(m_index.opcodeOf(ValueID(firstValue() + place)));
    return number;
}

// ---- Arms

// As at any join, what arrives under one name keeps it. The walk goes on where the arms have met, or nowhere where none has come through.
void KnownThreading::meet(Vector<Arm, 4>& arms, std::span<const ValueID> values)
{
    Walker& walker = m_walker;
    walker.m_block = { };
    walker.m_state.clear();
    if (arms.isEmpty() || walker.m_hasFailed)
        return;
    if (arms.size() == 1) {
        walker.m_block = arms[0].block;
        walker.m_state = WTF::move(arms[0].state);
        for (size_t position = 0; position < values.size(); ++position)
            walker.bind(values[position], arms[0].values[position]);
        return;
    }

    BlockFrequency frequency = BlockFrequency::Rare;
    for (const Arm& arm : arms)
        frequency = std::min(frequency, walker.m_code.at(arm.block).frequency);
    BlockID meeting = walker.addBlock(frequency);
    Vector<size_t, 8> positionsOfParameters;
    Vector<Representation, 4> forms;
    for (size_t position = 0; position < values.size(); ++position) {
        Reference first = arms[0].values[position];
        bool allBringOneValue = std::ranges::all_of(arms, [&](const Arm& arm) {
            return arm.values[position] == first;
        });
        if (allBringOneValue) {
            walker.bind(values[position], first);
            continue;
        }
        TypeID type;
        forms.shrink(0);
        for (const Arm& arm : arms) {
            TypeID arriving = walker.ownTypeOf(arm.values[position]);
            type = type ? walker.joinTypes(type, arriving) : arriving;
            forms.append(walker.representationOf(arm.values[position]));
        }
        walker.bind(values[position], walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span())));
        positionsOfParameters.append(position);
    }

    // jumpTo() lets leave whatever a parameter becomes a second name of, and brings each argument into the form of its parameter.
    WalkState met;
    Vector<Reference, 8> arguments;
    for (size_t number = 0; number < arms.size() && !walker.m_hasFailed; ++number) {
        arguments.shrink(0);
        for (size_t position : positionsOfParameters)
            arguments.append(arms[number].values[position]);
        walker.m_block = arms[number].block;
        walker.m_state = WTF::move(arms[number].state);
        walker.jumpTo(meeting, arguments.span());
        if (number)
            met.meetWith(walker.m_state);
        else
            met = WTF::move(walker.m_state);
    }
    walker.m_state = WTF::move(met);
}

// ---- A join

void KnownThreading::walkBlockOnWays(std::span<const uint32_t> places, uint32_t namesAtHead)
{
    Walker& walker = m_walker;
    if (places.empty() || walker.m_hasFailed)
        return;
    Walker::InputBlock& record = walker.inputBlock(m_frameIndex, m_block.id());
    record = Walker::InputBlock { };
    record.firstNarrowerName = namesAtHead;
    for (uint32_t place : places)
        walker.m_openEdges[place].next = std::exchange(record.firstOpenEdge, place);
    // The block is stood in anew.
    frame().block = { };
    walker.walkBlock(m_block.id());
}

// ---- What stands behind an operation that several ways have come through

// ---- An arm that walks the terminator of its block

bool KnownThreading::endArmWithBranch()
{
    Walker& walker = m_walker;
    uint32_t numberOfOperations = m_block.numberOfValues();
    m_begin = frame().walkedUpTo;
    if (frame().block != m_block.id() || frame().endOfStretch != numberOfOperations || !letArmsWalkEndOfBlockFrom(m_begin))
        return false;

    walker.walkStretch(m_block.id(), m_begin, numberOfOperations);
    // The next arm walks them for itself.
    frame().walkedUpTo = m_begin;
    walker.walkTerminator(m_block.terminator());
    forgetValuesFrom(ValueID(firstValue() + m_begin));
    return true;
}

// ---- The ways of closed arms

// The block that is asked about, the one that takes the proof of an edge in front of it, and one more.
static constexpr uint32_t mostBlocksLookedThroughInKnownThreading = 3;

bool KnownThreading::mayBeWalkedApartFromClosedWays()
{
    BlockID block = m_block.id();
    if (m_index.isLoopHead(block) || m_block.isHandlerBlock() || m_index.isEnteredByHandlerEdge(block) || m_block.hasHandler() || m_index.hasIrreducibleFlow())
        return false;
    // No value is live for the sake of a hint, so a later block would let go of what the way that came last has built.
    if (m_index.holdsParametersReadByReleaseHintsAlone() || m_index.opcodes().contains(Opcode::ReleaseHint))
        return false;
    TerminatorKind end = m_block.terminator().kind();
    bool returns = end == TerminatorKind::Return || end == TerminatorKind::ReturnCall;
    if (returns ? m_frameIndex && !frame().returnsInPlaceOfUnit : !goesByOneOperand(end) && end != TerminatorKind::Jump)
        return false;
    uint32_t numberOfOperations = m_block.numberOfValues();
    return numberOfOperationsBuiltIn(0, numberOfOperations) <= mostOperationsOfJoinWalkedForEachWay && operationsMayBeWalkedAgain(0, numberOfOperations);
}

// As terminatorMayBeWalkedAgain(). An edge that comes round to the head of a loop being walked is closed where it is led, so each way has its own as each `continue` has.
bool KnownThreading::terminatorMayBeWalkedByWaysApart()
{
    // As there: the looks at a loop have promised what a relation leaves of its operands, and the walk keeps that only where the edge enters alone.
    if (m_walker.isInLoop() && boundsProvenOnEdge(m_index, m_block.terminator(), 0))
        return false;
    for (BodyTarget target : m_block.terminator().targets()) {
        BlockID to = target.block();
        if (m_walker.inputBlock(m_frameIndex, to).head && m_index.isInLoop(m_block.id(), m_index.loopOf(to)))
            continue;
        if (to == m_block.id() || m_index.isLoopHead(to) || m_index.isEnteredByHandlerEdge(to) || m_index.block(to).isHandlerBlock() || m_walker.inputBlock(m_frameIndex, to).isEntered)
            return false;
    }
    return true;
}

template<typename Functor>
bool KnownThreading::looksAtValueFromInFront(const BodyValue& operation, const Functor& isLeanedOn)
{
    if (!operationsTypedByWayInKnownThreading.contains(operation.opcode()))
        return false;
    bool looks = false;
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        // `(type as UnionType).objectFlags`: a cast that stands in the block hands on what comes from in front of it.
        while (operand >= m_block.firstParameter() && !m_index.isParameter(operand) && inputOperationsThatHandOn.contains(m_index.opcodeOf(operand)))
            operand = m_index.value(operand).operand(0);
        if (looks || operand >= m_block.firstParameter() || operation.operandUses(operandIndex).containsOnly({ OperandUse::Retains, OperandUse::Returns }))
            return;
        looks = isLeanedOn(operand);
    });
    return looks;
}

// A target of a Branch takes the proof of the edge and jumps on: `if (..) .. else if (type.flags & B) ..` reads in the block behind it.
// So the blocks behind jumps, each entered by one edge as written and small enough to be walked apart itself, are looked at too: ways that are apart in the first are apart in the next.
template<typename Functor>
bool KnownThreading::looksAtValueFromInFrontHereOrBehindJumps(const Functor& isLeanedOn)
{
    Vector<BlockID, 16> blocks { m_block.id() };
    for (uint32_t number = 0; number < blocks.size() && number < mostBlocksLookedThroughInKnownThreading; ++number) {
        KnownThreading here(m_walker, blocks[number]);
        if (number && (m_index.numberOfIncomingEdges(blocks[number]) != 1 || !here.mayBeWalkedApartFromClosedWays() || !here.terminatorMayBeWalkedByWaysApart()))
            continue;
        for (uint32_t place = 0; place < here.m_block.numberOfValues(); ++place) {
            if (looksAtValueFromInFront(m_index.value(ValueID(here.firstValue() + place)), isLeanedOn))
                return true;
        }
        BodyTerminator end = here.m_block.terminator();
        if (end.kind() == TerminatorKind::Jump)
            blocks.append(end.targets()[0].block());
    }
    return false;
}

// Asked ahead, of a target of the terminator of the block being walked, where the arms of a guess at `value` are about to be built.
bool KnownThreading::wouldBeWalkedApartFor(Reference value)
{
    if (m_index.numberOfIncomingEdges(m_block.id()) != 1 || m_walker.inputBlock(m_frameIndex, m_block.id()).isEntered)
        return false;
    if (!mayBeWalkedApartFromClosedWays() || !terminatorMayBeWalkedByWaysApart())
        return false;
    return looksAtValueFromInFrontHereOrBehindJumps([&](ValueID operand) {
        return m_walker.referenceOf(operand) == value;
    });
}

// Some operation of the block looks at a value of which the passing ways together know more than some way of a closed arm does.
// What a way knows: the type of the name that it brings for the value, or of the name that a test has proven on it (WalkState::checkedNameOf()).
bool KnownThreading::leansOnWhatWaysHaveProven(std::span<const uint32_t> passing, std::span<const uint32_t> closed)
{
    Walker& walker = m_walker;
    auto typeOnWay = [&](uint32_t place, ValueID value) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        Reference name = walker.referenceOf(value);
        for (auto [builtTwice, nameOnWay] : open.names) {
            if (builtTwice == value)
                name = nameOnWay;
        }
        while (Reference checked = name ? open.state.checkedNameOf(name) : Reference())
            name = checked;
        return walker.ownTypeOf(name);
    };
    return looksAtValueFromInFrontHereOrBehindJumps([&](ValueID operand) {
        TypeID proven;
        for (uint32_t place : passing) {
            TypeID type = typeOnWay(place, operand);
            if (!type)
                return false;
            proven = proven ? walker.joinTypes(proven, type) : type;
        }
        return std::ranges::any_of(closed, [&](uint32_t place) {
            TypeID type = typeOnWay(place, operand);
            return type && !walker.types().isSubset(type, proven);
        });
    });
}

// `if (type.flags & A) .. else if (type.flags & B) ..`: the arms of a guess have each walked the branch of their block, and the block behind it asks for what the passing arm has proven.
// The ways of closed arms walk it for themselves, closed and for size, and lead on as such. The others meet among themselves, so the proof, and all that their states hold, goes on.
// Two walks a block at most, whatever the number of ways: nothing multiplies.
bool KnownThreading::walkApartFromClosedWays()
{
    Walker& walker = m_walker;
    BlockID block = m_block.id();
    Vector<uint32_t, 4> passing;
    Vector<uint32_t, 4> closed;
    uint32_t firstWay = walker.inputBlock(m_frameIndex, block).firstOpenEdge;
    for (uint32_t place = firstWay; place != Walker::noOpenEdge; place = walker.m_openEdges[place].next) {
        const Walker::OpenEdge& open = walker.m_openEdges[place];
        if (open.block || open.arguments.size() != m_block.numberOfParameters() || open.names.size() != walker.m_openEdges[firstWay].names.size())
            return false;
        (open.isLedByClosedArm ? closed : passing).append(place);
    }
    if (passing.isEmpty() || closed.isEmpty() || walker.isWalkingBlockForEachWay(m_frameIndex) || !mayBeWalkedApartFromClosedWays())
        return false;
    // In the order in which they were led.
    passing.reverse();
    closed.reverse();
    if (!leansOnWhatWaysHaveProven(passing.span(), closed.span()))
        return false;
    if (!terminatorMayBeWalkedByWaysApart() || !letTerminatorBeWalkedForEachWay(m_block.firstParameter()))
        return false;

    uint32_t namesAtHead = walker.inputBlock(m_frameIndex, block).firstNarrowerName;
    {
        Walker::ClosedArm closedArm(walker);
        ++walker.m_numberOfCopiesBuiltForSize;
        walkBlockOnWays(closed.span(), namesAtHead);
        --walker.m_numberOfCopiesBuiltForSize;
    }
    walkBlockOnWays(passing.span(), namesAtHead);

    // Behind the block nothing of either walk is at hand but what the edges bring.
    frame().blocksWalkedForEachWay.shrink(0);
    walker.inputBlock(m_frameIndex, block).firstNarrowerName = namesAtHead;
    forgetValuesFrom(m_block.firstParameter());
    walker.m_block = { };
    walker.m_state.clear();
    return true;
}

// ---- The doors

bool Walker::isWalkingBlockForEachWay(size_t frameIndex) const
{
    const Frame& frame = m_frames[frameIndex];
    return frame.blocksWalkedForEachWay.contains(frame.block);
}

// Behind the link of the chain nobody goes on with the call. buildFreshObject() asks the same function and notes what the call has given, and whoever builds arms of his own has other ways that wait behind the call.
bool Walker::inlineCalleeAsLinkOfChainOfItsOpcodes(const BodyValue& call)
{
    SetForScope callCopiedByLinkOfChain(m_callCopiedByLinkOfChain, call.id());
    SetForScope numberOfFramesAtThatCall(m_numberOfFramesAtCallCopiedByLinkOfChain, m_frames.size());
    return inlineCallee(*this, call);
}

bool Walker::armsTakeBranchForProofOf(BlockID inputBlock, uint32_t end, Reference value)
{
    const Frame& frame = m_frames.last();
    BodyBlock block = frame.index->block(inputBlock);
    BodyTerminator terminator = block.terminator();
    if (!value.isValue() || !goesByOneOperand(terminator.kind()) || frame.endOfStretch != block.numberOfValues() || block.numberOfValues() - end > mostOperationsOfJoinWalkedForEachWay)
        return false;
    // A text in its builder goes over an edge only into a parameter that was made to hold a builder, and a name that two ways bring is joined in a plain one.
    if (hasTextsInStringBuilders())
        return false;
    for (BodyTarget target : terminator.targets()) {
        if (KnownThreading(*this, target.block()).wouldBeWalkedApartFor(value))
            return true;
    }
    return false;
}

bool Walker::walkBlockApartFromClosedWays(BlockID inputBlock)
{
    if (m_hasFailed || !has(Mechanism::Guesses) || !m_opensGuesses || m_numberOfCopiesBuiltForSize || hasTextsInStringBuilders())
        return false;
    if (m_openEdges[this->inputBlock(m_frames.size() - 1, inputBlock).firstOpenEdge].next == noOpenEdge)
        return false;
    return KnownThreading(*this, inputBlock).walkApartFromClosedWays();
}

bool Walker::endArmWithBranchOf(BlockID inputBlock)
{
    if (isUnreachable() || m_hasFailed || bodyKeepsValuesInWordsOfHeapInKnownThreading(*this))
        return false;
    return KnownThreading(*this, inputBlock).endArmWithBranch();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
