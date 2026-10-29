/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTHostEntriesTestSupport.h"
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

namespace JSC { namespace AOT {

std::span<const NativeTest> builtinCellsTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="buffer" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="length" module=m0 declared={type=anything} initialization=Undefined
variable g2 name="symbol" module=m0 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0 exports=[{name="buffer" variable=g0}, {name="length" variable=g1}, {name="symbol" variable=g2}]
entry module=m0 hostReads=[{exportName="buffer"}, {exportName="length"}, {exportName="symbol"}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=SymbolIterator
    v1 = Builtin builtin=ArrayBuffer
    v2 = Integer integer=8
    v3 = Construct v1, -, v2
    v4 = PropGet v3 key="byteLength"
    PutModuleVariable v3 variable=g0
    PutModuleVariable v4 variable=g1
    PutModuleVariable v0 variable=g2
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
    failNativeTest(__FILE__, __LINE__, "the image has a cell for the key");
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

static void checkCellIs(LoadedProgram& program, BuiltinCellKey key, ASCIILiteral source)
{
    uint32_t number = numberOf(program, key);
    AOT_TEST_CHECK(!wordOf(program, number));
    JSCell* cell = cellsOf(program).cell(number);
    AOT_TEST_CHECK_FOR_SOURCE(cell && JSValue(cell) == evaluate(program, source), source);
    AOT_TEST_CHECK_FOR_SOURCE(wordOf(program, number) == cell && cellsOf(program).cell(number) == cell, source);
}

static void checkKeys(LoadedProgram& program)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::span<const uint32_t> keys = program.instance().tables().builtinCells();
    AOT_TEST_CHECK(!keys.empty());
    for (uint32_t encoded : keys) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(encoded, table);
        AOT_TEST_CHECK(key && key->encoded() == encoded && key->hasCell(table));
    }

    AOT_TEST_CHECK(!BuiltinCellKey::decode(UINT32_MAX, table) && !BuiltinCellKey::decode(3u << 24, table) && !BuiltinCellKey::decode(table.builtinCount(), table));
    AOT_TEST_CHECK(BuiltinCellKey::decode(BuiltinCellKey::setter(Builtins::MathMax).encoded(), table) == BuiltinCellKey::setter(Builtins::MathMax));
    AOT_TEST_CHECK(BuiltinCellKey::object(Builtins::MathMax).hasCell(table) && BuiltinCellKey::object(Builtins::SymbolIterator).hasCell(table));
    AOT_TEST_CHECK(!BuiltinCellKey::object(Builtins::MathPI).hasCell(table) && !BuiltinCellKey::getter(Builtins::MathMax).hasCell(table));
    AOT_TEST_CHECK(BuiltinCellKey::getter(Builtins::ArrayBufferPrototypeByteLength).hasCell(table) && !BuiltinCellKey::setter(Builtins::ArrayBufferPrototypeByteLength).hasCell(table));
    AOT_TEST_CHECK(!BuiltinCellKey::getter(Builtins::ArrayPrototypeLength).hasCell(table));
    AOT_TEST_CHECK(!BuiltinCellKey::object(Builtins::ModuleObjectExports).hasCell(table) && !BuiltinCellKey::object(Builtins::ImportMeta).hasCell(table));
    AOT_TEST_CHECK(!BuiltinCellKey::object(Builtins::CompilerHelpers).hasCell(table) && BuiltinCellKey::object(Builtins::CompilerHelpersTraverseArray).hasCell(table));
    AOT_TEST_CHECK(!BuiltinCellKey::object(Builtins::JSONText).hasCell(table) && BuiltinCellKey::object(hostBuiltin("JSONTextResponseJson"_s)).hasCell(table));
}

static void checkObjects(LoadedProgram& program)
{
    checkCellIs(program, BuiltinCellKey::object(Builtins::ArrayBuffer), "ArrayBuffer"_s);
    checkCellIs(program, BuiltinCellKey::object(Builtins::ArrayBufferPrototype), "ArrayBuffer.prototype"_s);
    checkCellIs(program, BuiltinCellKey::object(Builtins::GlobalThis), "globalThis"_s);
    checkCellIs(program, BuiltinCellKey::object(Builtins::SymbolIterator), "Symbol.iterator"_s);

    AOT_TEST_CHECK(cellsOf(program).findObject(Builtins::ArrayBuffer) == cellsOf(program).cell(numberOf(program, BuiltinCellKey::object(Builtins::ArrayBuffer))));
    AOT_TEST_CHECK(JSValue(cellsOf(program).findObject(Builtins::MathMax)) == evaluate(program, "Math.max"_s));
    AOT_TEST_CHECK(!cellsOf(program).findObject(Builtins::Bun));

    auto promiseResolve = findInRealm(program.realm(), BuiltinCellKey::object(Builtins::LinkTimeConstantPromiseResolve));
    AOT_TEST_CHECK(promiseResolve && *promiseResolve == program.realm()->linkTimeConstant(LinkTimeConstant::promiseResolve));
}

static void checkCellsBelongToTheRealmOfTheProgram(LoadedProgram& program)
{
    JSCell* constructor = cellsOf(program).cell(numberOf(program, BuiltinCellKey::object(Builtins::ArrayBuffer)));
    AOT_TEST_CHECK(JSValue(constructor) != program.realms().evaluate(program.realms().plain(), "ArrayBuffer"_s));
    AOT_TEST_CHECK(asObject(constructor)->realm() == program.realm());
}

static void checkNativeGetter(LoadedProgram& program)
{
    checkCellIs(program, BuiltinCellKey::getter(Builtins::ArrayBufferPrototypeByteLength), "Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get"_s);
}

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
    // The engine's own custom accessors are read by name. Some of a host's have a cell.
    uint32_t number = putKeyInPlaceOf(program, BuiltinCellKey::getter(Builtins::ArrayBufferPrototypeByteLength), customGetterKeyOf());
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
    AOT_TEST_CHECK(BuiltinTable::singleton().at(Builtins::Bun).mutability != BuiltinMutability::AlwaysLocked);
    AOT_TEST_CHECK(!findInRealm(program.realm(), BuiltinCellKey::object(Builtins::Bun)));
    uint32_t number = putKeyInPlaceOf(program, BuiltinCellKey::object(Builtins::ArrayBufferPrototype), BuiltinCellKey::object(Builtins::Bun));
    AOT_TEST_CHECK(!cellsOf(program).cell(number) && !wordOf(program, number));

    evaluate(program, "globalThis.Bun = { }"_s);
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

static constexpr size_t sizeOfDeepFrame = 4 * KB;

static NEVER_INLINE JSCell* askForCellAtEndOfStack(LoadedProgram& program, uint32_t number)
{
    volatile char frame[sizeOfDeepFrame];
    frame[0] = 0;
    JSCell* cell = stackAboveSoftLimit(program.vm()) > 2 * sizeOfDeepFrame ? askForCellAtEndOfStack(program, number) : cellsOf(program).cell(number);
    frame[1] = 0;
    return cell;
}

static void checkFirstUseAtEndOfStack(LoadedProgram& program)
{
    VM& vm = program.vm();
    size_t reservedZoneOfProgram = Options::softReservedZoneSize() + BuiltinCells::stackKeptFromProgram;
    AOT_TEST_CHECK(vm.softReservedZoneSize() == reservedZoneOfProgram);

    program.realm()->putDirectCustomAccessor(vm, Identifier::fromString(vm, "Bun"_s), CustomGetterSetter::create(vm, createHostObject, nullptr), static_cast<unsigned>(PropertyAttribute::CustomValue));
    uint32_t number = putKeyInPlaceOf(program, BuiltinCellKey::object(Builtins::ArrayBufferPrototype), BuiltinCellKey::object(Builtins::Bun));
    JSCell* cell = askForCellAtEndOfStack(program, number);
    AOT_TEST_CHECK(cell && cell->isObject() && wordOf(program, number) == cell);
    // A built-in module starts only with half of this free.
    AOT_TEST_CHECK(stackSeenByCreation >= BuiltinCells::stackKeptFromProgram / 2 && stackSeenByCreation <= BuiltinCells::stackKeptFromProgram + 2 * sizeOfDeepFrame);
    AOT_TEST_CHECK(vm.softReservedZoneSize() == reservedZoneOfProgram);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<builtinCellsTests, programText, body>;

std::span<const NativeTest> builtinCellsTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-cells/keys"_s, withProgram<checkKeys> },
        { "builtin-cells/objects"_s, withProgram<checkObjects> },
        { "builtin-cells/cells-belong-to-the-realm-of-the-program"_s, withProgram<checkCellsBelongToTheRealmOfTheProgram> },
        { "builtin-cells/native-getter"_s, withProgram<checkNativeGetter> },
        { "builtin-cells/getter-of-custom-accessor-survives-a-collection"_s, withProgram<checkGetterOfCustomAccessorSurvivesACollection> },
        { "builtin-cells/built-in-outside-the-realm"_s, withProgram<checkBuiltinOutsideTheRealm> },
        { "builtin-cells/per-module-member"_s, withProgram<checkPerModuleMember> },
        { "builtin-cells/first-use-at-end-of-stack"_s, withProgram<checkFirstUseAtEndOfStack> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
