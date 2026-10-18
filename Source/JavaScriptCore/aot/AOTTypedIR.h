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
    v(IsPlainRecord) \
    v(GetPlainRecordField) \
    v(IsClassObjectUnmodified) \
    v(GetStaticField) \
    v(PutStaticField) \
    v(IsClosureOf) \
    v(IsBuiltinFunction) \
    v(BuiltinFunction) \
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

struct CompactListHeader {
    uint32_t size;
    uint32_t capacity;
};
inline constexpr CompactListHeader emptyCompactListHeader { 0, 0 };

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

    void reserveCapacity(size_t newCapacity)
    {
        if (newCapacity <= capacity())
            return;
        RELEASE_ASSERT(newCapacity <= UINT32_MAX);
        bool wasEmpty = m_storage == emptyStorage();
        auto* storage = static_cast<Storage*>(fastRealloc(wasEmpty ? nullptr : m_storage, sizeof(Storage) + newCapacity * sizeof(T)));
        if (wasEmpty)
            storage->size = 0;
        storage->capacity = newCapacity;
        m_storage = storage;
    }
    void reserveInitialCapacity(size_t initialCapacity) { reserveCapacity(initialCapacity); }
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
        auto* newEnd = std::remove_if(begin(), end(), matches);
        size_t removed = end() - newEnd;
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
    struct Storage : CompactListHeader {
        T* elements() { return reinterpret_cast<T*>(this + 1); }
    };
    static Storage* emptyStorage() { return static_cast<Storage*>(const_cast<CompactListHeader*>(&emptyCompactListHeader)); }

    Storage* m_storage { emptyStorage() };
};

using Arguments = CompactList<ValueID>;

static constexpr uint32_t closureIsStackAllocated = 1;

static constexpr uint32_t shapeTestIsExact = 1;
static constexpr uint32_t shapeTestIsForThisInstance = 2;
static constexpr uint32_t shapeTestGuardsFastPath = 3;

static constexpr unsigned maxNumberOfPlainRecordFields = 16;
static constexpr uint32_t plainRecordIsConverted = 1;

enum class SpeculatedMethod : uint8_t { None, ArrayPush, MapGet, MapHas, MapSet, SetHas, SetAdd, RegExpExec, RegExpTest, StringMatch, CollectionGet, CollectionHas, CollectionSet, CollectionAdd, ArrayPushReadAhead };

struct Value {
    Opcode opcode { Opcode::Undefined };
    Rep rep : 4 { Rep::Void };
    bool isInTailPosition : 1 { false };
    bool hasStringKeysByType : 1 { false };
    bool constructs : 1 { false };
    bool isSignatureCall : 1 { false };
    bool receiverHasOriginalStructure : 1 { false };
    bool operandClassIsProven : 1 { false };
    bool mustBeProperTailCall : 1 { false };
    bool isOnSlowPath : 1 { false };
    bool isFusedWithMap : 1 { false };
    bool guardsFastPath : 1 { false };
    bool dropsDroppableCallerFrame : 1 { false };
    bool leavesDroppableFrame : 1 { false };
    bool receiverHasSingleStructure : 1 { false };
    bool storesLazyClosure : 1 { false };
    bool isAwaitedWithoutPromise : 1 { false };
    bool takesElementsOfArrayLiteral : 1 { false };
    bool mayConvertPlainRecord : 1 { false };
    bool takesUnsharedString : 1 { false };
    bool callsPredictedStatic : 1 { false };
    SpeculatedMethod speculatedMethod : 4 { SpeculatedMethod::None };
    uint8_t predictedReceiverClassPlusOne : 6 { 0 };
    uint32_t position { 0 };
    TypeID type;
    TypeID prediction() const { return isExpected(ExpectedType) ? TypeID(expectation & maxExpectationPayload) : TypeID(); }
    void setPrediction(TypeID type) { expect(ExpectedType, type.index(), !!type); }
    FunctionID predictedClosure() const { return isExpected(PredictedClosure) ? FunctionID(expectation & maxExpectationPayload) : FunctionID(); }
    void setPredictedClosure(FunctionID function) { expect(PredictedClosure, function.index(), !!function); }
    StringID predictedByKey() const { return isExpected(PredictedByKey) ? StringID(expectation & maxExpectationPayload) : StringID(); }
    void setPredictedByKey(StringID name) { expect(PredictedByKey, name.index(), !!name); }
    TypeID assertedType() const { return isExpected(Asserted) ? TypeID(expectation & maxExpectationPayload) : TypeID(); }
    void setAssertedType(TypeID type) { expect(Asserted, type.index(), !!type); }
    enum : uint32_t { ExpectedType, PredictedClosure, PredictedByKey, Asserted };
    static constexpr uint32_t maxExpectationPayload = (1u << 30) - 1;
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
        expectation = index < maxExpectationPayload ? kind << 30 | index : UINT32_MAX;
    }
    uint32_t inlineFrame : 8 { 0 };
    uint32_t holdsCallSignature : 1 { 0 };
    uint32_t calleeOrSignaturePlusOne : 23 { 0 };
    std::array<ValueID, 3> children;
    std::array<uint32_t, 2> immediates { Format::none, Format::none };
    Arguments arguments;

    ValueID child(unsigned index) const { return children[index]; }
    bool hasCallSignature() const { return (opcode == Opcode::Call || opcode == Opcode::CallMethod) && holdsCallSignature; }
    uint32_t callSignature() const { return holdsCallSignature ? static_cast<uint32_t>(calleeOrSignaturePlusOne) - 1 : Format::none; }
    void setCallSignature(uint32_t signature)
    {
        holdsCallSignature = signature < (1u << 23) - 1;
        calleeOrSignaturePlusOne = holdsCallSignature ? signature + 1 : 0;
    }
    bool hasDifferentPredictionThan(const Value& other) const { return expectation != UINT32_MAX && !isExpected(Asserted) && (other.isExpected(Asserted) || expectation != other.expectation); }
    void copyPredictionFrom(const Value& other)
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
    bool isStackAllocatedClosure() const { return opcode == Opcode::NewClosure && immediates[1] == closureIsStackAllocated; }
    uint32_t resultIndex() const { return immediates[0]; }
    static constexpr uint32_t jumpsToContinuation = 1u << 30;
    static constexpr uint32_t maxContinuations = 16;
    static constexpr uint32_t inliningWasDeclined = Format::none - 2;
    bool isCallWithDeclinedInlining() const { return opcode == Opcode::CallDirect && inliningWasDeclined - immediates[1] < 16; }
    uint32_t declinedInliningReason() const { return inliningWasDeclined - immediates[1]; }
    bool isJumpToContinuation() const { return opcode == Opcode::CallDirect && immediates[1] - jumpsToContinuation < maxContinuations; }
    bool testsPinnedField() const { return opcode == Opcode::IsClassObjectUnmodified && immediates[1] != Format::none; }
    uint32_t continuation() const { return immediates[1] - jumpsToContinuation; }
    VariableID variable() const { return VariableID(immediates[0]); }
    bool readsInitializedVariable() const { return opcode == Opcode::GetModuleVariable && immediates[1] == Format::VariableIsKnownInitialized; }
    LayoutID layout() const { return LayoutID(immediates[0]); }
    uint32_t field() const { return immediates[1]; }
    uint32_t captureIndex() const { return immediates[1]; }
    uint32_t allocation() const { return immediates[1]; }
    bool catchesExceptionObject() const { return opcode == Opcode::CatchValue && immediates[0] == 1; }
    CastReason castReason() const { return static_cast<CastReason>(immediates[1] & 0xff); }
    void setCastReason(CastReason reason) { immediates[1] = (immediates[1] & ~0xffu) | static_cast<uint32_t>(reason); }
    static constexpr uint32_t castChecksAwaitResult = 1u << 19;
    bool checksAwaitResult() const { return opcode == Opcode::Cast && immediates[1] & castChecksAwaitResult; }
    Format::Freshness freshness() const { return static_cast<Format::Freshness>(immediates[1] >> Format::freshnessShift & 3); }
    bool reportsMismatchAtCaller() const { return opcode == Opcode::Cast && immediates[1] & Format::CastReportsMismatchAtCaller; }
    uint32_t mismatchedArgumentIndex() const { return (immediates[1] >> Format::mismatchedArgumentIndexShift & 0xff) == 0xff ? UINT32_MAX : immediates[1] >> Format::mismatchedArgumentIndexShift & 0xff; }
    bool coercesFreshData() const { return freshness() != Format::Freshness::None; }
    bool keepsUnconvertibleObjects() const { return opcode == Opcode::Cast && immediates[1] & Format::CastKeepsUnconvertibleObjects; }
    FunctionID parameterCheckCallee() const { return opcode == Opcode::Cast && !holdsCallSignature && calleeOrSignaturePlusOne ? FunctionID(static_cast<uint32_t>(calleeOrSignaturePlusOne) - 1) : FunctionID(); }
    void setParameterCheckCallee(FunctionID function)
    {
        holdsCallSignature = false;
        calleeOrSignaturePlusOne = function.index() < (1u << 23) - 1 ? function.index() + 1 : 0;
    }
    uint32_t tableInImage() const { return opcode == Opcode::CallBuiltin && !holdsCallSignature && calleeOrSignaturePlusOne ? static_cast<uint32_t>(calleeOrSignaturePlusOne) - 1 : Format::none; }
    void setTableInImage(uint32_t table)
    {
        RELEASE_ASSERT(opcode == Opcode::CallBuiltin && table < (1u << 23) - 1);
        holdsCallSignature = false;
        calleeOrSignaturePlusOne = table + 1;
    }
    TypeID coercedType() const { return TypeID(immediates[0]); }
    StringID operandSource() const { return string(); }
    StringID accessedName() const { return castReason() == CastReason::Coerce || castReason() == CastReason::NonNull ? StringID() : string(); }
    uint32_t vectorLengthHintOffset() const { return immediates[0]; }
    uint32_t copyOnWriteButterflyOffset() const { return immediates[1] & 1 ? Format::none : immediates[1]; }
    uint32_t unsharedConstantLiteral() const { return immediates[1] != Format::none && immediates[1] & 1 ? immediates[1] >> 1 : Format::none; }
    static constexpr uint32_t keyIsListed = 1;
    static constexpr uint32_t isForInKey = 2;
    bool readsForInKey() const { return opcode == Opcode::GetByVal && immediates[0] == keyIsListed; }
    bool readsForInKeys() const { return opcode == Opcode::GetByVal && immediates[0] == isForInKey; }
    BlockID parameterBlock() const { return BlockID(immediates[0]); }
    uint32_t parameterIndex() const { return immediates[1]; }
    BuiltinID builtin() const { return BuiltinID(immediates[0]); }
    uint32_t engineClass() const { return immediates[1]; }
    BuiltinName comparedGlobal() const { return opcode == Opcode::IsBuiltinFunction && immediates[1] != Format::none ? BuiltinName(immediates[1]) : BuiltinName(); }
    ValueID methodGetById() const { return children[1]; }
    bool interleavesKeysAndValues() const { return !!children[2]; }
    bool copiesOwnPropertyDescriptors() const { return !!children[2]; }
    BuiltinID methodGetByIdBuiltin() const { return hasKnownSlotWord() ? BuiltinID() : BuiltinID(immediates[1]); }
    bool isInitialStore() const { return immediates[1] != Format::none && !hasKnownSlotWord(); }
    static constexpr uint32_t numberOfKnownSlots = 62;
    static constexpr uint32_t isFirstStore = 0x200;
    static constexpr uint32_t usesNameLookup = 0x400;
    bool hasKnownSlotWord() const { return (opcode == Opcode::GetById || opcode == Opcode::PutById) && immediates[1] >> 30 == 1; }
    bool isAtKnownSlot() const { return hasKnownSlotWord() && !(immediates[1] & usesNameLookup); }
    bool isStoreIntoInitialStructure() const { return opcode == Opcode::PutById && hasKnownSlotWord() && immediates[1] & isFirstStore; }
    static constexpr uint32_t maxInitialStructureOfFirstStore = 0x3ffff;
    uint32_t initialStructureOfFirstStore() const { return immediates[1] >> 11 & 0x3ffff; }
    void setInitialStructureOfFirstStore(uint32_t allocationSite) { immediates[1] = (immediates[1] & ~(0x3ffffu << 11)) | allocationSite << 11; }
    unsigned firstStoreAttributes() const { return (immediates[1] >> 7 & 1) << 1 | (immediates[1] >> 29 & 1) << 3; }
    bool hasInitialStructure() const { return opcode == Opcode::NewObject && immediates[0] != Format::none; }
    uint32_t initialStructure() const { return immediates[0]; }
    uint32_t initialStructureDescriptor() const { return opcode == Opcode::NewObject ? immediates[1] : Format::none; }
    uint32_t knownSlot() const { return immediates[1] & 0x3f; }
    bool receiverIsNeverNullish() const { return immediates[1] & 0x100; }
    uint32_t initialStoreIndex() const { return immediates[1]; }
    uint32_t numberOfArgumentsToSkip() const { return immediates[0]; }
    bool readsArgumentsObject() const { return immediates[1] == 1; }
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

using BlockValues = Vector<ValueID, 4>;

struct Block {
    CompactList<ValueID> parameters;
    BlockValues values;
    Terminator terminator;
};

static constexpr uint32_t FunctionIsGenericEntrypoint = 1 << 16;
static constexpr uint32_t closureIsUnmodified = 1;
static constexpr uint32_t closureMayBeModified = 2;
static constexpr uint32_t closureIsConstructingCallee = 3;
static constexpr uint32_t FunctionUsesArgumentBuffer = 1 << 17;
static constexpr uint32_t FunctionHasUnknownCallers = 1 << 18;
static constexpr uint32_t FunctionIsPartOfHiddenCoroutine = 1 << 19;
static constexpr uint32_t FunctionLogsTypeMismatches = 1 << 20;
static constexpr uint32_t FunctionIsEnteredByValue = 1 << 21;
static constexpr uint32_t FunctionIsPartOfAsyncFunction = 1 << 22;

struct Allocation {
    LayoutID layout;
    FunctionID constructedBy;
    Vector<uint16_t> order;
    uint16_t initialFieldCount { 0 };
    uint16_t discriminant { UINT16_MAX };
    uint16_t numberOfCaptures { 0 };

    uint32_t size { 0 };
    uint32_t numberOfLiterals { 0 };
    int32_t firstLiteral { 0 };
    uint32_t firstLiteralAllocation { 0 };

    friend bool operator==(const Allocation&, const Allocation&) = default;
};

static constexpr unsigned maxInlineFrames = 254;
static_assert(maxInlineFrames < (1u << 8), "Value::inlineFrame has eight bits");
struct InlineFrame {
    FunctionID callee;
    uint32_t callPosition { 0 };
    uint32_t parent { 0 };
    uint8_t licence { 0 };
    bool isTailCall { false };
    bool isConstruction { false };
};

static constexpr ASCIILiteral inlineReasonLetters = "-pemscathfkobwr"_s;

constexpr uint8_t inlineReasonForLetter(char letter)
{
    uint8_t index = 0;
    while (inlineReasonLetters[index] != letter)
        ++index;
    return index;
}

enum class AllocationKind : uint8_t { Object, Environment, CoroutineFrame, Array, Closure, Rest, Arguments };

struct BuiltinPath {
    bool isGlobalObject { false };
    bool isOnPrototype { false };
    StringID global;
    StringID member;

    explicit operator bool() const { return isGlobalObject || global; }
    friend bool operator==(const BuiltinPath&, const BuiltinPath&) = default;
};

struct PredictedBuiltins {
    BuiltinPath first;
    BuiltinPath second;
    bool hasTooMany { false };

    explicit operator bool() const { return !!first; }
    friend bool operator==(const PredictedBuiltins&, const PredictedBuiltins&) = default;
    void add(const BuiltinPath& path)
    {
        if (!path || hasTooMany || path == first || path == second)
            return;
        if (!first)
            first = path;
        else if (!second)
            second = path;
        else
            *this = { { }, { }, true };
    }
    void add(const PredictedBuiltins& other)
    {
        if (other.hasTooMany)
            *this = { { }, { }, true };
        add(other.first);
        add(other.second);
    }
};

struct Function {
    StringID name;
    ModuleID module;
    uint32_t position { 0 };
    uint32_t sourceStart { 0 };
    uint32_t sourceEnd { 0 };
    FunctionKind kind { FunctionKind::Plain };
    uint32_t flags { 0 };
    uint32_t length { 0 };
    Vector<TypeID> captureTypes;
    bool hasManyClosures { false };
    bool closuresHaveOwnProperties { false };
    bool takesCalleeClosure { false };
    FunctionID rampForClosures;
    FunctionID declaredGenericEntrypoint;
    FunctionID untypedClone;
    FunctionID selfCheckingCopy;
    bool isRampForClosures { false };
    bool isUsedAsObject { false };
    bool isSharedOutlinedBody { false };
    StringID sourceOfSeparatorPassed;
    bool isSharedCallBody { false };
    bool testsPinnedFunction { false };
    bool closuresCaptureOnlyTheirHolder { false };
    bool isGuardedPreludeEntry { false };
    bool onlyDestructuresParameter { false };
    bool copiesFirstArgumentToHeap { false };
    uint8_t copiedArgumentCaptureCount { 0 };
    Vector<FunctionID> pendingGuardedPreludeEntries;
    bool cleanUpMovedStores { false };
    bool returnsBoxedValue { false };
    bool takesCapturesFromClosure { false };
    bool isValueEntry { false };
    bool isValueEntryClone { false };
    FunctionID valueEntryTarget;
    FunctionID valueEntryAdapter;
    FunctionID valueEntry;
    uint32_t adapterResult { Format::none };
    uint64_t compatibleResultsMask { 0 };
    TypeID resultTypeAtAdapterSelection;
    TypeID resultTypeCheckedByCallers;
    bool resultIsCoercedByCallers { false };
    Vector<Value> callerResultCasts;
    bool isCalledDirectly { false };
    bool isCalledFromDroppableFrame { false };
    bool hasTailCallSites { false };
    bool isInIndirectTailCallCycle { false };
    uint16_t reservedStackArgumentCount { 0 };
    bool returnsIntegralNumbers { false };
    bool readsHostObjectMember { false };
    StringID cachedHostObjectName;
    FunctionID genericHostObjectReader;
    bool readsAsyncLocalStorage { false };
    FunctionID genericAsyncLocalStorageReader;
    LayoutID constructedClass;
    uint8_t declaredAs { 0 };
    FunctionID resumeFunction;
    FunctionID startFunction;
    uint32_t generatorTemplate { UINT32_MAX };
    bool savesFrameOnSuspend { false };
    bool mayRunWithoutPromise { false };
    bool capturesExceptions { false };
    Vector<uint32_t> suspensionPositions;
    uint32_t thisAllocation { Format::none };
    bool allocatesForUnknownCallers { false };
    TypeID thisType;
    TypeID resultType;
    Vector<TypeID, 4> resultTypes;
    FunctionID worker;
    uint8_t nullishResultKind { 0 };
    Vector<TypeID> parameterTypes;
    Vector<PredictedBuiltins> predictedBuiltinsOfEntryParameters;
    BitVector annotatedParameters;
    BitVector unusedParameters;
    bool didRemoveAllocation { false };
    BitVector calledOnlyParameters;
    BitVector calledOnlyCaptures;
    BitVector stackAllocatedEnvironments;
    BitVector mayPointIntoFrame;
    TypeID assertedThisType;
    Vector<TypeID> assertedParameterTypes;
    Vector<Allocation> allocations;
    Writes writes { Writes::Heap };
    bool writesShapeWords { true };
    Writes writesOnReturn { Writes::Heap };
    bool writesShapeWordsOnReturn { true };
    bool convertsArrayStorage { true };
    bool convertsArrayStorageOnReturn { true };
    uint16_t numberOfInitialStores { 0 };
    uint32_t orderWordAfterInitialStores { 0 };
    Vector<Block, 0, CrashOnOverflow, 2> blocks;
    Vector<Value, 0, CrashOnOverflow, 4> values;
    Vector<InlineFrame> inlineFrames;

    bool has(uint32_t flag) const { return flags & flag; }
    bool isGenericEntrypoint() const { return has(FunctionIsGenericEntrypoint); }
    bool isTyped() const { return has(Format::FunctionIsTyped); }
    CastReason parameterCastReason(unsigned index) const { return annotatedParameters.get(index) ? CastReason::As : CastReason::Store; }
    TypeID assertedParameterType(unsigned index) const { return index < assertedParameterTypes.size() ? assertedParameterTypes[index] : TypeID(); }
    uint32_t allocationOf(Allocation&&);
    Value& at(ValueID id) { ASSERT(id.index() < values.size()); return values.mutableSpan().data()[id.index()]; }
    const Value& at(ValueID id) const { ASSERT(id.index() < values.size()); return values.span().data()[id.index()]; }
    Block& at(BlockID id) { ASSERT(id.index() < blocks.size()); return blocks.mutableSpan().data()[id.index()]; }
    const Block& at(BlockID id) const { ASSERT(id.index() < blocks.size()); return blocks.span().data()[id.index()]; }
};

static constexpr uint32_t fieldIsMethod = 64;
static constexpr uint32_t fieldIsNeverReadUninitialized = 128;
static constexpr uint32_t fieldIsStoredOnce = 256;
static constexpr uint32_t fieldIsNeverStored = 512;
static constexpr uint32_t fieldIsPredictedInt32 = 1024;
static constexpr uint32_t fieldIsSetAtCreation = 2048;
static constexpr uint32_t fieldMayBeSetAtCreation = 4096;
static constexpr uint32_t fieldHoldsCachedClosure = 16384;
static constexpr uint32_t fieldDisallowsUndefined = 65536;
static constexpr uint32_t fieldHasWideReference = 524288;
static_assert((fieldIsOptional + fieldIsOutOfLine + fieldIsImmutable + fieldAllowsUndefined + fieldAllowsNull + fieldIsAnnotated + fieldIsMethod + fieldIsNeverReadUninitialized
    + fieldIsStoredOnce + fieldIsNeverStored + fieldIsPredictedInt32 + fieldIsSetAtCreation + fieldMayBeSetAtCreation + fieldIsAbsentAtCreation + fieldHoldsCachedClosure + fieldIsNeverObservedAbsent + fieldDisallowsUndefined
    + fieldIsPinned + fieldIsNeverObservedAbsentAsDeclared + fieldMayExistWithoutStorage + fieldIsPlaceholder + fieldMayBeUnlisted + fieldHoldsLazyClosure)
    == (fieldIsOptional | fieldIsOutOfLine | fieldIsImmutable | fieldAllowsUndefined | fieldAllowsNull | fieldIsAnnotated | fieldIsMethod | fieldIsNeverReadUninitialized
    | fieldIsStoredOnce | fieldIsNeverStored | fieldIsPredictedInt32 | fieldIsSetAtCreation | fieldMayBeSetAtCreation | fieldIsAbsentAtCreation | fieldHoldsCachedClosure | fieldIsNeverObservedAbsent | fieldDisallowsUndefined
    | fieldIsPinned | fieldIsNeverObservedAbsentAsDeclared | fieldMayExistWithoutStorage | fieldIsPlaceholder | fieldMayBeUnlisted | fieldHoldsLazyClosure));
static_assert(fieldIsNeverObservedAbsentAsDeclared < 1u << pinnedFieldGuardShift);
static_assert(fieldMayExistWithoutStorage < 1u << pinnedFieldGuardShift);
static_assert(fieldIsPlaceholder < 1u << pinnedFieldGuardShift);
static_assert(fieldMayBeUnlisted < 1u << pinnedFieldGuardShift);
static_assert(fieldHoldsLazyClosure < 1u << pinnedFieldGuardShift);

struct ShapeTest {
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint32_t set { UINT32_MAX };
    uint8_t blockByte { 0 };
    bool passesEarlierBlocks { false };

    bool isRange() const { return set == UINT32_MAX; }
    bool neverPasses() const { return !count; }
    uint32_t last() const { return first + count - 1; }
};

struct Literal {
    StringID string;
    double number { 0 };
};

struct Field {
    StringID name;
    TypeID type;
    uint32_t flags { 0 };
    Vector<Literal> literals;
    StringID typeSource;
    SlotID slot;
    TypeID stored;
    TypeID slotType;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
    bool isOutOfLine { false };
    bool isUndeclared { false };
    bool isCreatedWithLazyClosure { false };
    bool isMissingInSomeSubtype { false };
    bool isAnyObjectInSomeSubtype { false };
    bool hasDifferentTypeInSomeSupertype { false };
    bool mayBeCoercedInSomeSubtype { false };
    ShapeTest subtypesWithField;
    bool hasOtherOffsetInSomeSubtype { false };
    bool isPinned { false };
    bool isPinnedInEverySubtype { false };
    bool isPinnedToFunction { false };
    bool holdsIntegralNumbers { false };
    FunctionID singleFunction;
    bool singleFunctionIsProven { false };
    bool isSingleFunctionOrNullish { false };
    FunctionID otherFunction;
    FunctionID lazyClosure;
    PredictedBuiltins predictedBuiltins;
    StringID placeholderFor;
    bool hasSlotInEverySubtype { false };
};

struct Method {
    StringID name;
    FunctionID function;
    MethodKind kind { MethodKind::Method };
    uint32_t key { Format::none };
    bool mayBeReplaced { false };
};

struct StaticField {
    StringID name;
    TypeID type;
    uint32_t place { Format::none };
};

struct Layout {
    LayoutKind kind { LayoutKind::Record };
    StringID name;
    LayoutID base;
    Vector<Field> fields;
    FunctionID constructor;
    FunctionID declaredSuperclassConstructor;
    bool extendsGlobalClass { false };
    Vector<Method> methods;
    Vector<Method> staticMethods;
    Vector<StaticField> staticFields;
    uint32_t classObjectSlot { Format::none };
    LayoutID environment;
    bool hasFixedLayout { false };
    bool isEvaluatedOnce { false };
    bool hasUntypedBase { false };
    bool hasImmutableMembers { false };
    bool mayHaveOtherPrototype { false };
    bool hasNoOwnInstances { false };
    bool hasInstancesWithOtherPrototype { false };
    bool prototypeIsSetByTypedCode { false };
    bool constructorMayBeReplaced { false };
    bool prototypeMayGainMembers { false };
    bool prototypeChainMayGainMembers { false };
    bool mayBeConstructed { false };
    bool hasComputedMethodName { false };
    bool hasMethodWithSymbolKey { false };
    bool hasComputedStaticMemberName { false };
    bool instancesMayBecomeRecords { false };
    bool isAllocated { false };
    bool mayBeFrozen { false };
    bool isImmutableAfterAllocation { false };
    bool isDecodedRow { false };
    bool mayDespecialize { false };
    bool acceptsUntypedObjects { false };
    bool isRelaxedForLogging { false };
    bool acceptsOnlyOtherLayouts { false };
    bool isTypedOnlySupertype { false };
    bool hasFieldPinnedToFunction { false };
    bool checksUntypedObjects() const { return isRelaxedForLogging || acceptsOnlyOtherLayouts; }
    bool acceptsUncheckedObjects() const { return acceptsUntypedObjects && (!acceptsOnlyOtherLayouts || isRelaxedForLogging); }
    TypeID type;
    bool isCoroutineFrame { false };
    bool isStackAllocated { false };
    VariableID reuseVariable;
    bool mayBeStackAllocated { false };
    Vector<LayoutID> subtypes;
    Vector<LayoutID> structuralSubtypes;
    Vector<uint32_t> subtypeShapes;
    Vector<LayoutID> specializations;
    uint32_t cloneAllocation { Format::none };

    uint32_t size { 0 };
    uint32_t sizeWithSpecializationCapacity { 0 };
    uint16_t discriminant { UINT16_MAX };
    bool hasExtension { false };
    uint32_t extensionSize { 0 };
    uint32_t offsetOfExtraProperties { 0 };
    bool isAbstract { false };
    StringID declaredPath;
    uint32_t declaredLine { 0 };
    uint32_t shape { 0 };
    ShapeTest test;
    ShapeTest instances;
    ShapeTest retagSources;
    ShapeTest castTest;
    Vector<Vector<uint16_t>> orders;
    Vector<uint32_t> fieldsByName;

    void indexFields();
    std::optional<uint32_t> fieldNamed(StringID) const;
};

struct Slot {
    TypeID type;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
    bool isOutOfLine { false };
    bool isDiscriminant { false };
    bool hasPinnedFields { false };
};

struct Variable {
    StringID name;
    ModuleID module;
    TypeID type;
    bool startsUndefined { false };
    bool isNeverReadUninitialized { false };
    bool isStoredOnce { false };
    bool hasCard { false };
    bool startsZero { false };
    bool storesUndefinedAsZero { false };
    bool mayBeReadUninitialized() const { return !startsUndefined && !isNeverReadUninitialized && !startsZero; }
    FunctionID singleFunction;
    bool singleFunctionIsProven { false };
    bool isSingleFunctionOrNullish { false };
    Vector<FunctionID, 2> functionsCapturedBySingleFunction;
    PredictedBuiltins predictedBuiltins;
    BuiltinID predictedStaticOfReplaceableGlobal;
    Store store { Store::JSValue };
    uint32_t offset { 0 };
};

struct Module {
    StringID path;
    FunctionID initializer;
    StringID displayPath;
    bool isBuiltin { false };
    uint32_t lineTableSize { 0 };
    std::span<const uint8_t> printed;
    Vector<uint32_t> lineStarts;
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
    constexpr OpcodeSet operator|(const OpcodeSet& other) const
    {
        OpcodeSet result;
        for (size_t i = 0; i < m_words.size(); ++i)
            result.m_words[i] = m_words[i] | other.m_words[i];
        return result;
    }
    constexpr bool intersects(const OpcodeSet& other) const { return (m_words[0] & other.m_words[0]) | (m_words[1] & other.m_words[1]) | (m_words[2] & other.m_words[2]) | (m_words[3] & other.m_words[3]); }

private:
    std::array<uint64_t, 4> m_words { };
};

class Functions {
    struct Contents {
        OpcodeSet opcodes;
        uint16_t terminatorKinds { 0 };
        bool containsPredictedClosure { false };
    };

public:
    class ValueIndexRows {
    public:
        void append(size_t index, const Function&);

    private:
        friend class Functions;
        struct Entry {
            uint32_t function;
            uint32_t first;
            uint32_t valueCount;
            uint32_t indexedValueCount;
            Contents contents;
        };
        Vector<Entry> m_entries;
        Vector<ValueID> m_ids;
        Vector<Opcode> m_opcodes;
        Vector<Opcode> m_valueOpcodes;
    };

    size_t size() const { return m_functions.size(); }
    bool isEmpty() const { return m_functions.isEmpty(); }

    const Function& operator[](size_t index) const LIFETIME_BOUND { return m_functions[index]; }
    Function& operator[](size_t index) LIFETIME_BOUND
    {
        forget(index);
        return m_functions[index];
    }
    Function& atKeepingValueIndex(size_t index) LIFETIME_BOUND { return m_functions[index]; }

    const Function* begin() const LIFETIME_BOUND { return m_functions.begin(); }
    const Function* end() const LIFETIME_BOUND { return m_functions.end(); }
    Function* begin() LIFETIME_BOUND
    {
        invalidateValueIndex();
        return m_functions.begin();
    }
    Function* end() LIFETIME_BOUND { return m_functions.end(); }
    std::span<const Function> span() const LIFETIME_BOUND { return m_functions.span(); }

    void append(Function&& function) { m_functions.append(WTF::move(function)); }
    void appendVector(Vector<Function>&& functions) { m_functions.appendVector(WTF::move(functions)); }
    void grow(size_t size) { m_functions.grow(size); }
    void reserveInitialCapacity(size_t size) { m_functions.reserveInitialCapacity(size); }
    auto map(NOESCAPE const auto& functor) const { return m_functions.map(functor); }

    void rebuildInvalidatedValueIndex();
    void adoptValueIndexRows(std::span<const ValueIndexRows>);
    void verifyValueIndex() const;
    void invalidateValueIndex() { m_isIndexed.clear(); }
    void invalidateValueIndex(size_t index) { forget(index); }
    void releaseValueIndex();
    bool isIndexed(size_t index) const { return index < m_isIndexed.size() && WTF::atomicLoad(&m_isIndexed[index], std::memory_order_relaxed); }

    bool mayContain(size_t index, const OpcodeSet& requestedOpcodes) const { return !isIndexed(index) || m_contents[index].opcodes.intersects(requestedOpcodes); }
    bool mayEndWith(size_t index, TerminatorKind kind) const { return !isIndexed(index) || m_contents[index].terminatorKinds >> static_cast<unsigned>(kind) & 1; }
    bool mayContainPredictedClosure(size_t index) const { return !isIndexed(index) || m_contents[index].containsPredictedClosure; }

    void forEachIndexedValue(size_t index, const OpcodeSet& requestedOpcodes, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
    {
        if (isIndexed(index)) {
            if (!m_contents[index].opcodes.intersects(requestedOpcodes))
                return;
            auto opcodes = indexedOpcodes(index);
            auto ids = m_indexedValues.items<&IndexedValue::id>().subspan(m_firstIndexedValue[index], opcodes.size());
            for (size_t k = 0; k < opcodes.size(); ++k) {
                if (requestedOpcodes.contains(opcodes[k]))
                    functor(ids[k]);
            }
            return;
        }
        auto& function = m_functions[index];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                if (requestedOpcodes.contains(function.at(id).opcode))
                    functor(id);
            }
        }
    }

    void forEachIndexedValueWithBlock(size_t index, const OpcodeSet& requestedOpcodes, NOESCAPE const Invocable<void(unsigned, unsigned, ValueID)> auto& functor) const
    {
        auto& function = m_functions[index];
        if (isIndexed(index)) {
            if (!m_contents[index].opcodes.intersects(requestedOpcodes))
                return;
            auto opcodes = indexedOpcodes(index);
            auto ids = m_indexedValues.items<&IndexedValue::id>().subspan(m_firstIndexedValue[index], opcodes.size());
            unsigned block = 0;
            size_t blockStart = 0;
            for (size_t k = 0; k < opcodes.size(); ++k) {
                if (!requestedOpcodes.contains(opcodes[k]))
                    continue;
                while (k - blockStart >= function.blocks[block].values.size())
                    blockStart += function.blocks[block++].values.size();
                functor(block, static_cast<unsigned>(k - blockStart), ids[k]);
            }
            return;
        }
        for (unsigned block = 0; block < function.blocks.size(); ++block) {
            auto& values = function.blocks[block].values;
            for (unsigned k = 0; k < values.size(); ++k) {
                if (requestedOpcodes.contains(function.at(values[k]).opcode))
                    functor(block, k, values[k]);
            }
        }
    }

    void forEachValue(size_t index, const OpcodeSet& requestedOpcodes, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
    {
        if (isIndexed(index)) {
            if (!m_contents[index].opcodes.intersects(requestedOpcodes))
                return;
            auto opcodes = m_valueOpcodes.span().subspan(m_firstIndexedValue[index], m_valueCount[index]);
            for (size_t k = 0; k < opcodes.size(); ++k) {
                if (requestedOpcodes.contains(opcodes[k]))
                    functor(ValueID(k));
            }
            return;
        }
        auto& values = m_functions[index].values;
        for (size_t k = 0; k < values.size(); ++k) {
            if (requestedOpcodes.contains(values[k].opcode))
                functor(ValueID(k));
        }
    }

private:
    struct IndexedValue {
        ValueID id;
        Opcode opcode { Opcode::Undefined };
    };

    std::span<const Opcode> indexedOpcodes(size_t index) const
    {
        return m_indexedValues.items<&IndexedValue::opcode>().subspan(m_firstIndexedValue[index], m_indexedValueCount[index]);
    }

    void forget(size_t index)
    {
        if (!isIndexed(index))
            return;
        WTF::atomicStore(&m_isIndexed[index], static_cast<uint8_t>(0), std::memory_order_relaxed);
        if (!WTF::atomicLoad(&m_hasInvalidatedFunctions, std::memory_order_relaxed))
            WTF::atomicStore(&m_hasInvalidatedFunctions, static_cast<uint8_t>(1), std::memory_order_relaxed);
    }

    Vector<Function> m_functions;
    MultiArrayList<IndexedValue, &IndexedValue::id, &IndexedValue::opcode> m_indexedValues;

    static bool fillValueIndexRows(const Function&, std::span<ValueID> ids, std::span<Opcode> opcodes, std::span<Opcode> valueOpcodes, Contents&, uint32_t& indexedValueCount);
    void indexValues(size_t index);

    Vector<Opcode> m_valueOpcodes;
    Vector<uint32_t> m_firstIndexedValue;
    Vector<uint32_t> m_indexedValueCount;
    Vector<uint32_t> m_valueCount;
    Vector<Contents> m_contents;
    mutable Vector<uint8_t> m_isIndexed;
    uint8_t m_hasInvalidatedFunctions { 0 };
};

struct CallSignature {
    Vector<TypeID, 4> parameters;
    TypeID result;
    unsigned minArgumentCount { 0 };
    bool hasRestParameter { false };

    friend bool operator==(const CallSignature&, const CallSignature&) = default;
};

class Program final {
    WTF_MAKE_TZONE_ALLOCATED(Program);
    WTF_MAKE_NONCOPYABLE(Program);
public:
    Program() = default;

    static std::unique_ptr<Program> tryRead(std::span<const uint8_t>, String& failureReason);

    std::span<const uint8_t> string(StringID id) const { return strings[id.index()]; }
    BuiltinName builtinName(StringID id) const { return m_builtinNames[id.index()]; }
    BuiltinID operationOf(const Value& value) const { return value.opcode == Opcode::CallBuiltin || value.opcode == Opcode::Operation ? value.builtin() : BuiltinID(); }
    bool usesOperation(BuiltinID operation) const { return m_usedOperations[operation.index() / 32] >> operation.index() % 32 & 1; }
    StringID stringOf(BuiltinName name) const { return name.index() < m_builtinNameStrings.size() ? m_builtinNameStrings[name.index()] : StringID(); }
    Function& at(FunctionID id) { return functions[id.index()]; }
    const Function& at(FunctionID id) const { return functions[id.index()]; }
    Layout& at(LayoutID id) { return layouts[id.index()]; }
    const Layout& at(LayoutID id) const { return layouts[id.index()]; }
    Slot& at(SlotID id) { return slots[id.index()]; }
    const Slot& at(SlotID id) const { return slots[id.index()]; }
    const Type& at(TypeID id) const { return types.at(id); }
    const Type& typeOf(const Function& function, ValueID value) const { return types.at(function.at(value).type); }
    bool isHiddenInStackTraces(const Function& function) const { return function.isGenericEntrypoint() || function.has(Format::FunctionIsHiddenInStackTraces | FunctionIsPartOfHiddenCoroutine) || function.resumeFunction || function.startFunction || function.isRampForClosures || entries.containsIf([&](FunctionID entry) { return &function == &at(entry); }) || (function.has(Format::FunctionIsBuiltin) && !isShownAsBuiltinInStackTraces(function)) || isPrivateBuiltinFunction(function); }
    size_t preludeFunctionIndex(const Function& function) const
    {
        if (!function.has(Format::FunctionIsBuiltin))
            return notFound;
        return preludeFunctions.findIf([&](FunctionID id) {
            return id && &function == &at(id);
        });
    }
    bool implementsBuiltin(const Function& function) const { return preludeFunctionIndex(function) != notFound; }
    bool isShownAsBuiltinInStackTraces(const Function& function) const
    {
        size_t which = preludeFunctionIndex(function);
        return which != notFound && !isHiddenFromStackTraces(static_cast<PreludeFunction>(which));
    }
    BuiltinID builtinImplementedBy(const Function& function) const
    {
        size_t which = preludeFunctionIndex(function);
        return which == notFound ? BuiltinID() : Builtins::singleton().entryImplementedBy(static_cast<PreludeFunction>(which));
    }
    bool isResumeFunction(const Function& function) const
    {
        LayoutID captured = function.captureTypes.size() == 1 ? at(function.captureTypes[0]).singleLayout() : LayoutID();
        return captured && at(captured).isCoroutineFrame;
    }
    bool isUpcast(LayoutID, LayoutID) const;
    ValueID constantOf(const Function&, ValueID) const;
    ValueID underlyingValue(const Function&, ValueID) const;
    PredictedBuiltins predictedBuiltins(const Function&, ValueID) const;
    BuiltinID predictedStaticCalledBy(const Function&, const Value& call) const;
    bool castIsRedundant(const Type& from, const Type& to, CastReason reason) const
    {
        return from.isSubsetOf(to, *this, !isRelaxedForLogging(to) && (reason != CastReason::As || !annotationCoercesUntypedObjects(to)));
    }
    bool castIsRedundant(const Type& from, const Type& to, const Value& cast) const
    {
        if (cast.castReason() == CastReason::As && cast.coercesFreshData())
            return from.isSubsetOf(to, *this, false);
        return castIsRedundant(from, to, cast.castReason());
    }
    bool isRelaxedForLogging(const Type& to) const
    {
        bool isOpened = false;
        for (LayoutID supertype : to.objectLayouts) {
            if (!at(supertype).acceptsUntypedObjects)
                continue;
            if (!at(supertype).isRelaxedForLogging)
                return false;
            isOpened = true;
        }
        return isOpened;
    }
    bool annotationCoercesUntypedObjects(const Type& to) const
    {
        for (LayoutID supertype : to.objectLayouts) {
            if (!layoutsWithTrivialCoercion.get(supertype.index()))
                return true;
        }
        return to.objectLayouts.isEmpty();
    }
    bool passes(const ShapeTest& test, uint32_t shape) const
    {
        uint32_t i = test.last() - shape;
        return i < test.count && (test.isRange() || shapeSetBits.get(test.set + i));
    }
    bool isExtended(BuiltinName global) const { return extendedGlobals.get(global.index()); }
    LineColumn lineColumnFor(ModuleID, uint32_t position) const;
    BitVector extendedGlobals;
    BitVector prototypePropertyNames;
    BitVector hostAccessorNames;
    bool hasPrototypeThatMayGainMembers { false };
    BitVector methodFieldNames;
    BitVector lazyClosureNames;
    BitVector possibleAccessorNames;
    void findPossibleAccessorNames();
    Vector<std::pair<StringID, StringID>> regExpLiterals;
    bool logsTypeErrors { false };
    Vector<uint32_t> sites;
    Vector<std::pair<uint32_t, FunctionID>> builtinModules;
    struct BuiltinFunction {
        uint32_t id;
        FunctionID function;
        uint32_t visibility;
    };
    Vector<BuiltinFunction> builtinFunctions;
    BitVector builtinFunctionModules;
    bool isInBuiltinFunctionModule(const Function& function) const { return function.module && builtinFunctionModules.get(function.module.index()); }
    const BuiltinFunction* enclosingBuiltinFunction(const Function& function) const
    {
        if (!isInBuiltinFunctionModule(function))
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
        if (!isInBuiltinFunctionModule(function))
            return false;
        auto* around = enclosingBuiltinFunction(function);
        if (!around)
            return true;
        auto& builtin = at(around->function);
        return builtin.position == function.position && (around->visibility || builtin.declaredAs & functionIsAsync || visibilityInEngine(around->id));
    }
    const Type& declaredArgumentType(const Function& callee, unsigned index) const
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
    bool usesBuiltinTailCalls(const Function& function) const
    {
        if (function.isGenericEntrypoint() || function.resumeFunction || function.startFunction || function.declaredAs & (functionIsAsync | functionIsGenerator) || isResumeFunction(function))
            return false;
        if (implementsBuiltin(function))
            return true;
        auto* around = enclosingBuiltinFunction(function);
        return around && isStrictInEngine(around->id);
    }
    struct FunctionByKey {
        StringID property;
        StringID key;
        FunctionID function;
    };
    Vector<FunctionByKey> functionsByKey;
    FunctionID functionByKey(StringID property, StringID key) const;

    StringID intern(std::span<const uint8_t>);
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
    uint32_t numberOfGeneratorTemplates { 0 };
    Vector<Module> modules;
    Functions functions;
    Vector<unsigned> sizesForInlining;
    Vector<uint32_t> firstSupertypeOfLayout;
    Vector<LayoutID> supertypesOfLayouts;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsStoredThroughLayouts;
    struct InliningFacts {
        bool isAccessor { false };
        bool returnsMethodTable { false };
        BitVector parametersCalled;
        BitVector parametersCalledInLoops;
    };
    Vector<InliningFacts> inliningFacts;
    static constexpr uint32_t hasNoClosureCreator = UINT32_MAX;
    static constexpr uint32_t hasMultipleClosureCreators = UINT32_MAX - 1;
    Vector<uint32_t> singleClosureCreators;
    Vector<BitVector> noReturnBlocks;
    BitVector noReturnFunctions;
    bool neverReturns(FunctionID id) const { return noReturnFunctions.get(id.index()); }
    Vector<FunctionID, 1> entries;
    bool isEntry(FunctionID id) const { return entries.contains(id); }
    Vector<uint32_t, 1> entryModules;

    Vector<uint32_t> identifierNumbers;
    enum class AccessByName : uint8_t { Read, Store, Definition, Call, Other };
    struct NameUses {
        StringID name;
        uint32_t uses { 0 };
        uint32_t tableLookupUses { 0 };
        uint32_t firstTableLookupFunction { UINT32_MAX };
        std::array<uint32_t, 4> usesByAccess { };
    };
    Vector<NameUses> genericAccessNameUses() const;
    Vector<uint32_t> mostUsedStringKeys(unsigned limit) const;
    void collectNamesAndStringsFromValues();

    TypeTable types;
    BitVector typesNeededAtRuntime;
    bool isNeededAtRuntime(TypeID type) const { return typesNeededAtRuntime.get(type.index()); }
    LayoutID closureLayout;
    Vector<uint32_t> closureAllocations;
    uint32_t closureAllocation(unsigned numberOfCaptures) const { return closureAllocations[numberOfCaptures / 2]; }
    std::pair<unsigned, unsigned> closureReferenceAndCaptureCounts(const Function&) const;
    bool usesSlimClosures(const Function& function) const { return mayUseSlimClosures(function) && function.captureTypes.size() <= ClosureRequest::maxSlimClosureCaptures; }
    bool mayUseSlimClosures(const Function&) const;
    Vector<FunctionID> preludeFunctions;
    static constexpr unsigned maxGuardedPreludeEntryArguments = 3;
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> guardedPreludeEntries;
    static constexpr uint32_t guardedPreludeEntryKey(FunctionID prelude, unsigned numberOfArguments, std::optional<unsigned> copiedArgumentCaptureCount, bool checksResultType) { return ((prelude.index() * (maxGuardedPreludeEntryArguments + 1) + numberOfArguments) * (ClosureRequest::maxSlimClosureCaptures + 2) + (copiedArgumentCaptureCount ? *copiedArgumentCaptureCount + 1 : 0)) * 2 + checksResultType; }
    Vector<TypeID> engineClassTypes;
    Vector<TypeID> builtinIntegerResultTypes;
    Vector<Allocation> allocations;
    bool castsMayRetag { false };
    bool mayRunInMultipleInstances { false };
    Vector<LayoutID> typedOnlySupertypes;
    BitVector supertypesWithStableSubtypes;
    BitVector retagTargets;
    Vector<FunctionID> constructorFunctionsWithTypedThis;
    BitVector instantiationTargets;
    BitVector layoutsWithTrivialCoercion;
    Vector<Slot> slots;
    uint32_t numberOfShapes { 0 };
    Vector<uint32_t> shapeCountPrefixSums;
    uint32_t shapeCountInRange(uint32_t first, uint32_t last) const { return first > last ? 0 : shapeCountPrefixSums[last] - shapeCountPrefixSums[first - 1]; }
    BitVector shapeSetBits;
    struct MultiLayoutShapeTest {
        Vector<LayoutID, 1> objectLayouts;
        ShapeTest test;
        uint32_t numberOfShapes { 0 };
    };
    enum class ShapeTestKind : uint8_t { Members, SubtypesKeptByCast, RetagSources };
    UncheckedKeyHashMap<uint64_t, MultiLayoutShapeTest> multiLayoutShapeTests;
    static uint64_t multiLayoutShapeTestKey(const Vector<LayoutID, 1>& objectLayouts, ShapeTestKind kind)
    {
        uint64_t key = 0x9e3779b97f4a7c15ull + static_cast<uint64_t>(kind);
        for (LayoutID supertype : objectLayouts)
            key = (key ^ supertype.index()) * 0x100000001b3ull;
        return key | 1;
    }
    const ShapeTest* findMultiLayoutShapeTest(const Vector<LayoutID, 1>& objectLayouts, ShapeTestKind kind) const
    {
        auto found = multiLayoutShapeTests.find(multiLayoutShapeTestKey(objectLayouts, kind));
        return found != multiLayoutShapeTests.end() && found->value.objectLayouts == objectLayouts ? &found->value.test : nullptr;
    }
    StringID emptyString;
    uint32_t copyOnWriteButterfliesOffset { 0 };
    uint32_t numberOfCopyOnWriteButterflies { 0 };
    Vector<Vector<uint64_t>> arrayLiteralElements;
    struct ConstantTable {
        Vector<StringID> keys;
        Vector<uint64_t> values;
    };
    Vector<ConstantTable> tablesInImage;
    static constexpr unsigned maxInlineArrayLiteralLength = 16;
    uint32_t sharedRegExpObjectsOffset { 0 };
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> sharedRegExpObjectOffsets;
    uint32_t sizeOfStatics { 0 };
    uint32_t placedStaticsSize { 0 };
    uint32_t sizeOfLazilyInitializedStatics { 0 };
    std::array<Vector<uint32_t>, 2> globalPropertyCacheNumbers;
    uint32_t globalPropertyCacheNumber(StringID name, bool isStore) const { return name.index() < globalPropertyCacheNumbers[isStore].size() ? globalPropertyCacheNumbers[isStore][name.index()] : 0; }
    bool isGlobalThisName(StringID name) const
    {
        auto bytes = string(name);
        return equalSpans(bytes, "globalThis"_span8) || equalSpans(bytes, "global"_span8) || equalSpans(bytes, "self"_span8) || equalSpans(bytes, "window"_span8);
    }
    Vector<uint32_t> globalThisPropertyCacheNumbers;
    uint32_t globalThisPropertyCacheNumber(StringID name) const { return name.index() < globalThisPropertyCacheNumbers.size() ? globalThisPropertyCacheNumbers[name.index()] : 0; }
    bool isHostObjectDataProperty(StringID object, StringID name) const
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
    bool isHostObjectFunction(StringID object, StringID name) const
    {
        auto isOneOf = [bytes = string(name)](auto... names) { return (equalSpans(bytes, names) || ...); };
        return equalSpans(string(object), "Buffer"_span8) && isOneOf("byteLength"_span8, "from"_span8, "isBuffer"_span8, "alloc"_span8, "allocUnsafe"_span8, "concat"_span8, "compare"_span8, "isEncoding"_span8);
    }
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> globalMemberCacheNumbers;
    uint32_t globalMemberCacheNumber(StringID global, StringID name) const { return globalMemberCacheNumbers.get(static_cast<uint64_t>(global.index()) << 32 | name.index()); }
    uint32_t numberOfGlobalPropertyCaches { 0 };
    uint32_t globalPropertyCachesOffset { 0 };
    uint32_t numberOfStaticFieldSlots { 0 };
    uint32_t staticFieldSlotsOffset { 0 };
    UncheckedKeyHashMap<uint32_t, FunctionID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> genericEntrypoints;
    uint32_t firstGenericEntrypoint { 0 };
    uint32_t numberOfConsecutiveGenericEntrypoints { 0 };
    Vector<CallSignature> callSignatures;
    struct ValueCallShape {
        Vector<TypeID, 4> parameters;
        TypeID result;
        FunctionID adapter;
    };
    Vector<ValueCallShape> valueCallShapes;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> valueCallShapesByHash;
    FunctionID adapterForCall(const Value&) const;
    FunctionID firstSignatureAdapter;
    uint32_t signatureIndexOfAdapter(FunctionID adapter) const { return adapter.index() - firstSignatureAdapter.index(); }
    UncheckedKeyHashMap<uint32_t, TypeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOrUndefinedTypes;
    static constexpr unsigned maxBoundArguments = 3;
    UncheckedKeyHashMap<uint64_t, FunctionID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> boundFunctions;
    static constexpr uint64_t boundFunctionKey(FunctionID function, unsigned numberOfBoundArguments) { return static_cast<uint64_t>(function.index()) << 2 | numberOfBoundArguments; }
    FunctionID boundFunction(FunctionID function, unsigned numberOfBoundArguments) const { return boundFunctions.get(boundFunctionKey(function, numberOfBoundArguments)); }
    BitVector staticallyHotFunctions;
    struct LayoutAndNameBoolCache {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> answers;
    };
    mutable std::array<LayoutAndNameBoolCache, 64> prototypeChainLacksPropertyCache;
    mutable std::array<LayoutAndNameBoolCache, 64> storeMayTransitionCache;
    struct LayoutAndNameTypeCache {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, TypeID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> types;
    };
    mutable std::array<LayoutAndNameTypeCache, 64> widestStoredFieldTypeCache;
    mutable std::array<LayoutAndNameTypeCache, 64> widestTypeStoredThroughSupertypesCache;
    bool isStaticallyHot(FunctionID id) const { return staticallyHotFunctions.get(id.index()); }
    BitVector modulesRunPerRequest;
    BitVector functionsWithSiteCaches;
    BitVector staticallyWarmFunctions;
    bool isStaticallyWarm(FunctionID id) const { return staticallyWarmFunctions.get(id.index()); }
    BitVector staticallyRareFunctions;
    bool isStaticallyRare(FunctionID id) const { return staticallyRareFunctions.get(id.index()); }
    BitVector callGraphColdFunctions;
    bool functionLengthsAreBytes { false };
    BitVector functionsWithInliningDisabled;
    uint16_t disabledInlineReasons { 0 };
    bool isInliningDisabled(FunctionID id, uint8_t licence) const { return disabledInlineReasons >> licence & 1 && functionsWithInliningDisabled.get(id.index()); }
    bool isColdInCallGraph(FunctionID id) const { return callGraphColdFunctions.get(id.index()); }
    BitVector functionsHotOnlyInLoops;
    BitVector functionsInCycles;
    bool isInCycle(FunctionID id) const { return functionsInCycles.get(id.index()); }
    bool isHotOnlyInLoops(FunctionID id) const { return functionsHotOnlyInLoops.get(id.index()); }
    bool isHotOutsideLoops(FunctionID id) const { return isStaticallyHot(id) && !isHotOnlyInLoops(id); }
    BitVector functionsHotOnlyAsHottest;
    bool inlinesAsHotOutsideLoops(FunctionID id) const { return isHotOutsideLoops(id) && !functionsHotOnlyAsHottest.get(id.index()); }
    BitVector functionsCalledOnlyFromHotLoops;
    bool isCalledFromHotLoops(FunctionID id) const { return inlinesAsHotOutsideLoops(id) || functionsCalledOnlyFromHotLoops.get(id.index()); }
    bool mayEncounterMasqueradesAsUndefined { false };
    bool propagatesExceptionObjects { false };
    Vector<uint32_t> numberOfDirectCalls;
    BitVector uncalledFunctions;
    BitVector functionsWithCheckedStack;
    BitVector genericEntrypointsWithCalleeStackCheck;
    bool hasCheckedStackOnEntry(FunctionID id) const { return functionsWithCheckedStack.get(id.index()); }
    bool hasCode(FunctionID id) const { return !uncalledFunctions.get(id.index()); }
    BitVector adaptersCalledBySignature;
    UncheckedKeyHashMap<uint64_t, FunctionID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> resultAdapters;
    static uint64_t resultAdapterKey(TypeID result, uint32_t resultCast, size_t numberOfArguments) { return static_cast<uint64_t>(result.index()) << 32 | static_cast<uint64_t>(resultCast & 0xff) << 8 | numberOfArguments; }
    uint64_t resultsUsedByCalls { 0 };
    uint32_t compatibleResultsMaskSize { 0 };
    bool didResolveCallsBySignature { false };
    bool isUsedBySomeCall(FunctionID valueEntry) const
    {
        if (!didResolveCallsBySignature)
            return true;
        return adaptersCalledBySignature.get(at(valueEntry).valueEntryAdapter.index());
    }
    FunctionID compiledValueEntryOf(FunctionID generalEntry) const
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
            uint32_t precedingCalleeCount;
            uint32_t externalModule;
        };
        Vector<Fetch> fetches;
        static constexpr uint32_t functionMarker = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t fetchMarker = functionMarker - 1;
    };
    Vector<ModuleEvaluation> moduleEvaluations;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> moduleEvaluationIndexByFunction;
    FunctionID moduleEvaluator;
    FunctionID moduleEvaluatorIfNeeded;
    FunctionID externalModuleEvaluator;
    FunctionID moduleEvaluatorForExternalCall() const { return externalModuleEvaluator ? externalModuleEvaluator : moduleEvaluator; }
    std::array<FunctionID, numberOfArgumentGPRs + 1> constructBodies;
    FunctionID constructBody(const Value& construct) const { return construct.arguments.size() < constructBodies.size() ? constructBodies[construct.arguments.size()] : FunctionID(); }
    UncheckedKeyHashMap<uint64_t, uint32_t> castBodies;
    Vector<std::pair<LayoutID, FunctionID>> jsonSerializers;
    FunctionID jsonDispatcher;
    Vector<Vector<uint8_t>> formatPatterns;
    Vector<uint8_t> icuStampOfFormatPatterns;
    StringID nameOfFormatMethod;
    FunctionID jsonSerializerFor(LayoutID layout) const
    {
        auto found = std::ranges::lower_bound(jsonSerializers, layout.index(), { }, [](auto& entry) { return entry.first.index(); });
        return found == jsonSerializers.end() || found->first != layout ? FunctionID() : found->second;
    }
    bool mayReadRecordWhilePlain(LayoutID id, bool& hasLiterals) const
    {
        auto& candidate = at(id);
        if (candidate.kind != LayoutKind::Record || !candidate.acceptsUntypedObjects || candidate.isRelaxedForLogging || candidate.subtypes.size() != 1 || candidate.subtypes[0] != id)
            return false;
        unsigned numberOfFields = 0;
        for (auto& field : candidate.fields) {
            if (field.flags & fieldIsNeverStored)
                continue;
            if (!field.type || field.isPinned || field.isPinnedToFunction || field.isOutOfLine || field.flags & (fieldIsPlaceholder | fieldMayExistWithoutStorage | fieldHoldsLazyClosure | fieldIsMethod))
                return false;
            hasLiterals |= !field.literals.isEmpty();
            ++numberOfFields;
        }
        return numberOfFields && numberOfFields <= maxNumberOfPlainRecordFields;
    }
    bool mayHavePlainRecordConverter(LayoutID id) const
    {
        bool hasLiterals = false;
        return id && mayReadRecordWhilePlain(id, hasLiterals) && !hasLiterals;
    }
    Vector<std::pair<LayoutID, FunctionID>> plainRecordConverters;
    FunctionID plainRecordConverterFor(LayoutID layout) const
    {
        auto found = std::ranges::lower_bound(plainRecordConverters, layout.index(), { }, [](auto& entry) { return entry.first.index(); });
        return found == plainRecordConverters.end() || found->first != layout ? FunctionID() : found->second;
    }
    static bool mayHaveBody(const Value& cast) { return cast.opcode == Opcode::Cast && (cast.castReason() == CastReason::As || cast.castReason() == CastReason::Claim) && cast.immediates[0] == Format::none; }
    static uint64_t castBodyKey(const Value& cast) { return (static_cast<uint64_t>(cast.type.index()) + 1) << 32 | cast.immediates[1]; }
    FunctionID castBody(const Value& cast) const
    {
        if (castBodies.isEmpty() || !mayHaveBody(cast))
            return FunctionID();
        auto found = castBodies.find(castBodyKey(cast));
        return found == castBodies.end() ? FunctionID() : FunctionID(found->value);
    }
    BitVector runOnceFunctions;
    bool runsOnce(FunctionID id) const { return runOnceFunctions.get(id.index()); }
    BitVector functionsReturningMapKeys;
    Vector<uint16_t> freshResultClasses;
    BitVector comparedBuiltinCells;
    BitVector stringsCreatedOnce;
    Vector<StringID> literalStrings;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> literalStringIndices;
    std::optional<uint32_t> literalStringIndex(StringID string) const
    {
        auto found = literalStringIndices.find(string.index());
        return found == literalStringIndices.end() ? std::nullopt : std::optional<uint32_t>(found->value);
    }
    struct InitialStructure {
        uint32_t firstSlot { 0 };
        Vector<StringID, 8> names;
        bool isWhole { true };
    };
    Vector<InitialStructure> initialStructures;
    struct StructureDescriptor {
        Vector<StringID, 8> names;
        uint8_t inlineCapacity { 0 };
        bool hasNullPrototype { false };
    };
    Vector<StructureDescriptor> structureDescriptors;
    uint32_t firstThisStructureDescriptor { Format::none };
    Vector<std::array<uint32_t, 3>> structureTransitions;
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 1>> structureDescriptorsByName;
    UncheckedKeyHashMap<uint32_t, Vector<LayoutID, 1>> recordLayoutsByFieldName;
    BitVector structuresWithoutTransitions;
    BitVector structuresMadeByUntypedUserCode;
    UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>> structureDescriptorsByHash;
    uint32_t findStructureDescriptor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype) const;
    uint32_t ensureStructureDescriptor(std::span<const StringID> names, unsigned inlineCapacity, bool hasNullPrototype);
    bool hasAllocationAssertions { false };

private:
    friend class Preparation;
    friend class Reader;

    Vector<Vector<uint8_t>> m_ownStrings;
    static constexpr unsigned numberOfStringIDTables = 16;
    static unsigned stringIDTableForHash(unsigned hash) { return hash >> 20 & (numberOfStringIDTables - 1); }
    std::array<UncheckedKeyHashMap<String, uint32_t>, numberOfStringIDTables> m_stringIDs;
    struct ValueNamesAndStrings {
        Vector<Vector<StringID>> accessorNames;
        Vector<NameUses> nameUses;
        Vector<uint32_t> stringConstantUses;
    };
    std::optional<ValueNamesAndStrings> m_valueNamesAndStrings;
    void recordString(StringID);

    Vector<BuiltinName> m_builtinNames;
    Vector<StringID> m_builtinNameStrings;
    std::array<uint32_t, (numberOfBuiltins + 31) / 32> m_usedOperations { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
