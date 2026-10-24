/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRecords.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTRuntimeCounters.h"
#include "AOTVector.h"
#include "VM.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

RegExpRecords::RegExpRecords(const Image& image)
    : m_records(image.section<ImageRegExp>(ImageSection::RegExps))
    , m_hashes(image.section<ImageRegExpHash>(ImageSection::RegExpHashes))
    , m_details(image.section<uint8_t>(ImageSection::RegExpDetails))
    , m_code(image.regExpCode())
{
}

RegExpEntrypoints RegExpRecords::entrypointsOf(const ImageRegExp& record) const
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Code);
    return { m_code.subspan(record.codeFor8Bit).data(), m_code.subspan(record.codeFor16Bit).data() };
}

Yarr::CharacterSetView RegExpRecords::characterSetOf(const ImageRegExp& record) const
{
    RELEASE_ASSERT(record.hasCharacterSet());
    size_t offsetOfRanges = record.details + sizeof(ImageRegExpCharacterSet);
    const auto& set = spanReinterpretCast<const ImageRegExpCharacterSet>(m_details.subspan(record.details, sizeof(ImageRegExpCharacterSet)))[0];
    auto wideRanges = spanReinterpretCast<const Yarr::CharacterRange>(m_details.subspan(offsetOfRanges, set.bits.wideRangeCount * sizeof(Yarr::CharacterRange)));
    return Yarr::CharacterSetView(set.bits, wideRanges);
}

std::span<const ImageRegExpHash> RegExpRecords::recordsWithHash(uint32_t hash) const
{
    return std::ranges::equal_range(m_hashes, hash, { }, &ImageRegExpHash::hash);
}

RegExpNamedGroupWords RegExpRecords::namedGroupWordsOf(const ImageRegExp& record) const
{
    RELEASE_ASSERT(record.hasNamedGroups);
    size_t offsetOfWords = record.details + sizeof(ImageRegExpNamedGroups);
    const auto& header = spanReinterpretCast<const ImageRegExpNamedGroups>(m_details.subspan(record.details, sizeof(ImageRegExpNamedGroups)))[0];
    return { header, spanReinterpretCast<const uint32_t>(m_details.subspan(offsetOfWords, header.numberOfWords * sizeof(uint32_t))) };
}

const ImageRegExp* findRegExpRecord(VM& vm, const String& pattern, OptionSet<Yarr::Flags> flags)
{
    if (!vm.hasCompiledCode())
        return nullptr;

    const Image& image = *vm.aotState().image;
    RegExpRecords records(image);
    uint16_t rawFlags = ImageRegExp::significantFlags(flags).toRaw();
    for (const ImageRegExpHash& entry : records.recordsWithHash(ImageRegExp::hashOf(pattern.hash(), flags))) {
        const ImageRegExp& record = records.at(RegExpRecordID(entry.record));
        if (record.rawFlags == rawFlags && image.text().equals(StringID(record.pattern), pattern))
            return &record;
    }
    count(vm, RuntimeCounterID::RegExpsCreatedWithoutRecord);
    return nullptr;
}

RegExpEntrypoints regExpEntrypointsOf(VM& vm, const ImageRegExp& record)
{
    return RegExpRecords(*vm.aotState().image).entrypointsOf(record);
}

Yarr::CharacterSetView regExpCharacterSetOf(VM& vm, const ImageRegExp& record)
{
    return RegExpRecords(*vm.aotState().image).characterSetOf(record);
}

String regExpPatternOf(VM& vm, const ImageRegExp& record)
{
    return vm.aotState().image->text().createString(StringID(record.pattern));
}

String regExpAtomOf(VM& vm, const ImageRegExp& record)
{
    RELEASE_ASSERT(record.form() == RegExpRecordForm::Atom);
    return vm.aotState().image->text().createString(StringID(record.atom));
}

RegExpNamedGroups decodeRegExpNamedGroups(VM& vm, const ImageRegExp& record)
{
    const Image& image = *vm.aotState().image;
    const ImageText& text = image.text();
    RegExpNamedGroupWords encoded = RegExpRecords(image).namedGroupWordsOf(record);
    std::span<const uint32_t> words = encoded.words;

    RegExpNamedGroups groups;
    groups.numDuplicateNamedCaptureGroups = encoded.header.numDuplicateNamedCaptureGroups;
    groups.captureGroupNames = FixedVector<AtomString>(encoded.header.numberOfCaptureGroupNames);
    for (AtomString& name : groups.captureGroupNames) {
        if (uint32_t stringPlusOne = consume(words))
            name = AtomString(text.createAtom(StringID(stringPlusOne - 1)));
    }
    for (uint32_t group = 0; group < encoded.header.numberOfNamedGroups; ++group) {
        String name = text.createAtom(StringID(consume(words)));
        uint32_t numberOfIndices = consume(words);
        groups.namedGroupToParenIndices.add(WTF::move(name), Vector<unsigned>(consumeSpan(words, numberOfIndices)));
    }
    RELEASE_ASSERT(words.empty());
    return groups;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
