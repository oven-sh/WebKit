/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
#include <array>
#include <concepts>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

template<typename Kind>
struct EntryNamingRecordZeroEverywhere {
    uint32_t payloadWordAt(unsigned) const { return 0; }
    std::span<const uint32_t> payloadListFrom(unsigned) const LIFETIME_BOUND { return words; }

    Kind opcode;
    Kind kind;
    std::array<uint32_t, 60> words { };
};

template<typename Set, typename Kind, typename... References>
Set findKindsNaming(const auto& walk)
{
    Set result;
    for (Kind kind : allMembersOf<Kind>) {
        const EntryNamingRecordZeroEverywhere<Kind> entry { kind, kind };
        auto note = [&]<typename Reference, typename... Role>(const Reference&, Role...) {
            if constexpr ((std::same_as<Reference, References> || ...))
                result.add(kind);
        };
        walk(entry, note);
    }
    return result;
}

template<typename... References>
OpcodeSet findOpcodesNaming()
{
    return findKindsNaming<OpcodeSet, Opcode, References...>([](const auto& entry, auto& note) { walkValue(entry, note); });
}

template<typename... References>
TerminatorKindSet findTerminatorKindsNaming()
{
    return findKindsNaming<TerminatorKindSet, TerminatorKind, References...>([](const auto& entry, auto& note) { walkTerminator(entry, note); });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
