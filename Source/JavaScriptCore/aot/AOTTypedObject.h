/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedData.h"
#include "AOTTypedFormat.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "JSObject.h"
#include "JSScope.h"
#include "MarkedBlockCage.h"
#include "NativeExecutable.h"
#include "Strong.h"
#include "ThrowScope.h"
#include "Weak.h"
#include "WeakGCHashTable.h"
#include "WeakGCMap.h"
#include <wtf/CompactPointerTuple.h>
#include <wtf/MallocSpan.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

class AbstractModuleRecord;
class ArgList;
class CustomGetterSetter;
class HeapAnalyzer;
class JSPromise;
class JSPromiseCombinatorsGlobalContext;
class StackFrame;
class JSSlimPromiseReaction;

namespace AOT {

struct Instance;
struct ModuleEvaluation;
class GlobalPropertyWatchpoint;

namespace Typed {

class Extension;
struct EngineClass;
struct ExtraPropertiesHandle;
struct State;
struct PropertyListEntry {
    UniquedStringImpl* name;
    const FieldData* field;
    PropertyOffset offsetInExtraProperties;
};
struct CachedPropertyList {
    StructureID extraPropertiesStructureID;
    uint32_t shapeWord { 0 };
    uint32_t orderAndCount { 0 };
    bool didLookUpPlainCopyStructure { false };
    JSCellButterfly* keys { nullptr };
    Structure* plainCopyStructure { nullptr };
    Vector<PropertyListEntry> properties;
};
struct TransitionPlan;

static constexpr uint32_t keysAreInLayoutOrder = 0xffff;
static constexpr uint16_t noSlotInCell = 0;
static constexpr uint32_t orderAndCountMask = orderWordHasExtraProperties - 1;

class Object final : public JSObject {
public:
    using Base = JSObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, Instance&, JSValue prototype, const LayoutData*);
    static Object* create(VM&, Structure*, const LayoutData&, size_t size, uint32_t orderWord);
    static Object* createWithBody(VM&, Structure*, std::span<const uint8_t> body);
    static Object* createWithInitializedFields(VM&, Structure*, const LayoutData&, size_t size, uint32_t orderWord);
    Object* clone(JSGlobalObject*);
    Object* tryCloneWithPrototype(JSGlobalObject*, JSValue prototype);
    void adoptPrototypeAtBirth(JSGlobalObject*, JSObject* prototype, uint32_t allocation);
    void leaveInlinePaths() { at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties; }
    Object* copyWithStructure(VM&, Structure*, JSFinalObject* copiedExtraProperties);
    JSObject* tryCopyAsPlainObject(JSGlobalObject*) const;
    Structure* plainCopyStructure(JSGlobalObject*) const;
    JSFinalObject* tryCopyAsPlainObjectWithCapacity(JSGlobalObject*, Structure* plainCopyPrefixStructure) const;
    JSObject* tryCopyAsPlainObject(JSGlobalObject*, std::span<const EncodedJSValue> excluded) const;
    Object* tryCopyAs(JSGlobalObject*, const LayoutData& to, CopyNamesKind, std::span<const EncodedJSValue> names, bool* neverCopyable = nullptr);
    static Object* tryMergeAs(JSGlobalObject*, const LayoutData& to, Object& first, Object& second);
    static Object* tryCreateAs(JSGlobalObject*, State&, const LayoutData& to, std::span<const EncodedJSValue> namesToStoreNext);
    bool tryCopyFieldsOf(VM&, const Object& source);
    bool tryAssignPropertiesOf(VM&, JSObject* source);

    const LayoutData& layout() const;
    State& state() const;
    JSValue read(const FieldData&) const;
    JSValue readInline(const FieldData&) const;
    enum class ReadKind : uint32_t { Int32OrNumberBox, Cell32, Double, Boolean, JSValue, Int32, General };
    static constexpr unsigned readKindShift = 24;
    static uint32_t readKindAndOffset(const FieldData&);
    JSValue readInline(uint32_t readKindAndOffset, const Invocable<const FieldData&()> auto& field) const;
    static JSValue tryGetOwnData(VM&, JSCell*, PropertyName);
    bool isDiscriminant(const FieldData&) const;
    void positionExtraKey(State&, PropertyName, uint32_t position);
    void positionExtraKeys(State&, std::span<const std::pair<UniquedStringImpl*, uint32_t>> namesAndPositions);
    void positionExtraKeys(State&);
    bool write(VM&, const FieldData&, JSValue);
    bool assign(JSGlobalObject*, const FieldData&, JSValue, bool* givesUp = nullptr);
    JSObject* convertToOrdinaryObject(VM&, JSValue prototype = { });
    JSFinalObject* copyAsOrdinaryObject(VM&, unsigned inlineCapacity, JSValue prototype = { });
    void storeInNumberBox(VM&, const FieldData&, JSValue);

    uint32_t orderWord() const
    {
        uint32_t word = at<uint32_t>(offsetOfOrderWord);
        return word & orderWordHasExtraProperties ? orderWordSlow(word) : word;
    }
    uint64_t orderAndShapeWords() const { return static_cast<uint64_t>(at<uint32_t>(offsetOfShapeWord)) << 32 | orderWord(); }
    uint32_t shape() const { return shapeOf(at<uint32_t>(offsetOfShapeWord)); }
    bool isFrozen() const;
    bool mayRefuseStores() const;
    bool hasFlagsInOrderWord() const { return at<uint32_t>(offsetOfOrderWord) & ~orderAndCountMask; }
    unsigned attributesOf(const FieldData& field) const
    {
        uint32_t word = at<uint32_t>(offsetOfOrderWord);
        if (word & orderWordIsFrozen) [[unlikely]]
            return attributesOfSlow(field);
        return static_cast<unsigned>(word & orderWordIsSealed ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    }
    unsigned attributesOfSlow(const FieldData&) const;
    void setAttributesOf(VM&, const FieldData&, unsigned);
    bool isSealed() const { return at<uint32_t>(offsetOfOrderWord) & orderWordIsSealed; }
    bool isNotExtensible() const { return at<uint32_t>(offsetOfOrderWord) & orderWordIsNotExtensible; }
    bool hasExtraProperties() const { return at<uint32_t>(offsetOfOrderWord) & orderWordHasExtraProperties; }
    static bool structureGuaranteesAbsence(Structure*, UniquedStringImpl*);
    struct FieldAccess {
        uint32_t offset;
        Store store;
        bool isOutOfLine;
        bool mayBeAbsent;
        uint16_t acceptedPrimitives { 0 };
        bool acceptsInt32 { false };
        bool acceptsAnyObject { false };
        uint32_t typeOfAcceptedObjects { 0 };
    };
    std::optional<FieldAccess> fieldAccessFor(VM&, PropertyName, bool acceptsAnyMatchingValue = false);
    enum class Integrity : uint8_t { NotExtensible, Sealed, Frozen };
    bool setIntegrity(JSGlobalObject*, Integrity);
    JSObject* extraProperties() const { return hasExtraProperties() ? extraPropertiesSlow() : nullptr; }
    JSObject* extraPropertiesSlow() const;
    void setExtraProperties(VM&, JSObject*, bool isForLookupTable = false);
    void setExtraProperties(VM&, JSObject*, Structure* withExtraProperties);
    struct ExtraProperty {
        UniquedStringImpl* name;
        JSValue value;
    };
    static JSObject* createExtraProperties(JSGlobalObject* realm, std::span<const ExtraProperty> inOrder);
    void initializeExtraProperties(VM&, std::span<const ExtraProperty> inOrder);
    void didChangeExtraProperties(VM&);
    void specializeStructureForExtraProperties(VM&);
    static bool specializeStructureForExtraPropertiesIfNeeded(VM& vm, JSCell* cell)
    {
        Structure* structure = cell->structure();
        if (!structure->compiledLayout() || !(structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties) || structure->extraPropertiesStructureID()) [[likely]]
            return false;
        uncheckedDowncast<Object>(cell)->specializeStructureForExtraProperties(vm);
        return cell->structure() != structure;
    }
    static std::optional<PropertyOffset> offsetOfExtraProperty(VM&, JSCell*, PropertyName, unsigned& attributes);
    enum class ExtraPropertyStore : uint8_t { NotHandled, Replaced, Added };
    ExtraPropertyStore tryStoreExtraProperty(VM&, PropertyName, JSValue, PropertyOffset&, bool isKnownNotToBeField = false, bool isDefinition = false, bool* hasDefaultAttributes = nullptr, bool isForLookupTable = false);
    static bool putWithKnownField(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&, const FieldData*);
    bool mayDefineAccessorOnExtraProperties(VM&, State&, PropertyName, JSObject*& extra);
    const FieldData* fieldNamed(State&, PropertyName) const;
    enum class SymbolKeys : bool { Skip, Include };
    bool forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>&, SymbolKeys = SymbolKeys::Skip) const;
    bool forEachFieldAndExtraProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>&) const;
    JSCellButterfly* keys(VM&) const;
    JSCellButterfly* keysWithExtraProperties(VM&) const;
    JSArray* tryCreateKeysArrayWithExtraProperties(JSGlobalObject*) const;
    bool appendPlainCopyValues(VM&, Vector<JSValue, 16>&) const;
    void didCreateField(State&, const FieldData&, bool withStorage = true);
    bool isInKeyOrder(State&, const FieldData&) const;
    static bool didCreateOneField(uint32_t orderWordBefore, uint32_t orderWordAfter)
    {
        uint32_t before = countOf(orderWordBefore);
        uint32_t after = countOf(orderWordAfter);
        return orderWordBefore & orderWordHasExtraProperties ? after > before : after == before + 1;
    }
    void listFieldsDefinedSoFar(State&, const FieldData&, bool includesField, bool mayUnlist);
    void listFieldsBelow(State&, uint32_t end, bool mayUnlist);
    void listFieldStoredEarly(State&, const FieldData&);
    bool isNotDefinedYet(const FieldData&, JSValue held) const;
    JSValue materializeLazyClosure(const FieldData&);
    bool holdsUnmaterializedClosure(const FieldData& field) const { return field.flags & fieldHoldsLazyClosure && at<uint32_t>(field.offset) == unmaterializedClosureCell32; }
    void materializeLazyClosures();
    void willKeepKeysInLayoutOrder();
    void ensureStorageFor(VM& vm, State& state, const FieldData& field) { ensureExtensionCovers(vm, state, field); }
    void ensureStorageOfSize(VM&, State&, uint32_t size);
    JSValue getDirectFromNativeCode(VM&, PropertyName, unsigned* attributes);
    bool putDirectFromNativeCode(VM&, PropertyName, JSValue, unsigned attributes);
    void appendField(State&, const FieldData&);
    void didMoveExtraPropertyToField(State&, const FieldData&);
    void willCreateField(State&, const FieldData&);
    void putExtraWithoutRunningCode(JSGlobalObject*, PropertyName, JSValue);
    void setPrototypeAtCreation(VM&, JSObject*);
    void updateNeedsSlowLookup(State&, bool isCreation = false);

    struct PrivateElement {
        enum class Kind : uint8_t { None, Field, Method, Accessor };
        Kind kind { Kind::None };
        JSValue value;
        JSObject* getter { nullptr };
        JSObject* setter { nullptr };
    };
    static PrivateElement findPrivateElement(VM&, JSObject*, PropertyName);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static size_t estimatedSize(JSCell*, VM&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool isExtensible(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

private:
    friend class AsyncGenerator;
    friend class Coercion;
    friend class Generator;
    friend struct State;

    Object(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    template<typename T> T& at(uint32_t offset) const { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<Object*>(this)) + offset); }

    uint16_t indexOf(const FieldData& field) const { return &field - layout().fields(); }
    Extension* extension() const;
    void setExtension(Extension*);
    void* slotOf(const FieldData&) const;
    void ensureExtensionCovers(VM&, State&, const FieldData&);
    void replaceExtension(VM&, State&, uint32_t size);
    static double* numberBoxAt(int32_t word);
    void storeInOwnNumberBox(State&, uint32_t offset, double);
    void copyNumberBoxes(State&);
    bool store(VM&, const FieldData&, JSValue);
    void clear(const FieldData&);
    void setOrderAndCount(uint32_t orderAndCount)
    {
        if (orderOf(orderAndCount) == keysAreInLayoutOrder && orderOf(orderWord()) != keysAreInLayoutOrder) [[unlikely]]
            willKeepKeysInLayoutOrder();
        if (ExtraPropertiesHandle* handle = extraPropertiesHandle())
            setOrderAndCount(handle, orderAndCount);
        else
            at<uint32_t>(offsetOfOrderWord) = (at<uint32_t>(offsetOfOrderWord) & ~orderAndCountMask) | orderAndCount;
    }
    uint32_t orderWordSlow(uint32_t word) const;
    ExtraPropertiesHandle* extraPropertiesHandle() const;
    static void setOrderAndCount(ExtraPropertiesHandle*, uint32_t orderAndCount);
    void forEachKeyInOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    void forEachListedKey(State&, const Invocable<void(uint16_t)> auto&) const;
    CachedPropertyList* cachedPropertyList(VM&) const;
    JSValue readPropertyListEntry(const PropertyListEntry&, JSObject* extra) const;
    JSFinalObject* tryCopyUsingPropertyList(VM&, Structure*) const;
    bool keysDependOnlyOnStructureAndOrderWord() const;
    void forEachFieldInKeyOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    Vector<UniquedStringImpl*, 16> extraKeyNames() const;
    std::optional<uint16_t> extraKeyOrdinal(PropertyName) const;
    bool hasPositionAmongKeys(UniquedStringImpl*) const;
    void didRemoveExtraKey(State&, uint16_t ordinal, const FieldData* replacementField = nullptr);
    JSObject* ensureExtraProperties(VM&);
    JSFinalObject* tryCopyExtraPropertiesForClone(VM&) const;
    void willAddExtraProperty(VM&, PropertyName);
    void dropExtraPropertiesIfEmpty(VM&);
    void specialize(VM&, State&, uint32_t shape);
    bool specialize(VM&, State&, TransitionPlan&);
    bool specializeWithPlan(VM&, const TransitionPlan&);
};

ALWAYS_INLINE JSValue Object::readInline(const FieldData& field) const
{
    if (field.flags & (fieldIsOutOfLine | fieldMayBeUnlisted)) [[unlikely]]
        return read(field);
    switch (field.store()) {
    case Store::Cell32:
    case Store::OptionalCell32: {
        uint32_t word = at<uint32_t>(field.offset);
        if (word == unmaterializedClosureCell32) [[unlikely]]
            return read(field);
        return word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(word));
    }
    case Store::Int32OrNumberBox:
        if (int32_t word = at<int32_t>(field.offset); word >= minInlineInt32) [[likely]]
            return jsNumber(word);
        return read(field);
    case Store::Boolean:
        return jsBoolean(at<uint8_t>(field.offset));
    case Store::Cell:
    case Store::JSValue:
        return JSValue::decode(at<EncodedJSValue>(field.offset));
    case Store::Int32:
        return jsNumber(at<int32_t>(field.offset));
    case Store::Double:
        return jsNumber(purifyNaN(at<double>(field.offset)));
    default:
        return read(field);
    }
}

inline uint32_t Object::readKindAndOffset(const FieldData& field)
{
    auto kind = [&] {
        if (field.flags & (fieldIsOutOfLine | fieldMayBeUnlisted))
            return ReadKind::General;
        switch (field.store()) {
        case Store::Int32OrNumberBox:
            return ReadKind::Int32OrNumberBox;
        case Store::Cell32:
        case Store::OptionalCell32:
            return ReadKind::Cell32;
        case Store::Double:
            return ReadKind::Double;
        case Store::Boolean:
            return ReadKind::Boolean;
        case Store::Cell:
        case Store::JSValue:
            return ReadKind::JSValue;
        case Store::Int32:
            return ReadKind::Int32;
        default:
            return ReadKind::General;
        }
    }();
    return static_cast<uint32_t>(kind) << readKindShift | field.offset;
}

ALWAYS_INLINE JSValue Object::readInline(uint32_t readKindAndOffset, const Invocable<const FieldData&()> auto& field) const
{
    constexpr auto first = [](ReadKind kind) { return static_cast<uint32_t>(kind) << readKindShift; };
    static_assert(!first(ReadKind::Int32OrNumberBox));
    if (readKindAndOffset < first(ReadKind::Cell32)) {
        if (int32_t word = at<int32_t>(readKindAndOffset); word >= minInlineInt32) [[likely]]
            return jsNumber(word);
        return read(field());
    }
    if (readKindAndOffset < first(ReadKind::Double)) {
        uint32_t word = at<uint32_t>(readKindAndOffset - first(ReadKind::Cell32));
        if (word == unmaterializedClosureCell32) [[unlikely]]
            return read(field());
        return word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(word));
    }
    if (readKindAndOffset < first(ReadKind::Boolean))
        return jsNumber(purifyNaN(at<double>(readKindAndOffset - first(ReadKind::Double))));
    if (readKindAndOffset < first(ReadKind::JSValue))
        return jsBoolean(at<uint8_t>(readKindAndOffset - first(ReadKind::Boolean)));
    if (readKindAndOffset < first(ReadKind::Int32))
        return JSValue::decode(at<EncodedJSValue>(readKindAndOffset - first(ReadKind::JSValue)));
    if (readKindAndOffset < first(ReadKind::General))
        return jsNumber(at<int32_t>(readKindAndOffset - first(ReadKind::Int32)));
    return read(field());
}

struct DiscriminatingField {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DiscriminatingField);
    const FieldData* field { nullptr };
    UncheckedKeyHashMap<String, uint32_t> strings;
    Vector<const HashedString*> hashedStrings;
    UncheckedKeyHashSet<EncodedJSValue> numbers;
};

struct TransitionPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TransitionPlan);
    struct Slot {
        uint32_t offset;
        Store store;
    };
    Structure* structure { nullptr };
    uint32_t shapeWord { 0 };
    uint32_t size { 0 };
    const LayoutData* from { nullptr };
    const LayoutData* to { nullptr };
    Vector<Slot, 4> addedSlots;
    static constexpr unsigned maxOrderWords = 8;
    Vector<std::pair<uint32_t, uint32_t>, 2> orderWords;
};

struct StoreTransition {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StoreTransition);
    static constexpr unsigned maxHolders = 4;
    struct Cleared {
        uint32_t offset;
        uint32_t width;
        uint64_t absent;
    };
    uint64_t wordsBefore;
    uint64_t wordsAfter;
    uint64_t fieldWrite;
    JSCell* holders[maxHolders];
    uint32_t holderStructureIDs[maxHolders];
    uint32_t structureID;
    uint32_t size;
    uint32_t clearedSlotCount;
    const Cleared* cleared;
    Vector<Cleared, 4> clearedSlots;
    uint32_t extensionSize { 0 };
    uint32_t sizeIfStorageExists { 0 };
    uint32_t structureIDIfFieldIsInline { 0 };
    void initializeFastChecks();
};
static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && !(offsetOfOrderWord % sizeof(uint64_t)));

struct alignas(64) CastTransition {
    static constexpr unsigned indexBits = 7;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint32_t typeAndReason, uint64_t wordsBefore) { return ((structureID ^ typeAndReason ^ static_cast<uint32_t>(wordsBefore)) * multiplier) >> (32 - indexBits); }
    uint32_t structureID;
    uint32_t typeAndReason;
    uint64_t wordsBefore;
    uint64_t wordsAfter;
    uint32_t structureIDAfter;
    uint32_t size;
    const StoreTransition::Cleared* cleared;
    uint32_t numberOfEightByteSlots;
    uint32_t numberOfFourByteSlots;
    uint32_t numberOfOneByteSlots;
};
static_assert(sizeof(CastTransition) == 64);

struct KnownStoreTransition {
    static constexpr unsigned indexBits = 8;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint32_t name, uint32_t orderWord) { return ((structureID ^ name ^ orderWord) * multiplier) >> (32 - indexBits); }
    uint32_t structureID;
    uint32_t name;
    const StoreTransition* transition;
};
static_assert(sizeof(KnownStoreTransition) == 16);

class Extension {
public:
    static Extension* create(VM&, uint32_t size);

    uint32_t size() const { return m_size; }
    void* slot(uint32_t offset) { return reinterpret_cast<uint8_t*>(this) + offset; }
    std::span<uint8_t> slots() { return { static_cast<uint8_t*>(slot(offsetOfFirstExtensionSlot)), m_size - offsetOfFirstExtensionSlot }; }
    static constexpr ptrdiff_t offsetOfSize() { return OBJECT_OFFSETOF(Extension, m_size); }

private:
    uint32_t m_size;
};
static_assert(static_cast<uint32_t>(Extension::offsetOfSize()) == offsetOfExtensionSize && sizeof(Extension) == offsetOfFirstExtensionSlot);

inline Extension* Object::extension() const
{
    uint32_t word = at<uint32_t>(offsetOfExtension);
    return word ? std::bit_cast<Extension*>(MarkedBlockCage::decode(word)) : nullptr;
}

inline void Object::setExtension(Extension* extension)
{
    at<uint32_t>(offsetOfExtension) = extension ? MarkedBlockCage::encode(std::bit_cast<const JSCell*>(extension)) : 0;
}

struct ExtraPropertiesHandle {
    const JSCell* object;
    JSObject* extraProperties;
    uint32_t orderAndCount;
    uint32_t nextFree;
};
static_assert(sizeof(ExtraPropertiesHandle) == 24);

class ExtraPropertiesHandleTable : public RefCounted<ExtraPropertiesHandleTable> {
    WTF_MAKE_NONCOPYABLE(ExtraPropertiesHandleTable);
public:
    static Ref<ExtraPropertiesHandleTable> of(VM&, Instance&);
    ~ExtraPropertiesHandleTable();

    static constexpr unsigned segmentIndexBits = 14;
    static constexpr uint32_t handlesPerSegment = 1 << segmentIndexBits;

    ExtraPropertiesHandle* of(const JSCell* object, uint32_t number) const { return number ? numbered(number) : m_unnumberedHandles.get(object); }
    JSObject* extraPropertiesConcurrently(const JSCell* object, uint32_t number);
    uint32_t take(const JSCell* object, JSObject* extraProperties, uint32_t orderAndCount);
    void release(const JSCell* object, uint32_t number);
    void didReplaceObject(const JSCell* object, uint32_t number, const JSCell* itself);
    void releaseHandlesOfDeadObjects(VM&);

private:
    static constexpr uint32_t handleGrowthIncrement = 1 << 7;

    explicit ExtraPropertiesHandleTable(VM&);

    ExtraPropertiesHandle* numbered(uint32_t number) const { return m_segments[number >> segmentIndexBits] + (number & (handlesPerSegment - 1)); }
    bool hasFreeHandle() const { return m_end && m_segments[0]->nextFree; }
    bool tryGrow();

    VM& m_vm;
    ExtraPropertiesHandle** m_segments { nullptr };
    uint32_t m_end { 0 };
    Lock m_lock;
    UncheckedKeyHashMap<const JSCell*, std::unique_ptr<ExtraPropertiesHandle>> m_unnumberedHandles;
};

struct ExtraPropertyLocation {
    static constexpr unsigned indexBits = 8;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint32_t name, uint32_t shapeWord) { return ((structureID ^ name ^ shapeWord) * multiplier) >> (32 - indexBits); }
    uint32_t structureID;
    uint32_t name;
    uint32_t shapeWord;
    int32_t displacement;
};
static_assert(sizeof(ExtraPropertyLocation) == 16);

struct TypedCloneMove {
    enum class Kind : uint8_t { Value, NumberAsDouble, NumberAsInlineInt32, StringAsCell32, NullableStringAsCell32, BooleanAsByte, General };
    uint16_t from;
    uint16_t to;
    Kind kind;
    uint8_t acceptedKinds;
    Store store;
    uint8_t unused;
};
static_assert(sizeof(TypedCloneMove) == 8);

struct TypedClonePlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TypedClonePlan);
    enum class Path : uint32_t { PlainClone, Runtime, Stub };
    Path path { Path::PlainClone };
    uint32_t moveCount { 0 };
    uint32_t bodyWordCount { 0 };
    void* allocator { nullptr };
    uint64_t header { 0 };
    const uint64_t* bodyWords { nullptr };
    const TypedCloneMove* firstMove { nullptr };
    Structure* structure { nullptr };
    Vector<TypedCloneMove, 8> moves;
    Vector<const FieldData*, 8> fields;
    Vector<uint64_t, 8> body;
};

struct TypedCloneTarget {
    static constexpr unsigned shapeBits = 20;
    static constexpr unsigned fieldBits = 6;
    static constexpr unsigned maxStoredFields = 4;
    static constexpr uint32_t shapeOf(uint64_t target) { return target & ((1u << shapeBits) - 1); }
    static constexpr uint32_t storedFieldPlusOne(uint64_t target, unsigned i) { return target >> (shapeBits + fieldBits * i) & ((1u << fieldBits) - 1); }
    static constexpr uint64_t withStoredField(uint64_t target, unsigned i, uint32_t field) { return target | static_cast<uint64_t>(field + 1) << (shapeBits + fieldBits * i); }
};

struct alignas(32) TypedCloneCacheEntry {
    static constexpr unsigned indexBits = 7;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint64_t target) { return ((structureID ^ static_cast<uint32_t>(target) ^ static_cast<uint32_t>(target >> 32)) * multiplier) >> (32 - indexBits); }
    uint64_t target;
    const TypedClonePlan* plan;
    uint32_t structureID;
};
static_assert(sizeof(TypedCloneCacheEntry) == 32);

struct ConversionMove {
    enum class Kind : uint8_t { Value, CellOfShape, CellOfType, Function, Object, NumberAsInlineInt32, NumberAsDouble, BooleanAsByte, BooleanAsValueByte, LiteralString, SwitchSource };
    static constexpr uint16_t outOfLineBias = 2048;
    static constexpr uint8_t acceptsUndefined = 1;
    static constexpr uint8_t acceptsNull = 2;
    static constexpr uint8_t isWide = 4;
    static constexpr uint32_t reasonOfChild = 15;
    static constexpr uint32_t maxDepthOfChildren = 4;
    uint16_t from;
    uint16_t to;
    Kind kind;
    uint8_t flags;
    uint8_t cellType;
    uint8_t isOutOfLine;
    uint32_t lastShape;
    uint32_t shapeCount;
    uint32_t keyOfChild { 0 };
};
static_assert(sizeof(ConversionMove) == 20);

struct ConversionText {
    uint64_t text;
    uint64_t meaning;
};
static_assert(sizeof(ConversionText) == 16);

struct ConversionPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ConversionPlan);
    static constexpr size_t maxCellSize = 128;
    static constexpr unsigned maxTextCount = 16;
    const ConversionText* firstText { nullptr };
    uint32_t discriminantFrom { 0 };
    uint32_t memberCount { 0 };
    Vector<ConversionText, 4> texts;
    void* allocator { nullptr };
    const uint64_t* words { nullptr };
    const ConversionMove* firstMove { nullptr };
    uint32_t wordCount { 0 };
    uint32_t moveCount { 0 };
    void* allocatorOfExtraProperties { nullptr };
    const uint64_t* wordsOfExtraProperties { nullptr };
    const ConversionMove* firstMoveOfExtraProperties { nullptr };
    uint32_t wordCountOfExtraProperties { 0 };
    uint32_t moveCountOfExtraProperties { 0 };
    uint32_t offsetOfExtraProperties { 0 };
    uint32_t allocates { 0 };
    Vector<uint64_t, 8> cell;
    Vector<ConversionMove, 8> moves;
    Vector<uint64_t, 8> cellOfExtraProperties;
    Vector<ConversionMove, 2> movesOfExtraProperties;
};

struct alignas(16) ConversionCacheEntry {
    static constexpr unsigned indexBits = 8;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint32_t typeAndReason) { return ((structureID ^ typeAndReason) * multiplier) >> (32 - indexBits); }
    uint32_t structureID;
    uint32_t typeAndReason;
    const ConversionPlan* plan;
};
static_assert(sizeof(ConversionCacheEntry) == 16);

struct UnconvertedCastCheck {
    static constexpr uint32_t comparesStructureID = 1u << 8;
    static constexpr uint32_t isEnd = 1u << 9;
    const void* holder;
    uint32_t offset;
    uint32_t kinds;
    uint64_t expected;
};
static_assert(sizeof(UnconvertedCastCheck) == 24);

struct alignas(16) UnconvertedCastEntry {
    static constexpr unsigned indexBits = 8;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t structureID, uint32_t typeAndReason) { return ((structureID ^ typeAndReason) * multiplier) >> (32 - indexBits); }
    uint32_t structureID;
    uint32_t typeAndReason;
    const UnconvertedCastCheck* checks;
};
static_assert(sizeof(UnconvertedCastEntry) == 16);

struct alignas(32) LateFieldCreation {
    static constexpr unsigned indexBits = 10;
    static constexpr uint32_t multiplier = 0x9e3779b1;
    static constexpr unsigned placeOf(uint32_t extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCountBefore, uint64_t slotKeys) { return ((extraPropertiesStructureID ^ shapeWord ^ orderAndCountBefore ^ static_cast<uint32_t>(slotKeys)) * multiplier) >> (32 - indexBits); }
    uint64_t slotKeys;
    uint32_t extraPropertiesStructureID;
    uint32_t shapeWord;
    uint32_t orderAndCountBefore;
    uint32_t orderAndCountAfter;
};
static_assert(sizeof(LateFieldCreation) == 32);

class ExtraPropertiesTable final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(ExtraPropertiesTable);
public:
    explicit ExtraPropertiesTable(VM&);
    ~ExtraPropertiesTable() final;

    void set(Object*, JSObject*);
    void remove(Object*);
    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope) final;
    void forEachConcurrently(const Invocable<void(Object*, JSObject*)> auto& functor)
    {
        Locker locker { m_lock };
        for (auto& entry : m_map)
            functor(entry.key, entry.value);
    }

private:
    VM& m_vm;
    Lock m_lock;
    UncheckedKeyHashMap<Object*, JSObject*> m_map;
};

class NumberBoxTable final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(NumberBoxTable);
public:
    explicit NumberBoxTable(VM&);
    ~NumberBoxTable() final;

    static uintptr_t base();
    bool isEmpty() const { return m_map.isEmpty(); }
    int32_t wordFor(Object*, uint32_t offset);
    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope) final;

private:
    struct Box {
        uint32_t offset;
        uint32_t place;
    };
    VM& m_vm;
    UncheckedKeyHashMap<Object*, Vector<Box, 2>> m_map;
};

inline double* Object::numberBoxAt(int32_t word)
{
    return std::bit_cast<double*>(NumberBoxTable::base() + (static_cast<uintptr_t>(static_cast<uint32_t>(word) & INT32_MAX) << numberBoxShift));
}

class FieldAttributesTable final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(FieldAttributesTable);
public:
    struct Attributes {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Attributes);
        bool isFrozen;
        Vector<std::pair<uint32_t, uint8_t>, 2> attributesByField;
    };
    explicit FieldAttributesTable(VM&);
    ~FieldAttributesTable() final;

    Attributes* get(const Object* object) const { return m_map.get(const_cast<Object*>(object)); }
    Attributes& ensure(Object*);
    void remove(Object* object) { m_map.remove(object); }
    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope) final;

private:
    VM& m_vm;
    UncheckedKeyHashMap<Object*, std::unique_ptr<Attributes>> m_map;
};

class EvalScope final : public JSScope {
public:
    using Base = JSScope;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static EvalScope* create(VM&, Structure*, JSScope* next, JSObject* bindings);
    static Identifier newTargetKey(VM&);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

private:
    EvalScope(VM&, Structure*, JSScope* next, JSObject* bindings);

    WriteBarrier<JSObject> m_bindings;
};

class Closure final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.heap.aotClosureSpace; }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, bool isBound = false);
    static Closure* create(VM&, Structure*, uint32_t function, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    static Closure* createForCompiledCode(VM&, Structure*, size_t allocationSize);
    void setComputedName(VM&, const Identifier& key, Format::MethodKind);

    static constexpr unsigned maxNumberOfCaptures = UINT8_MAX;
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(Closure, m_entry); }
    static constexpr ptrdiff_t offsetOfCaptures() { return sizeof(Closure); }
    static constexpr size_t allocationSize(unsigned numberOfCaptures) { return sizeof(Closure) + numberOfCaptures * sizeof(uint64_t); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);

    JSValue toString(JSGlobalObject*) const;
    Format::FunctionKind kind() const;
    bool isDeclaredAs(uint8_t) const;
    bool sharesPrototypeProperty() const;
    bool createsFixedLayoutObjects() const;
    bool hasImmutablePrototypeProperty() const;
    bool mayReplacePrototypeProperty(JSGlobalObject*, JSValue, bool shouldThrow);
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), counts()[1] }; }
    unsigned numberOfReferences() const { return counts()[0]; }
    JSCell* environment() const { return numberOfReferences() ? std::bit_cast<JSCell*>(captures()[0]) : nullptr; }
    uint32_t functionIndex() const { return ClosureWord::functionOf(m_entry); }
    uint64_t entry() const { return m_entry; }
    bool mayBeCalled() const;
    friend struct State;
    const LayoutData* classData() const;
    bool isLazyClassObject() const;
    void ensureClassObject(VM&);
    void adoptBoundFunctionStructure(VM&);
    JSValue prototypeProperty(VM&);
    JSValue callAsConstructor(JSGlobalObject*, JSValue thisValue, JSValue newTarget, const ArgList&);

private:
    Closure(VM& vm, Structure* structure, NativeExecutable* executable, uint64_t entry)
        : Base(vm, executable, structure->realm(), structure)
        , m_entry(entry)
    {
    }

    bool ensurePrototypeProperty(VM&);
    std::array<uint8_t, 2> counts() const;

    uint64_t m_entry;
};
static_assert(sizeof(JSFunction) == 32 && Closure::offsetOfEntry() == 32 && sizeof(Closure) == 40);

class SlimClosure final : public JSObjectWithButterfly {
public:
    using Base = JSObjectWithButterfly;
    static constexpr unsigned ReifiedStructureFlags = Base::StructureFlags | OverridesGetCallData | TypeOfReturnsFunction | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr unsigned ReifiedConstructorStructureFlags = ReifiedStructureFlags | OverridesPut;
    static constexpr unsigned ReifiedBoundStructureFlags = ReifiedStructureFlags & ~ImplementsDefaultHasInstance;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | GetOwnPropertySlotIsImpureForPropertyAbsence | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | OverridesGetCallData | TypeOfReturnsFunction | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.heap.aotClosureSpace; }

    static Structure* createStructure(VM&, Instance&, JSValue prototype, bool hasProperties, bool constructs = false, bool isBound = false);
    static SlimClosure* create(VM&, Structure*, uint32_t function, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    static SlimClosure* createForCompiledCode(VM&, Structure*, size_t allocationSize);

    static constexpr ptrdiff_t offsetOfWord() { return butterflyOffset(); }
    static constexpr ptrdiff_t offsetOfFunctionIndexInButterfly() { return offsetInButterfly(firstOutOfLineOffset) * static_cast<ptrdiff_t>(sizeof(EncodedJSValue)); }
    static constexpr ptrdiff_t offsetOfCaptures() { return sizeof(SlimClosure); }
    static constexpr size_t allocationSize(unsigned numberOfCaptures) { return sizeof(SlimClosure) + numberOfCaptures * sizeof(uint64_t); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static void analyzeHeap(JSCell*, HeapAnalyzer&);
    static size_t estimatedSize(JSCell*, VM&);
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool isExtensible(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);

    uint32_t functionIndex() const { return isReified() ? functionIndexInButterfly() : ClosureWord::functionOf(word()); }
    State& state() const;
    uint8_t kindAndDeclaration() const;
    bool canConstruct() const { return !(kindAndDeclaration() & (functionKindMask | functionIsAsync)); }
    bool isBound() const { return static_cast<Format::FunctionKind>(kindAndDeclaration() & functionKindMask) == Format::FunctionKind::Bound; }
    std::span<const uint64_t> captures() const;
    unsigned numberOfReferences() const;
    String nameOfFunction() const;
    JSValue toString(JSGlobalObject*) const;
    void setComputedName(VM&, const Identifier& key, Format::MethodKind);

    bool isReified() const { return type() == SlimClosureWithPropertiesType; }
    void reifyProperties(VM&);

private:
    SlimClosure(VM& vm, Structure* structure, uint64_t word)
        : Base(vm, structure, std::bit_cast<Butterfly*>(word))
    {
    }

    uint64_t word() const { return *static_cast<const uint64_t*>(const_cast<SlimClosure*>(this)->butterflyAddress()); }
    uint32_t functionIndexInButterfly() const { return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(butterfly()) + offsetOfFunctionIndexInButterfly()); }
};
static_assert(SlimClosure::offsetOfWord() == 8 && sizeof(SlimClosure) == 16);

JSObject* ordinaryCreateFromConstructor(JSGlobalObject*, JSObject* newTarget);

struct FunctionTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionTables);

    static FunctionTables& of(VM&);
    explicit FunctionTables(VM&);

    static constexpr unsigned boundClosureExecutableKind = 5;
    static constexpr unsigned executableKind(uint8_t kindAndDeclaration, bool isHiddenWithCallees)
    {
        auto kind = static_cast<Format::FunctionKind>(kindAndDeclaration & functionKindMask);
        if (kind == Format::FunctionKind::Constructor)
            return 4;
        if (kind == Format::FunctionKind::Bound)
            return boundClosureExecutableKind;
        bool constructs = kind == Format::FunctionKind::Plain && !(kindAndDeclaration & (functionIsAsync | functionIsGenerator));
        return constructs * 2 + isHiddenWithCallees;
    }
    NativeExecutable* executableOf(uint32_t function) const;

    const Data& data;
    Vector<uint32_t, 4> functionsHiddenWithCallees;
    const uint64_t* words;
    static constexpr unsigned numberOfExecutables = 6;
    std::array<Strong<JSCell>, numberOfExecutables> executables;
    Vector<Weak<JSCell>> closuresWithoutInstance;
};

class PrototypeObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesPut;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static PrototypeObject* create(VM&, Structure*, uint32_t function);

    DECLARE_INFO;

    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    bool isGuarded() const;
    bool isModified() const { return m_isModified; }

private:
    PrototypeObject(VM& vm, Structure* structure, uint32_t function)
        : Base(vm, structure)
        , m_function(function)
    {
    }

    bool mayDefine(JSGlobalObject*, PropertyName, bool isWritableData);
    bool allowsModification(VM&);
    bool allowsPrototypeChange(VM&);

    uint32_t m_function;
    bool m_isModified { false };
};

class ClassPrototype final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | GetOwnPropertySlotIsImpure | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;
    static constexpr unsigned inlineCapacity = 2;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static ClassPrototype* create(VM&, Structure*, uint32_t classIndex);

    DECLARE_INFO;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool isExtensible(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    static void look(JSObject* holder)
    {
        if (holder->classInfo() == info()) [[unlikely]]
            createMembers(holder);
    }

    void convertToOrdinaryObject(VM&, Structure*);

    static bool hasMember(JSObject*, PropertyName);
    static bool willBeImmutable(JSObject*);
    uint32_t classIndex() const { return static_cast<uint32_t>(m_classIndex); }
    std::optional<bool> tryGetDeclaredMember(VM&, PropertyName, PropertySlot&);

private:
    ClassPrototype(VM& vm, Structure* structure, uint32_t classIndex)
        : Base(vm, structure)
        , m_classIndex(classIndex)
    {
    }

    static void createMembers(JSObject*);

    uint64_t m_classIndex;
    uint64_t m_declaredMemberLookups { 0 };
};

class GuardedObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = JSFinalObject::StructureFlags | OverridesPut;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
};

class ModuleNamespace final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | GetOwnPropertySlotIsImpure | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, bool hasHostPrototype);
    static ModuleNamespace* create(VM&, Structure*, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* starExportNames, bool isDeferred);
    static ModuleNamespace* createWithLazyNames(VM&, Structure*, JSObject* read, std::span<const uint32_t> nameStrings, JSObject* star, JSCellButterfly* starExportNames, bool isDeferred);
    static unsigned indexOfName(const Data&, std::span<const uint32_t> nameStrings, UniquedStringImpl*);
    static constexpr ptrdiff_t offsetOfRead() { return OBJECT_OFFSETOF(ModuleNamespace, m_read); }
    static constexpr unsigned noExport = UINT_MAX;
    unsigned numberOf(UniquedStringImpl*) const;
    bool isStarExport(UniquedStringImpl*) const;
    bool canReadByIndexDirectly() const { return !m_isDeferred && !m_mayNeedEvaluation; }
    void detach(JSGlobalObject*);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

private:
    ModuleNamespace(VM& vm, Structure* structure, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* starExportNames, bool isDeferred)
        : Base(vm, structure)
        , m_read(read, WriteBarrierEarlyInit)
        , m_names(names, WriteBarrierEarlyInit)
        , m_star(star, WriteBarrierEarlyInit)
        , m_starExportNames(starExportNames, WriteBarrierEarlyInit)
        , m_isDeferred(isDeferred)
        , m_mayNeedEvaluation(isDeferred)
    {
    }

    bool isSymbolLikeKey(UniquedStringImpl*) const;
    bool evaluateIfNeeded(JSGlobalObject*);
    unsigned numberOfNames() const { return m_names ? m_names->length() : m_numberOfNameStrings; }
    JSCellButterfly* ensureNames(JSGlobalObject*);

    JSValue fixedProperty(VM&, PropertyName) const;

    WriteBarrier<JSObject> m_read;
    WriteBarrier<JSCellButterfly> m_names;
    WriteBarrier<JSObject> m_star;
    WriteBarrier<JSCellButterfly> m_starExportNames;
    bool m_hasESModuleFlag { false };
    bool m_isDeferred;
    bool m_mayNeedEvaluation;
    unsigned m_numberOfNameStrings { 0 };
    const uint32_t* m_nameStrings { nullptr };
};

class Generator final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Generator* create(VM&, Structure*, Closure*, bool hasUnmodifiedIteration, uint32_t offsetOfState);
    static uint32_t offsetOfStateIn(Closure* resumeClosure);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    enum class ResumeMode : uint8_t { Next, Throw, Return };
    JSValue resume(JSGlobalObject*, ResumeMode, JSValue);
    JSValue resume(JSGlobalObject*, ResumeMode, JSValue, bool& isDone);
    static bool hasUnmodifiedIteration(JSGlobalObject*, Structure*);
    bool hasUnmodifiedIteration(JSGlobalObject*) const { return !m_hasOtherPrototype && !butterfly(); }
    bool isSuspendedAtStart() const;
    bool isCompleted() const;

    enum class StepCheck : uint8_t { None, IsUnmodifiedUnlessNext, IsUnmodified, IsGenerator, IsUnmodifiedGenerator };
    static constexpr int32_t didYield = 0;
    static constexpr int32_t didComplete = 1;
    static constexpr int32_t wasNotStepped = 2;
    static constexpr int32_t generatorSlotInStubFrame = -1;
#if CPU(ARM64)
    static constexpr uint32_t markerAfterBodyCall = 0xaa0c03ec;
#else
    static constexpr uint32_t markerAfterBodyCall = 0x26441f0f;
#endif
    struct Template {
        uint64_t header;
        uint64_t offsetOfStateAndFlags;
    };
    Template asTemplate() const;
    static Generator* forBodyFrame(const uint64_t* frame);
    void didThrow();
    static constexpr ptrdiff_t offsetOfResumeClosure() { return OBJECT_OFFSETOF(Generator, m_resumeClosure); }
    static constexpr ptrdiff_t offsetOfOffsetOfState() { return OBJECT_OFFSETOF(Generator, m_offsetOfState); }
    static constexpr ptrdiff_t offsetOfIsRunning() { return OBJECT_OFFSETOF(Generator, m_isRunning); }
    static constexpr ptrdiff_t offsetOfHasOtherPrototype() { return OBJECT_OFFSETOF(Generator, m_hasOtherPrototype); }

private:
    Generator(VM& vm, Structure* structure, Closure* resumeClosure)
        : Base(vm, structure)
        , m_resumeClosure(resumeClosure)
    {
    }

    int32_t& state() const;

    Closure* m_resumeClosure;
    uint32_t m_offsetOfState { 0 };
    bool m_isRunning { false };
    bool m_hasOtherPrototype { false };
};
static_assert(sizeof(Generator) == 32 && Generator::offsetOfOffsetOfState() == 24);

class SavedFrame final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, Instance&);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

#if CPU(X86_64)
    static constexpr GPRReg preservedRegister = X86Registers::ebx;
#else
    static constexpr GPRReg preservedRegister = InvalidGPRReg;
#endif
    static constexpr uint32_t locationInRegister = 0;
    static uint32_t locationInFrame(intptr_t offsetFromFramePointer)
    {
        RELEASE_ASSERT(offsetFromFramePointer < 0 && !(offsetFromFramePointer % static_cast<intptr_t>(sizeof(uint64_t))));
        return static_cast<uint32_t>(-offsetFromFramePointer / static_cast<intptr_t>(sizeof(uint64_t)));
    }
    static constexpr unsigned liveSlotMapOffsetBits = 18;
    static constexpr uint32_t functionSavesPreservedRegister = 1u << liveSlotMapOffsetBits;
    static constexpr uint32_t functionSavesR15 = 2u << liveSlotMapOffsetBits;
    static constexpr unsigned savedRegisterBits = 2;
    static constexpr unsigned promiseLocationShift = liveSlotMapOffsetBits + savedRegisterBits;
    static std::optional<uint64_t> siteWord(uint32_t row, uint32_t promiseLocation, uint32_t savedRegistersOrPairs)
    {
        if (row >> liveSlotMapOffsetBits || savedRegistersOrPairs >> savedRegisterBits)
            return std::nullopt;
        return row | savedRegistersOrPairs << liveSlotMapOffsetBits | static_cast<uint64_t>(promiseLocation) << promiseLocationShift;
    }

    static SavedFrame* save(VM&, Instance&, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* extra);
    static SavedFrame* forFrame(const Instance&, const uint64_t* frame);
    static bool isEnteredByJobLoop(const Instance&, const uint64_t* frame);
    void willBeRestoredToStack()
    {
        willWrite();
        m_liveSlotMapOffsetPlusOne = 0;
        didWrite();
    }
    JSPromise* promise() const;
    SavedFrame* waiter() const;
    JSPromiseCombinatorsGlobalContext* promiseAllContext() const;
    void setPromiseAllContext(VM&, JSPromiseCombinatorsGlobalContext*, uint32_t index);
    JSCell* promiseOrWaiter() const { return m_promiseOrWaiter; }
    void setWaiter(VM&, SavedFrame* waiter, JSValue asyncContext);
    void fulfill(JSGlobalObject*, VM&, JSValue);
    void reject(JSGlobalObject*, VM&, JSValue);
    JSPromise* ensurePromise(JSGlobalObject*, VM&);
    const void* resumeAddress() const { return m_resumeAddress; }

    static constexpr int32_t mayRunNextMicrotask = 128;
    static constexpr uint32_t minWordsForStringMove = 32;
    static constexpr ptrdiff_t offsetOfNumberOfWords() { return OBJECT_OFFSETOF(SavedFrame, m_numberOfWords); }
    static constexpr ptrdiff_t offsetOfVersion() { return OBJECT_OFFSETOF(SavedFrame, m_version); }
    static constexpr ptrdiff_t offsetOfLiveSlotMapOffsetPlusOne() { return OBJECT_OFFSETOF(SavedFrame, m_liveSlotMapOffsetPlusOne); }
    static constexpr ptrdiff_t offsetOfPromiseOrWaiter() { return OBJECT_OFFSETOF(SavedFrame, m_promiseOrWaiter); }
    static constexpr ptrdiff_t offsetOfAsyncContext() { return OBJECT_OFFSETOF(SavedFrame, m_asyncContext); }
    static constexpr ptrdiff_t offsetOfResumeAddress() { return OBJECT_OFFSETOF(SavedFrame, m_resumeAddress); }
    static constexpr ptrdiff_t offsetOfRegister() { return OBJECT_OFFSETOF(SavedFrame, m_register); }
    static constexpr ptrdiff_t offsetOfWords() { return sizeof(SavedFrame); }
    static size_t allocationSize(size_t numberOfWords) { return sizeof(SavedFrame) + numberOfWords * sizeof(uint64_t); }

private:
    SavedFrame(VM& vm, Structure* structure, uint32_t numberOfWords)
        : Base(vm, structure)
        , m_numberOfWords(numberOfWords)
    {
    }

    uint64_t* words() { return reinterpret_cast<uint64_t*>(this + 1); }
    void willWrite()
    {
        ++m_version;
        WTF::storeStoreFence();
    }
    void didWrite()
    {
        WTF::storeStoreFence();
        ++m_version;
    }

    uint32_t m_numberOfWords;
    uint32_t m_version { 0 };
    uint32_t m_liveSlotMapOffsetPlusOne { 0 };
    uint32_t m_indexInPromiseAll { 0 };
    void* m_resumeAddress { nullptr };
    JSCell* m_promiseOrWaiter { nullptr };
    uint64_t m_register { 0 };
    EncodedJSValue m_asyncContext { 0 };
};

class AsyncGenerator final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static AsyncGenerator* create(VM&, JSGlobalObject*, Structure*, Closure*);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    JSPromise* enqueue(JSGlobalObject*, Generator::ResumeMode, JSValue);
    JSValue enqueueDriver(JSGlobalObject*, JSValue, bool driverCanContinueImmediately);
    JSValue finishStepForDriver(JSGlobalObject*, JSValue result);
    void startEagerly(JSGlobalObject*);
    void didThrow(VM&, JSValue);
    static AsyncGenerator* forBodyFrame(const Instance&, const uint64_t* frame);
    static bool bodyCapturesExceptions(uint32_t resumeFunction);
    bool hasUnmodifiedIteration(JSGlobalObject*);
    bool isAwaitingDriver() const { return m_target.pointer() == this && !is(isDrainingBeforeDriverFlag); }
    void setDriver(VM&, JSObject* driver, JSValue asyncContext);
    void willSuspendAtAwait() { set(isSuspendingAtAwaitFlag, true); }
    void willSuspendAtYield() { set(isSuspendingAtAwaitFlag | isAwaitingYieldOperandFlag, true); }
    bool tryToYieldInPlace(Instance&, VM&, JSValue operand);
    void resumeFromAwait(JSGlobalObject*, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
    void appendRequestTargets(Vector<JSCell*, 4>&) const;

private:
    AsyncGenerator(VM& vm, Structure* structure, Closure* resumeClosure, int32_t* state)
        : Base(vm, structure)
        , m_resumeClosure(resumeClosure)
        , m_state(state)
    {
    }

    static constexpr uint16_t modeMask = 3;
    static constexpr uint16_t isBusyFlag = 4;
    static constexpr uint16_t isSuspendingAtAwaitFlag = 8;
    static constexpr uint16_t isAwaitingYieldOperandFlag = 16;
    static constexpr uint16_t isAwaitingReturnFlag = 32;
    static constexpr uint16_t isDrainingBeforeDriverFlag = 64;
    static constexpr uint16_t isEnteredByJobLoopFlag = 128;
    static constexpr uint16_t isYieldingInPlaceFlag = 256;
    static constexpr uint16_t hasThrownFlag = 512;
    static constexpr uint16_t canBeEnteredByStubFlag = 1024;
    bool is(uint16_t flag) const { return m_target.type() & flag; }
    void set(uint16_t flag, bool value) { m_target.setType(value ? m_target.type() | flag : m_target.type() & ~flag); }

    struct DriverToResume {
        JSObject* driver { nullptr };
        JSValue resolution;
        JSValue asyncContext;
        bool isRejected { false };
    };

    int32_t& state() const { return *m_state; }
    void append(VM&, Generator::ResumeMode, JSValue, JSObject* target);
    JSObject* takeFrontRequest(VM&);
    void drain(JSGlobalObject*);
    JSValue enter(VM&, int32_t mode, JSValue);
    JSValue valueForDriverToAwait(JSGlobalObject*);
    void run(JSGlobalObject*, int32_t mode, JSValue, DriverToResume* = nullptr);
    void finishRun(JSGlobalObject*, JSValue result, DriverToResume*);
    void settleFrontRequest(JSGlobalObject*, JSValue, bool isDone, bool isRejected, DriverToResume* = nullptr);
    JSPromise* immediateStep(JSGlobalObject*, JSValue);

    Closure* m_resumeClosure;
    int32_t* m_state;
    CompactPointerTuple<JSObject*, uint16_t> m_target;
    WriteBarrier<Unknown> m_value;
    WriteBarrier<Unknown> m_yieldedInPlace;
    WriteBarrier<JSSlimPromiseReaction> m_lastRequest;
    WriteBarrier<JSObject> m_cachedDriverResult;
    WriteBarrier<JSObject> m_cachedDriverResultTarget;
    WriteBarrier<JSPromise> m_immediateStep;

public:
    static constexpr unsigned targetFlagsShift = CompactPointerTuple<JSObject*, uint16_t>::maxNumberOfBitsInPointer;
    static constexpr uint64_t isEnteredByJobLoopInTarget = static_cast<uint64_t>(isEnteredByJobLoopFlag) << targetFlagsShift;
    static constexpr uint64_t isYieldingInPlaceInTarget = static_cast<uint64_t>(isYieldingInPlaceFlag) << targetFlagsShift;
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(AsyncGenerator, m_target); }
    static constexpr ptrdiff_t offsetOfYieldedInPlace() { return OBJECT_OFFSETOF(AsyncGenerator, m_yieldedInPlace); }
    static constexpr uint64_t pointerInTarget = CompactPointerTuple<JSObject*, uint16_t>::pointerMask;
    static constexpr uint64_t isBusyInTarget = static_cast<uint64_t>(isBusyFlag) << targetFlagsShift;
    static constexpr uint64_t isSuspendingAtAwaitInTarget = static_cast<uint64_t>(isSuspendingAtAwaitFlag) << targetFlagsShift;
    static constexpr uint64_t hasThrownInTarget = static_cast<uint64_t>(hasThrownFlag) << targetFlagsShift;
    static constexpr uint64_t canBeEnteredByStubInTarget = static_cast<uint64_t>(canBeEnteredByStubFlag) << targetFlagsShift;
    static constexpr uint64_t clearedOnDriverEntry = static_cast<uint64_t>(modeMask | isSuspendingAtAwaitFlag | isEnteredByJobLoopFlag) << targetFlagsShift;
    static constexpr uint64_t setOnDriverEntry = static_cast<uint64_t>(isDrainingBeforeDriverFlag | isBusyFlag) << targetFlagsShift;
    static constexpr unsigned shiftFromResumeModeToTarget = targetFlagsShift + 5;
    static_assert(static_cast<uint64_t>(CoroutineFrame::isEnteredByJobLoop) << shiftFromResumeModeToTarget == isEnteredByJobLoopInTarget);
    static constexpr uint64_t clearedOnSuspend = static_cast<uint64_t>(isDrainingBeforeDriverFlag | isEnteredByJobLoopFlag) << targetFlagsShift;
    static constexpr uint64_t clearedOnYieldInPlace = pointerInTarget | static_cast<uint64_t>(isDrainingBeforeDriverFlag | isEnteredByJobLoopFlag | isBusyFlag | isYieldingInPlaceFlag) << targetFlagsShift;
    static constexpr int32_t generatorSlotInStubFrame = -1;
    static constexpr int32_t modeSlotInStubFrame = -2;
    static constexpr unsigned maxBodyFrameSizeForStubEntry = 64 * KB;
#if CPU(ARM64)
    static constexpr uint32_t markerAfterBodyCall = 0xaa0e03ee;
#else
    static constexpr uint32_t markerAfterBodyCall = 0x24441f0f;
#endif
    static bool wasEnteredByStub(const uint64_t* frame);
#if CPU(ARM64)
    static constexpr uint32_t markerAfterBodyCallInJobLoop = 0xaa0d03ed;
#else
    static constexpr uint32_t markerAfterBodyCallInJobLoop = 0x25441f0f;
#endif
    static bool wasEnteredByJobLoopAdapter(const uint64_t* frame);
    static constexpr uint64_t requiresCallerOfJobLoop = static_cast<uint64_t>(isAwaitingYieldOperandFlag | isAwaitingReturnFlag) << targetFlagsShift;
    static constexpr uint64_t isAwaitingYieldOperandInTarget = static_cast<uint64_t>(isAwaitingYieldOperandFlag) << targetFlagsShift;
    static constexpr uint64_t isAwaitingReturnInTarget = static_cast<uint64_t>(isAwaitingReturnFlag) << targetFlagsShift;
    static constexpr uint64_t setOnSuspendAtYield = static_cast<uint64_t>(isSuspendingAtAwaitFlag | isAwaitingYieldOperandFlag) << targetFlagsShift;
    static constexpr uint64_t clearedOnSettledYield = pointerInTarget | static_cast<uint64_t>(isAwaitingYieldOperandFlag | isBusyFlag) << targetFlagsShift;
    static constexpr ptrdiff_t offsetOfCachedDriverResult() { return OBJECT_OFFSETOF(AsyncGenerator, m_cachedDriverResult); }
    static constexpr ptrdiff_t offsetOfCachedDriverResultTarget() { return OBJECT_OFFSETOF(AsyncGenerator, m_cachedDriverResultTarget); }
    static constexpr size_t stackSpaceForJobLoop = maxBodyFrameSizeForStubEntry + 4 * KB;
    void finishAfterJobLoop(JSGlobalObject*, JSValue result);
    static constexpr uint64_t isDrainingBeforeDriverInTarget = static_cast<uint64_t>(isDrainingBeforeDriverFlag) << targetFlagsShift;
    static constexpr ptrdiff_t offsetOfValue() { return OBJECT_OFFSETOF(AsyncGenerator, m_value); }
    static constexpr ptrdiff_t offsetOfResumeClosure() { return OBJECT_OFFSETOF(AsyncGenerator, m_resumeClosure); }
    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(AsyncGenerator, m_state); }
    static constexpr ptrdiff_t offsetOfLastRequest() { return OBJECT_OFFSETOF(AsyncGenerator, m_lastRequest); }
    static constexpr ptrdiff_t offsetOfImmediateStep() { return OBJECT_OFFSETOF(AsyncGenerator, m_immediateStep); }
};

JS_EXPORT_PRIVATE void installGeneratorMethods(JSGlobalObject*);
JS_EXPORT_PRIVATE Identifier enginePrivateName(VM&, const String& text);

JS_EXPORT_PRIVATE void resumeAsyncFunction(JSGlobalObject*, VM&, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
JS_EXPORT_PRIVATE void resumeAsyncGenerator(JSGlobalObject*, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
bool driveAsyncIterator(JSGlobalObject*, VM&, JSCell* awaited, JSObject* driver);
void resumeDriver(VM&, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
bool tryScheduleImmediateResume(Instance&, const uint64_t* frame, JSPromise*, JSValue resolution);
JS_EXPORT_PRIVATE bool isAsyncFunctionResumeClosure(JSCell*);
JS_EXPORT_PRIVATE JSPromise* asyncFunctionPromise(JSCell* resumeClosure);
JS_EXPORT_PRIVATE JSCell* promiseAllContextAwaiting(JSCell* resumeClosure);
bool settleElementOfPromiseAll(JSGlobalObject*, VM&, JSValue& element, JSPromiseCombinatorsGlobalContext*, uint64_t index);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledAfter(JSPromise*);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* resumeClosure);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* resumeClosure, size_t maxStackSize);
bool hasNoLazyPropertyNamed(VM&, JSCell*, UniquedStringImpl*, bool isReceiver = true);
bool closureHandlesPrototypeProperty(JSCell*);
GetValueFunc closurePrototypePropertyGetter();
bool hasFieldNamedThen(Structure*);
bool neverHasThen(Structure*);
bool isClassPrototypeWithoutThen(JSObject*);
void countThenLookup(VM&);

struct DescribedType {
    uint32_t primitives { 0 };
    bool allowsAnyObject { false };
    bool allowsAnyFunction { false };
    int64_t minInteger { 1 };
    int64_t maxInteger { 0 };
    Vector<String, 2> views;
    Vector<uint32_t, 2> jsTypes;
    String elements;
};
String describe(const DescribedType&, bool logsTypeErrors);

struct State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(VM&, Instance&, const Data&);
    RegExp* regExpLiteral(VM&, uint32_t source, uint32_t flags);
    ~State();

    const Identifier& name(VM&, uint32_t);
    std::span<GlobalPropertyCache> globalPropertyCaches() const;
    bool readsThroughModuleGraph(const String& builtin) const;
    bool trustGlobalProperty(VM&, JSGlobalObject*, uint32_t cacheNumber, UniquedStringImpl*, JSValue);
    bool trustGlobalMember(VM&, JSGlobalObject*, uint32_t cacheNumber, UniquedStringImpl* global, JSObject* holder, UniquedStringImpl* name);
    JSString* string(VM&, uint32_t);
    JSString* stringForLiteralStringWord(uint64_t);
    uint64_t literalStringWordFor(JSString*);
    JSString* createAndCacheStringConstant(VM&, uint32_t);
    JSCell* bigIntConstantIfCreated(uint32_t index) const { return m_bigIntConstants.get(index); }
    void setBigIntConstant(uint32_t, JSCell*);
    JSString* createStringConstant(VM&, uint32_t);
    const LayoutData& layoutForShape(uint32_t shape) const { return *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]); }
    bool hasLayoutForShape(uint32_t shape) const { return data.at<uint32_t>(data.layoutsOffset)[shape]; }
    const AllocationData& allocation(uint32_t number) const { return data.at<AllocationData>(data.allocationsOffset)[number]; }
    const RuntimeType& type(uint32_t index) const
    {
        uint32_t offset = data.at<uint32_t>(data.typesOffset)[index];
        RELEASE_ASSERT(offset);
        return *data.at<RuntimeType>(offset);
    }
    const RuntimeType& typeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.type()); }
    const RuntimeType& ownTypeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.ownType()); }
    uint8_t kindAndDeclarationOf(uint32_t function) const { return data.at<uint8_t>(data.functionKindsOffset)[function]; }
    Format::FunctionKind kindOf(uint32_t function) const { return static_cast<Format::FunctionKind>(kindAndDeclarationOf(function) & functionKindMask); }
    const LayoutData* classWithIndex(uint32_t) const;
    Structure* structure(VM&, uint32_t shape);
    Structure* structureFor(VM&, uint32_t shape, JSValue prototype, bool hasExtraProperties = false);
    Structure* structureFor(VM&, uint32_t shape, JSValue prototype, Structure& ofExtraProperties);
    Structure* recentStructureFor(uint32_t shape, JSValue prototype, Structure& ofExtraProperties) const;
    Structure* existingStructureFor(uint32_t shape, JSValue prototype, Structure& ofExtraProperties);
    void clearRecentStructures() { m_recentStructuresByExtraProperties.fill({ }); }
    Structure* structureWithExtraPropertiesMatching(VM&, Structure* model, uint32_t shape, JSValue prototype);
    bool isStructureWithoutExtraProperties(Structure*);
    JSCell* allocate(VM&, uint32_t allocation);
    static constexpr uint32_t maxAllocationEntries = std::numeric_limits<uint16_t>::max();
    uint32_t sizeWithSpecializationCapacity(uint32_t shape);
    Structure* closureStructure(VM&, uint32_t function);
    Structure* nonClassClosureStructure(VM&, uint32_t function);
    Structure* evalScopeStructure(VM&);
    bool tryPutLazyClosure(JSGlobalObject*, JSValue holder, const Identifier&, uint32_t function);
    void putLazyClosure(JSGlobalObject*, JSValue holder, const Identifier&, uint32_t function);
    CustomGetterSetter* lazyClosureCell(VM&, uint32_t function);
    JSObject* createClosureOver(VM&, uint32_t function, JSValue captured);
    JSObject* reifyLazyClosure(VM&, JSObject* holder, PropertyName);
    JSObject* reifyLazyClosureIfPresent(VM&, JSObject* holder, UniquedStringImpl* name, PropertyOffset&, bool& isCreatedByStubs);
    void reifyLazyClosures(VM&, JSObject* holder);
    uint32_t lazyClosureFunction(JSCell* cell) const { return m_lazyClosureFunctions.get(cell); }
    bool closuresHaveCaptures(uint32_t function) const;
    UncheckedKeyHashMap<JSCell*, uint32_t> m_lazyClosureFunctions;
    struct GeneratorStructure {
        Structure* structure { nullptr };
        uint32_t offsetOfState { 0 };
        bool hasUnmodifiedIteration { false };
    };
    GeneratorStructure& generatorStructure(VM&, uint32_t resumeFunction, bool isAsync);
    void setGeneratorTemplate(uint32_t index, uint32_t numberOfTemplates, Generator::Template);
    Structure* savedFrameStructure(VM&);
    GeneratorStructure& generatorStructure(VM&, Closure* generatorFunction, bool isAsync);
    uint32_t resumeFunctionOf(uint32_t generatorFunction) const;
    bool takesCalleeClosure(uint32_t generatorFunction) const;
    const LayoutData* classConstructedBy(uint32_t function) const;
    mutable std::array<std::atomic<uint64_t>, 64> recentClassLookups { };
    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    void createClass(JSGlobalObject*, Closure*, JSValue base, ClassPart = ClassPart::Everything, JSValue basePrototypeIfKnown = JSValue());
    struct WatchedMembers;
    void watchMembers(VM&, JSObject* holder, const LayoutData&);
    void updateStaticFieldLocations(VM&, WatchedMembers&);
    void didTransition(VM&, WatchedMembers&);
    void didModifyMembers(WatchedMembers&);
    JSObject* classPrototype(VM&, uint32_t classIndex);
    bool createsMembersLazily(const LayoutData&) const;
    bool createsPrototypeBeforeClassObject(const LayoutData&) const;
    JSObject* createClassPrototype(VM&, uint32_t classIndex, JSObject* parent);
    bool createsClassObjectOnMutation(const LayoutData&);
    Structure* lazyClassObjectStructure(VM&, const LayoutData&);
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    bool canDeferClassObject(JSObject* parent, const LayoutData&);
    bool isLazySubclassStructureOf(VM&, Structure*, JSObject* parent);
    bool tryAssumeAbsentFromLazySubclasses(VM&, JSObject* parent, UniquedStringImpl*);
    JSObject* classPrototypeIfExists(uint32_t classIndex) const { return classPrototypes.get(classIndex); }
    bool prototypeMatchesLayout(const LayoutData&, JSValue prototype);
    void updatePrototypeMatchesLayout(Structure*);
    void revalidatePrototypes(VM&, const ScopedLambda<bool(Structure*, const LayoutData&)>& shouldRevalidate);
    void didChangeDescendantPrototype(VM&, JSObject* changed, UniquedStringImpl* nameOfProperty);
    bool hasDescendantPrototypes() const { return !m_descendantPrototypes.isEmpty() || !m_shapesUnderPrototype.isEmpty(); }
    void removeDeadDescendantPrototypes(VM&);
    void didModifyPrototypeObject(VM&, PrototypeObject*);
    uint32_t initialOrderWord(VM&, const AllocationData&, Structure*);
    bool mayModifyPrototypeObjectOf(uint32_t function) const { return objectCreatorFlags(function) & Data::createsObjectsOfFlaggedLayout; }
    bool createsObjectsWithDefinitelyInitializedFields(uint32_t function) const { return objectCreatorFlags(function) & Data::createsObjectsWithDefinitelyInitializedFields; }
    JSValue createThis(JSGlobalObject*, Closure*, JSObject* newTarget);
    Structure* structureForThis(JSGlobalObject*, Closure* constructor, JSObject* newTarget, unsigned inlineCapacity);
    Structure* structureForThisWithSlimClosure(JSGlobalObject*, JSObject* constructor, JSObject* newTarget, unsigned inlineCapacity);
    unsigned inlineCapacityForThis(VM&, uint32_t constructor, uint32_t newTarget, unsigned seen, JSObject* prototype) const;
    const AllocationData* thisAllocation(uint32_t function) const;
    bool rejectsFreshObject(uint32_t function, uint32_t shape) const;
    bool createsFixedLayoutObjects(uint32_t function) const { return objectCreatorFlags(function) & isObjectCreator; }
    static constexpr uint32_t isObjectCreator = 4;
    uint32_t objectCreatorFlags(uint32_t function) const;
    bool hasGuardedPrototype(uint32_t function);
    bool isReservedFieldName(VM&, UniquedStringImpl*, uint32_t function);
    Structure* prototypeObjectStructure(VM&);
    Structure* moduleNamespaceStructure(VM&, bool hasHostPrototype);
    Structure* structureForNewModuleNamespace(VM&, bool hasHostPrototype);
    static constexpr ptrdiff_t offsetOfModuleNamespaceStructure() { return OBJECT_OFFSETOF(State, m_moduleNamespaceStructure); }
    struct NamedPropertiesTransition {
        Structure* from { nullptr };
        Structure* to { nullptr };
        Vector<uint32_t, 4> names;
        Vector<PropertyOffset, 4> offsets;
        unsigned oldOutOfLineCapacity { 0 };
        unsigned newOutOfLineCapacity { 0 };
        bool elementsDependOnVectorLength { false };
    };
    const NamedPropertiesTransition* namedPropertiesTransition(VM&, Structure* from, std::span<const EncodedJSValue> names);
    Object* createClassInstance(VM&, const LayoutData&, JSObject* prototype);
    JSValue readVariable(const Closure* classObject, const LayoutData&, const FieldData&) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    JSFunction* liveBindingGetter(VM&);
    CustomGetterSetter* liveBindingAccessor(VM&);
    CustomGetterSetter* liveBindingAccessorIfCreated() const { return m_liveBindingAccessor; }
    void adoptRealmClosures(VM&);
    Closure* closure(VM&, uint32_t function);
    void ensureProgramStaticsInitialized()
    {
        if (!m_programStaticsInitialized) [[unlikely]]
            initializeProgramStatics();
    }
    JSCell* closureIfCreated(uint32_t function) const;
    void registerConstructorClosure(Closure*);
    template<typename Visitor> void visit(Visitor&);
    void rememberElement(VM&, JSObject* array, JSValue);
    static constexpr unsigned minVectorLengthToRememberElements = 1024;

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    bool isPrivate(const FieldData&) const;
    bool hasSymbolKey(const FieldData& field) const { return data.stringFacts(field.name) & Data::stringIsConstantSymbol; }
    bool hasStringKey(const FieldData& field) const { return !isPrivate(field) && !hasSymbolKey(field); }
    bool hasFieldsWithSymbolKeys(const LayoutData& layout) const { return layout.hasFieldsWithSymbolKeys(); }
    bool isPublicSymbolThatKeysNoField(UniquedStringImpl* name) const { return name->isSymbol() && !static_cast<SymbolImpl*>(name)->isPrivate() && !fieldNameIndex(name); }
    bool hasPositionAmongKeys(const LayoutData& layout, UniquedStringImpl* name) { return !name->isSymbol() || (!static_cast<SymbolImpl*>(name)->isPrivate() && hasFieldsWithSymbolKeys(layout)); }
    bool findPrivateMethod(VM&, const LayoutData&, UniquedStringImpl*, Object::PrivateElement&);
    void addPrivateMethods(JSGlobalObject*, JSObject*, const LayoutData&);
    bool hasImmutableMethod(const LayoutData&, UniquedStringImpl*) const;
    bool storeNeverSpecializes(const LayoutData&, UniquedStringImpl*) const;
    bool declaresMethod(const LayoutData&, UniquedStringImpl*) const;
    std::optional<JSObject*> declaredMethod(VM&, const LayoutData&, UniquedStringImpl*);
    GetterSetter* declaredAccessor(VM&, const LayoutData&, UniquedStringImpl*);
    bool didGetDeclaredMember { false };
    struct DeclaredMemberLocation {
        JSObject* holder;
        PropertyOffset offset;
        bool isAccessor;
    };
    struct LazyPrototypeLookupResult {
        std::optional<DeclaredMemberLocation> member;
        JSObject* firstObjectAbove;
    };
    std::optional<LazyPrototypeLookupResult> findThroughLazyPrototypes(VM& vm, JSObject* base, UniquedStringImpl* name, uint32_t identifier, bool baseIsKnownMissing) { return findThroughLazyPrototypesByString(vm, base, name, data.identifierStringPlusOne(identifier), baseIsKnownMissing); }
    std::optional<LazyPrototypeLookupResult> findThroughLazyPrototypesByString(VM&, JSObject* base, UniquedStringImpl*, uint32_t stringPlusOne, bool baseIsKnownMissing = false);
    DeclaredMemberLocation declaredMemberLocation(VM&, std::optional<uint32_t> method, const MethodData* getter, const MethodData* setter);
    struct DeclaredMemberEntries {
        UniquedStringImpl* uid;
        uint32_t nameInTable;
        Vector<Structure*, 8> owners;
    };
    Vector<DeclaredMemberEntries> collectDeclaredMemberEntries(VM&, const LayoutData&, MarkedArgumentBuffer& liveOwners);
    void repointDeclaredMemberEntries(VM&, JSObject* prototype, const Vector<DeclaredMemberEntries>&);
    bool hasPrototypeMembers(const LayoutData&) const;
    bool mayBeAbsent(const FieldData& field) const;

    Object* compiledObject(JSValue) const;
    bool passes(const RuntimeShapeTest&, uint32_t shape) const;
    bool acceptsShape(const RuntimeType&, uint32_t shape) const;
    bool subtypesHaveNoSpecializations(const RuntimeType&) const;
    bool matchesTypedSubtype(const RuntimeType&, JSObject*) const;
    bool fits(const RuntimeType&, JSValue) const;
    bool accepts(JSGlobalObject*, const RuntimeType&, JSValue);
    enum class CoercionResult : bool { OriginalObject, ConvertedValue };
    JSValue coerce(JSGlobalObject*, JSValue, const RuntimeType&, Format::CastReason, AuditSite = { }, CoercionResult = CoercionResult::ConvertedValue, uint32_t typeAndReason = 0);
    bool tryPreviousTransition(VM&, JSValue, uint32_t typeAndReason);
    struct CoercionPlan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CoercionPlan);
        struct Field {
            PropertyOffset offset;
            const FieldData* field;
            UniquedStringImpl* name;
            uint8_t acceptedKinds;
        };
        static constexpr uint8_t acceptsInt32 = 1;
        static constexpr uint8_t acceptsString = 2;
        static constexpr uint8_t acceptsTrue = 4;
        static constexpr uint8_t acceptsFalse = 8;
        static constexpr uint8_t acceptsUndefined = 16;
        static constexpr uint8_t acceptsNull = 32;
        static constexpr uint8_t acceptsDouble = 64;
        static constexpr uint8_t takesUntypedObject = 128;
        static uint8_t kindOf(JSValue value) { return value.isInt32() ? acceptsInt32 : value.isString() ? acceptsString : value.isTrue() ? acceptsTrue : value.isFalse() ? acceptsFalse : value.isUndefined() ? acceptsUndefined : value.isNull() ? acceptsNull : value.isDouble() ? acceptsDouble : 0; }
        static constexpr unsigned maxNumberByDiscriminant = 256;
        static constexpr unsigned maxNumberOfDiscriminants = 4;
        using Discriminants = Vector<PropertyOffset, maxNumberOfDiscriminants>;
        const LayoutData* layout { nullptr };
        const LayoutData* chosen { nullptr };
        bool hasAlternatives { false };
        bool keepsObject { false };
        uint32_t orderAndCount { 0 };
        Vector<Field, 8> fields;
        Vector<std::pair<PropertyOffset, UniquedStringImpl*>, 2> extraProperties;
        static constexpr size_t maxInPlaceConversionSize = 256;
        Vector<uint8_t> initialBody;
        PropertyOffset discriminant { invalidOffset };
        UncheckedKeyHashMap<String, std::unique_ptr<CoercionPlan>> byDiscriminant;
        mutable uint32_t quickConversionEpoch { 0 };
        mutable uint32_t graphConversionEpoch { 0 };
        struct QuickMove {
            enum class Kind : uint8_t { Any, StringAsValue, StringAsCell32, InlineInt32, NumberAsDouble, BooleanAsByte };
            uint16_t from;
            uint16_t to;
            Kind kind;
            uint8_t width;
            uint8_t acceptedKinds;
            Store store;
        };
        static constexpr unsigned maxNumberOfQuickMoves = 14;
        mutable Vector<QuickMove, 8> quickMoves;
        mutable Vector<uint8_t> quickBody;
        mutable std::unique_ptr<ConversionPlan> inPlaceConversion;
        mutable std::unique_ptr<ConversionPlan> copyConversion;
        mutable uint32_t copyConversionEpoch { 0 };
    };
    struct QuickConversion {
        uint32_t structureID { 0 };
        PropertyOffset discriminantOffset { invalidOffset };
        const RuntimeType* type { nullptr };
        uint64_t discriminant { 0 };
        const CoercionPlan* plan { nullptr };
        uint64_t header { 0 };
        uint32_t sizeOfCell { 0 };
    };
    static constexpr unsigned numberOfQuickConversions = 512;
    static constexpr size_t maxSizeOfQuickConversion = 128;
    bool tryQuickConversion(VM&, JSValue, const RuntimeType&, uint32_t typeAndReason = 0);
    uint32_t conversionKeyOfChild(const RuntimeType& type) const { return static_cast<uint32_t>((reinterpret_cast<const uint8_t*>(&type) - reinterpret_cast<const uint8_t*>(&data)) >> 2) << 4 | ConversionMove::reasonOfChild; }
    bool tryConvertPlainObjectInPlace(VM&, JSValue, const RuntimeType&, uint32_t typeAndReason);
    void cacheQuickConversion(Structure*, const RuntimeType&, const CoercionPlan& root, const CoercionPlan&, uint64_t discriminant, Object* made, size_t sizeOfCell, uint32_t typeAndReason);
    std::optional<uint32_t> literalStringWordOfConstant(uint32_t string) const;
    void cacheCopyConversion(VM&, Structure*, const RuntimeType&, const CoercionPlan&, Structure* made, uint32_t typeAndReason);
    void cacheConversionWithExtraProperties(VM&, Structure*, const RuntimeType&, const CoercionPlan&, Object* made, size_t sizeOfCell, uint32_t typeAndReason);
    std::unique_ptr<ConversionPlan> createConversionPlan(const CoercionPlan&, size_t sizeOfCell, uint64_t header, void* allocator, PropertyOffset discriminant = invalidOffset, const Object* converted = nullptr);
    void cacheConversion(uint32_t structureID, uint32_t typeAndReason, const ConversionPlan&);
    bool m_hasCachedConversions { false };
    static uint64_t discriminantWord(JSValue);
    QuickConversion* quickConversion(uint32_t structureID, const RuntimeType&, uint64_t discriminant, bool evictsIfFull);
    std::unique_ptr<std::array<QuickConversion, numberOfQuickConversions>> m_quickConversions;
    uint32_t m_quickConversionEpoch { 1 };
    static constexpr unsigned maxDepthOfChildConversions = 4;
    bool m_isConvertingChildren { false };
    std::array<JSCell*, maxDepthOfChildConversions> m_ancestorsOfQuickConversion { };
    using ChildrenOfQuickConversion = Vector<std::pair<JSCell*, const RuntimeType*>, 16>;
    bool canConvertQuickly(JSValue, const RuntimeType&, ChildrenOfQuickConversion&, unsigned depth);
    bool canSettleByPlans(JSValue, const RuntimeType&, ChildrenOfQuickConversion& seen, ChildrenOfQuickConversion& converted, unsigned depth);
    bool tryKeepByPlan(VM&, JSObject*, const RuntimeType&);
    bool keepQuickly(VM&, JSObject*, const CoercionPlan&);
    struct UnconvertibleObjectPlan {
        struct Check {
            JSObject* holder;
            const FieldData* field;
            const RuntimeType* type;
            PropertyOffset offset;
        };
        struct Prototype {
            JSObject* object;
            uint32_t structureID;
            bool isCompiled;
        };
        static constexpr unsigned indexBits = 6;
        static unsigned placeOf(uint32_t structureID, const RuntimeType& type) { return ((structureID ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(&type) >> 3)) * 0x9e3779b1u) >> (32 - indexBits); }
        uint32_t structureID { 0 };
        const RuntimeType* type { nullptr };
        Vector<Check, 8> checks;
        Vector<Prototype, 3> prototypes;
        Vector<UnconvertedCastCheck, 12> checksForStub;
        bool isCheckedByRuntimeOnly { false };
    };
    bool staysUnconvertedByPlan(JSValue, const RuntimeType&) const;
    void shareUnconvertibleObjectPlanWithStub(JSObject*, const RuntimeType&, uint32_t typeAndReason);
    bool passesOpenTypeAsItIs(JSValue, const RuntimeType&, uint32_t typeAndReason);
    void didLeavePlainObjectAsItWas(JSValue, const RuntimeType&, uint32_t typeAndReason);
    bool m_hasUnconvertedCastsForStub { false };
    void cacheUnconvertibleObjectPlan(UnconvertibleObjectPlan&&);
    std::unique_ptr<std::array<UnconvertibleObjectPlan, 1u << UnconvertibleObjectPlan::indexBits>> m_unconvertibleObjectPlans;
    struct ObjectBelowKeptObject {
        JSObject* object;
        Structure* structure;
        const RuntimeType* type;
    };
    using ObjectsBelowKeptObject = Vector<ObjectBelowKeptObject, 8>;
    void collectObjectsBelowKeptObject(JSObject*, const RuntimeType&, ObjectsBelowKeptObject&, unsigned depth);
    void cacheConversionsBelowKeptObject(const ObjectsBelowKeptObject&);
    static constexpr unsigned maxTypedClonePlans = 4096;
    Object* tryCloneAs(VM&, JSObject* source, uint64_t target);
    const TypedClonePlan& typedClonePlan(VM&, Structure*, uint64_t target);
    bool canHoldWithoutConversion(const FieldData&, uint8_t acceptedKinds, JSValue) const;
    bool m_hasCachedTypedClones { false };
    bool m_hasNewTypedClonePlans { false };
    UncheckedKeyHashMap<std::pair<Structure*, uint64_t>, std::unique_ptr<TypedClonePlan>> m_typedClonePlans;
    TypedClonePlan m_noTypedClonePlan;
    const CoercionPlan* coercionPlan(Structure*, const RuntimeType&) const;
    const CoercionPlan* coercionPlan(JSObject*, const RuntimeType&) const;
    const CoercionPlan* firstCoercionPlanForShape(VM&, JSObject*, const RuntimeType&);
    void finishCoercionPlan(CoercionPlan&, const LayoutData&, uint32_t orderAndCount);
    void addCoercionPlan(JSObject*, Structure*, const RuntimeType&, std::unique_ptr<CoercionPlan>&&, const CoercionPlan::Discriminants&);
    bool canAddCoercionPlans() const { return m_coercionPlans.size() < maxCoercionPlans; }
    void removeDeadCoercionPlans(VM&, bool newOnly);
    static constexpr size_t maxCachedKeyLength = 64;
    JSString* cachedKeyFor(VM&, std::span<const Latin1Character>);
    JSString* makeKey(VM&, std::span<const Latin1Character>, unsigned hash);
    struct KeyByParts {
        static constexpr uint32_t maxNumberOfParts = 6;
        ALWAYS_INLINE bool tryTake(uint32_t count, const EncodedJSValue* values)
        {
            if (count > maxNumberOfParts)
                return false;
            uint32_t areCells = 0;
            uint64_t mixed = count;
            for (uint32_t i = 0; i < count; ++i) {
                JSValue value = JSValue::decode(values[i]);
                uint32_t part;
                if (value.isInt32())
                    part = value.asInt32();
                else if (value.isCell()) {
                    part = static_cast<uint32_t>(std::bit_cast<uintptr_t>(value.asCell()) >> MarkedBlockCage::shift);
                    areCells |= 1u << i;
                } else
                    return false;
                parts[i] = part;
                mixed = (mixed ^ part) * 0x9e3779b97f4a7c15ull;
            }
            countAndKinds = count | areCells << 8;
            hash = static_cast<uint32_t>((mixed ^ areCells) >> 32);
            return true;
        }
        ALWAYS_INLINE bool hasSameParts(const KeyByParts& other) const { return countAndKinds == other.countAndKinds && !memcmp(parts.data(), other.parts.data(), sizeof(parts)); }

        uint32_t countAndKinds { 0 };
        union {
            uint32_t key { 0 };
            uint32_t hash;
        };
        std::array<uint32_t, maxNumberOfParts> parts { };
    };
    static_assert(sizeof(KeyByParts) == 32);
    ALWAYS_INLINE JSString* tryFindKeyByParts(const KeyByParts& wanted) const
    {
        size_t numberOfPlaces = m_keysByParts.size();
        if (!numberOfPlaces) [[unlikely]]
            return nullptr;
        const KeyByParts* pair = m_keysByParts.span().data() + (wanted.hash & (numberOfPlaces - 1) & ~static_cast<size_t>(1));
        for (size_t i = 0; i < 2; ++i) {
            if (pair[i].hasSameParts(wanted))
                return uncheckedDowncast<JSString>(MarkedBlockCage::decode(pair[i].key));
        }
        return nullptr;
    }
    ALWAYS_INLINE void cacheKeyByParts(const KeyByParts& taken, JSString* key)
    {
        size_t numberOfPlaces = m_keysByParts.size();
        if (!numberOfPlaces || !MarkedBlockCage::contains(key)) [[unlikely]]
            return cacheFirstKeyByParts(taken, key);
        KeyByParts* place = m_keysByParts.mutableSpan().data() + (taken.hash & (numberOfPlaces - 1) & ~static_cast<size_t>(1));
        bool overwrites = false;
        if (place->countAndKinds) {
            overwrites = place[1].countAndKinds;
            place += overwrites ? taken.hash >> 31 : 1;
        }
        *place = taken;
        place->key = MarkedBlockCage::encode(key);
        if (overwrites && ++m_keysByPartsEvictionCount > numberOfPlaces / 4) [[unlikely]]
            growKeysByParts();
    }
    KeyByParts lastKeyPartsLookup;
    ALWAYS_INLINE JSString* tryFindCachedKey(std::span<const Latin1Character> characters, unsigned hash) const
    {
        size_t numberOfPlaces = m_cachedKeys.size();
        if (!numberOfPlaces) [[unlikely]]
            return nullptr;
        const uint64_t* pair = m_cachedKeys.span().data() + (hash & (numberOfPlaces - 1) & ~static_cast<size_t>(1));
        for (size_t i = 0; i < 2; ++i) {
            uint64_t entry = pair[i];
            if (entry >> 32 != hash)
                continue;
            auto* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(static_cast<uint32_t>(entry)));
            if (auto* impl = candidate->tryGetValueImpl(); impl->is8Bit() && CompactTable::hasCharacters<Latin1Character>(*impl, characters))
                return candidate;
        }
        return nullptr;
    }
    static constexpr size_t maxCaseConvertedStringLength = 64;
    JSString* caseConvertedString(VM&, std::span<const Latin1Character>);
    void removeDeadCachedKeys(VM&);
    void cacheExtraPropertyRead(Object*, JSObject* extraProperties, uint32_t name, PropertyOffset, unsigned attributes);
    void clearExtraPropertyReads();
    std::optional<uint32_t> orderAfterLateFieldCreation(const Object&, JSObject* extraProperties, uint64_t slotKeys) const;
    void cacheLateFieldCreation(const Object&, JSObject* extraProperties, uint32_t orderWordBefore, uint64_t slotKeys);
    void clearLateFieldCreations();
    static constexpr unsigned propertyListCacheSize = 256;
    CachedPropertyList& propertyListCacheEntry(StructureID, uint32_t shapeWord, uint32_t orderAndCount);
    Structure* plainCopyStructure(VM&, CachedPropertyList&);
    void clearPropertyListCache();
    void removeExtraPropertiesOf(Object*);
    uint8_t kindsAcceptedBy(const RuntimeType&) const;
    Object* copyUnchangedByPlan(VM&, JSObject*, const RuntimeType&, bool mayConvertInPlace = false, uint32_t typeAndReason = 0);
    Object* tryConvertOrCopyByPlan(VM&, JSObject*, const CoercionPlan&, bool mayConvertInPlace);
    bool tryConvertInPlaceByPlans(VM&, JSObject*, const RuntimeType&, uint32_t typeAndReason = 0);
    enum class WireFormat : uint8_t { Int32, Float32, Float64, Boolean, Text, TimestampMicroseconds };
    struct RowColumn {
        const uint8_t* name;
        uint32_t nameLength;
        WireFormat format;
    };
    struct RowDecoder {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RowDecoder);
        enum class Step : uint8_t { General, InlineInt32, Int32AsInt32, Int32AsDouble, Int32AsValue, Float32AsDouble, Float64AsDouble, BooleanAsByte, BooleanAsLowByteOfValue, TextAsCell32, TextAsValue, TextAsLiteral, DateAsCell32, DateAsValue };
        struct LiteralText {
            std::span<const uint8_t> text;
            uint64_t bits;
        };
        struct Column {
            const FieldData* field;
            uint64_t nullBits;
            uint32_t offset;
            WireFormat format;
            Step step;
            uint8_t nullWidth;
            uint8_t acceptedKinds;
            uint8_t literalWidth { 0 };
            uint16_t firstLiteral { 0 };
            uint16_t literalCount { 0 };
        };
        const LayoutData* layout { nullptr };
        Vector<Column, 16> columns;
        Vector<LiteralText, 8> literals;
        Vector<uint8_t> initialBody;
    };
    void setExpectedRowType(VM&, JSObject* query, uint32_t type);
    uint32_t expectedRowType(VM&, JSObject* query);
    std::unique_ptr<RowDecoder> createRowDecoder(VM&, JSGlobalObject*, const RuntimeType&, std::span<const RowColumn>);
    Object* tryDecodeRow(VM&, JSGlobalObject*, const RowDecoder&, std::span<const uint8_t>);
    bool tryConvertRow(VM&, JSObject*, const RuntimeType&);
    RefPtr<PrivateSymbolImpl> m_expectedRowTypeSymbol;
    bool hasCapacityToConvertInPlace(JSObject*, Object* copy);
    bool rejectsCopiesOfSharedObjects() const { return !data.logsTypeErrors && !data.isCompiledForAudit; }
    void become(VM&, JSObject*, Object* copy);
    JSValue hold(JSGlobalObject*, JSValue, const RuntimeType&);
    bool convertInPlace(JSGlobalObject*, JSObject*, const RuntimeType&, String* failureReason = nullptr);
    bool specializeToStore(JSGlobalObject*, Object*, PropertyName, JSValue);
    bool storeByKnownTransition(VM&, Object*, UniquedStringImpl* name, JSValue);
    const StoreTransition* storeTransitionFor(Structure* before, uint64_t wordsBefore, Object*, UniquedStringImpl* name, uint64_t fieldWrite);
    const StoreTransition* fieldCreatingStoreTransition(const FieldData&, uint64_t wordsBefore, Object*, uint64_t fieldWrite, bool checksPrototypeChain);
    const StoreTransition* storageCreatingStoreTransition(const FieldData&, Object*, uint64_t fieldWrite);
    bool tryCreateFieldWithCachedTransition(VM&, Object*, const FieldData&, JSValue);
    void removeDeadStoreTransitions(VM&);
    const Vector<uint16_t, 2>& requiredFieldsAdded(const LayoutData& from, const LayoutData& to);
    JSValue assertionFailed(JSValue, const RuntimeType&, uint32_t what, AuditSite, const String& failure = { }, JSValue storedTo = { });
    void reportInsufficientCapacity(const LayoutData&, size_t cellSize, const LayoutData& wanted);
    enum class InAudit : bool { Ignored, Performed };
    enum class ActionOnViolation : uint8_t { Threw, Continued, Dropped, Converted, Copied };
    struct Violation {
        ASCIILiteral kind;
        ASCIILiteral what { };
        const LayoutData* on { nullptr };
        JSObject* onBuiltin { nullptr };
        uint32_t onFunction { UINT32_MAX };
        UniquedStringImpl* member { nullptr };
        const RuntimeType* expected { nullptr };
        std::optional<uint32_t> expectedKinds { };
        uint32_t expectedName { UINT32_MAX };
        JSValue got { };
        std::optional<unsigned> parameter { };
        const String* reason { nullptr };
        bool isAtCaller { false };
        ActionOnViolation action { ActionOnViolation::Threw };
    };
    static constexpr ASCIILiteral defaultViolationKind = "type-mismatch"_s;
    void recordViolation(VM&, const Violation&);
    struct ListedSite {
        ASCIILiteral kind;
        unsigned ordinal { 0 };
    };
    std::optional<ListedSite> listedSite(const String& file, LineColumn, const Violation&, ASCIILiteral kind) const;
    static ASCIILiteral kindName(JSValue, const RuntimeType* expectedType = nullptr);
    bool reportOrThrow(JSGlobalObject*, const Violation&, const String& message, InAudit, ASCIILiteral departure = { });
    ALWAYS_INLINE bool reportOrThrow(ThrowScope& scope, JSGlobalObject* globalObject, const Violation& violation, const String& message, InAudit inAudit, ASCIILiteral departure = { })
    {
        bool goesOn = reportOrThrow(globalObject, violation, message, inAudit, departure);
        EXCEPTION_ASSERT_UNUSED(scope, !!scope.exception() == !goesOn);
        return goesOn;
    }
    ASCIILiteral ignoredRulesNote() const { return m_hasIgnoredRule ? " (after a rule was ignored)"_s : ""_s; }
    void countAllocation(uint32_t number, JSCell*);
    void countObject(Object*);
    void countTransition(const LayoutData& from, const LayoutData& to);
    void countFieldCreation(const LayoutData&, const FieldData&);
    void countExtension(uint32_t previousSize, uint32_t size);
    void countArrayGet(JSArray*, JSValue key);
    void countCoercion(JSValue, const LayoutData* before, const RuntimeType&, Format::CastReason, ASCIILiteral way, JSValue result);
    void countArrayPut(JSArray*, JSValue key, IndexingType shapeBefore, unsigned vectorLengthBefore);
    void countClosure(uint32_t function, unsigned numberOfCaptures);
    bool usesSlimClosures(uint32_t function) const { return data.at<uint8_t>(data.slimClosuresOffset)[function / 8] >> function % 8 & 1; }
    Structure* slimClosureStructure(VM&, uint32_t function);
    void createSlimClosureStructures(VM&);
    Structure* reifiedSlimClosureStructure(VM&, uint32_t function);
    Structure* boundClosureStructure(VM&, uint32_t function);
    JSObject* closureObjectFor(VM&, uint32_t function);
    JSObject* slimClosureLazyPropertyHolder(VM&);
    void countPlainAllocation(JSCell*, uint32_t function, uint32_t kindAndValue);
    void countExtraPropertyTable(const LayoutData&);
    void countExtraProperty(const LayoutData&, PropertyName);
    void didBecomePrototype(const LayoutData&);
    bool isPrototypeShape(uint32_t shape) const { return m_prototypeShapes.get(shape); }
    void countCompactTable(const String& event, uint32_t entries);
    enum class AccessKind : uint8_t { Read, Write, ReadExtra, WriteExtra };
    bool isCounting() const { return !!m_counts; }
    void countAccess(AccessKind, const void* returnAddress, PropertyName, JSValue receiver);
    void countCollection(CollectionScope);
    JSValue coerceToFieldType(JSGlobalObject*, const FieldData&, JSValue, String& failure);
    bool storesConvertPlainData { true };
    AsyncGenerator* enteredAsyncGenerator { nullptr };
    AsyncGenerator* asyncGeneratorToFinishAfterJobLoop { nullptr };
    EncodedJSValue asyncGeneratorResultAfterJobLoop { 0 };
    JSPromise* asyncGeneratorImmediateStep { nullptr };
    bool stubCanEnterAsyncGenerator(Closure* resumeClosure);
    bool isAllowedLiteral(const FieldData&, JSValue) const;
    bool isAllowedLiteral(std::span<const LiteralData>, JSValue) const;
    bool isAllowedLiteralWithoutField(VM&, const LayoutData&, UniquedStringImpl* property, JSValue);
    void willModify(const FieldData&, JSValue);
    bool isStringEqualTo(JSValue, uint32_t string) const;

    std::span<const uint16_t> fieldsInOrder(const LayoutData&, uint32_t order);
    JSCellButterfly* keysInOrder(VM&, const LayoutData&, uint32_t orderWord);
    struct FieldAsProperty {
        UniquedStringImpl* name;
        const FieldData* field;
    };
    std::optional<std::span<const FieldAsProperty>> propertiesInOrder(VM&, const LayoutData&, uint32_t orderWord, Object::SymbolKeys = Object::SymbolKeys::Skip);
    bool leavesFieldsUnlisted(const LayoutData&, uint32_t orderWord);
    static bool isDefinedByConstructor(const LayoutData& layout, uint16_t key) { return layout.kind == LayoutKind::Instance && key < layout.numberOfFields && !(layout.fields()[key].flags & (fieldIsAbsentAtCreation | fieldIsPlaceholder)); }
    struct KeysAsJSON {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(KeysAsJSON);
        struct Key {
            uint32_t readKindAndOffset;
            uint32_t fieldIndex;
            uint32_t start;
            uint32_t length;
        };
        static constexpr unsigned copyStride = 16;
        Vector<Key> keys;
        Vector<Latin1Character> text;
    };
    const KeysAsJSON* keysAsJSON(VM&, const LayoutData&, uint32_t orderWord);
    struct PlainCopyPlan {
        std::span<const FieldAsProperty> properties;
        Structure* structure { nullptr };
    };
    const PlainCopyPlan& plainCopyPlan(VM&, const LayoutData&, uint32_t orderWord);
    struct AssignmentPlan {
        enum class Kind : uint8_t { Int32OrNumberBox, Boxed, Cell32, Double, Boolean, General };
        struct FieldStore {
            uint32_t read;
            Kind kind;
            uint8_t acceptedKinds;
            bool acceptsWhatSourceHolds;
            const FieldData* from;
            const FieldData* to;
        };
        struct ExtraPropertyStore {
            uint32_t read;
            PropertyOffset offset;
            const FieldData* from;
        };
        StructureID target;
        StructureID source;
        StructureID extraPropertiesBefore;
        uint32_t sourceOrderWord { 0 };
        bool isUsable { false };
        bool addsExtraProperties { false };
        Structure* extraPropertiesAfter { nullptr };
        Structure* targetAfter { nullptr };
        Vector<FieldStore> fieldStores;
        Vector<ExtraPropertyStore> extraPropertyStores;
        Vector<StructureID, 4> prototypeStructures;
    };
    const AssignmentPlan& assignmentPlan(VM&, Object& target, JSObject* extraProperties, JSObject* source, uint32_t sourceOrderWord);
    void clearAssignmentPlans();
    bool everyFieldIsOrdinaryProperty(VM&, const LayoutData&);
    bool mayInterceptPutOfOwnProperty(VM&, const LayoutData&, JSObject* extraProperties, JSObject* prototype);
    void clearCheckedPrototypeChains();
    struct PlainCopyExcludingPlan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlainCopyExcludingPlan);
        Vector<RefPtr<UniquedStringImpl>, 2> excluded;
        FixedVector<FieldAsProperty> properties;
        Structure* structure { nullptr };
    };
    const PlainCopyExcludingPlan& plainCopyExcludingPlan(VM&, const LayoutData&, uint32_t orderWord, std::span<UniquedStringImpl* const> excluded);
    Structure* plainObjectStructureWith(VM&, std::span<const FieldAsProperty>);
    uint32_t orderStartingWith(const LayoutData&, std::span<const uint16_t> fields, bool registersPrefixes = true);
    std::optional<uint32_t> orderWithExtraKeyAt(const LayoutData&, uint32_t orderAndCount, uint16_t entry, uint32_t position);
    void didPositionExtraKey(const LayoutData&, uint32_t orderAndCount, uint16_t entry, uint32_t position, uint32_t result);
    uint32_t orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t field);
    const FieldData& fieldAt(const LayoutData&, uint32_t slotKey);
    uint32_t orderInSpecialization(VM&, const LayoutData& from, const LayoutData& to, uint32_t orderWord);
    TransitionPlan& transitionPlanFor(VM&, Object*, uint32_t shape);
    struct CopyPlan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CopyPlan);
        struct Move {
            uint32_t from;
            uint32_t to;
            uint32_t size;
        };
        struct Fill {
            uint32_t to;
            uint32_t size;
            uint64_t bits;
        };
        struct Given {
            uint16_t field;
            uint16_t index;
        };
        struct Conversion {
            uint16_t from;
            uint16_t to;
            uint8_t acceptedKinds { 0 };
        };
        Vector<RefPtr<UniquedStringImpl>, 2> excluded;
        Structure* structure { nullptr };
        uint32_t size { 0 };
        uint32_t orderWord { 0 };
        Vector<Move, 8> moves;
        Vector<Fill, 2> fills;
        Vector<Given, 2> given;
        Vector<Conversion, 2> conversions;
        Vector<Conversion, 2> checkedConversions;
    };
    const CopyPlan* copyPlan(uint32_t shape, uint32_t orderWord, uint32_t copyShape, std::span<UniquedStringImpl* const> excluded) const;
    void addCopyPlan(uint32_t shape, uint32_t orderWord, uint32_t copyShape, std::unique_ptr<CopyPlan>&&);
    struct MergePlan {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(MergePlan);
        enum class MoveKind : uint8_t { Copy, PurifiedDouble, BoxedDouble };
        struct Move {
            uint32_t from;
            uint32_t to;
            uint8_t size;
            uint8_t source;
            MoveKind kind;
            bool isToExtension;
        };
        struct Conversion {
            const FieldData* from;
            const FieldData* to;
            uint16_t source;
            bool alwaysMatches;
        };
        Structure* structure { nullptr };
        uint32_t size { 0 };
        uint32_t orderWord { 0 };
        Vector<Move, 16> moves;
        Vector<Conversion> conversions;
        const FieldData* lastOutOfLine { nullptr };
    };
    using MergeKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    const MergePlan* mergePlan(const MergeKey& key) const { return m_mergePlans.get(key); }
    void addMergePlan(const MergeKey& key, std::unique_ptr<MergePlan>&& plan) { m_mergePlans.add(key, WTF::move(plan)); }
    bool allExtraKeysHavePositions(const LayoutData&, uint32_t orderWord, JSObject* extraProperties);

    JSObject* prototypeForInstancesOf(VM&, Closure*);
    void didChangePrototypeOf(VM&, Closure*);

    static String describe(JSValue);
    String describe(const RuntimeType&) const;
    String describeKinds(uint32_t kinds, uint32_t name) const;
    String nameOf(const LayoutData&) const;
    String nameOf(const RuntimeShapeTest&) const;
    String describeAnonymousType(size_t numberOfProperties, const Invocable<uint32_t(uint32_t)> auto& nameOfProperty) const;
    String nameOfFunction(uint32_t) const;
    String text(uint32_t string) const;
    String describeSite(AuditSite) const;
    static String nonNullMessage(JSValue, const String& operandSource = { });
    static String nullishMessage(JSValue, const String& property, bool isWrite);
    static String nullishMessage(JSGlobalObject*, JSValue, JSValue key, bool isWrite);
    String mismatchMessage(VM&, JSValue, const RuntimeType& expected, const String& reason = { });
    String explainMismatch(VM&, JSValue, const RuntimeType&, bool subtypesOnly = false);
    String assignmentMessage(VM&, const String& target, const FieldData&, JSValue, const String& reason);
    bool isAssignable(const RuntimeType& from, const RuntimeType& to) const;

    Instance& instance;
    const Data& data;
    AbstractModuleRecord* moduleRecord { nullptr };
    String requestedFilePath;
    Identifier importerKey(VM&) const;
    ModuleEvaluation* moduleEvaluation { nullptr };
    int32_t moduleAsyncEvaluationCount { 0 };
    Vector<JSArray*, 4> modulesExecutingAsync;
    struct ImportFromHost {
        Strong<JSObject> promise;
        int32_t referrerAsyncEvaluationOrder;
    };
    Vector<ImportFromHost, 1> importsFromHost;
    JSObject* importMeta { nullptr };
    JSObject* importMetaForModule(JSGlobalObject*, JSString* path);
    struct LoadedEntryPoint {
        uint32_t moduleEntryOffset;
        AbstractModuleRecord* record;
        JSObject* importMeta;
    };
    Vector<LoadedEntryPoint, 1> loadedEntryPoints;
    void didLoadEntryPoint(uint32_t moduleEntryOffset, AbstractModuleRecord*);
    JSObject* importMetaForEntryPoint(JSGlobalObject*, uint32_t index);
    Vector<std::pair<AbstractModuleRecord*, JSObject*>, 1> entryPointImports;
    bool entryPointIsCommonJS { false };
    JSValue newTarget;
    static constexpr ptrdiff_t offsetOfNewTarget() { return OBJECT_OFFSETOF(State, newTarget); }
    const Identifier& nameIfCreated(uint32_t string) const;
    void createNamesForHeapAnalysis(VM&);
    uint32_t* structures { nullptr };
    UncheckedKeyHashMap<uint32_t, JSObject*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> classPrototypes;
    static constexpr unsigned declaredMembersPerHolder = 62;
    Vector<JSObject*> declaredMemberHolders;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> declaredMemberLocations;
    UncheckedKeyHashMap<JSObject*, Structure*> classPrototypeStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> lazyClassObjectStructures;
    struct LazySubclasses {
        Vector<uint32_t> withStaticMembers;
        Vector<RefPtr<UniquedStringImpl>> assumedAbsent;
    };
    UncheckedKeyHashMap<JSObject*, LazySubclasses> lazySubclasses;
    ExtraPropertiesTable extraProperties;
    Ref<ExtraPropertiesHandleTable> vmExtraPropertiesHandleTable;
    ExtraPropertiesHandleTable& extraPropertiesHandleTable;
    NumberBoxTable numberBoxes;
    FieldAttributesTable fieldAttributes;
    struct KeepFailure {
        uint32_t structureID;
        const RuntimeType* type;
        const FieldData* field;
    };
    KeepFailure& keepFailure(VM&, Structure*, const RuntimeType&);
    struct PropertiesWithSupertypes {
        static constexpr unsigned maxSize = 24;
        static constexpr unsigned maxParts = 8;
        struct Property {
            PropertyOffset offset;
            uint32_t type : 31;
            uint32_t leavesUntypedObjects : 1;
        };
        bool isOf(Structure* structure, const RuntimeType& ofObject, unsigned ofPart) const { return structureID == structure->id().bits() && type == &ofObject && part == ofPart; }
        std::span<const Property> span() const { return std::span { properties }.first(size); }
        uint32_t structureID;
        uint16_t size;
        uint8_t part;
        bool isLast;
        const RuntimeType* type;
        Property properties[maxSize];
    };
    const PropertiesWithSupertypes* propertiesWithSupertypes(VM&, Structure*, const RuntimeType&, unsigned part);
    void setPropertiesWithSupertypes(VM&, Structure*, const PropertiesWithSupertypes&);
    std::optional<bool> mayConvertNestedValue(JSValue, const RuntimeType& ofField, bool leavesUntypedObjects) const;
    bool isUnchangedByCoercion(VM&, JSObject*, const RuntimeType&);
    template<unsigned places> static unsigned placeOf(uint64_t key)
    {
        static_assert(hasOneBitSet(places));
        return WTF::intHash(key) % places;
    }
    template<unsigned maxEntries> struct AdaptiveIndexMask {
        static_assert(maxEntries >= 64);
        unsigned placeOf(uint64_t key) const { return State::placeOf<maxEntries>(key) & mask; }
        size_t count() const { return static_cast<size_t>(mask) + 1; }
        void didReplaceEntry()
        {
            if (++evictionCount <= mask || count() == maxEntries)
                return;
            mask = mask * 2 + 1;
            evictionCount = 0;
        }
        unsigned mask { 63 };
        unsigned evictionCount { 0 };
    };
    static constexpr unsigned propertyConversionCacheSize = 256;
    uintptr_t& propertyConversionCacheEntry(const RuntimeType& type) const { return m_propertyConversionCache[placeOf<propertyConversionCacheSize>(std::bit_cast<uintptr_t>(&type))]; }
    void captureBuiltinsFrom(ASCIILiteral global, JSObject*);
    void captureRealmBuiltins();
    bool realmIsLocked { true };
    Structure* compactTableStructure();

private:
    struct OrderTable {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OrderTable);
        using Map = UncheckedKeyHashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        Vector<Vector<uint16_t>> added;
        Map byPrefix;
        bool hasExpectedOrderPrefixes { false };
        std::optional<bool> everyFieldIsOrdinaryProperty;
        std::optional<uint32_t> initialInstanceOrderWord;
        Map transitions;
        UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placements;
        UncheckedKeyHashMap<uint32_t, JSCellButterfly*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keyLists;
        UncheckedKeyHashMap<uint32_t, FixedVector<FieldAsProperty>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> propertyLists;
        UncheckedKeyHashMap<uint32_t, FixedVector<FieldAsProperty>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> propertyListsWithSymbolKeys;
        UncheckedKeyHashMap<uint32_t, std::unique_ptr<KeysAsJSON>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keysAsJSON;
        UncheckedKeyHashMap<uint32_t, PlainCopyPlan, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> plainCopyPlans;
        UncheckedKeyHashMap<uint32_t, Vector<std::unique_ptr<PlainCopyExcludingPlan>, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> plainCopyExcludingPlans;
        uint32_t lastPlainCopyOrderWord { UINT32_MAX };
        PlainCopyPlan lastPlainCopy;
        uint32_t lastOrderWord { UINT32_MAX };
        std::span<const FieldAsProperty> lastProperties;
        uint32_t lastKeysAsJSONOrderWord { UINT32_MAX };
        const KeysAsJSON* lastKeysAsJSON { nullptr };
        struct ExtraKeysInOrder {
            uint16_t count { 0 };
            uint16_t end { 0 };
        };
        static_assert(sizeof(ExtraKeysInOrder) == 2 * sizeof(uint16_t));
        Vector<ExtraKeysInOrder> extraKeysInAddedOrders;
        Vector<uintptr_t> orderKeys;
        Vector<Vector<uint16_t>> slotsInOrder;
        Vector<uint16_t> fieldsBySlot;
        Vector<uint16_t> fieldsByExtensionSlot;
    };

    void didCreate(JSCell*);
    UncheckedKeyHashMap<uint64_t, RegExp*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_regExpLiterals;
    Structure* m_compactTableStructure { nullptr };
    bool hasText(uint32_t string, StringView) const;
    bool isName(uint32_t string, UniquedStringImpl*) const;
    void fill(VM&, uint32_t allocation);
    uint32_t structureIDOf(uint32_t allocation) const;
    uint32_t m_numberOfAllocationEntries { 0 };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_structureIDsOfAllocationsWithoutEntry;
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base()); }
    bool isImmutableIn(std::span<const MethodData>, UniquedStringImpl*, bool& isDeclared) const;
    OrderTable& orderTableFor(const LayoutData&);
    void didChangeOrder(const LayoutData&, OrderTable&, uint32_t order);
    void didFindTransition(const LayoutData&, OrderTable&, uint32_t order, uint32_t count, uint16_t field, uint32_t next);
    JSValue convert(JSGlobalObject*, JSValue, const RuntimeType&, AuditSite);
    JSValue nearestValueOfType(const RuntimeType&, JSValue) const;
    void reportCoercion(AuditSite, const String& what);
    std::span<Structure* const> closureStructures(VM&);
    Vector<Structure*, 2> m_environmentStructures;
    void setGeneratorPrototype(VM&, uint32_t resumeFunction, bool isAsync, JSObject*);
    uint32_t resumeFunctionAndFlagOf(uint32_t generatorFunction) const;

    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_reportedLocations;
    struct Counts;
    void countFieldsOf(Object*);
    void printCounts() const;
    static void printCountsAtExit();
    std::unique_ptr<Counts> m_counts;
    Structure* m_closureStructures[classObjectClosureStructure + 1] { };
    Structure* m_closureWithPrototypeStructure { nullptr };
    Structure* m_slimClosureStructures[numberOfSlimClosureKinds] { };
    Structure* m_reifiedSlimClosureStructures[2][3] { };
    Structure* m_boundClosureStructures[2] { };
    RefPtr<PrivateSymbolImpl> m_functionIndexPrivateName;
public:
    Structure* asyncBoundFunctionStructure { nullptr };
    void setAsyncBoundFunctionStructure(Structure*);
    void didAddPrototypeToClosure(Structure*);
private:
    JSObject* m_slimClosureLazyPropertyHolder { nullptr };
    void didCreateClosureFor(uint32_t function, JSCell*);
    Structure* m_prototypeObjectStructure { nullptr };
    Structure* m_moduleNamespaceStructure { nullptr };
    struct BuiltinModule {
        enum class Outcome : uint8_t { None, Exports, Thrown };
        Outcome outcome { Outcome::None };
        EncodedJSValue value { 0 };
    };
    UncheckedKeyHashMap<uint32_t, BuiltinModule, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_builtinModules;
    friend JSValue evaluateBuiltinModule(JSGlobalObject*, uint32_t);
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_inlineCapacitiesForThis;
    Vector<NamedPropertiesTransition> m_namedPropertiesTransitions;
    size_t m_lastNamedPropertiesTransitionIndex { 0 };
    UncheckedKeyHashSet<String> m_reportedRules;
    bool m_hasIgnoredRule { false };
    UncheckedKeyHashMap<uint32_t, GeneratorStructure, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_generatorStructures;
    Vector<Generator::Template> m_generatorTemplates;
    std::array<Structure*, 2> m_realmGeneratorStructures { };
    Structure* m_savedFrameStructure { nullptr };
    struct ClosureGeneratorStructure {
        GeneratorStructure structure;
        uint32_t epoch;
    };
    static constexpr unsigned maxClosureGeneratorStructures = 256;
    UncheckedKeyHashMap<Closure*, ClosureGeneratorStructure> m_closureGeneratorStructures;
    HeapVersion m_generatorStructuresVersion { 0 };
    struct InitialObjectState {
        Closure* constructor { nullptr };
        JSObject* newTarget { nullptr };
        Structure* structure { nullptr };
        uint32_t epoch { 0 };
        Vector<uint8_t> body;
    };
    std::array<InitialObjectState, 16> m_initialObjectStatesByNewTarget;
    void shareWithStub(VM&, const InitialObjectState&);
    void forgetInitialObjectStates();
    HeapVersion m_initialObjectStatesVersion { 0 };
    JSFunction* m_liveBindingGetter { nullptr };
    uint64_t m_asyncGeneratorStubEntryResults[16] { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };
    CustomGetterSetter* m_liveBindingAccessor { nullptr };
    Strong<Structure> m_evalScopeStructure;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nameIndices;
    UncheckedKeyHashMap<std::pair<const LayoutData*, UniquedStringImpl*>, uint32_t> m_privateMethods;
    SegmentedVector<Identifier, 128> m_names;
    const Identifier& nameSlow(VM&, uint32_t);
    UncheckedKeyHashMap<std::pair<uint32_t, RefPtr<UniquedStringImpl>>, Vector<uint32_t, 1>> m_specializationsWithField;
    UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, Vector<uint16_t, 2>> m_requiredFieldsAdded;
    std::optional<uint32_t> findFieldNameIndex(UniquedStringImpl*) const;
    std::optional<uint32_t> fieldNameIndex(UniquedStringImpl*) const;
    struct CachedFieldName {
        UniquedStringImpl* name;
        uint32_t numberPlusOne;
    };
    static constexpr unsigned numberOfCachedFieldNames = 1024;
    CachedFieldName* m_cachedFieldNames { nullptr };
    const FieldData* findField(VM&, const LayoutData&, UniquedStringImpl*);
    struct CachedField {
        uint32_t shape;
        UniquedStringImpl* name;
        const FieldData* field;
    };
    static constexpr unsigned numberOfCachedFields = 4096;
    CachedField* m_cachedFields { nullptr };
    UncheckedKeyHashMap<std::pair<Structure*, uint32_t>, std::unique_ptr<TransitionPlan>> m_transitionPlans;
    UncheckedKeyHashMap<std::tuple<uint32_t, uint32_t, uint32_t>, Vector<std::unique_ptr<CopyPlan>, 1>> m_copyPlans;
    static constexpr unsigned maxCoercionPlans = 4096;
    static constexpr unsigned maxCoercionPlansByDiscriminant = 16384;
    unsigned m_coercionPlansByDiscriminantCount { 0 };
    UncheckedKeyHashMap<std::pair<Structure*, const RuntimeType*>, std::unique_ptr<CoercionPlan>> m_coercionPlans;
    bool m_hasNewCoercionPlans { false };
    bool m_hasCachedExtraPropertyReads { false };
    bool m_hasCachedLateFieldCreations { false };
    bool m_propertyListCacheIsDirty { false };
    struct CheckedPrototypeChain {
        uint32_t shapePlusOne { 0 };
        StructureID extraProperties;
        std::array<StructureID, 5> structures { };
        friend bool operator==(const CheckedPrototypeChain&, const CheckedPrototypeChain&) = default;
    };
    std::array<CheckedPrototypeChain, 16> m_checkedPrototypeChains { };
    bool m_hasCheckedPrototypeChains { false };
    std::unique_ptr<std::array<CachedPropertyList, propertyListCacheSize>> m_propertyListCache;
    static constexpr unsigned numberOfAssignmentPlans = 64;
    std::unique_ptr<std::array<AssignmentPlan, numberOfAssignmentPlans>> m_assignmentPlans;
    bool m_hasAssignmentPlans { false };
    void fillAssignmentPlan(VM&, AssignmentPlan&, Object& target, JSObject* extraProperties, JSObject* source);
    struct LastCoercionPlanLookup {
        Structure* structure { nullptr };
        const RuntimeType* type { nullptr };
        const CoercionPlan* plan { nullptr };
    };
    mutable LastCoercionPlanLookup m_lastCoercionPlanLookup;
    TransitionPlan m_planToStay;
    UncheckedKeyHashMap<MergeKey, std::unique_ptr<MergePlan>> m_mergePlans;
    TransitionPlan* transitionPlan(JSGlobalObject*, Object*, const RuntimeType&, Format::CastReason);
    struct CastOutcome {
        TransitionPlan* plan;
        DiscriminatingField* discriminatingField;
    };
    CastOutcome castOutcome(JSGlobalObject*, Object*, const RuntimeType&, Format::CastReason, EncodedJSValue discriminant, bool isDiscriminantKey);
    static std::optional<EncodedJSValue> discriminantKeyFor(const DiscriminatingField&, JSValue);
    Vector<std::unique_ptr<DiscriminatingField>> m_discriminatingFields;
    struct CachedTransition {
        Structure* structure;
        const RuntimeType* type;
        EncodedJSValue discriminant;
        Format::CastReason reason;
        CastOutcome outcome;
    };
    static constexpr unsigned maxNumberOfOutcomes = 1 << 16;
    static constexpr unsigned numberOfCachedTransitions = 4096;
    CachedTransition* m_cachedTransitions { nullptr };
    AdaptiveIndexMask<numberOfCachedTransitions> m_cachedTransitionIndices;
    struct PreviousTransition {
        Structure* structure;
        const RuntimeType* type;
        TransitionPlan* plan;
        Format::CastReason reason;
    };
    static constexpr unsigned numberOfPreviousTransitions = 1024;
    PreviousTransition* m_previousTransitions { nullptr };
    AdaptiveIndexMask<numberOfPreviousTransitions> m_previousTransitionIndices;
    CastTransition* m_castTransitions { nullptr };
    UncheckedKeyHashMap<const TransitionPlan*, std::unique_ptr<Vector<StoreTransition::Cleared, 4>>> m_planClearedSlots;
    void cacheCastTransition(Structure* before, uint32_t typeAndReason, uint64_t wordsBefore, Object*, const TransitionPlan&);
    PreviousTransition& previousTransition(Structure* structure, const RuntimeType& type, Format::CastReason reason) { return m_previousTransitions[m_previousTransitionIndices.placeOf(std::bit_cast<uintptr_t>(structure) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(&type)), 23) ^ static_cast<uint64_t>(reason) << 57)]; }
    bool m_lastPlanDependsOnlyOnStructure { false };
    UncheckedKeyHashMap<std::tuple<Structure*, const RuntimeType*, EncodedJSValue, unsigned>, CastOutcome> m_castOutcomes;
    using ListedFields = Vector<std::pair<const FieldData*, const FieldData*>, 0>;
    const ListedFields& listedFieldsFor(VM&, const LayoutData& from, const LayoutData& to);
    UncheckedKeyHashMap<uint64_t, ListedFields> m_listedFields;
    bool hasListedFields(Object*, const ListedFields&) const;
    struct StoreSpecialization {
        const FieldData* created;
        TransitionPlan* plan;
        ListedFields listed;
        mutable Vector<std::pair<uint32_t, uint32_t>, 2> orderWords { };
    };
    struct StoreSpecializations {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StoreSpecializations);
        Vector<StoreSpecialization, 1> transitions;
        bool dependOnlyOnPrototypeChain { false };
    };
    bool store(VM&, Object*, const StoreSpecializations&, JSValue);
    UncheckedKeyHashMap<std::tuple<Structure*, RefPtr<UniquedStringImpl>, EncodedJSValue>, std::unique_ptr<StoreSpecializations>> m_storeSpecializations;
    UncheckedKeyHashMap<std::pair<const TransitionPlan*, const FieldData*>, Vector<std::unique_ptr<StoreTransition>, 1>> m_storeTransitions;
    UncheckedKeyHashMap<std::pair<Structure*, const FieldData*>, Vector<std::unique_ptr<StoreTransition>, 1>> m_fieldCreatingStoreTransitions[2];
    UncheckedKeyHashMap<std::pair<Structure*, const FieldData*>, Vector<std::unique_ptr<StoreTransition>, 1>> m_lateFieldCreationStoreTransitions[2];
    UncheckedKeyHashMap<std::pair<Structure*, const FieldData*>, Vector<std::unique_ptr<StoreTransition>, 1>> m_storageCreatingStoreTransitions;
    struct CachedStore {
        Structure* structure;
        UniquedStringImpl* name;
        EncodedJSValue discriminant;
        const StoreSpecializations* transitions;
    };
    static constexpr unsigned numberOfCachedStores = 1024;
    CachedStore* m_cachedStores { nullptr };
    AdaptiveIndexMask<numberOfCachedStores> m_cachedStoreIndices;
    bool isAcceptedUnchanged(JSObject*, const RuntimeType&) const;
    uintptr_t* m_propertyConversionCache { nullptr };
    static constexpr unsigned numberOfKeepFailures = 64;
    KeepFailure m_keepFailures[numberOfKeepFailures] { };
    HeapVersion m_keepFailuresVersion { 0 };
    static constexpr unsigned numberOfPropertiesWithSupertypes = 64;
    static constexpr unsigned propertiesWithSupertypesWays = 4;
    std::span<PropertiesWithSupertypes> propertiesWithSupertypesCandidates(VM&, Structure*, const RuntimeType&, unsigned part);
    PropertiesWithSupertypes* m_propertiesWithSupertypes { nullptr };
    HeapVersion m_propertiesWithSupertypesVersion { 0 };
    unsigned m_nextPropertiesWithSupertypesVictim { 0 };
    CachedStore& cachedStore(Structure* structure, UniquedStringImpl* name, EncodedJSValue discriminant) { return m_cachedStores[m_cachedStoreIndices.placeOf(std::bit_cast<uintptr_t>(structure) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(name)), 23) ^ static_cast<uint64_t>(discriminant) * 31)]; }
    UncheckedKeyHashSet<Structure*> m_planStructures;
    UncheckedKeyHashMap<uint32_t, OrderTable*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_orderTables;
    Vector<std::unique_ptr<OrderTable>> m_createdOrderTables;
    std::array<std::pair<uint32_t, OrderTable*>, 16> m_recentOrderTables { };
    const void* fieldTemplate(const LayoutData&, uint32_t cellSize);
    UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, MallocSpan<uint8_t>> m_fieldTemplates;
    WeakGCMap<std::pair<uint32_t, JSObject*>, Structure> m_structuresWithOtherPrototype;
    WeakGCMap<std::tuple<uint32_t, JSObject*, Structure*>, Structure> m_structuresByExtraProperties;
    static constexpr unsigned recentStructureIndexBits = 7;
    static unsigned recentStructureIndex(uint32_t shape, Structure& ofExtraProperties) { return (ofExtraProperties.id().bits() * 0x9e3779b1u ^ shape * 0x85ebca6bu) >> (32 - recentStructureIndexBits); }
    std::array<StructureID, 1u << recentStructureIndexBits> m_recentStructuresByExtraProperties { };
    static constexpr unsigned requestedStructureIndexBits = 12;
    static unsigned requestedStructureIndex(Structure& ofExtraProperties) { return ofExtraProperties.id().bits() * 0x9e3779b1u >> (32 - requestedStructureIndexBits); }
    WTF::BitSet<1u << requestedStructureIndexBits> m_requestedExtraPropertiesStructures;
    bool descendantsAddNoProperties(JSObject* prototype, JSObject* classPrototype);
    UncheckedKeyHashMap<JSObject*, JSObject*> m_classPrototypeByDescendantPrototype;
    UncheckedKeyHashSet<JSObject*> m_descendantPrototypes;
    bool holdsNoFieldOf(const LayoutData&, JSObject* prototype);
    UncheckedKeyHashMap<std::pair<JSObject*, uint32_t>, bool> m_prototypesHoldingNoField;
    UncheckedKeyHashMap<JSObject*, Vector<uint32_t, 2>> m_shapesUnderPrototype;
    UncheckedKeyHashSet<JSObject*> m_modifiedDescendantPrototypes;
    Vector<std::unique_ptr<WatchedMembers>> m_watchedMembers;
    JSCell* stringIfCreated(uint32_t) const;
    void setString(uint32_t, JSCell*);
    template<typename Visitor> void visitStrings(Visitor&);
    uint32_t m_createdStringCount { 0 };
    std::array<RefPtr<StringImpl>, 1 << literalStringCacheIndexBits> m_literalStringCacheKeys;
    void createStatics();
    void destroyStatics();
    void initializeStatics(uint32_t firstWord, uint32_t endWord);
    void initializeProgramStatics();
    bool m_programStaticsInitialized { false };
    uint32_t m_inaccessibleStaticsSize { 0 };
    Vector<std::unique_ptr<GlobalPropertyWatchpoint>> m_globalPropertyWatchpoints;
    Vector<std::unique_ptr<GlobalPropertyWatchpoint>> m_memberHolderWatchpoints;
    Vector<uint64_t*> m_retiredStringTables;
    Vector<uint64_t> m_cachedKeys;
    size_t m_cachedKeyEvictionCount { 0 };
    std::array<uint64_t, 256> m_caseConvertedStrings { };
    Vector<KeyByParts> m_keysByParts;
    size_t m_keysByPartsEvictionCount { 0 };
    void cacheFirstKeyByParts(const KeyByParts&, JSString*);
    void growKeysByParts();
    Vector<JSCell*> m_wideStrings;
    UncheckedKeyHashMap<uint32_t, JSCell*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_bigIntConstants;
    Vector<std::pair<String, JSObject*>> m_moduleImportMetas;
    struct RecentlyCreated {
        template<typename Visitor, typename Functor> bool visit(VM&, Visitor&, const Functor& handOver);

        Vector<uint32_t> cells;
        Vector<uint32_t> earlier;
        size_t numberVisited { 0 };
        HeapVersion lastVisitVersion { 0 };
    };
    RecentlyCreated m_recentStrings;
    RecentlyCreated m_recentStaticDataStrings;
    bool m_isLastStringConstantBackedByStaticData { false };
    template<typename Visitor> void visitClosures(Visitor&);
    RecentlyCreated m_recentClosures;
    static constexpr unsigned closuresPerChunkShift = 10;
    BitVector m_chunksWithClosures;
    BitVector m_prototypeShapes;
    Lock m_cellsLock;
    Vector<JSCell*> m_cells WTF_GUARDED_BY_LOCK(m_cellsLock);
    size_t m_numberOfCellsVisited WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    size_t m_previouslyMarkedCellCount WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    HeapVersion m_lastVisitVersion WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    HeapVersion m_lastFullCardVisitVersion { 0 };
    template<typename Visitor> void visitRootsWithCards(Visitor&);
    JSCell** m_youngRoots { nullptr };

public:
    struct ConstructionFromNative {
        const CallFrame* topCallFrame;
        ConstructionFromNative* previous;
    };
    ConstructionFromNative* constructionsFromNative { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
