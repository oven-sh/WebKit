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
    std::optional<WordOfGetterForSiteAnswers> wordAtCall = findWordOfGetterForSiteAnswers(SiteUse::Call, slot, cageBaseForSiteAnswersOfGettersTests, false);
    AOT_TEST_CHECK(wordAtCall && wordAtCall->form == word->form && wordAtCall->operand == word->operand);
}

static void testWhatNoOperandNamesIsNotRemembered()
{
    Slot slot = getterAboveForSiteAnswersOfGettersTests(bytesToHolderForSiteAnswersOfGettersTests, 3, 0);
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::AbsenceAmongExtraProperties, slot, cageBaseForSiteAnswersOfGettersTests, false));
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Call, slot, cageBaseForSiteAnswersOfGettersTests, true));
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, slot, cageBaseForSiteAnswersOfGettersTests, true));
    Slot farAway = getterAboveForSiteAnswersOfGettersTests(static_cast<uintptr_t>(UINT32_MAX) * sizeof(EncodedJSValue), 3, 0);
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, farAway, cageBaseForSiteAnswersOfGettersTests, false));
    Slot withoutHolder = slot;
    withoutHolder.holder = nullptr;
    AOT_TEST_CHECK(!findWordOfGetterForSiteAnswers(SiteUse::Read, withoutHolder, cageBaseForSiteAnswersOfGettersTests, false));
}

static void testStructureIDWithTopBitIsRemembered()
{
    using Form = SiteAnswers::Form;
    for (uint32_t bits : { 0x00004040u, 0x40004040u, 0x80004040u, 0xc0004040u, 0xe0004040u, 0xf8004040u, 0xfe004040u }) {
        for (SiteUse use : { SiteUse::Read, SiteUse::Call, SiteUse::AbsenceAmongExtraProperties }) {
            std::optional<SiteAnswers::Answer> answer = SiteAnswers::tryByStructure(use, std::bit_cast<StructureID>(bits), Form::OwnValueInCell, 16);
            AOT_TEST_CHECK(answer && answer->isByStructure() && !answer->isByShape());
            AOT_TEST_CHECK(answer->structureID().bits() == bits && SiteAnswers::formOf(use, answer->key) == Form::OwnValueInCell);
        }
    }
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
        { "site-answers-of-getters/what-no-operand-names-is-not-remembered"_s, testWhatNoOperandNamesIsNotRemembered },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
