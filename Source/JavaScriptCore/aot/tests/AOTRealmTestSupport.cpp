/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRealmTestSupport.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTRealmStartup.h"
#include "AOTVMState.h"
#include "Completion.h"
#include "ErrorInstance.h"
#include "GlobalObjectMethodTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "SourceCode.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"

namespace JSC::AOT {

template<typename Result, typename... Arguments>
[[noreturn]] static Result failOnModuleHostCall(Arguments...)
{
    RELEASE_ASSERT_NOT_REACHED();
}

static JSC_DECLARE_HOST_FUNCTION(requireNothing);

JSC_DEFINE_HOST_FUNCTION(requireNothing, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsUndefined());
}

static Identifier initialExportsName(VM& vm)
{
    return Identifier::fromString(vm, "initialExports"_s);
}

JSObject* createCommonJSModuleOfTestHost(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    JSObject* exports = constructEmptyObject(realm);
    JSObject* module = constructEmptyObject(realm);
    module->putDirect(vm, vm.propertyNames->exports, exports);
    module->putDirect(vm, initialExportsName(vm), exports);
    return module;
}

static ModuleAdoption adoptCommonJSModule(JSGlobalObject* realm, JSString* path, JSObject* module, CommonJSModuleObjects& objects)
{
    VM& vm = realm->vm();
    JSValue exports = module->getDirect(vm, vm.propertyNames->exports);
    if (!exports || exports != module->getDirect(vm, initialExportsName(vm)))
        return ModuleAdoption::Rejected;
    objects.module = module;
    objects.exports = exports;
    objects.require = JSFunction::create(vm, realm, 1, "require"_s, requireNothing, ImplementationVisibility::Public);
    objects.filename = path;
    objects.dirname = path;
    return ModuleAdoption::Adopted;
}

static JSValue requireBuiltinModule(JSGlobalObject* realm, uint32_t)
{
    VM& vm = realm->vm();
    JSObject* moduleObject = constructEmptyObject(realm);
    moduleObject->putDirect(vm, Identifier::fromString(vm, "propertyOfModuleObject"_s), jsNumber(1));
    return moduleObject;
}

static void captureSyntheticExports(JSGlobalObject* realm, SyntheticExportRule rule, JSValue value, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = realm->vm();
    AOT_TEST_CHECK(rule == SyntheticExportRule::RuntimeModule);
    Identifier property = Identifier::fromString(vm, "propertyOfModuleObject"_s);
    names.append(property);
    values.append(asObject(value)->getDirect(vm, property));
    names.append(vm.propertyNames->defaultKeyword);
    values.append(value);
}

static void captureNativeModuleExports(JSGlobalObject* realm, uint32_t registryID, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = realm->vm();
    names.append(vm.propertyNames->defaultKeyword);
    values.append(jsNumber(registryID));
    names.append(Identifier::fromString(vm, "listedByHost"_s));
    values.append(jsNumber(2));
}

static constexpr ModuleHost testModuleHost {
    .createCommonJSModule = failOnModuleHostCall,
    .adoptCommonJSModule = adoptCommonJSModule,
    .createRequirer = failOnModuleHostCall,
    .addChild = failOnModuleHostCall,
    .pushObservableChild = failOnModuleHostCall,
    .removeFromRequireCache = failOnModuleHostCall,
    .requireSpecifier = failOnModuleHostCall,
    .resolveSpecifier = failOnModuleHostCall,
    .captureSyntheticExports = captureSyntheticExports,
    .requireNativeModule = nullptr,
    .captureNativeModuleExports = captureNativeModuleExports,
    .requireCache = nullptr,
};

static JSGlobalObject* createPlainRealm(VM& vm)
{
    return JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
}

static JSGlobalObject* createRealmOfProgram(VM& vm, const GlobalObjectMethodTable& plainMethodTable)
{
    static GlobalObjectMethodTable methodTableWithHost = plainMethodTable;
    methodTableWithHost.moduleHost = &testModuleHost;
    methodTableWithHost.requireBuiltinModule = requireBuiltinModule;
    JSGlobalObject* realm = JSGlobalObject::createWithCustomMethodTable(vm, JSGlobalObject::createStructure(vm, jsNull()), &methodTableWithHost);
    AOT_TEST_CHECK(Instance::of(realm));
    return realm;
}

static JSObject* findOrCreateObject(JSGlobalObject* realm, BuiltinID id)
{
    if (id == Builtins::GlobalThis)
        return realm;
    auto found = findInRealm(realm, BuiltinCellKey::object(id));
    return found ? (*found)->getObject() : nullptr;
}

static void makeAlwaysLockedBuiltinsImmutable(VM& vm, JSGlobalObject* realm)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (BuiltinID id : table.lockedObjects()) {
        if (JSObject* object = findOrCreateObject(realm, id))
            AOT_TEST_CHECK(object->makePropertiesImmutable(vm));
    }
    BuiltinID previous;
    for (BuiltinID member : table.lockedMembers()) {
        BuiltinID holder = std::exchange(previous, table.at(member).holder);
        if (holder == previous)
            continue;
        if (JSObject* object = findOrCreateObject(realm, previous))
            AOT_TEST_CHECK(object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, previous).get()));
    }
}

static JSGlobalObject* createRealmWithImmutableBuiltins(VM& vm)
{
    JSGlobalObject* realm = createPlainRealm(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    makeAlwaysLockedBuiltinsImmutable(vm, realm);
    markOriginalStructuresPristine(vm, realm);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(!Instance::of(realm));
    return realm;
}

Realms::Realms(RuntimeValidation validation)
    : m_vm(VM::create())
{
    VM& vm = *m_vm;
    m_lock.emplace(vm);
    if (validation == RuntimeValidation::On)
        vm.aotState().validatesRuntime = true;
    m_plain.set(vm, createPlainRealm(vm));
    AOT_TEST_CHECK(!Instance::of(m_plain.get()));
    m_locked.set(vm, vm.hasCompiledCode() ? createRealmOfProgram(vm, *m_plain->globalObjectMethodTable()) : createRealmWithImmutableBuiltins(vm));
}

Realms::~Realms()
{
    m_vm->aotState().validatesRuntime = false;
    m_locked.clear();
    m_plain.clear();
    m_vm = nullptr;
    m_lock.reset();
}

JSValue Realms::evaluate(JSGlobalObject* realm, ASCIILiteral source)
{
    NakedPtr<Exception> exception;
    JSValue result = JSC::evaluate(realm, makeSource(source, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) [[unlikely]] {
        dataLogLn("threw ", exception->value().toWTFString(realm));
        failNativeTest(__FILE__, __LINE__, source.characters());
    }
    return result;
}

bool Realms::throws(JSGlobalObject* realm, ASCIILiteral source, ErrorType type)
{
    NakedPtr<Exception> exception;
    JSC::evaluate(realm, makeSource(source, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (!exception)
        return false;
    auto* error = dynamicDowncast<ErrorInstance>(exception->value());
    AOT_TEST_CHECK_FOR_SOURCE(error && error->errorType() == type, source);
    return true;
}

JSObject* Realms::holder(JSGlobalObject* realm, ASCIILiteral expression)
{
    JSObject* result = object(realm, expression);
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(result))
        return proxy->target();
    return result;
}

Identifier Realms::key(JSGlobalObject* realm, ASCIILiteral expression)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    Identifier result = evaluate(realm, expression).toPropertyKey(realm);
    AOT_TEST_CHECK_FOR_SOURCE(!scope.exception(), expression);
    return result;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
