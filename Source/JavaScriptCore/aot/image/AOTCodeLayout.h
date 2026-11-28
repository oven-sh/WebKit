/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTConstantPool.h"
#include "AOTLinkedFunctions.h"
#include "AOTImageText.h"
#include "AOTNumbering.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubBlob.h"
#include "AOTStubCalls.h"
#include <tuple>

namespace JSC { namespace AOT {

struct SharedCode {
    uint32_t thunkOffset(std::span<const uint64_t> moves) const;
    uint32_t thunkOffset(uint32_t positionOfBody, uint32_t indexInBody) const { return argumentThunks.offsets[thunksOfArgumentMoves[firstArgumentMovesOfBody[positionOfBody] + indexInBody]]; }

    StubBlob blob;
    Vector<KeptArgumentMoves> argumentMoves;
    Vector<uint32_t> firstArgumentMovesOfBody;
    Vector<uint32_t> thunksOfArgumentMoves;
    ArgumentThunks argumentThunks;
    uint32_t argumentThunksOffset { 0 };
    uint32_t veneersOffset { 0 };
};

struct VeneerTarget {
    friend bool operator==(const VeneerTarget&, const VeneerTarget&) = default;

    SymbolSpace space;
    uint16_t entry;
    uint32_t index;
};

inline bool comesFirstAmongVeneers(const VeneerTarget& a, const VeneerTarget& b)
{
    return std::tie(a.space, a.index, a.entry) < std::tie(b.space, b.index, b.entry);
}

struct FinalJump {
    uint32_t sizeWithoutJump { 0 };
    bool jumpsToNextBody { false };
};

struct FarInstanceWords {
    static size_t offsetBehind(size_t bodySize) { return WTF::roundUpToMultipleOf<sizeof(int32_t)>(bodySize); }

    bool isEmpty() const { return distances.isEmpty(); }
    size_t sizeOfDistances() const { return distances.size() * sizeof(int32_t); }
    size_t size() const { return sizeOfDistances() + numberOfDetours * sizeOfDetourToFarInstanceWord; }
    bool takesDetour(size_t bodySize, uint32_t offsetOfSite) const { return offsetBehind(bodySize) + sizeOfDistances() - offsetOfSite > reachOfReadOfFarInstanceWord; }

    Vector<int32_t, 0> distances;
    uint32_t numberOfDetours { 0 };
};

struct CodeLayoutInputs {
    size_t sizeOfFarInstanceWords(size_t position) const { return farInstanceWords.empty() ? 0 : farInstanceWords[position].size(); }

    const LinkedFunctions& functions;
    const ResolvedCallees& callees;
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
    std::span<const FarInstanceWords> farInstanceWords { };
};

struct CodeLayout {
    size_t firstVeneerOffset(uint32_t position, size_t bodySize, size_t sizeOfFarInstanceWords = 0) const { return bodyStarts[position] + FarInstanceWords::offsetBehind(bodySize) + sizeOfFarInstanceWords; }

    unsigned codeGranuleShift { 0 };
    Vector<uint32_t> bodyStarts;
    Vector<uint32_t> sharedCodeCopyByBody;
    Vector<uint32_t, 4> sharedCodeCopies;
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
    const ConstantPool& constants;
    const ImageNumbers& numbers;
    std::span<const ImageString> strings { };
    std::span<const ImageLongString> longStrings { };
};

unsigned chooseCodeGranuleShift(const LinkedFunctions&, BuildContext&);
Vector<FinalJump> findFinalJumps(const LinkedFunctions&, const ResolvedCallees&, BuildContext&);
Vector<FarInstanceWords> findFarInstanceWords(const LinkedFunctions&, const ImageNumbers&, BuildContext&);

CodeLayout layOutCode(const CodeLayoutInputs&, BuildContext&);
void removeFinalJumps(LinkedFunctions&, const BitVector& fallsIntoNext, std::span<const FinalJump>, BuildContext&);
void linkCode(const CodeLinkerInputs&, std::span<uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
