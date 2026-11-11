/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTPrimitives.h"
#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include "AOTVarint.h"
#include "JSCJSValue.h"
#include <bit>
#include <optional>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct StaticsLayout {
    uint32_t distanceFromArrayProfileToLastArray;
};
static_assert(sizeof(StaticsLayout) == 4 && isImageRecord<StaticsLayout>);

struct StaticsRun {
    uint32_t firstWord;
    uint32_t numberOfWords;
    uint32_t lowBits;
    uint32_t highBits;

    uint64_t bits() const { return lowBits | static_cast<uint64_t>(highBits) << 32; }
};
static_assert(sizeof(StaticsRun) == 16 && isImageRecord<StaticsRun>);

struct StaticsRange {
    uint32_t offset;
    uint32_t size;

    uint32_t end() const { return offset + size; }
    bool contains(uint32_t offsetInStatics) const { return offsetInStatics - offset < size; }
};
static_assert(sizeof(StaticsRange) == 8 && isImageRecord<StaticsRange>);

struct FunctionVariableData {
    uint32_t offset;
    uint32_t function;
};
static_assert(sizeof(FunctionVariableData) == 8 && isImageRecord<FunctionVariableData>);

class NamedVariablesWriter {
public:
    static constexpr uint32_t bytesInUnitOfStatics = 4;
    static constexpr SlotRepresentation representationInFrontOfFirst = SlotRepresentation::JSValue;

    void beginModule(uint32_t module, size_t numberOfVariables)
    {
        RELEASE_ASSERT(m_bytes.isEmpty() || module > m_lastModule);
        appendVarint(m_bytes, module - m_lastModule);
        appendVarint(m_bytes, numberOfVariables);
        m_lastModule = module;
        m_lastName = 0;
    }
    void endModule(std::span<const uint32_t> closureNumbers)
    {
        auto endOfRunFrom = [&](size_t first) {
            size_t end = first + 1;
            while (end < closureNumbers.size() && closureNumbers[end] == closureNumbers[end - 1] + 1)
                ++end;
            return end;
        };
        size_t numberOfRuns = 0;
        for (size_t first = 0; first < closureNumbers.size(); first = endOfRunFrom(first))
            ++numberOfRuns;
        appendVarint(m_bytes, numberOfRuns);
        for (size_t first = 0; first < closureNumbers.size();) {
            size_t end = endOfRunFrom(first);
            appendVarint(m_bytes, zigZagEncode(static_cast<int64_t>(closureNumbers[first]) - m_endOfLastRun));
            appendVarint(m_bytes, end - first);
            m_endOfLastRun = closureNumbers[end - 1] + 1;
            first = end;
        }
    }
    void append(const VariableData& variable)
    {
        bool isClosure = variable.slot.representation() == SlotRepresentation::DeferredClosure;
        RELEASE_ASSERT(variable.name >= m_lastName && (isClosure || !(variable.slot.offset % bytesInUnitOfStatics)));
        uint32_t place = isClosure ? variable.slot.offset : variable.slot.offset / bytesInUnitOfStatics;
        appendVarint(m_bytes, variable.name - m_lastName);
        bool changesRepresentation = variable.slot.representation() != m_lastRepresentation;
        appendVarint(m_bytes, zigZagEncode(static_cast<int64_t>(place) - m_lastPlace[isClosure]) << 1 | changesRepresentation);
        if (changesRepresentation)
            appendVarint(m_bytes, variable.slot.representationBits);
        m_lastName = variable.name;
        m_lastPlace[isClosure] = place;
        m_lastRepresentation = variable.slot.representation();
    }
    const Vector<uint8_t>& bytes() const { return m_bytes; }

private:
    Vector<uint8_t> m_bytes;
    uint32_t m_lastModule { 0 };
    uint32_t m_lastName { 0 };
    uint32_t m_lastPlace[2] { };
    uint32_t m_endOfLastRun { 0 };
    SlotRepresentation m_lastRepresentation { representationInFrontOfFirst };
};

bool forEachNamedVariable(std::span<const uint8_t> bytes, const auto& beginModule, const auto& visit, const auto& visitRunOfClosureNumbers)
{
    const uint8_t* cursor = bytes.data();
    const uint8_t* end = cursor + bytes.size();
    int64_t lastPlace[2] { };
    uint64_t representation = static_cast<uint64_t>(NamedVariablesWriter::representationInFrontOfFirst);
    uint64_t module = 0;
    int64_t endOfLastRun = 0;
    while (cursor < end) {
        bool isFirstModule = cursor == bytes.data();
        uint64_t modulesFurther = readVarint(cursor);
        module += modulesFurther;
        if ((!modulesFurther && !isFirstModule) || module > UINT32_MAX || cursor >= end)
            return false;
        uint64_t numberOfVariables = readVarint(cursor);
        beginModule(static_cast<uint32_t>(module), static_cast<size_t>(numberOfVariables));
        uint64_t name = 0;
        for (uint64_t index = 0; index < numberOfVariables; ++index) {
            if (cursor >= end)
                return false;
            name += readVarint(cursor);
            if (cursor >= end || name >= VariableData::noName)
                return false;
            uint64_t word = readVarint(cursor);
            if (word & 1) {
                if (cursor >= end)
                    return false;
                representation = readVarint(cursor);
                if (representation >= numberOfEnumerators<SlotRepresentation>)
                    return false;
            }
            bool isClosure = static_cast<SlotRepresentation>(representation) == SlotRepresentation::DeferredClosure;
            int64_t& place = lastPlace[isClosure];
            place += zigZagDecode(word >> 1);
            int64_t offset = isClosure ? place : place * NamedVariablesWriter::bytesInUnitOfStatics;
            if (place < 0 || offset > static_cast<int64_t>(SlotData::maxOffset))
                return false;
            visit(VariableData { static_cast<uint32_t>(name), SlotData::create(static_cast<uint32_t>(offset), static_cast<SlotRepresentation>(representation)) });
        }
        if (cursor >= end)
            return false;
        for (uint64_t runsLeft = readVarint(cursor); runsLeft; --runsLeft) {
            if (cursor >= end)
                return false;
            int64_t first = endOfLastRun + zigZagDecode(readVarint(cursor));
            if (cursor >= end)
                return false;
            uint64_t length = readVarint(cursor);
            if (first < 0 || !length || length > UINT32_MAX || first + static_cast<int64_t>(length) > UINT32_MAX)
                return false;
            visitRunOfClosureNumbers(static_cast<uint32_t>(first), static_cast<uint32_t>(length));
            endOfLastRun = first + static_cast<int64_t>(length);
        }
    }
    return cursor == end;
}

struct DataWord {
    enum class Kind : uint8_t { Immediate, StringConstant, BigIntConstant, Node };

    static constexpr unsigned operandShift = 3;
    static constexpr uint64_t tagMask = (1u << operandShift) - 1;
    static constexpr uint64_t stringConstantTag = 0;
    static constexpr uint64_t bigIntConstantTag = 1;
    static constexpr uint64_t nodeTag = 4;
    static constexpr uint64_t hole = 0;

    static constexpr uint64_t encode(uint64_t tag, uint32_t operand) { return (static_cast<uint64_t>(operand) + 1) << operandShift | tag; }
    static constexpr uint64_t encodeStringConstant(uint32_t number) { return encode(stringConstantTag, number); }
    static constexpr uint64_t encodeBigIntConstant(uint32_t number) { return encode(bigIntConstantTag, number); }
    static constexpr uint64_t encodeNode(uint32_t firstWord) { return encode(nodeTag, firstWord); }
    static uint64_t encodeImmediate(JSValue value)
    {
        ASSERT(!value || !value.isCell());
        return static_cast<uint64_t>(JSValue::encode(value));
    }

    static Kind kindOf(uint64_t word)
    {
        if (!word || !JSValue::decode(static_cast<EncodedJSValue>(word)).isCell())
            return Kind::Immediate;
        switch (word & tagMask) {
        case stringConstantTag:
            return Kind::StringConstant;
        case bigIntConstantTag:
            return Kind::BigIntConstant;
        case nodeTag:
            return Kind::Node;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    static constexpr uint32_t operandOf(uint64_t word) { return static_cast<uint32_t>(word >> operandShift) - 1; }
};
static_assert(!(DataWord::encode(DataWord::nodeTag, UINT32_MAX - 1) & JSValue::NotCellMask));

struct NarrowDataWord {
    static constexpr unsigned operandShift = 2;
    static constexpr uint32_t maxOperand = (UINT32_MAX - minCell32) >> operandShift;

    static constexpr bool isFinal(uint32_t word) { return word < minCell32; }
    static constexpr uint32_t encode(DataWord::Kind kind, uint32_t operand) { return minCell32 + (operand << operandShift | static_cast<uint32_t>(kind)); }
    static constexpr DataWord::Kind kindOf(uint32_t word) { return static_cast<DataWord::Kind>((word - minCell32) & ((1u << operandShift) - 1)); }
    static constexpr uint32_t operandOf(uint32_t word) { return (word - minCell32) >> operandShift; }
    static constexpr uint64_t widen(uint32_t word)
    {
        uint32_t operand = operandOf(word);
        switch (kindOf(word)) {
        case DataWord::Kind::StringConstant:
            return DataWord::encodeStringConstant(operand);
        case DataWord::Kind::BigIntConstant:
            return DataWord::encodeBigIntConstant(operand);
        case DataWord::Kind::Node:
            return DataWord::encodeNode(operand);
        case DataWord::Kind::Immediate:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
};
static_assert(static_cast<uint32_t>(DataWord::Kind::Node) < 1u << NarrowDataWord::operandShift);

struct DataNodeHeader {
    enum class Kind : uint32_t { Array, Object, ObjectWithOtherMembers, OtherMembers, ObjectBody, OrdinaryObject };
    static constexpr unsigned fieldIndicesPerWord = sizeof(uint64_t) / sizeof(uint16_t);
    static constexpr unsigned identifierNumbersPerWord = sizeof(uint64_t) / sizeof(uint32_t);
    static constexpr uint32_t firstNode = 1;
    static constexpr uint32_t wordsBeforeElements = 2;
    static constexpr uint32_t indexingTypeMask = 0xff;
    static constexpr uint32_t isStorage = 0x100;

    uint32_t count;
    uint32_t kindBits : 3;
    uint32_t operand : 29;

    Kind kind() const { return static_cast<Kind>(kindBits); }
    uint64_t encoded() const { return std::bit_cast<uint64_t>(*this); }
    static DataNodeHeader decode(uint64_t word) { return std::bit_cast<DataNodeHeader>(word); }
    static constexpr uint32_t numberOfWords(Kind kind, uint32_t count)
    {
        switch (kind) {
        case Kind::Array:
            return wordsBeforeElements + count;
        case Kind::Object:
            return 1 + count + (count + fieldIndicesPerWord - 1) / fieldIndicesPerWord;
        case Kind::ObjectWithOtherMembers:
            return numberOfWords(Kind::Object, count) + 1;
        case Kind::OtherMembers:
        case Kind::OrdinaryObject:
            return 1 + count + (count + identifierNumbersPerWord - 1) / identifierNumbersPerWord;
        case Kind::ObjectBody:
            return 1 + count;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
};
static_assert(sizeof(DataNodeHeader) == 8 && isImageRecord<DataNodeHeader>);

struct DataRequest {
    static constexpr uint32_t noSharedStorage = UINT32_MAX;
    static constexpr uint32_t rootHasOwnStorage = UINT32_MAX - 1;

    uint32_t root;
    uint32_t sharedStorageOffset;
};
static_assert(sizeof(DataRequest) == 8 && isImageRecord<DataRequest>);

struct CollectionDataRequest {
    uint32_t tableInImage;
    uint32_t elements;
};
static_assert(sizeof(CollectionDataRequest) == 8 && isImageRecord<CollectionDataRequest>);

struct NewClassRequest {
    uint32_t classIndex;
};
static_assert(sizeof(NewClassRequest) == 4 && isImageRecord<NewClassRequest>);

struct TemplateObjectRequest {
    struct Part {
        uint32_t cookedPlusOne;
        uint32_t raw;
    };

    uint32_t count;
    Part parts[];
};
static_assert(sizeof(TemplateObjectRequest) == 4);

struct ImportChecksRequest {
    struct Check {
        uint32_t offsetInStatics;
        uint32_t failure;
    };

    uint32_t count;
    Check checks[];
};
static_assert(sizeof(ImportChecksRequest) == 4 && sizeof(ImportChecksRequest::Check) == 8);

constexpr bool isAssignableBinding(EvalBindingKind kind)
{
    return kind == EvalBindingKind::Variable || kind == EvalBindingKind::Arguments || kind == EvalBindingKind::MappedArgument || kind == EvalBindingKind::SimpleCatchParameter;
}

struct EvalBindingData {
    static constexpr uint32_t noOperand = UINT32_MAX;

    uint32_t name;
    uint32_t kind : 8;
    uint32_t depth : 24;
    uint32_t operand;
    uint32_t function;
    uint32_t setter;
    uint32_t privateKey;
    SlotData slot;
};
static_assert(sizeof(EvalBindingData) == 28 && isImageRecord<EvalBindingData>);

struct DirectEvalRequest {
    static constexpr uint32_t noOuter = UINT32_MAX;

    std::span<const uint32_t> bindingsByName() const { return { reinterpret_cast<const uint32_t*>(bindings + numberOfBindings), numberOfBindings }; }

    uint32_t isStrict : 1;
    uint32_t isInFunction : 1;
    uint32_t isInArrowFunction : 1;
    uint32_t isInFieldInitializer : 1;
    uint32_t isInDerivedConstructor : 1;
    uint32_t allowsSuperProperty : 1;
    uint32_t allowsSuperCall : 1;
    uint32_t allowsNewTarget : 1;
    uint32_t unused : 24;
    uint32_t module;
    uint32_t outer;
    uint32_t numberOfBindings;
    EvalBindingData bindings[];
};
static_assert(sizeof(DirectEvalRequest) == 16);

struct PropertyDescriptorBits {
    uint32_t hasValue : 1;
    uint32_t hasWritable : 1;
    uint32_t isWritable : 1;
    uint32_t hasEnumerable : 1;
    uint32_t isEnumerable : 1;
    uint32_t hasConfigurable : 1;
    uint32_t isConfigurable : 1;
    uint32_t hasGet : 1;
    uint32_t hasSet : 1;
    uint32_t unused : 23;
};
static_assert(sizeof(PropertyDescriptorBits) == 4);

static constexpr uint32_t accessorIsGetter = 1;
static constexpr uint32_t accessorIsEnumerable = 2;

struct DefinedAccessor {
    EncodedJSValue key;
    EncodedJSValue function;
};
static_assert(sizeof(DefinedAccessor) == 2 * sizeof(EncodedJSValue));

struct DefinedMethodBits {
    uint32_t side : 1;
    uint32_t kind : 2;
    uint32_t isUnderComputedKey : 1;
    uint32_t index : 28;

    static constexpr uint32_t encode(ClassSide side, MethodKind kind, bool isUnderComputedKey, uint32_t index)
    {
        return static_cast<uint32_t>(side) | static_cast<uint32_t>(kind) << 1 | static_cast<uint32_t>(isUnderComputedKey) << 3 | std::min<uint32_t>(index, (1u << 28) - 1) << 4;
    }
    static DefinedMethodBits decode(uint32_t word) { return std::bit_cast<DefinedMethodBits>(word); }
};
static_assert(sizeof(DefinedMethodBits) == 4);

struct NumberAndValueBits {
    static constexpr unsigned typeBits = 31;

    uint32_t type : typeBits;
    uint32_t numberIsOnRight : 1;

    static constexpr uint32_t encode(uint32_t type, bool numberIsOnRight)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(!(type >> typeBits));
        return type | static_cast<uint32_t>(numberIsOnRight) << typeBits;
    }
    static NumberAndValueBits decode(uint32_t word) { return std::bit_cast<NumberAndValueBits>(word); }
};
static_assert(sizeof(NumberAndValueBits) == 4);

struct WrittenNativeResult {
    static constexpr uint32_t undefinedKind = 1 << 0;
    static constexpr uint32_t nullKind = 1 << 1;
    static constexpr uint32_t booleanKind = 1 << 2;
    static constexpr uint32_t numberKind = 1 << 3;
    static constexpr uint32_t stringKind = 1 << 4;
    static constexpr uint32_t symbolKind = 1 << 5;
    static constexpr uint32_t bigIntKind = 1 << 6;
    static constexpr uint32_t objectKind = 1 << 7;
    static constexpr unsigned numberOfKinds = 8;
    static constexpr uint32_t everyKind = (1 << numberOfKinds) - 1;

    uint32_t kinds : numberOfKinds;
    uint32_t constructs : 1;
    uint32_t builtin : 22;
    uint32_t writesLengths : 1;

    static constexpr uint32_t encode(uint32_t kinds, bool constructs, uint32_t builtin, bool writesLengths = false) { return kinds | static_cast<uint32_t>(constructs) << numberOfKinds | builtin << (numberOfKinds + 1) | static_cast<uint32_t>(writesLengths) << 31; }
    static WrittenNativeResult decode(uint32_t word) { return std::bit_cast<WrittenNativeResult>(word); }
};
static_assert(sizeof(WrittenNativeResult) == 4);

enum class HostResult : uint8_t { Boolean, String, Symbol, Object, Array };

constexpr uint32_t kindsOfHostResult(HostResult result)
{
    switch (result) {
    case HostResult::Boolean:
        return WrittenNativeResult::booleanKind;
    case HostResult::String:
        return WrittenNativeResult::stringKind;
    case HostResult::Symbol:
        return WrittenNativeResult::symbolKind;
    case HostResult::Object:
    case HostResult::Array:
        return WrittenNativeResult::objectKind;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class ThrowKind : uint8_t {
    NullishRead,
    NullishWrite,
    NullishIterate,
    NullishSpread,
    UninitializedBinding,
    NotAFunction,
    ErrorWithMessage,
    NullishUpdate,
};

enum class ThrowSiteSource : uint32_t {
    WordsAfterCall,
    WordInRegister,
    FaultSite,
};

struct FalseProofWord {
    static constexpr unsigned sourceBits = 4;
    static constexpr uint32_t primitivesMask = (1u << 10) - 1;
    static constexpr uint32_t objectBit = 1u << 10;
    static constexpr uint32_t stringLiteralBit = 1u << 11;
    static constexpr uint32_t symbolLiteralBit = 1u << 12;
    static constexpr uint32_t everyBit = (1u << 13) - 1;
    static_assert(Typed::Primitive::All == primitivesMask);

    static constexpr uint32_t summarize(uint16_t primitives, bool hasObjects)
    {
        uint32_t summary = primitives & primitivesMask;
        if ((primitives & Typed::Primitive::AnyString) == Typed::Primitive::LiteralString)
            summary |= stringLiteralBit;
        if ((primitives & Typed::Primitive::AnySymbol) == Typed::Primitive::LiteralSymbol)
            summary |= symbolLiteralBit;
        return summary | (hasObjects ? objectBit : 0);
    }
    static constexpr uint32_t encode(uint32_t source, uint32_t expectedType) { return source | expectedType << sourceBits; }
    static constexpr uint32_t sourceOf(uint32_t a) { return a & ((1u << sourceBits) - 1); }
    static constexpr uint32_t expectedTypeOf(uint32_t a) { return a >> sourceBits; }
};

struct ThrowSiteWord {
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned narrowABits = 17;
    static constexpr uint32_t narrowNone = (1u << narrowABits) - 1;
    static constexpr uint32_t maxNarrowA = narrowNone - 1;
    static constexpr unsigned numberOfRegistersPerBank = 32;
#if CPU(X86_64)
    static constexpr unsigned numberOfStoredGPRs = 16;
    static constexpr unsigned numberOfStoredFPRs = 16;
#else
    static constexpr unsigned numberOfStoredGPRs = 30;
    static constexpr unsigned numberOfStoredFPRs = 32;
#endif
#if CPU(X86_64)
    static constexpr uint8_t opcodeInFrontOfWordBehindCall = 0xa9;
    static constexpr size_t sizeOfWordBehindCall = sizeof(uint8_t) + sizeof(uint32_t);
#else
    static constexpr size_t sizeOfWordBehindCall = sizeof(uint32_t);
#endif

    static constexpr Typed::Representation runtimeErrorRepresentation = Typed::Representation::RawWord;

    bool isUninitializedBinding() const { return static_cast<ThrowKind>(kind) == ThrowKind::UninitializedBinding; }
    uint32_t narrowA() const
    {
        if (isUninitializedBinding() && calleeIsReadFromReceiver)
            return registerNumber << narrowABits | a;
        return a == narrowNone ? none : a;
    }
    bool throwsRuntimeError() const { return static_cast<ThrowKind>(kind) == ThrowKind::ErrorWithMessage && static_cast<Typed::Representation>(representation) == runtimeErrorRepresentation; }
    bool stopsAtFalseProof() const { return static_cast<ThrowKind>(kind) == ThrowKind::ErrorWithMessage && calleeIsReadFromReceiver; }
    bool isCallSite() const { return static_cast<ThrowKind>(kind) == ThrowKind::NotAFunction && static_cast<Typed::Representation>(representation) == Typed::Representation::Void; }
    bool differsOnlyInRegisterNumber(ThrowSiteWord other) const
    {
        other.registerNumber = registerNumber;
        return std::bit_cast<uint32_t>(other) == std::bit_cast<uint32_t>(*this);
    }
    static std::optional<uint32_t> withNarrowA(uint32_t firstWord, uint32_t a)
    {
        auto word = std::bit_cast<ThrowSiteWord>(firstWord);
        RELEASE_ASSERT(word.hasWideA && !word.a);
        if (a > maxNarrowA) {
            uint32_t above = a >> narrowABits;
            if (!word.isUninitializedBinding() || word.calleeIsReadFromReceiver || !above || above >= numberOfRegistersPerBank)
                return std::nullopt;
            word.registerNumber = above;
            word.calleeIsReadFromReceiver = true;
        }
        word.hasWideA = false;
        word.a = a & narrowNone;
        return std::bit_cast<uint32_t>(word);
    }
    static constexpr uint64_t inRegister(uint32_t firstWord, uint32_t wideA) { return firstWord | static_cast<uint64_t>(wideA) << 32; }

    uint32_t registerNumber : 5;
    uint32_t kind : 3;
    uint32_t representation : 4;
    uint32_t hasWideA : 1;
    uint32_t hasB : 1;
    uint32_t calleeIsReadFromReceiver : 1;
    uint32_t a : narrowABits;
};
static_assert(sizeof(ThrowSiteWord) == 4);
static_assert(static_cast<unsigned>(ThrowKind::NullishUpdate) < 1u << 3 && numberOfEnumerators<Typed::Representation> <= 1u << 4);

#define FOR_EACH_AOT_OPERATION_PROGRAM_TABLE(v) \
    v(staticsLayout, StaticsLayout) \
    v(staticsRuns, StaticsRun) \
    v(staticsRoots, StaticsRange) \
    v(staticsNarrowRoots, StaticsRange) \
    v(staticsClearedEachCollection, StaticsRange) \
    v(functionVariables, FunctionVariableData) \
    v(namedVariables, uint8_t) \
    v(identifiersByHash, uint32_t) \
    v(identifiersOfEngineSymbols, uint32_t) \
    v(keyCellIdentifiers, uint32_t) \
    v(bigIntConstants, uint32_t) \
    v(data, uint64_t) \
    v(requests, uint32_t) \
    v(builtinCells, uint32_t) \
    v(eagerBuiltinCells, uint32_t) \
    v(constantAndUnlockedBuiltinMembers, uint32_t) \
    v(namesTakenForAbsent, uint32_t) \
    v(globalBindings, uint32_t) \
    v(entryPoints, uint32_t)

enum class TrimmedEnds : uint8_t { Start, End, Both };

enum class URICoding : uint8_t { DecodeURI, DecodeURIComponent, EncodeURI, EncodeURIComponent };

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
