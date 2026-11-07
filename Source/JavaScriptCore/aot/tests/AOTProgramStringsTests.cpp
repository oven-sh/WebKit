/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTProgramStrings.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Symbol.h"
#include <algorithm>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> programStringsTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Light" fields=[{key="color" declared={type="red"|"green"|"\u{65e5}\u{672c}"} flags=[IsRequired]}, {key="x" declared={type=number}}]
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

static const ImageText& imageText()
{
    return Image::current()->text();
}

static String japanese()
{
    static constexpr char16_t characters[] = { 0x65e5, 0x672c, 0x8a9e };
    return String(std::span(characters));
}

static uint32_t constantNumberOf(StringView wanted)
{
    for (uint32_t number = 0; number < imageText().numberOfStringConstants(); ++number) {
        if (ImageStringWord word = imageText().stringConstant(number); word.hasText() && imageText().view(word) == wanted)
            return number;
    }
    failNativeTest(__FILE__, __LINE__, "the text is a string constant of the image");
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
    uint32_t wide = constantNumberOf(japanese());

    createConstants(program, { first, wide });
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Eden, CollectionScope::Full, CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        for (uint32_t number : { first, wide }) {
            JSString* cell = strings.constantIfCreated(number);
            AOT_TEST_CHECK(cell && vm.heap.isMarked(cell) && cell->isString());
        }
        AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(first), "first constant"_s));
        AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(wide), japanese()));
    }

    createConstants(program, { second });
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.isMarked(strings.constantIfCreated(second)));
    AOT_TEST_CHECK(hasText(program, strings.constantIfCreated(second), "second constant"_s));
    AOT_TEST_CHECK(vm.heap.isMarked(strings.constantIfCreated(first)));
}

static void testKeyCellOfText(LoadedProgram& program)
{
    VM& vm = program.vm();
    ProgramStrings& strings = *program.state().strings;
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

template<LoadedProgramTest body>
static void runNativeTestWithWideReferences()
{
    for (const NativeTest& test : programStringsTests()) {
        if (test.run == &runNativeTestWithWideReferences<body>) {
            runWithLoadedProgram(test.name, programText, body, { "references=wide"_s });
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> programStringsTests()
{
    static constexpr NativeTest tests[] = {
        { "program-strings/wide-references/constant-survives-collections"_s, runNativeTestWithWideReferences<testConstantSurvivesCollections> },
        { "program-strings/wide-references/key-cell-of-text"_s, runNativeTestWithWideReferences<testKeyCellOfText> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
