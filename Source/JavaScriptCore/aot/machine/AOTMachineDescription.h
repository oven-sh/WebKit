/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTMachine.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT { namespace Machine {

void describeListedConstants(const Procedure&, decltype(RareListsOfBody::constants)&);
inline void describeConstants(const Procedure& procedure, decltype(RareListsOfBody::constants)& constants)
{
    RELEASE_ASSERT(constants.isEmpty());
    if (!procedure.constantData().empty())
        describeListedConstants(procedure, constants);
}

void describeListedJumpTables(const Procedure&, const LinkBuffer&, decltype(RareListsOfBody::jumpTables)&);
inline void describeJumpTables(const Procedure& procedure, const LinkBuffer& linkBuffer, decltype(RareListsOfBody::jumpTables)& jumpTables)
{
    RELEASE_ASSERT(jumpTables.isEmpty());
    if (!procedure.jumpTables().empty())
        describeListedJumpTables(procedure, linkBuffer, jumpTables);
}

inline uint32_t describedOffsetOf(const LinkBuffer& linkBuffer, MacroAssembler::Label label)
{
    RELEASE_ASSERT(label.isSet());
    uint32_t offset = const_cast<LinkBuffer&>(linkBuffer).offsetOf(label);
    RELEASE_ASSERT(offset <= linkBuffer.size(), offset, linkBuffer.size());
    return offset;
}

inline void describeEntries(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<CodeEntry, 1>& entries)
{
    std::span<const Entrypoint> entrypoints = procedure.entrypoints();
    if (entries.size() + entrypoints.size() > entries.capacity())
        entries.reserveCapacity(entries.size() + entrypoints.size());
    for (const Entrypoint& entrypoint : entrypoints)
        entries.append(CodeEntry { describedOffsetOf(linkBuffer, entrypoint.label), entrypoint.id });
}

void describeFrame(const Procedure&, const LinkBuffer&, FrameDescription&);

struct LinkedTrappingAccess {
    uint32_t codeOffset;
    Origin origin;
    GPRReg reference;
    bool referenceIsNarrow;
};
void describeListedTrappingAccesses(const Procedure&, const LinkBuffer&, Vector<LinkedTrappingAccess>&);
inline void describeTrappingAccesses(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<LinkedTrappingAccess>& accesses)
{
    if (!procedure.trappingAccesses().empty())
        describeListedTrappingAccesses(procedure, linkBuffer, accesses);
}

enum class CodeOfBackEnd : uint8_t { Move, Reload, Save, Materialize, Swap, KeepPointer, StoreBehindDefinition, FrameSetup, FrameTeardown, StoreToOutgoingWord, LoadFromOutgoingWord };
enum class PlaceOfCodeOfBackEnd : uint8_t { Nowhere, Operation, Patch, Check, Branch, Return, LastPatch, Edge };

inline constexpr uint32_t firstOriginOfBackEnd = 0xFFFFFF00;
constexpr Origin originOf(CodeOfBackEnd code, PlaceOfCodeOfBackEnd place) { return Origin(firstOriginOfBackEnd | static_cast<uint32_t>(place) << 4 | static_cast<uint32_t>(code)); }
constexpr bool isOriginOfBackEnd(Origin origin) { return origin.bits() >= firstOriginOfBackEnd; }
constexpr CodeOfBackEnd codeOfBackEndAt(Origin origin) { return static_cast<CodeOfBackEnd>(origin.bits() & 15); }
constexpr PlaceOfCodeOfBackEnd placeOfCodeOfBackEndAt(Origin origin) { return static_cast<PlaceOfCodeOfBackEnd>(origin.bits() >> 4 & 15); }

struct LinkedRangeOfOrigin {
    uint32_t begin;
    uint32_t end;
    Origin origin;
    uint32_t operation { UINT32_MAX };
};
void describeRangesOfOrigins(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfOrigin>&);
void describeRangesOfOperations(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfOrigin>&);

struct LinkedRangeOfRareCode {
    uint32_t begin;
    uint32_t end;
};
void describeRareCode(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfRareCode>&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
