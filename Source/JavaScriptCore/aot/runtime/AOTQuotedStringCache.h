/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSString.h"
#include "MarkedBlockCage.h"
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct State;

struct QuotedStringCache {
    static constexpr unsigned maxRememberedStringLength = 32;
    static constexpr size_t numberOfEntries = 512;

    static QuotedStringCache& ensure(State&);

    static ALWAYS_INLINE bool canRemember(const StringImpl& text) { return text.is8Bit() && text.length() <= maxRememberedStringLength && text.hasHash(); }

    ALWAYS_INLINE JSString* tryFind(const StringImpl& text) const
    {
        unsigned hash = text.existingHash();
        uint64_t entry = entries[hash % numberOfEntries];
        if (entry >> 32 != hash)
            return nullptr;
        JSString* quoted = uncheckedDowncast<JSString>(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)));
        const StringImpl* known = quoted->tryGetValueImpl();
        if (!known || !known->is8Bit() || known->length() != text.length() + 2)
            return nullptr;
        return equal(known->span8().data() + 1, text.span8()) ? quoted : nullptr;
    }
    void remember(const StringImpl& text, const JSString* quoted);

    uint64_t entries[numberOfEntries];
    uintptr_t cageBase;
};
static_assert(std::is_trivially_default_constructible_v<QuotedStringCache>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
