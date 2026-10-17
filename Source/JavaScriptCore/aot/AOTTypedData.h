/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include "MarkedBlockCage.h"
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline Vector<char16_t> decodeProgramString(std::span<const uint8_t> bytes)
{
    static constexpr char32_t minCodePointForLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
    Vector<char16_t> units;
    for (size_t i = 0; i < bytes.size();) {
        uint8_t lead = bytes[i];
        unsigned length = lead < 0xc0 || lead >= 0xf8 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        char32_t character = length == 1 ? lead : lead & (0x7f >> length);
        bool isWhole = i + length <= bytes.size();
        for (unsigned k = 1; k < length && isWhole; ++k) {
            isWhole = (bytes[i + k] & 0xc0) == 0x80;
            character = character << 6 | (bytes[i + k] & 0x3f);
        }
        if (!isWhole || character < minCodePointForLength[length] || character > 0x10ffff) {
            character = 0xfffd;
            length = 1;
        }
        i += length;
        if (character < 0x10000)
            units.append(static_cast<char16_t>(character));
        else {
            units.append(static_cast<char16_t>(0xd7c0 + (character >> 10)));
            units.append(static_cast<char16_t>(0xdc00 | (character & 0x3ff)));
        }
    }
    return units;
}

enum class PlainAllocationKind : uint8_t { Object, This, Array, Clone };
static constexpr unsigned plainAllocationKindShift = 28;

enum class Writes : uint8_t {
    Pure,
    Nothing,
    RuntimeState,
    TypedArrayProperties,
    Fields,
    Heap,
};

enum class Store : uint8_t {
    Uint8,
    Int8,
    Uint16,
    Int16,
    Int32,
    Uint32,
    Int64,
    Float,
    Double,
    Cell,
    JSValue,
    OptionalInt64,
    OptionalDouble,
    Boolean,
    Int32OrNumberBox,
    Cell32,
    OptionalCell32,
    OptionalBoolean,
    LiteralString,
    LiteralString32,
};

enum class LayoutKind : uint8_t {
    Record,
    Instance,
    Environment,
    Closure,
};

inline bool isObjectLayout(LayoutKind kind) { return kind == LayoutKind::Record || kind == LayoutKind::Instance; }

static constexpr int64_t undefinedInteger = 0x4000ll << 48;
static constexpr int64_t nullInteger = 0x4001ll << 48;
static constexpr int64_t absentInteger = 0x4002ll << 48;
static constexpr uint64_t undefinedDouble = 0x7ffaull << 48;
static constexpr uint64_t nullDouble = 0x7ffbull << 48;
static constexpr uint64_t absentDouble = 0x7ffcull << 48;

static constexpr int32_t absentInt32 = INT32_MIN + 1;
static constexpr int32_t undefinedInt32 = INT32_MIN + 2;
static constexpr int32_t nullInt32 = INT32_MIN + 3;
static constexpr int32_t minInlineInt32 = -(1 << 30);
static constexpr unsigned numberBoxShift = 3;
inline bool refersToNumberBox(int32_t word) { return word > nullInt32 && word < minInlineInt32; }

static constexpr unsigned cell32Shift = MarkedBlockCage::shift;
static constexpr bool cell32IsPointer = false;
static constexpr uint32_t absentCell32 = 0;
static constexpr uint32_t unmaterializedClosureCell32 = 1;
static constexpr uint32_t nullCell32 = 2;
static constexpr uint32_t undefinedCell32 = 10;

static constexpr uint32_t emptyLiteralStringWord = 14;
static constexpr unsigned literalStringShift = 2;
static constexpr uint32_t maxLiteralStrings = 1 << 18;
static constexpr uint32_t literalStringWord(uint32_t number) { return emptyLiteralStringWord + (number << literalStringShift); }
static constexpr uint32_t literalStringIndex(uint32_t word) { return (word - emptyLiteralStringWord) >> literalStringShift; }
static constexpr uint32_t minCell32 = MarkedBlockCage::minIndex;
static_assert(literalStringWord(maxLiteralStrings) <= minCell32);
inline bool isLiteralStringWord(uint64_t word) { return word - emptyLiteralStringWord < minCell32 - emptyLiteralStringWord; }
static constexpr size_t maxShortStringLength = 7;
static constexpr uint64_t noShortString = UINT64_MAX;
template<typename CharacterType>
inline uint64_t shortStringWord(std::span<const CharacterType> text)
{
    uint64_t characters = 0;
    for (size_t i = 0; i < text.size(); ++i)
        characters |= static_cast<uint64_t>(text[i]) << (8 * i);
    return characters << 3 | text.size();
}
static constexpr unsigned literalStringCacheIndexBits = 8;
static constexpr unsigned literalStringCacheIndexShift = 5;
static constexpr unsigned literalStringCacheWordBits = 20;
static_assert(literalStringWord(maxLiteralStrings) >> 1 < 1u << literalStringCacheWordBits);
static_assert(maxLiteralStrings < 1u << 19);
static constexpr uint64_t literalStringCacheIndex(uintptr_t impl) { return impl >> literalStringCacheIndexShift & ((1 << literalStringCacheIndexBits) - 1); }
static constexpr uint64_t literalStringCacheEntry(uintptr_t impl, uint32_t wordOrZero) { return static_cast<uint64_t>(impl >> 3) << literalStringCacheWordBits | wordOrZero >> 1; }

inline bool representsAbsentAsZero(Store store) { return store != Store::OptionalInt64 && store != Store::OptionalDouble && store != Store::Int32OrNumberBox; }

static constexpr unsigned stringTableShift = 7;
static constexpr uint32_t startInStringTable(uint32_t key) { return key + (key << stringTableShift); }
static constexpr ptrdiff_t offsetOfMaskInStringTable = sizeof(uint32_t);

enum class CopyNamesKind : uint32_t { Excluded, StoredNext, StoredFirst };
static constexpr uint32_t shapeWordOf(uint32_t shape) { return 0 - shape; }
static constexpr uint32_t shapeOf(uint32_t shapeWord) { return 0 - shapeWord; }
static constexpr uint32_t offsetOfOrderWord = 8;
static constexpr uint32_t offsetOfShapeWord = 12;
static constexpr uint32_t offsetOfFirstField = 16;

static constexpr uint32_t offsetOfExtension = 16;
static constexpr uint32_t sizeOfExtensionWord = 4;
static constexpr uint32_t sizeOfExtraPropertiesWord = 4;
static constexpr uint32_t offsetOfExtensionSize = 0;
static constexpr uint32_t offsetOfFirstExtensionSlot = 4;
static constexpr uint32_t minExtensionSize = 16;
static constexpr uint32_t outOfLineSlotKey = 0x8000;
static constexpr uint32_t slotKeyFor(uint32_t offset, bool isOutOfLine) { return offset < outOfLineSlotKey ? offset | (isOutOfLine ? outOfLineSlotKey : 0) : 0; }
static constexpr uint32_t maxFieldsInOrder = (1 << 12) - 1;
static constexpr uint32_t orderWordOf(uint32_t order, uint32_t count) { return order | count << 16; }
static constexpr uint32_t orderOf(uint32_t orderWord) { return orderWord & 0xffff; }
static constexpr uint32_t countOf(uint32_t orderWord) { return orderWord >> 16 & maxFieldsInOrder; }
static constexpr uint32_t orderWordHasExtraProperties = 1u << 28;
static constexpr uint32_t orderWordIsFrozen = 1u << 29;
static constexpr uint32_t orderWordIsSealed = 1u << 30;
static constexpr uint32_t orderWordIsNotExtensible = 1u << 31;
static constexpr uint32_t listsFieldsWithoutUnlisting = 1u << 31;
static constexpr uint32_t listsFieldNamed = 1u << 30;

inline bool isPrivateName(std::span<const uint8_t> name) { return !name.empty() && name[0] == 0xfe; }
inline bool isEnginePrivateName(std::span<const uint8_t> name) { return !name.empty() && name[0] == 0xfd; }
inline bool isConstantSymbol(std::span<const uint8_t> name) { return !name.empty() && name[0] == 0xfc; }
inline bool isStringKey(std::span<const uint8_t> name) { return !isPrivateName(name) && !isConstantSymbol(name); }
Identifier createConstantSymbol(VM&, std::span<const uint8_t> name);

static constexpr uint32_t nameMayBeOwnField = 1u << 31;
static constexpr uint32_t fieldIsOptional = 1;
static constexpr uint32_t fieldIsOutOfLine = 2;
static constexpr uint32_t fieldIsImmutable = 4;
static constexpr uint32_t fieldAllowsUndefined = 8;
static constexpr uint32_t fieldAllowsNull = 16;
static constexpr uint32_t fieldIsAnnotated = 32;
static constexpr uint32_t fieldIsAbsentAtCreation = 8192;
static constexpr uint32_t fieldIsNeverObservedAbsent = 32768;
static constexpr uint32_t fieldIsNeverObservedAbsentAsDeclared = 262144;
static constexpr uint32_t fieldIsPinned = 131072;
static constexpr uint32_t fieldMayExistWithoutStorage = 1048576;
static constexpr uint32_t fieldIsPlaceholder = 2097152;
static constexpr uint32_t fieldMayBeUnlisted = 8388608;
static constexpr uint32_t fieldHoldsLazyClosure = 4194304;
static constexpr uint32_t lazyClosureCapturesObject = 1u << 31;
static constexpr uint32_t nameIsFieldCreatedWithoutStorage = 1u << 31;
static constexpr uint32_t pinnedFieldGuardShift = 24;
static constexpr uint32_t numberOfPinnedFieldGuards = 256;

struct RuntimeShapeTest {
    uint32_t first;
    uint32_t count;
    uint32_t set;
    uint32_t supertypeShape;
    uint32_t name : 31;
    uint32_t hasDeclaredLocation : 1;
    uint32_t fields;
    uint32_t declaredPath;
    uint32_t declaredLine;
    static constexpr uint32_t sizeWithSupertypeShape = 16;
    static constexpr uint32_t sizeWithoutDeclaredLocation = 24;
};

struct SupertypeField {
    uint32_t name;
    uint32_t type;
    uint32_t isOptional;
    uint32_t typeSource;
};

struct SupertypeFields {
    static void append(Vector<uint8_t>& bytes, uint32_t number)
    {
        for (; number >= 0x80; number >>= 7)
            bytes.append(static_cast<uint8_t>(number | 0x80));
        bytes.append(static_cast<uint8_t>(number));
    }
    Vector<SupertypeField> decode(const uint32_t* typeOffsets) const
    {
        const uint8_t* at = reinterpret_cast<const uint8_t*>(this);
        auto read = [&] {
            uint32_t number = 0;
            for (unsigned shift = 0;; shift += 7) {
                uint8_t byte = *at++;
                number |= static_cast<uint32_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80))
                    return number;
            }
        };
        return Vector<SupertypeField>(read(), [&](size_t) {
            uint32_t nameAndIsOptional = read();
            uint32_t type = typeOffsets[read()];
            return SupertypeField { nameAndIsOptional >> 1, type, nameAndIsOptional & 1, read() - 1 };
        });
    }
};

struct RuntimeType {
    uint16_t primitives; // Primitive::*
    uint8_t bits;
    uint8_t jsTypesCount;
    uint16_t supertypesCount;
    uint16_t unused;
    static constexpr uint8_t allowsAnyObjectBit = 1;
    static constexpr uint8_t allowsAnyFunctionBit = 2;
    static constexpr uint8_t acceptsUntypedObjectsBit = 4;
    static constexpr uint8_t hasElementsBit = 8;
    static constexpr uint8_t hasOneIntegerBit = 16;
    static constexpr uint8_t hasIntegersBit = 32;
    static constexpr uint8_t isRelaxedForLoggingBit = 64;
    static constexpr uint8_t acceptsOnlyOtherLayoutsBit = 128;
    bool allowsAnyObject() const { return bits & allowsAnyObjectBit; }
    bool allowsAnyFunction() const { return bits & allowsAnyFunctionBit; }
    bool acceptsUntypedObjects() const { return bits & (acceptsUntypedObjectsBit | isRelaxedForLoggingBit | acceptsOnlyOtherLayoutsBit); }
    bool isRelaxedForLogging() const { return bits & isRelaxedForLoggingBit; }
    bool acceptsOnlyOtherLayouts() const { return bits & acceptsOnlyOtherLayoutsBit; }
    bool checksUntypedObjects() const { return bits & (isRelaxedForLoggingBit | acceptsOnlyOtherLayoutsBit); }
    bool acceptsUncheckedObjects() const { return bits & (acceptsUntypedObjectsBit | isRelaxedForLoggingBit); }
    bool acceptsUntypedObjectsByNature() const { return bits & acceptsUntypedObjectsBit; }
    uint32_t numberOfSupertypes() const { return supertypesCount; }
    uint32_t numberOfJSTypes() const { return jsTypesCount; }
    uint32_t elements() const { return bits & hasElementsBit ? *rest() : 0; }
    int64_t minInteger() const { return integer(0, 1); }
    int64_t maxInteger() const { return integer(2, 0); }
    const int32_t* supertypeDistances() const { return reinterpret_cast<const int32_t*>(this + 1); }
    const RuntimeShapeTest& view(uint32_t index) const { return *reinterpret_cast<const RuntimeShapeTest*>(reinterpret_cast<const uint8_t*>(this) + supertypeDistances()[index]); }
    const uint32_t* jsTypes() const { return reinterpret_cast<const uint32_t*>(supertypeDistances() + numberOfSupertypes()); }
    const uint32_t* rest() const { return jsTypes() + numberOfJSTypes(); }
    int64_t integer(unsigned word, int64_t defaultValue) const
    {
        const uint32_t* words = rest() + !!(bits & hasElementsBit);
        if (bits & hasOneIntegerBit)
            return static_cast<int32_t>(words[0]);
        if (bits & hasIntegersBit)
            return static_cast<int64_t>(words[word] | static_cast<uint64_t>(words[word + 1]) << 32);
        return defaultValue;
    }
};

struct LiteralData {
    static constexpr uint32_t stringTag = 0xfffe0000;
    uint32_t low;
    uint32_t high;
    static LiteralData ofString(uint32_t string) { return { string, stringTag }; }
    static LiteralData ofNumber(double number) { return { static_cast<uint32_t>(std::bit_cast<uint64_t>(number)), static_cast<uint32_t>(std::bit_cast<uint64_t>(number) >> 32) }; }
    bool isNumber() const { return high != stringTag; }
    uint32_t string() const { return low; }
    double number() const { return std::bit_cast<double>(static_cast<uint64_t>(high) << 32 | low); }
};
static_assert(sizeof(LiteralData) == 8);

struct HashedString {
    using Digest = std::array<uint8_t, 16>;
    uint32_t string;
    uint32_t length;
    uint32_t hash;
    Digest digest;
    static HashedString create(uint32_t string, std::span<const char16_t>);
    JS_EXPORT_PRIVATE static Digest digestOf(std::span<const char16_t>);
    JS_EXPORT_PRIVATE static Digest digestOfNarrowestEncoding(StringView);
    bool matches(StringView) const;
};
static_assert(sizeof(HashedString) == 28);

struct FieldTypes {
    uint32_t type;
    uint32_t ownType;
    uint32_t numberOfLiterals;
    uint32_t literalsOffset;
};

struct FieldData {
    uint32_t name;
    uint32_t flags;
    uint32_t offset : 24;
    uint32_t storeBits : 8;
    int32_t typesDistance;
    Store store() const { return static_cast<Store>(storeBits); }
    const FieldTypes& types() const { return *reinterpret_cast<const FieldTypes*>(reinterpret_cast<const uint8_t*>(this) + typesDistance); }
    uint32_t type() const { return types().type; }
    uint32_t ownType() const { return types().ownType; }
    uint32_t numberOfLiterals() const { return types().numberOfLiterals; }
    uint32_t literalsOffset() const { return types().literalsOffset; }
};
static_assert(sizeof(FieldData) == 16);

struct VariableData {
    uint32_t name;
    uint32_t offset : 24;
    uint32_t storeBits : 8;
    Store store() const { return static_cast<Store>(storeBits); }
};
static_assert(sizeof(VariableData) == 8);

namespace CoroutineFrame {
static constexpr uint32_t stateField = 0;
static constexpr uint32_t promiseField = 1;
static constexpr uint32_t resumeClosureField = 2;
static constexpr uint32_t firstArgumentField = 3;
static constexpr int32_t completedState = -1;
static constexpr int32_t isEnteredByJobLoop = 4;
}

static constexpr uint32_t methodHasComputedName = 1;
static constexpr uint32_t methodTakesEnvironment = 2;
static constexpr uint32_t methodMayBeReplaced = 4;
static constexpr uint32_t methodNameIsAssumedAbsent = 8;

struct MethodData {
    uint32_t name : 24;
    uint32_t kind : 4; // Format::MethodKind
    uint32_t flags : 4;
    uint32_t function;
};
static_assert(sizeof(MethodData) == 8);

struct ClassData {
    uint32_t base;
    uint32_t classConstructor;
    uint32_t environment;
    uint32_t numberOfMethods;
    uint32_t numberOfStaticMethods;
    uint32_t numberOfStaticFields;
    uint32_t numberOfVariables;
    uint32_t firstStaticFieldLocation;
};
static_assert(sizeof(ClassData) == 32);

struct RareLayoutData {
    uint32_t sizeWithSpecializationCapacity;
    uint32_t numberOfSpecializations : 31;
    uint32_t hasFieldsWithSymbolKeys : 1;
    uint32_t specializationsOffset;
    uint32_t extensionSize;
    uint32_t discriminant;
    uint32_t fieldsByNameOffset;
    uint32_t literalsWithoutFieldOffset;
};

struct MessageLayoutData {
    uint32_t typeStringsOffset;
    uint32_t declaredPath;
    uint32_t declaredLine;
};

struct LayoutData {
    static constexpr uint32_t noBase = UINT32_MAX;
    static constexpr uint32_t sizeOfEnvironment = 32;

    LayoutKind kind;
    uint8_t mayBeFrozen : 1;
    uint8_t mayDespecialize : 1;
    uint8_t hasFixedLayout : 1;
    uint8_t isEvaluatedOnce : 1;
    uint8_t hasUntypedBase : 1;
    uint8_t hasImmutableMembers : 1;
    uint8_t mayHaveOtherPrototype : 1;
    uint8_t constructorTakesNewTarget : 1;
    uint8_t createsMembersLazily : 1;
    uint8_t isAbstract : 1;
    uint8_t hasExtension : 1;
    uint8_t createsPrototypeBeforeClassObject : 1;
    uint8_t createsClassObjectOnMutation : 1;
    uint8_t mayBeConstructed : 1;
    uint8_t constructorMayBeReplaced : 1;
    uint8_t hasRareData : 1;
    uint8_t hasMessageData : 1;
    uint8_t extraPropertiesSlotIndex : 7;
    uint32_t index;
    uint32_t shape;
    uint32_t name;
    uint32_t size;
    uint32_t numberOfFields;
    union {
        uint32_t numberOfOrders;
        uint32_t numberOfDescribedVariables;
    };
    int32_t referencesOffset;
    uint32_t ordersOffset;
    const RareLayoutData& rareData() const { return reinterpret_cast<const RareLayoutData*>(this)[-1]; }
    RareLayoutData& rareData() { return const_cast<RareLayoutData&>(const_cast<const LayoutData*>(this)->rareData()); }
    const MessageLayoutData& messageData() const { return reinterpret_cast<const MessageLayoutData*>(reinterpret_cast<const uint8_t*>(this) - (hasRareData ? sizeof(RareLayoutData) : 0))[-1]; }
    MessageLayoutData& messageData() { return const_cast<MessageLayoutData&>(const_cast<const LayoutData*>(this)->messageData()); }
    uint32_t typeStringsOffset() const { return hasMessageData ? messageData().typeStringsOffset : 0; }
    uint32_t declaredPath() const { return hasMessageData ? messageData().declaredPath : UINT32_MAX; }
    uint32_t declaredLine() const { return hasMessageData ? messageData().declaredLine : 0; }
    uint32_t sizeWithSpecializationCapacity() const { return hasRareData ? rareData().sizeWithSpecializationCapacity : size; }
    uint32_t numberOfSpecializations() const { return hasRareData ? rareData().numberOfSpecializations : 0; }
    bool hasFieldsWithSymbolKeys() const { return hasRareData && rareData().hasFieldsWithSymbolKeys; }
    uint32_t specializationsOffset() const { return hasRareData ? rareData().specializationsOffset : 0; }
    uint32_t extensionSize() const { return hasRareData ? rareData().extensionSize : 0; }
    uint32_t discriminant() const { return hasRareData ? rareData().discriminant : noBase; }
    uint32_t fieldsByNameOffset() const { return hasRareData ? rareData().fieldsByNameOffset : 0; }
    uint32_t literalsWithoutFieldOffset() const { return hasRareData ? rareData().literalsWithoutFieldOffset : 0; }
    static constexpr uint32_t extraPropertiesOffsetLimit = sizeOfExtraPropertiesWord << 7;
    uint32_t offsetOfExtraProperties() const { return extraPropertiesSlotIndex * sizeOfExtraPropertiesWord; }
    bool isClass() const { return kind == LayoutKind::Instance; }
    const ClassData& classData() const { return reinterpret_cast<const ClassData*>(reinterpret_cast<const uint8_t*>(this) - (hasRareData ? sizeof(RareLayoutData) : 0) - (hasMessageData ? sizeof(MessageLayoutData) : 0))[-1]; }
    ClassData& classData() { return const_cast<ClassData&>(const_cast<const LayoutData*>(this)->classData()); }
    uint32_t base() const { return isClass() ? classData().base : noBase; }
    const FieldData* fields() const { return reinterpret_cast<const FieldData*>(this + 1); }
    std::span<const MethodData> methods() const { return { reinterpret_cast<const MethodData*>(fields() + numberOfFields), isClass() ? classData().numberOfMethods : 0 }; }
    std::span<const MethodData> staticMethods() const { return { methods().data() + methods().size(), isClass() ? classData().numberOfStaticMethods : 0 }; }
    const FieldData* variables() const { return reinterpret_cast<const FieldData*>(staticMethods().data() + staticMethods().size()); }
    std::span<const FieldData> staticFields() const { return { variables(), isClass() ? classData().numberOfStaticFields : 0 }; }
    std::span<const VariableData> describedVariables() const { return { reinterpret_cast<const VariableData*>(reinterpret_cast<const uint8_t*>(this) + sizeOfEnvironment), numberOfDescribedVariables }; }
};
static_assert(sizeof(LayoutData) == 36 && offsetof(LayoutData, index) == 4 && offsetof(LayoutData, ordersOffset) == LayoutData::sizeOfEnvironment);

struct AllocationData {
    uint32_t shape : 31;
    uint32_t usesFieldTemplate : 1;
    uint32_t orderWord;
    uint32_t size;
    uint32_t constructedBy;
};
static_assert(sizeof(AllocationData) == 16);

static constexpr uint8_t functionKindMask = 7;
static constexpr uint8_t functionIsAsync = 8;
static constexpr uint8_t functionIsGenerator = 16;
static constexpr uint8_t functionIsHiddenInStackTraces = 32;
static constexpr uint8_t functionReadsNewTarget = 64;
static constexpr uint8_t moduleIsCommonJS = functionReadsNewTarget;
static constexpr uint8_t functionIsInBuiltinModule = 128;
static constexpr uint8_t constructorFunctionKind = 3;
static constexpr uint8_t sharedBodyFunctionKind = 6;
static constexpr uint8_t resumeFunctionNamedByOwnerKind = 7;
static constexpr unsigned classObjectClosureStructure = 4;
static constexpr unsigned closureStructureOf(uint8_t kindAndDeclarationBits)
{
    if ((kindAndDeclarationBits & functionKindMask) == constructorFunctionKind)
        return classObjectClosureStructure;
    return kindAndDeclarationBits / functionIsAsync & 3;
}

static constexpr uint32_t boundFunctionHoldsTarget = 4;
static constexpr unsigned numberOfSlimClosureKinds = 2;
static constexpr unsigned slimClosureKindOf(uint8_t kindAndDeclarationBits)
{
    return !!(kindAndDeclarationBits & functionIsAsync);
}

struct AuditSite {
    static constexpr unsigned columnBits = 12;
    static constexpr uint32_t maxColumn = (1u << columnBits) - 1;
    static constexpr uint32_t pack(uint32_t line, uint32_t column) { return line << columnBits | (column < maxColumn ? column : maxColumn); }

    uint32_t function { UINT32_MAX };
    uint32_t lineAndColumn { 0 };
};

struct GlobalPropertyCache {
    uint32_t structureID;
    uint32_t globalLexicalBindingEpoch;
    int64_t location;
    EncodedJSValue value;
};

struct FunctionTexts {
    uint32_t numberOfEntries;
    uint32_t entriesOffset;
    uint32_t textOffset;
    uint32_t pathsOffset;
};

struct Data {
    static constexpr uint32_t expectedMagic = 0x44505942;

    uint32_t magic;
    uint32_t numberOfStrings;
    uint32_t stringsOffset; // Right behind this header, a word each: the offset above the length. Length 0: what is above it numbers a pair of offset and length at listedStringsOffset.
    static constexpr unsigned stringLengthBits = 8;
    uint32_t listedStringsOffset;
    uint32_t atomStringsOffset;
    uint32_t numberOfDecodedStrings;
    uint32_t decodedStringsOffset;
    static constexpr uint32_t isDecodedTo16Bits = 1u << 31;
    uint32_t numberOfLayouts;
    uint32_t layoutsOffset;
    uint32_t numberOfAllocations;
    uint32_t allocationsOffset;
    uint32_t typesOffset;
    uint32_t numberOfTypes;
    uint32_t numberOfLayoutIndices;
    uint32_t classesOffset;
    uint32_t numberOfConstructors;
    uint32_t constructorsOffset;
    uint32_t numberOfBoundFunctions;
    uint32_t boundFunctionsOffset;
    uint32_t numberOfStructureDescriptors;
    uint32_t structureDescriptorsOffset;
    uint32_t firstThisStructureDescriptor;
    uint32_t numberOfStructureTransitions;
    uint32_t structureTransitionsOffset;
    uint32_t nameDisplacementsOffset;
    uint32_t offsetsByNameAndStructureOffset;
    static constexpr uint32_t nameIsOnObjectPrototype = 1u << 31;
    uint32_t numberOfEagerBuiltinCells;
    uint32_t eagerBuiltinCellsOffset;
    uint32_t shapeSetsOffset;
    uint32_t sizeOfStatics;
    uint32_t sizeOfLazilyInitializedStatics;
    uint32_t numberOfStaticsRuns;
    uint32_t staticsRunsOffset;
    uint32_t numberOfGlobalPropertyCaches;
    uint32_t globalPropertyCachesOffset;
    uint32_t staticFieldSlotsOffset;
    uint32_t numberOfRoots;
    uint32_t rootsOffset;
    static constexpr unsigned cardShift = 8;
    uint32_t numberOfCards;
    uint32_t rootMasksOffset;
    uint32_t copyOnWriteButterfliesOffset;
    uint32_t arrayLiteralsOffset;
    static constexpr uint64_t encodedStringConstant(uint32_t string) { return (static_cast<uint64_t>(string) + 1) << 3; }
    static constexpr uint32_t decodeStringConstant(uint64_t encoded) { return static_cast<uint32_t>(encoded >> 3) - 1; }
    static constexpr uint64_t encodedStringIsSmall = 1;
    static constexpr uint64_t encodedStringIsCached = 4;
    static constexpr uint64_t encodedSmallString(uint8_t characterOrZero) { return static_cast<uint64_t>(characterOrZero) << 3 | encodedStringIsSmall; }
    uint32_t numberOfFunctions;
    uint32_t functionsOffset; // For each, the number of the string that is its name.
    uint32_t functionKindsOffset; // For each a byte: its Format::FunctionKind, with functionIsAsync and functionIsGenerator.
    uint32_t functionLengthsOffset; // For each from firstClosureFunction to endOfClosureFunctions, its `length` property: in 8 bits where functionLengthsAreBytes, else in 16. The offset is of where that of function 0 would stand.
    uint32_t moduleEvaluationsOffset;
    uint32_t slimClosuresOffset;
    uint32_t givenNamesOffset;
    uint32_t firstClosureFunction;
    uint32_t endOfClosureFunctions;
    uint32_t omittedClosureWordCount;
    uint32_t closureCountsOffset; // For each from firstClosureFunction to endOfClosureFunctions two bytes: of its closures, how many words the collector follows, which come first, and how many words there are. The offset is of where those of function 0 would stand.
    uint32_t numberOfEntryPoints;
    uint32_t entryPointsOffset;
    uint32_t numberOfBuiltinModules;
    uint32_t builtinModulesOffset;
    uint32_t numberOfBuiltinFunctions;
    uint32_t builtinFunctionsOffset;
    uint32_t preludeFunctionsOffset;
    static constexpr uint32_t generatorTakesCalleeClosure = 1u << 31;
    uint32_t numberOfGeneratorFunctions;
    uint32_t generatorFunctionsOffset;
    uint32_t numberOfAsyncGeneratorBodies;
    uint32_t asyncGeneratorBodiesOffset;
    uint32_t numberOfSloppyPlainFunctions;
    uint32_t sloppyPlainFunctionsOffset;
    uint32_t numberOfConstructorFunctions;
    uint32_t constructorFunctionsOffset;
    uint32_t numberOfRejectedFreshObjects;
    uint32_t rejectedFreshObjectsOffset;
    uint32_t numberOfFunctionsWithoutFieldNamesOnPrototype;
    uint32_t functionsWithoutFieldNamesOnPrototypeOffset;
    static constexpr uint32_t propertyCountIncludesPrototypeChain = 1u << 31;
    static constexpr unsigned propertyCountBits = 12;
    static constexpr uint32_t propertyCountMask = (1u << propertyCountBits) - 1;
    uint32_t numberOfThisPropertyCounts;
    uint32_t thisPropertyCountsOffset;
    uint32_t numberOfReservedFieldNames;
    uint32_t reservedFieldNamesOffset;
    uint32_t numberOfFieldNames;
    uint32_t fieldNamesOffset;
    uint32_t isCompiledForAudit;
    uint32_t logsTypeErrors;
    uint32_t mayRunInMultipleInstances;
    uint32_t numberOfSiteWords;
    uint32_t sitesOffset;
    uint32_t countsAllocations;
    uint32_t usesWideReferences;
    uint32_t functionLengthsAreBytes;
    uint32_t lastShapeWithOtherPrototype;
    uint32_t reachOfShapesWithOtherPrototype;
    uint32_t setOfShapesWithOtherPrototype;
    uint32_t numberOfFunctionAliases;
    uint32_t numberOfFunctionAliasRuns;
    uint32_t functionAliasesOffset;
    uint32_t firstGenericEntrypoint;
    uint32_t numberOfConsecutiveGenericEntrypoints;
    uint32_t functionSourcesOffset;
    uint32_t functionTextsOffset;
    uint32_t numberOfObjectCreators;
    uint32_t objectCreatorsOffset;
    static constexpr uint32_t createsObjectsOfFlaggedLayout = 1;
    static constexpr uint32_t createsObjectsWithDefinitelyInitializedFields = 2;
    uint32_t numberOfAllocationsByConstructor;
    uint32_t allocationsByConstructorOffset;
    uint32_t stringFactsOffset;
    uint32_t numberOfIdentifierStrings;
    uint32_t identifierStringsOffset;
    uint32_t identifierHashesOffset;
    uint32_t numberOfLiteralStrings;
    uint32_t literalStringsOffset;
    uint32_t literalStringTableOffset;
    uint32_t literalStringTableMask;
    uint32_t maxLiteralStringLength;
    static constexpr unsigned literalStringTableIndexBits = 19;
    static constexpr uint32_t literalStringTableEntry(uint32_t index, uint32_t hash) { return (index + 1) | hash >> 11 << literalStringTableIndexBits; }
    uint32_t numberOfHashedStrings;
    uint32_t hashedStringsOffset;
    uint32_t formatPatternsOffset;
    uint32_t icuStampOfFormatPatternsOffset;
    static constexpr uint32_t stringHashMask = (1u << 24) - 1;
    static constexpr uint32_t stringIsASCII = 1u << 24;
    static constexpr uint32_t stringIsPrivateName = 1u << 25;
    static constexpr uint32_t stringIsEnginePrivateName = 1u << 26;
    static constexpr uint32_t stringHasIdentifierNumber = 1u << 27;
    static constexpr uint32_t stringStartsWithDigit = 1u << 28;
    static constexpr uint32_t stringIsConstantSymbol = 1u << 29;

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    uint32_t offsetOfClass(uint32_t index) const
    {
        const uint32_t* groups = at<uint32_t>(classesOffset);
        uint32_t members = groups[index / 32 * 2];
        uint32_t bit = 1u << index % 32;
        if (!(members & bit))
            return 0;
        return groups[(numberOfLayoutIndices + 31) / 32 * 2 + groups[index / 32 * 2 + 1] + std::popcount(members & (bit - 1))];
    }
    uint32_t functionLengthOf(uint32_t function) const { return functionLengthsAreBytes ? at<uint8_t>(functionLengthsOffset)[function] : at<uint16_t>(functionLengthsOffset)[function]; }
    std::span<const uint32_t> row(uint32_t number) const
    {
        const uint32_t* starts = at<uint32_t>(structureDescriptorsOffset);
        return { starts + starts[number], starts[number + 1] - starts[number] };
    }
    bool isEntryPoint(uint32_t moduleEntryOffset) const { return std::ranges::binary_search(std::span { at<uint32_t>(entryPointsOffset), numberOfEntryPoints }, moduleEntryOffset); }
    std::array<uint32_t, 2> stringOffsetAndSize(uint32_t index) const
    {
        uint32_t word = at<uint32_t>(stringsOffset)[index];
        if (uint32_t size = word & ((1u << stringLengthBits) - 1)) [[likely]]
            return { word >> stringLengthBits, size };
        return at<std::array<uint32_t, 2>>(listedStringsOffset)[word >> stringLengthBits];
    }
    std::span<const uint8_t> string(uint32_t index) const
    {
        auto [offset, size] = stringOffsetAndSize(index);
        if (!offset) [[unlikely]]
            return encodedBytesOfDecodedString(index);
        return { at<uint8_t>(offset), size };
    }
    std::span<const uint8_t> stringInImage(uint32_t index) const
    {
        auto [offset, size] = stringOffsetAndSize(index);
        return { at<uint8_t>(offset), size };
    }
    bool hasOnlyDecodedForm(uint32_t index) const { return !stringOffsetAndSize(index)[0]; }
    const HashedString* hashedString(uint32_t index) const
    {
        std::span allHashedStrings { at<HashedString>(hashedStringsOffset), numberOfHashedStrings };
        auto found = std::ranges::lower_bound(allHashedStrings, index, { }, &HashedString::string);
        return found == allHashedStrings.end() || found->string != index ? nullptr : &*found;
    }
    uint32_t stringSize(uint32_t index) const { return stringOffsetAndSize(index)[1]; }
    JS_EXPORT_PRIVATE std::span<const uint8_t> encodedBytesOfDecodedString(uint32_t index) const;
    bool isAtomString(uint32_t index) const { return at<uint32_t>(atomStringsOffset)[index / 32] >> index % 32 & 1; }
    uint32_t stringFacts(uint32_t index) const { return at<uint32_t>(stringFactsOffset)[index]; }
    uint32_t identifierStringPlusOne(uint32_t identifier) const { return identifier < numberOfIdentifierStrings ? at<uint32_t>(identifierStringsOffset)[identifier] : 0; }
    uint32_t identifierHash(uint32_t identifier) const { return at<uint32_t>(identifierHashesOffset)[identifier]; }
    uint32_t literalStringID(uint32_t number) const { return at<uint32_t>(literalStringsOffset)[number] - 1; }
};

static constexpr uint32_t offsetOfEnvironmentShapeLowByte = 4;
static constexpr uint32_t offsetOfEnvironmentShapeHighByte = 6;
inline const LayoutData& environmentLayout(const Data& data, uint32_t firstShape, const void* cell)
{
    auto* header = static_cast<const uint8_t*>(cell);
    uint32_t shape = firstShape + header[offsetOfEnvironmentShapeLowByte] + (header[offsetOfEnvironmentShapeHighByte] << 8);
    return *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
