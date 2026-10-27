/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIR.h"
#include "AOTIRPrinter.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTReaderFixture.h"
#include "AOTVector.h"
#include <array>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <wtf/DataLog.h>
#include <wtf/Function.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

namespace {

using namespace Typed;

constexpr FunctionID f0 { 0u };
constexpr FunctionID f1 { 1u };
constexpr FunctionID f3 { 3u };
constexpr FunctionID f7 = readerFixtureStreamFunction;
constexpr LayoutID L0 { 0u };
constexpr LayoutID L1 { 1u };
constexpr LayoutID L2 { 2u };
constexpr ModuleID m0 { 0u };
constexpr BlockID b0 { 0u };
constexpr ValueID v2 { 2u };
constexpr ValueID v4 { 4u };
constexpr ValueID v6 { 6u };
constexpr ValueID v7 { 7u };
constexpr ValueID v9 { 9u };
constexpr ValueID v16 { 16u };

constexpr uint32_t idPastEveryTable = 1000000;

void expectText(const String& printed, StringView expected)
{
    if (printed != expected)
        dataLogLn("printed:  ", printed, "\nexpected: ", expected);
    AOT_TEST_CHECK(printed == expected);
}

void expectPart(const String& printed, StringView expectedPart)
{
    if (!printed.contains(expectedPart))
        dataLogLn("printed:  ", printed, "\nexpected to contain: ", expectedPart);
    AOT_TEST_CHECK(printed.contains(expectedPart));
}

PropertyKey createKey(PropertyKeyKind kind, StringID text = { }, LayoutID owner = { }, std::optional<WellKnownSymbol> symbol = { })
{
    return { kind, text, owner, symbol, { }, { }, { } };
}

PropertyKey createSymbolKey(StringID description, ModuleID module, std::optional<uint32_t> callNumber, AllocationSiteID site = { })
{
    return { PropertyKeyKind::CreatedSymbol, description, { }, { }, module, callNumber, site };
}

struct PrinterTest {
    PrinterTest()
        : program(fixture.read())
        , types(program->types)
    {
    }

    template<typename T>
    String print(const T& value, PrintedFields fields = PrintedFields::All) const
    {
        StringPrintStream out;
        IRPrinter(out, *program, fields).print(value);
        return out.toString();
    }

    String printType(const Type& type, PrintedFields fields = PrintedFields::All) const
    {
        StringPrintStream out;
        IRPrinter(out, *program, fields).printType(type);
        return out.toString();
    }

    String printProgram(PrintedFields fields) const
    {
        StringPrintStream out;
        Typed::printProgram(out, *program, fields);
        return out.toString();
    }

    String printChangedCopy(FunctionID id, NOESCAPE const WTF::Function<void(Typed::Function&)>& change) const
    {
        Typed::Function copy = program->at(id);
        change(copy);
        StringPrintStream out;
        printFunction(out, *program, id, copy);
        return out.toString();
    }

    StringID string(ASCIILiteral text) { return program->strings.intern(StringView(text)); }

    Type unionOf(std::initializer_list<Type> parts)
    {
        Type result;
        for (const Type& part : parts)
            result = result.unionWith(part, types);
        return result;
    }

    Type classOf(BuiltinClassID builtinClass, std::initializer_list<TypeID> contents)
    {
        return Type::builtinClass(builtinClass, std::span<const TypeID> { contents.begin(), contents.size() });
    }

    ReaderFixture fixture;
    std::unique_ptr<Program> program;
    TypeArena types;
};

struct TypeSpelling {
    Type type;
    ASCIILiteral text;
    ASCIILiteral wireText;
};

void expectSpellings(const PrinterTest& test, std::initializer_list<TypeSpelling> spellings)
{
    for (const TypeSpelling& spelling : spellings) {
        expectText(test.printType(spelling.type), spelling.text);
        expectText(test.printType(spelling.type, PrintedFields::WireOnly), spelling.wireText);
    }
}

void testInvalidPayloadIDs()
{
    PrinterTest test;
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v9).setKey(PropertyKeyID(4000000000u));
    }), "v9 = PropGet v0 key=<invalid key 4000000000> @1009"_s);
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v16).setPattern(StringID(idPastEveryTable));
    }), "v16 = NewRegExp pattern=<invalid string 1000000> regExpFlags=[Global] @1016"_s);
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v6).setLayout(LayoutID(idPastEveryTable));
    }), "v6 = NewObject -, v3 layout=<invalid layout 1000000> fields=[0] @1006"_s);
}

void testInvalidEnumerator()
{
    PrinterTest test;
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v7).setMode(static_cast<ECMAMode>(9));
    }), "SuperSet v6, v2, v0, v3 mode=<invalid enumerator 9> @1007"_s);
    expectText(test.print(static_cast<LayoutKind>(200)), "<invalid enumerator 200>"_s);
    expectText(test.print(LayoutKind::Environment), "Environment"_s);
}

void testInvalidOpcode()
{
    PrinterTest test;
    String text = test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v2).opcode = static_cast<Opcode>(numberOfEnumerators<Opcode>);
    });
    expectPart(text, makeString("    <invalid opcode "_s, numberOfEnumerators<Opcode>, ">\n"_s));
    expectPart(text, "v3 = Double number=1.5 @1003"_s);
}

void testInvalidStorage()
{
    PrinterTest test;
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v2).opcode = Opcode::PropGet;
    }), "    v2 = PropGet <invalid operands 0>\n"_s);
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(b0).terminator.kind = TerminatorKind::Jump;
    }), "    Jump <invalid operands 1>\n"_s);
}

void testInvalidFlags()
{
    PrinterTest test;
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v9).flags |= 1u << 7;
    }), "v9 = PropGet v0 key=\"x\" [<invalid flag 7>] @1009"_s);
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(v9).setIsFreeName(true);
        copy.edit(v9).setIsForTypeOf(true);
    }), "v9 = PropGet v0 key=\"x\" [isFreeName, isForTypeOf] @1009"_s);
    expectText(test.print(OptionSet<FieldFlags>::fromRaw(1 | 1 << 15)), "[IsRequired, <invalid flag 15>]"_s);
}

void testInvalidTerminator()
{
    PrinterTest test;
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(b0).terminator.kind = static_cast<TerminatorKind>(numberOfEnumerators<TerminatorKind>);
    }), makeString("    <invalid terminator "_s, numberOfEnumerators<TerminatorKind>, ">\n"_s));
    expectPart(test.printChangedCopy(f7, [](Typed::Function& copy) {
        copy.edit(b0).terminator.targets.append(Target { BlockID(3u), { }, { } });
    }), "Branch v1 -> taken b1(), notTaken b2(), <invalid target 2> b3() @2000"_s);
}

void testValidCopy()
{
    PrinterTest test;
    for (uint32_t index = 0; index < test.program->functions.size(); ++index)
        AOT_TEST_CHECK(!test.printChangedCopy(FunctionID(index), [](Typed::Function&) { }).contains("<invalid"_s));
    AOT_TEST_CHECK(!test.printProgram(PrintedFields::All).contains("<invalid"_s));
}

void testIDsOutOfRange()
{
    PrinterTest test;
    expectText(test.print(FunctionID(idPastEveryTable)), "<invalid function 1000000>"_s);
    expectText(test.print(LayoutID(idPastEveryTable)), "<invalid layout 1000000>"_s);
    expectText(test.print(VariableID(idPastEveryTable)), "<invalid variable 1000000>"_s);
    expectText(test.print(ModuleID(idPastEveryTable)), "<invalid module 1000000>"_s);
    expectText(test.print(ModuleFailureID(idPastEveryTable)), "<invalid modulefailure 1000000>"_s);
    expectText(test.print(TypeSiteID(idPastEveryTable)), "<invalid typesite 1000000>"_s);
    expectText(test.print(EvalScopeID(idPastEveryTable)), "<invalid evalscope 1000000>"_s);
    expectText(test.print(ExpectedResultID(idPastEveryTable)), "<invalid expectedresult 1000000>"_s);
    expectText(test.print(DataID(idPastEveryTable)), "<invalid data 1000000>"_s);
    expectText(test.print(StringID(idPastEveryTable)), "<invalid string 1000000>"_s);
    expectText(test.print(PropertyKeyID(idPastEveryTable)), "<invalid key 1000000>"_s);
    expectText(test.print(TypeID(idPastEveryTable)), "<invalid type 1000000>"_s);
    expectText(test.print(LayoutSetID(idPastEveryTable)), "<invalid layoutset 1000000>"_s);
    expectText(test.print(CalleeSetID(idPastEveryTable)), "<invalid calleeset 1000000>"_s);
    expectText(test.print(BuiltinID(60000)), "<invalid builtin 60000>"_s);
    expectText(test.print(BuiltinClassID(60000)), "<invalid class 60000>"_s);
    expectText(test.print(BuiltinLayoutID(60000)), "<invalid layout 60000>"_s);
    expectText(test.print(RuntimeFunctionID(idPastEveryTable)), "<invalid runtimefunction 1000000>"_s);
    expectText(test.print(ValueID(idPastEveryTable)), "v1000000"_s);
    expectText(test.print(BlockID(idPastEveryTable)), "b1000000"_s);
    expectText(test.printType(Type::forLayoutSet(LayoutSetID(idPastEveryTable))), "<invalid layoutset 1000000>"_s);
    expectText(test.printType(Type::forOpaque(static_cast<OpaqueKind>(200))), "<invalid opaque 200>"_s);
}

void testNullIDs()
{
    PrinterTest test;
    expectText(test.print(FunctionID()), "-"_s);
    expectText(test.print(LayoutID()), "-"_s);
    expectText(test.print(ValueID()), "-"_s);
    expectText(test.print(StringID()), "-"_s);
    expectText(test.print(PropertyKeyID()), "-"_s);
    expectText(test.print(TypeID()), "-"_s);
    expectText(test.print(LayoutSetID()), "-"_s);
    expectText(test.print(CalleeSetID()), "-"_s);
}

void testProvisionalFunctionID()
{
    PrinterTest test;
    expectText(test.print(ProvisionalFunctions::idOf(3)), "f2147483651"_s);
    expectText(test.printType(Type::forFunction(ProvisionalFunctions::idOf(0))), "f2147483648"_s);
}

void testUnlistedValue()
{
    PrinterTest test;
    Typed::Function copy = test.program->at(f7);
    ValueID unlisted = addUnlistedValue(copy, Opcode::Null);
    AOT_TEST_CHECK(unlisted == ValueID(17u));

    StringPrintStream value;
    printValue(value, *test.program, copy, unlisted);
    expectText(value.toString(), "v17: anything = Null"_s);

    StringPrintStream function;
    printFunction(function, *test.program, f7, copy);
    AOT_TEST_CHECK(!function.toString().contains("v17"_s));

    StringPrintStream valuePastTheEnd;
    printValue(valuePastTheEnd, *test.program, copy, ValueID(18u));
    expectText(valuePastTheEnd.toString(), "<invalid value 18>"_s);

    expectPart(test.printChangedCopy(f7, [](Typed::Function& function) {
        function.edit(b0).parameters.append(ValueID(18u));
    }), "  b0(v0: anything, v1: boolean, <invalid value 18>):\n"_s);
}

void testOneFunction()
{
    PrinterTest test;
    expectText(test.printChangedCopy(f0, [](Typed::Function& copy) {
        copy.capabilities = { };
    }), "function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    expectText(test.printChangedCopy(f3, [](Typed::Function& copy) {
        copy.capabilities = { };
        copy.constructedClass = { };
    }), "function f3 name=\"D\" module=m0 kind=DerivedConstructor flags=[HasNewTarget] captures=[{type=env L1}] thisType={type=anything} results=[{type=anything}]\n"_s);
}

void testSets()
{
    PrinterTest test;
    TypeTable& types = test.program->types;
    expectText(test.print(types.intern(LayoutSet { { L2, L0 }, true })), "[L0, L2]"_s);
    expectText(test.print(types.intern(LayoutSet { { L0, L2 }, false })), "[L0, L2, ..]"_s);
    expectText(test.print(types.intern(LayoutSet { { }, false })), "[..]"_s);
    expectText(test.print(types.intern(LayoutSet { { }, true })), "[]"_s);
    expectText(test.print(types.intern(LayoutSet { { L2, L0 }, true, true })), "[L0, L2, nolayout]"_s);
    expectText(test.print(types.intern(LayoutSet { { L0 }, false, true })), "[L0, nolayout, ..]"_s);
    expectText(test.print(types.intern(LayoutSet { { }, true, true })), "[nolayout]"_s);
    expectText(test.print(types.intern(CalleeSet { { f3, f1 }, { Builtins::GlobalThis }, true })), "[f1, f3, builtin(GlobalThis)]"_s);
    expectText(test.print(types.intern(CalleeSet { { f1 }, { }, false })), "[f1, ..]"_s);
    expectText(test.print(types.intern(CalleeSet { { }, { Builtins::MathMax }, false })), "[builtin(MathMax), ..]"_s);
}

void testNumbersAddedLayouts()
{
    PrinterTest test;
    Program& program = *test.program;
    Layout layout;
    layout.kind = LayoutKind::CoroutineFrame;
    program.layouts.append(WTF::move(layout));
    LayoutID added(program.layouts.size() - 1);
    Layout record;
    record.name = test.string("Added"_s);
    program.layouts.append(WTF::move(record));
    LayoutID addedRecord(program.layouts.size() - 1);

    String text = test.printProgram(PrintedFields::All);
    expectPart(text, "layout L4 kind=CoroutineFrame\n"_s);
    expectPart(text, "layout L5 name=\"Added\"\n"_s);
    AOT_TEST_CHECK(!text.contains("kind=BuiltinResult"_s) && !text.contains("kind=ModuleNamespace"_s));

    expectText(test.print(added), "L4"_s);
    expectText(test.print(program.layoutOf(BuiltinLayouts::IteratorResult)), "L(IteratorResult)"_s);
    expectText(test.print(program.at(m0).namespaceLayout), "L(m0)"_s);

    std::array<LayoutID, 4> layouts { program.at(m0).namespaceLayout, program.layoutOf(BuiltinLayouts::IteratorResult), addedRecord, L0 };
    expectText(test.printType(Type::forLayouts(layouts, test.types)), "L0|L5|L(IteratorResult)|L(m0)"_s);
    expectText(test.print(test.types.completeLayoutSet(layouts)), "[L0, L5, L(IteratorResult), L(m0)]"_s);
}

void testNullContents()
{
    PrinterTest test;
    expectText(test.printType(test.classOf(BuiltinClasses::Map, { TypeID(), TypeTable::number })), "Map<anything, number>"_s);
    expectText(test.printType(test.classOf(BuiltinClasses::Map, { TypeTable::string, TypeID() })), "Map<string, anything>"_s);
    expectText(test.printType(test.classOf(BuiltinClasses::Map, { TypeTable::string })), "Map<string, anything>"_s);
    expectText(test.printType(test.classOf(BuiltinClasses::Array, { TypeTable::number, TypeTable::anything })), "Array<number>"_s);
    expectText(test.printType(test.classOf(BuiltinClasses::Map, { TypeTable::anything, TypeTable::anything })), "Map"_s);
    expectText(test.printType(Type::builtinClass(BuiltinClasses::Map)), "Map"_s);
    expectText(test.printType(test.classOf(BuiltinClasses::Array, { TypeID(idPastEveryTable) })), "Array<<invalid type 1000000>>"_s);
}

void testPrimitiveTypes()
{
    PrinterTest test;
    expectSpellings(test, {
        { Type::never(), "never"_s, "never"_s },
        { Type::anything(), "anything"_s, "anything"_s },
        { Type::anything().withPrimitives(Primitive::Absent), "anything|absent"_s, "anything|absent"_s },
        { Type::fromPrimitives(Primitive::All), "undefined|null|boolean|number|string|symbol|bigint"_s, "undefined|null|boolean|number|string|symbol|bigint"_s },
        { Type::fromPrimitives(Primitive::Undefined), "undefined"_s, "undefined"_s },
        { Type::fromPrimitives(Primitive::Boolean), "boolean"_s, "boolean"_s },
        { Type::fromPrimitives(Primitive::True), "true"_s, "true"_s },
        { Type::fromPrimitives(Primitive::False | Primitive::Null), "null|false"_s, "null|false"_s },
        { Type::fromPrimitives(Primitive::Absent), "absent"_s, "absent"_s },
        { Type::fromPrimitives(Primitive::Undefined | Primitive::BigInt | Primitive::Absent), "undefined|bigint|absent"_s, "undefined|bigint|absent"_s },
        { Type::object(), "object"_s, "object"_s },
        { Type::function(), "function"_s, "function"_s },
        { Type::object().withPrimitives(Primitive::Null | Primitive::String), "null|string|object"_s, "null|string|object"_s },
        { Type::function().withPrimitives(Primitive::Undefined | Primitive::Absent), "undefined|function|absent"_s, "undefined|function|absent"_s },
    });
}

void testNumberTypes()
{
    PrinterTest test;
    expectSpellings(test, {
        { Type::fromPrimitives(Primitive::Number), "number"_s, "number"_s },
        { Type::integer(Range::full()), "int"_s, "int"_s },
        { Type::integer(Range::int32()), "i32"_s, "i32"_s },
        { Type::integer(Range::one(7)), "7"_s, "7"_s },
        { Type::integer(Range::one(-5)), "-5"_s, "-5"_s },
        { Type::integer({ 0, 9 }), "int[0, 9]"_s, "int[0, 9]"_s },
        { Type::integer({ -maxTrackedInteger, 0 }), "int[-4611686018427387903, 0]"_s, "int[-4611686018427387903, 0]"_s },
        { Type::fromPrimitives(Primitive::Double), "double"_s, "number"_s },
        { Type::fromPrimitives(Primitive::WholeNumber), "whole"_s, "number"_s },
        { Type::fromPrimitives(Primitive::FractionalNumber), "fractional"_s, "number"_s },
        { Type::number(1.5), "fractional"_s, "number"_s },
        { Type::number(-0.0), "whole"_s, "number"_s },
        { Type::integer({ 0, 9 }).withPrimitives(Primitive::WholeNumber), "int[0, 9]|whole"_s, "number"_s },
        { Type::integer(Range::int32()).withPrimitives(Primitive::Double), "i32|double"_s, "number"_s },
        { Type::integer(Range::full()).withPrimitives(Primitive::FractionalNumber | Primitive::Undefined), "undefined|int|fractional"_s, "undefined|number"_s },
    });
}

void testLiteralTypes()
{
    PrinterTest test;
    StringID internedFirst = test.string("~z"_s);
    StringID internedSecond = test.string("~a"_s);
    AOT_TEST_CHECK(internedFirst < internedSecond);
    Type literals = test.unionOf({ Type::stringLiteral(internedFirst), Type::stringLiteral(internedSecond) });

    PropertyKeyID iterator = test.types.wellKnownSymbolKey(WellKnownSymbol::Iterator);
    PropertyKeyID asyncDispose = test.types.wellKnownSymbolKey(WellKnownSymbol::AsyncDispose);
    PropertyKeyID registered = test.types.registeredSymbolKey(internedSecond);
    PropertyKeyID createdLater = test.types.intern(createSymbolKey({ }, m0, 120));
    PropertyKeyID createdEarlier = test.types.intern(createSymbolKey({ }, m0, 7));
    AOT_TEST_CHECK(createdLater < createdEarlier);
    Type symbols = test.unionOf({ Type::symbolLiteral(registered), Type::symbolLiteral(createdLater), Type::symbolLiteral(createdEarlier), Type::symbolLiteral(iterator), Type::symbolLiteral(asyncDispose) });

    expectSpellings(test, {
        { Type::stringLiteral(internedFirst), "\"~z\""_s, "\"~z\""_s },
        { literals, "\"~a\"|\"~z\""_s, "\"~a\"|\"~z\""_s },
        { test.unionOf({ literals, Type::fromPrimitives(Primitive::Number | Primitive::Absent) }), "number|absent|\"~a\"|\"~z\""_s, "number|absent|\"~a\"|\"~z\""_s },
        { test.unionOf({ literals, Type::fromPrimitives(Primitive::String) }), "string"_s, "string"_s },
        { Type::symbolLiteral(iterator), "@@iterator"_s, "never"_s },
        { symbols, "@@asyncDispose|@@iterator|symbol@m0:7|symbol@m0:120|Symbol.for(\"~a\")"_s, "never"_s },
        { test.unionOf({ symbols, literals, Type::fromPrimitives(Primitive::Undefined) }), "undefined|\"~a\"|\"~z\"|@@asyncDispose|@@iterator|symbol@m0:7|symbol@m0:120|Symbol.for(\"~a\")"_s, "undefined|\"~a\"|\"~z\""_s },
    });
}

void testObjectTypes()
{
    PrinterTest test;
    std::array<LayoutID, 2> layouts { L2, L0 };
    Type withFunction = Type::function();
    withFunction.layouts = test.types.completeLayoutSet(layouts);
    withFunction.builtins.add(Builtins::GlobalThis);

    expectSpellings(test, {
        { Type::forLayout(L2, test.types), "L2"_s, "L2"_s },
        { Type::forLayouts(layouts, test.types), "L0|L2"_s, "L0|L2"_s },
        { Type::forLayoutSet(test.types.intern(LayoutSet { { L0, L2 }, true, true })), "L0|L2|nolayout"_s, "L0|L2|nolayout"_s },
        { Type::forEnvironment(L1), "env L1"_s, "env L1"_s },
        { Type::forEnvironment(L1).withPrimitives(Primitive::Absent), "absent|env L1"_s, "absent|env L1"_s },
        { Type::forFunction(f3), "f3"_s, "f3"_s },
        { test.unionOf({ Type::forFunction(f3), Type::forFunction(f1), Type::fromPrimitives(Primitive::Undefined) }), "undefined|f1|f3"_s, "undefined|f1|f3"_s },
        { Type::forBuiltin(Builtins::GlobalThis), "builtin(GlobalThis)"_s, "builtin(GlobalThis)"_s },
        { test.unionOf({ Type::forBuiltin(Builtins::MathMax), Type::forBuiltin(Builtins::GlobalThis) }), "builtin(GlobalThis)|builtin(MathMax)"_s, "builtin(GlobalThis)|builtin(MathMax)"_s },
        { withFunction, "function|L0|L2|builtin(GlobalThis)"_s, "function|L0|L2|builtin(GlobalThis)"_s },
        { Type::forOpaque(OpaqueKind::IteratorState), "iteratorstate"_s, "iteratorstate"_s },
        { Type::forOpaque(OpaqueKind::ClassToken), "classtoken"_s, "classtoken"_s },
        { Type::forOpaque(OpaqueKind::Guard), "guard"_s, "guard"_s },
        { Type::forOpaque(OpaqueKind::FlatString), "flatstring"_s, "flatstring"_s },
        { Type::forOpaque(OpaqueKind::CodeAddress), "codeaddress"_s, "codeaddress"_s },
        { Type::forOpaque(OpaqueKind::BuilderCursor), "buildercursor"_s, "buildercursor"_s },
        { Type::forOpaque(OpaqueKind::Exception).withPrimitives(Primitive::Absent), "absent|exception"_s, "absent|exception"_s },
    });
}

void testClassTypes()
{
    PrinterTest test;
    TypeID arrayOfNumbers = test.types.intern(test.classOf(BuiltinClasses::Array, { TypeTable::number }));
    TypeID instance = test.types.intern(Type::forLayout(L2, test.types));

    Type provenArray = test.classOf(BuiltinClasses::Array, { TypeTable::number });
    provenArray.classes[0].contentsAreProven = true;
    provenArray.classes[0].arrayStorage = ArrayStorage::Int32;
    Type provenArrayOfAnything = Type::builtinClass(BuiltinClasses::Array).withContentsProven(true);
    Type contiguousArray = Type::builtinClass(BuiltinClasses::Array);
    contiguousArray.classes[0].arrayStorage = ArrayStorage::Contiguous;
    Type pristineArray = test.classOf(BuiltinClasses::Array, { TypeTable::number });
    pristineArray.classes[0].provenPristine = { PristineAspects::Storage, PristineAspects::Names };
    Type regExps = test.unionOf({ Type::regExpLiteral(test.string("x"_s), { }), Type::regExpLiteral(test.string("ab+c"_s), { RegExpFlags::Global }) });

    expectSpellings(test, {
        { test.classOf(BuiltinClasses::Array, { TypeTable::number }), "Array<number>"_s, "Array<number>"_s },
        { test.classOf(BuiltinClasses::Map, { TypeTable::string, instance }), "Map<string, L2>"_s, "Map<string, L2>"_s },
        { Type::builtinClass(BuiltinClasses::Promise), "Promise"_s, "Promise"_s },
        { test.classOf(BuiltinClasses::Array, { arrayOfNumbers }), "Array<Array<number>>"_s, "Array<Array<number>>"_s },
        { test.unionOf({ Type::builtinClass(BuiltinClasses::Set), test.classOf(BuiltinClasses::Array, { TypeTable::number }), Type::fromPrimitives(Primitive::Null) }), "null|Array<number>|Set"_s, "null|Array<number>|Set"_s },
        { provenArray, "Array<number, proven, Int32>"_s, "Array<number>"_s },
        { provenArrayOfAnything, "Array<anything, proven>"_s, "Array"_s },
        { contiguousArray, "Array<anything, Contiguous>"_s, "Array"_s },
        { pristineArray, "Array<number, pristine[Storage, Names]>"_s, "Array<number>"_s },
        { test.classOf(BuiltinClasses::Array, { instance }), "Array<L2>"_s, "Array<L2>"_s },
        { test.classOf(BuiltinClasses::Array, { instance }).withContentsProven(true), "Array<L2, proven>"_s, "Array<L2>"_s },
        { Type::builtinClass(BuiltinClasses::RegExp), "RegExp"_s, "RegExp"_s },
        { regExps, "RegExp<\"ab+c\"[Global], \"x\"[]>"_s, "RegExp"_s },
    });
}

void testOrderOfTypeParts()
{
    PrinterTest test;
    Type type = test.unionOf({
        test.classOf(BuiltinClasses::Array, { TypeTable::number }),
        Type::forBuiltin(Builtins::GlobalThis),
        Type::forFunction(f1),
        Type::forLayout(L0, test.types),
        Type::symbolLiteral(test.types.wellKnownSymbolKey(WellKnownSymbol::Iterator)),
        Type::stringLiteral(test.string("~a"_s)),
        Type::fromPrimitives(Primitive::Absent | Primitive::BigInt | Primitive::Null),
        Type::integer(Range::int32()),
    });
    expectSpellings(test, {
        { type, "null|i32|bigint|absent|\"~a\"|@@iterator|L0|f1|builtin(GlobalThis)|Array<number>"_s, "null|i32|bigint|absent|\"~a\"|L0|f1|builtin(GlobalThis)|Array<number>"_s },
    });
    expectText(test.print(test.types.intern(type)), "null|i32|bigint|absent|\"~a\"|@@iterator|L0|f1|builtin(GlobalThis)|Array<number>"_s);
}

void testStrings()
{
    PrinterTest test;
    const Vector<char16_t> units { u'a', u'"', u'b', u'\\', u'c', u'\n', 0x7f, 0xe9, 0xd83d, 0xde00, 0xd800 };
    expectText(test.print(test.program->strings.intern(StringView(units.span()))), R"("a\"b\\c\n\u{7f}\u{e9}\u{1f600}\u{d800}")"_s);
    const Vector<char16_t> controlCharacters { 0, 0x1f, u' ', u'~', u'\t' };
    expectText(test.print(test.program->strings.intern(StringView(controlCharacters.span()))), R"("\u{0}\u{1f} ~\u{9}")"_s);
    expectText(test.print(test.program->strings.emptyString()), "\"\""_s);

    StringPrintStream out;
    printString(out, *test.program, test.string("x"_s));
    expectText(out.toString(), "\"x\""_s);
}

void testBytes()
{
    PrinterTest test;
    constexpr std::array<uint8_t, 10> bytes { 'a', '"', '\\', 0x00, 0x1f, 0x7f, 0xe9, '\n', ' ', '~' };
    expectText(test.print(std::span<const uint8_t> { bytes }), R"("a\x22\x5c\x00\x1f\x7f\xe9\x0a ~")"_s);
    expectText(test.print(std::span<const uint8_t> { }), "\"\""_s);
}

void testDoubles()
{
    PrinterTest test;
    expectText(test.print(0.0), "0"_s);
    expectText(test.print(-0.0), "-0"_s);
    expectText(test.print(1.5), "1.5"_s);
    expectText(test.print(100.0), "100"_s);
    expectText(test.print(1e21), "1e+21"_s);
    expectText(test.print(-2.5e-7), "-2.5e-7"_s);
    expectText(test.print(5e-324), "5e-324"_s);
    expectText(test.print(std::numeric_limits<double>::quiet_NaN()), "NaN"_s);
    expectText(test.print(std::numeric_limits<double>::infinity()), "Infinity"_s);
    expectText(test.print(-std::numeric_limits<double>::infinity()), "-Infinity"_s);
}

void testPropertyKeys()
{
    PrinterTest test;
    StringID name = test.string("x"_s);
    auto printKey = [&](const PropertyKey& key) {
        return test.print(test.types.intern(key));
    };
    expectText(printKey(createKey(PropertyKeyKind::Text, name)), "\"x\""_s);
    expectText(printKey(createKey(PropertyKeyKind::EnginePrivateName, name)), "@\"x\""_s);
    expectText(printKey(createKey(PropertyKeyKind::ModulePrivateName, name)), "@#\"x\""_s);
    expectText(printKey(createKey(PropertyKeyKind::PrivateName, name, L2)), "#\"x\"@L2"_s);
    expectText(printKey(createKey(PropertyKeyKind::WellKnownSymbol, { }, { }, WellKnownSymbol::Iterator)), "@@iterator"_s);
    expectText(printKey(createKey(PropertyKeyKind::WellKnownSymbol, { }, { }, WellKnownSymbol::AsyncDispose)), "@@asyncDispose"_s);
    expectText(printKey(createKey(PropertyKeyKind::WellKnownSymbol, { }, { }, WellKnownSymbol::ToPrimitive)), "@@toPrimitive"_s);
    expectText(printKey(createSymbolKey({ }, m0, 120)), "symbol@m0:120"_s);
    expectText(printKey(createSymbolKey(name, m0, 120)), "symbol(\"x\")@m0:120"_s);
    expectText(printKey(createSymbolKey({ }, m0, 120, AllocationSiteID(4u))), "symbol@m0:120"_s);
    expectText(printKey(createKey(PropertyKeyKind::RegisteredSymbol, name)), "Symbol.for(\"x\")"_s);

    StringPrintStream out;
    printPropertyKey(out, *test.program, test.types.textKey(name));
    expectText(out.toString(), "\"x\""_s);
}

void testMalformedPropertyKeys()
{
    PrinterTest test;
    auto printKey = [&](const PropertyKey& key) {
        return test.print(test.types.intern(key));
    };
    expectText(printKey(createKey(PropertyKeyKind::Text)), "-"_s);
    expectText(printKey(createKey(PropertyKeyKind::EnginePrivateName)), "@-"_s);
    expectText(printKey(createKey(PropertyKeyKind::PrivateName)), "#-@-"_s);
    expectText(printKey(createKey(PropertyKeyKind::WellKnownSymbol)), "@@-"_s);
    expectText(printKey(createKey(PropertyKeyKind::WellKnownSymbol, { }, { }, static_cast<WellKnownSymbol>(200))), "@@<invalid enumerator 200>"_s);
    expectText(printKey(createSymbolKey({ }, { }, { })), "symbol@-:-"_s);
    expectText(printKey(createKey(PropertyKeyKind::RegisteredSymbol)), "Symbol.for(-)"_s);
    expectText(printKey(createKey(static_cast<PropertyKeyKind>(200))), "<invalid enumerator 200>"_s);
    expectText(printKey(createKey(PropertyKeyKind::PrivateName, StringID(idPastEveryTable), LayoutID(idPastEveryTable))), "#<invalid string 1000000>@<invalid layout 1000000>"_s);
}

void testTargets()
{
    PrinterTest test;
    String text = test.printChangedCopy(readerFixtureHandlerFunction, [](Typed::Function& copy) {
        Target& taken = copy.edit(b0).terminator.taken();
        taken.arguments.append(ValueID(0u));
        taken.proofs.append(1);
    });
    expectPart(text, "Branch v1 -> taken b1(v0 | 0, 1), notTaken b2() @4000\n"_s);
    expectPart(text, "  b1(v2) handler=b3(v0):\n"_s);
    expectPart(text, "  b3(v4) role=CatchHandler:\n"_s);

    expectPart(test.printChangedCopy(readerFixtureHandlerFunction, [](Typed::Function& copy) {
        Block& block = copy.edit(BlockID(3u));
        block.role = BlockRole::DisposalCleanup;
        block.subject = ValueID(1u);
        block.frequency = BlockFrequency::Rare;
        block.handler = Target { BlockID(3u), { }, { } };
    }), "  b3(v4) role=DisposalCleanup subject=v1 frequency=Rare handler=b3():\n"_s);
}

void addEngineFields(PrinterTest& test)
{
    Program& program = *test.program;
    Typed::Function& stream = program.at(f7);
    stream.inlineFrames.append(InlineFrame { });
    InlineFrame frame;
    frame.callee = f1;
    frame.callPosition = 4;
    frame.module = m0;
    stream.inlineFrames.append(frame);

    Value& read = stream.edit(v9);
    read.type = TypeTable::number;
    read.setExpected(ExpectedType(TypeTable::int32));
    read.representation = Representation::Double;
    read.inlineFrame = 1;
    read.setAccessSite(AccessSiteID(4u));
    read.setReceivers(program.types.intern(LayoutSet { { L0, L2 }, true }));
    stream.edit(v4).isTailCall = true;
    stream.edit(b0).terminator.inlineFrame = 1;

    program.at(L0).fields[0].flow.occupancy = Occupancy::AlwaysFilled;
    program.at(L0).flow.members.append(L0);
    program.at(L2).classFlow.evaluations = ExecutionCount::AtMostOnce;
    program.variables[0].flow.writers = WriterState::AllKnown;
}

void testEngineFields()
{
    PrinterTest test;
    addEngineFields(test);
    String text = test.printProgram(PrintedFields::All);
    for (ASCIILiteral part : {
        "    v9: number ~ i32 @Double = PropGet v0 key=\"x\" accessSite=4 receivers=[L0, L2] @1009:1\n"_s,
        "    v4 = Call v0, -, v2, v3 expectedResult=x0 [isAnnotatedPure] @1004 tail\n"_s,
        "    Branch v1 -> taken b1(), notTaken b2() @2000:1\n"_s,
        " inlineFrames=[{}, {callee=f1 callPosition=4 module=m0}] {\n"_s,
        " flags=[IsRequired] flow={occupancy=AlwaysFilled}}] ownFieldCount=1 flow={members=[L0]}\n"_s,
        " classFlow={evaluations=AtMostOnce}\n"_s,
        " initialization=Undefined flow={writers=AllKnown}\n"_s,
        " namespaceLayout=L(m0)\n"_s,
        " constructedClass=L2\n"_s,
        " capabilities=["_s,
    })
        expectPart(text, part);
}

void testWireFieldsOnly()
{
    PrinterTest test;
    expectText(test.printProgram(PrintedFields::WireOnly), readerFixtureText);
    AOT_TEST_CHECK(test.printProgram(PrintedFields::All) != readerFixtureText);

    addEngineFields(test);
    expectText(test.printProgram(PrintedFields::WireOnly), readerFixtureText);

    StringPrintStream allFields;
    Typed::printProgram(allFields, *test.program);
    expectText(allFields.toString(), test.printProgram(PrintedFields::All));
}

} // anonymous namespace

std::span<const NativeTest> irPrinterTests()
{
    static constexpr NativeTest tests[] = {
        { "printer/invalid-payload-ids"_s, testInvalidPayloadIDs },
        { "printer/invalid-enumerator"_s, testInvalidEnumerator },
        { "printer/invalid-opcode"_s, testInvalidOpcode },
        { "printer/invalid-storage"_s, testInvalidStorage },
        { "printer/invalid-flags"_s, testInvalidFlags },
        { "printer/invalid-terminator"_s, testInvalidTerminator },
        { "printer/valid-copy"_s, testValidCopy },
        { "printer/ids-out-of-range"_s, testIDsOutOfRange },
        { "printer/null-ids"_s, testNullIDs },
        { "printer/provisional-function-id"_s, testProvisionalFunctionID },
        { "printer/unlisted-value"_s, testUnlistedValue },
        { "printer/one-function"_s, testOneFunction },
        { "printer/sets"_s, testSets },
        { "printer/numbers-added-layouts"_s, testNumbersAddedLayouts },
        { "printer/null-contents"_s, testNullContents },
        { "printer/primitive-types"_s, testPrimitiveTypes },
        { "printer/number-types"_s, testNumberTypes },
        { "printer/literal-types"_s, testLiteralTypes },
        { "printer/object-types"_s, testObjectTypes },
        { "printer/class-types"_s, testClassTypes },
        { "printer/order-of-type-parts"_s, testOrderOfTypeParts },
        { "printer/strings"_s, testStrings },
        { "printer/bytes"_s, testBytes },
        { "printer/doubles"_s, testDoubles },
        { "printer/property-keys"_s, testPropertyKeys },
        { "printer/malformed-property-keys"_s, testMalformedPropertyKeys },
        { "printer/targets"_s, testTargets },
        { "printer/engine-fields"_s, testEngineFields },
        { "printer/wire-fields-only"_s, testWireFieldsOnly },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
