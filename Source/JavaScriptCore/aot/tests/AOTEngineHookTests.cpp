/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTClosure.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeTable.h"
#include "AOTVMState.h"
#include "FunctionPrototype.h"
#include "GlobalObjectMethodTable.h"
#include "InternalFunction.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSCallee.h"
#include "JSFunctionWithFields.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSWeakObjectRef.h"
#include "MapConstructor.h"
#include "NullSetterFunction.h"
#include "PropertyRun.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#include <array>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> engineHookTests();

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

namespace {

struct CreatorFlags {
    bool hasSpecialProperties;
    bool hasUnderscoreProtoProperty;
    bool allowsQuickEnumeration;
    PropertyOffset maxOffset;

    friend bool operator==(const CreatorFlags&, const CreatorFlags&) = default;
};

struct ArrayState {
    unsigned length;
    IndexingType indexingMode;
    Vector<EncodedJSValue> elements;

    friend bool operator==(const ArrayState&, const ArrayState&) = default;
};

struct FunctionCase {
    JSCell* cell;
    const ClassInfo* classInfo;
};

struct NonFunctionCase {
    JSCell* cell;
    JSType type;
    bool isCallable;
};

struct ProtectedObjectMembers : JSObject {
    using JSObject::tryConvertArrayStorageToContiguous;
};

constexpr ASCIILiteral lookupObjects[] = {
    "({})"_s, "chain()"_s,
    "(o => (delete o.p9, o))(chain())"_s,
    "(o => (delete o.p5, o))(chain())"_s,
    "Object.defineProperty(chain(), 'p5', { enumerable: false })"_s,
    "Object.seal(chain())"_s,
};

constexpr ASCIILiteral convertibleArrays[] = {
    "(a => { a[100000] = 1; for (var i = 0; i < 100000; i++) a[i] = i; return a; })([])"_s,
    "[1, 'a']"_s,
    "new (class A extends Array { })(1, 2)"_s,
};

constexpr ASCIILiteral unconvertibleArrays[] = {
    "(a => (a[100000] = 1, a))([])"_s,
    "Object.defineProperty([1, 2], 0, { get() { } })"_s,
    "Object.defineProperty([1, 2], 'length', { writable: false })"_s,
    "(a => (a.foo = 1, a))([1, 2])"_s,
    "Object.preventExtensions([1, 2])"_s,
};

void checkLookupsMatchGet(VM& vm, Structure* structure, std::span<const Identifier> names)
{
    PropertyTable* table = structure->materializedPropertyTable();
    Vector<std::pair<PropertyOffset, unsigned>, 4> lookups;
    for (const Identifier& name : names) {
        unsigned attributes = 0;
        std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(name.impl(), attributes);
        AOT_TEST_CHECK(offset);
        lookups.append({ *offset, attributes });
    }
    AOT_TEST_CHECK(structure->materializedPropertyTable() == table);
    for (size_t i = 0; i < names.size(); ++i) {
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, names[i], attributes);
        AOT_TEST_CHECK(lookups[i].first == offset && lookups[i].second == attributes);
    }
}

void checkLookups(VM& vm, Structure* structure, std::span<const Identifier> names)
{
    checkLookupsMatchGet(vm, structure, names);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!!structure->materializedPropertyTable() == structure->isPinnedPropertyTable());
    checkLookupsMatchGet(vm, structure, names);
}

CreatorFlags flagsOf(Structure* structure)
{
    AOT_TEST_CHECK(structure);
    return { structure->hasSpecialProperties(), structure->hasUnderscoreProtoPropertyExcludingOriginalProto(), structure->isQuickPropertyAccessAllowedForEnumeration(), structure->maxOffset() };
}

ArrayState stateOf(JSGlobalObject* realm, JSArray* array)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realm->vm());
    ArrayState state { array->length(), array->indexingMode(), { } };
    for (unsigned i = 0; i < state.length; ++i) {
        state.elements.append(JSValue::encode(array->getDirectIndex(realm, i)));
        AOT_TEST_CHECK(!scope.exception());
    }
    return state;
}

void checkConversion(Realms& realms, JSGlobalObject* realm, ASCIILiteral expression, bool shouldConvert)
{
    VM& vm = realms.vm();
    JSArray* array = realms.array(realm, expression);
    array->ensureArrayStorage(vm);
    ArrayState state = stateOf(realm, array);
    AOT_TEST_CHECK_FOR_SOURCE(hasAnyArrayStorage(state.indexingMode), expression);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK_FOR_SOURCE((array->*&ProtectedObjectMembers::tryConvertArrayStorageToContiguous)(vm) == shouldConvert, expression);
    if (shouldConvert)
        state.indexingMode = ArrayWithContiguous;
    AOT_TEST_CHECK_FOR_SOURCE(stateOf(realm, array) == state, expression);
    array->validatePristine(vm);
}

void checkPromiseAllOfElements(Realms& realms, JSGlobalObject* realm)
{
    VM& vm = realms.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    EncodedJSValue elements[] = { JSValue::encode(jsNumber(1)), JSValue::encode(realms.evaluate(realm, "promise"_s)), JSValue::encode(realms.evaluate(realm, "thenable"_s)) };
    JSPromise* promise = promiseAllOfElements(realm, elements);
    AOT_TEST_CHECK(promise && !scope.exception());
    vm.drainMicrotasks();
    AOT_TEST_CHECK(promise->status() == JSPromise::Status::Fulfilled);
    auto* values = dynamicDowncast<JSArray>(promise->result());
    AOT_TEST_CHECK(values && values->length() == std::size(elements));
    for (unsigned i = 0; i < values->length(); ++i)
        AOT_TEST_CHECK(values->getIndexQuickly(i) == jsNumber(i + 1));
}

} // anonymous namespace

static void testGetWithoutMaterializing()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* plain = realms.plain();
    Identifier names[] = { Identifier::fromString(vm, "p0"_s), Identifier::fromString(vm, "p5"_s), Identifier::fromString(vm, "p9"_s), Identifier::fromString(vm, "nameNobodyHas"_s) };
    realms.evaluate(plain, "function chain() { var o = {}; for (var i = 0; i < 10; i++) o['p' + i] = i; return o; }"_s);
    for (ASCIILiteral source : lookupObjects)
        checkLookups(vm, realms.object(plain, source)->structure(), names);
    Structure* chain = realms.object(plain, "chain()"_s)->structure();
    AOT_TEST_CHECK(!chain->isPinnedPropertyTable());

    JSObject* dictionary = realms.object(plain, "chain()"_s);
    dictionary->convertToDictionary(vm);
    checkLookups(vm, dictionary->structure(), names);
    Structure* empty = plain->objectStructureForObjectConstructor();
    UniquedStringImpl* addedNames[] = { names[0].impl(), names[1].impl() };
    const PropertyRun* run = vm.propertyRuns.intern(addedNames);
    AOT_TEST_CHECK(run);
    Structure* afterRun = Structure::addPropertyRunTransition(vm, empty, *run);
    AOT_TEST_CHECK(afterRun && afterRun->transitionKind() == TransitionKind::PropertyRunAddition);
    checkLookups(vm, afterRun, names);
    PropertyOffset offset;
    checkLookups(vm, Structure::addPropertyTransition(vm, afterRun, names[2], 0, offset), names);
}

static void testPropertyRunFlags()
{
    Realms realms;
    VM& vm = realms.vm();
    Structure* empty = realms.plain()->objectStructureForObjectConstructor();
    PropertyOffset offset;
    Structure* baseOfRuns = Structure::addPropertyTransition(vm, empty, Identifier::fromString(vm, "baseOfRuns"_s), 0, offset);
    Structure* baseOfChains = Structure::addPropertyTransition(vm, empty, Identifier::fromString(vm, "baseOfChains"_s), 0, offset);
    Identifier a = Identifier::fromString(vm, "a"_s);
    Identifier b = Identifier::fromString(vm, "b"_s);
    std::pair<std::array<UniquedStringImpl*, 2>, CreatorFlags> nameLists[] = {
        { { a.impl(), b.impl() }, { false, false, true, 2 } },
        { { a.impl(), vm.propertyNames->then.impl() }, { true, false, true, 2 } },
        { { vm.propertyNames->underscoreProto.impl(), a.impl() }, { false, true, true, 2 } },
        { { a.impl(), vm.propertyNames->iteratorSymbol.impl() }, { false, false, false, 2 } },
    };
    AOT_TEST_CHECK(flagsOf(baseOfRuns) == CreatorFlags({ false, false, true, 0 }));
    for (auto& [nameArray, expected] : nameLists) {
        std::span<UniquedStringImpl* const> names = nameArray;
        Structure* grown = baseOfChains;
        for (UniquedStringImpl* name : names)
            grown = Structure::addPropertyTransition(vm, grown, name, 0, offset);
        AOT_TEST_CHECK(flagsOf(grown) == expected);

        const PropertyRun* run = vm.propertyRuns.intern(names);
        AOT_TEST_CHECK(run);
        Structure* afterRun = Structure::addPropertyRunTransition(vm, baseOfRuns, *run);
        AOT_TEST_CHECK(afterRun && afterRun->transitionKind() == TransitionKind::PropertyRunAddition);
        AOT_TEST_CHECK(flagsOf(afterRun) == expected);
    }
}

static void testArrayStorageToContiguous()
{
    Realms realms;
    for (JSGlobalObject* realm : { realms.plain(), realms.locked() }) {
        for (ASCIILiteral expression : convertibleArrays)
            checkConversion(realms, realm, expression, true);
        for (ASCIILiteral expression : unconvertibleArrays)
            checkConversion(realms, realm, expression, false);
        realm->haveABadTime(realms.vm());
        for (ASCIILiteral expression : convertibleArrays)
            checkConversion(realms, realm, expression, false);
    }
}

static void testPromiseAllGenericPath()
{
    Realms realms;
    JSGlobalObject* plain = realms.plain();
    realms.evaluate(plain, "var log = []; var promise = new Promise(resolve => resolve(2)); var thenable = { then(resolve) { resolve(3); } };"_s);
    checkPromiseAllOfElements(realms, plain);
    AOT_TEST_CHECK(realms.evaluate(plain, "log.length"_s) == jsNumber(0));
    realms.evaluate(plain, "(original => { Promise.resolve = function (value) { log.push(value); return original.call(this, value); }; })(Promise.resolve)"_s);
    checkPromiseAllOfElements(realms, plain);
    AOT_TEST_CHECK(realms.evaluate(plain, "log.length === 3 && log[0] === 1 && log[1] === promise && log[2] === thenable"_s) == jsBoolean(true));
}

static void testFunctionTypes()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    auto [resolve, reject] = JSPromise::create(vm, realm->promiseStructure())->createResolvingFunctions(vm, realm);

    FunctionCase functions[] = {
        { realms.object(realm, "parseInt"_s), JSFunction::info() },
        { realms.object(realm, "(function () { })"_s), JSSloppyFunction::info() },
        { realms.object(realm, "(function () { 'use strict'; })"_s), JSStrictFunction::info() },
        { realms.object(realm, "(() => { })"_s), JSArrowFunction::info() },
        { realms.object(realm, "(function () { }).bind(null)"_s), JSBoundFunction::info() },
        { resolve, JSFunctionWithFields::info() },
        { reject, JSFunctionWithFields::info() },
        { realms.object(realm, "Map"_s), MapConstructor::info() },
        { realm->nullSetterFunction(), NullSetterFunction::info() },
    };
    for (auto [cell, classInfo] : functions)
        AOT_TEST_CHECK(cell->classInfo() == classInfo && isFunctionType(cell->type()) && cell->isCallable());
    AOT_TEST_CHECK(MapConstructor::info()->isSubClassOf(InternalFunction::info()));
    AOT_TEST_CHECK(isFunctionType(Closure::createStructure(vm, realm, realm->functionPrototype())->typeInfo().type()));

    NonFunctionCase otherCells[] = {
        { realms.object(realm, "({})"_s), FinalObjectType, false },
        { realms.object(realm, "[]"_s), ArrayType, false },
        { realms.object(realm, "new Proxy(function () { }, {})"_s), ProxyObjectType, true },
        { realm->globalCallee(), JSCalleeType, false },
        { realms.evaluate(realm, "'abc'"_s).asCell(), StringType, false },
    };
    for (auto [cell, type, isCallable] : otherCells)
        AOT_TEST_CHECK(cell->type() == type && !isFunctionType(type) && cell->isCallable() == isCallable);
}

static void testOnlyRealmWithModuleHostHasInstance(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance* instance = Instance::of(program.realm());
    AOT_TEST_CHECK(program.realm()->globalObjectMethodTable()->moduleHost);
    AOT_TEST_CHECK(instance && instance->state);
    AOT_TEST_CHECK(instance->vm == &vm && instance->globalObject == program.realm());
    AOT_TEST_CHECK(vm.aotState().instances.size() == 1 && vm.aotState().instances[0] == instance);

    JSGlobalObject* realmWithoutHost = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    AOT_TEST_CHECK(!realmWithoutHost->globalObjectMethodTable()->moduleHost);
    AOT_TEST_CHECK(!Instance::of(realmWithoutHost));
    AOT_TEST_CHECK(vm.aotState().instances.size() == 1);
}

static void testImageIsAttachedToVM(LoadedProgram& program)
{
    VMState& state = program.vm().aotState();
    AOT_TEST_CHECK(state.hasCompiledCode && state.image == Image::current());
    AOT_TEST_CHECK(state.structureIDBase == structureIDBase());
    AOT_TEST_CHECK(state.runtimeTable && state.identifiers && state.caches && state.propertyCaches);
    AOT_TEST_CHECK(state.counters && state.reporter && state.callSiteCache && state.regExpLiterals);
    AOT_TEST_CHECK(state.moduleRecordStructure && state.stringBuilderStorageStructure);
    AOT_TEST_CHECK(program.instance().runtimeTable == state.runtimeTable->entries());
    AOT_TEST_CHECK(program.instance().image.programIdentifiers == state.identifiers->identifiers());
}

static void testClearingPristineBitsBumpsShapeEpoch(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    auto bumpsDuring = [&](ASCIILiteral source) {
        uint64_t epoch = instance.shapeEpoch;
        uint64_t count = valueOf(vm, RuntimeCounterID::ShapeEpochBumps);
        program.realms().evaluate(program.realm(), source);
        AOT_TEST_CHECK_FOR_SOURCE(instance.shapeEpoch - epoch == valueOf(vm, RuntimeCounterID::ShapeEpochBumps) - count, source);
        return instance.shapeEpoch - epoch;
    };

    AOT_TEST_CHECK(program.realms().object(program.realm(), "[1, 2, 3]"_s)->structure()->isPristine());
    bumpsDuring("(a => { a.foo = 1; })([1, 2, 3])"_s);
    AOT_TEST_CHECK(!bumpsDuring("(a => { a.foo = 1; })([1, 2, 3])"_s));
    AOT_TEST_CHECK(!bumpsDuring("(a => { a.index = 0; a.input = 'a'; })(['a'])"_s));
    AOT_TEST_CHECK(bumpsDuring("(a => { a.map = 1; })([1, 2, 3])"_s));
    AOT_TEST_CHECK(bumpsDuring("(a => { a['-1'] = 1; })(['a'])"_s));
    AOT_TEST_CHECK(bumpsDuring("Object.setPrototypeOf([1, 2, 3], {})"_s));
    AOT_TEST_CHECK(!bumpsDuring("(a => { a.foo = 1; })([1, 2, 3])"_s));
}

static void testInstanceClassInfos(LoadedProgram& program)
{
    const ClassInfo* const* classInfos = program.instance().realm.instanceClassInfos;
    AOT_TEST_CHECK(classInfos[BuiltinClasses::WeakRef.index()] == JSWeakObjectRef::info());

    AOT_TEST_CHECK(!BuiltinBindings::hasHost());
    const BuiltinTable& table = BuiltinTable::singleton();
    unsigned numberOfHostClasses = 0;
    for (unsigned index = 0; index < numberOfBuiltinClasses; ++index) {
        if (table.at(BuiltinClassID(index)).origin != BuiltinOrigin::Host)
            continue;
        ++numberOfHostClasses;
        AOT_TEST_CHECK(!classInfos[index]);
    }
    AOT_TEST_CHECK(numberOfHostClasses);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<engineHookTests, programText, body>;

std::span<const NativeTest> engineHookTests()
{
    static constexpr NativeTest tests[] = {
        { "engine-hooks/get-without-materializing"_s, testGetWithoutMaterializing },
        { "engine-hooks/property-run-flags"_s, testPropertyRunFlags },
        { "engine-hooks/array-storage-to-contiguous"_s, testArrayStorageToContiguous },
        { "engine-hooks/promise-all-generic-path"_s, testPromiseAllGenericPath },
        { "engine-hooks/function-types"_s, testFunctionTypes },
        { "engine-hooks/only-realm-with-module-host-has-instance"_s, withProgram<testOnlyRealmWithModuleHostHasInstance> },
        { "engine-hooks/image-is-attached-to-vm"_s, withProgram<testImageIsAttachedToVM> },
        { "engine-hooks/clearing-pristine-bits-bumps-shape-epoch"_s, withProgram<testClearingPristineBitsBumpsShapeEpoch> },
        { "engine-hooks/instance-class-infos"_s, withProgram<testInstanceClassInfos> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
