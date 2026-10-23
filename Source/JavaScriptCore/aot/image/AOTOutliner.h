/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeIndex.h"
#include "AOTCodeTemplates.h"
#include "AOTLinkedFunctions.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <utility>

namespace JSC { namespace AOT {

struct OutlinedSite {
    uint32_t offset;
    uint32_t length;
    uint32_t body;
};

struct OutlinedBody {
    uint32_t function;
    uint32_t offset;
    uint32_t length;
    uint32_t numberOfSites;
    OptionalIndex singleCaller;
    bool endsInCall;
};

struct Outlining {
    Vector<Vector<OutlinedSite>> sites;
    Vector<OutlinedBody> bodies;
};

struct OutlinedBodies {
    size_t sizeOf(uint32_t number) const { return (number + 1 < offsets.size() ? offsets[number + 1] : bytes.size()) - offsets[number]; }

    std::span<const Relocation> relocationsOf(uint32_t number) const LIFETIME_BOUND
    {
        std::span<const Relocation> all = relocations.span();
        size_t begin = offsets[number];
        auto first = std::ranges::lower_bound(all, begin, { }, &Relocation::offset);
        auto last = std::ranges::lower_bound(first, all.end(), begin + sizeOf(number), { }, &Relocation::offset);
        return all.subspan(first - all.begin(), last - first);
    }

    Vector<uint8_t> bytes;
    Vector<uint32_t> offsets;
    // Offsets count from the start of bytes, and symbols index the lists of OutlinedCode.
    Vector<Relocation> relocations;
};

struct OutlinedCode {
    size_t ownBodiesSize(uint32_t caller) const;

    OutlinedBodies sharedBodies;
    OutlinedBodies ownBodies;
    Vector<uint32_t> indexWithinKind;
    Vector<Vector<uint32_t, 0>> ownBodiesByCaller;
    Vector<ResolvedCallee> callees;
    Vector<StubKey> stubKeys;
    Vector<ArgumentMoves> argumentMoves;
    Vector<Vector<std::pair<uint32_t, uint32_t>, 0>> paddingBeforeAliases;
};

struct OutlinerInputs {
    const LinkedFunctions& functions;
    const ResolvedCallees& callees;
    std::span<const EntryAlias> aliases;
    const CodeTemplates& templates;
    unsigned numberOfSharedCodeCopies;
};

Outlining chooseOutlinedSequences(const OutlinerInputs&, BuildContext&);

struct OutliningRewriteInputs {
    const CodeIndex& index;
    const ResolvedCallees& callees;
    const Outlining& outlining;
    const CodeTemplates& templates;
};

OutlinedCode applyOutlining(LinkedFunctions&, Vector<EntryAlias>&, const OutliningRewriteInputs&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
