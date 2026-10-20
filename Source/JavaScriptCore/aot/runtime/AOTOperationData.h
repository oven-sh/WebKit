/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include "JSCJSValue.h"
#include <bit>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct StaticsLayout {
    uint32_t sizeFilledAtProgramEntry;
    uint32_t distanceFromArrayProfileToLastArray;
};
static_assert(sizeof(StaticsLayout) == 8 && isImageRecord<StaticsLayout>);

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

// The names that compiled code takes to be absent from a built-in prototype: numbers of identifiers, ascending, in closedAbsentNames.
struct ClosedAbsentNamesOfHolder {
    uint32_t holder;
    uint32_t firstName;
    uint32_t numberOfNames;
};
static_assert(sizeof(ClosedAbsentNamesOfHolder) == 12 && isImageRecord<ClosedAbsentNamesOfHolder>);

struct LiteralStringLimits {
    uint32_t maxLength;
};
static_assert(sizeof(LiteralStringLimits) == 4 && isImageRecord<LiteralStringLimits>);

struct LiteralStringTable {
    static constexpr unsigned indexBits = 19;
    static constexpr uint32_t indexMask = (1u << indexBits) - 1;
    static constexpr uint32_t entryFor(uint32_t number, uint32_t hash) { return (number + 1) | hash >> 11 << indexBits; }
    static constexpr uint32_t numberOf(uint32_t entry) { return (entry & indexMask) - 1; }
};
static_assert(maxLiteralStrings < 1u << LiteralStringTable::indexBits);

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

struct DataNodeHeader {
    enum class Kind : uint32_t { Array, Object };
    static constexpr unsigned fieldIndicesPerWord = sizeof(uint64_t) / sizeof(uint16_t);

    uint32_t count;
    uint32_t kindBits : 1;
    uint32_t operand : 31;

    Kind kind() const { return static_cast<Kind>(kindBits); }
    uint64_t encoded() const { return std::bit_cast<uint64_t>(*this); }
    static DataNodeHeader decode(uint64_t word) { return std::bit_cast<DataNodeHeader>(word); }
    static constexpr uint32_t numberOfWords(Kind kind, uint32_t count)
    {
        return 1 + count + (kind == Kind::Object ? (count + fieldIndicesPerWord - 1) / fieldIndicesPerWord : 0);
    }
};
static_assert(sizeof(DataNodeHeader) == 8 && isImageRecord<DataNodeHeader>);

struct DataRequest {
    static constexpr uint32_t noSharedStorage = UINT32_MAX;

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

struct CloneAsRequest {
    uint32_t shape;
    uint32_t count;
    uint32_t fields[];
};
static_assert(sizeof(CloneAsRequest) == 8);

struct TemplateObjectRequest {
    struct Part {
        uint32_t cookedPlusOne;
        uint32_t raw;
    };

    uint32_t count;
    Part parts[];
};
static_assert(sizeof(TemplateObjectRequest) == 4);

struct EvalBindingData {
    static constexpr uint32_t noOperand = UINT32_MAX;

    uint32_t name;
    uint32_t kind : 8;
    uint32_t depth : 24;
    uint32_t operand;
    uint32_t function;
    uint32_t setter;
    // The identifier number of the private symbol that instances hold: the field's key, or the brand of a method or an accessor. 0 for a static name.
    uint32_t privateKey;
    SlotData slot;
    uint32_t type;
};
static_assert(sizeof(EvalBindingData) == 32 && isImageRecord<EvalBindingData>);

struct DirectEvalRequest {
    // The bindings are followed by their indexes, sorted by the identifier number of the name. Among bindings with the same name the inner one, which has the lower index, comes first.
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
    uint32_t numberOfBindings;
    EvalBindingData bindings[];
};
static_assert(sizeof(DirectEvalRequest) == 12);

static constexpr uint32_t typeAllowsObjects = 1 << 10;
static constexpr uint32_t typeAllowsFunctions = 1 << 11;

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
    uint32_t unused : 29;

    static constexpr uint32_t encode(ClassSide side, MethodKind kind) { return static_cast<uint32_t>(side) | static_cast<uint32_t>(kind) << 1; }
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

enum class ThrowKind : uint8_t {
    TypeMismatch,
    KindMismatch,
    NullishRead,
    NullishWrite,
    NullishIterate,
    UninitializedBinding,
    NotAFunction,
    ErrorWithMessage,
};

enum class ThrowSiteSource : uint32_t {
    WordsAfterCall,
    WordInRegister,
    FaultSite,
};

struct ThrowSiteWord {
    static constexpr uint32_t maxNarrowA = (1u << 17) - 1;
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned numberOfRegistersPerBank = 32;

    uint32_t kind : 3;
    uint32_t representation : 4;
    uint32_t registerNumber : 5;
    uint32_t hasWideA : 1;
    uint32_t hasB : 1;
    uint32_t unused : 1;
    uint32_t a : 17;
};
static_assert(sizeof(ThrowSiteWord) == 4);
static_assert(static_cast<unsigned>(ThrowKind::ErrorWithMessage) < 1u << 3 && numberOfEnumerators<Typed::Representation> <= 1u << 4);

#define FOR_EACH_AOT_OPERATION_PROGRAM_TABLE(v) \
    v(staticsLayout, StaticsLayout) \
    v(staticsRuns, StaticsRun) \
    v(staticsRoots, StaticsRange) \
    v(staticsClearedEachCollection, StaticsRange) \
    v(functionVariables, FunctionVariableData) \
    v(identifiersByHash, uint32_t) \
    v(identifiersOfEngineSymbols, uint32_t) \
    v(literalStrings, uint32_t) \
    v(literalStringTable, uint32_t) \
    v(literalStringLimits, LiteralStringLimits) \
    v(bigIntConstants, uint32_t) \
    v(data, uint64_t) \
    v(requests, uint32_t) \
    v(builtinCells, uint32_t) \
    v(eagerBuiltinCells, uint32_t) \
    v(constantBuiltinMembers, uint32_t) \
    v(holdersOfClosedAbsentNames, ClosedAbsentNamesOfHolder) \
    v(closedAbsentNames, uint32_t) \
    v(entryPoints, uint32_t)

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
