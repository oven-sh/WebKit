/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTTypedData.h"
#include "AOTTypedFormat.h"
#include "FunctionRareData.h"
#include "JSFunction.h"
#include "JSObject.h"
#include "JSScope.h"
#include "MarkedBlockCage.h"
#include "NativeExecutable.h"
#include "Strong.h"
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
class StackFrame;
class JSSlimPromiseReaction;

namespace AOT {

struct Instance;
struct ModuleEvaluation;
class GlobalPropertyWatchpoint;

namespace Typed {

class Extension;
struct EngineClass;
struct State;
struct TransitionPlan;

// In place of the number of an order: the keys are in the order of the fields of the layout. For a shape that has run out of numbers.
static constexpr uint32_t keysAreInLayoutOrder = 0xffff;
// See Instance::typedOrderKeys: of a field out of line, of one whose offset 16 bits do not hold, and of an extra property. No slot is there.
static constexpr uint16_t noSlotInCell = 0;
static constexpr uint32_t orderAndCountMask = orderWordHasExtraProperties - 1;

// An object laid out by the compiler, see AOTTypedData.h. No butterfly. To all else an ordinary object, but that it stays what its layout
// says and every field holds a value of its type. What would change that throws a TypeError: a store of a value that is not of the type
// of the field (where that type is written down, after what `as` does has been tried), a change of the attributes of a field, an accessor in its place, the deletion of a field that is not optional, another
// prototype, an own property by the name of a method of its class that the program is not seen to replace, an indexed property that is
// no writable data property, and Object.freeze(), seal() and preventExtensions() unless the program is seen to hand objects of the layout
// to one of them. So does what goes past the method table and would make a transition (Structure::hasImmutableShape()).
class Object final : public JSObject {
public:
    using Base = JSObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    // What the language does not see as an object (an environment, the frame of a coroutine) is no object to the engine either.
    static Structure* createStructure(VM&, Instance&, JSValue prototype, const LayoutData*);
    // Every field that can be absent is, the others are clear.
    static Object* create(VM&, Structure*, const LayoutData&, size_t size, uint32_t orderWord);
    static Object* createWithBody(VM&, Structure*, std::span<const uint8_t> body);
    // Of a class, as compiled code makes it: every field is there, and holds `undefined` if it can, but for fieldIsAbsentAtBirth.
    static Object* createWithBornFields(VM&, Structure*, const LayoutData&, size_t size, uint32_t orderWord);
    // `{ ...record }`, of a record: ObjectCreate(%Object.prototype%), then CopyDataProperties. The copy is of the layout the record has.
    // Null: a getter among its extra properties has thrown.
    Object* clone(JSGlobalObject*);
    // `{ ...record }` where the copy is an ordinary object: born with the Structure that `{}` gets to by the names in their order. Null, and nothing has happened that anybody can see: it has extra properties, or
    // State::planOfPlainCopy() has no Structure.
    JSObject* tryCopyAsPlainObject(JSGlobalObject*) const;
    // The same with keys left out: `const { a, ...rest } = record`. Null also where a key is no atom string.
    JSObject* tryCopyAsPlainObject(JSGlobalObject*, std::span<const EncodedJSValue> excluded) const;
    // CopyDataProperties with excluded keys onto a new object, which is of the layout `to`. Null, and nothing has happened that anybody can see: this is no record, it has extra
    // properties, `to` has no field for one of its own or does not take the value as it is, or `to` requires more. `storedNext`: fields of `to` that whoever asks stores to before anybody sees the copy: the last of its keys, in this order.
    Object* tryCopyAs(JSGlobalObject*, const LayoutData& to, NamesOfCopy, std::span<const EncodedJSValue> names, bool* isNeverCopied = nullptr);
    static Object* tryMergeAs(JSGlobalObject*, const LayoutData& to, Object& first, Object& second);
    static Object* tryMakeAs(JSGlobalObject*, State&, const LayoutData& to, std::span<const EncodedJSValue> namesStoredNext);
    // CopyDataProperties onto this record, WHICH NOBODY HAS YET (the copy or the literal that a spread goes onto), from another. False: the general way is to be gone, from the first key: what has been copied by
    // then it copies again, which nobody can tell.
    bool tryCopyFieldsOf(VM&, const Object& source);

    const LayoutData& layout() const;
    State& state() const;
    // Empty: the field is absent.
    JSValue read(const FieldData&) const;
    // The same, for whoever reads field after field: in place where the field is in the cell and takes no more than a load and a test.
    JSValue readInline(const FieldData&) const;
    // [[Get]] where it comes down to an own data property: a field that is set, or an extra property that is no accessor. Empty: anything
    // else, also for a cell that is no object of the program.
    static JSValue tryGetOwnData(VM&, JSCell*, PropertyName);
    // See LayoutData::discriminant.
    bool isDiscriminant(const FieldData&) const;
    // Gives an extra property that has just been defined a place among the keys: `position` keys come before it. Without this an extra
    // property is listed after all fields.
    void placeExtraKey(State&, PropertyName, uint32_t position);
    // The same for several, by ascending position, which is that in the list with all of them in it.
    void placeExtraKeys(State&, std::span<const std::pair<UniquedStringImpl*, uint32_t>> namesAndPositions);
    // Every extra property that has no place yet gets one after all keys, in the order of their creation.
    void placeExtraKeys(State&);
    // For the run time's own stores. False: the value is not of what the slot holds, or the field is immutable.
    bool write(VM&, const FieldData&, JSValue);
    // A store by code that was not compiled with the type: see State::holdToTypeOf(). False: it has thrown.
    bool assign(JSGlobalObject*, const FieldData&, JSValue, bool* givesUp = nullptr);
    JSObject* becomeOrdinary(VM&);
    JSFinalObject* copyAsOrdinary(VM&, unsigned inlineCapacity);
    // Store::Int32OrElsewhere: a number, as a rule one that the word cannot hold. Also for compiled code, whose first stores to the object
    // may not be over: this looks at the word of the field only.
    void storeElsewhere(VM&, const FieldData&, JSValue);

    uint32_t orderWord() const { return at<uint32_t>(offsetOfOrderWord); }
    // With the shape word above it.
    uint64_t orderAndShapeWords() const { return at<uint64_t>(offsetOfOrderWord); }
    uint32_t shape() const { return shapeOf(at<uint32_t>(offsetOfShapeWord)); }
    bool isFrozen() const;
    unsigned attributesOf(const FieldData& field) const
    {
        uint32_t word = orderWord();
        if (word & orderWordIsFrozen) [[unlikely]]
            return attributesOfFieldOfWhatAsks(field);
        return static_cast<unsigned>(word & orderWordIsSealed ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    }
    unsigned attributesOfFieldOfWhatAsks(const FieldData&) const;
    void setAttributesOf(VM&, const FieldData&, unsigned);
    bool isSealed() const { return orderWord() & orderWordIsSealed; }
    bool isNotExtensible() const { return orderWord() & orderWordIsNotExtensible; }
    // The Structure says so too: see State::structureFor().
    bool hasExtraProperties() const { return orderWord() & orderWordHasExtraProperties; }
    // For whoever caches by Structure. No object of the Structure has an own property of the name, now or later. (A field may be absent in
    // one object and there in the next: that is not by Structure.)
    static bool absenceIsByStructure(Structure*, UniquedStringImpl*);
    struct FieldAccess {
        // From the cell, or from the start of the extension.
        uint32_t offset;
        Store store;
        bool isOutOfLine;
        bool mayBeAbsent;
        // What a store to the field, if it is there and the object is not frozen, may put there without asking put(). Primitive::*, with
        // Int for every integral number. Nothing: ask.
        uint16_t acceptedPrimitives { 0 };
        bool acceptsInt32 { false };
        bool acceptsAnyObject { false };
        uint32_t typeOfAcceptedObjects { 0 };
    };
    // Of the field of the name, for every object of the Structure of this one. Nothing: there is no such field.
    std::optional<FieldAccess> fieldAccessFor(VM&, PropertyName);
    // SetIntegrityLevel, or [[PreventExtensions]] alone. False: an exception was thrown.
    enum class Integrity : uint8_t { NotExtensible, Sealed, Frozen };
    bool setIntegrity(JSGlobalObject*, Integrity);
    // The properties the layout has no field for, in an ordinary object that nobody else sees.
    JSObject* extraProperties() const;
    const FieldData* fieldNamed(State&, PropertyName) const;
    // The own properties in key order, for the engine's own algorithms (JSON.stringify): name and value of each field that is set and is no
    // private name. All of them are enumerable data properties with string keys. The functor says whether to go on. It runs no code of the
    // program and creates no field: the order that is walked may move. Nothing is allocated here.
    // False, and the functor has not been called: the object has extra properties, which may be anything.
    bool forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>&) const;
    // The names those are, as strings, in a list that arrays share until they are written to: what Object.keys yields. There is one for all
    // objects of the layout with the keys in that order, as the engine has one for a Structure. Null: the object has extra properties, or
    // its layout keeps no orders. It may collect garbage.
    JSCellButterfly* keys(VM&) const;
    // The field is absent and is about to be stored to: it is the last of the keys now. If it is out of line, the object has an extension
    // of its own from here on: compiled code loads the pointer again. That may collect garbage.
    void didCreateField(State&, const FieldData&, bool withStorage = true);
    bool isAmongKeysInOrder(State&, const FieldData&) const;
    void willKeepKeysInLayoutOrder();
    void ensureStorageFor(VM& vm, State& state, const FieldData& field) { ensureExtensionCovers(vm, state, field); }
    void ensureStorageOfSize(VM&, State&, uint32_t size);
    // JSObject::getDirect() and putDirect(): native code that goes past the method table.
    JSValue getDirectFromNativeCode(VM&, PropertyName, unsigned* attributes);
    // False: the object cannot hold it.
    bool putDirectFromNativeCode(VM&, PropertyName, JSValue, unsigned attributes);
    // Likewise, for whoever has seen to the places of the extra properties.
    void appendField(State&, const FieldData&);
    // Likewise, where an extra property of the name becomes the field: the key stays where it is if it has a place.
    void didMoveExtraPropertyToField(State&, const FieldData&);
    void willCreateField(State&, const FieldData&);
    // [[Set]] of a property the layout has no field for. Where that would run code of the program, it throws.
    void putExtraWithoutRunningCode(JSGlobalObject*, PropertyName, JSValue);
    // OrdinaryCreateFromConstructor, for an object nobody has seen yet.
    void setPrototypeAtBirth(VM&, JSObject*);
    // Compiled code takes what is absent for absent unless orderWordHasExtraProperties is set: so it is also for an object of a layout with
    // mayHaveOtherPrototype that inherits from anything but what its layout says, State::isPrototypeByLayout(). No other object: a word with the bit never hits
    // the cache of a store that creates a field. To be called once the object has its prototype. `isBirth`: it has no extra properties.
    void noteWhetherToAsk(State&, bool isBirth = false);

    struct PrivateElement {
        enum class Kind : uint8_t { None, Field, Method, Accessor };
        Kind kind { Kind::None };
        // Of a field or a method.
        JSValue value;
        // Of an accessor. Null: it has none.
        JSObject* getter { nullptr };
        JSObject* setter { nullptr };
    };
    // PrivateElementFind, of any object.
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
    // Only with LayoutData::hasExtension.
    Extension* extension() const;
    void setExtension(Extension*);
    // Where the field is kept. Null: out of line, and beyond the size of the extension, so the field is absent.
    void* slotOf(const FieldData&) const;
    // From here on the object has an extension of its own with room for the slot of the field, which is out of line. It may collect garbage.
    void ensureExtensionCovers(VM&, State&, const FieldData&);
    // By a copy with room for that many bytes.
    void replaceExtension(VM&, State&, uint32_t size);
    static double* numberBoxAt(int32_t word);
    void storeInNumberBoxOfItsOwn(State&, uint32_t offset, double);
    void copyNumberBoxes(State&);
    // The value as the field keeps it, whatever the type of the field. False: it cannot be kept that way. It may collect garbage.
    bool store(VM&, const FieldData&, JSValue);
    // Absent if it can be.
    void clear(const FieldData&);
    void setOrderAndCount(uint32_t orderAndCount)
    {
        if (orderOf(orderAndCount) == keysAreInLayoutOrder && orderOf(orderWord()) != keysAreInLayoutOrder) [[unlikely]]
            willKeepKeysInLayoutOrder();
        at<uint32_t>(offsetOfOrderWord) = (orderWord() & ~orderAndCountMask) | orderAndCount;
    }
    // The entries of the object's key order: a field index, or from LayoutData::numberOfFields on a placed extra key (State::extraKeyName).
    void forEachKeyInOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    void forEachFieldInKeyOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    void removeExtraKey(State&, PropertyName);
    JSObject* ensureExtraProperties(VM&);
    void willAddExtraProperty(VM&, PropertyName);
    void dropExtraPropertiesIfEmpty(VM&);
    // From here on it is an object of the shape, which is among the specializations of its own. What that has more is absent. It keeps its
    // prototype, also where it was made by a class and the shape is that of a record. The cell has the room.
    void specialize(VM&, State&, uint32_t shape);
    // The same by the plan for its Structure and the shape. False: the cell has not got the room, and nothing has been done.
    bool specialize(VM&, State&, TransitionPlan&);
};

ALWAYS_INLINE JSValue Object::readInline(const FieldData& field) const
{
    if (field.flags & fieldIsOutOfLine) [[unlikely]]
        return read(field);
    switch (field.store()) {
    case Store::Cell32:
    case Store::OptionalCell32: {
        uint32_t word = at<uint32_t>(field.offset);
        return word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(word));
    }
    case Store::Int32OrElsewhere:
        if (int32_t word = at<int32_t>(field.offset); word >= leastInt32InPlace) [[likely]]
            return jsNumber(word);
        return read(field);
    case Store::Boolean:
        return jsBoolean(at<uint8_t>(field.offset));
    case Store::Cell:
    case Store::JSValue:
        return JSValue::decode(at<EncodedJSValue>(field.offset));
    case Store::Int32:
        return jsNumber(at<int32_t>(field.offset));
    default:
        return read(field);
    }
}

// The one field of a layout whose value tells apart the layouts that a type offers to its objects: `kind: "a"`.
struct DecidingField {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DecidingField);
    const FieldData* field { nullptr };
    // What those layouts list for it: a string by its text, with its number; a number as a value.
    UncheckedKeyHashMap<String, uint32_t> strings;
    UncheckedKeyHashSet<EncodedJSValue> numbers;
};

// What is done to an object of a Structure that becomes one of a shape. Plain data: compiled code can do the same in place.
struct TransitionPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TransitionPlan);
    struct Slot {
        uint32_t offset;
        Store store;
    };
    // Null: the object stays what it is, and the rest says nothing.
    Structure* structure { nullptr };
    uint32_t shapeWord { 0 };
    // What the cell has to have room for.
    uint32_t size { 0 };
    const LayoutData* from { nullptr };
    const LayoutData* to { nullptr };
    // The slots in the cell that the layout adds. They are made absent: nothing has been written there since the cell was allocated.
    Vector<Slot, 4> addedSlots;
    // Order words of the old layout, the first that have come up, and what they are in the new one.
    static constexpr unsigned maxOrderWords = 8;
    Vector<std::pair<uint32_t, uint32_t>, 2> orderWords;
};

// What Stub::PutById goes by where the site of a store has seen it make a transition: see Slot::isStoreTransition and State::storeTransitionFor(). Plain data. It stays as long as the State.
struct StoreTransition {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StoreTransition);
    static constexpr unsigned maxHolders = 2;
    struct Cleared {
        uint32_t offset;
        // In bytes: 1, 4 or 8.
        uint32_t width;
        uint64_t absent;
    };
    // Object::orderAndShapeWords(): of the object the store is for, and with the transition made and the field created.
    uint64_t wordsBefore;
    uint64_t wordsAfter;
    // As Slot::pointer of a store to the field, in the new layout.
    uint64_t fieldWrite;
    // The chain of prototypes, and the Structures with which nothing on it takes or refuses a store. Null: there is no more. The second is
    // looked at only where the first has passed.
    JSCell* holders[maxHolders];
    uint32_t structureIDsOfHolders[maxHolders];
    uint32_t structureID;
    // What the cell has to have room for.
    uint32_t size;
    uint32_t numberOfCleared;
    const Cleared* cleared;
    Vector<Cleared, 4> clearedSlots;
};
static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && !(offsetOfOrderWord % sizeof(uint64_t)));

// The out-of-line slots of an Object: see offsetOfExtension in AOTTypedData.h.
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

// By the object, its extra properties. An entry lives as long as its key, which is what visits the value.
class ExtraPropertiesTable final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(ExtraPropertiesTable);
public:
    explicit ExtraPropertiesTable(VM&);
    ~ExtraPropertiesTable() final;

    JSObject* get(const Object* object) const { return m_map.get(const_cast<Object*>(object)); }
    // For the collector, which may be marking while the program adds to the table.
    JSObject* getConcurrently(const Object*);
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
        Vector<std::pair<uint32_t, uint8_t>, 2> ofFields;
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

// A function of the program as a value: where its code is, and the variables around it.
//
// To the engine and the host it is a function like any that is written in native code: a JSFunction whose NativeExecutable has what
// [[Call]] and [[Construct]] run. All closures share five of those: FunctionTables::kindOfExecutable(). Which function it is a closure of
// is in its one word (ClosureWord), by which the image has the name, the `length` and all else that is the same for all of them.
//
// As an object it is what the function is declared as. An async function, a generator function and an async generator function inherit
// from what the realm has for each, and do not construct. A generator function has a `prototype`, which its generators inherit from.
//
// The closure of the constructor of a class is the class object. What it has, and the prototype object, State::makeClass() gives it. Of a
// class with fixed members the static fields are variables of the program, which compiled code accesses as such. The class object shows
// them as data properties. What would make one of them anything else throws a TypeError, like a store of a value that is not of the type
// of the variable. So does another prototype for the class object, and a property of its own by the name of a static method or
// accessor it inherits: compiled code calls those without looking.
class EvalScope final : public JSScope {
public:
    using Base = JSScope;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static EvalScope* create(VM&, Structure*, JSScope* next, JSObject* bindings);
    static Identifier keyOfNewTarget(VM&);

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
    // What a closure shows and does not store is what any function does: `length`, `name`, `prototype`. A class object also shows its static
    // fields, which are variables of the program.
    static constexpr unsigned StructureFlags = Base::StructureFlags | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.heap.aotClosureSpace; }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, bool isBound = false);
    static Closure* create(VM&, Structure*, uint32_t function, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    // For compiled code, where it cannot allocate by itself. It writes everything from the executable on, the word last. Until then the cell has no captures.
    static Closure* createForCompiledCode(VM&, Structure*, size_t allocationSize);
    // SetFunctionName, for a name that is computed: of a member of a class, of a function under a computed key of a literal.
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

    // Function.prototype.toString(). There is no text: what it is declared as, and the name it was compiled with.
    JSValue toString(JSGlobalObject*) const;
    Format::FunctionKind kind() const;
    // functionIsAsync, functionIsGenerator: whether it is any of those given.
    bool isDeclaredAs(uint8_t) const;
    // It is of a generator function of which it is not the one closure: see throwSharesPrototypeProperty().
    bool sharesPrototypeProperty() const;
    // It is of an ordinary function whose objects have a layout: [[Construct]] takes an object that compiled code has made for it.
    bool makesLaidOutObjects() const;
    // And those layouts go by the prototype chain having nothing by the name of a field. Its `prototype` is a PrototypeObject, for good.
    bool keepsPrototypeProperty() const;
    // False: it has thrown.
    bool mayReplacePrototypeProperty(JSGlobalObject*, JSValue, bool shouldThrow);
    // What the function takes before `this`: copies of variables that are never stored to again, and environments. A word each, written
    // once, when the closure is made. The program never sees them. First the references, which the collector follows: a cell or a boxed
    // value. Then what holds no cell, as it is kept in a register.
    std::span<const uint64_t> captures() const { return { reinterpret_cast<const uint64_t*>(this + 1), counts()[1] }; }
    unsigned numberOfReferences() const { return counts()[0]; }
    // The first capture where that is a cell by what the closure is: the environment of a class that is not evaluated once, of its class
    // object; the frame of a coroutine, of its resume closure. Null: there are no references.
    JSCell* environment() const { return numberOfReferences() ? std::bit_cast<JSCell*>(captures()[0]) : nullptr; }
    uint32_t functionIndex() const { return ClosureWord::functionOf(m_entry); }
    uint64_t entry() const { return m_entry; }
    // [[Call]] runs it: it is no class. Then it is entered at closureEntrypoint().
    bool mayBeCalled() const;
    friend struct State;
    // The class it is the class object of. Null: it is none.
    const LayoutData* classData() const;
    // The class object of a class that State::makesClassObjectWhenChanged(), and nobody has looked at anything it has, or changed or listed it: nothing is stored in it, and it shares its Structure.
    bool isUnmadeClassObject() const;
    void ensureClassObject(VM&);
    void takeStructureOfBoundFunction(VM&);
    // What `prototype` holds. It is made if need be, an unmade class object stays one. Empty: a function of its kind has none.
    JSValue prototypeProperty(VM&);
    // What [[Construct]] does once `this` is there, which is `undefined` for a class with a foreign root. Yields what the function returns:
    // an object takes the place of `this`.
    JSValue callAsConstructor(JSGlobalObject*, JSValue thisValue, JSValue newTarget, const ArgList&);

private:
    Closure(VM& vm, Structure* structure, NativeExecutable* executable, uint64_t entry)
        : Base(vm, executable, structure->realm(), structure)
        , m_entry(entry)
    {
    }

    // `prototype` is made when it is first asked for, and cannot be deleted. False: a function of its kind has none.
    bool ensurePrototypeProperty(VM&);
    // Data::closureCountsOffset
    std::array<uint8_t, 2> counts() const;

    // The ClosureWord of its function. Of the resume closure of a coroutine, which compiled code makes: that of the resume function
    // itself, which takes the frame and the resume mode, with ClosureWord::isNotCallableBit. See Stub::EnterResume.
    uint64_t m_entry;
};
static_assert(sizeof(JSFunction) == 32 && Closure::offsetOfEntry() == 32 && sizeof(Closure) == 40);

class SlimClosure final : public JSObjectWithButterfly {
public:
    using Base = JSObjectWithButterfly;
    static constexpr unsigned ReifiedStructureFlags = Base::StructureFlags | OverridesGetCallData | TypeOfYieldsFunction | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr unsigned ReifiedConstructorStructureFlags = ReifiedStructureFlags | OverridesPut;
    static constexpr unsigned ReifiedBoundStructureFlags = ReifiedStructureFlags & ~ImplementsDefaultHasInstance;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | GetOwnPropertySlotIsImpureForPropertyAbsence | OverridesGetOwnPropertyNames | OverridesPut | OverridesIsExtensible | OverridesGetCallData | TypeOfYieldsFunction | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ImplementsHasInstance | ImplementsDefaultHasInstance;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.heap.aotClosureSpace; }

    static Structure* createStructure(VM&, Instance&, JSValue prototype, bool hasProperties, bool constructs = false, bool isBound = false);
    static SlimClosure* create(VM&, Structure*, uint32_t function, std::span<const uint64_t> captures = { }, unsigned numberOfReferences = 0);
    static SlimClosure* createForCompiledCode(VM&, Structure*, size_t allocationSize);

    // The butterfly slot holds the ClosureWord until reifyProperties() installs a real butterfly.
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

// What is the same in every Instance of a VM, and is needed where there is no Instance yet: see builtinFunction().
struct FunctionTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionTables);

    static FunctionTables& of(VM&);
    explicit FunctionTables(VM&);

    // Which executable the closures of a function have: by what [[Call]] and [[Construct]] run, and by ImplementationVisibility (Private,
    // PrivateRecursive). `kindAndDeclaration`: the byte at Data::functionKindsOffset. No class is hidden with what it calls.
    static constexpr unsigned kindOfExecutable(uint8_t kindAndDeclaration, bool isHiddenWithWhatItCalls)
    {
        auto kind = static_cast<Format::FunctionKind>(kindAndDeclaration & functionKindMask);
        if (kind == Format::FunctionKind::Constructor)
            return 4;
        bool constructs = kind == Format::FunctionKind::Plain && !(kindAndDeclaration & (functionIsAsync | functionIsGenerator));
        return constructs * 2 + isHiddenWithWhatItCalls;
    }
    NativeExecutable* executableOf(uint32_t function) const;

    const Data& data;
    // ImplementationVisibility::PrivateRecursive: see Data::builtinFunctionsOffset.
    Vector<uint32_t, 4> functionsHiddenWithWhatTheyCall;
    // Instance::typedFunctions and Instance::typedExecutables.
    const uint64_t* words;
    static constexpr unsigned numberOfExecutables = 5;
    std::array<Strong<JSCell>, numberOfExecutables> executables;
    // Made before their realm had an Instance: see State::adoptClosuresOfRealm().
    Vector<Weak<JSCell>> closuresWithoutInstance;
};

// The `prototype` object of an ordinary function of the program whose objects have a layout: `function Point(this: Point, ..)`. It is an
// ordinary object but for this. Compiled code reads a field that the layout of an object lacks without asking the prototype chain. So
// nothing on that chain is by the name of a field, unless the compiler has seen the program define it (Data::reservedFieldNamesOffset):
// such a property is refused here, this object goes on inheriting from Object.prototype, the function keeps it as its `prototype`, and
// [[Construct]] with a new.target whose `prototype` is anything else is refused. In an audit each is a line, and the program goes on.
//
// Not where the compiler has seen the program replace the `prototype` of the function, give that object another prototype, or construct
// with another new.target: inheritance as it was written before there were classes. Then the layouts of its objects say that they may
// inherit from anything (LayoutData::mayHaveOtherPrototype), compiled code asks the chain, and the `prototype` is an ordinary object.
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

    // Nothing is defined on it, and nothing comes to be inherited through it, that compiled code does not know of. Not so for a function of
    // one of the host's own modules, which the program's facts know nothing of: that is an ordinary object in all but its class.
    bool isGuarded() const;
    // See State::didAlterPrototypeObject().
    bool isAltered() const { return m_isAltered; }

private:
    PrototypeObject(VM& vm, Structure* structure, uint32_t function)
        : Base(vm, structure)
        , m_function(function)
    {
    }

    // False: it has thrown. `isWritableData`: what is defined is a data property that can be stored over.
    bool mayDefine(JSGlobalObject*, PropertyName, bool isWritableData);
    // Whether what compiled code does not know of is let be, and the objects told. Otherwise it is refused.
    bool letsItselfBeAltered(VM&);
    // The same for another prototype, on which anything may be defined later, unseen.
    bool letsItselfBeReparented(VM&);

    uint32_t m_function;
    bool m_isAltered { false };
};

// The `prototype` object of a class that is evaluated once, from when the first object of the class needs something to inherit from until
// somebody looks at it. Compiled code calls the members of a class by the shape of the receiver and never looks. Whoever does (a lookup by
// name that walks the chain, reflection, a store) comes through one of these hooks: the class object and the members are made then, by
// State::makeClass(), and this cell is a JSFinalObject from there on.
class ClassPrototype final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    // Impure: no cache and no watchpoint goes by what its Structure says it has or lacks.
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

    // Whoever walks a prototype chain by itself and reads what the objects on it have past their hooks (getDirect()) calls this on each first.
    static void look(JSObject* holder)
    {
        if (holder->classInfo() == info()) [[unlikely]]
            makeMembers(holder);
    }

    // For State::makeClass(). The Structure is of JSFinalObjects with inlineCapacity, has the same prototype, and no other object has it.
    void becomeOrdinary(VM&, Structure*);

    static bool hasMember(JSObject*, PropertyName);
    static bool willBeImmutable(JSObject*);

private:
    ClassPrototype(VM& vm, Structure* structure, uint32_t classIndex)
        : Base(vm, structure)
        , m_classIndex(classIndex)
    {
    }

    static void makeMembers(JSObject*);

    // Where a JSFinalObject has its properties.
    uint64_t m_classIndex;
    [[maybe_unused]] uint64_t m_unused { 0 };
};

// The namespace object of a compiled ES module: what JSModuleNamespaceObject is for a module of the engine's own loader. An export is whatever `read` yields for its number, each time it is
// asked: a variable, a closure, a class object, what another module exports; or `read` throws, in the dead zone. See MakeModuleNamespace.
class ModuleNamespace final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    // Impure: no cache goes by what its Structure says it has or lacks.
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | GetOwnPropertySlotIsImpure | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    // Not extensible, and the prototype is null.
    static Structure* createStructure(VM&, JSGlobalObject*, bool hasPrototypeOfHost);
    // The names: strings that are atoms, in the order of their numbers, which is that of their code units.
    static ModuleNamespace* create(VM&, Structure*, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* namesOfStar);
    static constexpr ptrdiff_t offsetOfRead() { return OBJECT_OFFSETOF(ModuleNamespace, m_read); }
    static constexpr unsigned noExport = UINT_MAX;
    unsigned numberOf(UniquedStringImpl*) const;
    bool isBroughtByStar(UniquedStringImpl*) const;

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
    ModuleNamespace(VM& vm, Structure* structure, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* namesOfStar)
        : Base(vm, structure)
        , m_read(read, WriteBarrierEarlyInit)
        , m_names(names, WriteBarrierEarlyInit)
        , m_star(star, WriteBarrierEarlyInit)
        , m_namesOfStar(namesOfStar, WriteBarrierEarlyInit)
    {
    }

    // Neither an export nor absent: Symbol.toStringTag, and `__esModule` once the module has been required, unless it exports that itself.
    JSValue fixedProperty(VM&, PropertyName) const;

    WriteBarrier<JSObject> m_read;
    WriteBarrier<JSCellButterfly> m_names;
    WriteBarrier<JSObject> m_star;
    WriteBarrier<JSCellButterfly> m_namesOfStar;
    bool m_saysItIsAModule { false };
};

// The object a compiled generator function returns. The state of the generator is in the coroutine frame, which is the scope of the
// resume closure.
class Generator final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Generator* create(VM&, Structure*, Closure*);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    enum class ResumeMode : uint8_t { Next, Throw, Return };
    JSValue resume(JSGlobalObject*, ResumeMode, JSValue);
    // The same without the result object: what that would have for `value`, and for `done`.
    JSValue resume(JSGlobalObject*, ResumeMode, JSValue, bool& isDone);
    // Whether it finds the @@iterator, the `next` and the `return` that the realm started with: then a `for..of` over it comes down to resume().
    bool iteratesAsMade(JSGlobalObject*);
    bool isSuspendedAtStart() const;
    bool isCompleted() const;

private:
    Generator(VM& vm, Structure* structure, Closure* resumeClosure)
        : Base(vm, structure)
        , m_resumeClosure(resumeClosure)
    {
    }

    Closure* m_resumeClosure;
    bool m_isRunning { false };
};

// The object a compiled async generator function returns: the request queue of the specification, driving the resume closure.
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
    JSValue enqueueDriver(JSGlobalObject*, JSValue);
    bool iteratesAsMade(JSGlobalObject*);
    bool isAwaitingDriver() const { return m_target.pointer() == this && !is(isDrainingAheadOfDriverFlag); }
    void setDriver(VM&, JSObject* driver, JSValue asyncContext);
    void willSuspendAtAwait() { set(isSuspendingAtAwaitFlag, true); }
    void willSuspendAtYield() { set(isSuspendingAtAwaitFlag | isAwaitingYieldOperandFlag, true); }
    void resumeFromAwait(JSGlobalObject*, JSValue resolution, bool isRejected);
    void appendTargetsOfRequests(Vector<JSCell*, 4>&) const;

private:
    AsyncGenerator(VM& vm, Structure* structure, Closure* resumeClosure)
        : Base(vm, structure)
        , m_resumeClosure(resumeClosure)
    {
    }

    static constexpr uint8_t modeMask = 3;
    static constexpr uint8_t isBusyFlag = 4;
    static constexpr uint8_t isSuspendingAtAwaitFlag = 8;
    static constexpr uint8_t isAwaitingYieldOperandFlag = 16;
    static constexpr uint8_t isAwaitingReturnFlag = 32;
    static constexpr uint8_t isDrainingAheadOfDriverFlag = 64;
    bool is(uint8_t flag) const { return m_target.type() & flag; }
    void set(uint8_t flag, bool value) { m_target.setType(value ? m_target.type() | flag : m_target.type() & ~flag); }

    int32_t& state() const;
    void append(VM&, Generator::ResumeMode, JSValue, JSObject* target);
    JSObject* takeFrontRequest(VM&);
    void drain(JSGlobalObject*);
    void run(JSGlobalObject*, int32_t mode, JSValue);
    void settleFrontRequest(JSGlobalObject*, JSValue, bool isDone, bool isRejected);

    Closure* m_resumeClosure;
    CompactPointerTuple<JSObject*, uint8_t> m_target;
    WriteBarrier<Unknown> m_value;
    WriteBarrier<JSSlimPromiseReaction> m_lastRequest;
    WriteBarrier<JSObject> m_cachedDriverResult;
    WriteBarrier<JSObject> m_cachedDriverResultTarget;
};

// The generators of the program inherit from %GeneratorPrototype% and %AsyncGeneratorPrototype% of the realm, as those of the engine do.
// From here on next(), return() and throw() of the two take both. To be called before the two are made immutable. Once is enough.
JS_EXPORT_PRIVATE void installGeneratorMethods(JSGlobalObject*);
// `@text` of the engine's and the host's builtin code. What nobody has registered is made and registered: whoever asks next finds the same.
JS_EXPORT_PRIVATE Identifier enginePrivateName(VM&, const String& text);

// Runs the resume function of a suspended async function. The context is its resume closure. See CoroutineFrame::isEnteredByJobLoop.
JS_EXPORT_PRIVATE void resumeAsyncFunction(JSGlobalObject*, VM&, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
// The context: the resume closure of an async generator that awaits, or the generator, which awaits what return() was given.
JS_EXPORT_PRIVATE void resumeAsyncGenerator(JSGlobalObject*, JSValue context, JSValue resolution, bool isRejected);
bool driveAsyncIterator(JSGlobalObject*, VM&, JSCell* awaited, JSObject* driver);
void resumeDriver(VM&, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop);
JS_EXPORT_PRIVATE bool isResumeClosureOfAsyncFunction(JSCell*);
JS_EXPORT_PRIVATE JSPromise* promiseOfAsyncFunction(JSCell* resumeClosure);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine);
JS_EXPORT_PRIVATE Vector<JSPromise*, 4> promisesSettledBehind(JSPromise*);
JS_EXPORT_PRIVATE void appendSuspendedAsyncFunction(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* resumeClosure);
JS_EXPORT_PRIVATE JSCell* appendDirectAsyncCallers(VM&, JSCell* owner, Vector<StackFrame>&, JSCell* resumeClosure, size_t maxStackSize);
// Of the structure of compiled objects, for isDefinitelyNonThenable().
bool closureLacksWhatItsStructureLacks(VM&, JSCell*, UniquedStringImpl*, bool isReceiver = true);
bool closureAnswersForPrototypeProperty(JSCell*);
GetValueFunc getterOfPrototypePropertyOfClosures();
bool hasFieldNamedThen(Structure*);
bool neverHasThen(Structure*);

// What an instance of the program keeps for compiled code.
struct State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(VM&, Instance&, const Data&);
    // The RegExp of a literal: `source` is a string of the program, `flags` an OptionSet<Yarr::Flags>. One for each pair, kept for good, as a CodeBlock keeps those of its literals. It may be invalid.
    RegExp* regExpLiteral(VM&, uint32_t source, uint32_t flags);
    ~State();

    const Identifier& name(VM&, uint32_t);
    std::span<GlobalPropertyCache> globalPropertyCaches() const;
    bool trustGlobalProperty(VM&, JSGlobalObject*, uint32_t cacheNumber, UniquedStringImpl*, JSValue);
    JSString* string(VM&, uint32_t);
    JSString* makeAndKeepStringConstant(VM&, uint32_t);
    JSString* makeStringConstant(VM&, uint32_t);
    const LayoutData& layoutOfShape(uint32_t shape) const { return *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]); }
    const AllocationData& allocation(uint32_t number) const { return data.at<AllocationData>(data.allocationsOffset)[number]; }
    // By TypeID.
    const RuntimeType& type(uint32_t index) const { return *data.at<RuntimeType>(data.at<uint32_t>(data.typesOffset)[index]); }
    // What the slot holds, and what the type of the layout says the property is.
    const RuntimeType& typeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.type()); }
    const RuntimeType& ownTypeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.ownType()); }
    // See Data::functionKindsOffset.
    uint8_t kindAndDeclarationOf(uint32_t function) const { return data.at<uint8_t>(data.functionKindsOffset)[function]; }
    Format::FunctionKind kindOf(uint32_t function) const { return static_cast<Format::FunctionKind>(kindAndDeclarationOf(function) & functionKindMask); }
    // By LayoutData::index. Null: not a class.
    const LayoutData* classWithIndex(uint32_t) const;
    // With the prototype the objects of the shape have unless the allocation, or whoever constructs, says otherwise.
    Structure* structure(VM&, uint32_t shape);
    // There is one for each layout, prototype, and whether its objects have extra properties.
    Structure* structureFor(VM&, uint32_t shape, JSValue prototype, bool hasExtraProperties = false);
    bool isOfObjectsWithoutExtraProperties(Structure*);
    // The first one gives the allocation its row: see Instance::typedAllocationRows.
    JSCell* allocate(VM&, uint32_t allocation);
    static constexpr uint32_t maxNumberOfAllocationRows = std::numeric_limits<uint16_t>::max();
    // What an object of the shape takes, so that it can be made one of a specialization where it is.
    uint32_t sizeWithRoomToSpecialize(uint32_t shape);
    // By the generic entrypoint of the function. Its prototype is that of functions, async functions, generator functions or async
    // generator functions.
    Structure* closureStructure(VM&, uint32_t function);
    Structure* structureOfClosureThatIsNoClassObject(VM&, uint32_t function);
    Structure* evalScopeStructure(VM&);
    bool tryPutLazyClosure(JSGlobalObject*, JSValue holder, const Identifier&, uint32_t function);
    void putLazyClosure(JSGlobalObject*, JSValue holder, const Identifier&, uint32_t function);
    CustomGetterSetter* lazyClosureCell(VM&, uint32_t function);
    JSObject* createClosureOver(VM&, uint32_t function, JSValue captured);
    JSObject* reifyLazyClosure(VM&, JSObject* holder, PropertyName);
    void reifyLazyClosures(VM&, JSObject* holder);
    uint32_t functionOfLazyClosure(JSCell* cell) const { return m_functionsOfLazyClosures.get(cell); }
    bool closuresHaveCaptures(uint32_t function) const;
    UncheckedKeyHashMap<JSCell*, uint32_t> m_functionsOfLazyClosures;
    // Of the generators that a function makes, by the generic entrypoint of the function that runs its body. They inherit from the
    // `prototype` of the function.
    Structure* generatorStructure(VM&, uint32_t resumeFunction, bool isAsync);
    // Of what a closure of a function that takesItsClosure() makes now: it inherits from the `prototype` the closure has now.
    Structure* generatorStructure(VM&, Closure* generatorFunction, bool isAsync);
    // Both by generic entrypoint.
    uint32_t resumeFunctionOf(uint32_t generatorFunction) const;
    // See Typed::Function::takesItsClosure.
    bool takesItsClosure(uint32_t generatorFunction) const;
    // The class whose constructor has that generic entrypoint. Null: there is none.
    const LayoutData* classConstructedBy(uint32_t function) const;
    // Gives the closure of the constructor what makes it the class object, and makes the prototype object. `base`: what stands after
    // `extends`, where that is not known from the class alone. Throws only for a class with a foreign root.
    // Where the members of the prototype wait for whoever looks (makesMembersWhenLookedAt()) a class is made in two halves.
    enum class PartOfClass : uint8_t { Everything, ClassObject, MembersOfPrototype };
    void makeClass(JSGlobalObject*, Closure*, JSValue base, PartOfClass = PartOfClass::Everything, JSValue prototypeOfBaseIfRead = JSValue());
    // The class object of a class that is evaluated once is an ordinary function object as far as its static methods and accessors go: they can
    // be stored to, redefined, deleted and shadowed. Compiled code goes by the declarations until one has been. The engine's own watchpoints
    // tell, whatever path the change takes, native code that goes past the method table included.
    struct WatchedMembers;
    void watchMembers(VM&, JSObject* holder, const LayoutData&);
    void updateStaticFieldPlaces(VM&, WatchedMembers&);
    void didMakeTransition(VM&, WatchedMembers&);
    void didAlterMembers(WatchedMembers&);
    // Of a class that is evaluated once. A ClassPrototype, where that will do and the class object has not been made.
    JSObject* prototypeOfClass(VM&, uint32_t classIndex);
    bool makesMembersWhenLookedAt(const LayoutData&) const;
    // An object of the class may need the prototype before anybody has used the class as a value.
    bool hasPrototypeAheadOfClassObject(const LayoutData&) const;
    JSObject* makeClassPrototype(VM&, uint32_t classIndex, JSObject* parent);
    // See Closure::isUnmadeClassObject().
    bool makesClassObjectWhenChanged(const LayoutData&);
    Structure* structureOfUnmadeClassObject(VM&, const LayoutData&);
    Structure* structureOfUnmadeClassObjectInheritingFrom(VM&, JSObject* parent);
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    bool mayLeaveClassObjectUnmade(JSObject* parent, const LayoutData&);
    bool isStructureOfUnmadeHeirsOf(VM&, Structure*, JSObject* parent);
    bool tryAssumeAbsentFromUnmadeHeirs(VM&, JSObject* parent, UniquedStringImpl*);
    // Null: neither an object of the class nor the class object has been made.
    JSObject* prototypeOfClassIfExists(uint32_t classIndex) const { return prototypesOfClasses.get(classIndex); }
    // Whether it is what compiled code takes an object of the layout to inherit from: see mayInherit().
    bool isPrototypeByLayout(const LayoutData&, JSValue prototype) const;
    // Something has been defined on it, or has come to be inherited through it, that compiled code does not know of: what inherits from it asks from now on.
    void didAlterPrototypeObject(VM&, PrototypeObject*);
    uint32_t orderWordAtBirth(VM&, const AllocationData&, Structure*);
    // Whether the layouts of the objects that the function makes say that nothing is known of their prototype chain.
    bool mayAlterPrototypeObjectOf(uint32_t function) const { return makerFlags(function) & Data::makesObjectsOfFlaggedLayout; }
    // Whether compiled code reads some field of an object that the function makes without a test: fieldIsNeverSeenAbsentAsWritten, as isBornUnderAnyPrototype() has it.
    bool makesObjectsWithDefinitelyInitializedFields(uint32_t function) const { return makerFlags(function) & Data::makesObjectsWithDefinitelyInitializedFields; }
    // What [[Construct]] of the closure hands it for `this`. Empty: it has thrown.
    JSValue createThis(JSGlobalObject*, Closure*, JSObject* newTarget);
    // Where createThis() makes an ordinary object and runs no code of the program for it: the Structure of that. It holds until
    // Instance::typedPrototypeEpoch moves. `inlineCapacity`: how many properties are seen to be stored to `this`. The first number
    // given for the function of a new.target stands, for createThis() too. Null: ask createThis().
    Structure* structureForThis(JSGlobalObject*, Closure* constructor, JSObject* newTarget, unsigned inlineCapacity);
    Structure* structureForThisWithSlimClosure(JSGlobalObject*, JSObject* constructor, JSObject* newTarget, unsigned inlineCapacity);
    unsigned inlineCapacityForThis(uint32_t constructor, uint32_t newTarget, unsigned seen) const;
    // Of a plain function, by its generic entrypoint: what makes an object of the layout it declares `this` to be of. Null: it declares none.
    const AllocationData* allocationOfThis(uint32_t function) const;
    // Data::refusedFreshObjectsOffset.
    bool refusesFreshObject(uint32_t function, uint32_t shape) const;
    // By its generic entrypoint: `new` on it makes an object of a layout, where compiled code knows the function or by createThis().
    bool makesLaidOutObjects(uint32_t function) const { return makerFlags(function) & isMaker; }
    static constexpr uint32_t isMaker = 4;
    uint32_t makerFlags(uint32_t function) const;
    // And none of those layouts has LayoutData::mayHaveOtherPrototype.
    bool hasGuardedPrototype(uint32_t function);
    bool isReservedFieldName(VM&, UniquedStringImpl*, uint32_t function);
    Structure* prototypeObjectStructure(VM&);
    Structure* moduleNamespaceStructure(VM&, bool hasPrototypeOfHost);
    static constexpr ptrdiff_t offsetOfModuleNamespaceStructure() { return OBJECT_OFFSETOF(State, m_moduleNamespaceStructure); }
    // What adding properties of these names, in this order, makes of an array of the Structure: see operationAOTTypedTryAddNamedProperties.
    struct NamedPropertiesTransition {
        Structure* from { nullptr };
        // None: they are to be stored one by one.
        Structure* to { nullptr };
        Vector<uint32_t, 4> names;
        Vector<PropertyOffset, 4> offsets;
    };
    // Nothing: it is not known, and no more are remembered.
    const NamedPropertiesTransition* namedPropertiesTransition(VM&, Structure* from, std::span<const EncodedJSValue> names);
    Object* createObjectOfClass(VM&, const LayoutData&, JSObject* prototype);
    // The variables of a class, see LayoutData::variables(). Empty: it holds nothing yet.
    JSValue readVariable(const Closure* classObject, const LayoutData&, const FieldData&) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    // function (name) { return this[name] }
    JSFunction* liveBindingGetter(VM&);
    CustomGetterSetter* liveBindingAccessor(VM&);
    CustomGetterSetter* liveBindingAccessorIfMade() const { return m_liveBindingAccessor; }
    // The Instance has just been made. If it is that of the default loader of its realm, the closures that the realm has made so far are its own.
    void adoptClosuresOfRealm(VM&);
    Closure* closure(VM&, uint32_t function);
    void ensureStaticsOfProgramAreFilled()
    {
        if (!m_areStaticsOfProgramFilled) [[unlikely]]
            fillStaticsOfProgram();
    }
    JSCell* closureIfMade(uint32_t function) const;
    void makeTheOneThatConstructs(Closure*);
    template<typename Visitor> void visit(Visitor&);
    // The slow path of the barrier behind a store of the value into the array. Stub::RememberElement does the usual case by itself.
    void rememberElement(VM&, JSObject* array, JSValue);
    static constexpr unsigned minVectorLengthToRememberElements = 1024;

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    bool isPrivate(const FieldData&) const;
    bool findPrivateMethod(VM&, const LayoutData&, UniquedStringImpl*, Object::PrivateElement&);
    // Whether what an object of the layout inherits under the name is a method or accessor, of its class or of one that class extends,
    // that the program is not seen to replace: compiled code goes by the shape of the object for it, and does not look.
    bool hasFixedMethod(const LayoutData&, UniquedStringImpl*) const;
    bool declaresMethod(const LayoutData&, UniquedStringImpl*) const;
    // Whether its class, or one that class extends, declares anything that the prototype object has.
    bool hasPrototypeMembers(const LayoutData&) const;
    bool mayBeAbsent(const FieldData& field) const;

    // Null: it is no object of a Record or Instance layout of the program.
    Object* compiledObject(JSValue) const;
    bool passes(const RuntimeShapeTest&, uint32_t shape) const;
    bool admitsShape(const RuntimeType&, uint32_t shape) const;
    bool membersHaveNoSpecializations(const RuntimeType&) const;
    // Of an object: whether the type has it for what it is, and not because it admits whatever object comes.
    bool fitsAsOneOfItsOwn(const RuntimeType&, JSObject*) const;
    // What compiled code tests where it holds a value to a type.
    bool fits(const RuntimeType&, JSValue) const;
    // As: what enters a place whose type is written down is made of the type, or turned away. Coerce, `value as T`: it is made of the type
    // if it can be, converted if it is of another kind, and let through if it is an object that nothing can be made of. Flow, where a
    // value is narrowed to the type: only an object of the program is made anything of, where it is. Empty: it has thrown.
    // Store, Claim: it is tested, and nothing becomes of it.
    enum class HandsBack : bool { TheObjectItself, WhatComesOfIt };
    JSValue coerce(JSGlobalObject*, JSValue, const RuntimeType&, Format::CastReason, AuditSite = { }, HandsBack = HandsBack::WhatComesOfIt);
    struct PlanOfCoercion {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfCoercion);
        struct Field {
            PropertyOffset offset;
            const FieldData* field;
            UniquedStringImpl* name;
            uint8_t kindsTaken;
        };
        static constexpr uint8_t takesInt32 = 1;
        static constexpr uint8_t takesString = 2;
        static constexpr uint8_t takesTrue = 4;
        static constexpr uint8_t takesFalse = 8;
        static constexpr uint8_t takesUndefined = 16;
        static constexpr uint8_t takesNull = 32;
        static uint8_t kindOf(JSValue value) { return value.isInt32() ? takesInt32 : value.isString() ? takesString : value.isTrue() ? takesTrue : value.isFalse() ? takesFalse : value.isUndefined() ? takesUndefined : value.isNull() ? takesNull : 0; }
        static constexpr unsigned maxNumberByDiscriminant = 256;
        static constexpr unsigned maxNumberOfDiscriminants = 4;
        using Discriminants = Vector<PropertyOffset, maxNumberOfDiscriminants>;
        const LayoutData* layout { nullptr };
        uint32_t orderAndCount { 0 };
        Vector<Field, 8> fields;
        static constexpr size_t maxSizeMadeOverWhereItIs = 256;
        Vector<uint8_t> bodyBeforeStores;
        PropertyOffset discriminant { invalidOffset };
        UncheckedKeyHashMap<String, std::unique_ptr<PlanOfCoercion>> byDiscriminant;
    };
    const PlanOfCoercion* planOfCoercion(Structure*, const RuntimeType&) const;
    const PlanOfCoercion* planOfCoercion(JSObject*, const RuntimeType&) const;
    void addPlanOfCoercion(JSObject*, Structure*, const RuntimeType&, std::unique_ptr<PlanOfCoercion>&&, const PlanOfCoercion::Discriminants&);
    bool takesMorePlansOfCoercions() const { return m_plansOfCoercions.size() < maxNumberOfPlansOfCoercions; }
    void forgetPlansOfCoercionsOfDeadStructures(VM&, bool newOnly);
    static constexpr size_t maxLengthOfMadeKey = 64;
    JSString* keyMadeOf(VM&, std::span<const Latin1Character>);
    void forgetMadeKeysThatAreDead(VM&);
    uint8_t kindsTakenBy(const RuntimeType&) const;
    Object* copyWhatStaysAsItIsByPlan(VM&, JSObject*, const RuntimeType&, bool mayBecomeIt = false);
    Object* tryBecomeOrCopyByPlan(VM&, JSObject*, const PlanOfCoercion&, bool mayBecomeIt);
    bool tryBecomeByPlans(VM&, JSObject*, const RuntimeType&);
    bool hasRoomToBecome(JSObject*, Object* copy);
    bool refusesCopiesOfWhatOthersMayHold() const { return !data.logsTypeErrors && !data.isCompiledForAudit; }
    void become(VM&, JSObject*, Object* copy);
    // What has got somewhere behind the type of the place, and nobody has cast: it is made of the type as by As, but an object of the
    // program only becomes what it has everything for. Empty: it has thrown.
    JSValue hold(JSGlobalObject*, JSValue, const RuntimeType&);
    bool becomeWhereItIs(JSGlobalObject*, JSObject*, const RuntimeType&);
    // A data property, writable, enumerable and configurable, is about to be created under a name that the layout of the object has no
    // field for. If the object can become one of a layout that has, where it is, it does, and the value is stored to the field: the
    // smallest such layout, of several the one with the least index. False: there is none, and nothing has been done.
    bool specializeToStore(JSGlobalObject*, Object*, PropertyName, JSValue);
    // [[Set]] on the object itself under a name that its layout has no field for, where specializeToStore() has answered for such an object
    // before and nothing on the way there could say otherwise. False: nothing has been done, and it is for the whole way to say.
    bool storeByKnownTransition(VM&, Object*, UniquedStringImpl* name, JSValue);
    // After that has said yes, for the site of the store. The Structure and the words are those the object had, `fieldWrite` is for the field in the layout it has now. Null: not for a site to remember.
    const StoreTransition* storeTransitionFor(Structure* before, uint64_t wordsBefore, Object*, UniquedStringImpl* name, uint64_t fieldWrite);
    // The fields of `to` that are required and that `from` has none for, by their indices in `to`.
    const Vector<uint16_t, 2>& requiredFieldsAdded(const LayoutData& from, const LayoutData& to);
    // Options::auditAOTTypedAssertions(): the value is not of the type it is held to at the place, where it would be turned away. Prints a
    // line, once for each place, and yields what goes on: the value. Compiled code relies on no type that has room for it. Where the
    // type is kept as a bare number or boolean there is no room: then it is what ToNumber or ToBoolean make of the value, but that an
    // object, a symbol, a bigint and a string that is a rope count as not a number, so that no code runs and nothing is made or
    // thrown. Where the type has only some integers: `undefined` for what is not a number if the type has that, else the number
    // without its fraction, zero for what is not a number, and then the integer of the type that is nearest. Where the type has
    // `undefined` and `null` only: `undefined` if it has that. The line says "converted": from there on the run may differ from what
    // the program does as JavaScript.
    // `what`: see assertionIsOnResult.
    // `storedTo`: the object the value is about to be stored to, if that is why it is held to the type.
    JSValue assertionFailed(JSValue, const RuntimeType&, uint32_t what, AuditSite, const String& failure = { }, JSValue storedTo = { });
    // Likewise a line, once for each pair of layouts: a cast would have made an object of the one layout one of the other, where it is.
    void reportLackOfRoom(const LayoutData&, size_t cellSize, const LayoutData& wanted);
    // A rule of ours, not of the language, refuses what the program does. Throws a TypeError, and says false. In an audit: a line, once
    // for each place and message, and true. The caller then leaves the thing undone, or does it where the objects of an audit image can
    // represent it: compiled code goes by name there. After what is left undone the run is no longer that of the program, and every
    // line says so.
    enum class InAudit : bool { Ignored, CarriedOut };
    // `departure`: in what the run differs from that of the program though the thing is done.
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
    static constexpr ASCIILiteral kindOfUnnamedViolation = "type-mismatch"_s;
    void noteViolation(VM&, const Violation&);
    struct ListedSite {
        ASCIILiteral kind;
        unsigned ordinal { 0 };
    };
    std::optional<ListedSite> listedSite(const String& file, LineColumn, const Violation&, ASCIILiteral kind) const;
    static ASCIILiteral wordForKind(JSValue, const RuntimeType* heldTo = nullptr);
    bool reportOrThrow(JSGlobalObject*, const Violation&, const String& message, InAudit, ASCIILiteral departure = { });
    ASCIILiteral noteOnIgnoredRules() const { return m_hasIgnoredRule ? " (after a rule was ignored)"_s : ""_s; }
    // Counters, only in an image compiled for the audit or with Options::countAOTTypedAllocations(); elsewhere these do nothing. Printed
    // when the process exits or the State goes away, one `AOT count:` line per record. A new object is counted once its order word says
    // which fields it was born with.
    void countAllocation(uint32_t number, JSCell*);
    // What the run time allocates without an AllocationData.
    void countObject(Object*);
    void countTransition(const LayoutData& from, const LayoutData& to);
    // A field went from absent to set. The layout is the one the object has at that moment.
    void countFieldCreation(const LayoutData&, const FieldData&);
    // An object got an extension of its own, or a larger one.
    void countExtension(uint32_t previousSize, uint32_t size);
    // Compiled code had no answer to a read of an element.
    void countArrayGet(JSArray*, JSValue key);
    // See coerce(). The layout an object of the program had before, else null.
    void countCoercion(JSValue, const LayoutData* before, const RuntimeType&, Format::CastReason, ASCIILiteral way, JSValue result);
    void countArrayPut(JSArray*, JSValue key, IndexingType shapeBefore, unsigned vectorLengthBefore);
    // A closure was made, by the function and the number of captures.
    void countClosure(uint32_t function, unsigned numberOfCaptures);
    bool makesSlimClosures(uint32_t function) const { return data.at<uint8_t>(data.slimClosuresOffset)[function / 8] >> function % 8 & 1; }
    Structure* slimClosureStructure(VM&, uint32_t function);
    void createSlimClosureStructures(VM&);
    Structure* reifiedSlimClosureStructure(VM&, uint32_t function);
    Structure* structureOfBoundClosures(VM&, uint32_t function);
    JSObject* closureObjectFor(VM&, uint32_t function);
    JSObject* slimClosureLazyPropertyHolder(VM&);
    void countPlainAllocation(JSCell*, uint32_t function, uint32_t kindAndValue);
    // An object got a table for the properties its layout has no field for. A property was created in such a table.
    void countExtraPropertyTable(const LayoutData&);
    void countExtraProperty(const LayoutData&, PropertyName);
    // What has happened to a CompactTable with that many entries.
    void countCompactTable(const String& event, uint32_t entries);
    // An access by name that compiled code left to the run time: by call site (the return address into compiled code), name, what the
    // receiver is and where it has the property.
    enum class AccessKind : uint8_t { Read, Write, ReadExtra, WriteExtra };
    bool isCounting() const { return !!m_counts; }
    void countAccess(AccessKind, const void* returnAddress, PropertyName, JSValue receiver);
    // At the end of a collection, with the program stopped. A line about each. The heap is walked after one that leaves at least 5% more
    // than the last one it was walked after, and only the last walk is kept: what is live at the peak, or close to it.
    void countCollection(CollectionScope);
    // What a field, or a variable of a class, is to hold when code that was not compiled with its type stores the value: the value, if it
    // is of the type. If the type is written down, what `as` makes of it. Empty: nothing, and the string may say why, or it has thrown.
    JSValue holdToTypeOf(JSGlobalObject*, const FieldData&, JSValue, String& failure);
    bool storesMakeOverPlainData { true };
    // Whether it is one of the strings, or of the numbers, that are all the type of the field allows. See FieldData::literalsOffset.
    bool isAmongLiterals(const FieldData&, JSValue) const;
    bool isAmongLiterals(std::span<const LiteralData>, JSValue) const;
    // See LayoutData::literalsWithoutFieldOffset. True where the layout says nothing of the property.
    bool isAmongLiteralsWithoutField(VM&, const LayoutData&, UniquedStringImpl* property, JSValue);
    // In front of a store to a field with fieldIsPinned, and, without a value, of whatever takes the property away.
    void willAlter(const FieldData&, JSValue);
    // Whether the value, if there is one, is a string with the text of that string of the program.
    bool holdsText(JSValue, uint32_t string) const;

    std::span<const uint16_t> fieldsInOrder(const LayoutData&, uint32_t order);
    // See Object::keys(). The order word is of an object of the layout without extra properties.
    JSCellButterfly* keysInOrder(VM&, const LayoutData&, uint32_t orderWord);
    // The fields that are properties of such an object, in key order, each with its name: no private names. It holds as long as the program.
    // Nothing: the layout keeps no orders, or the order has the place of an extra property.
    struct FieldAsProperty {
        UniquedStringImpl* name;
        const FieldData* field;
    };
    std::optional<std::span<const FieldAsProperty>> propertiesInOrder(VM&, const LayoutData&, uint32_t orderWord);
    // Those, and what an ordinary object of this realm that is given them one after the other ends up with, all of them in the cell, the i-th at offset i. No Structure: there is none such.
    struct PlanOfPlainCopy {
        std::span<const FieldAsProperty> properties;
        Structure* structure { nullptr };
    };
    const PlanOfPlainCopy& planOfPlainCopy(VM&, const LayoutData&, uint32_t orderWord);
    // The same without the properties of those names, to which it holds a reference.
    struct PlanOfPlainCopyWithout {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfPlainCopyWithout);
        Vector<RefPtr<UniquedStringImpl>, 2> excluded;
        FixedVector<FieldAsProperty> properties;
        Structure* structure { nullptr };
    };
    const PlanOfPlainCopyWithout& planOfPlainCopyWithout(VM&, const LayoutData&, uint32_t orderWord, std::span<UniquedStringImpl* const> excluded);
    Structure* structureOfPlainObjectWith(VM&, std::span<const FieldAsProperty>);
    // These yield orderWordOf(order, count).
    // `entersPrefixes`: an object that has got as far as some part of the sequence finds the order too.
    uint32_t orderStartingWith(const LayoutData&, std::span<const uint16_t> fields, bool entersPrefixes = true);
    // What an object that goes by the order and count goes by once the extra key has been put at the position. Nothing: that has not come up yet.
    std::optional<uint32_t> orderWithExtraKeyPlaced(const LayoutData&, uint32_t orderAndCount, uint16_t entry, uint32_t position);
    void didPlaceExtraKey(const LayoutData&, uint32_t orderAndCount, uint16_t entry, uint32_t position, uint32_t result);
    uint32_t orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t field);
    // The field whose slot is at the offset in the cell. From the first time this is asked of a layout, Instance::typedOrderKeys has its orders.
    const FieldData& fieldAt(const LayoutData&, uint32_t slotKey);
    // Of an object that becomes one of `to`, which has every field of `from`.
    uint32_t orderInSpecialization(VM&, const LayoutData& from, const LayoutData& to, uint32_t orderWord);
    // For the Structure of the object and the shape, which is among the specializations of its own. Made once.
    TransitionPlan& planOfTransition(VM&, Object*, uint32_t shape);
    // What Object::tryCopyAs() comes to for every record of a layout with the same order word. A field of the copy that is typed otherwise than the one it is copied from is held to its type each time.
    struct PlanOfCopy {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfCopy);
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
            uint8_t kindsTaken { 0 };
        };
        Vector<RefPtr<UniquedStringImpl>, 2> excluded;
        Structure* structure { nullptr };
        uint32_t size { 0 };
        uint32_t orderWord { 0 };
        Vector<Move, 8> moves;
        Vector<Fill, 2> fills;
        Vector<Given, 2> given;
        Vector<Conversion, 2> conversions;
        Vector<Conversion, 2> fitted;
    };
    const PlanOfCopy* planOfCopy(uint32_t shape, uint32_t orderWord, uint32_t shapeOfCopy, std::span<UniquedStringImpl* const> excluded) const;
    void addPlanOfCopy(uint32_t shape, uint32_t orderWord, uint32_t shapeOfCopy, std::unique_ptr<PlanOfCopy>&&);
    struct PlanOfMerge {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlanOfMerge);
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
            bool alwaysFits;
        };
        Structure* structure { nullptr };
        uint32_t size { 0 };
        uint32_t orderWord { 0 };
        Vector<Move, 16> moves;
        Vector<Conversion> conversions;
        const FieldData* lastOutOfLine { nullptr };
    };
    using KeyOfMerge = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    const PlanOfMerge* planOfMerge(const KeyOfMerge& key) const { return m_plansOfMerges.get(key); }
    void addPlanOfMerge(const KeyOfMerge& key, std::unique_ptr<PlanOfMerge>&& plan) { m_plansOfMerges.add(key, WTF::move(plan)); }
    // The entry that stands for an extra property of that name in the key orders of the layout. Nothing: there is none, or no room for one.
    std::optional<uint16_t> extraKeyEntry(const LayoutData&, UniquedStringImpl*, bool create);
    const Identifier& extraKeyName(const LayoutData&, uint16_t entry);

    JSObject* prototypeForObjectsOf(VM&, Closure*);
    void didChangePrototypeOf(VM&, Closure*);

    // Error messages. None of these runs user code.
    static String describe(JSValue);
    String describe(const RuntimeType&) const;
    // A type as a ThrowKind::KindMismatch site has it.
    String describeKinds(uint32_t kinds, uint32_t name) const;
    String nameOf(const LayoutData&) const;
    String nameOf(const RuntimeShapeTest&) const;
    String describeAnonymousType(size_t numberOfProperties, const Invocable<uint32_t(uint32_t)> auto& nameOfProperty) const;
    String nameOfFunction(uint32_t) const;
    // A string of the program, by its StringID.
    String text(uint32_t string) const;
    // "f (src/a.ts:12:5)"
    String describeSite(AuditSite) const;
    static String nonNullMessage(JSValue, const String& operandText = { });
    // Of a read of, or a store to, a property of `undefined` or `null`. The name may be empty.
    static String nullishMessage(JSValue, const String& property, bool isWrite);
    // The same for a computed key, which is named if that takes no conversion.
    static String nullishMessage(JSGlobalObject*, JSValue, JSValue key, bool isWrite);
    // "expected T, got V", then the reason on a second line: `reason` if given, else explainMismatch().
    String mismatchMessage(VM&, JSValue, const RuntimeType& expected, const String& reason = { });
    // The first property of the object that is missing or has the wrong type. Null: nothing to add.
    String explainMismatch(VM&, JSValue, const RuntimeType&, bool membersAlone = false);
    // `target` reads like "'x' of Foo".
    String assignmentMessage(VM&, const String& target, const FieldData&, JSValue, const String& reason);
    // Whether every value of `from` is a value of `to`. Only messages depend on it.
    bool isAssignable(const RuntimeType& from, const RuntimeType& to) const;

    Instance& instance;
    const Data& data;
    // The module whose code the program's entry function is. External modules are its dependencies.
    AbstractModuleRecord* moduleRecord { nullptr };
    String pathOfFileAskedFor;
    Identifier keyOfImporter(VM&) const;
    ModuleEvaluation* moduleEvaluation { nullptr };
    int32_t moduleAsyncEvaluationCount { 0 };
    Vector<JSArray*, 4> modulesExecutingAsync;
    JSObject* importMeta { nullptr };
    JSObject* importMetaOfModule(JSGlobalObject*, JSString* path);
    Vector<std::pair<AbstractModuleRecord*, JSObject*>, 1> importsOfEntryPoints;
    // From where [[Construct]] enters a function that is functionReadsNewTarget to where that takes it out, which is the first thing it does.
    JSValue newTarget;
    static constexpr ptrdiff_t offsetOfNewTarget() { return OBJECT_OFFSETOF(State, newTarget); }
    // The name that name() has made of the string, or the null Identifier.
    const Identifier& nameIfMade(uint32_t string) const;
    void makeNamesForHeapAnalysis(VM&);
    // By shape: StructureID::bits(), or 0. Every layout that has an object has one.
    uint32_t* structures { nullptr };
    // By LayoutData::index. Compiled code does not read it, and few of the numbers are ever asked for.
    UncheckedKeyHashMap<uint32_t, JSObject*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> prototypesOfClasses;
    // By what they inherit from.
    UncheckedKeyHashMap<JSObject*, Structure*> structuresOfClassPrototypes;
    // By what is extended: the class object of a class of the program, or something foreign. What extends nothing has the realm's.
    UncheckedKeyHashMap<JSObject*, Structure*> structuresOfUnmadeClassObjects;
    struct UnmadeHeirs {
        Vector<uint32_t> withStaticMembers;
        Vector<RefPtr<UniquedStringImpl>> assumedAbsent;
    };
    UncheckedKeyHashMap<JSObject*, UnmadeHeirs> unmadeHeirs;
    ExtraPropertiesTable extraProperties;
    NumberBoxTable numberBoxes;
    FieldAttributesTable fieldAttributes;
    // Coercion::keep() has failed for an object of the Structure at the type, which has one layout to make objects of: at this property. Empty after a collection: the number of a Structure that has died may be given out again.
    struct FailureOfKeeping {
        uint32_t structureID;
        const RuntimeType* type;
        const FieldData* field;
    };
    FailureOfKeeping& failureOfKeeping(VM&, Structure*, const RuntimeType&);
    // For Coercion::mayChangeSomethingBelow(): of the plain objects of a Structure at a type: where the properties are that have a field whose own type has views, in whichever of the layouts such an object could be made one of,
    // and those types, by their offsets in Data. Four places are open to a pair, and another pair may take the one it has. A dictionary has none.
    // No entry outlives what it goes by. The type is in the image. Nobody keeps the Structure: but the only way to an entry is placesOpenToPropertiesWithViews(), which drops them all as soon as MarkedSpace::newlyAllocatedVersion() has moved, and
    // the number of a Structure can be given out again only after that: endMarking() moves it in every collection, before any block is handed back for sweeping. An entry is filled for the Structure of an object in hand. It holds
    // offsets: no name, no cell.
    struct PropertiesWithViews {
        static constexpr unsigned maxSize = 24;
        // What does not go into one goes on in the next part, which has a place of its own.
        static constexpr unsigned maxParts = 8;
        struct Property {
            PropertyOffset offset;
            uint32_t type : 31;
            // The type admits them, and Coercion::mayConvertProperties() says no for it.
            uint32_t leavesForeignObjects : 1;
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
    const PropertiesWithViews* propertiesWithViews(VM&, Structure*, const RuntimeType&, unsigned part);
    void setPropertiesWithViews(VM&, Structure*, const PropertiesWithViews&);
    // Whether Coercion::keep() could make anything of what a property holds whose field has the type: as far as that can be told without a Coercion, for one that has planned nothing yet. Nothing: it cannot be told.
    std::optional<bool> mayBeChangedBelow(JSValue, const RuntimeType& ofField, bool leavesForeignObjects) const;
    // Of a plain object at a type that admits it: Coercion::keep() would change nothing.
    bool staysAsItIsBelow(VM&, JSObject*, const RuntimeType&);
    // Which of so many places a key goes to. NOT `key * odd % places`: the lower bits of such a product are those of the key, and an address or a StructureID ends in zeros: they all went to a few places, or to one.
    template<unsigned places> static unsigned placeOf(uint64_t key)
    {
        static_assert(hasOneBitSet(places));
        return WTF::intHash(key) % places;
    }
    // For Coercion::mayConvertProperties(): the address of a type that has been asked about, with the answer in its lowest bit. Another type may take the place.
    static constexpr unsigned numberOfTypesAskedAboutProperties = 256;
    uintptr_t& typeAskedAboutProperties(const RuntimeType& type) const { return m_typesAskedAboutProperties[placeOf<numberOfTypesAskedAboutProperties>(std::bit_cast<uintptr_t>(&type))]; }
    // See Builtins::cellsOfReplaceableGlobals().
    void captureBuiltinsOf(ASCIILiteral global, JSObject*);
    void captureBuiltinsOfRealm();
    bool isOfLockedRealm { true };
    // Of CompactTable. Made at the first use.
    Structure* compactTableStructure();

private:
    // The orders of a shape: those the compiler expects, which are in the data, then those that have come up.
    struct OrderTable {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OrderTable);
        using Map = UncheckedKeyHashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        Vector<Vector<uint16_t>> added;
        // By the hash of a sequence of fields, an order that starts with it. See orderStartingWith().
        Map byPrefix;
        bool hasPrefixesOfExpectedOrders { false };
        // By order, count and the field that is created next.
        Map transitions;
        // See orderWithExtraKeyPlaced().
        UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placements;
        // By orderWordOf(order, count): see keysInOrder(). An order only ever gets longer, so what it starts with stays.
        UncheckedKeyHashMap<uint32_t, JSCellButterfly*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keyLists;
        // Likewise: see propertiesInOrder().
        UncheckedKeyHashMap<uint32_t, FixedVector<FieldAsProperty>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> propertyLists;
        // Likewise: see planOfPlainCopy(). didCreate() keeps the Structures alive; the lists are those of `propertyLists`, which only grows.
        UncheckedKeyHashMap<uint32_t, PlanOfPlainCopy, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> plansOfPlainCopies;
        UncheckedKeyHashMap<uint32_t, Vector<std::unique_ptr<PlanOfPlainCopyWithout>, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> plansOfPlainCopiesWithout;
        uint32_t orderWordOfLastPlainCopy { UINT32_MAX };
        PlanOfPlainCopy lastPlainCopy;
        // The one that was asked for last. No order word has all bits set.
        uint32_t lastOrderWord { UINT32_MAX };
        std::span<const FieldAsProperty> lastProperties;
        // Entry LayoutData::numberOfFields + i of an order stands for an extra property of this name.
        Vector<Identifier> extraKeys;
        UncheckedKeyHashMap<UniquedStringImpl*, uint16_t> indexesOfExtraKeys;
        // See Instance::typedOrderKeys. Empty: nothing has asked yet.
        Vector<uintptr_t> orderKeys;
        // The words of each order as Instance::typedOrderKeys describes them: the length, the slots of the keys, the pairs.
        Vector<Vector<uint16_t>> slotsInOrder;
        // By the offset of a slot in the cell, the index of its field.
        Vector<uint16_t> fieldsBySlot;
        Vector<uint16_t> fieldsByExtensionSlot;
    };

    void didCreate(JSCell*);
    // By source and flags: see regExpLiteral().
    UncheckedKeyHashMap<uint64_t, RegExp*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_regExpLiterals;
    Structure* m_compactTableStructure { nullptr };
    bool hasText(uint32_t string, StringView) const;
    bool isName(uint32_t string, UniquedStringImpl*) const;
    void fill(VM&, uint32_t allocation);
    // StructureID::bits(). 0 until fill().
    uint32_t structureIDOf(uint32_t allocation) const;
    uint32_t m_numberOfAllocationRows { 0 };
    // Of those that have no allocator, which compiled code takes a row to have, and of those that came when the rows had run out.
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_structureIDsOfAllocationsWithoutRow;
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base()); }
    bool isFixedIn(std::span<const MethodData>, UniquedStringImpl*, bool& isDeclared) const;
    // The class of the engine, of those whose objects compiled code knows by their type, that has this constructor.
    const EngineClass* engineClassConstructedBy(JSObject*);
    OrderTable& ordersOf(const LayoutData&);
    // The order is new, or has been made longer.
    void didChangeOrder(const LayoutData&, OrderTable&, uint32_t order);
    void didFindTransition(const LayoutData&, OrderTable&, uint32_t order, uint32_t count, uint16_t field, uint32_t next);
    JSValue convert(JSGlobalObject*, JSValue, const RuntimeType&, AuditSite);
    JSValue nearestOf(const RuntimeType&, JSValue) const;
    // Options::auditAOTTypedAssertions(): a line for what `as` has made of a value at the place. Each place once.
    void reportCoercion(AuditSite, const String& what);
    std::span<Structure* const> closureStructures(VM&);
    // By shape / 65,536. See layoutOfEnvironment().
    Vector<Structure*, 2> m_environmentStructures;
    void setPrototypeOfGenerators(VM&, uint32_t resumeFunction, bool isAsync, JSObject*);
    uint32_t resumeFunctionAndFlagOf(uint32_t generatorFunction) const;

    // What stands there, the function and the position. Or a field, for a store by code that was not compiled with its type. Or two shapes.
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_reportedPlaces;
    struct Counts;
    void countFieldsOf(Object*);
    void printCounts() const;
    static void printCountsAtExit();
    std::unique_ptr<Counts> m_counts;
    // As Instance::typedClosureStructureIDs.
    Structure* m_closureStructures[closureStructureOfClassObjects + 1] { };
    Structure* m_slimClosureStructures[numberOfSlimClosureKinds] { };
    Structure* m_reifiedSlimClosureStructures[2][3] { };
    Structure* m_structuresOfBoundClosures[2] { };
    RefPtr<PrivateSymbolImpl> m_functionIndexPrivateName;
public:
    Structure* asyncBoundFunctionStructure { nullptr };
    void setAsyncBoundFunctionStructure(Structure*);
private:
    JSObject* m_slimClosureLazyPropertyHolder { nullptr };
    void didCreateClosureFor(uint32_t function, JSCell*);
    Structure* m_prototypeObjectStructure { nullptr };
    Structure* m_moduleNamespaceStructure { nullptr };
    // By the host's number. See evaluateBuiltinModule().
    struct BuiltinModule {
        enum class Outcome : uint8_t { None, Exports, Thrown };
        Outcome outcome { Outcome::None };
        EncodedJSValue value { 0 };
    };
    UncheckedKeyHashMap<uint32_t, BuiltinModule, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_builtinModules;
    friend JSValue evaluateBuiltinModule(JSGlobalObject*, uint32_t);
    // By function. See structureForThis().
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_inlineCapacitiesForThis;
    Vector<NamedPropertiesTransition> m_namedPropertiesTransitions;
    UncheckedKeyHashSet<String> m_reportedRules;
    bool m_hasIgnoredRule { false };
    // By the generic entrypoint of the function that runs the body, made when first asked for.
    UncheckedKeyHashMap<uint32_t, Structure*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_generatorStructures;
    // Of generators and of async generators that inherit from what the realm has.
    std::array<Structure*, 2> m_generatorStructuresOfRealm { };
    // What generatorStructure() last found for a closure. It holds until a function gets another `prototype` (Instance::typedPrototypeEpoch). All are dropped after every collection: nothing is kept alive by them.
    struct GeneratorStructureOfClosure {
        Closure* closure;
        Structure* structure;
        uint32_t epoch;
    };
    std::array<GeneratorStructureOfClosure, 16> m_generatorStructuresOfClosures { };
    HeapVersion m_versionOfGeneratorStructures { 0 };
    JSFunction* m_liveBindingGetter { nullptr };
    CustomGetterSetter* m_liveBindingAccessor { nullptr };
    Strong<Structure> m_evalScopeStructure;
    // By the number of a string: where in m_names. Few of the strings of a program are ever asked for as names. What name() hands out stays where it is.
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_placesOfNames;
    SegmentedVector<Identifier, 128> m_names;
    const Identifier& nameSlow(VM&, uint32_t);
    // By shape number, made when first asked for.
    // By shape and name, for specializeToStore(): the shapes that will do whatever the object holds, the best first. As to the names, see
    // m_cachedFields: the key holds the reference.
    UncheckedKeyHashMap<std::pair<uint32_t, RefPtr<UniquedStringImpl>>, Vector<uint32_t, 1>> m_specializationsWithField;
    UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, Vector<uint16_t, 2>> m_requiredFieldsAdded;
    // From the data alone: see Data::fieldNamesOffset and LayoutData::fieldsByNameOffset.
    std::optional<uint32_t> findNumberOfFieldName(UniquedStringImpl*) const;
    std::optional<uint32_t> numberOfFieldName(UniquedStringImpl*) const;
    struct CachedFieldName {
        UniquedStringImpl* name;
        uint32_t numberPlusOne;
    };
    static constexpr unsigned numberOfCachedFieldNames = 1024;
    CachedFieldName* m_cachedFieldNames { nullptr };
    const FieldData* findField(VM&, const LayoutData&, UniquedStringImpl*);
    // What field() answered last, by shape and name. The name goes by its address, SO AN ENTRY HOLDS A REFERENCE TO IT: a name dies when the last reference to it goes, collection or no (a host makes one for a
    // read and drops it), and the next name may be born at its address. Nothing else keeps the names of fields: findField() answers from the data alone. That holds for "no field" as for a field.
    struct CachedField {
        uint32_t shape;
        UniquedStringImpl* name;
        const FieldData* field;
    };
    static constexpr unsigned numberOfCachedFields = 4096;
    CachedField* m_cachedFields { nullptr };
    UncheckedKeyHashMap<std::pair<Structure*, uint32_t>, std::unique_ptr<TransitionPlan>> m_plansOfTransitions;
    UncheckedKeyHashMap<std::tuple<uint32_t, uint32_t, uint32_t>, Vector<std::unique_ptr<PlanOfCopy>, 1>> m_plansOfCopies;
    static constexpr unsigned maxNumberOfPlansOfCoercions = 4096;
    static constexpr unsigned maxNumberOfPlansOfCoercionsByDiscriminants = 16384;
    unsigned m_numberOfPlansOfCoercionsByDiscriminants { 0 };
    UncheckedKeyHashMap<std::pair<Structure*, const RuntimeType*>, std::unique_ptr<PlanOfCoercion>> m_plansOfCoercions;
    bool m_hasAddedPlansOfCoercionsSinceLastCollection { false };
    struct PlanOfCoercionLastAskedFor {
        Structure* structure { nullptr };
        const RuntimeType* type { nullptr };
        const PlanOfCoercion* plan { nullptr };
    };
    mutable PlanOfCoercionLastAskedFor m_planOfCoercionLastAskedFor;
    // Without a Structure: the object stays what it is.
    TransitionPlan m_planToStay;
    UncheckedKeyHashMap<KeyOfMerge, std::unique_ptr<PlanOfMerge>> m_plansOfMerges;
    // What a cast makes of an object, where that goes by its Structure, the type, the reason of the cast and its discriminant, or what one
    // of its fields holds, alone: of one that has no extra properties and has not been frozen, sealed or made non-extensible. Null: it is
    // not for a plan to say.
    TransitionPlan* transitionPlan(JSGlobalObject*, Object*, const RuntimeType&, Format::CastReason);
    struct OutcomeOfCast {
        TransitionPlan* plan { nullptr };
        // It goes by what this field holds: to be asked again with classOfValue() of that for the discriminant.
        DecidingField* decidedBy { nullptr };
    };
    OutcomeOfCast outcomeOfCast(JSGlobalObject*, Object*, const RuntimeType&, Format::CastReason, EncodedJSValue discriminant, bool isClassOfValue);
    // Equal for two values if and only if none of the layouts takes one amiss and not the other. Never zero. Nothing: it is in no such class.
    static std::optional<EncodedJSValue> classOfValue(const DecidingField&, JSValue);
    Vector<std::unique_ptr<DecidingField>> m_decidingFields;
    struct CachedTransition {
        Structure* structure;
        const RuntimeType* type;
        EncodedJSValue discriminant;
        Format::CastReason reason;
        OutcomeOfCast outcome;
    };
    static constexpr unsigned maxNumberOfOutcomes = 1 << 16;
    // What transitionPlan() answered last. All it has ever answered is in the table behind.
    static constexpr unsigned numberOfCachedTransitions = 4096;
    CachedTransition* m_cachedTransitions { nullptr };
    UncheckedKeyHashMap<std::tuple<Structure*, const RuntimeType*, EncodedJSValue, unsigned>, OutcomeOfCast> m_plansOfCasts;
    // For specializeToStore(), of such an object, by its Structure, the name and its discriminant: what it can become so that it has a field
    // of the name, as far as that goes by those three, the best first. As to the names, see m_cachedFields: the key holds the reference.
    using FieldsListed = Vector<std::pair<const FieldData*, const FieldData*>, 0>;
    const FieldsListed& fieldsListedBy(VM&, const LayoutData& from, const LayoutData& to);
    UncheckedKeyHashMap<uint64_t, FieldsListed> m_fieldsListed;
    bool holdsWhatIsListed(Object*, const FieldsListed&) const;
    struct TransitionOfStore {
        const FieldData* created;
        TransitionPlan* plan;
        FieldsListed listed;
        // Order words of the old layout, the first that have come up, and what they are in the new one with the field created.
        mutable Vector<std::pair<uint32_t, uint32_t>, 2> orderWords { };
    };
    struct TransitionsOfStore {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TransitionsOfStore);
        Vector<TransitionOfStore, 1> transitions;
        // The name is no index, not `__proto__` and no method that the class fixes: between [[Set]] and these only the chain of prototypes is asked.
        bool areReachedByTheChainAlone { false };
    };
    bool store(VM&, Object*, const TransitionsOfStore&, JSValue);
    UncheckedKeyHashMap<std::tuple<Structure*, RefPtr<UniquedStringImpl>, EncodedJSValue>, std::unique_ptr<TransitionsOfStore>> m_transitionsOfStores;
    UncheckedKeyHashMap<std::pair<const TransitionPlan*, const FieldData*>, Vector<std::unique_ptr<StoreTransition>, 1>> m_storeTransitions;
    // What was last found there: so the table behind keeps the name of an entry. The two are emptied together.
    struct CachedStore {
        Structure* structure;
        UniquedStringImpl* name;
        EncodedJSValue discriminant;
        const TransitionsOfStore* transitions;
    };
    static constexpr unsigned numberOfCachedStores = 1024;
    CachedStore* m_cachedStores { nullptr };
    // The object, which is not compiled, enters a place of the type, which admits such objects, as it is, whatever the place, and nothing is made of what it holds: it is no plain data, or
    // Coercion::mayConvertProperties() has said no for the type.
    bool entersAsItIs(JSObject*, const RuntimeType&) const;
    uintptr_t* m_typesAskedAboutProperties { nullptr };
    static constexpr unsigned numberOfFailuresOfKeeping = 64;
    FailureOfKeeping m_failuresOfKeeping[numberOfFailuresOfKeeping] { };
    HeapVersion m_versionOfFailuresOfKeeping { 0 };
    static constexpr unsigned numberOfPropertiesWithViews = 64;
    static constexpr unsigned numberOfPlacesOpenToPropertiesWithViews = 4;
    std::span<PropertiesWithViews> placesOpenToPropertiesWithViews(VM&, Structure*, const RuntimeType&, unsigned part);
    PropertiesWithViews* m_propertiesWithViews { nullptr };
    HeapVersion m_versionOfPropertiesWithViews { 0 };
    unsigned m_nextPlaceTakenFromPropertiesWithViews { 0 };
    CachedStore& cachedStore(Structure* structure, UniquedStringImpl* name, EncodedJSValue discriminant) { return m_cachedStores[placeOf<numberOfCachedStores>(std::bit_cast<uintptr_t>(structure) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(name)), 23) ^ static_cast<uint64_t>(discriminant) * 31)]; }
    UncheckedKeyHashSet<Structure*> m_structuresOfPlans;
    UncheckedKeyHashMap<uint32_t, OrderTable*, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_orderTables;
    Vector<std::unique_ptr<OrderTable>> m_createdOrderTables;
    // By shape and cell size. See Instance::TypedAllocation::fieldTemplate.
    const void* fieldTemplate(const LayoutData&, uint32_t cellSize);
    UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, MallocSpan<uint8_t>> m_fieldTemplates;
    // By shape and prototype, where that is not the usual one. They live as long as their objects.
    WeakGCMap<std::pair<uint32_t, JSObject*>, Structure> m_structuresWithOtherPrototype;
    Vector<std::unique_ptr<WatchedMembers>> m_watchedMembers;
    // By constructor: EngineClass::index. Filled when first asked. The global object keeps them.
    UncheckedKeyHashMap<JSObject*, uint16_t> m_engineClassesByConstructor;
    // Instance::typedStrings. The program alone writes it, under m_cellsLock: the collector visits it meanwhile.
    JSCell* stringIfMade(uint32_t) const;
    void keepString(uint32_t, JSCell*);
    template<typename Visitor> void visitStrings(Visitor&);
    uint32_t m_numberOfStringsMade { 0 };
    void createStatics();
    void destroyStatics();
    void fillStatics(uint32_t firstWord, uint32_t endWord);
    void fillStaticsOfProgram();
    bool m_areStaticsOfProgramFilled { false };
    uint32_t m_sizeOfStaticsWithoutAccess { 0 };
    Vector<std::unique_ptr<GlobalPropertyWatchpoint>> m_globalPropertyWatchpoints;
    // Not released, and still to be read: compiled code may hold the address of one, and finds in it what was there or nothing. It leans on OSAllocator::decommit() leaving the pages
    // readable (madvise and no more): with an mprotect(PROT_NONE) in there, as upstream has had, such a read would fault.
    Vector<uint64_t*> m_formerStringTables;
    Vector<uint64_t> m_madeKeys;
    size_t m_numberOfMadeKeysOverwritten { 0 };
    Vector<JSCell*> m_wideStrings;
    Vector<std::pair<String, JSObject*>> m_importMetaOfModules;
    // What an earlier collection has marked stays marked in a collection of the young, which is handed those alone that were made since the last visit of the collection before: as Cell32;
    // how many of them the collector had been handed at its last visit; which collection that visit was of.
    struct MadeLately {
        // True: that was all this visit needs.
        template<typename Visitor, typename Functor> bool visit(VM&, Visitor&, const Functor& handOver);

        Vector<uint32_t> cells;
        Vector<uint32_t> earlier;
        size_t numberVisited { 0 };
        HeapVersion versionOfLastVisit { 0 };
    };
    MadeLately m_stringsMadeLately;
    MadeLately m_stringsBackedByStaticDataMadeLately;
    bool m_isLastStringConstantBackedByStaticData { false };
    template<typename Visitor> void visitClosures(Visitor&);
    MadeLately m_closuresMadeLately;
    static constexpr unsigned closuresInStretchShift = 10;
    BitVector m_stretchesWithClosures;
    // The closures, prototypes and Structures that were made to stay. The collector visits them while the program adds to them.
    Lock m_cellsLock;
    Vector<JSCell*> m_cells WTF_GUARDED_BY_LOCK(m_cellsLock);
    // See visit(). How many of them the collector had been handed at its last visit; how many at the last visit of the collection before this one; which collection that visit was of.
    size_t m_numberOfCellsVisited WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    size_t m_numberOfCellsMarkedBefore WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    HeapVersion m_versionOfLastVisit WTF_GUARDED_BY_LOCK(m_cellsLock) { 0 };
    HeapVersion m_versionOfLastVisitOfAllCards { 0 };
    template<typename Visitor> void visitRootsWithCards(Visitor&);
    // Pairs of AllocationData::constructedBy and the allocation, ascending. Made when first asked for.
    // Young cells that old arrays hold which are not remembered: see rememberElement(). From here to Instance::youngRootsCursor. The program adds to it only while no collection is under way.
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
