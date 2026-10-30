/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTChildProcess.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramFile.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeType.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTErrorMessages.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "Options.h"
#include <memory>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> typeErrorMessageTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Circle" fields=[{key="radius" declared={type=number} flags=[IsRequired]}]
layout L1 name="Square" fields=[{key="side" declared={type=number} flags=[IsRequired]}]
layout L2 name="Holder" fields=[{key="red" declared={type=number} flags=[IsRequired]}, {key="green" declared={type=string} flags=[IsRequired]}, {key=symbol("tag")@m0:120 declared={type=anything} flags=[IsRequired]}, {key="circle" declared={type=L0} flags=[IsRequired]}, {key="square" declared={type=L1} flags=[IsRequired]}, {key="either" declared={type=L0|L1} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
typesite w0 module=m0 position=200 kind=Assertion text="written behind as"
typesite w1 module=m0 position=210 kind=Parameter text="written for a parameter"
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = PropGet v0 key="input" [isFreeName]
    v3 = Check v2 expectedType=number
    v4 = Check v2 expectedType=string
    v5 = NewObject -, v3 layout=L0 fields=[0]
    v6 = NewObject -, v3 layout=L1 fields=[0]
    v7 = NewObject -, v3, v4, v2, v5, v6, v5 layout=L2 fields=[0, 1, 2, 3, 4, 5]
    v8 = NewObject -, v3, v4, v2, v5, v6, v6 layout=L2 fields=[0, 1, 2, 3, 4, 5]
    v9 = Call v1, -, v5
    v10 = Call v1, -, v6
    v11 = Call v1, -, v7
    v12 = Call v1, -, v8
    v13 = NewClosure function=f1
    v14 = Call v1, -, v13
    v15 = NewClosure function=f2
    v16 = Call v1, -, v15
    v17 = Builtin builtin=ArrayPrototype
    v18 = Call v1, -, v17
    v19 = Builtin builtin=ObjectPrototype
    v20 = Call v1, -, v19
    v21 = NewClosure function=f3
    v22 = Call v1, -, v21
    Return
}
function f1 name="area" parameters=[{type=anything}] results=[{type=anything}] {
    Return v0
}
function f2 name="perimeter" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Call v0
    Return v1
}
function f3 name="checks" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Check v0 expectedType=string|Array site=w0
    v3 = Check v1 expectedType=string|Array site=w1
    v4 = Add v2, v3
    Return v4
}
)IR"_s;

namespace {

using Primitives = OptionSet<PrimitiveSet>;
using IntegerRange = std::pair<int64_t, int64_t>;

class TypeRecord {
public:
    explicit TypeRecord(Primitives primitives, std::initializer_list<RuntimeCellTest> tests = { }, std::initializer_list<uint32_t> literalStrings = { }, std::optional<IntegerRange> range = std::nullopt, std::optional<uint32_t> contents = std::nullopt, std::optional<uint32_t> layoutSet = std::nullopt)
    {
        uint8_t flags = (range ? RuntimeType::hasIntegerRange : 0) | (contents ? RuntimeType::hasContents : 0) | (layoutSet ? RuntimeType::hasLayoutSet : 0);
        // As the encoder of the tables writes them: the tests of one function each stand behind all others, by rising number.
        Vector<RuntimeCellTest> ordered(tests);
        std::ranges::stable_sort(ordered, { }, [](const RuntimeCellTest& test) -> int64_t {
            return test.kind == CellTestKind::IsClosureOf ? static_cast<int64_t>(test.operand) : -1;
        });
        size_t numberInFront = std::ranges::count_if(ordered, [](const RuntimeCellTest& test) { return test.kind != CellTestKind::IsClosureOf; });
        m_words.append(primitives.toRaw() | static_cast<uint32_t>(flags) << 16 | static_cast<uint32_t>(std::min<size_t>(numberInFront, RuntimeType::hasManyTestsOfMoreThanOneFunction)) << 24);
        m_words.append(static_cast<uint32_t>(tests.size()) | static_cast<uint32_t>(literalStrings.size()) << 16);
        for (const RuntimeCellTest& test : ordered) {
            auto words = std::bit_cast<std::array<uint32_t, 2>>(test);
            m_words.append(words[0]);
            m_words.append(words[1]);
        }
        for (uint32_t string : literalStrings)
            m_words.append(string);
        if (range) {
            for (int64_t bound : { range->first, range->second }) {
                m_words.append(static_cast<uint32_t>(bound));
                m_words.append(static_cast<uint32_t>(static_cast<uint64_t>(bound) >> 32));
            }
        }
        if (contents)
            m_words.append(*contents);
        if (layoutSet)
            m_words.append(*layoutSet);
    }

    operator const RuntimeType&() const { return *reinterpret_cast<const RuntimeType*>(m_words.span().data()); }

private:
    Vector<uint32_t, 16> m_words;
};

} // anonymous namespace

static constexpr Primitives noPrimitives { };
static constexpr Primitives everyNumber { PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber };
static constexpr Primitives allPrimitives { PrimitiveSet::Undefined, PrimitiveSet::Null, PrimitiveSet::False, PrimitiveSet::True, PrimitiveSet::Integer, PrimitiveSet::WholeNumber, PrimitiveSet::FractionalNumber, PrimitiveSet::String, PrimitiveSet::Symbol, PrimitiveSet::BigInt };

static const ImageText& imageText()
{
    return Image::current()->text();
}

static uint32_t findText(StringView wanted)
{
    for (uint32_t index = 0; index < imageText().numberOfStrings(); ++index) {
        if (imageText().hasText(StringID(index)) && imageText().equals(StringID(index), wanted))
            return index;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the text");
}

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && imageText().equals(StringID(tables.layoutForShape(shape).name), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static JSObject* allocateObject(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape == shape)
            return asObject(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static uint32_t layoutSetWith(LoadedProgram& program, uint32_t firstShape, uint32_t count)
{
    std::span<const LayoutSetData> sets = program.state().tables.layoutSets();
    for (uint32_t index = 0; index < sets.size(); ++index) {
        if (sets[index].members.first() == firstShape && sets[index].members.count == count)
            return index;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the layout set");
}

static uint32_t layoutSetOf(LoadedProgram& program, ASCIILiteral layoutName)
{
    return layoutSetWith(program, layoutNamed(program, layoutName).shape, 1);
}

static uint32_t layoutSetOfBoth(LoadedProgram& program)
{
    uint32_t circle = layoutNamed(program, "Circle"_s).shape;
    AOT_TEST_CHECK(layoutNamed(program, "Square"_s).shape == circle + 1);
    return layoutSetWith(program, circle, 2);
}

static uint32_t shapeTestOf(LoadedProgram& program, uint32_t layoutSet)
{
    const ProgramTables& tables = program.state().tables;
    return tables.offsetOf(&tables.layoutSets()[layoutSet].members);
}

static uint32_t typeOfField(LoadedProgram& program, ASCIILiteral layoutName, unsigned field)
{
    return layoutNamed(program, layoutName).fields()[field].type;
}

static FunctionID closureFunctionNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t function = tables.closureFunctions.first; function < tables.closureFunctions.end; ++function) {
        if (imageText().equals(StringID(tables.nameOfFunction(function)), name))
            return FunctionID(function);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static uint32_t builtinCellNumberOf(LoadedProgram& program, BuiltinID builtin)
{
    std::span<const uint32_t> keys = program.state().tables.builtinCells();
    for (uint32_t number = 0; number < keys.size(); ++number) {
        if (keys[number] == BuiltinCellKey::object(builtin).encoded())
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the built-in cell");
}

static uint32_t identifierNumberOf(const Invocable<bool(const ImageIdentifierWord&)> auto& matches)
{
    for (uint32_t number = 1; number < imageText().numberOfIdentifiers(); ++number) {
        if (matches(imageText().identifier(number)))
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the identifier");
}

static bool isDescribedAs(LoadedProgram& program, const RuntimeType& type, StringView expected)
{
    String description = describe(program.state().tables, type);
    if (description == expected)
        return true;
    dataLogLn("described as ", description, ", expected ", expected);
    return false;
}

static bool hasMessage(LoadedProgram& program, JSValue value, const RuntimeType& type, StringView expected)
{
    String message = makeString("expected "_s, describe(program.state().tables, type), ", got "_s, describe(value));
    if (message == expected)
        return true;
    dataLogLn("the message is ", message, ", expected ", expected);
    return false;
}

static JSValue evaluate(LoadedProgram& program, ASCIILiteral source)
{
    return program.realms().evaluate(program.realms().plain(), source);
}

static void testIsObject(LoadedProgram& program)
{
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(noPrimitives, { RuntimeCellTest::isObject() }), "object"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }, { RuntimeCellTest::isObject() }), "object | string"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(allPrimitives, { RuntimeCellTest::isObject() }), "any"_s));
    AOT_TEST_CHECK(hasMessage(program, jsNumber(3), TypeRecord(noPrimitives, { RuntimeCellTest::isObject() }), "expected object, got number"_s));
}

static void testIsObjectOfListedLayouts(LoadedProgram& program)
{
    TypeRecord circle(noPrimitives, { RuntimeCellTest::isObject() }, { }, std::nullopt, std::nullopt, layoutSetOf(program, "Circle"_s));
    TypeRecord either(noPrimitives, { RuntimeCellTest::isObject() }, { }, std::nullopt, std::nullopt, layoutSetOfBoth(program));
    AOT_TEST_CHECK(isDescribedAs(program, circle, "Circle"_s));
    AOT_TEST_CHECK(isDescribedAs(program, either, "Circle | Square"_s));
    AOT_TEST_CHECK(hasMessage(program, jsDoubleNumber(1.5), circle, "expected Circle, got number"_s));
    AOT_TEST_CHECK(hasMessage(program, jsBoolean(true), either, "expected Circle | Square, got boolean"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "Symbol()"_s), either, "expected Circle | Square, got symbol"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "1n << 70n"_s), either, "expected Circle | Square, got bigint"_s));
}

static void testIsCallable(LoadedProgram& program)
{
    TypeRecord type(noPrimitives, { RuntimeCellTest::isCallable() });
    AOT_TEST_CHECK(isDescribedAs(program, type, "Function"_s));
    AOT_TEST_CHECK(hasMessage(program, jsBoolean(true), type, "expected Function, got boolean"_s));
    AOT_TEST_CHECK(hasMessage(program, jsDoubleNumber(1.5), type, "expected Function, got number"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(noPrimitives, { RuntimeCellTest::isObject(), RuntimeCellTest::isCallable() }), "object"_s));
}

static void testCellTypeIs(LoadedProgram& program)
{
    TypeRecord maps(noPrimitives, { RuntimeCellTest::cellTypeIs(JSMapType, JSMapType) });
    AOT_TEST_CHECK(isDescribedAs(program, maps, "Map"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "[]"_s), maps, "expected Map, got Array"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Set"_s), maps, "expected Map, got built-in Set"_s));

    uint32_t number = typeOfField(program, "Holder"_s, 0);
    uint32_t string = typeOfField(program, "Holder"_s, 1);
    uint32_t anything = typeOfField(program, "Holder"_s, 2);
    uint32_t circle = typeOfField(program, "Holder"_s, 3);
    auto arraysOf = [](uint32_t contents) {
        return TypeRecord(noPrimitives, { RuntimeCellTest::cellTypeIs(ArrayType, DerivedArrayType) }, { }, std::nullopt, contents);
    };
    AOT_TEST_CHECK(isDescribedAs(program, arraysOf(number), "number[]"_s));
    AOT_TEST_CHECK(isDescribedAs(program, arraysOf(string), "string[]"_s));
    AOT_TEST_CHECK(isDescribedAs(program, arraysOf(anything), "any[]"_s));
    AOT_TEST_CHECK(isDescribedAs(program, arraysOf(circle), "Circle[]"_s));
    AOT_TEST_CHECK(hasMessage(program, jsNumber(4), arraysOf(string), "expected string[], got number"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Map"_s), arraysOf(string), "expected string[], got built-in Map"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Uint8Array(1)"_s), arraysOf(anything), "expected any[], got built-in Uint8Array"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "({ length: 0 })"_s), arraysOf(anything), "expected any[], got untyped object"_s));
}

static void testShapeIn(LoadedProgram& program)
{
    TypeRecord circle(noPrimitives, { RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOf(program, "Circle"_s))) });
    TypeRecord square(noPrimitives, { RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOf(program, "Square"_s))) });
    TypeRecord either(noPrimitives, { RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOfBoth(program))) });
    TypeRecord twoTests(noPrimitives, { RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOf(program, "Circle"_s))), RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOf(program, "Square"_s))) });
    AOT_TEST_CHECK(isDescribedAs(program, circle, "Circle"_s));
    AOT_TEST_CHECK(isDescribedAs(program, square, "Square"_s));
    AOT_TEST_CHECK(isDescribedAs(program, either, "Circle | Square"_s));
    AOT_TEST_CHECK(isDescribedAs(program, twoTests, "Circle | Square"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::Undefined }, { RuntimeCellTest::shapeIn(shapeTestOf(program, layoutSetOf(program, "Circle"_s))) }), "Circle | undefined"_s));
    AOT_TEST_CHECK(hasMessage(program, jsDoubleNumber(1.5), circle, "expected Circle, got number"_s));
    AOT_TEST_CHECK(hasMessage(program, allocateObject(program, "Square"_s), circle, "expected Circle, got Square"_s));
}

static void testInheritsClassInfo(LoadedProgram& program)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    TypeRecord maps(noPrimitives, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::Map) });
    TypeRecord sets(noPrimitives, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::Set) });
    AOT_TEST_CHECK(isDescribedAs(program, maps, "Map"_s) && isDescribedAs(program, sets, "Set"_s));
    AOT_TEST_CHECK(isDescribedAs(program, maps, table.textOf(table.at(BuiltinClasses::Map).constructor)));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "[]"_s), maps, "expected Map, got Array"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "({})"_s), maps, "expected Map, got untyped object"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "(function () { })"_s), maps, "expected Map, got function"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "(function list() { })"_s), maps, "expected Map, got function list"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Date(0)"_s), maps, "expected Map, got built-in Date"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(noPrimitives, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::Map), RuntimeCellTest::inheritsClassInfo(BuiltinClasses::Set) }), "Map | Set"_s));

    TypeRecord weakReferences(noPrimitives, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::WeakRef) });
    AOT_TEST_CHECK(isDescribedAs(program, weakReferences, "WeakRef"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new WeakMap"_s), weakReferences, "expected WeakRef, got built-in WeakMap"_s));
}

static void testIsPristine(LoadedProgram& program)
{
    TypeRecord maps(noPrimitives, { RuntimeCellTest::isPristine(BuiltinClasses::Map) });
    AOT_TEST_CHECK(isDescribedAs(program, maps, "Map"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Set"_s), maps, "expected Map, got built-in Set"_s));

    TypeRecord arrays(noPrimitives, { RuntimeCellTest::isPristine(BuiltinClasses::Array) }, { }, std::nullopt, typeOfField(program, "Holder"_s, 1));
    AOT_TEST_CHECK(isDescribedAs(program, arrays, "string[]"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(noPrimitives, { RuntimeCellTest::isPristine(BuiltinClasses::Map), RuntimeCellTest::inheritsClassInfo(BuiltinClasses::Map) }), "Map"_s));
}

static void testIsBuiltinCell(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    const BuiltinTable& table = BuiltinTable::singleton();
    String arrayPrototype = describe(tables, TypeRecord(noPrimitives, { RuntimeCellTest::isBuiltinCell(builtinCellNumberOf(program, Builtins::ArrayPrototype)) }));
    String objectPrototype = describe(tables, TypeRecord(noPrimitives, { RuntimeCellTest::isBuiltinCell(builtinCellNumberOf(program, Builtins::ObjectPrototype)) }));
    AOT_TEST_CHECK(arrayPrototype.contains(table.textOf(Builtins::ArrayPrototype)));
    AOT_TEST_CHECK(objectPrototype.contains(table.textOf(Builtins::ObjectPrototype)));
    AOT_TEST_CHECK(arrayPrototype != objectPrototype && !arrayPrototype.contains(table.textOf(Builtins::ObjectPrototype)));
}

static void testIsClosureOf(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    String area = describe(tables, TypeRecord(noPrimitives, { RuntimeCellTest::isClosureOf(closureNumberOf(closureFunctionNamed(program, "area"_s))) }));
    String perimeter = describe(tables, TypeRecord(noPrimitives, { RuntimeCellTest::isClosureOf(closureNumberOf(closureFunctionNamed(program, "perimeter"_s))) }));
    AOT_TEST_CHECK(area.contains("area"_s) && !area.contains("perimeter"_s));
    AOT_TEST_CHECK(perimeter.contains("perimeter"_s) && !perimeter.contains("area"_s));

    JSCell* closure = program.state().objects->closures.closure(program.vm(), closureFunctionNamed(program, "area"_s));
    AOT_TEST_CHECK(describe(JSValue(closure)) == "function area"_s);
    AOT_TEST_CHECK(hasMessage(program, closure, TypeRecord(everyNumber), "expected number, got function area"_s));
}

static void testIsSymbol(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t tag = identifierNumberOf([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::CreatedSymbol && record.text() && imageText().equals(record.text(), "tag"_s);
    });
    String description = describe(tables, TypeRecord(noPrimitives, { RuntimeCellTest::isSymbol(tag) }));
    AOT_TEST_CHECK(description.contains("tag"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::Symbol }), "symbol"_s));
    AOT_TEST_CHECK(!program.vm().aotState().identifiers->tryGetIdentifierConcurrently(tag));
}

static void testPrimitives(LoadedProgram& program)
{
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(everyNumber), "number"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }), "string"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::True, PrimitiveSet::False }), "boolean"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord(everyNumber | Primitives { PrimitiveSet::Undefined }), "number | undefined"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "'wrong'"_s), TypeRecord(everyNumber | Primitives { PrimitiveSet::Undefined }), "expected number | undefined, got string"_s));
    AOT_TEST_CHECK(hasMessage(program, jsNumber(7), TypeRecord({ PrimitiveSet::String }), "expected string, got number"_s));
    AOT_TEST_CHECK(hasMessage(program, jsNumber(5), TypeRecord({ PrimitiveSet::True, PrimitiveSet::False }), "expected boolean, got number"_s));
    AOT_TEST_CHECK(hasMessage(program, jsString(program.vm(), String("abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"_s)), TypeRecord(everyNumber), "expected number, got string"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new (class Legacy { })"_s), TypeRecord(everyNumber), "expected number, got untyped Legacy"_s));
}

static void testLiteralStrings(LoadedProgram& program)
{
    uint32_t red = findText("red"_s);
    uint32_t green = findText("green"_s);
    uint32_t circle = findText("circle"_s);
    uint32_t square = findText("square"_s);
    uint32_t either = findText("either"_s);
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }, { }, { red }), "\"red\""_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }, { }, { red, green }), "\"red\" | \"green\""_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }, { }, { red, green, circle, square }), "\"red\" | \"green\" | \"circle\" | \"square\""_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord({ PrimitiveSet::String }, { }, { red, green, circle, square, either }), "\"red\" | \"green\" | \"circle\" | \"square\" | ..."_s));
    AOT_TEST_CHECK(hasMessage(program, jsDoubleNumber(1.5), TypeRecord({ PrimitiveSet::String }, { }, { red, green, circle, square, either }), "expected \"red\" | \"green\" | \"circle\" | \"square\" | ..., got number"_s));
}

static Typed::TypeSiteID typeSiteOfKind(LoadedProgram& program, TypeSiteKind kind)
{
    for (const TypeSiteData& site : program.state().tables.typeSites()) {
        if (static_cast<TypeSiteKind>(site.kind) == kind && site.text != TypeSiteData::noText)
            return Typed::TypeSiteID(site.site);
    }
    failNativeTest(__FILE__, __LINE__, "the image has the type site with its text");
}

static void testTextOfSite(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    Typed::TypeSiteID assertion = typeSiteOfKind(program, TypeSiteKind::Assertion);
    Typed::TypeSiteID parameter = typeSiteOfKind(program, TypeSiteKind::Parameter);
    TypeRecord stringsAndArrays({ PrimitiveSet::String }, { RuntimeCellTest::cellTypeIs(ArrayType, DerivedArrayType) });
    AOT_TEST_CHECK(describe(tables, stringsAndArrays, { }) == "any[] | string"_s);
    AOT_TEST_CHECK(describe(tables, stringsAndArrays, parameter) == "any[] | string"_s);
    AOT_TEST_CHECK(describe(tables, stringsAndArrays, assertion) == "written behind as"_s);
    AOT_TEST_CHECK(describe(tables, TypeRecord({ PrimitiveSet::String }), assertion) == "written behind as"_s);
    AOT_TEST_CHECK(describe(tables, TypeRecord({ PrimitiveSet::String }, { RuntimeCellTest::isObject() }), parameter) == "written for a parameter"_s);
}

static Vector<uint8_t> compileReportingMismatches()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.executableIdentity = { };
        options.mismatchPolicy = Wire::MismatchPolicy::Report;
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        if (!program) {
            build.diagnostics().finish(nullptr);
            Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
        }
        AOT_TEST_CHECK(program);

        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static constexpr ASCIILiteral textOfSiteTest = "type-error-messages/text-of-site"_s;

// A check that fails goes on in the generic version and says nothing, so its site is in the image only where the build reports mismatches.
static void testTextOfSiteWhereMismatchesAreReported()
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(textOfSiteTest, programText, testTextOfSite);
        return;
    }

    TemporaryFile image(".image"_s, compileReportingMismatches().span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(textOfSiteTest), makeString("--aotImagePath="_s, image.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image ", result.description());
        failNativeTest(__FILE__, __LINE__, textOfSiteTest.characters());
    }
}

// Only a build that logs type errors puts a type into words, so only its image says what a container holds and names the sets of layouts.
template<LoadedProgramTest body>
static void withProgram()
{
    for (const NativeTest& test : typeErrorMessageTests()) {
        if (test.run == &withProgram<body>) {
            runWithLoadedProgram(test.name, programText, body, { "type-errors=log"_s });
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> typeErrorMessageTests()
{
    static constexpr NativeTest tests[] = {
        { "type-error-messages/is-object"_s, withProgram<testIsObject> },
        { "type-error-messages/object-of-listed-layouts"_s, withProgram<testIsObjectOfListedLayouts> },
        { "type-error-messages/is-callable"_s, withProgram<testIsCallable> },
        { "type-error-messages/cell-type-is"_s, withProgram<testCellTypeIs> },
        { "type-error-messages/shape-in"_s, withProgram<testShapeIn> },
        { "type-error-messages/inherits-class-info"_s, withProgram<testInheritsClassInfo> },
        { "type-error-messages/is-pristine"_s, withProgram<testIsPristine> },
        { "type-error-messages/is-builtin-cell"_s, withProgram<testIsBuiltinCell> },
        { "type-error-messages/is-closure-of"_s, withProgram<testIsClosureOf> },
        { "type-error-messages/is-symbol"_s, withProgram<testIsSymbol> },
        { "type-error-messages/primitives"_s, withProgram<testPrimitives> },
        { "type-error-messages/literal-strings"_s, withProgram<testLiteralStrings> },
        { textOfSiteTest, testTextOfSiteWhereMismatchesAreReported },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
