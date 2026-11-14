/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTModules.h"
#include "AOTNativeTest.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> keptExportsTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/dep.js" displayPath="dep.js" body=f1 exports=[{name="fixed" variable=g2}, {name="greet" variable=g0}, {name="late" variable=g1}]
entry module=m0
entry module=m1
variable g0 name="greet" module=m1 declared={type=anything} initialization=Undefined flags=[IsReplaceable]
variable g1 name="late" module=m1 declared={type=anything|absent} flags=[IsReplaceable]
variable g2 name="fixed" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="dep" module=m1 kind=Module results=[{type=anything}] {
    v0 = Integer integer=1
    PutModuleVariable v0 variable=g0
    v2 = Integer integer=2
    PutModuleVariable v2 variable=g1
    PutModuleVariable v0 variable=g2
    Return
}
)IR"_s;

static constexpr ModuleID dep { 1u };

static JSValue exported(LoadedProgram& program, const Identifier& name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSObject* namespaceObject = moduleNamespace(program.realm(), dep);
    AOT_TEST_CHECK(namespaceObject && !scope.exception());
    JSValue value = namespaceObject->get(program.realm(), name);
    AOT_TEST_CHECK(!scope.exception());
    return value;
}

// A store goes to the place that the table kept, and the kept value goes back there.
static void checkStoreAndPutBackGoToKeptPlace(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = modulesOf(program.instance());
    Identifier greet = Identifier::fromString(vm, "greet"_s);
    Identifier keptUnder = Identifier::fromString(vm, "handed on as"_s);
    JSObject* kept = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());

    evaluateModuleSynchronously(realm, dep);
    AOT_TEST_CHECK(!scope.exception());
    Modules::ReplaceablePlace place = modules.findPlaceToReplaceExport(dep, greet.impl());
    AOT_TEST_CHECK(place.kind == Modules::ReplaceablePlace::Kind::Variable);
    AOT_TEST_CHECK(modules.readReplaceablePlace(place) == jsNumber(1));

    // Nothing is kept yet, so the table has no place to store into.
    AOT_TEST_CHECK(!modules.storeIntoKeptPlace(kept, keptUnder.impl(), jsNumber(5)));
    AOT_TEST_CHECK(!modules.putBackKeptExport(kept, keptUnder.impl()));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(1));

    AOT_TEST_CHECK(modules.keepExportToReplace(dep, greet.impl(), nullptr, kept, keptUnder.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(kept->getDirect(vm, keptUnder) == jsNumber(1));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(1));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, keptUnder.impl(), jsNumber(5)));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(5));
    AOT_TEST_CHECK(modules.readKeptPlace(kept, keptUnder.impl()) == jsNumber(5));

    // The first value stays the one to put back.
    AOT_TEST_CHECK(modules.keepExportToReplace(dep, greet.impl(), nullptr, kept, keptUnder.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(kept->getDirect(vm, keptUnder) == jsNumber(1));
    AOT_TEST_CHECK(modules.putBackKeptExport(kept, keptUnder.impl()));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(1));

    // The names of the table are the export names alone: the places are out of the sight of a walk over them.
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    kept->getOwnPropertyNames(kept, realm, names, DontEnumPropertiesMode::Include);
    AOT_TEST_CHECK(names.size() == 1 && names[0] == keptUnder);
    AOT_TEST_CHECK(!scope.exception());
}

// An export that has no value yet is kept as the table itself. Its place is read again once the body has run, and it does not go back to no value.
static void checkExportWithoutValueIsKeptAsTable(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = modulesOf(program.instance());
    Identifier late = Identifier::fromString(vm, "late"_s);
    JSObject* kept = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());

    AOT_TEST_CHECK(modules.keepExportToReplace(dep, late.impl(), nullptr, kept, late.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(kept->getDirect(vm, late) == JSValue(kept));
    AOT_TEST_CHECK(!modules.readKeptPlace(kept, late.impl()));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, late.impl(), jsNumber(7)));
    AOT_TEST_CHECK(modules.readKeptPlace(kept, late.impl()) == jsNumber(7));

    evaluateModuleSynchronously(realm, dep);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(modules.readKeptPlace(kept, late.impl()) == jsNumber(2));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, late.impl(), jsNumber(7)));
    AOT_TEST_CHECK(modules.putBackKeptExport(kept, late.impl()));
    AOT_TEST_CHECK(exported(program, late) == jsNumber(7));
    AOT_TEST_CHECK(!scope.exception());
}

// Where tryReplaceExport() stores nothing, nothing is kept either.
static void checkNothingIsKeptWhereNothingIsReplaced(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = modulesOf(program.instance());
    JSObject* kept = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());

    evaluateModuleSynchronously(realm, dep);
    AOT_TEST_CHECK(!scope.exception());
    for (ASCIILiteral text : { "fixed"_s, "absent"_s }) {
        Identifier name = Identifier::fromString(vm, text);
        AOT_TEST_CHECK(!modules.findPlaceToReplaceExport(dep, name.impl()));
        AOT_TEST_CHECK(!modules.tryReplaceExport(dep, name.impl(), jsNumber(9)));
        AOT_TEST_CHECK(modules.keepExportToReplace(dep, name.impl(), nullptr, kept, name.impl()) == KeptExport::Refused);
        AOT_TEST_CHECK(!kept->getDirect(vm, name));
        AOT_TEST_CHECK(!modules.storeIntoKeptPlace(kept, name.impl(), jsNumber(9)));
    }
    AOT_TEST_CHECK(exported(program, Identifier::fromString(vm, "fixed"_s)) == jsNumber(1));

    Identifier greet = Identifier::fromString(vm, "greet"_s);
    AOT_TEST_CHECK(modules.tryReplaceExport(dep, greet.impl(), jsNumber(6)));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(6));
    AOT_TEST_CHECK(!scope.exception());
}

// dep.js has an instance for each load. Its export is a field of the instance, and each load gives it the number that the test laid on globalThis.
static constexpr ASCIILiteral textWithInstances = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/dep.js" displayPath="dep.js" flags=[MayBeMocked, MayBeReevaluated] body=f1 exports=[{name="step" kind=Field layout=L0 field=0}]
entry module=m0
entry module=m1
layout L0 kind=Environment module=m1 fields=[{key=@"0" declared={type=anything|absent flags=[IsVariableOfModule]} flags=[IsReplaceable] variableName="step"}, {key=@"1" declared={type=anything|absent flags=[IsFromCompiler]}}, {key=@"2" declared={type=anything|absent flags=[IsFromCompiler]}}]
variable g0 module=m1 declared={type=env L0 flags=[IsFromCompiler]} flags=[HoldsInstance]
variable g1 module=m1 declared={type=anything} initialization=Undefined flags=[IsReplaceable, GuardsInstance]
variable g2 module=m1 declared={type=anything} initialization=Function function=f2 flags=[IsReplaceable, AllocatesInstance]
variable g3 module=m1 declared={type=anything} initialization=Undefined flags=[IsReplaceable, HasSeveralInstances]
moduleinstance module=m1 layout=L0 namespaceField=1 deferredNamespaceField=2
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="dep" module=m1 kind=Module results=[{type=anything}] {
  b0():
    v0 = GetModuleVariable variable=g0
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="load"
    PutEnvironmentField v0, v2 layout=L0
    Return
}
function f2 name="allocateDep" module=m1 results=[{type=anything}] {
  b0():
    v0 = NewObject layout=L0
    PutModuleVariable v0 variable=g0
    v2 = Boolean boolean=true
    PutModuleVariable v2 variable=g1
    Return
}
)IR"_s;

// The module is loaded again after the store. The put-back reaches the first load, and the second load keeps its own value.
static void checkPutBackReachesLoadThatWasStoredInto(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = modulesOf(program.instance());
    Identifier step = Identifier::fromString(vm, "step"_s);
    JSObject* kept = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());
    auto load = [&](int number) {
        realm->putDirect(vm, Identifier::fromString(vm, "load"_s), jsNumber(number));
        AOT_TEST_CHECK(modules.ensureInstance(dep));
        evaluateModuleSynchronously(realm, dep);
        AOT_TEST_CHECK(!scope.exception());
    };

    load(1);
    AOT_TEST_CHECK(exported(program, step) == jsNumber(1));
    Modules::ReplaceablePlace first = modules.findPlaceToReplaceExport(dep, step.impl());
    AOT_TEST_CHECK(first.kind == Modules::ReplaceablePlace::Kind::Field && first.instance);
    AOT_TEST_CHECK(modules.keepExportToReplace(dep, step.impl(), nullptr, kept, step.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, step.impl(), jsNumber(7)));
    AOT_TEST_CHECK(exported(program, step) == jsNumber(7));

    releaseModule(realm, dep);
    load(2);
    Modules::ReplaceablePlace second = modules.findPlaceToReplaceExport(dep, step.impl());
    AOT_TEST_CHECK(second.kind == Modules::ReplaceablePlace::Kind::Field && second.instance && second.instance != first.instance);
    AOT_TEST_CHECK(exported(program, step) == jsNumber(2));
    AOT_TEST_CHECK(modules.readReplaceablePlace(first) == jsNumber(7));

    AOT_TEST_CHECK(modules.putBackKeptExport(kept, step.impl()));
    AOT_TEST_CHECK(modules.readReplaceablePlace(first) == jsNumber(1));
    AOT_TEST_CHECK(exported(program, step) == jsNumber(2));

    // A later keep finds that the name leads to the second load, and keeps that one.
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, step.impl(), jsNumber(7)));
    AOT_TEST_CHECK(modules.keepExportToReplace(dep, step.impl(), nullptr, kept, step.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(modules.readReplaceablePlace(first) == jsNumber(1));
    AOT_TEST_CHECK(kept->getDirect(vm, step) == jsNumber(2));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, step.impl(), jsNumber(9)));
    AOT_TEST_CHECK(exported(program, step) == jsNumber(9));
    AOT_TEST_CHECK(modules.putBackKeptExport(kept, step.impl()));
    AOT_TEST_CHECK(exported(program, step) == jsNumber(2));
    AOT_TEST_CHECK(!scope.exception());
}

// file.cjs gives its export the number that the test laid on globalThis. The exports of a CommonJS file are in a table that is made when they are first asked for.
static constexpr ASCIILiteral textWithCommonJSFile = R"IR(layout L0 module=m1 fields=[{key="greet" declared={type=anything} flags=[IsRequired]}] ownFieldCount=1
variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/file.cjs" displayPath="file.cjs" kind=CommonJs flags=[CachesEvaluationError, MayBeMocked, MayBeEvicted] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0
entry module=m0
entry module=m1
function f0 name="main" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f1 name="file" module=m1 kind=CommonJsModule results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="load"
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    PutModuleVariable v3 variable=g0
    Return
}
)IR"_s;

// An entry of a table of exports is kept, replaced and put back. A name that the table does not have is refused, as tryReplaceExport() refuses it.
static void checkEntryOfTableOfExportsIsKept(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Modules& modules = modulesOf(program.instance());
    Identifier greet = Identifier::fromString(vm, "greet"_s);
    Identifier absent = Identifier::fromString(vm, "absent"_s);
    JSObject* kept = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());

    realm->putDirect(vm, Identifier::fromString(vm, "load"_s), jsNumber(1));
    requireModule(realm, dep);
    AOT_TEST_CHECK(!scope.exception());
    Modules::ReplaceablePlace place = modules.findPlaceToReplaceExport(dep, greet.impl());
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(place.kind == Modules::ReplaceablePlace::Kind::CapturedTable && place.table);
    AOT_TEST_CHECK(modules.readReplaceablePlace(place) == jsNumber(1));

    AOT_TEST_CHECK(modules.keepExportToReplace(dep, absent.impl(), nullptr, kept, absent.impl()) == KeptExport::Refused);
    AOT_TEST_CHECK(!modules.tryReplaceExport(dep, absent.impl(), jsNumber(9)));
    AOT_TEST_CHECK(modules.keepExportToReplace(dep, greet.impl(), nullptr, kept, greet.impl()) == KeptExport::Kept);
    AOT_TEST_CHECK(kept->getDirect(vm, greet) == jsNumber(1));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, greet.impl(), jsNumber(7)));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(7));
    AOT_TEST_CHECK(modules.readKeptPlace(kept, greet.impl()) == jsNumber(7));
    AOT_TEST_CHECK(modules.putBackKeptExport(kept, greet.impl()));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(1));
    AOT_TEST_CHECK(!scope.exception());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<keptExportsTests, programText, body>;
template<LoadedProgramTest body>
static constexpr void (*withCommonJSFile)() = runNativeTestWithLoadedProgram<keptExportsTests, textWithCommonJSFile, body>;
template<LoadedProgramTest body>
static constexpr void (*withInstances)() = runNativeTestWithLoadedProgram<keptExportsTests, textWithInstances, body>;

std::span<const NativeTest> keptExportsTests()
{
    static constexpr NativeTest tests[] = {
        { "kept-exports/a-store-and-the-put-back-go-to-the-kept-place"_s, withProgram<checkStoreAndPutBackGoToKeptPlace> },
        { "kept-exports/an-export-without-a-value-is-kept-as-the-table"_s, withProgram<checkExportWithoutValueIsKeptAsTable> },
        { "kept-exports/nothing-is-kept-where-nothing-is-replaced"_s, withProgram<checkNothingIsKeptWhereNothingIsReplaced> },
        { "kept-exports/the-put-back-reaches-the-load-that-was-stored-into"_s, withInstances<checkPutBackReachesLoadThatWasStoredInto> },
        { "kept-exports/an-entry-of-a-table-of-exports-is-kept"_s, withCommonJSFile<checkEntryOfTableOfExportsIsKept> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
