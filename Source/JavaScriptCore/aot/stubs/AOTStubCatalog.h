/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCompileOptions.h"
#include "AOTEffects.h"
#include "AOTStubDescriptor.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include "RegisterSet.h"
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct RecordedOptions;
struct StubRecording;

namespace Typed {
class Program;
}

struct StubOptions {
    static StubOptions from(const RecordedOptions&, const Typed::Program&, bool hasOneInstance, bool matchesRegExps, bool recordsLastMatch, bool checksProofs, bool checksWalk);

    bool useWideReferences { false };
    bool imageHasWebAssembly { false };
    bool hasOneInstance { false };
    bool matchesRegExps { true };
    bool recordsLastMatch { true };
    bool checksProofs { false };
    bool checksWalk { false };
    bool methodsOfBuiltinsMayBeReplaced { false };
    bool callsNumberOrStringByValue { true };
    WTF::BitSet<numberOfBuiltinBoxedStubs> boxedCallsOutOfReach { };
    bool isPositionIndependent { imageFormOfThisRuntime() == ImageForm::PositionIndependent };
    uint32_t maxNameInFieldTable { UINT16_MAX };
    CPUFeatureSet targetFeatures;
};

struct StubPolicy {
    bool shapesFitHalfword { true };
};

struct StubSummary {
    StubSignature signature;
    RegisterSet clobbered;
    RegisterSet clobberedInline;
};

class StubCatalog {
    WTF_MAKE_NONCOPYABLE(StubCatalog);
public:
    explicit StubCatalog(const StubOptions&, StubPolicy&& = { });
    ~StubCatalog();

    const StubSummary& summaryOf(StubKey) const;
    const RegisterSet& clobberedByRegisterVariantsOf(StubKey) const;
    const RegisterSet& clobberedBesideResultByRegisterVariantsOf(StubKey) const;
    Typed::Effects effectsOf(StubKey) const;
    const StubSummary& summaryAndEffectsOf(StubKey key, Typed::Effects& effects) const
    {
        const Answer& answer = answerFor(key);
        if (isOperationCallHead(key))
            effects = effectsOf(key);
        else
            effects = answer.effects;
        return answer.summary;
    }
    bool isLeaf(StubKey) const;
    bool bindsImmediate(StubKey) const;
    const StubOptions& options() const { return m_options; }
    uint64_t identifier() const { return m_identifier; }
    const StubPolicy& policy() const { return m_policy; }

    const StubRecording& recordingOf(StubKey body) const;
    bool hasRecordingOf(StubKey body) const;
    StubRecording dryRun(StubKey body) const;
    bool answersCover(const StubRecording&) const;

private:
    struct Node;
    struct Answer {
        StubSummary summary;
        Typed::Effects effects;
        bool leavesStubs;
    };
    using IndexByKey = HashMap<uint64_t, unsigned>;
    struct PlaceOfAnswer {
        unsigned node { 0 };
        unsigned answer { 0 };
        unsigned numberOfKey { 0 };
    };

    void listBodies();
    const Node& closedNodeAt(unsigned index) const;
    void generateAndClose(unsigned index) const;
    static void checkDeclarations(std::span<Node* const>);
    const Node* tryFindNode(StubKey body) const;
    const Node& nodeOf(StubKey body) const;
    const Answer* tryFindOwnAnswer(StubKey) const;
    const Answer& answerFor(StubKey) const;

    const StubOptions m_options;
    const StubPolicy m_policy;
    const uint64_t m_identifier;
    Vector<StubKey> m_bodies;
    IndexByKey m_nodeOfBody;
    HashMap<uint64_t, PlaceOfAnswer> m_answerOfKeyWithoutImmediate;
    Vector<Vector<StubKey, 2>> m_keysWithoutImmediateOfNode;
    mutable Lock m_lock;
    mutable Vector<std::unique_ptr<Node>> m_nodes;
    mutable Vector<std::atomic<bool>> m_isClosed;
    mutable Vector<RegisterSet> m_clobberedByRegisterVariants;
    mutable Vector<std::atomic<bool>> m_knowsRegisterVariants;
    mutable Vector<RegisterSet> m_clobberedBesideResultByRegisterVariants;
    mutable Vector<std::atomic<bool>> m_knowsWhatRegisterVariantsClobberBesideResult;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
