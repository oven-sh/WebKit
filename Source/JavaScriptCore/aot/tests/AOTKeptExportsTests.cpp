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

    AOT_TEST_CHECK(modules.keepExportToReplace(dep, greet.impl(), nullptr, kept, keptUnder.impl()));
    AOT_TEST_CHECK(kept->getDirect(vm, keptUnder) == jsNumber(1));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(1));
    AOT_TEST_CHECK(modules.storeIntoKeptPlace(kept, keptUnder.impl(), jsNumber(5)));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(5));
    AOT_TEST_CHECK(modules.readKeptPlace(kept, keptUnder.impl()) == jsNumber(5));

    // The first value stays the one to put back.
    AOT_TEST_CHECK(modules.keepExportToReplace(dep, greet.impl(), nullptr, kept, keptUnder.impl()));
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

    AOT_TEST_CHECK(modules.keepExportToReplace(dep, late.impl(), nullptr, kept, late.impl()));
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
        AOT_TEST_CHECK(!modules.keepExportToReplace(dep, name.impl(), nullptr, kept, name.impl()));
        AOT_TEST_CHECK(!kept->getDirect(vm, name));
        AOT_TEST_CHECK(!modules.storeIntoKeptPlace(kept, name.impl(), jsNumber(9)));
    }
    AOT_TEST_CHECK(exported(program, Identifier::fromString(vm, "fixed"_s)) == jsNumber(1));

    Identifier greet = Identifier::fromString(vm, "greet"_s);
    AOT_TEST_CHECK(modules.tryReplaceExport(dep, greet.impl(), jsNumber(6)));
    AOT_TEST_CHECK(exported(program, greet) == jsNumber(6));
    AOT_TEST_CHECK(!scope.exception());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<keptExportsTests, programText, body>;

std::span<const NativeTest> keptExportsTests()
{
    static constexpr NativeTest tests[] = {
        { "kept-exports/a-store-and-the-put-back-go-to-the-kept-place"_s, withProgram<checkStoreAndPutBackGoToKeptPlace> },
        { "kept-exports/an-export-without-a-value-is-kept-as-the-table"_s, withProgram<checkExportWithoutValueIsKeptAsTable> },
        { "kept-exports/nothing-is-kept-where-nothing-is-replaced"_s, withProgram<checkNothingIsKeptWhereNothingIsReplaced> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
