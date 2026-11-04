/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct RecordedOptions;
struct StubRecording;

namespace Typed {
class Program;
class SlotPlan;
}

// Everything that may change the code of a stub. The image records every member.
struct StubOptions {
    // hasOneInstance: Facts proved that the program runs in at most one instance. Unproven is false.
    static StubOptions from(const RecordedOptions&, const Typed::Program&, bool hasOneInstance, bool matchesRegExps, bool checksProofs);

    bool useWideReferences { false };
    bool imageHasWebAssembly { false };
    bool hasOneInstance { false };
    // Facts::matchesRegExps(). Whether the handlers that every image holds match a RegExp themselves. It chooses between two roads to one answer: where it is false they leave a RegExp to the host function,
    // and the image lists no operation of RemovableFeature::RegExp on their account.
    bool matchesRegExps { true };
    // TestingSwitch::CheckProofsAtRunTime. No option of a build. A stub that stores by what it remembers holds that against all that it would have asked, and stops where the two differ.
    bool checksProofs { false };
    bool isPositionIndependent { imageFormOfThisRuntime() == ImageForm::PositionIndependent };
    // Every row of the image's table of fields has an entry for each name up to this one.
    uint32_t maxNameInFieldTable { UINT16_MAX };
    CPUFeatureSet targetFeatures;
};

struct StubPolicy {
    // FieldTableProbePlan. Plan chooses the two, so the catalog of Compile has them as well. They change the order of the tests in a stub, and none of what a catalog says of it.
    bool readsTestOutOfLineValuesSecond { false };
    bool readsTestDoublesSecond { false };
};

// What Plan knows, for the catalog of Compile and for that of Image.
StubPolicy stubPolicyOf(const Typed::SlotPlan&);

// clobbered: every register that may hold another value when the stub returns normally, with its results, its immediate register and the macro scratch registers.
// clobberedInline: the part of it that the stubs' own instructions write, without what the C++ functions, host functions and compiled code that they call may write.
// A register outside `clobbered` is preserved only when the stub returns. Nothing is said of a register at a landing pad.
struct StubSummary {
    StubSignature signature;
    RegisterSet clobbered;
    RegisterSet clobberedInline;
};

// What it says of a stub follows from the code of the stub and of the stubs that it refers to, so no row states a register by hand.
// An answer is a function of (StubOptions, StubKey) alone. A body is generated when the first question about it comes, from whichever thread, and the order of the questions changes no answer.
class StubCatalog {
    WTF_MAKE_NONCOPYABLE(StubCatalog);
public:
    explicit StubCatalog(const StubOptions&, StubPolicy&& = { });
    ~StubCatalog();

    const StubSummary& summaryOf(StubKey) const;
    // StubSummary::clobbered of the key and of its variants together: forEachRegisterVariant().
    const RegisterSet& clobberedByRegisterVariantsOf(StubKey) const;
    // Of a stub whose variants hand the result back in the register of their key: what a site loses beside that register, whichever of them it calls.
    const RegisterSet& clobberedBesideResultByRegisterVariantsOf(StubKey) const;
    Typed::Effects effectsOf(StubKey) const;
    bool isLeaf(StubKey) const;
    // The key of the call carries the immediate, whichever it is, and the site moves none.
    bool bindsImmediate(StubKey) const;
    const StubOptions& options() const { return m_options; }
    uint64_t identifier() const { return m_identifier; }
    const StubPolicy& policy() const { return m_policy; }

    // For AOTStubBlob.cpp and the tests. Every body without an immediate has a recording, and so have the head that tail-calls a leaf that preserves most registers and the heads of an operation with a prelude.
    // dryRun serves the other bodies with an immediate, and is not memoized.
    const StubRecording& recordingOf(StubKey body) const;
    bool hasRecordingOf(StubKey body) const;
    StubRecording dryRun(StubKey body) const;
#if ASSERT_ENABLED || ASAN_ENABLED
    // Of a body that dryRun served: whether what is said here of its entries holds of the body and of all that it refers to.
    bool answersCover(const StubRecording&) const;
#endif

private:
    struct Node;
    struct Answer {
        StubSummary summary;
        Typed::Effects effects;
        bool leavesStubs;
    };
    using IndexByKey = HashMap<uint64_t, unsigned>;
    // The node, the place among Node::answers, and the number of the key among those without an immediate.
    struct PlaceOfAnswer {
        unsigned node { 0 };
        unsigned answer { 0 };
        unsigned numberOfKey { 0 };
    };

    void listBodies();
    const Node& closedNodeAt(unsigned index) const;
    void generateAndClose(unsigned index) const;
    static void checkDeclarations(std::span<Node* const>);
#if ASSERT_ENABLED || ASAN_ENABLED
    const Node* tryFindNode(StubKey body) const;
#endif
    const Node& nodeOf(StubKey body) const;
    const Answer* tryFindOwnAnswer(StubKey) const;
    const Answer& answerFor(StubKey) const;

    const StubOptions m_options;
    const StubPolicy m_policy;
    const uint64_t m_identifier;
    // The constructor fills these four, and nothing changes them behind it.
    Vector<StubKey> m_bodies;
    IndexByKey m_nodeOfBody;
    HashMap<uint64_t, PlaceOfAnswer> m_answerOfKeyWithoutImmediate;
    Vector<Vector<StubKey, 2>> m_keysWithoutImmediateOfNode;
    // A node is written under the lock and read by whoever has seen that it is closed.
    mutable Lock m_lock;
    mutable Vector<std::unique_ptr<Node>> m_nodes;
    mutable Vector<std::atomic<bool>> m_isClosed;
    // By PlaceOfAnswer::numberOfKey. A set is written under the lock and read by whoever has seen that it is known.
    mutable Vector<RegisterSet> m_clobberedByRegisterVariants;
    mutable Vector<std::atomic<bool>> m_knowsRegisterVariants;
    mutable Vector<RegisterSet> m_clobberedBesideResultByRegisterVariants;
    mutable Vector<std::atomic<bool>> m_knowsWhatRegisterVariantsClobberBesideResult;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
