/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFacts.h"
#include "AOTID.h"
#include "AOTOpcode.h"
#include "AOTOperandKinds.h"
#include "AOTResultRules.h"
#include "AOTSchemaRecords.h"
#include "AOTSchemaSupport.h"
#include "AOTTypeKinds.h"
#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class Arrivals;
class CallingConventions;
class Code;
class Plan;
class Program;
class TypeArena;

using EdgeID = ID<struct EdgeIDTag>;
using VirtualObjectID = ID<struct VirtualObjectIDTag>;

// What an operand of an operation under construction names: a record of Code, a constant (a record of Code that lies in no block), an object that has not been built,
// or an operation that waits for its user (Walker::postpone()).
class Reference {
public:
    enum class Kind : uint8_t { Value, Constant, Virtual, Postponed };

    static constexpr uint32_t kindShift = 30;
    static constexpr uint32_t indexMask = (1u << kindShift) - 1;

    constexpr Reference() = default;
    constexpr Reference(std::nullptr_t) { }

    static constexpr Reference value(ValueID id) { return make(Kind::Value, id.index()); }
    static constexpr Reference constant(ValueID id) { return make(Kind::Constant, id.index()); }
    static constexpr Reference virtualObject(VirtualObjectID id) { return make(Kind::Virtual, id.index()); }
    static constexpr Reference postponed(uint32_t index) { return make(Kind::Postponed, index); }
    static constexpr Reference fromBits(uint32_t bits)
    {
        Reference reference;
        reference.m_bits = bits;
        return reference;
    }

    constexpr uint32_t bits() const { return m_bits; }
    // Of a reference that is not empty.
    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> kindShift); }
    constexpr explicit operator bool() const { return m_bits != UINT32_MAX; }
    constexpr bool isValue() const { return kind() == Kind::Value; }
    constexpr bool isConstant() const { return kind() == Kind::Constant; }
    constexpr bool isVirtual() const { return kind() == Kind::Virtual; }
    constexpr bool isPostponed() const { return kind() == Kind::Postponed && m_bits != UINT32_MAX; }
    constexpr bool isRecord() const { return isValue() || isConstant(); }

    // Of a value or a constant.
    constexpr ValueID id() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(isRecord());
        return ValueID(m_bits & indexMask);
    }
    constexpr VirtualObjectID object() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(isVirtual());
        return VirtualObjectID(m_bits & indexMask);
    }
    constexpr uint32_t postponedIndex() const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(isPostponed());
        return m_bits & indexMask;
    }

    friend constexpr bool operator==(Reference, Reference) = default;

private:
    static constexpr Reference make(Kind kind, uint32_t index)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(index < indexMask);
        return fromBits(static_cast<uint32_t>(kind) << kindShift | index);
    }

    uint32_t m_bits { UINT32_MAX };
};

static_assert(sizeof(Reference) == sizeof(uint32_t) && !Reference() && !Reference().isPostponed() && !Reference().isRecord() && !Reference().isVirtual());

// An array whose elements never move, so that a reference to one outlives every later append. Shrinking keeps the memory: a thread's arrays are filled again by its next function.
template<typename T, unsigned logOfSegmentSize>
class StableArray {
    WTF_MAKE_NONCOPYABLE(StableArray);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    static constexpr uint32_t segmentSize = 1u << logOfSegmentSize;

    StableArray() = default;
    ~StableArray()
    {
        for (T* segment : m_segments)
            fastFree(segment);
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }

    T& operator[](uint32_t index) LIFETIME_BOUND
    {
        ASSERT(index < m_size);
        return m_segments[index >> logOfSegmentSize][index & (segmentSize - 1)];
    }
    const T& operator[](uint32_t index) const LIFETIME_BOUND
    {
        ASSERT(index < m_size);
        return m_segments[index >> logOfSegmentSize][index & (segmentSize - 1)];
    }
    T& last() LIFETIME_BOUND { return (*this)[m_size - 1]; }
    const T& last() const LIFETIME_BOUND { return (*this)[m_size - 1]; }

    uint32_t append(const T& value)
    {
        uint32_t index = m_size;
        if ((index >> logOfSegmentSize) == m_segments.size()) [[unlikely]]
            m_segments.append(static_cast<T*>(fastMalloc(sizeof(T) * segmentSize)));
        ++m_size;
        (*this)[index] = value;
        return index;
    }
    void grow(uint32_t newSize, const T& fill)
    {
        while (m_size < newSize)
            append(fill);
    }
    void shrink(uint32_t newSize)
    {
        ASSERT(newSize <= m_size);
        m_size = newSize;
    }
    void clear() { m_size = 0; }

private:
    Vector<T*, 4> m_segments;
    uint32_t m_size { 0 };
};

// The side array: runs of words that never move. Long operand lists, payload lists, block parameters and edge arguments lie here.
class SideWords {
    WTF_MAKE_NONCOPYABLE(SideWords);
public:
    struct Mark {
        uint32_t segment { 0 };
        uint32_t used { 0 };
    };

    SideWords() = default;
    ~SideWords();

    // A run longer than a segment gets a segment of its own size.
    uint32_t* allocate(uint32_t numberOfWords);
    template<typename T>
    std::span<const T> copy(std::span<const T> elements)
    {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) == sizeof(uint32_t));
        if (elements.empty())
            return { };
        uint32_t* words = allocate(static_cast<uint32_t>(elements.size()));
        memcpy(words, elements.data(), elements.size_bytes());
        return { reinterpret_cast<const T*>(words), elements.size() };
    }

    Mark mark() const { return { m_current, m_used }; }
    void rewindTo(Mark);
    void clear() { rewindTo({ }); }

private:
    static constexpr uint32_t wordsPerSegment = 1u << 14;

    struct Segment {
        uint32_t* words { nullptr };
        uint32_t capacity { 0 };
    };

    Vector<Segment, 4> m_segments;
    uint32_t m_current { 0 };
    uint32_t m_used { 0 };
};

// What Operation and Terminator share. The words of a record are written when Code appends it and never again.
// The positions of payload words, flags and operands are the schema's, as AOTSchemaPayloadPositions.h, AOTSchemaFlagMasks.h and AOTSchemaOperandPositions.h give them.
class Instruction {
public:
    using OperandID = ValueID;

    static constexpr uint32_t noPosition = UINT32_MAX;
    static constexpr uint32_t numberOfInlineWords = 4;

    std::span<const ValueID> operands() const LIFETIME_BOUND
    {
        if (m_hasSideWords)
            return { reinterpret_cast<const ValueID*>(sideWords() + numberOfSidePayloadWords()), m_words[3] };
        return { reinterpret_cast<const ValueID*>(m_words.data()), m_numberOfInlineOperands };
    }
    uint32_t numberOfOperands() const { return m_hasSideWords ? m_words[3] : m_numberOfInlineOperands; }
    uint32_t numberOfNamedOperands() const { return m_hasSideWords ? m_words[2] & maxNumberOfNamedOperands : m_numberOfInlineOperands; }
    uint32_t numberOfVariadicOperands() const { return numberOfOperands() - numberOfNamedOperands(); }
    ValueID operand(uint32_t operandIndex) const
    {
        std::span<const ValueID> all = operands();
        RELEASE_ASSERT(operandIndex < all.size());
        return all[operandIndex];
    }
    ValueID child(unsigned index) const { return index < numberOfNamedOperands() ? operand(index) : ValueID(); }
    std::span<const ValueID> variadicOperands() const LIFETIME_BOUND { return operands().subspan(numberOfNamedOperands()); }
    std::optional<uint32_t> variadicOperandIndex(uint32_t operandIndex) const
    {
        uint32_t numberOfNamedOperands = this->numberOfNamedOperands();
        if (operandIndex < numberOfNamedOperands)
            return std::nullopt;
        return operandIndex - numberOfNamedOperands;
    }

    void forEachOperand(NOESCAPE const Invocable<void(ValueID, uint32_t operandIndex)> auto& functor) const
    {
        uint32_t operandIndex = 0;
        for (ValueID operand : operands()) {
            if (operand)
                functor(operand, operandIndex);
            ++operandIndex;
        }
    }
    void forEachChild(NOESCAPE const Invocable<void(ValueID)> auto& functor) const
    {
        for (ValueID operand : operands()) {
            if (operand)
                functor(operand);
        }
    }

    std::span<const uint32_t> payload() const LIFETIME_BOUND
    {
        if (m_hasSideWords)
            return { sideWords(), numberOfSidePayloadWords() };
        return { m_words.data() + m_numberOfInlineOperands, m_numberOfInlinePayloadWords };
    }
    uint32_t payloadWordAt(unsigned word) const
    {
        std::span<const uint32_t> words = payload();
        if (word < words.size()) [[likely]]
            return words[word];
        RELEASE_ASSERT(word < numberOfInlinePayloadWords);
        return absentPayloadWord;
    }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND
    {
        std::span<const uint32_t> words = payload();
        if (firstWord >= words.size())
            return { };
        return words.subspan(firstWord);
    }
    template<typename Element>
    std::span<const Element> payloadListOf(unsigned firstWord) const LIFETIME_BOUND
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        return spanReinterpretCast<const Element>(payloadListFrom(firstWord));
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame, !!isTailCall, !!isPartOfConstruction }; }

private:
    std::array<uint32_t, numberOfInlineWords> m_words { };
public:
    uint32_t position { noPosition };
    uint32_t inlineFrame : 24 { 0 };
    uint32_t flags : 8 { 0 };
    uint8_t isTailCall : 1 { false };
    uint8_t isPartOfConstruction : 1 { false };
private:
    uint8_t m_hasSideWords : 1 { false };
    uint8_t m_numberOfInlineOperands : 3 { 0 };
    uint8_t m_numberOfInlinePayloadWords : 2 { 0 };

protected:
    Instruction() = default;

private:
    friend class Code;

    static constexpr uint32_t maxNumberOfNamedOperands = 15;

    const uint32_t* sideWords() const { return std::bit_cast<const uint32_t*>(std::array<uint32_t, 2> { m_words[0], m_words[1] }); }
    uint32_t numberOfSidePayloadWords() const { return m_words[2] >> 4; }
};

// The entries of the schema that only Code holds. No body of the input has them. This is the text of ir/schema/code-operations.def without its comments:
//
//   family Cast Placed..Placed
//   engine operation Narrowed (value: Convertible/returns, test?: Convertible/inspects, operandIndex: UInt32, caseIndex?: UInt32) [| engine isOnTakenEdge narrowsHolderOfOperand narrowsOtherResultOfOperand] -> natural(same(value)) ; none
//
// The family names the emitter: CastEmitter::generateNarrowed(). The generator has no rule for Narrowed and needs none: by the schema it has the type of `value`,
// which is what resultTypeOf() answers, an upper bound. Walker::append() and verifyTypesOfCode() ask typeOfNarrowed() in its place.
// It bounds the values that the Narrowed may hold, not its type under isSubset(): in Compile a named object that only may fit a test gives way to `unknown` within the tested layouts it may have (narrowByTest()),
// so the type of a Narrowed lies inside what was tested and need not lie inside the type of `value`.
// The kind Convertible leaves each operand in the form it has: nothing is boxed for a Narrowed.
//
// Narrowed is what the edge behind a test hands over: `value` under the type that the test leaves of it. It is the only way in which a value gets a narrower type in Code.
// It stands at the head of a block that has one incoming edge, in front of every other operation but operationsThatStandAmongNarrowedValues. That edge leaves a Branch or a TestAndBranch on `test`, or on what LogicalNot and ToBoolean alone
// have made of `test`, on the side on which `test` is true where isOnTakenEdge is set and false where it is not,
// or, with no `test`, a Switch, SwitchString or SwitchOnShape on `value`: case `caseIndex`, or `otherwise` where there is none.
// `value` is operand `operandIndex` of `test`. With narrowsHolderOfOperand that operand is a read of a slot of `value`.
// A `test` that proves nothing of its operands is a value that the branch asks directly, `if (r.ok)`: it stands in the place of that operand itself, and `operandIndex` says nothing.
// With narrowsOtherResultOfOperand operand `operandIndex` of `test` gives several results: `test` is the ResultAt that reads one of them, and `value` the ResultAt that reads another.
// With no `test` and narrowsHolderOfOperand, the subject of the Switch or SwitchString is a read of a slot of `value`.
// Where the read of the slot tests its own guess the Narrowed proves no more than `value`: it is the name under which the walk guesses more on that edge.
// Its type is typeOfNarrowed(). It makes no code where its representation is that of `value`, and else the conversion that needs no test.

// A record of Code. A constant has no block.
// `type` is proven, and nobody states it: it is what the result rule of the opcode gives for the operation's own words, the proven types of its operands and the facts that Code::factsUsedBy() lists.
struct Operation : Instruction {
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    TypeID type;
    // The operation in front of it in its block.
    ValueID previous;
    uint32_t blockIndex : 32 - EffectTraits::numberOfBits = noBlock;
    uint32_t traitBits : EffectTraits::numberOfBits = 0;

    static constexpr uint32_t noBlock = (1u << (32 - EffectTraits::numberOfBits)) - 1;

    BlockID block() const { return blockIndex == noBlock ? BlockID() : BlockID(blockIndex); }
    // Of the operation with the operands it has, a callee's summary applied: what the walk struck Known by.
    EffectTraits traits() const { return EffectTraits::fromBits(traitBits); }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }

#include "AOTSchemaOperationGetters.inc"
};

static_assert(sizeof(Operation) <= 48 && std::is_trivially_copyable_v<Operation>);

// `to` is null while the edge is open. Code::closeEdge() fills `to` and the arguments, once.
struct Edge {
    BlockID from;
    BlockID to;

    bool isOpen() const { return !to; }
    std::span<const ValueID> arguments() const LIFETIME_BOUND { return { m_arguments, m_numberOfArguments }; }

private:
    friend class Code;

    const ValueID* m_arguments { nullptr };
    uint32_t m_numberOfArguments { 0 };
};

struct Terminator : Instruction {
    TerminatorKind kind { TerminatorKind::Unreachable };
    bool isSet { false };
    EdgeID firstEdge;
    uint32_t numberOfEdges { 0 };

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind, operandIndex); }

    bool isConditionalBranch() const { return kind == TerminatorKind::Branch || kind == TerminatorKind::TestAndBranch; }
    bool hasCases() const { return !numberOfFixedTargetsOf(kind); }
    uint32_t numberOfCases() const { return hasCases() ? numberOfEdges - 1 : 0; }

    EdgeID edge(uint32_t targetIndex) const
    {
        RELEASE_ASSERT(targetIndex < numberOfEdges);
        return EdgeID(firstEdge.index() + targetIndex);
    }
    EdgeID next() const { RELEASE_ASSERT(kind == TerminatorKind::Jump); return edge(0); }
    EdgeID taken() const { RELEASE_ASSERT(isConditionalBranch()); return edge(0); }
    EdgeID notTaken() const { RELEASE_ASSERT(isConditionalBranch()); return edge(1); }
    EdgeID caseEdge(uint32_t caseIndex) const { RELEASE_ASSERT(caseIndex < numberOfCases()); return edge(caseIndex); }
    EdgeID otherwise() const { RELEASE_ASSERT(hasCases()); return edge(numberOfEdges - 1); }
    EdgeID done() const { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return edge(0); }
    EdgeID miss() const { RELEASE_ASSERT(kind == TerminatorKind::TryCallInline); return edge(1); }

#include "AOTSchemaTerminatorGetters.inc"
};

struct Block {
    Terminator terminator;
    // The edge that a throw in the block takes. As the walk left it: Code::handlerOf() knows what the sweep took away.
    EdgeID handler;
    ValueID subject;
    BlockRole role { BlockRole::Ordinary };
    BlockFrequency frequency { BlockFrequency::Normal };
    // In a loop of this function, after inlining.
    bool isInLoop { false };
    // Walked as the head of a loop, so the types of its parameters are guesses that hold whatever arrives. Also where no edge has come back to it, and it lies in no loop of its own.
    bool isLoopHead { false };
    // Entered from outside the function's own edges: the entry of the function. A resumption of a coroutine enters no block: it goes on behind the call that suspended.
    bool isEntry { false };

    bool isHandlerBlock() const { return role != BlockRole::Ordinary; }
    bool hasEnded() const { return terminator.isSet; }
    std::span<const ValueID> parameters() const LIFETIME_BOUND { return { m_parameters, m_numberOfParameters }; }
    // The first edge that the walk has closed onto the block. Null: none yet.
    EdgeID firstEdgeIn() const { return m_firstEdgeIn; }

private:
    friend class Code;
    friend class Sweep;

    const ValueID* m_parameters { nullptr };
    uint32_t m_numberOfParameters { 0 };
    EdgeID m_firstEdgeIn;
    ValueID m_last;
    uint32_t m_firstInOrder { 0 };
    uint32_t m_numberInOrder { 0 };
};

// A way into the code from outside. Entry 0 is the unit's primary entry. The parameters of `block` arrive by the unit's convention.
struct EntryOfCode {
    BlockID block;
};

// A fact of Facts that the walk has leaned on for a record. Every use of a fact goes through Walker::leanOn(), which appends one of these.
struct FactUse {
    ValueID record;
    FactKind kind { };
    // What the fact speaks of, by `subjectKind`.
    FactSubject::Kind subjectKind { FactSubject::Kind::Variable };
    // A function, a variable, a layout or a cell.
    uint32_t subject { UINT32_MAX };
    // An entry parameter, a result, a field.
    uint32_t index { 0 };
    TypeID type;
    PropertyKeyID key;
};

enum class SweepMark : uint8_t {
    IsNeeded = 1 << 0,
    // A ReleaseHint behind the last use of its operand. A ReleaseEnvironment of a cell that this code has made, behind which nothing reads the cell under any name.
    ReleasesOperand = 1 << 1,
    // A store whose owner meets a later barrier before anything can collect.
    LaterBarrierCovers = 1 << 2,
    // A LoopHint with FateOfPoll::LeavesHandler: it is emitted as one that has the flag leavesOwedError.
    LeavesOwedError = 1 << 3,
    // A LoopHint with FateOfPoll::IsDropped. It is never needed.
    PollsForNothing = 1 << 4,
    // An allocation behind which every way comes to the fence of a later allocation before a cell is handed on to where the collector finds it.
    LaterFenceCovers = 1 << 5,
};

// What the walk learns of a LoopHint when the loop is closed. The record stands in front of the loop's body and never changes, so the sweep is told beside it.
enum class FateOfPoll : uint8_t {
    // The poll of a loop under a handler. Nothing is owed where a turn comes to it, so it throws nothing that the handler could catch.
    LeavesHandler,
    // The poll of a loop that has no back edge, or one in front of a loop that was made for the sake of a poll that stays under its handler.
    IsDropped,
};

struct PollWithFate {
    ValueID poll;
    FateOfPoll fate { FateOfPoll::IsDropped };
};

// An operation under construction. It has what the old Value had to read and to change, with references for operands.
class DraftWords {
public:
    using OperandID = Reference;

    static constexpr uint32_t noPosition = UINT32_MAX;

    std::span<const Reference> operands() const LIFETIME_BOUND { return m_operands.span(); }
    std::span<Reference> mutableOperands() LIFETIME_BOUND { return m_operands.mutableSpan(); }
    uint32_t numberOfOperands() const { return m_operands.size(); }
    uint32_t numberOfNamedOperands() const { return m_numberOfNamedOperands; }
    uint32_t numberOfVariadicOperands() const { return numberOfOperands() - m_numberOfNamedOperands; }
    Reference operand(uint32_t operandIndex) const { return m_operands[operandIndex]; }
    Reference child(unsigned index) const { return index < m_numberOfNamedOperands ? m_operands[index] : Reference(); }
    void setOperand(uint32_t operandIndex, Reference reference) { m_operands[operandIndex] = reference; }

    std::span<const Reference> variadicOperands() const LIFETIME_BOUND { return operands().subspan(m_numberOfNamedOperands); }
    void reserveVariadicOperands(uint32_t count) { m_operands.reserveCapacity(m_numberOfNamedOperands + count); }
    void appendVariadicOperand(Reference reference) { m_operands.append(reference); }
    void setVariadicOperands(std::span<const Reference> references)
    {
        Vector<Reference, 8> copy(references);
        m_operands.shrink(m_numberOfNamedOperands);
        m_operands.appendVector(copy);
    }

    void forEachOperand(NOESCAPE const Invocable<void(Reference, uint32_t operandIndex)> auto& functor) const
    {
        for (uint32_t operandIndex = 0; operandIndex < m_operands.size(); ++operandIndex) {
            if (m_operands[operandIndex])
                functor(m_operands[operandIndex], operandIndex);
        }
    }

    std::span<const uint32_t> payload() const LIFETIME_BOUND { return m_payload.span(); }
    uint32_t payloadWordAt(unsigned word) const
    {
        if (word < m_payload.size()) [[likely]]
            return m_payload[word];
        RELEASE_ASSERT(word < numberOfInlinePayloadWords);
        return absentPayloadWord;
    }
    void setPayloadWordAt(unsigned word, uint32_t value) { m_payload[word] = value; }
    uint64_t payloadWordPairAt(unsigned word) const { return static_cast<uint64_t>(payloadWordAt(word + 1)) << 32 | payloadWordAt(word); }
    void setPayloadWordPairAt(unsigned word, uint64_t value)
    {
        setPayloadWordAt(word, static_cast<uint32_t>(value));
        setPayloadWordAt(word + 1, static_cast<uint32_t>(value >> 32));
    }
    std::span<const uint32_t> payloadListFrom(unsigned firstWord) const LIFETIME_BOUND
    {
        if (firstWord >= m_payload.size())
            return { };
        return m_payload.span().subspan(firstWord);
    }
    void setPayloadListFrom(unsigned firstWord, std::span<const uint32_t> words)
    {
        Vector<uint32_t, 16> copy(words);
        while (m_payload.size() < firstWord)
            m_payload.append(absentPayloadWord);
        m_payload.shrink(firstWord);
        m_payload.appendVector(copy);
    }
    template<typename Element>
    std::span<const Element> payloadListOf(unsigned firstWord) const LIFETIME_BOUND
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        return spanReinterpretCast<const Element>(payloadListFrom(firstWord));
    }
    template<typename Element>
    void setPayloadListOf(unsigned firstWord, std::span<const Element> elements)
    {
        static_assert(std::is_trivially_copyable_v<Element> && alignof(Element) <= alignof(uint32_t) && !(sizeof(Element) % sizeof(uint32_t)));
        setPayloadListFrom(firstWord, spanReinterpretCast<const uint32_t>(elements));
    }

    bool hasPosition() const { return position != noPosition; }
    Origin origin() const { return { position, inlineFrame, isTailCall, isPartOfConstruction }; }
    void setOrigin(Origin origin)
    {
        position = origin.position;
        inlineFrame = origin.inlineFrame;
        isTailCall = origin.isTailCall;
        isPartOfConstruction = origin.isPartOfConstruction;
    }

    uint32_t position { noPosition };
    uint32_t inlineFrame { 0 };
    uint8_t flags { 0 };
    bool isTailCall { false };
    bool isPartOfConstruction { false };

protected:
    DraftWords() = default;

    void resetOperandsAndPayload(uint32_t numberOfNamedOperands, std::span<const uint32_t> defaultPayload)
    {
        flags = 0;
        m_numberOfNamedOperands = numberOfNamedOperands;
        m_operands.fill(Reference(), numberOfNamedOperands);
        m_payload.resize(defaultPayload.size());
        uint32_t* words = m_payload.mutableSpan().data();
        for (size_t index = 0; index < defaultPayload.size(); ++index)
            words[index] = defaultPayload[index];
    }
    void copyWordsOf(const Instruction&, NOESCAPE const Invocable<Reference(ValueID)> auto& referenceOf);

private:
    Vector<Reference, 8> m_operands;
    Vector<uint32_t, 12> m_payload;
    uint32_t m_numberOfNamedOperands { 0 };
};

void DraftWords::copyWordsOf(const Instruction& instruction, NOESCAPE const Invocable<Reference(ValueID)> auto& referenceOf)
{
    setOrigin(instruction.origin());
    flags = instruction.flags;
    m_numberOfNamedOperands = instruction.numberOfNamedOperands();
    m_operands.shrink(0);
    for (ValueID operand : instruction.operands())
        m_operands.append(operand ? referenceOf(operand) : Reference());
    m_payload.shrink(0);
    m_payload.append(instruction.payload());
}

// It has no proven type: Walker::append() computes that by the schema's rule, and no handler can say otherwise.
struct Draft : DraftWords {
    // Unchosen until somebody decides it. Walker::append() asks resultRepresentationOf() for what is left.
    Representation representation { Representation::Unchosen };
    Opcode opcode { Opcode::Undefined };
    // A guess about the result that no rule derives: a written type, an expected result of a call. It never reaches Code. Walker::append() keeps what both it and the type derived at the level Expected say: their intersection.
    ExpectedType expected;
    // The draft stands for the last operation of a block of the body being walked, the unit's own or an inlined callee's, and the ReturnCall that ends the block returns its result. Walker::walkOperation() sets it, and a copy keeps it. A draft that becomes another operation loses it.
    // It is no part of the origin: what a handler builds beside the operation has the origin and not the mark. Whoever makes a new draft that gives the result of a marked one hands the mark on. It never reaches Code.
    bool resultIsReturnedAtOnce { false };
    // An allocation of the walk's own, beside the operation being walked: what becomes of the value of that operation says nothing of it. A copy keeps it. It never reaches Code.
    bool standsForNoValueOfInput { false };
    // The operation of the input whose value is what the draft gives, unchanged. Walker::walkOperation() sets it, and a copy keeps it. A draft that becomes another operation loses it.
    // It is no part of the origin either. Whoever makes a new draft that gives the result of a marked one may hand the mark on: DirectCallRequest::givesResultOfSite. It never reaches Code.
    ValueID inputValueGiven;

    explicit Draft(Opcode newOpcode) { become(newOpcode); }
    Draft(Opcode newOpcode, Origin origin)
    {
        become(newOpcode);
        setOrigin(origin);
    }

    // Keeps the origin. Operands and payload start over.
    void become(Opcode newOpcode)
    {
        opcode = newOpcode;
        representation = Representation::Unchosen;
        expected = { };
        resultIsReturnedAtOnce = false;
        standsForNoValueOfInput = false;
        inputValueGiven = { };
        const FreshWordsOfOperation& fresh = freshWordsOf(newOpcode);
        resetOperandsAndPayload(fresh.numberOfNamedOperands, { fresh.payload, fresh.numberOfPayloadWords });
    }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(opcode, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(opcode, operandIndex); }

#include "AOTSchemaOperationGetters.inc"
// Begins with `using DraftWords::setOperand;`, which setOperand(), the getter of the operand `set` of SetNext, would hide. A second such line in the struct does not compile.
#include "AOTSchemaOperationSetters.inc"
};

struct TerminatorDraft : DraftWords {
    TerminatorKind kind { TerminatorKind::Unreachable };

    explicit TerminatorDraft(TerminatorKind newKind) { become(newKind); }
    TerminatorDraft(TerminatorKind newKind, Origin origin)
    {
        become(newKind);
        setOrigin(origin);
    }

    void become(TerminatorKind newKind)
    {
        kind = newKind;
        resetOperandsAndPayload(numberOfNamedOperandsOf(newKind), defaultPayloadOf(newKind));
    }

    OperandKind operandKind(uint32_t operandIndex) const { return operandKindOf(kind, operandIndex); }
    OperandUses operandUses(uint32_t operandIndex) const { return operandUsesOf(kind, operandIndex); }

#include "AOTSchemaTerminatorGetters.inc"
#include "AOTSchemaTerminatorSetters.inc"
};

// The values that a saved frame holds at a suspension, as the sweep found them live.
struct LiveAtSuspension {
    ValueID suspension;
    uint32_t first { 0 };
    uint32_t count { 0 };
};

// `x = null` behind a suspension: the registers and slots that hold the value are those of the run that read it last, so they are cleared where that run read it last.
// `handle(readAll())` with the body of `handle` walked at the call: the call alone held the value, and the body may run on for long behind its last read of it.
struct ReleaseBehindLastRead {
    BlockID block;
    // An operation of the block. Null: the head of the block, behind what a handler block catches.
    ValueID behind;
    ValueID released;
};

// The walk's output for one unit. A thread has one and fills it again for each unit.
// The walk appends. The sweep adds marks and the order of each block. The emitters read.
class Code {
    WTF_MAKE_NONCOPYABLE(Code);
public:
    Code() = default;

    void reset(CodeUnit);

    CodeUnit unit() const { return m_unit; }
    FunctionID function() const { return m_unit.function; }
    CodeRole role() const { return m_unit.role; }

    // Reading.

    uint32_t numberOfValues() const { return m_operations.size(); }
    uint32_t numberOfBlocks() const { return m_blocks.size(); }
    uint32_t numberOfEdges() const { return m_edges.size(); }

    const Operation& at(ValueID id) const LIFETIME_BOUND { return m_operations[id.index()]; }
    const Block& at(BlockID id) const LIFETIME_BOUND { return m_blocks[id.index()]; }
    const Edge& at(EdgeID id) const LIFETIME_BOUND { return m_edges[id.index()]; }

    bool isConstant(ValueID id) const { return !at(id).block(); }
    BlockID blockOf(ValueID id) const { return at(id).block(); }
    ValueID lastOperationOf(BlockID id) const { return at(id).m_last; }

    // [0] is the function itself.
    std::span<const InlineFrame> inlineFrames() const LIFETIME_BOUND { return m_inlineFrames.span(); }
    std::span<const EntryOfCode> entries() const LIFETIME_BOUND { return m_entries.span(); }
    uint32_t numberOfFrameWords() const { return m_numberOfFrameWords; }

    // In the order in which the walk added them. Ascending by record once the sweep has run.
    std::span<const FactUse> factUses() const LIFETIME_BOUND { return m_factUses.span(); }
    // Behind the sweep.
    std::span<const FactUse> factsUsedBy(ValueID) const LIFETIME_BOUND;
    // In the order in which loops were closed. No poll is listed twice.
    std::span<const PollWithFate> pollsWithFates() const LIFETIME_BOUND { return m_pollsWithFates.span(); }
    // `list`: FactCallSite::callees of a row of the operation as written. `function`: the type of a called operand names it. Neither: the call may enter any closure.
    // `call` is null where no record stands for the call: a terminator, an operation that was taken away while it waited.
    struct CalleesOfIndirectCall {
        ValueID call;
        CalleeListID list;
        FunctionID function;
    };
    std::span<const CalleesOfIndirectCall> calleesOfIndirectCalls() const LIFETIME_BOUND { return m_calleesOfIndirectCalls.span(); }

    bool hasHandlers() const { return m_hasHandlers; }
    bool hasSuspensions() const { return m_hasSuspensions; }
    bool hasLoops() const { return m_hasLoops; }
    // Some operation allocates in the frame of the unit. It stays set when the walk cuts back, which errs on the side of a frame.
    bool holdsFrameCells() const { return m_holdsFrameCells; }
    // Some closure or environment in the frame is not isMadeForOneUse, so an edge may list it. It stays set when the walk cuts back.
    bool holdsFrameCellsThatOutliveOneUse() const { return m_holdsFrameCellsThatOutliveOneUse; }
    // Some direct call enters code that counts on the stack checks of its callers, and Plan has not counted the frames of that code below this unit. So the unit tests the stack in its own frame, whatever Plan says of it.
    // It stays set when the walk cuts back, which errs on the side of a test.
    bool holdsCallThatPlanHasNotCounted() const { return m_holdsCallThatPlanHasNotCounted; }
    // Facts says of the function that no call of it collects, and its callers go by that. So the unit does not collect in its prologue either: callsOfFunctionNeverCollect() of AOTAnswersOfFacts.h.
    bool callsOfFunctionNeverCollect() const { return m_callsOfFunctionNeverCollect; }
    // Of some operation readsNumberOutOfBytes() holds. It stays set when the walk cuts back.
    bool readsNumbersOutOfBytes() const { return m_readsNumbersOutOfBytes; }
    const OpcodeSet& opcodesOfValues() const LIFETIME_BOUND { return m_opcodesOfValues; }

    void forEachEdgeOf(BlockID block, NOESCAPE const Invocable<void(EdgeID, const Edge&)> auto& functor) const
    {
        const Terminator& terminator = at(block).terminator;
        for (uint32_t index = 0; index < terminator.numberOfEdges; ++index)
            functor(terminator.edge(index), at(terminator.edge(index)));
        if (EdgeID handler = handlerOf(block))
            functor(handler, at(handler));
    }

    Wire::SourceLocation locationOf(const Program&, Origin) const;

    // Reading what the sweep left. None of it may be asked before sweep() has run.

    bool isSwept() const { return m_isSwept; }
    bool isNeeded(ValueID id) const { return marksOf(id).contains(SweepMark::IsNeeded); }
    OptionSet<SweepMark> marksOf(ValueID id) const { return OptionSet<SweepMark>::fromRaw(m_marks[id.index()]); }
    bool isReached(BlockID id) const { return m_reachedBlocks.get(id.index()); }
    // The needed operations of the block, first to last, without its parameters.
    std::span<const ValueID> valuesOf(BlockID id) const LIFETIME_BOUND { return m_order.span().subspan(at(id).m_firstInOrder, at(id).m_numberInOrder); }
    EdgeID handlerOf(BlockID id) const { return m_isSwept && m_blocksWithoutThrows.get(id.index()) ? EdgeID() : at(id).handler; }
    // By needed operations, terminators and edges. It stops counting at 255.
    uint8_t useCountOf(ValueID id) const { return m_useCounts[id.index()]; }
    // The only operation that uses the value, where useCountOf() is 1 and the user is no terminator and no edge.
    ValueID onlyUserOf(ValueID id) const { return m_onlyUsers[id.index()]; }
    // Asked only where hasLoops(). A needed parameter that an edge hands the value to: where edges hand it to several, one that an edge leads back to before any other. Null where none does.
    ValueID parameterFedBy(ValueID id) const { return m_parametersFed[id.index()]; }
    // Ascending by value. Null where the operation turned out not to suspend.
    std::span<const ValueID> liveAt(ValueID suspension) const LIFETIME_BOUND;
    // One record a suspension, ascending by suspension.
    std::span<const LiveAtSuspension> suspensions() const LIFETIME_BOUND { return m_suspensions.span(); }
    // Those at the head first, then in the order of the block's operations.
    std::span<const ReleaseBehindLastRead> releasesBehindLastReadsIn(BlockID id) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(m_releasesBehindLastReads.span(), id, { }, &ReleaseBehindLastRead::block);
        return { found.begin(), found.end() };
    }

    // Appending. Only the walk and the parser of Code's text call these.

    BlockID addBlock(BlockRole = BlockRole::Ordinary, BlockFrequency = BlockFrequency::Normal);
    void setLoopMarks(BlockID, bool isInLoop, bool isLoopHead);
    void setSubject(BlockID, ValueID);
    // Once, before anything is appended to the block. Each parameter is a record with Opcode::Parameter, made by appendParameter().
    void setParameters(BlockID, std::span<const ValueID>);
    ValueID appendParameter(BlockID, TypeID, Representation, Origin);
    // Of a parameter of a block that is no entry and holds no operation yet, so nothing has read it: Walker::typeParametersByWhatArrivesAt().
    void setTypeOfJoinParameter(ValueID, TypeID);
    // Every operand of the draft is a value or a constant, in the representation the operation takes it in. The draft's representation is decided.
    // `proven` is the caller's answer from the result rule. An opcode of opcodesThatStateTypes (ir/AOTOpcodeSets.h) or opcodesWithoutEmitter() stops the process.
    ValueID append(BlockID, const Draft&, TypeID proven, EffectTraits);
    // What append() makes of the draft of a Box, an Unbox or a Compress: one operand, no other word, no effect.
    ValueID appendConversion(BlockID, Opcode, ValueID operand, Representation, TypeID proven, Origin);
    ValueID addConstant(const Draft&, TypeID proven);
    // What addConstant() makes of the words, the origin and the type of a constant without operands, in another form.
    ValueID addCopyOfConstant(ValueID constant, Representation);
    // Makes `numberOfEdges` open edges.
    void end(BlockID, const TerminatorDraft&, uint32_t numberOfEdges);
    // An open edge from the block to its handler. Once, before the first operation that may throw.
    EdgeID addHandlerEdge(BlockID);
    void closeEdge(EdgeID, BlockID to, std::span<const ValueID> arguments);
    // For a call that is walked here. reset() has made [0]. A frame that names the function itself, handed in before the first block is added, describes [0] and takes its place.
    uint32_t addInlineFrame(const InlineFrame&);
    uint16_t addEntry(BlockID);
    // Once the walk is over and the verifiers have run: the entry keeps its number and stands at `block`, which has ended. The block that it stood at is a block like any other, and the sweep drops what nothing else reaches.
    void moveEntryTo(uint16_t entry, BlockID);
    void addFactUse(const FactUse&);
    void addFateOfPoll(ValueID poll, FateOfPoll);
    void reserveFrameWords(uint32_t numberOfWords) { m_numberOfFrameWords = std::max(m_numberOfFrameWords, numberOfWords); }
    void noteCallThatPlanHasNotCounted() { m_holdsCallThatPlanHasNotCounted = true; }
    void noteThatCallsOfFunctionNeverCollect() { m_callsOfFunctionNeverCollect = true; }
    void addCalleesOfIndirectCall(const CalleesOfIndirectCall& callees) { m_calleesOfIndirectCalls.append(callees); }

    // Cutting back. Everything appended since the mark goes: records, blocks, edges, side words, inline frames, entries, uses of facts, fates of polls.
    // A block older than the mark gets back the last operation, the terminator and the handler it had. `openBlocks` are the blocks older than the mark that may have grown since.
    struct OpenBlock {
        BlockID block;
        ValueID last;
        bool hadEnded { false };
        bool hadHandler { false };
    };
    struct Mark {
        uint32_t numberOfOperations { 0 };
        uint32_t numberOfBlocks { 0 };
        uint32_t numberOfEdges { 0 };
        uint32_t numberOfInlineFrames { 0 };
        uint32_t numberOfEntries { 0 };
        uint32_t numberOfFrameWords { 0 };
        uint32_t numberOfFactUses { 0 };
        uint32_t numberOfPollsWithFates { 0 };
        uint32_t numberOfCalleesOfIndirectCalls { 0 };
        SideWords::Mark sideWords;
        bool hasHandlers { false };
    };
    Mark mark() const;
    OpenBlock stateOf(BlockID) const;
    void cutBackTo(const Mark&, std::span<const OpenBlock> openBlocks);

private:
    friend class Sweep;

    void fill(Instruction&, const DraftWords&);

    CodeUnit m_unit;
    StableArray<Operation, 10> m_operations;
    StableArray<Block, 7> m_blocks;
    StableArray<Edge, 8> m_edges;
    SideWords m_sideWords;
    Vector<InlineFrame, 4> m_inlineFrames;
    Vector<EntryOfCode, 2> m_entries;
    Vector<FactUse> m_factUses;
    Vector<PollWithFate> m_pollsWithFates;
    Vector<CalleesOfIndirectCall> m_calleesOfIndirectCalls;
    OpcodeSet m_opcodesOfValues;
    uint32_t m_numberOfFrameWords { 0 };
    bool m_hasHandlers { false };
    bool m_hasSuspensions { false };
    bool m_hasLoops { false };
    // True until reset() clears it, which it may do only where append() and addConstant() set it.
    bool m_holdsFrameCells { true };
    bool m_holdsFrameCellsThatOutliveOneUse { true };
    bool m_holdsCallThatPlanHasNotCounted { false };
    bool m_callsOfFunctionNeverCollect { false };
    bool m_readsNumbersOutOfBytes { false };

    bool m_isSwept { false };
    Vector<uint8_t> m_marks;
    Vector<uint8_t> m_useCounts;
    Vector<ValueID> m_onlyUsers;
    Vector<ValueID> m_parametersFed;
    Vector<ValueID> m_order;
    BitVector m_reachedBlocks;
    BitVector m_blocksWithoutThrows;
    Vector<LiveAtSuspension> m_suspensions;
    Vector<ValueID> m_liveValues;
    // Ascending by block.
    Vector<ReleaseBehindLastRead> m_releasesBehindLastReads;
};

// AOTCodeText.cpp, AOTCodeParser.cpp. One line an operation, in the words of the IR's text with the engine's fields, the representation and the proven type behind them.
// A record that leans on facts is followed by those facts. printCode() heads the text with `code <unit> {`, and parseCode() resets the Code with that unit.
// What printCode() writes of a Code that is not swept, parseCode() reads back to an equal Code. Of a swept Code only what the emitters read has a line, and what the sweep added stands behind `#`.
struct CodeTextContext {
    const Program& program;
    TypeArena& types;
    // For verifyCode() alone. Null: the forms in which operations take their operands are not checked, nor is the index of a ResultAt on a direct call.
    const Plan* plan { nullptr };
};

void printCode(PrintStream&, const CodeTextContext&, const Code&);
// Null on success, else the line and what is wrong with it.
String parseCode(const CodeTextContext&, StringView text, Code&);

// The result rule of Narrowed. How far a test narrows is written once, in ir/AOTTypeNarrowing.h: these only hand on. `Test` is Draft or Operation, `typesOfTestOperands` its operands' proven types by the schema's index.
template<typename Test>
TypeID narrowOperandByTest(const TypingContext& context, const Test& test, OperandTypes typesOfTestOperands, uint32_t operandIndex, EdgeSide side)
{
    return typeOfEdgeResult(context, test, typesOfTestOperands, operandIndex, side);
}
// What a test speaks of: its operand `operandIndex`, or the test itself where it proves nothing of its operands and the branch asks it directly. Null: the test has no such operand.
inline ValueID valueThatTestSpeaksOf(const Code& code, ValueID test, uint32_t operandIndex)
{
    const Operation& operation = code.at(test);
    if (provesOf(operation.opcode) == Proof::None)
        return test;
    return operandIndex < operation.numberOfOperands() ? operation.operand(operandIndex) : ValueID();
}
// A CheckProof: TestingSwitch::CheckProofsAtRunTime asks about each Narrowed where it stands, and an edge may hand over several.
inline constexpr OpcodeSet operationsThatStandAmongNarrowedValues = conversionsOfForm | OpcodeSet { Opcode::CheckProof };
// What they give lay in a slot of their object, or the slot was empty. A read by name may have met a getter or a field that has left its slot.
inline constexpr OpcodeSet readsThatNarrowTheirHolders { Opcode::GetSlot, Opcode::GetSlotOrUndefined };
template<typename Read>
ReadOfEmptySlot whatReadOfEmptySlotGives(const Read& read)
{
    return opcodeOf(read) == Opcode::GetSlotOrUndefined ? ReadOfEmptySlot::GivesUndefined : ReadOfEmptySlot::IsRuledOut;
}
// `read` is one of them: the key is its own word. `valueRead` is the type of what it gave, behind the test of that.
// A read that tests its own guess may have read by name, so it proves nothing of its holder. What is expected of the holder it cuts like any other.
template<typename Read>
TypeID narrowHolderOfRead(const TypingContext& context, const Read& read, TypeID holder, TypeID valueRead)
{
    if (context.level == TypeLevel::Proven && testsItsOwnGuess(read))
        return holder;
    return narrowHolderByFieldValue(context, holder, read.key(), valueRead, whatReadOfEmptySlotGives(read));
}
// `read` gave the operand of the test.
template<typename Test, typename Read>
TypeID narrowHolderByTest(const TypingContext& context, const Test& test, OperandTypes typesOfTestOperands, uint32_t operandIndex, const Read& read, TypeID holder, EdgeSide side)
{
    return narrowHolderOfRead(context, read, holder, typeOfEdgeResult(context, test, typesOfTestOperands, operandIndex, side));
}
// `read` gave what the branch asks directly. `condition` is the type of that in front of the branch.
template<typename Read>
TypeID narrowHolderByCondition(const TypingContext& context, TypeID condition, const Read& read, TypeID holder, EdgeSide side)
{
    return narrowHolderOfRead(context, read, holder, narrowByTruthiness(context, condition, side));
}
// No case: `otherwise`.
template<typename Switch>
TypeID narrowSubjectByCase(const TypingContext& context, const Switch& terminator, TypeID subject, std::optional<uint32_t> caseIndex)
{
    return typeOfCaseResult(context, terminator, subject, caseIndex.value_or(UINT32_MAX));
}
// `read` is the read of a slot of the holder that gave the subject. `subject` is the type of the terminator's own operand. No case: `otherwise`.
template<typename Switch, typename Read>
TypeID narrowHolderOfSubjectByCase(const TypingContext& context, const Switch& terminator, TypeID subject, const Read& read, TypeID holder, std::optional<uint32_t> caseIndex)
{
    return narrowHolderOfRead(context, read, holder, narrowSubjectByCase(context, terminator, subject, caseIndex));
}
// AOTWalk.cpp. `test` is a ResultAt that lies in Code, and `value` a name of another ResultAt on the same producer: narrowResultByOtherResult() of what the two read, for the type that `value` has.
// Null: they are no such reads, or the one result says nothing of the other.
TypeID narrowOtherResultByTest(const TypingContext&, const Code&, ValueID test, ValueID value, EdgeSide);
// AOTWalk.cpp. Of a Narrowed that lies in Code: it finds the test or the switch, and asks one of those above.
TypeID typeOfNarrowed(const TypingContext&, const Code&, ValueID narrowed);
// AOTCodeTypeVerifier.cpp. The rule of a parameter of a join: the union of what the edges that reach the join bring. The walk types the parameter by it once the edges are closed, and verifyTypesOfCode() asks it once more.
// Null: some edge brings nothing that has a type.
TypeID typeOfJoinParameter(TypeArena&, std::span<const EdgeID> edgesThatReachJoin, NOESCAPE const ScopedLambda<TypeID(EdgeID)>& typeArrivingOn);

// AOTCodeVerifier.cpp. Where WalkOptions::verifiesCode is set. Null where every rule holds, else the first that does not: each edge closed once, each block ended once, definitions in front of uses
// on every way, operands in the representations their operations take, operands of the kinds their opcodes declare, no operation that has no emitter or states a type, no virtual object,
// arguments that fit parameters, each Narrowed where its test has passed. `kindOfBrokenRule` is written where a rule does not hold.
String verifyCode(const CodeTextContext&, const Code&, InternalErrorKind* kindOfBrokenRule = nullptr);
// AOTCodeVerifier.cpp. Which block dominates which, for whoever has the reached blocks in reverse postorder. A throw counts as leaving a block at its end.
class DominatorsOfCode {
    WTF_MAKE_NONCOPYABLE(DominatorsOfCode);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DominatorsOfCode);
public:
    DominatorsOfCode(const Code&, std::span<const BlockID> reachedBlocksInReversePostorder);

    // Both are reached.
    bool dominates(BlockID dominator, BlockID block) const { return m_firstNumberOfSubtree[block.index()] - m_firstNumberOfSubtree[dominator.index()] < m_sizeOfSubtree[dominator.index()]; }

private:
    // By block. The dominator tree is numbered in preorder: what a block dominates has the numbers from its own on, as many as its subtree has blocks.
    Vector<uint32_t> m_sizeOfSubtree;
    Vector<uint32_t> m_firstNumberOfSubtree;
};
// AOTCodeTypeVerifier.cpp. Derives the type of every record from scratch and compares: the rule of the opcode over the operands' derived types, with `positions` answering as it did for the walk.
// A parameter of a join holds the union of its arguments. A parameter of a loop head holds every argument, which makes the types a solution.
// Or it is a tally: every back edge hands it itself plus integer constants, and its type holds what enters and that change for as many turns as an integer parameter of the same head, which every turn moves one way, has values.
// A direct call whose result Plan splits is derived as the walk types it: the call as undefined, each ResultAt over it by the slot of its field.
// Where a record bears the mark of a fact, Facts is asked once more: the summaries of a call's callees over the derived types of its operands, what a bound function holds. Of an entry parameter Arrivals::provenAt() is.
String verifyTypesOfCode(const TypingContext&, const CallingConventions&, const Facts&, const Arrivals&, const Code&);

// AOTCode.cpp
// No emitter stands behind them. The walk lowers each, so none may be appended.
extern const OpcodeSet opcodesWithoutEmitterInCode;
inline const OpcodeSet& opcodesWithoutEmitter() { return opcodesWithoutEmitterInCode; }
// In Code a Double holds one NaN. The bytes of a view may spell any, and boxed or beside markers such bits would be read as a cell, an integer or a marker.
// The code generator makes them the one NaN in front of the first reader that could tell: CodeGenerator::settleNaNOf().
// The operation loads an element of a Float32Array or a Float64Array in line.
bool readsNumberOutOfBytes(const Operation&);
// Its answer is the same for every NaN, and a NaN among its operands gives its bits to the result.
bool handsOnBitsOfNaN(const Operation&);
// What the user makes of the operand is the same for every NaN: handsOnBitsOfNaN(), a relation of numbers, a conversion to an int32, a store into a view in line.
bool takesAnyNaN(const Operation& user, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
