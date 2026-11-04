/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTSiteAnswersOfGetters.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> siteAnswersOfGettersTests();

static constexpr uintptr_t cageBaseForSiteAnswersOfGettersTests = 1ull << 36;
static constexpr uintptr_t bytesToHolderForSiteAnswersOfGettersTests = 4096;

// What the tables hold for `leaf.size`, where an object above the leaf has the accessor. No word of the holder is read.
static Slot getterAboveForSiteAnswersOfGettersTests(uintptr_t bytesToHolder, int16_t wordIndex, uint8_t flags)
{
    Slot slot = Slot::create(StructureID(), Slot::Kind::Getter, flags | Slot::dependsOnChain, static_cast<uint16_t>(wordIndex));
    slot.holder = std::bit_cast<JSCell*>(cageBaseForSiteAnswersOfGettersTests + bytesToHolder);
    return slot;
}

static void testGetterInCellOfHolderIsNamedByItsWordInCage()
{
    Slot slot = getterAboveForSiteAnswersOfGettersTests(bytesToHolderForSiteAnswersOfGettersTests, 3, 0);
    std::optional<WordOfGetterForSiteAnswers> word = findWordOfGetterForSiteAnswers(SiteUse::Read, slot, cageBaseForSiteAnswersOfGettersTests, false);
    AOT_TEST_CHECK(word);
    AOT_TEST_CHECK(word->form == SiteAnswers::Form::GetterInWordOfCage);
    AOT_TEST_CHECK(word->operand == bytesToHolderForSiteAnswersOfGettersTests / sizeof(EncodedJSValue) + 3);
}

static void testGetterInStorageOfHolderIsNamedByHolderAndIndex()
{
    Slot slot = getterAboveForSiteAnswersOfGettersTests(bytesToHolderForSiteAnswersOfGettersTests, -5, Slot::isOutOfLine);
    std::optional<WordOfGetterForSiteAnswers> word = findWordOfGetterForSiteAnswers(SiteUse::Read, slot, cageBaseForSiteAnswersOfGettersTests, false);
    AOT_TEST_CHECK(word);
    AOT_TEST_CHECK(word->form == SiteAnswers::Form::GetterInStorageOfHolder);
    AOT_TEST_CHECK(word->operand == bytesToHolderForSiteAnswersOfGettersTests / sizeof(EncodedJSValue));
    AOT_TEST_CHECK(word->indexInStorageOfHolder == -5);
}

static void testWhatNoOperandNamesIsNotRemembered()
{
    Slot slot = getterAboveForSiteAnswersOfGettersTests(bytesToHolderForSiteAnswersOfGettersTests, 3, 0);
    // A place that calls would take the GetterSetter for the callee.
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Call, slot, cageBaseForSiteAnswersOfGettersTests, false));
    // No reference of 32 bits names a holder then.
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, slot, cageBaseForSiteAnswersOfGettersTests, true));
    Slot farAway = getterAboveForSiteAnswersOfGettersTests(static_cast<uintptr_t>(UINT32_MAX) * sizeof(EncodedJSValue), 3, 0);
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, farAway, cageBaseForSiteAnswersOfGettersTests, false));
    Slot withoutHolder = slot;
    withoutHolder.holder = nullptr;
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, withoutHolder, cageBaseForSiteAnswersOfGettersTests, false));
}

// One comparison of a stub finds a Structure and a form, so no two forms have the same bits, and none has the bits of the form that a place which reads meets most.
static void testFormsOfGettersHaveBitsOfTheirOwn()
{
    using Form = SiteAnswers::Form;
    constexpr Form forms[] { Form::OwnGetterInCell, Form::OwnGetterInStorage, Form::GetterInWordOfCage, Form::GetterInStorageOfHolder };
    for (Form form : forms) {
        uint32_t bits = SiteAnswers::bitsOf(SiteUse::Read, form);
        AOT_TEST_CHECK(bits && bits <= SiteAnswers::formMask);
        AOT_TEST_CHECK(SiteAnswers::formOf(SiteUse::Read, bits) == form);
        AOT_TEST_CHECK(bits != SiteAnswers::bitsOf(SiteUse::Read, Form::Nothing));
    }
}

// Under a limit on address space the heap of Structures is small and begins at a multiple of its own size, so every StructureID of a process may have the top bit.
static void testStructureIDWithTopBitIsRemembered()
{
    using Form = SiteAnswers::Form;
    for (uint32_t bits : { 0x00004040u, 0x40004040u, 0x80004040u, 0xc0004040u, 0xe0004040u, 0xf8004040u, 0xfe004040u }) {
        for (SiteUse use : { SiteUse::Read, SiteUse::Call }) {
            std::optional<SiteAnswers::Answer> answer = SiteAnswers::tryByStructure(use, std::bit_cast<StructureID>(bits), Form::OwnValueInCell, 16);
            AOT_TEST_CHECK(answer && answer->isByStructure() && !answer->isByShape());
            AOT_TEST_CHECK(answer->structureID().bits() == bits && SiteAnswers::formOf(use, answer->key) == Form::OwnValueInCell);
        }
    }
    // What could be taken for a key by shape, and the key of no shape.
    for (uint32_t bits : { 0xff004040u, 0xbf004040u, 0xfd004040u, 0x80000000u })
        AOT_TEST_CHECK(!SiteAnswers::tryByStructure(SiteUse::Call, std::bit_cast<StructureID>(bits), Form::OwnValueInCell, 16));
    for (uint32_t shape : { 1u, 2u, 65536u, static_cast<uint32_t>(Number24::max) }) {
        AOT_TEST_CHECK(SiteAnswers::byShape(shapeWordOf(shape), 16).isByShape());
        AOT_TEST_CHECK(SiteAnswers::tryByShapeInExtension(shapeWordOf(shape), 0)->isByShape());
        AOT_TEST_CHECK(SiteAnswers::tryByShapeInTypedSlot(shapeWordOf(shape), SlotRepresentation::OptionalInt32, 16)->isByShape());
    }
    AOT_TEST_CHECK(SiteAnswers::withoutKey(SiteUse::Read, true).isByShape() && !SiteAnswers::withoutKey(SiteUse::Call, true).isByStructure());
}

std::span<const NativeTest> siteAnswersOfGettersTests()
{
    static constexpr NativeTest tests[] {
        { "site-answers-of-getters/a-structure-id-with-the-top-bit-is-remembered"_s, testStructureIDWithTopBitIsRemembered },
        { "site-answers-of-getters/a-getter-in-the-cell-of-a-holder-is-named-by-its-word-in-the-cage"_s, testGetterInCellOfHolderIsNamedByItsWordInCage },
        { "site-answers-of-getters/a-getter-in-the-storage-of-a-holder-is-named-by-the-holder-and-the-index"_s, testGetterInStorageOfHolderIsNamedByHolderAndIndex },
        { "site-answers-of-getters/what-no-operand-names-is-not-remembered"_s, testWhatNoOperandNamesIsNotRemembered },
        { "site-answers-of-getters/the-forms-of-getters-have-bits-of-their-own"_s, testFormsOfGettersHaveBitsOfTheirOwn },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
