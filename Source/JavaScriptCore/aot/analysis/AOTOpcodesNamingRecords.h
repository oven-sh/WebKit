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
    // As long as a whole number of records of 2, 3, 4 or 5 words.
    std::span<const uint32_t> payloadListFrom(unsigned) const LIFETIME_BOUND { return words; }

    Kind opcode;
    Kind kind;
    std::array<uint32_t, 60> words { };
};

template<typename... References>
OpcodeSet findOpcodesNaming()
{
    OpcodeSet result;
    for (Opcode opcode : allMembersOf<Opcode>) {
        const EntryNamingRecordZeroEverywhere<Opcode> operation { opcode, opcode };
        auto note = [&]<typename Reference, typename... Role>(const Reference&, Role...) {
            if constexpr ((std::same_as<Reference, References> || ...))
                result.add(opcode);
        };
        walkValue(operation, note);
    }
    return result;
}

template<typename... References>
TerminatorKindSet findTerminatorKindsNaming()
{
    TerminatorKindSet result;
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        const EntryNamingRecordZeroEverywhere<TerminatorKind> terminator { kind, kind };
        auto note = [&]<typename Reference, typename... Role>(const Reference&, Role...) {
            if constexpr ((std::same_as<Reference, References> || ...))
                result.add(kind);
        };
        walkTerminator(terminator, note);
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
