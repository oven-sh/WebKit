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

class RepresentationWebs {
    WTF_MAKE_NONCOPYABLE(RepresentationWebs);
public:
    RepresentationWebs(const RepresentationInputs&, FunctionAnalyses&);

    void build();
    void choose();

    Representation representationOf(ValueID) const;
    unsigned numberOfWebs() const { return m_webs.size(); }

private:
    using Votes = std::array<uint32_t, numberOfEnumerators<Representation>>;

    struct Web {
        RepresentationSet legal;
        Votes produced { };
        Votes wanted { };
        Representation preferred { Representation::Unchosen };
        Representation chosen { Representation::Unchosen };
    };

    uint32_t find(uint32_t valueIndex) const;
    void unite(ValueID, ValueID);
    void uniteCopies();
    void numberWebs();
    void restrictToLegalRepresentations();
    void collectProducers();
    void collectUses();
    void addProducer(ValueID member, Representation, BlockID);
    void addUse(ValueID member, Representation, BlockID);
    uint32_t votesOf(BlockID) const;
    bool mayStayNarrow(ValueID member) const;
    bool isReachedBeforeCollectorCanRun(const Use&, BlockID definingBlock) const;
    void findWhereCollectorMayHaveRun();
    Representation chooseFor(const Web&) const;
    static uint32_t conversionsNeededBy(const Web&, Representation);

    RepresentationInputs m_inputs;
    FunctionAnalyses& m_analyses;
    mutable Vector<uint32_t> m_parent;
    Vector<std::optional<uint32_t>> m_webOfRoot;
    Vector<Representation> m_fixed;
    Vector<uint32_t> m_firstCollectingValue;
    Vector<std::optional<uint32_t>> m_indexOfBlockWithReference;
    Vector<BitVector> m_mayHaveCollectedSinceEntryOf;
    Vector<Web> m_webs;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
