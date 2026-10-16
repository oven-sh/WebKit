/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedFormat.h"
#include "AOTTypedID.h"
#include "AOTTypedType.h"
#include "LineColumn.h"
#include "TypedArrayType.h"
#include "TypeofType.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MultiArrayList.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_TYPED_INTERNAL_OPCODE(v) \
    v(GetCapture) \
    v(GetArgument) \
    v(CreateRest) \
    v(GetRestLength) \
    v(GetMyArgumentByVal) \
    v(CallForwardVarargs) \
    v(CreateThis) \
    v(ConstructSuper) \
    v(ConstructSuperForwardVarargs) \
    v(CallBuiltin) \
    v(HasShapeOf) \
    v(IsClassObjectAsDeclared) \
    v(GetStaticField) \
    v(PutStaticField) \
    v(IsClosureOf) \
    v(IsBuiltinFunction) \
    v(BuiltinFunction) \
    v(ConstantSymbol) \
    v(Results) \
    v(Unspecified) \
    v(ResultAt) \
    v(TypeOfIs) \
    v(AssertNotAllocated) \
    v(MaterializeEnvironment) \
    v(ModuleEvaluationWord) \
    v(OffsetIndex) \
    v(GetModuleVariableAt) \
    v(PutModuleVariableAt) \
    v(CallModuleFunction) \
    v(JumpToModuleEvaluator) \
    v(FailModuleCycle) \
    v(AddToModuleEvaluationDepth)

enum class Opcode : uint8_t {
#define AOT_TYPED_DECLARE_OPCODE(name) name,
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_DECLARE_OPCODE)
    FOR_EACH_AOT_TYPED_INTERNAL_OPCODE(AOT_TYPED_DECLARE_OPCODE)
#undef AOT_TYPED_DECLARE_OPCODE
};

static constexpr unsigned numberOfFormatOpcodes = 0
#define AOT_TYPED_COUNT_OPCODE(name) + 1
    FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_COUNT_OPCODE)
#undef AOT_TYPED_COUNT_OPCODE
    ;

ASCIILiteral nameOf(Opcode);

using Format::CastReason;
using Format::FunctionKind;
using Format::MethodKind;
using Format::TerminatorKind;

// A list in eight bytes: a pointer to its size, its capacity and its elements, null while it is empty. For lists that most of their
// owners do not have. It has the members of WTF::Vector that are used on it.
struct CompactListHead {
    uint32_t size;
    uint32_t capacity;
};
inline constexpr CompactListHead emptyCompactListHead { 0, 0 };

template<typename T>
class CompactList {
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
public:
    using ValueType = T;
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    CompactList() = default;
    CompactList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    CompactList(std::span<const T> list) { append(list); }
    template<size_t inlineCapacity> CompactList(const Vector<T, inlineCapacity>& list) { append(list.span()); }
    CompactList(const CompactList& other) { append(other.span()); }
    CompactList(CompactList&& other)
        : m_storage(std::exchange(other.m_storage, emptyStorage()))
    {
    }
    ~CompactList()
    {
        if (m_storage != emptyStorage())
            fastFree(m_storage);
    }
    CompactList& operator=(const CompactList& other)
    {
        if (this != &other) {
            shrink(0);
            append(other.span());
        }
        return *this;
    }
    CompactList& operator=(CompactList&& other)
    {
        if (this != &other) {
            if (m_storage != emptyStorage())
                fastFree(m_storage);
            m_storage = std::exchange(other.m_storage, emptyStorage());
        }
        return *this;
    }

    size_t size() const { return m_storage->size; }
    size_t capacity() const { return m_storage->capacity; }
    bool isEmpty() const { return !size(); }

    T* begin() LIFETIME_BOUND { return m_storage->elements(); }
    T* end() LIFETIME_BOUND { return begin() + size(); }
    const T* begin() const LIFETIME_BOUND { return m_storage->elements(); }
    const T* end() const LIFETIME_BOUND { return begin() + size(); }
    std::span<const T> span() const LIFETIME_BOUND { return { begin(), size() }; }
    std::span<T> mutableSpan() LIFETIME_BOUND { return { begin(), size() }; }
    std::span<const T> subspan(size_t offset, size_t length = std::dynamic_extent) const LIFETIME_BOUND { return span().subspan(offset, length); }

    T& at(size_t index) LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < size());
        return begin()[index];
    }
    const T& at(size_t index) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < size());
        return begin()[index];
    }
    T& operator[](size_t index) LIFETIME_BOUND { return at(index); }
    const T& operator[](size_t index) const LIFETIME_BOUND { return at(index); }
    T& first() LIFETIME_BOUND { return at(0); }
    const T& first() const LIFETIME_BOUND { return at(0); }
    T& last() LIFETIME_BOUND { return at(size() - 1); }
    const T& last() const LIFETIME_BOUND { return at(size() - 1); }

    void reserveCapacity(size_t wanted)
    {
        if (wanted <= capacity())
            return;
        RELEASE_ASSERT(wanted <= UINT32_MAX);
        bool wasEmpty = m_storage == emptyStorage();
        auto* storage = static_cast<Storage*>(fastRealloc(wasEmpty ? nullptr : m_storage, sizeof(Storage) + wanted * sizeof(T)));
        if (wasEmpty)
            storage->size = 0;
        storage->capacity = wanted;
        m_storage = storage;
    }
    void reserveInitialCapacity(size_t wanted) { reserveCapacity(wanted); }
    void shrinkToFit()
    {
        if (!size()) {
            clear();
            return;
        }
        if (size() == capacity())
            return;
        m_storage = static_cast<Storage*>(fastRealloc(m_storage, sizeof(Storage) + size() * sizeof(T)));
        m_storage->capacity = m_storage->size;
    }

    void append(T element)
    {
        if (size() == capacity())
            reserveCapacity(std::max<size_t>(2, capacity() + capacity() / 2 + 1));
        m_storage->elements()[m_storage->size++] = element;
    }
    void append(std::span<const T> list)
    {
        if (list.empty())
            return;
        size_t before = size();
        if (list.data() >= begin() && list.data() < end()) {
            Vector<T, 8> copy(list);
            append(copy.span());
            return;
        }
        reserveCapacity(before + list.size());
        memcpy(m_storage->elements() + before, list.data(), list.size_bytes());
        m_storage->size = before + list.size();
    }
    void appendVector(const CompactList& other) { append(other.span()); }
    template<size_t inlineCapacity> void appendVector(const Vector<T, inlineCapacity>& other) { append(other.span()); }
    void appendList(std::initializer_list<T> list) { append(std::span<const T> { list.begin(), list.size() }); }
    template<typename Iterator> void appendRange(Iterator from, Iterator to)
    {
        for (; from != to; ++from)
            append(*from);
    }
    void insert(size_t position, T element)
    {
        RELEASE_ASSERT(position <= size());
        append(element);
        std::rotate(begin() + position, end() - 1, end());
    }
    void removeAt(size_t position, size_t length = 1)
    {
        RELEASE_ASSERT(position + length <= size());
        if (!length)
            return;
        std::move(begin() + position + length, end(), begin() + position);
        m_storage->size -= length;
    }
    void removeLast() { removeAt(size() - 1); }
    T takeLast()
    {
        T result = last();
        removeLast();
        return result;
    }
    void shrink(size_t newSize)
    {
        RELEASE_ASSERT(newSize <= size());
        if (newSize != size())
            m_storage->size = newSize;
    }
    void grow(size_t newSize)
    {
        RELEASE_ASSERT(newSize >= size());
        if (newSize == size())
            return;
        reserveCapacity(newSize);
        std::fill(end(), begin() + newSize, T());
        m_storage->size = newSize;
    }
    void resize(size_t newSize)
    {
        if (newSize <= size())
            shrink(newSize);
        else
            grow(newSize);
    }
    void fill(T element, size_t newSize)
    {
        resize(newSize);
        std::fill(begin(), end(), element);
    }
    void clear()
    {
        if (m_storage != emptyStorage())
            fastFree(m_storage);
        m_storage = emptyStorage();
    }
    void reverse() { std::reverse(begin(), end()); }

    size_t find(T element) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if (begin()[index] == element)
                return index;
        }
        return notFound;
    }
    bool contains(T element) const { return find(element) != notFound; }
    size_t findIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if (matches(begin()[index]))
                return index;
        }
        return notFound;
    }
    bool containsIf(NOESCAPE const Invocable<bool(const T&)> auto& matches) const { return findIf(matches) != notFound; }
    size_t removeAllMatching(NOESCAPE const Invocable<bool(const T&)> auto& matches)
    {
        auto* kept = std::remove_if(begin(), end(), matches);
        size_t removed = end() - kept;
        shrink(size() - removed);
        return removed;
    }
    size_t removeAll(T element)
    {
        return removeAllMatching([&](const T& other) {
            return other == element;
        });
    }
    auto map(NOESCAPE const auto& functor) const
    {
        Vector<std::decay_t<decltype(functor(T()))>> result;
        result.reserveInitialCapacity(size());
        for (T element : *this)
            result.append(functor(element));
        return result;
    }

    friend bool operator==(const CompactList& a, const CompactList& b) { return std::ranges::equal(a.span(), b.span()); }

private:
    struct Storage : CompactListHead {
        T* elements() { return reinterpret_cast<T*>(this + 1); }
    };
    static Storage* emptyStorage() { return static_cast<Storage*>(const_cast<CompactListHead*>(&emptyCompactListHead)); }

    Storage* m_storage { emptyStorage() };
};

using Arguments = CompactList<ValueID>;

static constexpr uint32_t closureIsInFrame = 1;

static constexpr uint32_t shapeTestIsExact = 1;
static constexpr uint32_t shapeTestIsForThisInstance = 2;
static constexpr uint32_t shapeTestGuardsFastPath = 3;

enum class MethodAskedByStub : uint8_t { None, ArrayPush, MapGet, MapHas, MapSet, SetHas, SetAdd, RegExpExec, RegExpTest, StringMatch };

struct Value {
    Opcode opcode { Opcode::Undefined };
    // repOf(type).
    Rep rep : 4 { Rep::Void };
    bool isInTailPosition : 1 { false };
    bool hasStringKeysByType : 1 { false };
    bool constructs : 1 { false };
    bool goesBySignature : 1 { false };
    bool receiverHasOriginalStructure : 1 { false };
    bool operandClassIsProven : 1 { false };
    bool mustBeProperTailCall : 1 { false };
    bool isOnSlowPath : 1 { false };
    bool guardsFastPath : 1 { false };
    bool dropsDroppableCallerFrame : 1 { false };
    bool leavesDroppableFrame : 1 { false };
    bool receiverIsExpectedOfOneRow : 1 { false };
    bool storesClosureMadeWhenRead : 1 { false };
    MethodAskedByStub methodAskedByStub : 4 { MethodAskedByStub::None };
    uint8_t classExpectedOfReceiverPlusOne : 6 { 0 };
    uint32_t position { 0 };
    // What the value is: given by the front end, or by the pass that made the value, which copies a type that is there. Whoever uses the
    // value relies on it. So the operation sees to it: where what it does by itself does not make sure of the type (a property read
    // the general way, a call of a function that is not known, a field whose slot holds more than this view says), it tests what it got
    // and throws a TypeError.
    TypeID type;
    // What the checker expects the value to be, where that is more than the type says: as a rule the value of an expression of
    // JavaScript, whose type is anything. It may be wrong. Nothing relies on it: the verifier does not look at it, no pass derives one
    // from another, and no summary goes by it. Only the lowering of an operation that is carried out the general way reads those of
    // its own operands: it tests for what is expected, does that in place, and does the general thing otherwise. None: nothing is expected.
    TypeID prediction() const { return isExpected(ExpectedType) ? TypeID(expectation & greatestExpected) : TypeID(); }
    void setPrediction(TypeID type) { expect(ExpectedType, type.index(), !!type); }
    // Likewise: the function that the value is expected to be a closure of.
    FunctionID predictedClosure() const { return isExpected(ExpectedClosure) ? FunctionID(expectation & greatestExpected) : FunctionID(); }
    void setPredictedClosure(FunctionID function) { expect(ExpectedClosure, function.index(), !!function); }
    // Likewise, of a GetByVal: see Program::functionByKey().
    StringID predictedByKey() const { return isExpected(ExpectedByKey) ? StringID(expectation & greatestExpected) : StringID(); }
    void setPredictedByKey(StringID name) { expect(ExpectedByKey, name.index(), !!name); }
    // Options::auditAOTTypedAssertions(): the type the front end has given the value, where `type` is anything in its place and the
    // operation is one that would hold what it yields to the type. Nothing relies on it. The lowering tests for it, has a value that fails
    // reported, and lets it go on. Of a Cast: what it holds its operand to. See Reader::typeReliedOn().
    TypeID assertedType() const { return isExpected(Asserted) ? TypeID(expectation & greatestExpected) : TypeID(); }
    void setAssertedType(TypeID type) { expect(Asserted, type.index(), !!type); }
    enum : uint32_t { ExpectedType, ExpectedClosure, ExpectedByKey, Asserted };
    static constexpr uint32_t greatestExpected = (1u << 30) - 1;
    uint32_t expectation { UINT32_MAX };
    bool isExpected(uint32_t kind) const { return expectation != UINT32_MAX && expectation >> 30 == kind; }
    void expect(uint32_t kind, uint32_t index, bool isSet)
    {
        if (!isSet) {
            if (isExpected(kind))
                expectation = UINT32_MAX;
            return;
        }
        if (isExpected(Asserted) && kind != Asserted)
            return;
        expectation = index < greatestExpected ? kind << 30 | index : UINT32_MAX;
    }
    // Index into Function::inlineFrames: which inlined call this value was copied in by. 0: it belongs to the function itself.
    // `position` is an offset into the source of that frame's function.
    uint32_t inlineFrame : 8 { 0 };
    uint32_t isSignatureOfCall : 1 { 0 };
    uint32_t calleeOrSignaturePlusOne : 23 { 0 };
    std::array<ValueID, 3> children;
    std::array<uint32_t, 2> immediates { Format::none, Format::none };
    Arguments arguments;

    ValueID child(unsigned index) const { return children[index]; }
    bool hasCallSignature() const { return (opcode == Opcode::Call || opcode == Opcode::CallMethod) && isSignatureOfCall; }
    uint32_t callSignature() const { return isSignatureOfCall ? static_cast<uint32_t>(calleeOrSignaturePlusOne) - 1 : Format::none; }
    void setCallSignature(uint32_t signature)
    {
        isSignatureOfCall = signature < (1u << 23) - 1;
        calleeOrSignaturePlusOne = isSignatureOfCall ? signature + 1 : 0;
    }
    // Whether something is expected of this value that is not expected of the other: whoever took the other in its place would lose that.
    // A pass that finds the two to be one value leaves this one as an Upcast of the other then, which is no code.
    bool hasPredictionUnlike(const Value& other) const { return expectation != UINT32_MAX && !isExpected(Asserted) && (other.isExpected(Asserted) || expectation != other.expectation); }
    // For a value that a pass makes to stand for the other, or puts in its place under its number.
    void takePredictionOf(const Value& other)
    {
        if (isExpected(Asserted))
            return;
        expectation = other.isExpected(Asserted) ? UINT32_MAX : other.expectation;
    }
    bool boolean() const { return immediates[0]; }
    uint64_t bits() const { return static_cast<uint64_t>(immediates[1]) << 32 | immediates[0]; }
    int64_t integer() const { return static_cast<int64_t>(bits()); }
    void setBits(uint64_t bits)
    {
        immediates[0] = static_cast<uint32_t>(bits);
        immediates[1] = static_cast<uint32_t>(bits >> 32);
    }
    StringID string() const { return StringID(immediates[0]); }
    FunctionID function() const { return FunctionID(immediates[0]); }
    bool isClosureInFrame() const { return opcode == Opcode::NewClosure && immediates[1] == closureIsInFrame; }
    // Of ResultAt: which of Function::resultTypes of the function its operand calls.
    uint32_t resultIndex() const { return immediates[0]; }
    // THE SECOND IMMEDIATE OF A CallDirect, WHOSE NUMBERS ARE WHOSE. Format::none and small numbers: the inliner's: the depth of copying the call stands at. Marks of the
    // inliner count DOWN from Format::none - 1: its head stands in front of it; the sixteen below: the copy was declined, less the licence it would have had. From jumpsToContinuation on, maxContinuations numbers: this is no call: SplitFunction::entry jumps to that continuation of
    // the function itself and returns what it returns: made behind all passes, read by the lowering alone. Whoever needs more numbers takes a range and writes it here.
    static constexpr uint32_t jumpsToContinuation = 1u << 30;
    static constexpr uint32_t maxContinuations = 16;
    static constexpr uint32_t copyWasDeclined = Format::none - 2;
    bool isCallWhoseCopyWasDeclined() const { return opcode == Opcode::CallDirect && copyWasDeclined - immediates[1] < 16; }
    uint32_t licenceOfDeclinedCopy() const { return copyWasDeclined - immediates[1]; }
    bool isJumpToContinuation() const { return opcode == Opcode::CallDirect && immediates[1] - jumpsToContinuation < maxContinuations; }
    uint32_t continuation() const { return immediates[1] - jumpsToContinuation; }
    VariableID variable() const { return VariableID(immediates[0]); }
    bool findsVariableInitialized() const { return opcode == Opcode::GetModuleVariable && immediates[1] == Format::VariableIsInitialized; }
    LayoutID layout() const { return LayoutID(immediates[0]); }
    uint32_t field() const { return immediates[1]; }
    // Of GetCapture: which of Function::captureTypes of function().
    uint32_t captureIndex() const { return immediates[1]; }
    // Of NewObjectWithShape: among Function::allocations until these are linked, then among Program::allocations. Until
    // assignAllocations(): the name that goes with the first child, see Format::Value.
    uint32_t allocation() const { return immediates[1]; }
    bool yieldsExceptionItself() const { return opcode == Opcode::CatchValue && immediates[0] == 1; }
    CastReason castReason() const { return static_cast<CastReason>(immediates[1] & 0xff); }
    void setCastReason(CastReason reason) { immediates[1] = (immediates[1] & ~0xffu) | static_cast<uint32_t>(reason); }
    static constexpr uint32_t castHoldsWhatAwaitYields = 1u << 19;
    bool holdsWhatAwaitYields() const { return opcode == Opcode::Cast && immediates[1] & castHoldsWhatAwaitYields; }
    // Of a Cast for the reason Coerce: see Format::CastReason.
    Format::Freshness freshness() const { return static_cast<Format::Freshness>(immediates[1] >> Format::freshnessShift & 3); }
    bool notesMisfitAtCaller() const { return opcode == Opcode::Cast && immediates[1] & Format::CastNotesMisfitAtCaller; }
    uint32_t argumentOfMisfitAtCaller() const { return (immediates[1] >> Format::argumentOfMisfitAtCallerShift & 0xff) == 0xff ? UINT32_MAX : immediates[1] >> Format::argumentOfMisfitAtCallerShift & 0xff; }
    bool coercesFreshData() const { return freshness() != Format::Freshness::None; }
    FunctionID parameterCheckCallee() const { return opcode == Opcode::Cast && !isSignatureOfCall && calleeOrSignaturePlusOne ? FunctionID(static_cast<uint32_t>(calleeOrSignaturePlusOne) - 1) : FunctionID(); }
    void setParameterCheckCallee(FunctionID function)
    {
        isSignatureOfCall = false;
        calleeOrSignaturePlusOne = function.index() < (1u << 23) - 1 ? function.index() + 1 : 0;
    }
    // Of a Cast for the reason Coerce: the type after `as`. See Format::CastReason.
    TypeID coercedType() const { return TypeID(immediates[0]); }
    // Of a Cast for the reason NonNull: the source text of the operand, or none.
    StringID operandText() const { return string(); }
    // Of a Cast that stands for the access of a property of its operand, and so turns away `undefined` and `null`: the name. See resolveCalls().
    StringID accessedName() const { return castReason() == CastReason::Coerce || castReason() == CastReason::NonNull ? StringID() : string(); }
    // Of NewArray, from placement on: where in the statics the site keeps something, or none. See Placement::placeArraySites().
    uint32_t vectorLengthHintOffset() const { return immediates[0]; }
    uint32_t copyOnWriteButterflyOffset() const { return immediates[1] & 1 ? Format::none : immediates[1]; }
    uint32_t unsharedLiteralOfConstants() const { return immediates[1] != Format::none && immediates[1] & 1 ? immediates[1] >> 1 : Format::none; }
    static constexpr uint32_t keyIsListed = 1;
    static constexpr uint32_t isKeyOfForInList = 2;
    static constexpr uint32_t elementIsKnownByTheStores = 3;
    bool readsListedKey() const { return opcode == Opcode::GetByVal && immediates[0] == keyIsListed; }
    bool readsForInList() const { return opcode == Opcode::GetByVal && immediates[0] == isKeyOfForInList; }
    bool readsElementKnownByTheStores() const { return (opcode == Opcode::GetByVal || opcode == Opcode::IteratorValue) && immediates[0] == elementIsKnownByTheStores; }
    BlockID parameterBlock() const { return BlockID(immediates[0]); }
    uint32_t parameterIndex() const { return immediates[1]; }
    // Of CallBuiltin: the entry of the table that holds, and the class of the receiver or of what is made (EngineClass::index), if any.
    // Of IsBuiltinFunction: the entry whose function the operand is compared with. Of BuiltinFunction: the entry whose function it is.
    BuiltinID builtin() const { return BuiltinID(immediates[0]); }
    uint32_t engineClass() const { return immediates[1]; }
    BuiltinName globalComparedWith() const { return opcode == Opcode::IsBuiltinFunction && immediates[1] != Format::none ? BuiltinName(immediates[1]) : BuiltinName(); }
    // Of CallBuiltin of a method on an object: the GetById that read the method from the receiver before the arguments were evaluated, if
    // there is one. The generic fallback calls what it has read.
    ValueID methodRead() const { return children[1]; }
    bool listsKeysAndValuesAlternately() const { return !!children[2]; }
    bool copiesOwnPropertyDescriptors() const { return !!children[2]; }
    // Of GetById: it is the methodRead() of a CallBuiltin of this entry and has no other use. Where the receiver has its original
    // structure it reads nothing and yields the empty value, which stands for the entry's function.
    BuiltinID readsMethodFor() const { return hasWordOfKnownPlace() ? BuiltinID() : BuiltinID(immediates[1]); }
    // Of PutById: it stands for a pair of an `Initialize`, and for which. See initialStoresOf().
    bool isInitialStore() const { return immediates[1] != Format::none && !hasWordOfKnownPlace(); }
    static constexpr uint32_t numberOfKnownPlaces = 62;
    static constexpr uint32_t isFirstStore = 0x200;
    static constexpr uint32_t goesByName = 0x400;
    static constexpr uint32_t receiverIsThis = 0x40;
    bool hasWordOfKnownPlace() const { return (opcode == Opcode::GetById || opcode == Opcode::PutById) && immediates[1] >> 30 == 1; }
    bool isAtKnownPlace() const { return hasWordOfKnownPlace() && !(immediates[1] & (goesByName | receiverIsThis)); }
    bool isFirstStoreToThis() const { return opcode == Opcode::PutById && hasWordOfKnownPlace() && (immediates[1] & (isFirstStore | receiverIsThis)) == (isFirstStore | receiverIsThis); }
    bool isStoreToObjectBornWhole() const { return opcode == Opcode::PutById && hasWordOfKnownPlace() && (immediates[1] & (isFirstStore | receiverIsThis)) == isFirstStore; }
    static constexpr uint32_t greatestBirthOfFirstStore = 0x3ffff;
    uint32_t birthOfFirstStore() const { return immediates[1] >> 11 & 0x3ffff; }
    void setBirthOfFirstStore(uint32_t birth) { immediates[1] = (immediates[1] & ~(0x3ffffu << 11)) | birth << 11; }
    unsigned attributesOfFirstStore() const { return (immediates[1] & receiverIsThis ? 0 : (immediates[1] >> 7 & 1) << 1) | (immediates[1] >> 29 & 1) << 3; }
    bool isDefinitionInRowOfThis() const { return isFirstStoreToThis() && immediates[1] & 0x80; }
    bool hasRowOfBirth() const { return opcode == Opcode::NewObject && immediates[0] != Format::none; }
    uint32_t rowOfBirth() const { return immediates[0]; }
    uint32_t numberOfRowOfBirth() const { return opcode == Opcode::NewObject ? immediates[1] : Format::none; }
    uint32_t knownPlace() const { return immediates[1] & 0x3f; }
    bool receiverIsNeverNil() const { return immediates[1] & 0x100; }
    uint32_t initialStoreIndex() const { return immediates[1]; }
    // Of CreateRest, GetRestLength, GetMyArgumentByVal and CallForwardVarargs: how many arguments come before the first that counts.
    // GetMyArgumentByVal yields what the array CreateRest makes has under the key, or the `arguments` object made from that array.
    // CallForwardVarargs callee, this: a call with the elements of that array for arguments.
    uint32_t numberOfArgumentsToSkip() const { return immediates[0]; }
    bool readsArgumentsObject() const { return immediates[1] == 1; }
    // CreateThis: what `new` makes before it calls a function of untyped code: an ordinary object that inherits from the `prototype` of the
    // operand, which is a closure of the program, or from Object.prototype if that is no object. A second operand: the closure of the constructor that is going to run, where the first
    // is the `new.target` of a `super(..)`.
    // ConstructSuper class, newTarget (arguments..): `super(..)` in the constructor of a class that is not laid out: [[Construct]] of what the
    // class object inherits from. It is the operation SuperConstruct with the arguments as operands.
    // Of TypeOfIs: what `typeof` of the operand is compared with.
    TypeofType typeofType() const { return static_cast<TypeofType>(immediates[0]); }

    template<typename Functor>
    void forEachChild(const Functor& functor) const
    {
        for (ValueID child : children) {
            if (child)
                functor(child);
        }
        for (ValueID argument : arguments)
            functor(argument);
    }

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        for (ValueID& child : children) {
            if (child)
                functor(child);
        }
        for (ValueID& argument : arguments)
            functor(argument);
    }
};

struct Target {
    BlockID block;
    Arguments arguments;
};

struct Terminator {
    TerminatorKind kind { TerminatorKind::Unreachable };
    ValueID value;
    Vector<Target, 2> targets;
    CompactList<int64_t> cases;

    template<typename Functor>
    void forEachChild(const Functor& functor)
    {
        if (value)
            functor(value);
        for (Target& target : targets) {
            for (ValueID& argument : target.arguments)
                functor(argument);
        }
    }
};

using ValuesOfBlock = Vector<ValueID, 4>;

struct Block {
    CompactList<ValueID> parameters;
    ValuesOfBlock values;
    Terminator terminator;
};

static constexpr uint32_t FunctionIsGenericEntrypoint = 1 << 16;
// The second immediate of IsClosureOf: and the closure has the Structure it was allocated with: no property of its own, and it inherits from what it did.
static constexpr uint32_t closureIsAsMade = 1;
static constexpr uint32_t closureEvenIfAltered = 2;
static constexpr uint32_t closureIsTheOneThatConstructs = 3;
// A general entry that is handed how many arguments there are and where they are, not the arguments.
static constexpr uint32_t FunctionUsesArgumentBuffer = 1 << 17;
// Code outside the program may call it as it is, without a general entry in between.
static constexpr uint32_t FunctionHasUnknownCallers = 1 << 18;
static constexpr uint32_t FunctionIsHalfOfHiddenCoroutine = 1 << 19;
static constexpr uint32_t FunctionLogsMisfits = 1 << 20;
static constexpr uint32_t FunctionIsEnteredByValue = 1 << 21;
static constexpr uint32_t FunctionIsHalfOfAsyncFunction = 1 << 22;

// How objects come to be, at one or several places of the program.
struct Allocation {
    LayoutID layout;
    // They are made by `new` on this function, which is declared where there is one closure of it, and have the `prototype` of that
    // closure for their prototype. None: Object.prototype, or the prototype of the class.
    FunctionID constructedBy;
    // The order in which the properties are expected to be created, as indexes into Layout::fields.
    Vector<uint16_t> order;
    // The first so many of them are created by the stores that follow the allocation. Every other field is absent at first.
    uint16_t born { 0 };
    // The field, as an index into Layout::fields, that holds the first child of the NewObjectWithShape for as long as the object lives, if
    // it is ever stored to by name: see Format::Value. UINT16_MAX: there is no such child.
    uint16_t discriminant { UINT16_MAX };
    // Of a closure: how many words it has room for, for what it captures. Odd: an even number makes a cell of the same size.
    uint16_t numberOfCaptures { 0 };

    // From placement on.
    // In bytes. An object has room for all that casts can turn it into where it is, given what is known of it when it is made.
    uint32_t size { 0 };
    // Not 0: that depends on the discriminant. If it is the integer firstLiteral + i, i < numberOfLiterals, the object is made with
    // Program::allocations[allocationOfFirstLiteral + i]. If it is any other integer: with [allocationOfFirstLiteral + numberOfLiterals].
    // If it is no integer: with this one, which has room for everything.
    // An entry has the final layout for its constant where there is one: the single-constant layout of the family that all others with
    // that constant are reachable from. `order` and `discriminant` are then indexes into the fields of that layout. Otherwise it has
    // the layout of this allocation, with room for the layouts that list the constant and for the abstract ones with a subtype that does.
    // A transition the cell has no room for is refused: Coercion::forEachSpecialization().
    uint32_t numberOfLiterals { 0 };
    int32_t firstLiteral { 0 };
    uint32_t allocationOfFirstLiteral { 0 };

    friend bool operator==(const Allocation&, const Allocation&) = default;
};

// A call that was inlined, for stack traces. A call site in the image has 8 bits for the frame: see PackedSite.
static constexpr unsigned maxInlineFrames = 254;
static_assert(maxInlineFrames < (1u << 8), "Value::inlineFrame has eight bits");
struct InlineFrame {
    FunctionID callee;
    // Where the call is: a position in the source of the function of `parent`.
    uint32_t callPosition { 0 };
    uint32_t parent { 0 };
    // For the map of the image: what let the inliner take the body in. An index into licencesForMap.
    uint8_t licence { 0 };
    bool isTailCall { false };
    bool isConstruction { false };
};

static constexpr ASCIILiteral licencesForMap = "-pemscathfkobwr"_s;

constexpr uint8_t licenceForMap(char letter)
{
    uint8_t index = 0;
    while (licencesForMap[index] != letter)
        ++index;
    return index;
}

enum class AllocationKind : uint8_t { Object, Environment, CoroutineFrame, Array, Closure, Rest, Arguments };
enum class AllocationOutcome : uint8_t { Removed, BuiltLate, Unchanged };

// For dumpAtStage(.., "allocations"): a place of the program that allocates, in one compiled function.
struct AllocationNote {
    AllocationKind kind { AllocationKind::Object };
    Opcode opcode { Opcode::Undefined };
    uint32_t inlineFrame { 0 };
    uint32_t position { 0 };
    LayoutID layout;
    FunctionID closureOf;
    AllocationOutcome outcome { AllocationOutcome::Unchanged };
    Vector<ValueID, 1> left;
    ASCIILiteral forcedBy;
    uint32_t forcedByInlineFrame { 0 };
    uint32_t forcedByPosition { 0 };
    BuiltinID forcedByBuiltin;
    FunctionID forcedByCallee;
    uint32_t forcedByArgument { 0 };
    ValueID forcedByInto;
    Vector<ValueID, 2> heldBy;
    ASCIILiteral why;
};

struct BuiltinPath {
    bool isGlobalObject { false };
    bool isOnPrototype { false };
    StringID global;
    StringID member;

    explicit operator bool() const { return isGlobalObject || global; }
    friend bool operator==(const BuiltinPath&, const BuiltinPath&) = default;
};

struct ExpectedBuiltins {
    BuiltinPath first;
    BuiltinPath second;
    bool areTooMany { false };

    explicit operator bool() const { return !!first; }
    friend bool operator==(const ExpectedBuiltins&, const ExpectedBuiltins&) = default;
    void add(const BuiltinPath& path)
    {
        if (!path || areTooMany || path == first || path == second)
            return;
        if (!first)
            first = path;
        else if (!second)
            second = path;
        else
            *this = { { }, { }, true };
    }
    void add(const ExpectedBuiltins& other)
    {
        if (other.areTooMany)
            *this = { { }, { }, true };
        add(other.first);
        add(other.second);
    }
};

struct Function {
    StringID name;
    ModuleID module;
    uint32_t position { 0 };
    uint32_t placeOfText { 0 };
    uint32_t endOfText { 0 };
    FunctionKind kind { FunctionKind::Plain };
    uint32_t flags { 0 };
    // Its `length` property.
    uint32_t length { 0 };
    // What it takes before `this`, see Format::Function::captures. Of a generic entrypoint: the closure.
    Vector<TypeID> captureTypes;
    // From createGenericEntrypoints() on: some NewClosure makes closures of it. Otherwise, if it captures nothing, there is one, which
    // FunctionReference stands for.
    bool hasManyClosures { false };
    bool closuresAreGivenProperties { false };
    // Of the ramp of a generator function: its first parameter is the closure that was called, which what it makes inherits from the `prototype` of. Only the general entry calls it.
    bool takesItsClosure { false };
    FunctionID rampForClosures;
    FunctionID writtenEntry;
    FunctionID secondBody;
    bool isRampForClosures { false };
    bool isUsedAsObject { false };
    bool isBodyOfSeveralSites { false };
    bool isGuardedPreludeEntry { false };
    bool onlyDestructuresParameter { false };
    bool copiesFirstArgumentOutOfFrame { false };
    uint8_t numberOfCapturesOfCopiedArgument { 0 };
    Vector<FunctionID> guardedPreludeEntriesWantedLater;
    bool storesWereMovedByCleanUp { false };
    bool returnsBoxedValue { false };
    bool takesCapturesFromClosure { false };
    bool isValueEntry { false };
    bool isTwinOfItsFunction { false };
    FunctionID functionOfValueEntry;
    FunctionID adapterOfValueEntry;
    FunctionID valueEntry;
    uint32_t resultOfAdapter { Format::none };
    uint64_t resultsHoldingWhatIsYielded { 0 };
    TypeID typeYieldedWhenResultsWereCompared;
    TypeID resultTypeCheckedByCallers;
    bool resultIsCoercedByCallers { false };
    bool isCalledInPlace { false };
    bool isCalledFromDroppableFrame { false };
    bool hasTailCallSites { false };
    bool isInCycleOfIndirectTailCalls { false };
    uint16_t reservedStackArgumentCount { 0 };
    bool returnsWholeNumbers { false };
    bool readsMemberOfHostObject { false };
    StringID hostObjectReadFromCache;
    FunctionID generalReaderOfHostObject;
    bool readsAsyncLocalStorage { false };
    FunctionID generalReaderOfAsyncLocalStorage;
    // Of a constructor, and of its general entry: the class.
    LayoutID constructedClass;
    // functionIsAsync, functionIsGenerator: what it is declared as, and so what its closures are as objects. Also of its general entry.
    // It stays when the splitting of coroutines takes the flags away.
    uint8_t declaredAs { 0 };
    // From the splitting of coroutines on, of what was split: the function that runs its body.
    FunctionID resumeFunction;
    // Of an async function: the one that runs the body up to the first suspension.
    FunctionID startFunction;
    bool savesItsFrame { false };
    bool capturesExceptions { false };
    // Of a resume function: where in the source of the function each Await and Yield is. The frame of a coroutine that is suspended at the n-th has the state n + 1.
    Vector<uint32_t> suspensionPositions;
    // From linkAllocations() on, of a plain function that is a value and declares `this` to be of a layout that objects are made of: how
    // [[Construct]] makes the object where whoever constructs does not know the function. None: an ordinary object.
    uint32_t allocationOfThis { Format::none };
    // None: anything.
    TypeID thisType;
    TypeID resultType;
    // Not empty: it yields this many values, each in a register, in the representation of its type: every Return has a `Results` of as many, and what a call of it yields is looked at by `ResultAt` alone. See returnResultsInRegisters().
    Vector<TypeID, 4> resultTypes;
    // Its body is a call of that function, which has resultTypes, and the making of the object out of what that yields: whoever wants the object or comes by value comes here. Every direct call takes it in. See splitOffWorkers().
    FunctionID worker;
    // Of such a one: 1, 2: in the place of the object it may yield `undefined`, `null`: the first of what the worker yields says whether there is an object.
    uint8_t nilOfResults { 0 };
    Vector<TypeID> parameterTypes;
    Vector<ExpectedBuiltins> builtinsExpectedOfEntryParameters;
    // By parameter: its type is written down. What is passed and does not fit is coerced as by `as`. Otherwise it is turned away.
    BitVector annotatedParameters;
    BitVector parametersOnlyCalled;
    BitVector capturesOnlyCalled;
    BitVector environmentsInFrame;
    BitVector mayPointIntoFrame;
    // As Value::assertedType, of `this` and by parameter, for the casts of the general entry. None: thisType and parameterTypes say it.
    TypeID assertedThisType;
    Vector<TypeID> assertedParameterTypes;
    // See Value::allocation().
    Vector<Allocation> allocations;
    // What a call of it may change, all it calls included.
    Writes writes { Writes::Heap };
    // Whether a call of it may turn an object into one of a specialization of its layout: it comes to an `as` or `!` that does, or to
    // code that is not known, which may. A specialization is a member of every view its base layout is a member of, so a shape test
    // that has passed stays passed. Only that an object is not of a shape has to be found out again after such a call.
    bool writesShapeWords { true };
    // The same two of a call that comes back. What the function does on its way to throwing is nothing to a call that no handler
    // watches: control does not come back there. Never more than the two above.
    Writes writesIfItReturns { Writes::Heap };
    bool writesShapeWordsIfItReturns { true };
    bool convertsArrayStorage { true };
    bool convertsArrayStorageIfItReturns { true };
    // Of a function that starts with Operation "Initialize": how many of its pairs, from the first on, may be stored to a fresh `this`
    // without a test. Every allocated member of the type of `this` has the field, inline, in one slot.
    uint16_t numberOfInitialStores { 0 };
    // The order word of an object that was fresh and has got exactly those.
    uint32_t orderWordAfterInitialStores { 0 };
    Vector<Block, 0, CrashOnOverflow, 2> blocks;
    Vector<Value, 0, CrashOnOverflow, 4> values;
    // Entry 0 stands for the function itself and is unused. Empty: nothing was inlined.
    Vector<InlineFrame> inlineFrames;
    Vector<AllocationNote> allocationNotes;
    UncheckedKeyHashMap<uint64_t, unsigned, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> allocationNoteIndices;

    bool has(uint32_t flag) const { return flags & flag; }
    bool isGenericEntrypoint() const { return has(FunctionIsGenericEntrypoint); }
    // Written in TypeScript. In any other function every value that is not known by its operation is anything.
    bool isTyped() const { return has(Format::FunctionIsTyped); }
    // Why what is passed for the parameter is held to its type.
    CastReason reasonToHoldParameter(unsigned index) const { return annotatedParameters.get(index) ? CastReason::As : CastReason::Store; }
    TypeID assertedTypeOfParameter(unsigned index) const { return index < assertedParameterTypes.size() ? assertedParameterTypes[index] : TypeID(); }
    uint32_t allocationOf(Allocation&&);
    Value& at(ValueID id) { ASSERT(id.index() < values.size()); return values.mutableSpan().data()[id.index()]; }
    const Value& at(ValueID id) const { ASSERT(id.index() < values.size()); return values.span().data()[id.index()]; }
    Block& at(BlockID id) { ASSERT(id.index() < blocks.size()); return blocks.mutableSpan().data()[id.index()]; }
    const Block& at(BlockID id) const { ASSERT(id.index() < blocks.size()); return blocks.span().data()[id.index()]; }
};

// Declared with the syntax of a method: an object of a class has it on its prototype.
static constexpr uint32_t fieldIsMethod = 64;
// Of a variable in an environment: as Variable::isNeverSeenUninitialized and Variable::isStoredOnce.
static constexpr uint32_t fieldIsNeverSeenUninitialized = 128;
static constexpr uint32_t fieldIsStoredOnce = 256;
// See Format::FieldIsNeverStored. Layout::fieldNamed() does not find such a field, it has no slot, and the run time is not told of it.
// Whoever goes through all fields of a layout passes over it.
static constexpr uint32_t fieldIsNeverStored = 512;
// See Format::FieldIsExpectedInt32. Only placement looks at it.
static constexpr uint32_t fieldIsExpectedInt32 = 1024;
// See Format::FieldIsSetAtCreation.
static constexpr uint32_t fieldIsSetAtCreation = 2048;
static constexpr uint32_t fieldMayBeSetAtCreation = 4096;
// Of a variable in an environment: some NewClosureOnce keeps its closure there. Nothing else reads it, and nothing else stores to it but
// the `undefined` it starts with.
static constexpr uint32_t fieldHoldsClosureMadeOnce = 16384;
// See Format::FieldDoesNotAdmitUndefined.
static constexpr uint32_t fieldDoesNotAdmitUndefined = 65536;
static constexpr uint32_t fieldKeepsWideReference = 524288;
// Those of AOTTypedData.h too: each has a bit of its own.
static_assert((fieldIsOptional + fieldIsOutOfLine + fieldIsImmutable + fieldAllowsUndefined + fieldAllowsNull + fieldIsAnnotated + fieldIsMethod + fieldIsNeverSeenUninitialized
    + fieldIsStoredOnce + fieldIsNeverStored + fieldIsExpectedInt32 + fieldIsSetAtCreation + fieldMayBeSetAtCreation + fieldIsAbsentAtBirth + fieldHoldsClosureMadeOnce + fieldIsNeverSeenAbsent + fieldDoesNotAdmitUndefined
    + fieldIsPinned + fieldIsNeverSeenAbsentAsWritten + fieldMayExistWithoutStorage + fieldIsPlaceholder + fieldMayBeUnlisted + fieldHoldsClosureMadeWhenRead)
    == (fieldIsOptional | fieldIsOutOfLine | fieldIsImmutable | fieldAllowsUndefined | fieldAllowsNull | fieldIsAnnotated | fieldIsMethod | fieldIsNeverSeenUninitialized
    | fieldIsStoredOnce | fieldIsNeverStored | fieldIsExpectedInt32 | fieldIsSetAtCreation | fieldMayBeSetAtCreation | fieldIsAbsentAtBirth | fieldHoldsClosureMadeOnce | fieldIsNeverSeenAbsent | fieldDoesNotAdmitUndefined
    | fieldIsPinned | fieldIsNeverSeenAbsentAsWritten | fieldMayExistWithoutStorage | fieldIsPlaceholder | fieldMayBeUnlisted | fieldHoldsClosureMadeWhenRead));
static_assert(fieldIsNeverSeenAbsentAsWritten < 1u << guardOfPinnedFieldShift);
static_assert(fieldMayExistWithoutStorage < 1u << guardOfPinnedFieldShift);
static_assert(fieldIsPlaceholder < 1u << guardOfPinnedFieldShift);
static_assert(fieldMayBeUnlisted < 1u << guardOfPinnedFieldShift);
static_assert(fieldHoldsClosureMadeWhenRead < 1u << guardOfPinnedFieldShift);

// Which shapes pass. Program::passes() says it in code.
//
// A range, `set == UINT32_MAX`: the `count` shapes first .. last(). No count: none.
//
// A set, any other `set`: of the shapes first .. last() ONLY those that have a bit. Here `count` is how far the set reaches, not how many
// pass. WHOEVER GOES BY `first` AND `count` WITHOUT LOOKING AT `set` LETS OBJECTS PASS THAT ARE NOT OF THE TYPE, and what follows reads
// their bytes as fields. All sets share one row of bits, Program::shapeSetBits; at run time these are 32-bit words from
// Instance::typedShapeSets on (Data::shapeSetsOffset). `set` is the number of the first bit of this set in the row, a multiple of 32. The
// bits go from the last shape down: shape `last() - i` has bit `set + i`, for i < count. Bits 0 and count - 1 are always set.
//
// Compiled, with `word` the 32-bit shape word of a cell (0 - shape; in a cell that is no object of the program the upper half of a pointer,
// or zero, see AOTTypedData.h):
//     i = word + last()                                           in 32 bits, wrapping
//     if (i >= count) fail                                        unsigned; a range is done here, and with count 1 it is `i != 0`
//     bits = load32(typedShapeSets + set / 8 + (i >> 5) * 4)      `set / 8` is a constant
//     if (!(bits >> (i & 31) & 1)) fail
// Only the comparison keeps the load within the row: the load depends on it (not IsSafe), or `i` is clamped first. With count <= 64 the
// bits may be an immediate instead of a load. Tests of several views: merge ranges that are adjacent, placement tries to make them so.
struct ShapeTest {
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint32_t set { UINT32_MAX };
    uint8_t blockByte { 0 };
    bool passesEarlierBlocks { false };

    bool isRange() const { return set == UINT32_MAX; }
    bool passesNothing() const { return !count; }
    uint32_t last() const { return first + count - 1; }
};

// A string or a number that a literal type stands for.
struct Literal {
    // None: it is a number.
    StringID string;
    double number { 0 };
};

struct Field {
    StringID name;
    // What the type of this layout says the property is. None: anything.
    TypeID type;
    uint32_t flags { 0 };
    // See Format::Field::literals. The run time looks at them, and what goes by Allocation::discriminant.
    Vector<Literal> literals;
    // See Format::Field::typeText.
    StringID typeText;
    // From placement on. The field of a view that some operation of the program gets at through the view (fieldOf(),
    // Lowering::fieldExpectedFor()) and the fields of that name of its members are one slot: one offset, one way to store. So are a field
    // and that of a specialization. Any other field has a slot of its own, however many layouts have a field of the name. It holds
    // what the widest of their types says, `stored`, going by the layouts that objects are made as and by what compiled code stores
    // through the others. Where that is more than `type`, a read tests. It may be less than the `type` of a view that nothing is made
    // as: what such a view calls optional is what a read may find in some member. With fieldIsNeverStored: no slot, no
    // `stored`, an offset that no object reaches, and no member has it.
    SlotID slot;
    TypeID stored;
    TypeID storedInSlot;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
    // The slot is in the extension, and `offset` is from the start of that: see offsetOfExtension in AOTTypedData.h. Then `store` is
    // JSValue, `stored` admits Absent, no allocation has the field among its born fields, and it is neither immutable nor a discriminant.
    bool isOutOfLine { false };
    // Not every member of this layout has a field for the property. It may be optional. It may be required and be on the prototype of
    // a class (a method, an accessor) or on Object.prototype (`toString`). An access tests the shape for the members that have the
    // field. Of any other object it is read the general way (extra properties, prototype chain), and what that yields is held to the
    // type of the value. A store to any other object goes to the run time.
    // Also set, with a test that nothing passes, for a field that no operation gets at through this layout: a member may have the
    // property somewhere else. Only the first stores to an object that has just been made of the layout go by such a field.
    bool isLackedBySomeMember { false };
    bool isAnyObjectInSomeMember { false };
    bool isTypedOtherwiseInSomeView { false };
    bool mayBecomeItsTypeInSomeMember { false };
    ShapeTest membersWithIt;
    bool isElsewhereInSomeMember { false };
    // From placement on: see fieldIsPinned in AOTTypedData.h. Of a field of a layout that objects are made as.
    bool isPinned { false };
    // Of a field of a view: every object that gets there is of a member, and each member has it, pinned.
    bool isPinnedInEveryMember { false };
    bool holdsWholeNumbers { false };
    // See Format::Field::onlyFunction and Format::OnlyFunctionFlag.
    FunctionID onlyFunction;
    bool onlyFunctionIsFact { false };
    bool onlyFunctionOrNil { false };
    FunctionID otherFunction;
    FunctionID closureMadeWhenRead;
    ExpectedBuiltins expectedBuiltins;
    StringID placeholderFor;
    bool hasSlotInEveryMember { false };
};

struct Method {
    // None: the name is computed.
    StringID name;
    FunctionID function;
    MethodKind kind { MethodKind::Method };
    // The variable of the class that holds a computed name, see Layout::environment.
    uint32_t key { Format::none };
    // See Format::MethodMayBeReplaced.
    bool mayBeReplaced { false };
};

struct StaticField {
    StringID name;
    TypeID type;
    uint32_t place { Format::none };
};

// Of the objects of a type of the program: an interface, a type alias, the type of a literal, a class. Also of environments, closures
// and the frames of coroutines, which the language does not see as objects.
//
// It is two things. Objects are allocated with it, if anything does that. And it is a view: the way to look at the objects in places of
// the type, which were allocated with one of its members.
//
// Every class has one, of the kind Instance, which also says what the class is made of. See Format::Shape.
struct Layout {
    LayoutKind kind { LayoutKind::Record };
    StringID name;
    LayoutID base;
    Vector<Field> fields;
    // Of a class: its constructor, whose closure is the class object, what the prototype object has, what the class object has.
    // `methods` of a Record: the accessors that an object literal of the type declares. There is no field of the name: the object gets the
    // accessor as an extra property where the literal is evaluated, and may come to have something else under the name. So all this
    // says is that an access by the name may run code: see onlyFieldsSupply(). The run time is not told.
    FunctionID constructor;
    // Of a class that is evaluated once and whose declaration names the one closure of a constructor for what it extends, where `base` does not say it: see findDeclaredParents().
    FunctionID declaredParentConstructor;
    bool isDeclaredToExtendGlobal { false };
    Vector<Method> methods;
    Vector<Method> staticMethods;
    Vector<StaticField> staticFields;
    uint32_t classObjectPlace { Format::none };
    // Of a class that is not evaluated once: of the environment that its class object captures. A variable of the class is a field of that.
    // None: a variable of the class is a module variable. See Format::Shape::environment.
    LayoutID environment;
    // See Format::ShapeFlag.
    bool isLaidOut { false };
    bool isEvaluatedOnce { false };
    bool hasForeignRoot { false };
    bool hasFixedMembers { false };
    bool mayHaveOtherPrototype { false };
    bool constructorMayBeReplaced { false };
    bool mayBeMadeByNew { false };
    // The prototype object has a method whose name is computed and may be any string: what an object of the class, or of one that extends
    // it, finds under a name is not known here.
    bool hasMethodOfUnknownName { false };
    // The same of the class object. See Format::ShapeHasStaticMemberOfUnknownName.
    bool hasStaticMemberOfUnknownName { false };
    // From before types are frozen. A cast may turn an object of the class, or of one that extends it, into one of the layout of a record,
    // where it is. It keeps its prototype, and its shape no longer says what made it.
    bool instancesMayBecomeRecords { false };
    bool isAllocated { false };
    // The program is seen to freeze or seal objects that are looked at as a type this is a member of. A store to a field tests for that.
    // Freezing an object of any other layout is refused.
    bool mayBeFrozen { false };
    bool isStoredToOnlyAtBirth { false };
    bool mayGiveUp { false };
    // As a view. The checker has seen an object that the program has not laid out, which honestly is of the type, get into a place of the
    // type: an object of the host, of a class of a `.d.ts` file or of JavaScript. It can be neither copied nor turned away. So all that a
    // type with this view says of an object is that it is one: ANY object is of the type, also one of the program that is no member.
    // An access applies `test`: what passes is accessed in place, as through any view. Of anything else the object itself is asked, the
    // general way, which may run code, and what that yields is held to its type. Nothing may conclude from the type that an object has the
    // fields, or that it is of the program.
    bool admitsForeignObjects { false };
    bool isOpenedForLog { false };
    bool admitsOnlyOtherLayouts { false };
    bool isMemberOnlyView { false };
    bool checksForeignObjects() const { return isOpenedForLog || admitsOnlyOtherLayouts; }
    bool admitsUncheckedObjects() const { return admitsForeignObjects && (!admitsOnlyOtherLayouts || isOpenedForLog); }
    // An object of it, and not `undefined` or `null`: Type::view(this layout). Of every layout, from before types are frozen.
    TypeID type;
    // The frame of a coroutine: the run time stores to its fixed fields, see CoroutineFrame.
    bool isCoroutineFrame { false };
    // Of an environment, from optimize() on: it is no cell. See findEnvironmentsInFrames().
    bool isInFrame { false };
    VariableID reuseVariable;
    bool mayBeInFrame { false };
    // The allocated layouts whose objects get into places of this type untested, itself among them if it is allocated. The front end
    // says which, from what the checker has found assignable. Not each has a field for every property: see Field::isLackedBySomeMember. Sorted.
    Vector<LayoutID> members;
    Vector<LayoutID> membersByFit;
    Vector<uint32_t> shapesOfMembers;
    // What `as` may turn an object into, where it is, that was allocated with this layout: layouts that have all its slots. Sorted.
    Vector<LayoutID> specializations;
    // Among Program::allocations: one of a record of the layout with Object.prototype, whatever it is born with. For a copy, which has its keys from what is copied.
    uint32_t allocationOfClone { Format::none };

    // From placement on.
    uint32_t size { 0 };
    // What an allocation takes that may be specialized: the size of the largest.
    uint32_t sizeWithRoomToSpecialize { 0 };
    // From placement on. The layout is in a family: the transition closure of an allocation-site layout whose objects have an integer
    // discriminant that never changes after allocation (Allocation::discriminant). This is that field, as an index into `fields`.
    uint16_t discriminant { UINT16_MAX };
    // From placement on. Objects of the layout have the pointer to an extension at offsetOfExtension. So has every layout they can
    // transition to or from. extensionSize: see LayoutData.
    bool hasExtension { false };
    uint32_t extensionSize { 0 };
    uint32_t offsetOfExtraProperties { 0 };
    // In a family, and does not list exactly one constant for the discriminant: an abstract supertype or a union. Coerce never transitions
    // an object into it. A layout that lists one constant only transitions to layouts that list the same one.
    bool isAbstract { false };
    StringID declaredPath;
    uint32_t declaredLine { 0 };
    // Its number among the allocated layouts, from 1. Zero: no objects.
    uint32_t shape { 0 };
    // Whether an object is of a member.
    ShapeTest test;
    // Of a class: whether an object was made by it or by a class that extends it. Not `test`: a record may be assignable to a class.
    ShapeTest instances;
    // As a view: whether an object is of a layout that a cast to the view makes a transition from. Either it is no member, and the
    // transition makes it one. Or it is a member that lacks fields of the view and has the view's own layout among its specializations.
    ShapeTest retagSources;
    // Whether an object is of a member that a cast to the view leaves as it is: `test` without the members among retagSources, whether
    // or not the program has a Coerce to the view.
    ShapeTest castTest;
    // The keys of an object are in the order its properties were created in, which is no part of where the fields are, nor of the
    // shape. An order is a list of indexes into `fields`. An object says which it goes by and how far it has got: the order word, see
    // AOTTypedData.h. These are the orders the allocations of the program expect. The run time adds others.
    Vector<Vector<uint16_t>> orders;
    // The fields by name, those of one name in their own order: what fieldNamed() searches. A layout that indexFields() has made none for,
    // because it has few fields, or whose fields have changed since, is searched one by one.
    Vector<uint32_t> fieldsByName;

    void indexFields();
    std::optional<uint32_t> fieldNamed(StringID) const;
};

struct Slot {
    TypeID type;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
    bool isOutOfLine { false };
    // The discriminant of a family, see Layout::discriminant. It never changes once it is set: a store that is not an initializing store
    // goes through only if the field is absent or already holds the value.
    bool isDiscriminant { false };
    // Some field in it is pinned: whoever stores to it sees to the byte.
    bool hasPinnedFields { false };
};

struct Variable {
    StringID name;
    ModuleID module;
    // None: anything.
    TypeID type;
    // It holds `undefined` from before any code runs. Otherwise nothing, until it is initialized: a read in between throws.
    bool startsUndefined { false };
    // There is no read in between: see Format::Initialization::NeverSeen. So a read tests nothing and cannot throw, and nothing has to
    // stand for "nothing" in the variable, which is kept as its type alone says.
    bool isNeverSeenUninitialized { false };
    // And it has one store. To every function but the one with the store it never changes.
    bool isStoredOnce { false };
    bool hasCard { false };
    // See Format::Initialization::Zero.
    bool startsZero { false };
    bool mayBeSeenUninitialized() const { return !startsUndefined && !isNeverSeenUninitialized && !startsZero; }
    // See Format::Field::onlyFunction and Format::OnlyFunctionFlag.
    FunctionID onlyFunction;
    bool onlyFunctionIsFact { false };
    bool onlyFunctionOrNil { false };
    ExpectedBuiltins expectedBuiltins;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
};

struct Module {
    StringID path;
    FunctionID initializer;
    StringID displayPath;
    // Of Format::Module::lineTable, for the dump.
    uint32_t lineTableSize { 0 };
    std::span<const uint8_t> printed;
    // Byte offset of each line.
    Vector<uint32_t> lineStarts;
    // For lines with non-ASCII text: the byte offset just past such a character, and how many fewer UTF-16 units than bytes its line has
    // up to there. Sorted by offset.
    struct ColumnAdjustment {
        uint32_t offset;
        uint32_t line;
        uint32_t excessBytes;
    };
    Vector<ColumnAdjustment> columnAdjustments;
};

class OpcodeSet {
public:
    constexpr OpcodeSet() = default;
    constexpr OpcodeSet(std::initializer_list<Opcode> opcodes)
    {
        for (Opcode opcode : opcodes)
            add(opcode);
    }

    constexpr void add(Opcode opcode) { m_words[static_cast<unsigned>(opcode) / 64] |= 1ull << (static_cast<unsigned>(opcode) % 64); }
    constexpr bool contains(Opcode opcode) const { return m_words[static_cast<unsigned>(opcode) / 64] >> (static_cast<unsigned>(opcode) % 64) & 1; }
    constexpr bool intersects(const OpcodeSet& other) const { return (m_words[0] & other.m_words[0]) | (m_words[1] & other.m_words[1]) | (m_words[2] & other.m_words[2]) | (m_words[3] & other.m_words[3]); }

private:
    std::array<uint64_t, 4> m_words { };
};

class Functions {
public:
    size_t size() const { return m_functions.size(); }
    bool isEmpty() const { return m_functions.isEmpty(); }

    const Function& operator[](size_t index) const LIFETIME_BOUND { return m_functions[index]; }
    Function& operator[](size_t index) LIFETIME_BOUND
    {
        forget(index);
        return m_functions[index];
    }
    Function& withSameValues(size_t index) LIFETIME_BOUND { return m_functions[index]; }

    const Function* begin() const LIFETIME_BOUND { return m_functions.begin(); }
    const Function* end() const LIFETIME_BOUND { return m_functions.end(); }
    Function* begin() LIFETIME_BOUND
    {
        forgetAll();
        return m_functions.begin();
    }
    Function* end() LIFETIME_BOUND { return m_functions.end(); }
    std::span<const Function> span() const LIFETIME_BOUND { return m_functions.span(); }

    void append(Function&& function) { m_functions.append(WTF::move(function)); }
    void appendVector(Vector<Function>&& functions) { m_functions.appendVector(WTF::move(functions)); }
    void grow(size_t size) { m_functions.grow(size); }
    void reserveInitialCapacity(size_t size) { m_functions.reserveInitialCapacity(size); }
    auto map(NOESCAPE const auto& functor) const { return m_functions.map(functor); }

    void listValuesInOrder();
    void listForgottenValuesInOrder();
    void forgetAll() { m_isListed.clear(); }
    bool isListed(size_t index) const { return index < m_isListed.size() && WTF::atomicLoad(&m_isListed[index], std::memory_order_relaxed); }

    bool mayHold(size_t index, const OpcodeSet& wanted) const { return !isListed(index) || m_contents[index].opcodes.intersects(wanted); }
    bool mayEndIn(size_t index, TerminatorKind kind) const { return !isListed(index) || m_contents[index].terminatorKinds >> static_cast<unsigned>(kind) & 1; }
    bool mayHoldPredictedClosure(size_t index) const { return !isListed(index) || m_contents[index].holdsPredictedClosure; }

    void forEachValueInOrder(size_t index, const OpcodeSet& wanted, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
    {
        if (isListed(index)) {
            auto opcodes = opcodesInOrder(index);
            auto ids = m_valuesInOrder.items<&ValueInOrder::id>().subspan(m_firstInOrder[index], opcodes.size());
            for (size_t k = 0; k < opcodes.size(); ++k) {
                if (wanted.contains(opcodes[k]))
                    functor(ids[k]);
            }
            return;
        }
        auto& function = m_functions[index];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (wanted.contains(function.at(id).opcode))
                    functor(id);
            }
        }
    }

private:
    struct ValueInOrder {
        ValueID id;
        Opcode opcode { Opcode::Undefined };
    };

    std::span<const Opcode> opcodesInOrder(size_t index) const
    {
        return m_valuesInOrder.items<&ValueInOrder::opcode>().subspan(m_firstInOrder[index], m_numberInOrder[index]);
    }

    void forget(size_t index)
    {
        if (isListed(index))
            WTF::atomicStore(&m_isListed[index], static_cast<uint8_t>(0), std::memory_order_relaxed);
    }

    Vector<Function> m_functions;
    MultiArrayList<ValueInOrder, &ValueInOrder::id, &ValueInOrder::opcode> m_valuesInOrder;
    struct Contents {
        OpcodeSet opcodes;
        uint16_t terminatorKinds { 0 };
        bool holdsPredictedClosure { false };
    };

    Vector<uint32_t> m_firstInOrder;
    Vector<uint32_t> m_numberInOrder;
    Vector<Contents> m_contents;
    mutable Vector<uint8_t> m_isListed;
};

struct CallSignature {
    Vector<TypeID, 4> parameters;
    TypeID result;
    unsigned leastNumberOfArguments { 0 };
    bool hasRestParameter { false };

    friend bool operator==(const CallSignature&, const CallSignature&) = default;
};

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);
public:
    Program() = default;

    static std::unique_ptr<Program> tryRead(std::span<const uint8_t>, ASCIILiteral& whyFailed);

    std::span<const uint8_t> string(StringID id) const { return strings[id.index()]; }
    // What the table knows the string as, if it does.
    BuiltinName builtinName(StringID id) const { return m_builtinNames[id.index()]; }
    BuiltinID operationOf(const Value& value) const { return value.opcode == Opcode::CallBuiltin || value.opcode == Opcode::Operation ? value.builtin() : BuiltinID(); }
    bool usesOperation(BuiltinID operation) const { return m_operationsUsed[operation.index() / 32] >> operation.index() % 32 & 1; }
    // The other way around. Nothing: the program has no such string.
    StringID stringOf(BuiltinName name) const { return name.index() < m_stringsOfBuiltinNames.size() ? m_stringsOfBuiltinNames[name.index()] : StringID(); }
    Function& at(FunctionID id) { return functions[id.index()]; }
    const Function& at(FunctionID id) const { return functions[id.index()]; }
    Layout& at(LayoutID id) { return layouts[id.index()]; }
    const Layout& at(LayoutID id) const { return layouts[id.index()]; }
    Slot& at(SlotID id) { return slots[id.index()]; }
    const Slot& at(SlotID id) const { return slots[id.index()]; }
    const Type& at(TypeID id) const { return types.at(id); }
    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    // Its frames only exist because of how the code was compiled, or are those of a built-in. Stack traces leave them out.
    bool isHiddenInStackTraces(const Function& function) const { return function.isGenericEntrypoint() || function.has(Format::FunctionIsHiddenInStackTraces | FunctionIsHalfOfHiddenCoroutine) || function.resumeFunction || function.startFunction || function.isRampForClosures || entries.containsIf([&](FunctionID entry) { return &function == &at(entry); }) || (function.has(Format::FunctionIsBuiltin) && !isListedAsBuiltinInStackTraces(function)) || isPrivateBuiltinFunction(function); }
    size_t preludeFunctionThatIs(const Function& function) const
    {
        if (!function.has(Format::FunctionIsBuiltin))
            return notFound;
        return preludeFunctions.findIf([&](FunctionID id) {
            return id && &function == &at(id);
        });
    }
    bool standsForBuiltin(const Function& function) const { return preludeFunctionThatIs(function) != notFound; }
    bool isListedAsBuiltinInStackTraces(const Function& function) const
    {
        size_t which = preludeFunctionThatIs(function);
        return which != notFound && !leavesNoFrameInStackTraces(static_cast<PreludeFunction>(which));
    }
    BuiltinID builtinCarriedOutBy(const Function& function) const
    {
        size_t which = preludeFunctionThatIs(function);
        return which == notFound ? BuiltinID() : Builtins::singleton().entryCarriedOutBy(static_cast<PreludeFunction>(which));
    }
    // It runs the body of a coroutine: what it captures is the frame.
    bool isResumeFunction(const Function& function) const
    {
        LayoutID captured = function.captureTypes.size() == 1 ? at(function.captureTypes[0]).onlyView() : LayoutID();
        return captured && at(captured).isCoroutineFrame;
    }
    // Whether an object that is looked at as the one layout says can be looked at as the other says: every member of the one is a member
    // of the other, and the other admits foreign objects if the one does. Then it takes any object.
    bool isUpcast(LayoutID, LayoutID) const;
    ValueID constantOf(const Function&, ValueID) const;
    ValueID originOf(const Function&, ValueID) const;
    ExpectedBuiltins expectedBuiltins(const Function&, ValueID) const;
    // Whether a cast for the reason has nothing to do for a value of the one type: it may be left out. `as` and `!` make something of an
    // object that is of a type only because the type admits foreign objects. What they do goes by the value and by the type they cast
    // to, never by how much a pass has found out about the operand. Store and Claim take such an object as it is.
    bool isHeldAlready(const Type& from, const Type& to, CastReason reason) const
    {
        return from.isWithin(to, *this, !isOpenedForLog(to) && (reason != CastReason::As || !annotatedPlaceLooksAtForeignObjects(to)));
    }
    bool isHeldAlready(const Type& from, const Type& to, const Value& cast) const
    {
        if (cast.castReason() == CastReason::As && cast.coercesFreshData())
            return from.isWithin(to, *this, false);
        return isHeldAlready(from, to, cast.castReason());
    }
    bool isOpenedForLog(const Type& to) const
    {
        bool isOpened = false;
        for (LayoutID view : to.views) {
            if (!at(view).admitsForeignObjects)
                continue;
            if (!at(view).isOpenedForLog)
                return false;
            isOpened = true;
        }
        return isOpened;
    }
    // Whether a place whose type is written down (`As`) makes anything of an object that is of the type only because the type admits foreign objects: it lays out what a plain object holds
    // (Coercion::keep()), and turns an object of the program into what it can still become. Not where no view of the type gives it anything to do: see viewsLeftAlone.
    bool annotatedPlaceLooksAtForeignObjects(const Type& to) const
    {
        for (LayoutID view : to.views) {
            if (!viewsLeftAlone.get(view.index()))
                return true;
        }
        return to.views.isEmpty();
    }
    bool passes(const ShapeTest& test, uint32_t shape) const
    {
        uint32_t i = test.last() - shape;
        return i < test.count && (test.isRange() || shapeSetBits.get(test.set + i));
    }
    // The classes of the engine (see Builtins::mayBeExtended) that a class of the program extends. The front end says which.
    bool isExtended(BuiltinName global) const { return extendedGlobals.get(global.index()); }
    // One-based line and UTF-16 column of a Value::position or Function::position.
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    BitVector extendedGlobals;
    // By the index of the string. See Format::Header.
    BitVector prototypePropertyNames;
    BitVector methodFieldNames;
    BitVector namesOfClosuresMadeWhenRead;
    BitVector namesThatMayBeAccessors;
    void findNamesThatMayBeAccessors();
    Vector<std::pair<StringID, StringID>> regExpLiterals;
    bool logsTypeErrors { false };
    Vector<uint32_t> sites;
    // See Format::Header.
    Vector<std::pair<uint32_t, FunctionID>> builtinModules;
    struct BuiltinFunction {
        uint32_t id;
        FunctionID function;
        uint32_t visibility;
    };
    Vector<BuiltinFunction> builtinFunctions;
    BitVector filesOfBuiltinFunctions;
    bool isOfFileOfBuiltinFunctions(const Function& function) const { return function.module && filesOfBuiltinFunctions.get(function.module.index()); }
    const BuiltinFunction* builtinFunctionAround(const Function& function) const
    {
        if (!isOfFileOfBuiltinFunctions(function))
            return nullptr;
        const BuiltinFunction* found = nullptr;
        for (auto& builtin : builtinFunctions) {
            auto& candidate = at(builtin.function);
            if (candidate.module == function.module && candidate.position <= function.position && (!found || at(found->function).position < candidate.position))
                found = &builtin;
        }
        return found;
    }
    bool isPrivateBuiltinFunction(const Function& function) const
    {
        if (!isOfFileOfBuiltinFunctions(function))
            return false;
        auto* around = builtinFunctionAround(function);
        if (!around)
            return true;
        auto& builtin = at(around->function);
        return builtin.position == function.position && (around->visibility || builtin.declaredAs & functionIsAsync || visibilityInEngine(around->id));
    }
    const Type& typePassedAs(const Function& callee, unsigned index) const
    {
        if (index < callee.captureTypes.size())
            return at(callee.captureTypes[index]);
        unsigned position = index - callee.captureTypes.size();
        TypeID declared;
        if (callee.has(Format::FunctionHasThis) && !position--)
            declared = callee.thisType;
        else if (position < callee.parameterTypes.size())
            declared = callee.parameterTypes[position];
        else
            return at(callee.at(callee.blocks[0].parameters[index]).type);
        return at(declared ? declared : types.anything);
    }
    bool makesTailCallsAsBuiltin(const Function& function) const
    {
        if (function.isGenericEntrypoint() || function.resumeFunction || function.startFunction || function.declaredAs & (functionIsAsync | functionIsGenerator) || isResumeFunction(function))
            return false;
        if (standsForBuiltin(function))
            return true;
        auto* around = builtinFunctionAround(function);
        return around && isStrictInEngine(around->id);
    }
    struct FunctionByKey {
        StringID property;
        StringID key;
        FunctionID function;
    };
    Vector<FunctionByKey> functionsByKey;
    // What a dictionary held under the property is expected to have under the key. None: nothing is expected.
    FunctionID functionByKey(StringID property, StringID key) const;

    StringID intern(std::span<const uint8_t>);
    // From here on the string `from`, if the program has it, reads `to`. Before any pass has run.
    void replaceString(std::span<const uint8_t> from, std::span<const uint8_t> to);
    std::optional<StringID> find(std::span<const uint8_t>) const;

    void dump(PrintStream&) const;
    void dump(PrintStream&, FunctionID) const;
    void dump(PrintStream&, const Type&) const;

    std::span<const uint8_t> buffer;
    Vector<std::span<const uint8_t>> strings;
    Vector<std::span<const uint8_t>> webAssemblyModules;
    Vector<Layout> layouts;
    Vector<Variable> variables;
    Vector<Module> modules;
    Functions functions;
    Vector<unsigned> sizesForInlining;
    Vector<uint32_t> firstViewOfLayout;
    Vector<LayoutID> viewsOfLayouts;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsStoredToThroughLayouts;
    struct FactsForInlining {
        bool isAccessor { false };
        bool returnsMethodTable { false };
        BitVector parametersCalled;
        BitVector parametersCalledInLoops;
    };
    Vector<FactsForInlining> factsForInlining;
    static constexpr uint32_t hasNoMakerOfClosures = UINT32_MAX;
    static constexpr uint32_t hasSeveralMakersOfClosures = UINT32_MAX - 1;
    Vector<uint32_t> onlyMakersOfClosures;
    Vector<BitVector> blocksThatNeverReturn;
    // What is left of that behind the inlining: no path from the first block reaches a return.
    BitVector functionsThatNeverReturn;
    bool neverReturns(FunctionID id) const { return functionsThatNeverReturn.get(id.index()); }
    // In the order of the entry points. Each is compiled in the place of the top-level code of a module.
    Vector<FunctionID, 1> entries;
    bool isEntry(FunctionID id) const { return entries.contains(id); }
    // By entry: what the program data knows that module by (ProgramModule::entryOffset). The link says it, for Data::isEntryPoint().
    Vector<uint32_t, 1> modulesOfEntries;

    // By string: its number among the identifiers of the whole image, which the caches go by. Zero: it has none.
    Vector<uint32_t> identifierNumbers;
    // The names by which values of unknown layout are accessed.
    enum class AccessByName : uint8_t { Read, Store, Definition, Call, Other };
    struct UsesOfName {
        StringID name;
        uint32_t uses { 0 };
        uint32_t usesThatAskTheTable { 0 };
        std::array<uint32_t, 4> usesByAccess { };
    };
    Vector<UsesOfName> genericAccessIdentifiersAndNumberOfUses() const;
    Vector<uint32_t> keysOfStringsAskedForMost(unsigned limit) const;
    void readNamesAndStringsOffValues();

    TypeTable types;
    BitVector typesNamedAtRunTime;
    bool isNamedAtRunTime(TypeID type) const { return typesNamedAtRunTime.get(type.index()); }
    LayoutID closureLayout;
    // From the linking of allocations on, by half the number of captures, rounded up: what a closure is allocated with. None: the program
    // makes no such closure.
    Vector<uint32_t> closureAllocations;
    uint32_t allocationOfClosure(unsigned numberOfCaptures) const { return closureAllocations[numberOfCaptures / 2]; }
    // Of the closures of the function, which is not a generic entrypoint: how many words the collector follows, and how many words there are.
    std::pair<unsigned, unsigned> referencesAndCapturesOfClosures(const Function&) const;
    bool makesSlimClosures(const Function& function) const { return mayMakeSlimClosures(function) && function.captureTypes.size() <= ClosureRequest::maxNumberOfCapturesOfSlimClosure; }
    bool mayMakeSlimClosures(const Function&) const;
    // By PreludeFunction. None: the program has no such function.
    Vector<FunctionID> preludeFunctions;
    static constexpr unsigned maxArgumentsOfGuardedPreludeEntry = 3;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> guardedPreludeEntries;
    static constexpr uint32_t keyOfGuardedPreludeEntry(FunctionID prelude, unsigned numberOfArguments, std::optional<unsigned> numberOfCapturesOfCopiedArgument, bool holdsResultToType) { return ((prelude.index() * (maxArgumentsOfGuardedPreludeEntry + 1) + numberOfArguments) * (ClosureRequest::maxNumberOfCapturesOfSlimClosure + 2) + (numberOfCapturesOfCopiedArgument ? *numberOfCapturesOfCopiedArgument + 1 : 0)) * 2 + holdsResultToType; }
    // By EngineClass::index: an instance, and not `undefined` or `null`. Of the classes a receiver is tested for, and of strings.
    Vector<TypeID> engineClassTypes;
    // By BuiltinID: what the table says the built-in yields, where that is a range of integers, with other primitives beside it or without. Else none.
    Vector<TypeID> integersYieldedByBuiltins;
    // From the linking of allocations on: every way objects come to be, in the order of the functions.
    Vector<Allocation> allocations;
    // From the effect summaries on. Some layout has specializations, so that an `as` or `!` may write the shape word of an object.
    bool castsMayRetag { false };
    bool mayRunInSeveralInstances { false };
    Vector<LayoutID> memberOnlyViews;
    BitVector viewsThatKeepTheirMembers;
    // From before the functions are optimized on. By layout, as a view: a cast to it may turn an object into one of a specialization of
    // its layout where it is. That is the layout itself if objects are made of it, else one of its members.
    BitVector viewsRetaggedTo;
    BitVector viewsInstantiatedTo;
    // From the same time on; empty before, and in a program that is compiled for an audit. By layout, as a view: no cast retags to it, and of what the fields of the records hold that objects of it are made of, and of all that hangs
    // below, nothing can be made: no field has a type with views, or that type admits foreign objects and its views are such views themselves. Coercion::keep() would walk it all and change nothing.
    BitVector viewsLeftAlone;
    // From placement on.
    Vector<Slot> slots;
    uint32_t numberOfShapes { 0 };
    Vector<uint32_t> numberOfShapesUpTo;
    uint32_t numberOfShapesWithin(uint32_t first, uint32_t last) const { return first > last ? 0 : numberOfShapesUpTo[last] - numberOfShapesUpTo[first - 1]; }
    // The bits of all tests that are no range, one set after the other. See ShapeTest.
    BitVector shapeSetBits;
    struct TestOfSeveralViews {
        Vector<LayoutID, 1> views;
        ShapeTest test;
        uint32_t numberOfShapes { 0 };
    };
    enum class AskedOfViews : uint8_t { Members, MembersKeptByCast, RetagSources };
    UncheckedKeyHashMap<uint64_t, TestOfSeveralViews> testsOfSeveralViews;
    static uint64_t keyOfSeveralViews(const Vector<LayoutID, 1>& views, AskedOfViews asked)
    {
        uint64_t key = 0x9e3779b97f4a7c15ull + static_cast<uint64_t>(asked);
        for (LayoutID view : views)
            key = (key ^ view.index()) * 0x100000001b3ull;
        return key | 1;
    }
    const ShapeTest* testOfAll(const Vector<LayoutID, 1>& views, AskedOfViews asked) const
    {
        auto found = testsOfSeveralViews.find(keyOfSeveralViews(views, asked));
        return found != testsOfSeveralViews.end() && found->value.views == views ? &found->value.test : nullptr;
    }
    StringID emptyString;
    // In the statics: cells. See Placement::placeArraySites().
    uint32_t copyOnWriteButterfliesOffset { 0 };
    uint32_t numberOfCopyOnWriteButterflies { 0 };
    Vector<Vector<uint64_t>> elementsOfArrayLiterals;
    static constexpr unsigned maxLengthOfArrayLiteralMadeInline = 16;
    // In the statics: cells, one behind the other: the one object of a RegExp literal for the sites where nothing can get hold of it. By keyOfRegExpLiteral(). See
    // Placement::placeRegExpLiterals().
    uint32_t sharedRegExpObjectsOffset { 0 };
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> offsetsOfSharedRegExpObjects;
    uint32_t sizeOfStatics { 0 };
    uint32_t sizeOfStaticsAsPlaced { 0 };
    uint32_t sizeOfStaticsFilledLate { 0 };
    // For reads and for stores, by string, from one. Zero: the name has none.
    std::array<Vector<uint32_t>, 2> globalPropertyCacheNumbers;
    uint32_t globalPropertyCacheNumber(StringID name, bool isStore) const { return name.index() < globalPropertyCacheNumbers[isStore].size() ? globalPropertyCacheNumbers[isStore][name.index()] : 0; }
    // A free name that is the global object's `this` unless the program has replaced it.
    bool isNameOfGlobalThis(StringID name) const
    {
        auto bytes = string(name);
        return equalSpans(bytes, "globalThis"_span8) || equalSpans(bytes, "global"_span8) || equalSpans(bytes, "self"_span8) || equalSpans(bytes, "window"_span8);
    }
    // By string, from one, among the same caches. Zero: the name has none.
    Vector<uint32_t> propertyOfGlobalThisCacheNumbers;
    uint32_t propertyOfGlobalThisCacheNumber(StringID name) const { return name.index() < propertyOfGlobalThisCacheNumbers.size() ? propertyOfGlobalThisCacheNumbers[name.index()] : 0; }
    bool isDataMemberOfHostObject(StringID object, StringID name) const
    {
        auto isOneOf = [bytes = string(name)](auto... names) { return (equalSpans(bytes, names) || ...); };
        if (equalSpans(string(object), "process"_span8)) {
            return isOneOf("env"_span8, "stdout"_span8, "stderr"_span8, "stdin"_span8, "pid"_span8, "version"_span8, "versions"_span8, "execPath"_span8, "argv0"_span8, "platform"_span8, "arch"_span8,
                "release"_span8, "features"_span8, "config"_span8, "isBun"_span8, "browser"_span8, "send"_span8, "channel"_span8);
        }
        if (equalSpans(string(object), "Bun"_span8))
            return isOneOf("env"_span8, "version"_span8, "revision"_span8, "stdout"_span8, "stderr"_span8, "stdin"_span8, "argv"_span8, "cwd"_span8, "isMainThread"_span8);
        return false;
    }
    bool isFunctionOfHostObject(StringID object, StringID name) const
    {
        auto isOneOf = [bytes = string(name)](auto... names) { return (equalSpans(bytes, names) || ...); };
        return equalSpans(string(object), "Buffer"_span8) && isOneOf("byteLength"_span8, "from"_span8, "isBuffer"_span8, "alloc"_span8, "allocUnsafe"_span8, "concat"_span8, "compare"_span8, "isEncoding"_span8);
    }
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> memberOfGlobalCacheNumbers;
    uint32_t memberOfGlobalCacheNumber(StringID global, StringID name) const { return memberOfGlobalCacheNumbers.get(static_cast<uint64_t>(global.index()) << 32 | name.index()); }
    uint32_t numberOfGlobalPropertyCaches { 0 };
    uint32_t globalPropertyCachesOffset { 0 };
    uint32_t numberOfStaticFieldPlaces { 0 };
    uint32_t staticFieldPlacesOffset { 0 };
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> genericEntrypoints;
    uint32_t firstGenericEntrypoint { 0 };
    uint32_t numberOfConsecutiveGenericEntrypoints { 0 };
    Vector<CallSignature> callSignatures;
    struct ShapeOfValueCall {
        Vector<TypeID, 4> parameters;
        TypeID result;
        FunctionID adapter;
    };
    Vector<ShapeOfValueCall> shapesOfValueCalls;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> shapesOfValueCallsByHash;
    FunctionID adapterOfCall(const Value&) const;
    FunctionID firstAdapterOfSignature;
    uint32_t indexOfSignatureOf(FunctionID adapter) const { return adapter.index() - firstAdapterOfSignature.index(); }
    UncheckedKeyHashMap<uint32_t, TypeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbersOrUndefined;
    // By the function and how many arguments are bound: see createBoundFunctions().
    static constexpr unsigned maxBoundArguments = 3;
    UncheckedKeyHashMap<uint64_t, FunctionID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> boundFunctions;
    static constexpr uint64_t keyOfBoundFunction(FunctionID function, unsigned numberOfBoundArguments) { return static_cast<uint64_t>(function.index()) << 2 | numberOfBoundArguments; }
    FunctionID boundFunction(FunctionID function, unsigned numberOfBoundArguments) const { return boundFunctions.get(keyOfBoundFunction(function, numberOfBoundArguments)); }
    // Where the program spends its time, as far as its text says: see findFunctionsHotByText(). Elsewhere a small form is worth more than a fast one.
    BitVector functionsHotByText;
    // What onlyFieldsSupply() has answered, by view, name and flag. Every pass asks for every access, from all threads, and the answer takes a walk of all members.
    struct AnswersByViewAndName {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> answers;
    };
    mutable std::array<AnswersByViewAndName, 64> whetherOnlyFieldsSupply;
    mutable std::array<AnswersByViewAndName, 64> whetherStoreMayTransition;
    struct TypesByViewAndName {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, TypeID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> types;
    };
    mutable std::array<TypesByViewAndName, 64> widestTypesStoredInFields;
    bool isHotByText(FunctionID id) const { return functionsHotByText.get(id.index()); }
    BitVector functionsWarmByText;
    bool isWarmByText(FunctionID id) const { return functionsWarmByText.get(id.index()); }
    BitVector functionsCalledNearLoops;
    bool isCalledNearLoops(FunctionID id) const { return functionsCalledNearLoops.get(id.index()); }
    BitVector functionsRareByText;
    bool isRareByText(FunctionID id) const { return functionsRareByText.get(id.index()); }
    BitVector functionsColdByGraph;
    bool functionLengthsAreBytes { false };
    BitVector functionsThatTakeNoCopies;
    uint16_t licencesTheyAreNotGiven { 0 };
    bool takesNoCopy(FunctionID id, uint8_t licence) const { return licencesTheyAreNotGiven >> licence & 1 && functionsThatTakeNoCopies.get(id.index()); }
    bool isColdByGraph(FunctionID id) const { return functionsColdByGraph.get(id.index()); }
    // Those of them that have a loop, that no loop calls and that nothing hot makes: outside its loops such a function is like any that is not hot.
    BitVector functionsHotOnlyInTheirLoops;
    BitVector functionsInCycles;
    bool isInCycle(FunctionID id) const { return functionsInCycles.get(id.index()); }
    bool isHotOnlyInItsLoops(FunctionID id) const { return functionsHotOnlyInTheirLoops.get(id.index()); }
    bool isHotOutsideItsLoops(FunctionID id) const { return isHotByText(id) && !isHotOnlyInItsLoops(id); }
    bool mayMeetMasquerader { false };
    bool passesExceptionsOn { false };
    // By function: the direct calls of it in the program as read, that of its general entry not counted.
    Vector<uint32_t> numberOfDirectCalls;
    // From optimize() on: the functions that only direct calls reach and that no call is left of. They have no code.
    BitVector functionsWithoutCalls;
    BitVector functionsEnteredWithRoomOnStack;
    BitVector generalEntriesWhoseFunctionAsks;
    bool isEnteredWithRoomOnStack(FunctionID id) const { return functionsEnteredWithRoomOnStack.get(id.index()); }
    bool hasCode(FunctionID id) const { return !functionsWithoutCalls.get(id.index()); }
    BitVector adaptersCalledBySignature;
    UncheckedKeyHashMap<uint64_t, FunctionID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> adaptersOfResults;
    static uint64_t keyOfAdapterOfResult(TypeID result, uint32_t castOfResult, size_t numberOfArguments) { return static_cast<uint64_t>(result.index()) << 32 | static_cast<uint64_t>(castOfResult & 0xff) << 8 | numberOfArguments; }
    uint64_t resultsThatCallsGoBy { 0 };
    uint32_t sizeOfResultsHoldingWhatIsYielded { 0 };
    bool callsBySignatureAreChosen { false };
    bool isAskedForBySomeCall(FunctionID valueEntry) const
    {
        if (!callsBySignatureAreChosen)
            return true;
        return adaptersCalledBySignature.get(at(valueEntry).adapterOfValueEntry.index());
    }
    FunctionID valueEntryWithCodeOf(FunctionID generalEntry) const
    {
        FunctionID entry = at(generalEntry).valueEntry;
        return entry && hasCode(entry) ? entry : FunctionID();
    }
    struct ModuleEvaluation {
        FunctionID function;
        VariableID isEvaluated;
        VariableID error;
        Vector<FunctionID, 4> callees;
        struct Fetch {
            uint32_t numberOfCalleesAhead;
            uint32_t externalModule;
        };
        Vector<Fetch> fetches;
        static constexpr uint32_t markOfFunction = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t markOfFetch = markOfFunction - 1;
    };
    Vector<ModuleEvaluation> moduleEvaluations;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> moduleEvaluationOfFunction;
    FunctionID moduleEvaluator;
    FunctionID moduleEvaluatorIfNeeded;
    FunctionID moduleEvaluatorFromOutside;
    FunctionID moduleEvaluatorForCallFromOutside() const { return moduleEvaluatorFromOutside ? moduleEvaluatorFromOutside : moduleEvaluator; }
    std::array<FunctionID, numberOfArgumentGPRs + 1> bodiesOfConstructions;
    FunctionID bodyOfConstruction(const Value& construct) const { return construct.arguments.size() < bodiesOfConstructions.size() ? bodiesOfConstructions[construct.arguments.size()] : FunctionID(); }
    UncheckedKeyHashMap<uint64_t, uint32_t> bodiesOfCasts;
    Vector<std::pair<LayoutID, FunctionID>> jsonSerializers;
    FunctionID jsonDispatcher;
    FunctionID jsonSerializerOf(LayoutID layout) const
    {
        auto found = std::ranges::lower_bound(jsonSerializers, layout.index(), { }, [](auto& entry) { return entry.first.index(); });
        return found == jsonSerializers.end() || found->first != layout ? FunctionID() : found->second;
    }
    static bool mayHaveBody(const Value& cast) { return cast.opcode == Opcode::Cast && (cast.castReason() == CastReason::As || cast.castReason() == CastReason::Claim) && cast.immediates[0] == Format::none; }
    static uint64_t keyOfBodyOfCast(const Value& cast) { return (static_cast<uint64_t>(cast.type.index()) + 1) << 32 | cast.immediates[1]; }
    FunctionID bodyOfCast(const Value& cast) const
    {
        if (bodiesOfCasts.isEmpty() || !mayHaveBody(cast))
            return FunctionID();
        auto found = bodiesOfCasts.find(keyOfBodyOfCast(cast));
        return found == bodiesOfCasts.end() ? FunctionID() : FunctionID(found->value);
    }
    // From optimize() on: the code of a module, and a function that nothing reaches but one direct call in code that runs once, outside every loop: `(function (E) { .. })(E || (E = {}))`. For what pays only by being run again.
    BitVector functionsThatRunOnce;
    bool runsOnce(FunctionID id) const { return functionsThatRunOnce.get(id.index()); }
    BitVector functionsReturningMapKeys;
    Vector<uint16_t> classesOfFreshResults;
    BitVector builtinCellsComparedWith;
    BitVector stringsMadeOnce;
    Vector<StringID> literalStrings;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> literalStringIndices;
    std::optional<uint32_t> literalStringIndex(StringID string) const
    {
        auto found = literalStringIndices.find(string.index());
        return found == literalStringIndices.end() ? std::nullopt : std::optional<uint32_t>(found->value);
    }
    struct RowOfBirth {
        uint32_t firstPlace { 0 };
        Vector<StringID, 8> names;
        bool isWhole { true };
        bool isOfThis { false };
        uint32_t number { Format::none };
    };
    Vector<RowOfBirth> rowsOfBirths;
    struct NumberedRow {
        Vector<StringID, 8> names;
        uint8_t inlineCapacity { 0 };
        bool hasNullPrototype { false };
    };
    Vector<NumberedRow> numberedRows;
    uint32_t firstNumberOfRowsOfThis { Format::none };
    Vector<std::array<uint32_t, 3>> rowsGrownInto;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>> numbersOfRowsByName;
    UncheckedKeyHashMap<uint32_t, Vector<LayoutID, 1>> recordsThatPlainDataBecomesByName;
    BitVector rowsThatStayAsBorn;
    UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>> numbersOfRowsByHash;
    uint32_t numberOfRow(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const;
    uint32_t ensureNumberOfRow(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype);
    bool hasAllocationAssertions { false };
    bool notesAllocations { false };
    // For dumpAtStage(.., "allocations"): by layout, the first thing that was seen to take an environment of it off the stack.
    struct UseOfEnvironment {
        ASCIILiteral operation;
        FunctionID function;
        uint32_t inlineFrame { 0 };
        uint32_t position { 0 };
    };
    UncheckedKeyHashMap<uint32_t, UseOfEnvironment, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whyEnvironmentsAreOnHeap;

private:
    friend class Preparation;
    friend class Reader;

    Vector<Vector<uint8_t>> m_ownStrings;
    static constexpr unsigned numberOfTablesOfStringIDs = 16;
    static unsigned tableOfStringWithHash(unsigned hash) { return hash >> 20 & (numberOfTablesOfStringIDs - 1); }
    std::array<UncheckedKeyHashMap<String, uint32_t>, numberOfTablesOfStringIDs> m_stringIDs;
    struct NamesAndStringsInValues {
        Vector<Vector<StringID>> namesDefinedAsAccessors;
        Vector<UsesOfName> usesOfNames;
        Vector<uint32_t> usesOfStringConstants;
    };
    std::optional<NamesAndStringsInValues> m_namesAndStringsInValues;
    void noteString(StringID);

    Vector<BuiltinName> m_builtinNames;
    Vector<StringID> m_stringsOfBuiltinNames;
    std::array<uint32_t, (numberOfBuiltins + 31) / 32> m_operationsUsed { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
