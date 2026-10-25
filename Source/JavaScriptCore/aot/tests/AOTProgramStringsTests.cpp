/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTLiteralStringDigest.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramFile.h"
#include "AOTProgramStrings.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include "Symbol.h"
#include <algorithm>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> programStringsTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Light" fields=[{key="color" declared={type="red"|"green"|"\u{65e5}\u{672c}"|"amber"|"violet"|"\u{7d05}\u{8449}"} flags=[IsRequired]}, {key="x" declared={type=number}}]
layout L1 name="Holder" fields=[]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="constants" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = String string="first constant"
    v2 = Call v0, -, v1
    v3 = String string="\u{65e5}\u{672c}\u{8a9e}"
    v4 = Call v0, -, v3
    v5 = String string=""
    v6 = Call v0, -, v5
    v7 = String string="x"
    v8 = Call v0, -, v7
    v9 = String string="red"
    v10 = NewObject -, v9 layout=L0 fields=[0]
    v11 = Call v0, -, v10
    v12 = String string="green"
    v13 = NewObject -, v12 layout=L0 fields=[0]
    v14 = Call v0, -, v13
    v15 = String string="\u{65e5}\u{672c}"
    v16 = NewObject -, v15 layout=L0 fields=[0]
    v17 = Call v0, -, v16
    v18 = BigInt digits="-123456789012345678901234567890"
    v19 = Call v0, -, v18
    v20 = String string="second constant"
    v21 = Call v0, -, v20
    v22 = NewObject layout=L1 fields=[]
    v23 = NewClosure function=f2
    DefineGetter v22, v23 key="hue"
    v25 = FinishObject v22
    v26 = Call v0, -, v25
    Return
}
function f2 name="readHue" flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=1
    Return v1
}
)IR"_s;

static Vector<uint8_t> compileWithReferences(ASCIILiteral referencesWord)
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static void runWithReferences(ASCIILiteral testName, ASCIILiteral referencesWord, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }

    TemporaryFile image(".image"_s, compileWithReferences(referencesWord).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, image.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process that uses the image with ", referencesWord, " ", result.description());
        failNativeTest(__FILE__, __LINE__, testName.characters());
    }
}

static const ImageText& imageText()
{
    return Image::current()->text();
}

static String japanese(unsigned length)
{
    static constexpr char16_t characters[] = { 0x65e5, 0x672c, 0x8a9e };
    return String(std::span(characters).first(length));
}

static uint32_t constantNumberOf(StringView wanted)
{
    for (uint32_t number = 0; number < imageText().numberOfStringConstants(); ++number) {
        if (imageText().equals(imageText().stringConstant(number), wanted))
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the text is a string constant of the image");
}

static uint32_t literalNumberOf(LoadedProgram& program, StringView wanted)
{
    std::span<const uint32_t> literalStrings = program.state().tables.literalStrings();
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        if (imageText().equals(StringID(literalStrings[number]), wanted))
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the text is a literal string of the image");
}

static uint32_t identifierNumberOf(const Invocable<bool(const ImageIdentifierWord&)> auto& matches)
{
    for (uint32_t number = 1; number < imageText().numberOfIdentifiers(); ++number) {
        if (matches(imageText().identifier(number)))
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the identifier");
}

static uint32_t identifierNumberOfText(StringView wanted)
{
    return identifierNumberOf([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::Text && record.text() && imageText().equals(record.text(), wanted);
    });
}

static uint32_t keyCellNumberOf(LoadedProgram& program, uint32_t identifier)
{
    std::span<const uint32_t> identifiers = program.state().tables.keyCellIdentifiers();
    for (uint32_t number = 0; number < identifiers.size(); ++number) {
        if (identifiers[number] == identifier)
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the identifier has a key cell");
}

static bool hasText(LoadedProgram& program, JSString* string, StringView expected)
{
    return string->value(program.realm()).data == expected;
}

static bool usesWideReferences()
{
    return Image::current()->header().options.useWideReferences();
}

static JSCell* cellInInstance(LoadedProgram& program, uint32_t number)
{
    Instance& instance = program.instance();
    if (usesWideReferences())
        return instance.runtime.constants.wideStringCells[number].get();
    uint32_t word = instance.stringCell(number).get();
    return word ? MarkedBlockCage::decode(instance.cellCageBase, word) : nullptr;
}

static void testConstantIsCreatedOnce(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    uint32_t first = constantNumberOf("first constant"_s);
    uint32_t second = constantNumberOf("second constant"_s);
    AOT_TEST_CHECK(first != second);
    AOT_TEST_CHECK(imageText().numberOfStringConstants() == program.state().tables.instanceSizes.numberOfStringCells);
    AOT_TEST_CHECK(!!program.instance().runtime.constants.wideStringCells == usesWideReferences());

    AOT_TEST_CHECK(!strings.constantIfCreated(first) && !cellInInstance(program, first));
    JSString* cell = strings.constant(vm, first);
    AOT_TEST_CHECK(hasText(program, cell, "first constant"_s));
    AOT_TEST_CHECK(strings.constantIfCreated(first) == cell && cellInInstance(program, first) == cell);
    AOT_TEST_CHECK(strings.constant(vm, first) == cell);
    AOT_TEST_CHECK(!strings.constantIfCreated(second) && !cellInInstance(program, second));

    JSString* other = strings.constant(vm, second);
    AOT_TEST_CHECK(other != cell && hasText(program, other, "second constant"_s));
    AOT_TEST_CHECK(strings.constantIfCreated(first) == cell);
}

static NEVER_INLINE void createConstants(LoadedProgram& program, std::initializer_list<uint32_t> numbers)
{
    for (uint32_t number : numbers)
        program.state().strings->constant(program.vm(), number);
}

static void testConstantSurvivesCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    uint32_t first = constantNumberOf("first constant"_s);
    uint32_t second = constantNumberOf("second constant"_s);
    uint32_t wide = constantNumberOf(japanese(3));

    createConstants(program, { first, wide });
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        for (uint32_t number : { first, wide }) {
            JSString* cell = strings.constantIfCreated(number);
            AOT_TEST_CHECK(cell && vm.heap.isMarked(cell) && cell->isString());
        }
        AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(first), "first constant"_s));
        AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(wide), japanese(3)));
    }

    createConstants(program, { second });
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.isMarked(strings.constantIfCreated(second)));
    AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(second), "second constant"_s));
    AOT_TEST_CHECK(vm.heap.isMarked(strings.constantIfCreated(first)));
}

static void testTextIn16Bits(LoadedProgram& program)
{
    JSString* cell = program.state().strings->constant(program.vm(), constantNumberOf(japanese(3)));
    AOT_TEST_CHECK(cell->length() == 3 && !cell->is8Bit());
    AOT_TEST_CHECK(hasText(program, cell, japanese(3)));
    AOT_TEST_CHECK(!hasText(program, cell, japanese(2)));

    JSString* narrow = program.state().strings->constant(program.vm(), constantNumberOf("first constant"_s));
    AOT_TEST_CHECK(narrow->is8Bit());
}

static void testSmallStringsAreTheEngines(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    StringID empty(program.state().tables.literalStrings()[literalNumberOf(program, ""_s)]);
    StringID letter = imageText().identifier(identifierNumberOfText("x"_s)).text();
    AOT_TEST_CHECK(strings.string(vm, empty) == vm.smallStrings.emptyString());
    AOT_TEST_CHECK(strings.createConstant(vm, empty.index()) == vm.smallStrings.emptyString());
    AOT_TEST_CHECK(strings.string(vm, letter) == vm.smallStrings.singleCharacterString('x'));
    AOT_TEST_CHECK(strings.createConstant(vm, letter.index()) == vm.smallStrings.singleCharacterString('x'));
}

static void testCreatedConstantIsFresh(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    uint32_t number = constantNumberOf("first constant"_s);
    StringID text = imageText().stringConstant(number);

    JSString* first = strings.createConstant(vm, text.index());
    JSString* second = strings.createConstant(vm, text.index());
    AOT_TEST_CHECK(first != second);
    AOT_TEST_CHECK(hasText(program, first, "first constant"_s) && hasText(program, second, "first constant"_s));
    AOT_TEST_CHECK(!strings.constantIfCreated(number));

    JSString* stored = strings.constant(vm, number);
    AOT_TEST_CHECK(stored != first && stored != second);
    JSString* byText = strings.string(vm, text);
    AOT_TEST_CHECK(byText != stored && hasText(program, byText, "first constant"_s));
}

static void testLiteralStringWords(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    for (const String& text : { String("red"_s), String("green"_s), japanese(2) }) {
        uint64_t word = literalStringWord(literalNumberOf(program, text));
        AOT_TEST_CHECK(isLiteralStringWord(word));
        AOT_TEST_CHECK(strings.literalStringWordFor(jsString(vm, text.isolatedCopy())) == word);
        AOT_TEST_CHECK(strings.literalStringWordFor(jsString(vm, text.isolatedCopy())) == word);

        JSString* cell = strings.stringForLiteralStringWord(word);
        AOT_TEST_CHECK(hasText(program, cell, text));
        AOT_TEST_CHECK(strings.stringForLiteralStringWord(word) == cell);
        AOT_TEST_CHECK(strings.literalStringWordFor(cell) == word);
        AOT_TEST_CHECK(strings.literalStringIfCreated(literalStringIndex(static_cast<uint32_t>(word))) == cell);
    }
    AOT_TEST_CHECK(literalNumberOf(program, "red"_s) != literalNumberOf(program, "green"_s));
    AOT_TEST_CHECK(strings.literalStringWordFor(jsEmptyString(vm)) == emptyLiteralStringWord);
    AOT_TEST_CHECK(isLiteralStringWord(emptyLiteralStringWord));
}

static uint32_t numberOfLiteralWithoutText(LoadedProgram& program, StringView text)
{
    for (const LiteralStringDigest& digest : program.state().tables.literalStringDigests()) {
        if (digest.length == text.length() && digest.digest == digestOfLiteralString(text))
            return digest.number;
    }
    failNativeTest(__FILE__, __LINE__, "the image has the digest of the text");
}

// Three members of the field's type are spelled by no code.
static void testFirstStringFromOutsideLendsItsText(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    constexpr std::array<char16_t, 6> violetIn16Bits { 'v', 'i', 'o', 'l', 'e', 't' };
    constexpr std::array<char16_t, 2> autumnLeaves { 0x7d05, 0x8449 };
    for (const String& text : { String("amber"_s), String(std::span { violetIn16Bits }), String(std::span { autumnLeaves }) }) {
        uint32_t number = numberOfLiteralWithoutText(program, text);
        uint64_t word = literalStringWord(number);
        AOT_TEST_CHECK(!imageText().hasText(StringID(program.state().tables.literalStrings()[number])));
        AOT_TEST_CHECK(!strings.literalStringIfCreated(number));

        JSString* other = jsString(vm, makeString(StringView(text).substring(1), 'x'));
        AOT_TEST_CHECK(other->length() == text.length() && strings.literalStringWordFor(other) == std::bit_cast<uint64_t>(other));
        AOT_TEST_CHECK(!strings.literalStringIfCreated(number));

        JSString* first = jsString(vm, text.isolatedCopy());
        AOT_TEST_CHECK(strings.literalStringWordFor(first) == word);
        JSString* lent = strings.literalStringIfCreated(number);
        AOT_TEST_CHECK(lent && lent != first && lent->tryGetValueImpl() != first->tryGetValueImpl() && hasText(program, lent, text));
        AOT_TEST_CHECK(strings.stringForLiteralStringWord(word) == lent && strings.literalStringWordFor(lent) == word);
        AOT_TEST_CHECK(strings.literalStringWordFor(jsString(vm, text.isolatedCopy())) == word);
        AOT_TEST_CHECK(strings.literalStringWordFor(other) == std::bit_cast<uint64_t>(other));
        AOT_TEST_CHECK(strings.literalStringIfCreated(number) == lent);
    }
    AOT_TEST_CHECK(strings.literalStringWordFor(jsString(vm, String("violet"_s))) == literalStringWord(numberOfLiteralWithoutText(program, "violet"_s)));
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(hasText(program, strings.literalStringIfCreated(numberOfLiteralWithoutText(program, "amber"_s)), "amber"_s));
}

static void testLiteralStringWordsInHeaders(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    for (const String& text : { String("red"_s), String("green"_s), japanese(2) }) {
        uint32_t word = literalStringWord(literalNumberOf(program, text));
        const StringImpl* header = strings.stringForLiteralStringWord(word)->tryGetValueImpl();
        AOT_TEST_CHECK(header && imageText().literalStringWordOfHeader(*header) == word);
        AOT_TEST_CHECK(!imageText().literalStringWordOfHeader(*text.isolatedCopy().impl()));
    }
    // The header of a constant that is no literal string is made with its cell, outside of the image.
    JSString* cell = strings.constant(vm, constantNumberOf("first constant"_s));
    const StringImpl* other = cell->tryGetValueImpl();
    AOT_TEST_CHECK(other && !imageText().literalStringWordOfHeader(*other));
    AOT_TEST_CHECK(strings.literalStringWordFor(cell) == std::bit_cast<uint64_t>(cell));
}

// In the first instance of the process the header is as one of the image: static, with its hash, over the characters in the image. There is one for each constant made.
static void testConstantGetsHeader(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    const ConstantCellWords& constants = program.instance().runtime.constants;
    AOT_TEST_CHECK(constants.sizeOfStringHeaders && constants.nextStringHeader == constants.startOfStringHeaders);
    unsigned numberMade = 0;
    for (const String& text : { String("first constant"_s), String("second constant"_s), japanese(3) }) {
        uint32_t number = constantNumberOf(text);
        AOT_TEST_CHECK(imageText().hasListedHash(imageText().stringConstant(number)));
        const StringImpl* header = strings.constant(vm, number)->tryGetValueImpl();
        AOT_TEST_CHECK(header && header->isStatic() && header->hasHash() && header->existingHash() == text.isolatedCopy().impl()->hash());
        AOT_TEST_CHECK(StringView(*header) == text && header->is8Bit() == text.is8Bit() && Image::current()->containsCharactersOf(*header));
        AOT_TEST_CHECK(std::bit_cast<const uint8_t*>(header) == constants.startOfStringHeaders + numberMade++ * sizeof(ImageStringHeader));
        AOT_TEST_CHECK(strings.constant(vm, number)->tryGetValueImpl() == header);
    }
    AOT_TEST_CHECK(constants.nextStringHeader == constants.startOfStringHeaders + numberMade * sizeof(ImageStringHeader));
}

static void testLiteralStringWordOfRope(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    JSString* rope = jsString(program.realm(), jsString(vm, String("gre"_s)), jsString(vm, String("en"_s)));
    AOT_TEST_CHECK(rope->isRope() && !rope->isSubstring());
    AOT_TEST_CHECK(strings.literalStringWordFor(rope) == literalStringWord(literalNumberOf(program, "green"_s)));

    JSString* wideRope = jsString(program.realm(), jsString(vm, japanese(1)), jsString(vm, japanese(2).substring(1)));
    AOT_TEST_CHECK(wideRope->isRope() && !wideRope->is8Bit());
    AOT_TEST_CHECK(strings.literalStringWordFor(wideRope) == literalStringWord(literalNumberOf(program, japanese(2))));

    JSString* otherRope = jsString(program.realm(), jsString(vm, String("gre"_s)), jsString(vm, String("y"_s)));
    AOT_TEST_CHECK(otherRope->isRope());
    AOT_TEST_CHECK(strings.literalStringWordFor(otherRope) == std::bit_cast<uint64_t>(otherRope));
}

static void testLiteralStringWordOfSubstring(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    JSString* base = jsString(vm, String("the light is green or red today"_s));
    JSString* green = jsSubstring(program.realm(), base, 13, 5);
    JSString* red = jsSubstring(program.realm(), base, 22, 3);
    JSString* light = jsSubstring(program.realm(), base, 4, 5);
    for (JSString* substring : { green, red, light })
        AOT_TEST_CHECK(substring->isSubstring());
    AOT_TEST_CHECK(strings.literalStringWordFor(green) == literalStringWord(literalNumberOf(program, "green"_s)));
    AOT_TEST_CHECK(strings.literalStringWordFor(red) == literalStringWord(literalNumberOf(program, "red"_s)));
    AOT_TEST_CHECK(strings.literalStringWordFor(light) == std::bit_cast<uint64_t>(light));
}

static void testStringThatIsNoLiteral(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    std::span<const LiteralStringLimits> limits = program.state().tables.literalStringLimits();
    AOT_TEST_CHECK(limits.size() == 1 && limits[0].maxLength >= 5);

    for (ASCIILiteral text : { "blue"_s, "Red"_s, "re"_s, "redd"_s, "first constant"_s, "a text that is longer than every literal string of the program"_s }) {
        JSString* string = jsString(vm, String(text));
        uint64_t word = strings.literalStringWordFor(string);
        AOT_TEST_CHECK(word == std::bit_cast<uint64_t>(string) && !isLiteralStringWord(word));
        AOT_TEST_CHECK(strings.literalStringWordFor(string) == word);
    }
    JSString* wide = jsString(vm, japanese(3));
    AOT_TEST_CHECK(strings.literalStringWordFor(wide) == std::bit_cast<uint64_t>(wide));
}

static void testLiteralStringsSurviveCollections(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    uint64_t word = literalStringWord(literalNumberOf(program, "red"_s));
    strings.stringForLiteralStringWord(word);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        JSString* cell = strings.stringForLiteralStringWord(word);
        AOT_TEST_CHECK(vm.heap.isMarked(cell) && hasText(program, cell, "red"_s));
    }
}

static void testKeyCellOfWellKnownSymbol(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    struct Case {
        WellKnownSymbol symbol;
        ASCIILiteral expression;
    };
    static constexpr Case cases[] = {
        { WellKnownSymbol::Iterator, "Symbol.iterator"_s },
        { WellKnownSymbol::HasInstance, "Symbol.hasInstance"_s },
    };
    for (const Case& testCase : cases) {
        uint32_t number = keyCellNumberOf(program, identifierNumberOf([&](const ImageIdentifierWord& record) {
            return record.kind() == PropertyKeyKind::WellKnownSymbol && record.wellKnownSymbol() == testCase.symbol;
        }));
        JSCell* cell = strings.keyCell(vm, number);
        AOT_TEST_CHECK(cell->isSymbol());
        AOT_TEST_CHECK(JSValue(cell) == program.realms().evaluate(program.realm(), testCase.expression));
        AOT_TEST_CHECK(JSValue(cell) == program.realms().evaluate(program.realms().plain(), testCase.expression));
        AOT_TEST_CHECK(strings.keyCell(vm, number) == cell);
        AOT_TEST_CHECK(strings.keyCellIfCreated(number) == cell);
    }
}

static void testKeyCellOfText(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    // No operation hands the name of the field over as a value.
    AOT_TEST_CHECK(!std::ranges::contains(program.state().tables.keyCellIdentifiers(), identifierNumberOfText("color"_s)));

    uint32_t number = keyCellNumberOf(program, identifierNumberOfText("hue"_s));
    AOT_TEST_CHECK(!strings.keyCellIfCreated(number));
    JSCell* cell = strings.keyCell(vm, number);
    AOT_TEST_CHECK(cell->isString() && hasText(program, uncheckedDowncast<JSString>(cell), "hue"_s));
    AOT_TEST_CHECK(uncheckedDowncast<JSString>(cell)->tryGetValueImpl() == Identifier::fromString(vm, "hue"_s).impl());
    AOT_TEST_CHECK(strings.keyCell(vm, number) == cell);
    AOT_TEST_CHECK(strings.keyCellIfCreated(number) == cell);

    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        AOT_TEST_CHECK(vm.heap.isMarked(cell) && strings.keyCell(vm, number) == cell);
    }
}

static void testBigIntConstant(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
    AOT_TEST_CHECK(program.state().tables.bigIntConstants().size() == 1);
    AOT_TEST_CHECK(!program.instance().runtime.constants.bigIntCells[0]);

    JSValue constant = strings.bigIntConstant(program.realm(), 0);
    AOT_TEST_CHECK(constant.isHeapBigInt());
    AOT_TEST_CHECK(constant.asHeapBigInt()->toString(program.realm(), 10) == "-123456789012345678901234567890"_s);
    AOT_TEST_CHECK(strings.bigIntConstant(program.realm(), 0) == constant);
    AOT_TEST_CHECK(program.instance().runtime.constants.bigIntCells[0] == constant.asCell());

    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.isMarked(constant.asCell()) && strings.bigIntConstant(program.realm(), 0) == constant);
}

static void testTextWithoutCells(LoadedProgram& program)
{
    const Image& image = *Image::current();
    const ProgramTables& tables = program.state().tables;
    StringID narrow = imageText().stringConstant(constantNumberOf("first constant"_s));
    StringID wide = imageText().stringConstant(constantNumberOf(japanese(3)));

    AOT_TEST_CHECK(ProgramStrings::text(image, narrow) == "first constant"_s && ProgramStrings::text(tables, narrow) == "first constant"_s);
    AOT_TEST_CHECK(ProgramStrings::text(image, wide) == japanese(3));
    AOT_TEST_CHECK(ProgramStrings::viewInImage(tables, wide) == japanese(3) && !ProgramStrings::viewInImage(tables, wide).is8Bit());
    AOT_TEST_CHECK(ProgramStrings::hasText(image, narrow, "first constant"_s) && !ProgramStrings::hasText(image, narrow, "first constan"_s));
    AOT_TEST_CHECK(ProgramStrings::hasText(image, wide, japanese(3)) && !ProgramStrings::hasText(image, wide, japanese(2)));
    AOT_TEST_CHECK(ProgramStrings::atomOf(image, narrow) == AtomString("first constant"_s));

    uint32_t color = identifierNumberOf([&](const ImageIdentifierWord& record) {
        return record.kind() == PropertyKeyKind::Text && record.text() && imageText().equals(record.text(), "color"_s);
    });
    AOT_TEST_CHECK(ProgramStrings::text(tables, ProgramStrings::identifierString(tables, color)) == "color"_s);
}

template<const ASCIILiteral& referencesWord, LoadedProgramTest body>
static void runNativeTestWithReferences()
{
    for (const NativeTest& test : programStringsTests()) {
        if (test.run == &runNativeTestWithReferences<referencesWord, body>) {
            runWithReferences(test.name, referencesWord, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ASCIILiteral narrowReferences = "references=narrow"_s;
static constexpr ASCIILiteral wideReferences = "references=wide"_s;

static void checkReferencesAreNarrow(LoadedProgram& program)
{
    AOT_TEST_CHECK(!usesWideReferences());
    testConstantIsCreatedOnce(program);
}

static void checkReferencesAreWide(LoadedProgram& program)
{
    AOT_TEST_CHECK(usesWideReferences());
    testConstantIsCreatedOnce(program);
}

template<LoadedProgramTest body> static constexpr void (*narrow)() = runNativeTestWithReferences<narrowReferences, body>;
template<LoadedProgramTest body> static constexpr void (*wide)() = runNativeTestWithReferences<wideReferences, body>;

std::span<const NativeTest> programStringsTests()
{
    static constexpr NativeTest tests[] = {
        { "program-strings/constant-is-created-once"_s, narrow<checkReferencesAreNarrow> },
        { "program-strings/constant-survives-collections"_s, narrow<testConstantSurvivesCollections> },
        { "program-strings/text-in-16-bits"_s, narrow<testTextIn16Bits> },
        { "program-strings/small-strings-are-the-engines"_s, narrow<testSmallStringsAreTheEngines> },
        { "program-strings/created-constant-is-fresh"_s, narrow<testCreatedConstantIsFresh> },
        { "program-strings/literal-string-words"_s, narrow<testLiteralStringWords> },
        { "program-strings/first-string-from-outside-lends-its-text"_s, narrow<testFirstStringFromOutsideLendsItsText> },
        { "program-strings/literal-string-words-in-headers"_s, narrow<testLiteralStringWordsInHeaders> },
        { "program-strings/constant-gets-header"_s, narrow<testConstantGetsHeader> },
        { "program-strings/literal-string-word-of-rope"_s, narrow<testLiteralStringWordOfRope> },
        { "program-strings/literal-string-word-of-substring"_s, narrow<testLiteralStringWordOfSubstring> },
        { "program-strings/string-that-is-no-literal"_s, narrow<testStringThatIsNoLiteral> },
        { "program-strings/literal-strings-survive-collections"_s, narrow<testLiteralStringsSurviveCollections> },
        { "program-strings/key-cell-of-well-known-symbol"_s, narrow<testKeyCellOfWellKnownSymbol> },
        { "program-strings/key-cell-of-text"_s, narrow<testKeyCellOfText> },
        { "program-strings/big-int-constant"_s, narrow<testBigIntConstant> },
        { "program-strings/text-without-cells"_s, narrow<testTextWithoutCells> },
        { "program-strings/wide-references/constant-is-created-once"_s, wide<checkReferencesAreWide> },
        { "program-strings/wide-references/constant-survives-collections"_s, wide<testConstantSurvivesCollections> },
        { "program-strings/wide-references/small-strings-are-the-engines"_s, wide<testSmallStringsAreTheEngines> },
        { "program-strings/wide-references/literal-string-words"_s, wide<testLiteralStringWords> },
        { "program-strings/wide-references/first-string-from-outside-lends-its-text"_s, wide<testFirstStringFromOutsideLendsItsText> },
        { "program-strings/wide-references/key-cell-of-text"_s, wide<testKeyCellOfText> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
