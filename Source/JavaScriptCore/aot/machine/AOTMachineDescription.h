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

void describeConstants(const Procedure&, decltype(CodeBody::constants)&);
void describeJumpTables(const Procedure&, const LinkBuffer&, decltype(CodeBody::jumpTables)&);
void describeEntries(const Procedure&, const LinkBuffer&, Vector<CodeEntry, 1>&);
void describeFrame(const Procedure&, const LinkBuffer&, FrameDescription&);

struct LinkedTrappingAccess {
    uint32_t codeOffset;
    Origin origin;
    GPRReg reference;
    bool referenceIsNarrow;
};
void describeTrappingAccesses(const Procedure&, const LinkBuffer&, Vector<LinkedTrappingAccess>&);

enum class CodeOfBackEnd : uint8_t { Move, Reload, Save, Materialize, Swap, ClearSpill, KeepPointer, StoreBehindDefinition, FrameSetup, FrameTeardown, StoreToOutgoingWord, LoadFromOutgoingWord };
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
