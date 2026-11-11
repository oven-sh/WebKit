/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTRuntimeRecords.h"
#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static_assert(static_cast<uint32_t>(SiteAnswers::Form::GetterInStorageOfHolder) < static_cast<uint32_t>(SiteAnswers::Form::Nothing) && static_cast<uint32_t>(SiteAnswers::Form::Nothing) <= SiteAnswers::formMask);

struct WordOfGetterForSiteAnswers {
    SiteAnswers::Form form;
    uint32_t operand;
    int32_t indexInStorageOfHolder { 0 };
};

inline std::optional<WordOfGetterForSiteAnswers> findWordOfGetterForSiteAnswers(SiteUse use, const Slot& slot, uintptr_t cageBase, bool usesWideReferences)
{
    using Form = SiteAnswers::Form;
    static_assert(Slot::payloadOf(Slot::Kind::Getter) == SlotPayload::Holder && Slot::locationOf(Slot::Kind::Getter) == SlotLocation::WordIndex);
    if (!isRead(use) && use != SiteUse::Call)
        return std::nullopt;
    int32_t wordIndex = slot.wordIndex();
    if (slot.has(Slot::isOwn)) {
        if (slot.has(Slot::isOutOfLine))
            return WordOfGetterForSiteAnswers { Form::OwnGetterInStorage, static_cast<uint32_t>(wordIndex) };
        return WordOfGetterForSiteAnswers { Form::OwnGetterInCell, static_cast<uint32_t>(static_cast<uint32_t>(wordIndex) * sizeof(EncodedJSValue)) };
    }
    if (!slot.holder || usesWideReferences)
        return std::nullopt;
    uintptr_t wordsToHolder = (std::bit_cast<uintptr_t>(slot.holder) - cageBase) / sizeof(EncodedJSValue);
    if (wordsToHolder + static_cast<uint32_t>(std::max(wordIndex, 0)) > UINT32_MAX)
        return std::nullopt;
    if (slot.has(Slot::isOutOfLine))
        return WordOfGetterForSiteAnswers { Form::GetterInStorageOfHolder, static_cast<uint32_t>(wordsToHolder), wordIndex };
    return WordOfGetterForSiteAnswers { Form::GetterInWordOfCage, static_cast<uint32_t>(wordsToHolder) + static_cast<uint32_t>(wordIndex) };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
