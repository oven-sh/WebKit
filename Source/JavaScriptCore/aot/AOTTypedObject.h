/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedData.h"
#include "AOTTypedFormat.h"
#include "JSObject.h"
#include "WeakGCHashTable.h"
#include "WeakGCMap.h"

namespace JSC {

class AbstractModuleRecord;
class ArgList;
class HeapAnalyzer;
class JSPromise;

namespace AOT {

struct Instance;

namespace Typed {

struct EngineClass;
struct State;

// In place of the number of an order: the keys are in the order of the fields of the layout. For a shape that has run out of numbers.
static constexpr uint32_t keysAreInLayoutOrder = 0xffff;
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
    static Object* create(VM&, Structure*, size_t size, uint32_t orderWord);
    // Of a class, as compiled code makes it: every field is there, and holds `undefined` if it can.
    static Object* createWithAllFields(VM&, Structure*, size_t size, uint32_t orderWord);

    const LayoutData& layout() const;
    State& state() const;
    // Empty: the field is absent.
    JSValue read(const FieldData&) const;
    // For the run time's own stores. False: the value is not of what the slot holds, or the field is immutable.
    bool write(VM&, const FieldData&, JSValue);
    // A store by code that was not compiled with the type: see State::holdToTypeOf(). False: it has thrown.
    bool assign(JSGlobalObject*, const FieldData&, JSValue);

    uint32_t orderWord() const { return at<uint32_t>(offsetOfOrderWord); }
    uint32_t shape() const { return shapeOf(at<uint32_t>(offsetOfShapeWord)); }
    bool isFrozen() const { return orderWord() & orderWordIsFrozen; }
    bool isSealed() const { return orderWord() & orderWordIsSealed; }
    bool isNotExtensible() const { return orderWord() & orderWordIsNotExtensible; }
    // SetIntegrityLevel, or [[PreventExtensions]] alone. False: an exception was thrown.
    enum class Integrity : uint8_t { NotExtensible, Sealed, Frozen };
    bool setIntegrity(JSGlobalObject*, Integrity);
    // The properties the layout has no field for, in an ordinary object that nobody else sees.
    JSObject* extraProperties() const;
    const FieldData* fieldNamed(State&, PropertyName) const;
    // The field is absent and is about to be stored to: it is the last of the keys now.
    void didCreateField(State&, const FieldData&);
    // [[Set]] of a property the layout has no field for. Where that would run code of the program, it throws.
    void putExtraWithoutRunningCode(JSGlobalObject*, PropertyName, JSValue);
    // OrdinaryCreateFromConstructor, for an object nobody has seen yet.
    void setPrototypeAtBirth(VM&, JSObject*);

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
    friend class Coercion;

    Object(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    template<typename T> T& at(uint32_t offset) const { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<Object*>(this)) + offset); }

    uint16_t indexOf(const FieldData& field) const { return &field - layout().fields(); }
    // The value as the field keeps it, whatever the type of the field. False: it cannot be kept that way.
    bool store(VM&, const FieldData&, JSValue);
    // Absent if it can be.
    void clear(const FieldData&);
    void setOrderAndCount(uint32_t orderAndCount) { at<uint32_t>(offsetOfOrderWord) = (orderWord() & ~orderAndCountMask) | orderAndCount; }
    void forEachFieldInKeyOrder(State&, const Invocable<void(uint16_t)> auto&) const;
    JSObject* ensureExtraProperties(VM&);
    void willAddExtraProperty(VM&, PropertyName);
    void dropExtraPropertiesIfEmpty(VM&);
    // From here on it is an object of the shape, which is among the specializations of its own. What that has more is absent. It keeps its
    // prototype, also where it was made by a class and the shape is that of a record.
    void specialize(VM&, State&, uint32_t shape);
};

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

private:
    VM& m_vm;
    Lock m_lock;
    UncheckedKeyHashMap<Object*, JSObject*> m_map;
};

// A function of the program as a value: where its code is, and the variables around it.
//
// As an object it is what the function is declared as. An async function, a generator function and an async generator function inherit
// from what the realm has for each, and do not construct. A generator function has a `prototype`, which its generators inherit from.
//
// The closure of the constructor of a class is the class object. What it has, and the prototype object, State::makeClass() gives it. Of a
// class with fixed members the static fields are variables of the program, which compiled code accesses as such. The class object shows
// them as data properties. What would make one of them anything else throws a TypeError, like a store of a value that is not of the type
// of the variable. So does another prototype for the class object, and a property of its own by the name of a static method or
// accessor it inherits: compiled code calls those without looking.
class Closure final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesPut | OverridesGetCallData | OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames | ImplementsDefaultHasInstance | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static Closure* create(VM&, Structure*, uint64_t entry, JSCell* environment, uint32_t function);

    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(Closure, m_entry); }
    static constexpr ptrdiff_t offsetOfEnvironment() { return OBJECT_OFFSETOF(Closure, m_environment); }
    static constexpr ptrdiff_t offsetOfFunction() { return OBJECT_OFFSETOF(Closure, m_function); }
    static constexpr size_t allocationSize() { return sizeof(Closure); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    Format::FunctionKind kind() const;
    // functionIsAsync, functionIsGenerator: whether it is any of those given.
    bool isDeclaredAs(uint8_t) const;
    // It is of a generator function of which it is not the one closure: see throwSharesPrototypeProperty().
    bool sharesPrototypeProperty() const;
    JSCell* environment() const { return m_environment; }
    uint32_t functionIndex() const { return m_function; }
    friend struct State;
    // The class it is the class object of. Null: it is none.
    const LayoutData* classData() const;
    // What [[Construct]] does once `this` is there, which is `undefined` for a class with a foreign root. Yields what the function returns:
    // an object takes the place of `this`.
    JSValue callAsConstructor(JSGlobalObject*, JSValue thisValue, JSValue newTarget, const ArgList&);

private:
    Closure(VM& vm, Structure* structure, uint64_t entry, JSCell* environment, uint32_t function)
        : Base(vm, structure)
        , m_entry(entry)
        , m_environment(environment)
        , m_function(function)
    {
    }

    // `length`, `name` and `prototype` become ordinary properties, which is what anything but a read of them needs.
    void reifyProperties(VM&, JSGlobalObject*);
    bool makePrototypeProperty(VM&, State&);

    // The address of the code that takes its arguments as the engine passes them, with how many it takes.
    uint64_t m_entry;
    JSCell* m_environment;
    uint32_t m_function;
    bool m_hasReifiedProperties { false };
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
    void willSuspendAtAwait() { m_isSuspendingAtAwait = true; }
    // An await in the body has settled.
    void resumeFromAwait(JSGlobalObject*, JSValue resolution, bool isRejected);
    // The value given to return() on a completed generator has settled.
    void didAwaitReturn(JSGlobalObject*, JSValue resolution, bool isRejected);

private:
    AsyncGenerator(VM& vm, Structure* structure, Closure* resumeClosure, JSArray* queue)
        : Base(vm, structure)
        , m_resumeClosure(resumeClosure)
        , m_queue(queue)
    {
    }

    Object* frame() const;
    void drain(JSGlobalObject*);
    void run(JSGlobalObject*, int32_t mode);
    void settleFrontRequest(JSGlobalObject*, JSValue, bool isDone, bool isRejected);

    Closure* m_resumeClosure;
    // Triples of resume mode, value and promise. Settled requests are before m_head.
    JSArray* m_queue;
    unsigned m_head { 0 };
    // Executing, suspended at an await, or awaiting the value given to return().
    bool m_isRunningOrAwaiting { false };
    // The body has started an await and is about to return to the driver.
    bool m_isSuspendingAtAwait { false };
};

// The generators of the program inherit from %GeneratorPrototype% and %AsyncGeneratorPrototype% of the realm, as those of the engine do.
// From here on next(), return() and throw() of the two take both. To be called before the two are made immutable. Once is enough.
JS_EXPORT_PRIVATE void installGeneratorMethods(JSGlobalObject*);

// Runs the resume function of a suspended async function. The context is its resume closure.
JS_EXPORT_PRIVATE void resumeAsyncFunction(JSGlobalObject*, VM&, JSValue context, JSValue resolution, bool isRejected);

// What an instance of the program keeps for compiled code.
struct State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(VM&, Instance&, const Data&);
    ~State();

    const Identifier& name(VM&, uint32_t);
    JSString* string(VM&, uint32_t);
    const LayoutData& layoutOfShape(uint32_t shape) const { return *data.at<LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]); }
    const AllocationData& allocation(uint32_t number) const { return data.at<AllocationData>(data.allocationsOffset)[number]; }
    // By TypeID.
    const RuntimeType& type(uint32_t index) const { return *data.at<RuntimeType>(data.at<uint32_t>(data.typesOffset)[index]); }
    // What the slot holds, and what the type of the layout says the property is.
    const RuntimeType& typeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.type); }
    const RuntimeType& ownTypeOf(const FieldData& field) const { return *data.at<RuntimeType>(field.ownType); }
    // See Data::functionKindsOffset.
    uint8_t kindAndDeclarationOf(uint32_t function) const { return data.at<uint8_t>(data.functionKindsOffset)[function]; }
    Format::FunctionKind kindOf(uint32_t function) const { return static_cast<Format::FunctionKind>(kindAndDeclarationOf(function) & functionKindMask); }
    // By LayoutData::index. Null: not a class.
    const LayoutData* classWithIndex(uint32_t) const;
    // With the prototype the objects of the shape have unless the allocation, or whoever constructs, says otherwise.
    Structure* structure(VM&, uint32_t shape);
    // There is one for each layout and prototype.
    Structure* structureFor(VM&, uint32_t shape, JSValue prototype);
    // Fills the entry of Instance::typedAllocations.
    JSCell* allocate(VM&, uint32_t allocation);
    // What an object of the shape takes, so that it can be made one of a specialization where it is.
    uint32_t sizeWithRoomToSpecialize(uint32_t shape);
    // By the generic entrypoint of the function. Its prototype is that of functions, async functions, generator functions or async
    // generator functions.
    Structure* closureStructure(VM&, uint32_t function);
    // Of the generators that a function makes, by the generic entrypoint of the function that runs its body. They inherit from the
    // `prototype` of the function.
    Structure* generatorStructure(VM&, uint32_t resumeFunction, bool isAsync);
    // Both by generic entrypoint.
    uint32_t resumeFunctionOf(uint32_t generatorFunction) const;
    // The class whose constructor has that generic entrypoint. Null: there is none.
    const LayoutData* classConstructedBy(uint32_t function) const;
    // Gives the closure of the constructor what makes it the class object, and makes the prototype object. `base`: what stands after
    // `extends`, where that is not known from the class alone. Throws only for a class with a foreign root.
    void makeClass(JSGlobalObject*, Closure*, JSValue base);
    // Of a class that is evaluated once.
    JSObject* prototypeOfClass(VM&, uint32_t classIndex);
    // Null: the class object has not been made.
    JSObject* prototypeOfClassIfExists(uint32_t classIndex) const { return prototypesOfClasses[classIndex]; }
    // What [[Construct]] of the closure hands it for `this`. Empty: it has thrown.
    JSValue createThis(JSGlobalObject*, Closure*, JSObject* newTarget);
    // Of a plain function, by its generic entrypoint: what makes an object of the layout it declares `this` to be of. Null: it declares none.
    const AllocationData* allocationOfThis(uint32_t function) const;
    Object* createObjectOfClass(VM&, const LayoutData&, JSObject* prototype);
    // The variables of a class, see LayoutData::variables(). Empty: it holds nothing yet.
    JSValue readVariable(const Closure* classObject, const LayoutData&, const FieldData&) const;
    // False: the value is not of the type of the variable.
    bool writeVariable(VM&, Closure* classObject, const LayoutData&, const FieldData&, JSValue);
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    // Whether what it inherits under the name is a static method or accessor of a class it extends that the program is not seen to replace.
    bool inheritsFixedStaticMethod(const LayoutData&, UniquedStringImpl*) const;
    // function (name) { return this[name] }
    JSFunction* liveBindingGetter(VM&);
    uint64_t entry(uint32_t function);
    Closure* closure(VM&, uint32_t function);
    template<typename Visitor> void visit(Visitor&);

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    bool isPrivate(const FieldData&) const;
    bool findPrivateMethod(VM&, const LayoutData&, UniquedStringImpl*, Object::PrivateElement&);
    // Whether what an object of the layout inherits under the name is a method or accessor, of its class or of one that class extends,
    // that the program is not seen to replace: compiled code goes by the shape of the object for it, and does not look.
    bool hasFixedMethod(const LayoutData&, UniquedStringImpl*) const;
    // Whether its class, or one that class extends, declares anything that the prototype object has.
    bool hasPrototypeMembers(const LayoutData&) const;
    bool mayBeAbsent(const FieldData& field) const;

    // Null: it is no object of a Record or Instance layout of the program.
    Object* compiledObject(JSValue) const;
    bool passes(const RuntimeShapeTest&, uint32_t shape) const;
    bool admitsShape(const RuntimeType&, uint32_t shape) const;
    // Of an object: whether the type has it for what it is, and not because it admits whatever object comes.
    bool fitsAsOneOfItsOwn(const RuntimeType&, JSObject*) const;
    // What compiled code tests where it holds a value to a type.
    bool fits(const RuntimeType&, JSValue) const;
    // As: what enters a place whose type is written down is made of the type, or turned away. Coerce, `value as T`: it is made of the type
    // if it can be, converted if it is of another kind, and let through if it is an object that nothing can be made of. Flow, where a
    // value is narrowed to the type: only an object of the program is made anything of, where it is. Empty: it has thrown.
    JSValue coerce(JSGlobalObject*, JSValue, const RuntimeType&, Format::CastReason, uint32_t function, uint32_t position);
    // What a field, or a variable of a class, is to hold when code that was not compiled with its type stores the value: the value, if it
    // is of the type. If the type is written down, what `as` makes of it. Empty: nothing, and the string may say why, or it has thrown.
    JSValue holdToTypeOf(JSGlobalObject*, const FieldData&, JSValue, String& failure);
    // Whether it is one of the strings, or of the numbers, that are all the type of the field allows. See FieldData::literalsOffset.
    bool isAmongLiterals(const FieldData&, JSValue) const;

    std::span<const uint16_t> fieldsInOrder(const LayoutData&, uint32_t order);
    // These yield orderWordOf(order, count).
    uint32_t orderStartingWith(const LayoutData&, std::span<const uint16_t> fields);
    uint32_t orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t field);

    JSObject* prototypeForObjectsOf(VM&, Closure*);
    void didChangePrototypeOf(VM&, Closure*);

    // For messages. None of them runs code of the program.
    static String describe(JSValue);
    String describe(const RuntimeType&) const;
    // As operationAOTTypedThrowTypeMismatch is told of a type.
    String describeKinds(uint32_t kinds, uint32_t name) const;
    String nameOf(const LayoutData&) const;
    String nameOfFunction(uint32_t) const;
    String describeRefusedStore(const String& place, const FieldData&, JSValue, const String& failure) const;
    String describePosition(uint32_t function, uint32_t position) const;
    // Options::reportAOTTypedCoercions(): a line for what `as` or `!` came to at the place. Each place once.
    void reportCoercion(uint32_t function, uint32_t position, const String& what);

    Instance& instance;
    const Data& data;
    // The module whose code the program's entry function is. External modules are its dependencies.
    AbstractModuleRecord* moduleRecord { nullptr };
    JSObject* importMeta { nullptr };
    // By the number of a string, a shape or a layout. Like the tables of the Instance, memory that reads as zero and costs nothing until written.
    Identifier* names { nullptr };
    Structure** structures { nullptr };
    JSObject** prototypesOfClasses { nullptr };
    ExtraPropertiesTable extraProperties;

private:
    using FieldTable = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
    // The orders of a shape: those the compiler expects, which are in the data, then those that have come up.
    struct OrderTable {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OrderTable);
        using Map = UncheckedKeyHashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        Vector<Vector<uint16_t>> added;
        // By the hash of a sequence of fields, an order that starts with it.
        Map byPrefix;
        // By order, count and the field that is created next.
        Map transitions;
    };

    void didCreate(JSCell*);
    bool hasText(uint32_t string, StringView) const;
    bool isName(uint32_t string, UniquedStringImpl*) const;
    void fill(VM&, uint32_t allocation);
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base == LayoutData::noBase ? nullptr : classWithIndex(layout.base); }
    bool isFixedIn(std::span<const MethodData>, UniquedStringImpl*, bool& isDeclared) const;
    // The class of the engine, of those whose objects compiled code knows by their type, that has this constructor.
    const EngineClass* engineClassConstructedBy(JSObject*);
    OrderTable& ordersOf(const LayoutData&);
    JSValue convert(JSGlobalObject*, JSValue, const RuntimeType&, uint32_t function, uint32_t position);
    std::span<Structure* const> closureStructures(VM&);
    void setPrototypeOfGenerators(VM&, uint32_t resumeFunction, bool isAsync, JSObject*);

    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_reportedCoercions;
    // As Instance::typedClosureStructureIDs.
    Structure* m_closureStructures[4] { };
    // By the generic entrypoint of the function that runs the body, made when first asked for.
    Structure** m_generatorStructures { nullptr };
    JSFunction* m_liveBindingGetter { nullptr };
    Vector<uint32_t> m_createdNames;
    // By shape number, made when first asked for.
    FieldTable** m_fieldTables { nullptr };
    Vector<std::unique_ptr<FieldTable>> m_createdFieldTables;
    OrderTable** m_orderTables { nullptr };
    Vector<std::unique_ptr<OrderTable>> m_createdOrderTables;
    uint32_t* m_sizesWithRoomToSpecialize { nullptr };
    // By shape and prototype, where that is not the usual one. They live as long as their objects.
    WeakGCMap<std::pair<uint32_t, JSObject*>, Structure> m_structuresWithOtherPrototype;
    // By constructor: EngineClass::index. Filled when first asked. The global object keeps them.
    UncheckedKeyHashMap<JSObject*, uint16_t> m_engineClassesByConstructor;
    // The strings, closures, prototypes and Structures that were made to stay. The collector visits them while the program adds to them.
    Lock m_cellsLock;
    Vector<JSCell*> m_cells WTF_GUARDED_BY_LOCK(m_cellsLock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
