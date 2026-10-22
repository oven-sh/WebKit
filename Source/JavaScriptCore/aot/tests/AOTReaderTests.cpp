/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRParser.h"
#include "AOTReaderTestSupport.h"
#include "AOTWireBody.h"
#include <array>
#include <utility>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

namespace {

using namespace ReaderTests;

void testAcceptsFixture()
{
    ReaderFixture fixture;
    auto program = fixture.read();

    auto parsed = parseProgram(readerFixtureText, IRForm::AsRead, fixture.build);
    AOT_TEST_CHECK(parsed);
    StringPrintStream parsedText;
    printProgram(parsedText, *parsed);
    StringPrintStream readText;
    printProgram(readText, *program);
    AOT_TEST_CHECK(parsedText.toString() == readText.toString());

    AOT_TEST_CHECK(program->form() == IRForm::AsRead);
    AOT_TEST_CHECK(program->at(f7).blocks[2].frequency == BlockFrequency::Unlikely);
    AOT_TEST_CHECK(program->at(f8).blocks[1].handler && program->at(f8).blocks[1].handler->block == BlockID(3u));
    AOT_TEST_CHECK(program->at(f8).blocks[3].role == BlockRole::CatchHandler);
    AOT_TEST_CHECK(hasElements(program->at(f8).blocks[0].terminator.taken().proofs, { 0u }));
    AOT_TEST_CHECK(program->at(f1).constructedClass == L2 && !program->at(f7).constructedClass);
    AOT_TEST_CHECK(program->at(f5).role == FunctionRole::Library && program->at(f5).enclosingRuntimeFunction == RuntimeFunctionID(0u));
    AOT_TEST_CHECK(findRuntimeFunction(*program, RuntimeFunctionID(0u))->function == f5 && !findRuntimeFunction(*program, RuntimeFunctionID(2u)));
    const Typed::Function& stream = program->at(f7);
    AOT_TEST_CHECK(stream.at(ValueID(0u)).type == TypeTable::anything && stream.at(ValueID(1u)).type == TypeTable::boolean);
    AOT_TEST_CHECK(!stream.at(ValueID(2u)).type && !stream.at(ValueID(15u)).type);

    typeProgramAsRead(*program, fixture.build);
    AOT_TEST_CHECK(verifyProgram(*program, IRForm::AsRead, fixture.build));
    AOT_TEST_CHECK(finishRead(fixture.build, program.get()).errors.isEmpty());
    expectAccepted("fixture"_s, readBytes(fixture.bytes.span()));
}

void testListCounts()
{
    ReaderFixture fixture;
    const std::pair<List, uint32_t> counts[] = {
        { List::ClassParts, 2 }, { List::TypeSites, 1 }, { List::DeclaredTypes, 15 }, { List::ExpectedResults, 1 },
        { List::TemplateParts, 1 }, { List::ValidationRules, 2 }, { List::ValidationMembers, 1 }, { List::ValidationNodes, 3 }, { List::Layouts, 4 },
        { List::Fields, 4 }, { List::StaticFields, 1 }, { List::Variables, 4 }, { List::Modules, 2 }, { List::ModuleRequests, 1 },
        { List::ModuleExports, 2 }, { List::SyntheticExports, 2 }, { List::RequiredNames, 1 }, { List::ColumnAdjustments, 1 },
        { List::ModuleFailures, 1 }, { List::EvalScopes, 1 }, { List::EvalBindings, 1 }, { List::RelationSubjects, 1 }, { List::StoredIntos, 1 },
        { List::HandedTos, 1 }, { List::OperandsOf, 1 }, { List::Functions, 9 }, { List::DataNodes, 2 },
        { List::DataMembers, 2 }, { List::Data, 1 }, { List::Entries, 1 }, { List::HostReads, 1 }, { List::RuntimeFunctions, 2 },
        { List::WebAssemblyModules, 1 },
    };
    for (auto [list, count] : counts)
        AOT_TEST_CHECK(fixture.countOf(list) == count);
    for (List list : allMembersOf<List>)
        AOT_TEST_CHECK(fixture.countOf(list));

    auto program = fixture.read();
    AOT_TEST_CHECK(program->numberOfLayoutsAsRead() == 4 && program->layouts.size() == numberOfLayoutIDs(fixture));
    AOT_TEST_CHECK(program->numberOfFunctionsAsRead() == 9 && program->functions.size() == 9);
    AOT_TEST_CHECK(program->typeSites.size() == 1 && program->expectedResults.size() == 1);
    AOT_TEST_CHECK(program->variables.size() == 4 && program->modules.size() == 2 && program->moduleFailures.size() == 1);
    AOT_TEST_CHECK(program->evalScopes.size() == 1 && program->data.size() == 1 && program->entries.size() == 1);
    AOT_TEST_CHECK(program->runtimeFunctions.size() == 2 && program->webAssemblyModules.size() == 1);
    AOT_TEST_CHECK(program->strings.size() >= fixture.countOf(List::Strings));
    AOT_TEST_CHECK(program->expectedResults[0].validation.size() == 3 && program->expectedResults[0].validation[2].rules.size() == 1);
}

void testEveryChangedWord()
{
    ReaderFixture fixture;
    Vector<uint8_t> bytes = fixture.bytes;
    for (size_t offset = 0; offset + wordSize <= bytes.size(); offset += wordSize) {
        dataLogLn("word at ", offset);
        uint32_t original = wordAt(bytes.span(), offset);
        for (uint32_t word : { 0u, 1u, original + 1, original - 1, hugeCount, invalidExternal, Wire::none }) {
            if (word == original)
                continue;
            setWordAt(bytes, offset, word);
            readBytes(bytes.span());
        }
        setWordAt(bytes, offset, original);
    }
}

void testEveryTruncation()
{
    ReaderFixture fixture;
    const Vector<uint8_t>& bytes = fixture.bytes;
    size_t endOfRecords = fixture.offsetOf<Wire::WebAssemblyModule>(List::WebAssemblyModules, 0) + fixture.countOf(List::WebAssemblyModules) * sizeof(Wire::WebAssemblyModule);
    AOT_TEST_CHECK(endOfRecords <= bytes.size() && bytes.size() - endOfRecords < sizeof(uint64_t));
    for (size_t length = 0; length < bytes.size(); length += wordSize) {
        dataLogLn("length ", length);
        expectRejected("truncated"_s, readBytes(bytes.span().first(length)), Kind::ProgramTooShort);
        if (length < sizeof(Wire::Header) || length >= endOfRecords)
            continue;
        Vector<uint8_t> copy(bytes.span().first(length));
        setWordAt(copy, offsetof(Wire::Header, sizeInBytes), static_cast<uint32_t>(length));
        AOT_TEST_CHECK(!readBytes(copy.span()).didRead);
    }
}

void testHeader()
{
    ReaderFixture fixture;
    auto size = static_cast<uint32_t>(fixture.bytes.size());
    auto incremented = [&](size_t offset) {
        return fixture.word(offset) + 1;
    };
    expectEach(fixture, {
        { "magic"_s, offsetof(Wire::Header, magic), incremented(offsetof(Wire::Header, magic)), Kind::NotAProgram },
        { "schemaHash.low"_s, offsetof(Wire::Header, schemaHash.low), incremented(offsetof(Wire::Header, schemaHash.low)), Kind::SchemaMismatch },
        { "schemaHash.high"_s, offsetof(Wire::Header, schemaHash.high), incremented(offsetof(Wire::Header, schemaHash.high)), Kind::SchemaMismatch },
        { "sizeInBytes too small"_s, offsetof(Wire::Header, sizeInBytes), size - static_cast<uint32_t>(wordSize), Kind::ProgramTooShort },
        { "sizeInBytes too large"_s, offsetof(Wire::Header, sizeInBytes), size + static_cast<uint32_t>(wordSize), Kind::ProgramTooShort },
    });
    expectRejected("no bytes"_s, readBytes({ }), Kind::ProgramTooShort);

    Vector<uint8_t> shifted(fixture.bytes.size() + 1);
    memcpySpan(shifted.mutableSpan().subspan(1), fixture.bytes.span());
    expectRejected("unaligned bytes"_s, readBytes(shifted.span().subspan(1)), Kind::ProgramTooShort);
}

void testSections()
{
    ReaderFixture fixture;
    auto size = static_cast<uint32_t>(fixture.bytes.size());
    for (List list : allMembersOf<List>) {
        size_t count = ReaderFixture::offsetOfSection(list, offsetof(Wire::Section, count));
        size_t start = ReaderFixture::offsetOfSection(list, offsetof(Wire::Section, offsetInBytes));
        const Damage damages[] = {
            { "count past the end"_s, count, hugeCount, Kind::SectionOutOfBounds },
            { "count whose size overflows 32 bits"_s, count, Wire::none, Kind::SectionOutOfBounds },
            { "count as large as the program"_s, count, size, Kind::SectionOutOfBounds },
            { "start past the end"_s, start, size + 8, Kind::SectionOutOfBounds },
            { "start near the end of the address space"_s, start, Wire::none - 3, Kind::SectionOutOfBounds },
            { "start unaligned"_s, start, fixture.word(start) + 1, Kind::SectionOutOfBounds },
        };
        for (const Damage& damage : damages) {
            ReadResult result = readBytes(damagedCopy(fixture, damage.offset, damage.word).span());
            expectRejected(damage.name, result, damage.expected);
            AOT_TEST_CHECK(result.errors[0].count == static_cast<uint32_t>(list));
        }
    }
}

void testStrings()
{
    ReaderFixture fixture;
    uint32_t ordinaryName = wireStringOf(fixture, "handled"_s);
    uint32_t builtinName = wireStringOf(fixture, "stream"_s);
    AOT_TEST_CHECK(fixture.word(stringOffset(fixture, ordinaryName, offsetof(Wire::String, builtinName))) == Wire::none);
    AOT_TEST_CHECK(fixture.word(stringOffset(fixture, builtinName, offsetof(Wire::String, builtinName))) == BuiltinNames::stream.index());
    expectEach(fixture, {
        { "String.length"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, length)), hugeCount, Kind::ListOutOfBounds },
        { "String.offset"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, offset)), fixture.countOf(List::Bytes), Kind::ListOutOfBounds },
        { "String.nameClass"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, nameClass)), numberOfEnumerators<NameClass>, Kind::UnknownEnumerator },
        { "String.builtinName"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, builtinName)), invalidExternal, Kind::IdOutOfRange },
        { "class of another name"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, nameClass)), static_cast<uint32_t>(NameClass::Length), Kind::NameClassMismatch },
        { "built-in name on an ordinary string"_s, stringOffset(fixture, ordinaryName, offsetof(Wire::String, builtinName)), BuiltinNames::length.index(), Kind::NameClassMismatch },
        { "built-in name of another string"_s, stringOffset(fixture, builtinName, offsetof(Wire::String, builtinName)), BuiltinNames::length.index(), Kind::NameClassMismatch },
        { "built-in name left out"_s, stringOffset(fixture, builtinName, offsetof(Wire::String, builtinName)), Wire::none, Kind::NameClassMismatch },
    });

    size_t firstByte = fixture.offsetOf<uint8_t>(List::Bytes, fixture.word(stringOffset(fixture, ordinaryName, offsetof(Wire::String, offset))));
    expectAfter(fixture, "equal strings"_s, Kind::DuplicateString, [&](Vector<uint8_t>& bytes) {
        memcpySpan(bytes.mutableSpan().subspan(stringOffset(fixture, 1, 0), sizeof(Wire::String)), fixture.bytes.subspan(stringOffset(fixture, 0, 0), sizeof(Wire::String)));
    });
    expectAfter(fixture, "overlong sequence"_s, Kind::StringNotCanonical, [&](Vector<uint8_t>& bytes) {
        bytes[firstByte] = 0xc0;
        bytes[firstByte + 1] = 0x80;
    });
    expectAfter(fixture, "surrogate pair in two sequences"_s, Kind::StringNotCanonical, [&](Vector<uint8_t>& bytes) {
        constexpr std::array<uint8_t, 6> splitPair { 0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80 };
        memcpySpan(bytes.mutableSpan().subspan(firstByte, splitPair.size()), std::span<const uint8_t> { splitPair });
    });
}

void testPropertyKeys()
{
    ReaderFixture fixture;
    uint32_t textKey = fixture.word(fieldOffset(fixture, L2, 0, offsetof(Wire::Field, key)));
    uint32_t privateName = fixture.word(fieldOffset(fixture, L2, 1, offsetof(Wire::Field, key)));
    expectEach(fixture, {
        { "PropertyKey.kind"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, kind)), numberOfEnumerators<PropertyKeyKind>, Kind::UnknownEnumerator },
        { "registered symbol"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, kind)), static_cast<uint32_t>(PropertyKeyKind::RegisteredSymbol), Kind::EngineOnlyOnWire },
        { "PropertyKey.text"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, text)), fixture.countOf(List::Strings), Kind::IdOutOfRange },
        { "PropertyKey.owner"_s, keyOffset(fixture, privateName, offsetof(Wire::PropertyKey, owner)), fixture.countOf(List::Layouts), Kind::IdOutOfRange },
        { "PropertyKey.symbol"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, symbol)), numberOfEnumerators<WellKnownSymbol>, Kind::UnknownEnumerator },
        { "PropertyKey.module"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, module)), fixture.countOf(List::Modules), Kind::IdOutOfRange },
        { "text key without text"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, text)), Wire::none, Kind::InvalidPropertyKey },
        { "text key with an owner"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, owner)), L0.index(), Kind::InvalidPropertyKey },
        { "text key with a symbol"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, symbol)), 0, Kind::InvalidPropertyKey },
        { "text key with a position"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, position)), 0, Kind::InvalidPropertyKey },
        { "private name without an owner"_s, keyOffset(fixture, privateName, offsetof(Wire::PropertyKey, owner)), Wire::none, Kind::InvalidPropertyKey },
        { "well-known symbol without a symbol"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, kind)), static_cast<uint32_t>(PropertyKeyKind::WellKnownSymbol), Kind::InvalidPropertyKey },
        { "created symbol without a module"_s, keyOffset(fixture, textKey, offsetof(Wire::PropertyKey, kind)), static_cast<uint32_t>(PropertyKeyKind::CreatedSymbol), Kind::InvalidPropertyKey },
    });
    expectAfter(fixture, "equal keys"_s, Kind::DuplicatePropertyKey, [&](Vector<uint8_t>& bytes) {
        memcpySpan(bytes.mutableSpan().subspan(keyOffset(fixture, 1, 0), sizeof(Wire::PropertyKey)), fixture.bytes.subspan(keyOffset(fixture, 0, 0), sizeof(Wire::PropertyKey)));
    });
}

void testTypeWords()
{
    ReaderFixture fixture;
    WireTypes types(fixture);
    expectEach(fixture, {
        { "Type.primitives"_s, typeOffset(fixture, types.number, offsetof(Wire::Type, primitives)), fixture.word(typeOffset(fixture, types.number, offsetof(Wire::Type, primitives))) | highBit, Kind::UnknownFlag },
        { "Type.flags"_s, typeOffset(fixture, types.number, offsetof(Wire::Type, flags)), 1u << numberOfEnumerators<TypeFlags>, Kind::UnknownFlag },
        { "Type.opaque"_s, typeOffset(fixture, types.number, offsetof(Wire::Type, opaque)), numberOfEnumerators<OpaqueKind>, Kind::UnknownEnumerator },
        { "opaque kind of the engine"_s, typeOffset(fixture, types.number, offsetof(Wire::Type, opaque)), static_cast<uint32_t>(OpaqueKind::Guard), Kind::EngineOnlyOnWire },
        { "Type.environment"_s, typeOffset(fixture, types.environment, offsetof(Wire::Type, environment)), numberOfLayoutIDs(fixture), Kind::IdOutOfRange },
        { "environment that is an object layout"_s, typeOffset(fixture, types.environment, offsetof(Wire::Type, environment)), L0.index(), Kind::InvalidLayout },
        { "Type.strings"_s, typeOffset(fixture, types.literals, offsetof(Wire::Type, strings.count)), hugeCount, Kind::ListOutOfBounds },
        { "Type.layouts"_s, typeOffset(fixture, types.objects, offsetof(Wire::Type, layouts.count)), hugeCount, Kind::ListOutOfBounds },
        { "Type.classes"_s, typeOffset(fixture, types.objects, offsetof(Wire::Type, classes.count)), hugeCount, Kind::ListOutOfBounds },
        { "Type.builtins"_s, typeOffset(fixture, types.builtins, offsetof(Wire::Type, builtins.count)), hugeCount, Kind::ListOutOfBounds },
    });
}

void testTypeLists()
{
    ReaderFixture fixture;
    WireTypes types(fixture);
    Wire::Span literals = fixture.spanAt(typeOffset(fixture, types.literals, offsetof(Wire::Type, strings)));
    Wire::Span layouts = fixture.spanAt(typeOffset(fixture, types.objects, offsetof(Wire::Type, layouts)));
    Wire::Span classes = fixture.spanAt(typeOffset(fixture, types.objects, offsetof(Wire::Type, classes)));
    Wire::Span builtins = fixture.spanAt(typeOffset(fixture, types.builtins, offsetof(Wire::Type, builtins)));
    AOT_TEST_CHECK(literals.count == 2 && layouts.count == 3 && classes.count == 2 && builtins.count == 2);
    auto classPartOffset = [&](uint32_t index, size_t member) {
        return fixture.offsetOf<Wire::ClassPart>(List::ClassParts, classes.start + index, member);
    };
    AOT_TEST_CHECK(fixture.word(classPartOffset(0, offsetof(Wire::ClassPart, builtinClass))) == BuiltinClasses::Array.index());
    size_t lastLayout = wordOffset(fixture, layouts.start + 2);

    expectOrderChecked(fixture, "string literals"_s, wordOffset(fixture, literals.start), wordSize);
    expectOrderChecked(fixture, "layouts"_s, wordOffset(fixture, layouts.start), wordSize);
    expectOrderChecked(fixture, "class parts"_s, classPartOffset(0, 0), sizeof(Wire::ClassPart));
    expectOrderChecked(fixture, "built-ins"_s, wordOffset(fixture, builtins.start), wordSize);

    expectEach(fixture, {
        { "string literal past the end"_s, wordOffset(fixture, literals.start + 1), fixture.countOf(List::Strings), Kind::IdOutOfRange },
        { "layout past the derived layouts"_s, lastLayout, numberOfLayoutIDs(fixture), Kind::IdOutOfRange },
        { "environment among the layouts"_s, wordOffset(fixture, layouts.start), L1.index(), Kind::InvalidLayout },
        { "built-in past the end"_s, wordOffset(fixture, builtins.start + 1), invalidExternal, Kind::IdOutOfRange },
        { "ClassPart.builtinClass"_s, classPartOffset(0, offsetof(Wire::ClassPart, builtinClass)), invalidExternal, Kind::IdOutOfRange },
        { "no contents"_s, classPartOffset(0, offsetof(Wire::ClassPart, contents.count)), 0, Kind::ListOutOfBounds },
        { "too many contents"_s, classPartOffset(0, offsetof(Wire::ClassPart, contents.count)), 2, Kind::ListOutOfBounds },
        { "too few contents"_s, classPartOffset(1, offsetof(Wire::ClassPart, contents.count)), 1, Kind::ListOutOfBounds },
        { "ClassPart.contents"_s, classPartOffset(0, offsetof(Wire::ClassPart, contents.start)), fixture.countOf(List::Words), Kind::ListOutOfBounds },
        { "contents name the type itself"_s, wordOffset(fixture, fixture.spanAt(classPartOffset(0, offsetof(Wire::ClassPart, contents))).start), types.objects, Kind::TypeRefersForward },
    });

    expectAccepted("first derived layout"_s, readBytes(damagedCopy(fixture, lastLayout, fixture.countOf(List::Layouts)).span()));
    expectAccepted("last derived layout"_s, readBytes(damagedCopy(fixture, lastLayout, numberOfLayoutIDs(fixture) - 1).span()));
}

void testTypesAreCanonicalized()
{
    ReaderFixture fixture;
    WireTypes types(fixture);
    auto expectType = [&](ASCIILiteral name, size_t offset, uint32_t word, TypeID (*typeOf)(const Program&), ASCIILiteral expected) {
        BuildContext build(createOptions());
        Vector<uint8_t> storage;
        auto program = readChanged(fixture, build, storage, offset, word);
        String printed = printedType(*program, typeOf(*program));
        if (printed != expected)
            dataLogLn(name, ": read as ", printed, ", expected ", expected);
        AOT_TEST_CHECK(printed == expected);
        expectAccepted(name, readBytes(storage.span()));
    };
    auto typeOfLiterals = [](const Program& program) { return program.variables[0].declared.type; };
    auto typeOfInt32 = [](const Program& program) { return program.layouts[0].fields[0].declared.type; };
    auto typeOfNumber = [](const Program& program) { return program.layouts[2].fields[0].declared.type; };
    auto typeOfObjects = [](const Program& program) { return program.variables[1].declared.type; };
    auto primitives = [](Wire::Primitives first, Wire::Primitives second) { return static_cast<uint32_t>(first) | static_cast<uint32_t>(second); };

    size_t literalPrimitives = typeOffset(fixture, types.literals, offsetof(Wire::Type, primitives));
    expectType("unchanged"_s, literalPrimitives, fixture.word(literalPrimitives), typeOfLiterals, "undefined|\"a\"|\"b\""_s);
    expectType("string absorbs literals"_s, literalPrimitives, primitives(Wire::Primitives::Undefined, Wire::Primitives::String), typeOfLiterals, "undefined|string"_s);
    expectType("literals stay beside a number"_s, literalPrimitives, primitives(Wire::Primitives::Undefined, Wire::Primitives::Number), typeOfLiterals, "undefined|number|\"a\"|\"b\""_s);
    expectType("minimum above maximum"_s, typeOffset(fixture, types.int32, offsetof(Wire::Type, minimum.high)), 1, typeOfInt32, "never"_s);
    expectType("maximum past the tracked integers"_s, typeOffset(fixture, types.int32, offsetof(Wire::Type, maximum.high)), 0x40000000, typeOfInt32, "int[-2147483648, 4611686018427387903]"_s);
    expectType("integers beside number"_s, typeOffset(fixture, types.number, offsetof(Wire::Type, flags)), static_cast<uint32_t>(TypeFlags::HasIntegers), typeOfNumber, "number"_s);
    expectType("object absorbs layouts and classes"_s, typeOffset(fixture, types.objects, offsetof(Wire::Type, flags)), static_cast<uint32_t>(TypeFlags::AnyObject), typeOfObjects, "object"_s);
}

constexpr ASCIILiteral declaredLayoutsText = R"IR(layout L0 name="I" module=m0
layout L1 name="J" module=m0 supertypes=[L0]
layout L2 kind=Instance name="C" module=m0 supertypes=[L0]
variable g0 name="a" module=m0 declared={type=undefined|L0}
variable g1 name="b" module=m0 declared={type=L0 flags=[IsExpected]}
variable g2 name="c" module=m0 declared={type=L2}
module m0 path="/a.ts" displayPath="a.ts"
function f0 name="f" module=m0 parameters=[{type=L1}] results=[{type=anything}] {
  b0(v0: L1):
    Return v0
}
entry module=m0
)IR"_s;

// Each layout in a type stands for its family. Nothing is moved aside, and no position is widened to any object.
void testPositionsKeepTheirLayouts()
{
    ReaderFixture fixture(declaredLayoutsText);
    auto program = fixture.read();
    auto expectPosition = [&](const DeclaredType& declared, ASCIILiteral type, OptionSet<PositionFlags> flags) {
        AOT_TEST_CHECK(printedType(*program, declared.type) == type);
        AOT_TEST_CHECK(declared.flags == flags);
    };
    expectPosition(program->variables[0].declared, "undefined|L0|L1|L2"_s, { });
    expectPosition(program->variables[1].declared, "L0|L1|L2"_s, { PositionFlags::IsExpected });
    expectPosition(program->variables[2].declared, "L2"_s, { });
    expectPosition(program->functions[0].parameters[0], "L1"_s, { });
    AOT_TEST_CHECK(program->functions[0].at(ValueID(0u)).type == program->functions[0].parameters[0].type);
    expectAccepted("declared layouts"_s, readBytes(fixture.bytes.span()));

    ReadResult prepared = readBytesInForm(fixture.bytes.span(), IRForm::Prepared);
    expectAccepted("declared layouts in a later form"_s, prepared);
}

void testLongListsAreNotWidened()
{
    BuildContext build(createOptions());
    auto numberOfLayouts = static_cast<uint32_t>(build.limit(LimitID::MaxLayoutsInType)) + 1;
    StringBuilder text;
    StringBuilder type;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        text.append("layout L"_s, index, '\n');
        type.append(index ? "|L"_s : "L"_s, index);
    }
    text.append("variable g0 name=\"g\" module=m0 declared={type="_s, type.toString(), "}\nmodule m0 path=\"/a.ts\" displayPath=\"a.ts\"\nentry module=m0\n"_s);

    ReaderFixture fixture(text.toString());
    auto program = Program::read(fixture.bytes.span(), build);
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(membersOf(*program, program->at(program->variables[0].declared.type)).size() == numberOfLayouts);
    AOT_TEST_CHECK(build.takeLosses(program.get()).isEmpty());
}

void testFormParameter()
{
    static_assert(!formsOf(Opcode::Unspecified).contains(IRForm::AsRead) && formsOf(Opcode::Unspecified).contains(IRForm::Prepared));
    static_assert(formsOf(Opcode::Convert).contains(IRForm::AsRead) && !formsOf(Opcode::Convert).contains(IRForm::Prepared));
    static_assert(formsOf(Opcode::Check).contains(IRForm::Prepared) && !formsOf(Opcode::Check).contains(IRForm::Selected));
    ReaderFixture fixtureWithRequest;
    expectAccepted("request in the first form"_s, readBytesInForm(fixtureWithRequest.bytes.span(), IRForm::AsRead));
    expectRejected("request in the second form"_s, readBytesInForm(fixtureWithRequest.bytes.span(), IRForm::Prepared), Kind::RequiredPassLeftOperation);

    String textWithoutRequest = makeStringByReplacingAll(String(readerFixtureText), "Convert v9 declared={type=number flags=[IsExpected]}"_s, "Check v9 castType=number"_s);
    AOT_TEST_CHECK(textWithoutRequest != readerFixtureText);
    ReaderFixture fixture(textWithoutRequest);
    expectAccepted("fixture in the first form"_s, readBytesInForm(fixture.bytes.span(), IRForm::AsRead));
    expectAccepted("fixture in the second form"_s, readBytesInForm(fixture.bytes.span(), IRForm::Prepared));
    expectRejected("opcode of an earlier form"_s, readBytesInForm(fixture.bytes.span(), IRForm::Selected), Kind::OpcodeNotInForm);

    Vector<uint8_t> withLaterOpcode = damagedCopy(fixture, fixture.offsetOfHeadAt(f7, 1011), static_cast<uint32_t>(Opcode::Unspecified));
    expectRejected("opcode of a later form"_s, readBytesInForm(withLaterOpcode.span(), IRForm::AsRead), Kind::EngineOnlyOnWire);
    expectAccepted("opcode of the form read"_s, readBytesInForm(withLaterOpcode.span(), IRForm::Prepared));

    Vector<uint8_t> withEngineKind = damagedCopy(fixture, layoutOffset(fixture, L1, offsetof(Wire::Layout, kind)), static_cast<uint32_t>(LayoutKind::Closure));
    expectRejected("layout kind of the engine"_s, readBytesInForm(withEngineKind.span(), IRForm::AsRead), Kind::EngineOnlyOnWire);
    expectAccepted("layout kind of the engine in a later form"_s, readBytesInForm(withEngineKind.span(), IRForm::Prepared));
}

constexpr ASCIILiteral parameterSitesText = R"IR(typesite w0 module=m0 position=3 kind=Parameter
typesite w1 module=m0 position=9 kind=Variable
variable g0 name="g" module=m0 declared={type=number site=w1}
module m0 path="/a.ts" displayPath="a.ts"
function f0 name="untyped" module=m0 parameters=[{type=anything}] results=[{type=undefined}]
function f1 name="typed" module=m0 parameters=[{type=anything}, {type=number site=w0}] results=[{type=undefined}]
entry module=m0
)IR"_s;

void testParameterSiteFunctions()
{
    ReaderFixture fixture(parameterSitesText);
    auto program = fixture.read();
    AOT_TEST_CHECK(program->typeSites[0].function == f1 && !program->typeSites[1].function);
}

void testProgramOutlivesBytes()
{
    ReaderFixture fixture;
    auto program = fixture.read();
    fixture.bytes.fill(0xff);
    const Module& module = program->modules[0];
    AOT_TEST_CHECK(String::fromUTF8(program->string(module.path)) == "/a.ts"_s);
    AOT_TEST_CHECK(String::fromUTF8(module.functionText) == "a => a\n"_s);
    AOT_TEST_CHECK(module.positions.lineStarts.size() == 2 && module.positions.lineStarts[1] == 12);
    AOT_TEST_CHECK(program->buffer().empty());
}

class BodyTranscript final : public WireBodyVisitor {
public:
    static void add(StringPrintStream& out, const Instruction& instruction)
    {
        out.print(" @", instruction.position, " flags ", static_cast<unsigned>(instruction.flags), " operands");
        for (ValueID operand : instruction.operands())
            out.print(" ", operand.index());
        out.print(" payload");
        for (uint32_t word : instruction.inlinePayload())
            out.print(" ", word);
        for (uint32_t word : instruction.payloadListFrom(numberOfInlinePayloadWords))
            out.print(" ", word);
        out.println();
    }
    static void add(StringPrintStream& out, const Target& target)
    {
        out.print("  target ", target.block.index(), " arguments");
        for (ValueID argument : target.arguments)
            out.print(" ", argument.index());
        out.print(" proofs");
        for (uint32_t proof : target.proofs)
            out.print(" ", proof);
        out.println();
    }
    static void addBlock(StringPrintStream& out, BlockID id, std::span<const ValueID> parameters, BlockRole role, BlockFrequency frequency, ValueID subject, const Target* handler, size_t numberOfValues)
    {
        out.print("block ", id.index(), " role ", static_cast<unsigned>(role), " frequency ", static_cast<unsigned>(frequency), " subject ", subject.index(), " values ", numberOfValues, " parameters");
        for (ValueID parameter : parameters)
            out.print(" ", parameter.index());
        out.println();
        if (handler)
            add(out, *handler);
    }
    static void addValue(StringPrintStream& out, ValueID id, const Value& value)
    {
        out.print(" value ", id.index(), " ", nameOf(value.opcode));
        add(out, value);
    }
    static void addTerminator(StringPrintStream& out, const Terminator& terminator)
    {
        out.print(" terminator ", nameOf(terminator.kind));
        add(out, terminator);
        for (const Target& target : terminator.targets)
            add(out, target);
    }

    void beginBody(const WireBodyHead& head) final { text.println("body ", head.function.index(), " kind ", static_cast<unsigned>(head.record.kind), " values ", head.numberOfValues, " blocks ", head.numberOfBlocks); }
    void beginBlock(const WireBlockHead& head) final { addBlock(text, head.block, head.parameters, head.role, head.frequency, head.subject, head.handler, head.numberOfValues); }
    void visitValue(ValueID id, const Value& value) final { addValue(text, id, value); }
    void visitTerminator(BlockID, const Terminator& terminator) final { addTerminator(text, terminator); }
    void endBody() final { text.println("end"); }

    StringPrintStream text;
};

void testWireBodiesAreWalkedAsRead()
{
    ReaderFixture fixture;
    auto [program, bodies] = readProgramKeepingWireBodies(fixture.bytes.span(), fixture.build);
    AOT_TEST_CHECK(program && bodies);
    unsigned numberOfBodies = 0;
    for (uint32_t index = 0; index < program->functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program->at(id);
        AOT_TEST_CHECK(bodies->hasBody(id) == function.hasBody());
        if (!function.hasBody())
            continue;
        ++numberOfBodies;

        StringPrintStream expected;
        OpcodeSet operations;
        expected.println("body ", index, " kind ", static_cast<unsigned>(function.kind), " values ", function.values.size(), " blocks ", function.blocks.size());
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            const Block& block = function.blocks[blockIndex];
            BodyTranscript::addBlock(expected, BlockID(blockIndex), block.parameters.span(), block.role, block.frequency, block.subject, block.handler ? &*block.handler : nullptr, block.values.size());
            for (ValueID value : block.values) {
                BodyTranscript::addValue(expected, value, function.at(value));
                operations.add(function.at(value).opcode);
            }
            BodyTranscript::addTerminator(expected, block.terminator);
        }
        expected.println("end");

        BodyTranscript transcript;
        bodies->walk(id, transcript);
        AOT_TEST_CHECK(transcript.text.toString() == expected.toString());
        AOT_TEST_CHECK(bodies->operationsOf(id) == operations);
    }
    AOT_TEST_CHECK(numberOfBodies > 2);
    AOT_TEST_CHECK(!bodies->hasBody(FunctionID(static_cast<uint32_t>(program->functions.size()))));
}

} // anonymous namespace

std::span<const NativeTest> readerTests()
{
    static constexpr NativeTest tests[] = {
        { "reader/accepts-fixture"_s, testAcceptsFixture },
        { "reader/list-counts"_s, testListCounts },
        { "reader/survives-every-changed-word"_s, testEveryChangedWord },
        { "reader/survives-every-truncation"_s, testEveryTruncation },
        { "reader/header"_s, testHeader },
        { "reader/sections"_s, testSections },
        { "reader/strings"_s, testStrings },
        { "reader/property-keys"_s, testPropertyKeys },
        { "reader/type-words"_s, testTypeWords },
        { "reader/type-lists"_s, testTypeLists },
        { "reader/types-are-canonicalized"_s, testTypesAreCanonicalized },
        { "reader/positions-keep-their-layouts"_s, testPositionsKeepTheirLayouts },
        { "reader/long-lists-are-not-widened"_s, testLongListsAreNotWidened },
        { "reader/form-parameter"_s, testFormParameter },
        { "reader/parameter-site-functions"_s, testParameterSiteFunctions },
        { "reader/program-outlives-bytes"_s, testProgramOutlivesBytes },
        { "reader/wire-bodies-are-walked-as-read"_s, testWireBodiesAreWalkedAsRead },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
