/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTConstantPool.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubBlob.h"
#include "AOTStubCalls.h"
#include <tuple>

namespace JSC { namespace AOT {

// The stubs, then the argument thunks, then the veneers of both.
struct SharedCode {
    uint32_t thunkOffset(const ArgumentMoves&) const;
    void buildArgumentMovesHashIndex();

    StubBlob blob;
    // Each distinct list once, ascending by its words. They point into the functions.
    Vector<const ArgumentMoves*> argumentMoves;
    // index + 1, linear probing. Empty: thunkOffset() searches the sorted list.
    Vector<uint32_t> argumentMovesHashSlots;
    ArgumentThunks argumentThunks;
    uint32_t argumentThunksOffset { 0 };
    uint32_t veneersOffset { 0 };
};

struct VeneerTarget {
    friend bool operator==(const VeneerTarget&, const VeneerTarget&) = default;

    SymbolSpace space;
    uint16_t entry;
    // A position among the functions, or the number of a regular expression's record.
    uint32_t index;
};

// The order of each list of veneers in CodeLayout.
inline bool comesFirstAmongVeneers(const VeneerTarget& a, const VeneerTarget& b)
{
    return std::tie(a.space, a.index, a.entry) < std::tie(b.space, b.index, b.entry);
}

struct FinalJump {
    uint32_t sizeWithoutJump { 0 };
    bool jumpsToNextBody { false };
};

// What lies behind a body for the words of the instance that it names and isFarInstanceWord() holds of: their distances, then the detours.
struct FarInstanceWords {
    static size_t offsetBehind(size_t bodySize) { return WTF::roundUpToMultipleOf<sizeof(int32_t)>(bodySize); }

    bool isEmpty() const { return distances.isEmpty(); }
    size_t sizeOfDistances() const { return distances.size() * sizeof(int32_t); }
    size_t size() const { return sizeOfDistances() + numberOfDetours * sizeOfDetourToFarInstanceWord; }
    // A site either reads every distance or none, so the sites with a detour are the first ones of the body.
    bool takesDetour(size_t bodySize, uint32_t offsetOfSite) const { return offsetBehind(bodySize) + sizeOfDistances() - offsetOfSite > reachOfReadOfFarInstanceWord; }

    // Ascending, each once. linkCode() finds one by binary search.
    Vector<int32_t, 0> distances;
    // The i-th site that takes one has the i-th.
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
    // By body, or empty where no body names a far word.
    std::span<const FarInstanceWords> farInstanceWords { };
};

struct CodeLayout {
    size_t firstVeneerOffset(uint32_t position, size_t bodySize, size_t sizeOfFarInstanceWords = 0) const { return bodyStarts[position] + FarInstanceWords::offsetBehind(bodySize) + sizeOfFarInstanceWords; }

    unsigned codeGranuleShift { 0 };
    Vector<uint32_t> bodyStarts;
    Vector<uint32_t> sharedCodeCopyByBody;
    Vector<uint32_t, 4> sharedCodeCopies;
    // Ascending by space, index, entry. Each target once.
    // linkCode() finds a veneer by binary search, and the i-th lies i * sizeOfVeneer() behind the first.
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
};

unsigned chooseCodeGranuleShift(const LinkedFunctions&, BuildContext&);
Vector<FinalJump> findFinalJumps(const LinkedFunctions&, const ResolvedCallees&, BuildContext&);
// Empty where no body names a far word, which is so of every program on x86-64. What it finds follows from the body and the numbers alone, not from where the body comes to lie.
Vector<FarInstanceWords> findFarInstanceWords(const LinkedFunctions&, const ImageNumbers&, BuildContext&);

// The bodies lie in the order of the functions. One run, no rounds:
// a first placement without veneers gives the start of each body. A transfer gets a veneer behind its body where its distance there, plus what the front one of the two bodies and the bodies between them could grow by (a veneer for each of their transfers, a final jump that stays, alignment) and the room of one copy of the shared code, is not within the reach of a near call.
// The second placement is the layout. No distance in it is longer than that, so nothing is out of reach.
// The far words of a body lie right behind it in both placements, in front of its veneers, and such a body does not fall into the next.
CodeLayout layOutCode(const CodeLayoutInputs&, BuildContext&);
void removeFinalJumps(LinkedFunctions&, const BitVector& fallsIntoNext, std::span<const FinalJump>, BuildContext&);
// In parallel by function. Resolves the mentions of a function once each, then its relocations.
void linkCode(const CodeLinkerInputs&, std::span<uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
