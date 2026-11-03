/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTLayoutData.h"
#include "AOTObjectVisiting.h"
#include "AOTPropertyList.h"
#include "AOTPropertyWalk.h"
#include "AOTSlotAccess.h"
#include "AOTVector.h"
#include "JSObject.h"
#include <algorithm>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class HeapAnalyzer;
class JSArray;
class JSCellButterfly;
class JSPropertyNameEnumerator;

namespace AOT {

class Extension;
class RuntimeLayoutTable;
struct Instance;
struct State;

enum class SlotExitReason : uint8_t { Delete, Redefinition, StoreOfOtherType };

using PropertyWalk::isCompiledObject;

// What RuntimeLayoutTable::slotAdmits() lets into the slot of one field.
class AcceptedValue {
public:
    static AcceptedValue provenAtBuildTime(JSValue value) { return AcceptedValue(value); }

    JSValue value() const { return m_value; }

private:
    friend class RuntimeLayoutTable;

    explicit AcceptedValue(JSValue value)
        : m_value(value)
    {
    }

    JSValue m_value;
};

// An entry of a key order below the number of fields lists that field.
class ExtraKeyCounter {
public:
    explicit ExtraKeyCounter(uint32_t numberOfFields)
        : m_numberOfFields(numberOfFields)
    {
    }

    uint32_t count() const { return m_count; }

    void add(uint16_t entry)
    {
        ASSERT(entry >= m_numberOfFields && entry <= maxKeyOrderEntry);
        uint32_t count = m_countOfFullEntries + entry - m_numberOfFields + 1;
        if (entry == maxKeyOrderEntry)
            m_countOfFullEntries = count;
        m_count = std::max(m_count, count);
    }

    // What add() makes of each entry of the list that lists extra keys. Without a full entry among them each counts on top of the same number, so the largest says it all.
    void addAll(std::span<const uint16_t> entries)
    {
        uint16_t largest = 0;
        for (uint16_t entry : entries)
            largest = std::max(largest, entry);
        if (largest < m_numberOfFields)
            return;
        if (largest < maxKeyOrderEntry) {
            add(largest);
            return;
        }
        for (uint16_t entry : entries) {
            if (entry >= m_numberOfFields)
                add(entry);
        }
    }

    void appendEntriesCountingTo(uint32_t count, auto& entries)
    {
        while (m_count < count) {
            uint16_t entry = static_cast<uint16_t>(std::min<uint64_t>(static_cast<uint64_t>(m_numberOfFields) + count - m_countOfFullEntries - 1, maxKeyOrderEntry));
            entries.append(entry);
            add(entry);
        }
    }

private:
    uint32_t m_numberOfFields;
    uint32_t m_countOfFullEntries { 0 };
    uint32_t m_count { 0 };
};

class Object final : public JSObject {
public:
    using Base = JSObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    template<typename Visitor> ALWAYS_INLINE static void visitFields(Object*, Visitor&);
    static size_t estimatedSize(JSCell*, VM&);
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

    static CompiledStructure* createStructure(VM&, const CompiledStructure::ObjectParameters&);
    static Object* create(VM&, Structure*, const LayoutData&, size_t size, uint32_t orderWord);
    // The bytes that create() left behind the header of an earlier object of the Structure's layout, order word and shape word among them.
    static Object* createAsAtBirth(VM&, Structure*, std::span<const uint8_t> bodyAtBirth, bool isBornUnfinished);
    static Object* createWithBody(VM&, Structure*, std::span<const uint8_t> body);

    const CompiledStructure& compiledStructure() const { return *uncheckedDowncast<CompiledStructure>(structure()); }
    const LayoutData& layout() const { return compiledStructure().layout(); }
    Instance& instance() const;
    State& state() const;
    // Whether the tables of the state speak of the object: it is of that realm and, where the realm runs several images, of that image.
    bool isDescribedBy(const State&) const;
    // An object may lie in a frame, where its address leads to no block of the heap and says nothing about the cage.
    bool isInFrame() const { return static_cast<uint8_t>(cellState()) == frameCellState; }
    VM& vm() const;

    template<typename T> T& at(uint32_t offset) const { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<Object*>(this)) + offset); }
    uint32_t orderWord() const { return at<uint32_t>(offsetOfOrderWord); }
    uint32_t shapeWord() const { return at<uint32_t>(offsetOfShapeWord); }
    uint32_t shape() const { return shapeOf(shapeWord()); }
    uint64_t orderAndShapeWords() const { return static_cast<uint64_t>(shapeWord()) << 32 | orderWord(); }
    bool hasFlagsInOrderWord() const { return orderWord() & ~orderAndCountMask; }
    bool isFrozen() const { return orderWord() & orderWordIsFrozen; }
    bool isSealed() const { return orderWord() & orderWordIsSealed; }
    bool isNotExtensible() const { return orderWord() & orderWordIsNotExtensible; }
    bool hasExtraProperties() const { return orderWord() & orderWordHasExtraProperties; }
    bool hasOutOfLineKeyOrder() const { return isOrderStoredOutOfLine(orderWord()); }
    // Keys come to a dictionary and go without another Structure, so an order that objects share cannot count them. Such an order names the last extra key in front of a field: it changes only when that key goes.
    bool hasKeyOrderOfItsOwn() const { return hasOutOfLineKeyOrder() && compiledStructure().hasKeyOrderOfOneObject(); }
    void setOrderAndCount(uint32_t orderAndCount) { at<uint32_t>(offsetOfOrderWord) = (orderWord() & ~orderAndCountMask) | orderAndCount; }
    unsigned attributesOfPublicFields() const
    {
        uint32_t word = orderWord();
        if (word & orderWordIsFrozen) [[unlikely]]
            return PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
        return static_cast<unsigned>(word & orderWordIsSealed ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    }
    unsigned attributesOf(const FieldData& field) const
    {
        if (field.isPrivate())
            return static_cast<unsigned>(PropertyAttribute::None);
        unsigned attributes = field.attributes() | attributesOfPublicFields();
        return field.isAccessor() ? attributes & ~PropertyAttribute::ReadOnly : attributes;
    }

    inline const FieldData* fieldNamed(State&, PropertyName) const;
    uint16_t indexOf(const FieldData& field) const { return layout().indexOf(field); }
    void* slotOf(const FieldData&) const;
    bool isInSlot(const FieldData&) const;
    JSValue read(const FieldData&) const;
    JSValue readInline(const FieldData&) const;
    // stringForWordInSlot() also reads a literal-string word here.
    JSValue readInline(const FieldData&, const auto* strings) const;
    // Calls the functor with an int32_t, a bool or a double where the slot holds one, and with a JSValue otherwise, which is empty for an absent field.
    template<typename Functor, typename Strings = std::nullptr_t> auto readInlineUnboxed(const FieldData&, const Functor&, Strings = nullptr) const;
    bool tryStore(VM&, const FieldData&, JSValue);
    bool tryInitialize(VM&, const FieldData&, JSValue);
    bool store(VM&, const FieldData&, AcceptedValue);
    void clear(const FieldData&);

    Extension* extension() const;
    void setExtension(VM&, Extension*);
    void ensureStorageFor(VM&, const FieldData&);
    void ensureExtensionGranules(VM&, uint64_t granuleBits);
    void ensureExtensionGranules(VM&, uint64_t granuleBits, bool collectorMayVisit);

    JSValue materializeDeferredClosure(VM&, const FieldData&);
    void materializeDeferredClosures(VM&);

    struct LeavingProperty {
        JSValue valueOrAccessor;
        unsigned attributes { 0 };
    };
    void moveFieldToExtraProperties(VM&, const FieldData&, LeavingProperty, SlotExitReason);
    // False where the object is not in the state that the other object was in, or the slot takes the value.
    bool tryRepeat(VM&, const FieldExit&, JSValue);
    // False where the object is not in the state that the other object was in.
    bool tryRepeat(VM&, const AddedExtraProperty&, JSValue);
    // `Object.defineProperty(object, name, descriptor)` with a descriptor that is an object: what it describes is added as to the last object in the same state, or nothing is done.
    bool tryDefineAsOnTheLastObject(VM&, PropertyName, const PropertyDescriptor&);
    bool tryMoveFieldBackToSlot(VM&, const FieldData&);
    void deleteField(VM&, const FieldData&);
    bool hasEveryFieldOfBirth() const;
    JS_EXPORT_PRIVATE void tryFinishBirth();

    enum class Integrity : uint8_t { NotExtensible, Sealed, Frozen };
    void setIntegrity(JSGlobalObject*, Integrity);
    bool testIntegrity(VM&, Integrity) const;
    void setPrototypeKeepingShape(VM&, JSValue prototype);
    void setPrototypeAtCreation(VM&, JSValue prototype);

    // The caller may know that no object on the prototype chain has an accessor or a read-only property with this name.
    static bool putWithKnownField(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&, const FieldData*, bool chainLetsStoresPass = false);
    JSValue getDirectFromNativeCode(VM&, PropertyName, unsigned* attributes);
    // False with a TypeError thrown, and the object as it was, where the layout keeps the field from what the caller asks for. A caller that looks at neither meets the error at its next check.
    bool putDirectFromNativeCode(VM&, PropertyName, JSValue, unsigned attributes);
    // The same, but a property that exists keeps its attributes.
    bool storeDirectFromNativeCode(VM&, PropertyName, JSValue);
    bool tryDefineAbsentExtraProperty(VM&, PropertyName, JSValue valueOrAccessor, unsigned attributes);
    // The caller has already looked up the name: `field` is fieldNamed(name).
    bool tryDefineAbsentProperty(VM&, const FieldData*, PropertyName, JSValue valueOrAccessor, unsigned attributes);
    // What the properties that no field holds say of a name that is the name of no field.
    struct ExtraPropertyFound {
        enum class Kind : uint8_t { Absent, MayBeRedefinedInEveryWay, Other };
        Kind kind;
        LeavingProperty property;
    };
    ExtraPropertyFound findExtraProperty(VM&, PropertyName) const;
    bool tryAddAbsentExtraProperty(VM&, PropertyName, JSValue valueOrAccessor, unsigned attributes);
    void replaceExtraProperty(VM&, PropertyName, LeavingProperty);
    bool tryDefineAbsentGetterInSlot(VM&, JSGlobalObject*, UniquedStringImpl* name, JSObject* getter, unsigned attributes);
    bool tryDefineAbsentGetterInSlot(VM&, JSGlobalObject*, const FieldData&, UniquedStringImpl* name, JSObject* getter, unsigned attributes);
    // The keys are strings, each followed by its function. Bit i of `getters` says that function i is a getter. True if all of them now lie in slots. Otherwise the object is as it was.
    bool tryDefineAccessorsOfLiteral(VM&, JSGlobalObject*, std::span<const EncodedJSValue> keysAndFunctions, uint32_t getters, uint32_t enumerable);
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

    JSObject* extraProperties() const { return hasExtraProperties() ? extraPropertiesSlow() : nullptr; }
    JSObject* extraPropertiesSlow() const;
    JSObject* ensureExtraProperties(VM&);
    void adoptExtraProperties(VM&, JSObject* filled);
    void setExtraProperties(VM&, JSObject*, CompiledStructure* withExtraProperties);
    struct ExtraProperty {
        UniquedStringImpl* name;
        JSValue value;
    };
    static JSObject* createExtraProperties(JSGlobalObject* realm, std::span<const ExtraProperty> inOrder);
    // For an object that nobody has seen yet and names that are none of its fields. False where the layout has no place for extra properties.
    bool tryCreateExtraPropertiesOfNewObject(VM&, std::span<const ExtraProperty> inOrder);
    void didChangeExtraProperties(VM&);
    void ensureStructureForExtraProperties(VM&);
    static bool ensureStructureForExtraPropertiesIfNeeded(VM&, JSCell*);
    enum class ExtraPropertyStore : uint8_t { NotHandled, Replaced, Added };
    enum class ExtraPropertyStoreFlag : uint8_t {
        IsKnownNotToBeField = 1,
        IsDefinition = 2,
        NamesExtraPropertiesStructure = 4,
        IsKnownToBeAbsent = 8,
        // The name is no index, no private name and not "__proto__", and the program does not spell it: it names no field and no member of a class.
        IsMadeAtRunTime = 16,
    };
    struct ExtraPropertyStoreResult {
        ExtraPropertyStore kind { ExtraPropertyStore::NotHandled };
        PropertyOffset offset { invalidOffset };
    };
    ExtraPropertyStoreResult tryStoreExtraProperty(VM&, PropertyName, JSValue, OptionSet<ExtraPropertyStoreFlag>);
    // False for the first object that gets this property after the extra properties it has. Most such objects stay the only one.
    bool repeatsExtraPropertyStore(VM&, PropertyName) const;
    void willAddExtraProperty(VM&, PropertyName, bool isSpelledByProgram = true);

    using KeyOrderStorage = Vector<uint16_t, 16>;
    std::span<const uint16_t> keyOrder(State&, KeyOrderStorage&) const;
    void forEachKeyInOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
    {
        KeyOrderStorage storage;
        for (uint16_t key : keyOrder(state, storage))
            functor(key);
    }
    static size_t forEachPositionedKey(std::span<const uint16_t> entries, uint32_t numberOfFields, size_t numberOfExtraKeys, const Invocable<void(uint16_t)> auto& fieldFunctor, const Invocable<void(size_t)> auto& extraKeyFunctor)
    {
        size_t nextExtraKey = 0;
        ExtraKeyCounter positioned(numberOfFields);
        for (uint16_t entry : entries) {
            if (entry < numberOfFields) {
                fieldFunctor(entry);
                continue;
            }
            positioned.add(entry);
            for (size_t end = std::min<size_t>(positioned.count(), numberOfExtraKeys); nextExtraKey < end; ++nextExtraKey)
                extraKeyFunctor(nextExtraKey);
        }
        return nextExtraKey;
    }
    void forEachFieldInKeyOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    bool isInKeyOrder(State&, const FieldData&) const;
    std::optional<uint32_t> numberOfExtraKeysByStructure() const;
    void willCreateField(State&, const FieldData&);
    void didCreateField(State&, const FieldData&, bool withStorage = true);
    void setKeyOrder(VM&, State&, std::span<const uint16_t> keys);
    bool trySetNumberedKeyOrder(VM&, State&, std::span<const uint16_t> keys);
    struct InitialKeyOrder {
        uint32_t orderAndCount;
        CompiledStructure* structure;
    };
    std::optional<InitialKeyOrder> initialKeyOrder(VM&, std::span<const uint16_t> keys, JSObject* extraProperties);
    void setInitialKeyOrder(VM&, InitialKeyOrder, JSObject* extraProperties);
    Vector<UniquedStringImpl*, 16> extraKeyNames() const;
    struct PlaceOfExtraKey {
        // A number that is below the count of exactly those entries of the key order that count the key.
        std::optional<uint32_t> ordinal;
        // Which of the keys that the order names it is, and the extra key in front of it.
        std::optional<uint32_t> indexAmongNamedKeys;
        UniquedStringImpl* extraKeyInFront { nullptr };
        bool orderCountsExtraKeys { false };
    };
    PlaceOfExtraKey placeOfExtraKey(PropertyName);
    bool keyOrderCountsExtraKeys() const;
    // The key that placeOfExtraKey() was last asked for is gone from the extra properties.
    void didRemoveExtraKey(VM&, State&, const PlaceOfExtraKey&);
    void forgetPlacesOfPositionedKeys(State&);
    void takeKeyOrderOfItsOwn(VM&, State&);

    bool tryCopyFieldsOf(VM&, const Object& source);
    static Object* tryCreateCopyOfFields(VM&, const Object& source);
    // To make a plan costs several copies by name. A caller that can copy by name asks for one only from the second copy between the same shapes on.
    bool tryAssignPropertiesOf(VM&, JSObject* source, std::span<UniquedStringImpl* const> excludedNames = { });
    // Defines data properties in this order where each is a field that takes its value. The key order is looked up once, however many fields are created.
    // With attributes: a property that is absent and whose attributes are not those of its field is listed where it stands and goes to the extra properties.
    bool tryDefineFields(VM&, std::span<UniquedStringImpl* const> names, std::span<const EncodedJSValue> valuesOrAccessors, std::span<const unsigned> attributes = { });

    enum class SymbolKeys : bool { Skip, Include };
    bool forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>&, SymbolKeys = SymbolKeys::Skip) const;
    // The fields with string keys in key order. Nothing for an object that has extra properties or whose order has no number.
    std::optional<std::span<const FieldAsProperty>> fieldsInKeyOrder() const;
    JSValue readListedField(const FieldAsProperty&) const;
    const Vector<PropertyListEntry>* fieldsAndExtraPropertiesInKeyOrder(VM&) const;
    JSValue readPropertyListEntry(const PropertyListEntry&, JSObject* extraProperties) const;
    JSCellButterfly* keys(VM&, KeyListKind = KeyListKind::Strings) const;
    JSCellButterfly* keysWithExtraProperties(VM&) const;
    JSArray* tryCreateKeysArrayWithExtraProperties(JSGlobalObject*) const;
    JSPropertyNameEnumerator* propertyNameEnumerator(JSGlobalObject*);
    bool keysDependOnlyOnStructureAndOrderWord() const;

    static JSValue tryGetOwnDataProperty(VM&, JSCell*, PropertyName);
    static bool structureGuaranteesAbsence(Structure*, UniquedStringImpl*);

private:
    Object(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    bool store(VM&, const FieldData&, JSValue);
    void willStoreMarker(VM&, const FieldData&, JSValue marker);
    void copyFieldsOfSameLayout(VM&, const Object& source);
    bool tryCopyWordsOfSameLayout(VM&, const Object& source);
    static Object* createCopyOfWords(VM&, const Object& source, Structure*);
    static Object* tryCreateCopyOfFieldsAndExtraProperties(VM&, const Object& source);
    std::optional<std::span<const FieldAsProperty>> fieldsInKeyOrderSlow() const;
    void removeExtraKeyFromOrder(VM&, State&, uint32_t ordinal);
    void didRemoveCountedExtraKey(State&);
    bool tryListFieldBehindNamedKey(State&, const FieldData&);
};
static_assert(sizeof(Object) == offsetOfOrderWord);

class Extension {
public:
    // Every granule starts as zero, and so does the room for more.
    static Extension* create(VM&, uint64_t granuleBits);

    uint64_t granuleBits() const { return m_granuleBits; }
    bool holds(uint32_t offset) const { return m_granuleBits & granuleBitOf(offset); }
    void* slot(uint32_t offset)
    {
        ASSERT(holds(offset));
        return reinterpret_cast<uint8_t*>(granules() + std::popcount(m_granuleBits & (granuleBitOf(offset) - 1))) + offsetInGranule(offset);
    }
    void* slotOrNull(uint32_t offset) { return holds(offset) ? slot(offset) : nullptr; }
    uint64_t* granules() { return reinterpret_cast<uint64_t*>(this) + offsetOfFirstExtensionSlot / sizeof(uint64_t); }
    size_t sizeInBytes() const { return offsetOfFirstExtensionSlot + std::popcount(m_granuleBits) * extensionGranuleSize; }

    // Granules that come after every granule held take the room that extensionCapacityFor() has left. It is zero since the Extension was created, so the collector may see the bits at any time.
    bool tryAppend(uint64_t granuleBits);
    // Granules that belong between others take the same room, and the granules behind them move. Only while no thread of the collector visits cells.
    bool tryInsert(uint64_t granuleBits);

    static constexpr ptrdiff_t offsetOfGranuleBits() { return OBJECT_OFFSETOF(Extension, m_granuleBits); }

private:
    uint64_t m_granuleBits;
};
static_assert(static_cast<uint32_t>(Extension::offsetOfGranuleBits()) == offsetOfExtensionGranuleBits && sizeof(Extension) == offsetOfFirstExtensionSlot);

ALWAYS_INLINE Object* compiledObjectOrNull(JSValue value)
{
    if (!value || !value.isCell() || !isCompiledObject(value.asCell()))
        return nullptr;
    return uncheckedDowncast<Object>(value.asCell());
}

ALWAYS_INLINE Object* compiledObjectOrNull(const JSCell* cell)
{
    if (!cell || !isCompiledObject(cell))
        return nullptr;
    return uncheckedDowncast<Object>(const_cast<JSCell*>(cell));
}

inline Extension* Object::extension() const
{
    std::optional<SlotData> slot = compiledStructure().extensionSlot();
    if (!slot)
        return nullptr;
    return reinterpret_cast<Extension*>(const_cast<uint8_t*>(extensionStorageOrNull(this, *slot)));
}

template<typename Functor, typename Strings>
ALWAYS_INLINE auto Object::readInlineUnboxed(const FieldData& field, const Functor& functor, Strings strings) const
{
    if (field.isOutOfLine()) [[unlikely]]
        return functor(read(field));
    switch (field.slot.representation()) {
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32: {
        uint32_t word = at<uint32_t>(field.slot.offset);
        if (word <= undefinedCell32)
            return functor(JSValue::decode(word));
        if (isInFrame()) [[unlikely]]
            return functor(read(field));
        return functor(JSValue(decodeCell32(this, word)));
    }
    case SlotRepresentation::OptionalInt32:
        if (int32_t word = at<int32_t>(field.slot.offset); word >= minInlineInt32) [[likely]]
            return functor(word);
        return functor(read(field));
    case SlotRepresentation::Boolean:
        return functor(static_cast<bool>(at<uint8_t>(field.slot.offset)));
    case SlotRepresentation::Cell:
    case SlotRepresentation::JSValue:
        return functor(JSValue::decode(at<EncodedJSValue>(field.slot.offset)));
    case SlotRepresentation::Int32:
        return functor(at<int32_t>(field.slot.offset));
    case SlotRepresentation::Double:
        return functor(at<double>(field.slot.offset));
    case SlotRepresentation::Uint8:
        return functor(static_cast<int32_t>(at<uint8_t>(field.slot.offset)));
    case SlotRepresentation::Int8:
        return functor(static_cast<int32_t>(at<int8_t>(field.slot.offset)));
    case SlotRepresentation::Uint16:
        return functor(static_cast<int32_t>(at<uint16_t>(field.slot.offset)));
    case SlotRepresentation::Int16:
        return functor(static_cast<int32_t>(at<int16_t>(field.slot.offset)));
    case SlotRepresentation::Uint32:
        return functor(jsNumber(at<uint32_t>(field.slot.offset)));
    case SlotRepresentation::OptionalBoolean:
        return functor(JSValue::decode(at<uint8_t>(field.slot.offset)));
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::DeferredClosure:
        return functor(read(field));
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        if constexpr (!std::is_null_pointer_v<Strings>) {
            uint64_t word = field.slot.representation() == SlotRepresentation::LiteralString ? at<uint64_t>(field.slot.offset) : at<uint32_t>(field.slot.offset);
            if (auto* string = strings->stringForWordInSlot(word)) [[likely]]
                return functor(JSValue(string));
        }
        return functor(read(field));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline constexpr auto boxValueOfSlot = [](auto value) ALWAYS_INLINE_LAMBDA -> JSValue {
    using Unboxed = decltype(value);
    if constexpr (std::same_as<Unboxed, bool>)
        return jsBoolean(value);
    else if constexpr (std::same_as<Unboxed, double>)
        return jsNumber(withoutPayload(value));
    else if constexpr (std::same_as<Unboxed, int32_t>)
        return jsNumber(value);
    else
        return value;
};

ALWAYS_INLINE JSValue Object::readInline(const FieldData& field) const
{
    return readInlineUnboxed(field, boxValueOfSlot);
}

ALWAYS_INLINE JSValue Object::readInline(const FieldData& field, const auto* strings) const
{
    return readInlineUnboxed(field, boxValueOfSlot, strings);
}

ALWAYS_INLINE std::optional<std::span<const FieldAsProperty>> Object::fieldsInKeyOrder() const
{
    static_assert(CompiledStructure::ListedFields::noOrderWord & orderWordIntegrityBits);
    const CompiledStructure::ListedFields& listed = compiledStructure().listedFields();
    if ((orderWord() & ~orderWordIntegrityBits) == listed.orderAndCount) [[likely]]
        return std::span { listed.fields, listed.size };
    return fieldsInKeyOrderSlow();
}

ALWAYS_INLINE JSValue Object::readListedField(const FieldAsProperty& property) const
{
    JSValue value = readInline(*property.field);
    if (!value) [[unlikely]]
        return read(*property.field);
    return value;
}

void Object::forEachFieldInKeyOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    KeyOrderStorage storage;
    forEachPositionedKey(keyOrder(state, storage), layout().numberOfFields, 0, functor, [](size_t) { });
}

template<typename Visitor>
ALWAYS_INLINE void Object::visitFields(Object* object, Visitor& visitor)
{
    const LayoutData& layout = object->layout();
    visitReferences(object, layout, layout.hasExtension(), visitor);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
