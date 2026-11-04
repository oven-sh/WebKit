/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTMachine.h"
#include "LinkBuffer.h"

// The back end's share of CompiledCode, once the code is linked. Everything else in it (bytes, relocations, callees, stub keys, sites, positions, handlers, frame maps) comes from the records of the emitters:
// codegen/AOTCodeDescription.cpp.

namespace JSC { namespace AOT { namespace Machine {

// CodeBody::constants: symbol { SymbolSpace::Constant, i } is entry i.
void describeConstants(const Procedure&, decltype(CodeBody::constants)&);
// CodeBody::jumpTables: symbol { SymbolSpace::JumpTable, i } is entry i. The targets are offsets into the code of the function.
void describeJumpTables(const Procedure&, const LinkBuffer&, decltype(CodeBody::jumpTables)&);
// CodeBody::entries, one for each entrypoint of the procedure, in the order in which they were added. The caller adds those of its preambles and sorts.
void describeEntries(const Procedure&, const LinkBuffer&, Vector<CodeEntry, 1>&);
// frameSizeInBytes, bytesAboveFrameRecord (zero), bytesDeadAtSuspensions, hasFrameRecord (true), calleeSaveRegisters, frameRanges: ascending, apart, none empty, neighbors joined.
void describeFrame(const Procedure&, const LinkBuffer&, FrameDescription&);

struct LinkedTrappingAccess {
    uint32_t codeOffset;
    Origin origin;
    GPRReg reference;
    bool referenceIsNarrow;
};
// Ascending by offset.
void describeTrappingAccesses(const Procedure&, const LinkBuffer&, Vector<LinkedTrappingAccess>&);

// What the back end writes of its own accord, for no operation that was appended.
// A Save or a Reload that names a word of the outgoing area is apart from one that names a spill: an argument or a result on the stack.
enum class CodeOfBackEnd : uint8_t { Move, Reload, Save, Materialize, Swap, ClearSpill, KeepPointer, StoreBehindDefinition, FrameSetup, FrameTeardown, StoreToOutgoingWord, LoadFromOutgoingWord };
// What it stands in front of. A Patch that ends its block is apart from one within a block.
enum class PlaceOfCodeOfBackEnd : uint8_t { Nowhere, Operation, Patch, Check, Branch, Return, LastPatch, Edge };

// Where ranges of origins are noted, such code has one of the 256 highest origins. No emitter appends under one of them.
inline constexpr uint32_t firstOriginOfBackEnd = 0xFFFFFF00;
constexpr Origin originOf(CodeOfBackEnd code, PlaceOfCodeOfBackEnd place) { return Origin(firstOriginOfBackEnd | static_cast<uint32_t>(place) << 4 | static_cast<uint32_t>(code)); }
constexpr bool isOriginOfBackEnd(Origin origin) { return origin.bits() >= firstOriginOfBackEnd; }
constexpr CodeOfBackEnd codeOfBackEndAt(Origin origin) { return static_cast<CodeOfBackEnd>(origin.bits() & 15); }
constexpr PlaceOfCodeOfBackEnd placeOfCodeOfBackEndAt(Origin origin) { return static_cast<PlaceOfCodeOfBackEnd>(origin.bits() >> 4 & 15); }

struct LinkedRangeOfOrigin {
    uint32_t begin;
    uint32_t end;
    Origin origin;
    // RangeOfOrigin::operation, of the first operation where the range holds the code of several.
    uint32_t operation { UINT32_MAX };
};
// The code of the procedure by the origin of the operation it was written for, late paths included. Ascending, apart, none empty. Nothing unless ProcedureTraits::notesRangesOfOrigins.
// Neighbors with one origin are one range, but for an origin of the back end: there each step has a range of its own.
void describeRangesOfOrigins(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfOrigin>&);
// The same code, and each operation has ranges of its own.
void describeRangesOfOperations(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfOrigin>&);

struct LinkedRangeOfRareCode {
    uint32_t begin;
    uint32_t end;
};
// The code of the rare blocks and the late paths: the rare regions that stand among the other code, and then everything from one offset on. Ascending, apart, none empty.
void describeRareCode(const Procedure&, const LinkBuffer&, Vector<LinkedRangeOfRareCode>&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
