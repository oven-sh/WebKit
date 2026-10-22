/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTRepresentationRules.h"
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

uint64_t votesOfConversionIn(FunctionAnalyses&, BlockID);

class RepresentationWebs {
    WTF_MAKE_NONCOPYABLE(RepresentationWebs);
public:
    RepresentationWebs(const RepresentationInputs&, FunctionAnalyses&);

    void build();
    void choose();

    Representation representationOf(ValueID) const;
    unsigned numberOfWebs() const { return m_webs.size(); }

private:
    using Votes = std::array<uint64_t, numberOfEnumerators<Representation>>;

    struct Web {
        RepresentationSet legal;
        Votes produced { };
        Votes wanted { };
        Representation preferred { Representation::Unchosen };
        Representation chosen { Representation::Unchosen };
        bool fitsNarrowReference { false };

        // Where there is none, nobody collects votes.
        bool hasChoice() const { return fitsNarrowReference || legal != RepresentationSet { preferred }; }
    };

    struct Member {
        ValueID value;
        uint32_t web { 0 };
    };

    // What `voter` chooses is a vote in the choice of `web`: a value goes from one to the other.
    struct Link {
        uint32_t voter { 0 };
        uint32_t web { 0 };
        uint64_t votes { 0 };
        bool isProducer { false };
    };

    uint32_t find(uint32_t valueIndex) const;
    void unite(ValueID, ValueID);
    void uniteCopies();
    void numberWebs();
    void restrictToLegalRepresentations();
    void collectProducers();
    void collectUses();
    static void addProducer(Web&, Representation, uint64_t votes);
    static void addUse(Web&, Representation, uint64_t votes);
    uint64_t votesOf(BlockID) const;
    uint64_t votesOf(Edge) const;
    uint64_t votesOfArgument(Edge, ValueID) const;
    void admitNarrowReferences();
    BitVector findNarrowWebsAmong(const BitVector& webs);
    BitVector findWebsThatStayNarrow(const BitVector& webs);
    void excludeWebsThatMayLieInFrames(BitVector& narrowWebs);
    bool mayStayNarrow(ValueID member) const;
    bool isReachedBeforeCollectorCanRun(const Use&, BlockID definingBlock) const;
    void findFirstCollectingValues();
    void findWhereCollectorMayHaveRun(const BitVector& webs);
    BitVector findValuesThatMayLieInFrames() const;
    Representation chooseFor(const Web&) const;
    Representation chooseForConstant(ValueID) const;
    void chooseForConstants();
    static uint64_t conversionsNeededBy(const Web&, Representation);

    RepresentationInputs m_inputs;
    FunctionAnalyses& m_analyses;
    mutable Vector<uint32_t> m_parent;
    Vector<std::optional<uint32_t>> m_webOfRoot;
    Vector<Representation> m_fixed;
    // Zero until somebody asks.
    mutable Vector<uint64_t, 16> m_votesOfBlock;
    Vector<uint32_t> m_firstCollectingValue;
    Vector<std::optional<uint32_t>> m_indexOfBlockWithReference;
    Vector<BitVector> m_mayHaveCollectedSinceEntryOf;
    Vector<Web> m_webs;
    // By ascending value.
    Vector<Member> m_members;
    Vector<Link> m_links;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
