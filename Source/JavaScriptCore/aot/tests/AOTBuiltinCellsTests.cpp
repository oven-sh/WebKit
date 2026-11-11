/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include "JSCustomGetterFunction.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "VMInlines.h"
#include "WeakInlines.h"
#include <bit>
#include <wtf/StackPointer.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> builtinCellsTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="buffer" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="maxByteLength" module=m0 declared={type=anything} initialization=Undefined
variable g2 name="symbol" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="bufferPrototype" module=m0 declared={type=anything} initialization=Undefined
variable g4 name="globalObject" module=m0 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0 exports=[{name="buffer" variable=g0}, {name="bufferPrototype" variable=g3}, {name="globalObject" variable=g4}, {name="maxByteLength" variable=g1}, {name="symbol" variable=g2}]
entry module=m0 hostReads=[{exportName="buffer"}, {exportName="maxByteLength"}, {exportName="symbol"}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=SymbolIterator
    v1 = Builtin builtin=ArrayBuffer
    v2 = Integer integer=8
    v3 = Construct v1, -, v2
    v4 = PropGet v3 key="maxByteLength"
    v5 = Builtin builtin=ArrayBufferPrototype
    v6 = Builtin builtin=GlobalThis
    PutModuleVariable v3 variable=g0
    PutModuleVariable v4 variable=g1
    PutModuleVariable v0 variable=g2
    PutModuleVariable v5 variable=g3
    PutModuleVariable v6 variable=g4
    Return
}
)IR"_s;

static BuiltinCells& cellsOf(LoadedProgram& program)
{
    return *program.state().builtinCells;
}

static uint32_t numberOf(LoadedProgram& program, BuiltinCellKey key)
{
    std::span<const uint32_t> keys = program.instance().tables().builtinCells();
    for (uint32_t number = 0; number < keys.size(); ++number) {
        if (keys[number] == key.encoded())
            return number;
    }
    failNativeTest(__FILE__, __LINE__, byteCast<char>(makeString("the image has a cell for "_s, WTF::enumName(key.kind()), " "_s, BuiltinTable::singleton().textOf(key.builtin())).utf8().data()));
}

static JSCell*& wordOf(LoadedProgram& program, uint32_t number)
{
    return program.instance().prefix().builtinCells[number];
}

static uint32_t putKeyInPlaceOf(LoadedProgram& program, BuiltinCellKey keyOfImage, BuiltinCellKey key)
{
    uint32_t number = numberOf(program, keyOfImage);
    AOT_TEST_CHECK(!wordOf(program, number));
    overwriteWordInImage(program.instance().tables().builtinCells()[number], key.encoded());
    return number;
}

static JSValue evaluate(LoadedProgram& program, ASCIILiteral source)
{
    return program.realms().evaluate(program.realm(), source);
}

static BuiltinCellKey nativeGetterKeyOf() { return BuiltinCellKey::getter(Builtins::ArrayBufferPrototypeMaxByteLength); }

static BuiltinCellKey customGetterKeyOf() { return BuiltinCellKey::getter(Builtins::IntlCollatorPrototypeCompare); }
static constexpr NamedBuiltin otherCustomAccessors[] = {
    Builtins::IntlLocalePrototypeBaseName, Builtins::IntlLocalePrototypeCalendar, Builtins::IntlLocalePrototypeCaseFirst, Builtins::IntlLocalePrototypeCollation,
    Builtins::IntlLocalePrototypeHourCycle, Builtins::IntlLocalePrototypeLanguage, Builtins::IntlLocalePrototypeNumberingSystem, Builtins::IntlLocalePrototypeNumeric,
    Builtins::IntlLocalePrototypeRegion, Builtins::IntlLocalePrototypeScript, Builtins::IntlNumberFormatPrototypeFormat, Builtins::IntlDateTimeFormatPrototypeFormat,
};

static NEVER_INLINE uintptr_t fillCellOfCustomGetter(LoadedProgram& program, uint32_t number)
{
    JSCell* getter = cellsOf(program).cell(number);
    AOT_TEST_CHECK(getter && getter->inherits<JSCustomGetterFunction>() && wordOf(program, number) == getter);
    return ~std::bit_cast<uintptr_t>(getter);
}

static NEVER_INLINE Vector<Weak<JSObject>> createUnreferencedCustomGetters(LoadedProgram& program)
{
    Vector<Weak<JSObject>> getters;
    for (BuiltinID accessor : otherCustomAccessors) {
        auto found = findInRealm(program.realm(), BuiltinCellKey::getter(accessor));
        AOT_TEST_CHECK(found && (*found)->inherits<JSCustomGetterFunction>());
        for (const Weak<JSObject>& earlier : getters)
            AOT_TEST_CHECK(earlier.get() != *found);
        getters.append(Weak<JSObject>(asObject(*found)));
    }
    return getters;
}

static void checkGetterOfCustomAccessorSurvivesACollection(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint32_t number = putKeyInPlaceOf(program, nativeGetterKeyOf(), customGetterKeyOf());
    uintptr_t hiddenGetter = fillCellOfCustomGetter(program, number);
    Vector<Weak<JSObject>> otherGetters = createUnreferencedCustomGetters(program);

    vm.heap.collectSync(CollectionScope::Full);
    JSCell* getter = std::bit_cast<JSCell*>(~hiddenGetter);
    AOT_TEST_CHECK(vm.heap.isMarked(getter) && wordOf(program, number) == getter && cellsOf(program).cell(number) == getter);

    unsigned numberOfCollectedGetters = 0;
    for (const Weak<JSObject>& other : otherGetters) {
        AOT_TEST_CHECK(other.get() != getter);
        numberOfCollectedGetters += !other;
    }
    AOT_TEST_CHECK(numberOfCollectedGetters);

    vm.heap.collectSync(CollectionScope::Eden);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.isMarked(getter) && getter->inherits<JSCustomGetterFunction>() && cellsOf(program).cell(number) == getter);
}

static void checkBuiltinOutsideTheRealm(LoadedProgram& program)
{
    AOT_TEST_CHECK(BuiltinTable::singleton().at(Builtins::File).mutability != BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(!findInRealm(program.realm(), BuiltinCellKey::object(Builtins::File)));
    uint32_t number = putKeyInPlaceOf(program, BuiltinCellKey::object(Builtins::ArrayBufferPrototype), BuiltinCellKey::object(Builtins::File));
    AOT_TEST_CHECK(!cellsOf(program).cell(number) && !wordOf(program, number));

    evaluate(program, "globalThis.File = { }"_s);
    AOT_TEST_CHECK(!cellsOf(program).cell(number) && !wordOf(program, number));
}

static void checkPerModuleMember(LoadedProgram& program)
{
    BuiltinCells& cells = cellsOf(program);
    JSGlobalObject* realm = program.realm();
    auto found = findInRealm(realm, BuiltinCellKey::object(Builtins::ModuleObjectExports));
    AOT_TEST_CHECK(!found && found.error() == BuiltinLookupFailure::IsPerModule);

    JSObject* modules = program.realms().object(realm, "globalThis.modules = { first: { exports: { } }, second: { exports: [] }, without: { }, primitive: { exports: 1 }, accessor: { get exports() { return { } } } }"_s);
    AOT_TEST_CHECK(modules);
    auto exportsOf = [&](ASCIILiteral holder) {
        return JSValue(cells.readMember(realm, program.realms().object(realm, holder), Builtins::ModuleObjectExports));
    };

    AOT_TEST_CHECK(exportsOf("modules.first"_s) == evaluate(program, "modules.first.exports"_s));
    AOT_TEST_CHECK(exportsOf("modules.second"_s) == evaluate(program, "modules.second.exports"_s));
    AOT_TEST_CHECK(exportsOf("modules.first"_s) != exportsOf("modules.second"_s));

    evaluate(program, "modules.first.exports = modules.second.exports"_s);
    AOT_TEST_CHECK(exportsOf("modules.first"_s) == exportsOf("modules.second"_s));

    for (ASCIILiteral holder : { "modules.without"_s, "modules.primitive"_s, "modules.accessor"_s })
        AOT_TEST_CHECK(!cells.readMember(realm, program.realms().object(realm, holder), Builtins::ModuleObjectExports));
}

static size_t stackAboveSoftLimit(VM& vm)
{
    uintptr_t stackPointer = std::bit_cast<uintptr_t>(currentStackPointer());
    uintptr_t limit = std::bit_cast<uintptr_t>(vm.softStackLimit());
    return stackPointer > limit ? stackPointer - limit : 0;
}

static size_t stackSeenByCreation;

static JSC_DECLARE_CUSTOM_GETTER(createHostObject);

JSC_DEFINE_CUSTOM_GETTER(createHostObject, (JSGlobalObject* realm, EncodedJSValue, PropertyName))
{
    stackSeenByCreation = stackAboveSoftLimit(realm->vm());
    return JSValue::encode(constructEmptyObject(realm));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<builtinCellsTests, programText, body>;

std::span<const NativeTest> builtinCellsTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-cells/getter-of-custom-accessor-survives-a-collection"_s, withProgram<checkGetterOfCustomAccessorSurvivesACollection> },
        { "builtin-cells/built-in-outside-the-realm"_s, withProgram<checkBuiltinOutsideTheRealm> },
        { "builtin-cells/per-module-member"_s, withProgram<checkPerModuleMember> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
