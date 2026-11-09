/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeType.h"
#include "AOTVector.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "Symbol.h"
#include <cmath>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> runtimeTypeTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Circle" fields=[{key="radius" declared={type=number} flags=[IsRequired]}]
layout L1 name="Square" fields=[{key="side" declared={type=number} flags=[IsRequired]}]
layout L2 name="Holder" fields=[{key="red" declared={type=number} flags=[IsRequired]}, {key="green" declared={type=number} flags=[IsRequired]}, {key="\u{65e5}\u{672c}" declared={type=number} flags=[IsRequired]}, {key=symbol("tag")@m0:120 declared={type=number} flags=[IsRequired]}, {key="circle" declared={type=L0} flags=[IsRequired]}, {key="square" declared={type=L1} flags=[IsRequired]}]
layout L3 name="Light" fields=[{key="color" declared={type="red"|"green"|"\u{65e5}\u{672c}"} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = PropGet v0 key="input" [isFreeName]
    v3 = Check v2 expectedType=number
    v4 = NewObject -, v3 layout=L0 fields=[0]
    v5 = NewObject -, v3 layout=L1 fields=[0]
    v6 = NewObject -, v3, v3, v3, v3, v4, v5 layout=L2 fields=[0, 1, 2, 3, 4, 5]
    v7 = Call v1, -, v4
    v8 = Call v1, -, v5
    v9 = Call v1, -, v6
    v10 = NewClosure function=f1
    v11 = Call v1, -, v10
    v12 = NewClosure function=f2
    v13 = Call v1, -, v12
    v14 = Builtin builtin=ArrayPrototype
    v15 = Call v1, -, v14
    v16 = String string="red"
    v17 = NewObject -, v16 layout=L3 fields=[0]
    v18 = Call v1, -, v17
    Return
}
function f1 name="area" parameters=[{type=anything}] results=[{type=anything}] {
    Return v0
}
function f2 name="perimeter" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Call v0
    Return v1
}
)IR"_s;

namespace {

using Primitives = OptionSet<PrimitiveSet>;
using IntegerRange = std::pair<int64_t, int64_t>;

class TypeRecord {
public:
    TypeRecord(Primitives primitives, std::initializer_list<RuntimeCellTest> tests = { }, std::initializer_list<uint32_t> literalStrings = { }, std::optional<IntegerRange> range = std::nullopt, std::optional<uint32_t> contents = std::nullopt, std::optional<uint32_t> layoutSet = std::nullopt)
    {
        uint8_t flags = (range ? RuntimeType::hasIntegerRange : 0) | (contents ? RuntimeType::hasContents : 0) | (layoutSet ? RuntimeType::hasLayoutSet : 0);
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
        AOT_TEST_CHECK(m_words.size() * sizeof(uint32_t) == RuntimeType::sizeInBytes(tests.size(), literalStrings.size(), flags));
    }

    const RuntimeType& type() const { return *reinterpret_cast<const RuntimeType*>(m_words.span().data()); }
    operator const RuntimeType&() const { return type(); }

private:
    Vector<uint32_t, 16> m_words;
};

struct Values {
    explicit Values(LoadedProgram& program)
        : program(program)
        , layouts(program.state().objects->layouts)
    {
    }

    JSValue inProgramRealm(ASCIILiteral source) { return program.realms().evaluate(program.realm(), source); }
    JSValue inPlainRealm(ASCIILiteral source) { return program.realms().evaluate(program.realms().plain(), source); }
    bool hasType(const RuntimeType& type, JSValue value) const { return layouts.valueHasType(type, value); }

    LoadedProgram& program;
    const RuntimeLayoutTable& layouts;
};

} // anonymous namespace

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

static uint32_t identifierNumberOf(const Invocable<bool(const ImageIdentifierWord&)> auto& matches)
{
    for (uint32_t number = 1; number < imageText().numberOfIdentifiers(); ++number) {
        if (matches(imageText().identifier(number)))
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the identifier");
}

static uint32_t numberOfWellKnownSymbol(WellKnownSymbol symbol)
{
    return identifierNumberOf([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::WellKnownSymbol && record.wellKnownSymbol() == symbol;
    });
}

static const LayoutData& layoutNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && isNamedInTextOfLoadedProgram(tables.layoutForShape(shape), name))
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

static uint32_t shapeTestFor(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    uint32_t shape = layoutNamed(program, layoutName).shape;
    for (const FieldData& field : layoutNamed(program, "Holder"_s).fields()) {
        for (const RuntimeCellTest& test : tables.typeAt(field.type).cellTests()) {
            if (test.kind != CellTestKind::ShapeIn)
                continue;
            const ShapeTestData& members = tables.shapeTestAt(test.shapeTestOffset());
            if (members.first() == shape && members.count == 1)
                return test.shapeTestOffset();
        }
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static FunctionID closureFunctionNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t function = tables.closureFunctions.first; function < tables.closureFunctions.end; ++function) {
        if (std::optional<uint32_t> nameInTable = tables.nameOfFunction(function); nameInTable && imageText().equals(StringID(*nameInTable), name))
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

static void testOptionalWords()
{
    constexpr IntegerRange range { -5, 1ll << 40 };
    for (bool hasTestsAndLiterals : { false, true }) {
        for (uint8_t flags = 0; flags < 8; ++flags) {
            bool hasRange = flags & RuntimeType::hasIntegerRange;
            bool hasContents = flags & RuntimeType::hasContents;
            bool hasLayoutSet = flags & RuntimeType::hasLayoutSet;
            std::optional<IntegerRange> expectedRange = hasRange ? std::optional(range) : std::nullopt;
            std::optional<uint32_t> expectedContents = hasContents ? std::optional<uint32_t>(1234) : std::nullopt;
            std::optional<uint32_t> expectedSet = hasLayoutSet ? std::optional<uint32_t>(7) : std::nullopt;
            TypeRecord record = hasTestsAndLiterals
                ? TypeRecord({ PrimitiveSet::Integer }, { RuntimeCellTest::isCallable(), RuntimeCellTest::isSymbol(9) }, { 21, 22, 23 }, expectedRange, expectedContents, expectedSet)
                : TypeRecord({ PrimitiveSet::Integer }, { }, { }, expectedRange, expectedContents, expectedSet);
            const RuntimeType& type = record;

            AOT_TEST_CHECK(type.flags == flags && type.primitiveSet() == Primitives { PrimitiveSet::Integer });
            AOT_TEST_CHECK(type.integerRange() == expectedRange);
            AOT_TEST_CHECK(type.contents() == expectedContents);
            AOT_TEST_CHECK(type.layoutSet() == expectedSet);
            AOT_TEST_CHECK(type.cellTests().size() == (hasTestsAndLiterals ? 2u : 0u) && type.literalStrings().size() == (hasTestsAndLiterals ? 3u : 0u));
            if (hasTestsAndLiterals) {
                AOT_TEST_CHECK(type.cellTests()[0].kind == CellTestKind::IsCallable);
                AOT_TEST_CHECK(type.cellTests()[1].kind == CellTestKind::IsSymbol && type.cellTests()[1].symbolKeyNumber() == 9);
                AOT_TEST_CHECK(type.literalStrings()[0] == 21 && type.literalStrings()[2] == 23);
            }
        }
    }
}

static void testEmptyValueAndOtherCellsHaveNoType(LoadedProgram& program)
{
    Values values(program);
    TypeRecord everything(allPrimitives | Primitives { PrimitiveSet::Absent, PrimitiveSet::LiteralString, PrimitiveSet::LiteralSymbol }, { RuntimeCellTest::isObject(), RuntimeCellTest::isCallable() });
    AOT_TEST_CHECK(values.hasType(everything, jsNumber(1)) && values.hasType(everything, values.inPlainRealm("({})"_s)));
    AOT_TEST_CHECK(!values.hasType(everything, JSValue()));
    AOT_TEST_CHECK(!values.hasType(everything, program.vm().structureStructure.get()));
    AOT_TEST_CHECK(!values.hasType(everything, program.realm()->objectPrototype()->structure()));

    TypeRecord nothing(Primitives { });
    for (JSValue value : { jsUndefined(), jsNull(), jsBoolean(true), jsNumber(0), values.inPlainRealm("''"_s), values.inPlainRealm("({})"_s), JSValue() })
        AOT_TEST_CHECK(!values.hasType(nothing, value));
}

static void testLiteralStrings(LoadedProgram& program)
{
    VM& vm = program.vm();
    Values values(program);
    static constexpr char16_t japaneseCharacters[] = { 0x65e5, 0x672c };
    String japanese { std::span(japaneseCharacters) };
    uint32_t red = findText("red"_s);
    uint32_t green = findText("green"_s);
    uint32_t wide = findText(japanese);

    JSString* rope = jsString(program.realm(), jsString(vm, String("gre"_s)), jsString(vm, String("en"_s)));
    JSString* otherRope = jsString(program.realm(), jsString(vm, String("gre"_s)), jsString(vm, String("ys"_s)));
    JSString* base = jsString(vm, String("a red or blue light"_s));
    JSString* substring = jsSubstring(program.realm(), base, 2, 3);
    JSString* otherSubstring = jsSubstring(program.realm(), base, 9, 4);
    AOT_TEST_CHECK(rope->isRope() && otherRope->isRope() && substring->isSubstring() && otherSubstring->isSubstring());

    for (PrimitiveSet bit : { PrimitiveSet::String, PrimitiveSet::LiteralString }) {
        TypeRecord type({ bit }, { }, { red, green, wide });
        for (JSValue listed : { JSValue(jsString(vm, String("red"_s))), JSValue(jsString(vm, String("green"_s))), JSValue(jsString(vm, japanese)), JSValue(rope), JSValue(substring) })
            AOT_TEST_CHECK(values.hasType(type, listed));
        for (JSValue unlisted : { JSValue(jsString(vm, String("blue"_s))), JSValue(jsString(vm, String("re"_s))), JSValue(jsString(vm, String("redd"_s))), JSValue(jsEmptyString(vm)), JSValue(jsString(vm, japanese.left(1))), JSValue(otherRope), JSValue(otherSubstring) })
            AOT_TEST_CHECK(!values.hasType(type, unlisted));
        AOT_TEST_CHECK(!values.hasType(type, jsNumber(1)) && !values.hasType(type, jsUndefined()));
    }
    AOT_TEST_CHECK(rope->isRope() && otherRope->isRope());

    TypeRecord listWithoutBit({ PrimitiveSet::Integer }, { }, { red });
    AOT_TEST_CHECK(!values.hasType(listWithoutBit, jsString(vm, String("red"_s))));

    TypeRecord everyString(Primitives { PrimitiveSet::String });
    TypeRecord noString(Primitives { PrimitiveSet::LiteralString });
    for (JSValue string : { JSValue(jsString(vm, String("red"_s))), JSValue(jsString(vm, String("blue"_s))), JSValue(jsEmptyString(vm)), JSValue(otherRope) }) {
        AOT_TEST_CHECK(values.hasType(everyString, string));
        AOT_TEST_CHECK(!values.hasType(noString, string));
    }
}

static void testShapeIn(LoadedProgram& program)
{
    Values values(program);
    JSObject* circle = allocateObject(program, "Circle"_s);
    JSObject* square = allocateObject(program, "Square"_s);
    TypeRecord circles({ }, { RuntimeCellTest::shapeIn(shapeTestFor(program, "Circle"_s)) });
    TypeRecord squares({ }, { RuntimeCellTest::shapeIn(shapeTestFor(program, "Square"_s)) });
    AOT_TEST_CHECK(values.hasType(circles, circle) && !values.hasType(circles, square));
    AOT_TEST_CHECK(values.hasType(squares, square) && !values.hasType(squares, circle));
    for (JSValue other : { values.inPlainRealm("({ radius: 1 })"_s), values.inProgramRealm("({ radius: 1 })"_s), values.inPlainRealm("[]"_s), values.inPlainRealm("'Circle'"_s) })
        AOT_TEST_CHECK(!values.hasType(circles, other));

    TypeRecord either({ }, { RuntimeCellTest::shapeIn(shapeTestFor(program, "Circle"_s)), RuntimeCellTest::shapeIn(shapeTestFor(program, "Square"_s)) });
    AOT_TEST_CHECK(values.hasType(either, circle) && values.hasType(either, square) && !values.hasType(either, allocateObject(program, "Holder"_s)));
}

static void testInheritsClassInfo(LoadedProgram& program)
{
    Values values(program);
    TypeRecord weakReferences({ }, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::WeakRef) });
    for (JSValue reference : { values.inPlainRealm("new WeakRef({})"_s), values.inProgramRealm("new WeakRef({})"_s), values.inPlainRealm("new (class extends WeakRef { })({})"_s), values.inPlainRealm("Object.setPrototypeOf(new WeakRef({}), null)"_s) })
        AOT_TEST_CHECK(values.hasType(weakReferences, reference));
    for (JSValue other : { values.inPlainRealm("new WeakMap"_s), values.inPlainRealm("new FinalizationRegistry(() => { })"_s), values.inPlainRealm("Object.create(WeakRef.prototype)"_s), values.inPlainRealm("WeakRef"_s), values.inPlainRealm("({})"_s) })
        AOT_TEST_CHECK(!values.hasType(weakReferences, other));

    TypeRecord registries({ }, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::FinalizationRegistry) });
    AOT_TEST_CHECK(values.hasType(registries, values.inPlainRealm("new FinalizationRegistry(() => { })"_s)) && !values.hasType(registries, values.inPlainRealm("new WeakRef({})"_s)));
}

static void testIsPristine(LoadedProgram& program)
{
    Values values(program);
    TypeRecord pristineArrays({ }, { RuntimeCellTest::isPristine(BuiltinClasses::Array) });
    TypeRecord arrays({ }, { RuntimeCellTest::cellTypeIs(ArrayType, DerivedArrayType) });
    TypeRecord pristineMaps({ }, { RuntimeCellTest::isPristine(BuiltinClasses::Map) });

    JSValue pristine = values.inProgramRealm("[1, 2]"_s);
    AOT_TEST_CHECK(pristine.asCell()->isPristine());
    AOT_TEST_CHECK(values.hasType(pristineArrays, pristine) && values.hasType(arrays, pristine));
    AOT_TEST_CHECK(!values.hasType(pristineMaps, pristine));
    AOT_TEST_CHECK(values.hasType(pristineMaps, values.inProgramRealm("new Map"_s)) && !values.hasType(pristineArrays, values.inProgramRealm("new Map"_s)));

    for (JSValue changed : { values.inProgramRealm("(a => (a.constructor = 1, a))([1, 2])"_s), values.inProgramRealm("Object.setPrototypeOf([1, 2], null)"_s), values.inPlainRealm("[1, 2]"_s) }) {
        AOT_TEST_CHECK(!changed.asCell()->isPristine());
        AOT_TEST_CHECK(!values.hasType(pristineArrays, changed) && values.hasType(arrays, changed));
    }
}

static void testIsBuiltinCell(LoadedProgram& program)
{
    Values values(program);
    TypeRecord arrayPrototype({ }, { RuntimeCellTest::isBuiltinCell(builtinCellNumberOf(program, Builtins::ArrayPrototype)) });
    AOT_TEST_CHECK(values.hasType(arrayPrototype, values.inProgramRealm("Array.prototype"_s)));
    for (JSValue other : { values.inPlainRealm("Array.prototype"_s), values.inProgramRealm("Object.prototype"_s), values.inProgramRealm("[]"_s), values.inProgramRealm("Array"_s) })
        AOT_TEST_CHECK(!values.hasType(arrayPrototype, other));
}

static void testIsSymbol(LoadedProgram& program)
{
    Values values(program);
    TypeRecord iterator({ }, { RuntimeCellTest::isSymbol(numberOfWellKnownSymbol(WellKnownSymbol::Iterator)) });
    AOT_TEST_CHECK(values.hasType(iterator, values.inProgramRealm("Symbol.iterator"_s)) && values.hasType(iterator, values.inPlainRealm("Symbol.iterator"_s)));
    for (JSValue other : { values.inPlainRealm("Symbol.hasInstance"_s), values.inPlainRealm("Symbol('Symbol.iterator')"_s), values.inPlainRealm("Symbol.for('iterator')"_s), values.inPlainRealm("'Symbol.iterator'"_s), values.inPlainRealm("Object(Symbol.iterator)"_s) })
        AOT_TEST_CHECK(!values.hasType(iterator, other));

    TypeRecord everySymbol({ PrimitiveSet::Symbol }, { RuntimeCellTest::isSymbol(numberOfWellKnownSymbol(WellKnownSymbol::Iterator)) });
    AOT_TEST_CHECK(values.hasType(everySymbol, values.inPlainRealm("Symbol.hasInstance"_s)) && values.hasType(everySymbol, values.inPlainRealm("Symbol.iterator"_s)));
}

static void testAnyCellTestMayPass(LoadedProgram& program)
{
    Values values(program);
    TypeRecord type({ PrimitiveSet::Null }, { RuntimeCellTest::inheritsClassInfo(BuiltinClasses::WeakRef), RuntimeCellTest::isCallable(), RuntimeCellTest::shapeIn(shapeTestFor(program, "Square"_s)) });
    for (JSValue accepted : { values.inPlainRealm("new WeakRef({})"_s), values.inPlainRealm("(() => 1)"_s), JSValue(allocateObject(program, "Square"_s)), jsNull() })
        AOT_TEST_CHECK(values.hasType(type, accepted));
    for (JSValue refused : { values.inPlainRealm("new Set"_s), values.inPlainRealm("({})"_s), JSValue(allocateObject(program, "Circle"_s)), jsUndefined() })
        AOT_TEST_CHECK(!values.hasType(type, refused));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<runtimeTypeTests, programText, body>;

static constexpr ASCIILiteral noSlotIsHeldToType = "plan/AOTSlotPlan.cpp: typeHeldOfFieldForSlotPlan() holds no slot to a type, so the image has no record of a type with shapes"_s;

std::span<const NativeTest> runtimeTypeTests()
{
    static constexpr NativeTest tests[] = {
        { "runtime-types/optional-words"_s, testOptionalWords },
        { "runtime-types/empty-value-and-other-cells-have-no-type"_s, withProgram<testEmptyValueAndOtherCellsHaveNoType> },
        { "runtime-types/literal-strings"_s, withProgram<testLiteralStrings> },
        { "runtime-types/shape-in"_s, withProgram<testShapeIn>, noSlotIsHeldToType },
        { "runtime-types/inherits-class-info"_s, withProgram<testInheritsClassInfo> },
        { "runtime-types/is-pristine"_s, withProgram<testIsPristine> },
        { "runtime-types/is-builtin-cell"_s, withProgram<testIsBuiltinCell> },
        { "runtime-types/is-symbol"_s, withProgram<testIsSymbol> },
        { "runtime-types/any-cell-test-may-pass"_s, withProgram<testAnyCellTestMayPass>, noSlotIsHeldToType },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
