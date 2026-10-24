/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTConstantPool.h"
#include "AOTInstanceLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTOutliner.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubBlob.h"
#include "AOTStubCalls.h"

namespace JSC { namespace AOT {

struct SharedCode {
    uint32_t thunkOffset(const ArgumentMoves&) const;

    StubBlob blob;
    Vector<const ArgumentMoves*> argumentMoves;
    ArgumentThunks argumentThunks;
    uint32_t argumentThunksOffset { 0 };
    uint32_t sharedBodiesOffset { 0 };
    uint32_t veneersOffset { 0 };
};

struct VeneerTarget {
    friend bool operator==(const VeneerTarget&, const VeneerTarget&) = default;

    SymbolSpace space;
    uint16_t entry;
    uint32_t index;
    OptionalIndex alias;
};

struct FinalJump {
    uint32_t sizeWithoutJump { 0 };
    bool jumpsToNextBody { false };
};

struct CodeLayoutInputs {
    const LinkedFunctions& functions;
    const ResolvedCallees& callees;
    std::span<const EntryAlias> aliases;
    const OutlinedCode& outlinedCode;
    const SharedCode& sharedCode;
    const RegExpSection& regExpSection;
    const CodeTemplates& templates;
    std::span<const FinalJump> finalJumps;
    unsigned codeGranuleShift;
    size_t webAssemblyCodeSize;
    size_t webAssemblyCodeAlignment;
    size_t constantsSize;
    size_t ffiCodeSize { 0 };
    size_t ffiCodeAlignment { 1 };
    size_t ffiDirectorySize { 0 };
};

struct CodeLayout {
    size_t firstVeneerOffset(uint32_t position, size_t bodySize) const { return bodyStarts[position] + WTF::roundUpToMultipleOf<sizeof(uint32_t)>(bodySize); }

    unsigned codeGranuleShift { 0 };
    Vector<uint32_t> bodyStarts;
    Vector<uint32_t> sharedCodeCopyByBody;
    Vector<uint32_t, 4> sharedCodeCopies;
    Vector<uint32_t> ownBodyOffsets;
    Vector<Vector<VeneerTarget, 0>> veneersByBody;
    Vector<VeneerTarget> sharedCodeVeneers;
    BitVector fallsIntoNext;
    uint32_t sharedCodeSize { 0 };
    uint32_t functionsEnd { 0 };
    uint32_t webAssemblyCodeOffset { 0 };
    uint32_t ffiCodeOffset { 0 };
    uint32_t ffiCodeSize { 0 };
    uint32_t regExpCodeOffset { 0 };
    uint32_t executableSize { 0 };
    uint32_t constantsOffset { 0 };
    uint32_t ffiDirectoryOffset { 0 };
    uint64_t codeSize { 0 };
};

struct CodeLinkerInputs {
    const CodeLayoutInputs& layoutInputs;
    const CodeLayout& layout;
    const Outlining& outlining;
    const ConstantPool& constants;
    InstanceSizes instanceSizes;
};

unsigned chooseCodeGranuleShift(const LinkedFunctions&, BuildContext&);
unsigned countSharedCodeCopies(const LinkedFunctions&, BuildContext&);
Vector<FinalJump> findFinalJumps(const LinkedFunctions&, const ResolvedCallees&, const OutlinedCode&, BuildContext&);
CodeLayout layOutCode(const CodeLayoutInputs&, BuildContext&);
void removeFinalJumps(LinkedFunctions&, const BitVector& fallsIntoNext, std::span<const FinalJump>, BuildContext&);
void linkCode(const CodeLinkerInputs&, std::span<uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
