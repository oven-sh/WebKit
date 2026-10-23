/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTEffects.h"
#include "AOTStubDescriptor.h"
#include "CPUFeatureSet.h"
#include "RegisterSet.h"
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
class IdentifierNumbering;
class RegExpRequests;
struct KeyUses;
struct RecordedOptions;
struct StubRecording;

namespace Typed {
class FlowResults;
class Program;
}

// Everything that may change the code of a stub. The image records every member.
struct StubOptions {
    static StubOptions from(const RecordedOptions&, const Typed::Program&, const Typed::FlowResults&);

    bool useWideReferences { false };
    bool forceGCSlowPaths { false };
    bool imageHasWebAssembly { false };
    bool hasOneInstance { false };
    bool validates { false };
    bool countsAllocations { false };
    bool isPositionIndependent { imageFormOfThisRuntime() == ImageForm::PositionIndependent };
    uint32_t layoutSeed { 0 };
    CPUFeatureSet targetFeatures;
};

struct RegExpLiteralConstants {
    uint32_t minimumSize { 0 };
    uint32_t numberOfGroups { 0 };
    bool usesLastIndex { true };
    bool isGlobal { true };
    bool matchesAtCodePointBoundaries { true };
    bool recordsMatch { true };
    bool hasNamedGroups { true };
};

struct StubPolicy {
    static constexpr uint32_t minUsesForOwnStub = 512;

    const BitVector& namesWithOwnStub(Typed::Access access) const { return namesWithOwnStubByAccess[static_cast<size_t>(access)]; }
    RegExpLiteralConstants regExpLiteral(std::optional<uint32_t> record, bool isGlobal) const;

    // Bit i: the name that a stub takes as immediate i.
    std::array<BitVector, numberOfEnumerators<Typed::Access>> namesWithOwnStubByAccess;
    // [RegExpRecordID::index()], for the literal without the flag g.
    Vector<RegExpLiteralConstants> regExpLiterals;
};

StubPolicy chooseStubPolicy(const KeyUses&, const IdentifierNumbering&, BuildContext&);
void addRegExpLiterals(StubPolicy&, const RegExpRequests&, bool legacyStaticsMayBeRead, BuildContext&);

struct StubSummary {
    StubSignature signature;
    RegisterSet clobbered;
    RegisterSet clobberedInline;
};

class StubCatalog {
    WTF_MAKE_NONCOPYABLE(StubCatalog);
public:
    StubCatalog(const StubOptions&, StubPolicy&&);
    ~StubCatalog();

    const StubSummary& summaryOf(StubKey) const;
    Typed::Effects effectsOf(StubKey) const;
    bool isLeaf(StubKey) const;
    bool bindsImmediate(StubKey, uint32_t immediate) const;
    const StubOptions& options() const { return m_options; }
    const StubPolicy& policy() const { return m_policy; }

    // For AOTStubBlob.cpp and the tests. Every body without an immediate has a recording. dryRun serves the bound ones without, and is not memoized.
    const StubRecording& recordingOf(StubKey body) const;
    bool hasRecordingOf(StubKey body) const;
    StubRecording dryRun(StubKey body) const;
    RegisterSet closedClobbersOf(const StubRecording&) const;

private:
    struct Node;
    struct Answer {
        StubSummary summary;
        Typed::Effects effects;
        bool leavesStubs;
    };
    using IndexByKey = HashMap<uint64_t, unsigned>;

    void generateNodes();
    void checkDeclarations() const;
    void computeAnswers();
    const Node* tryFindNode(StubKey body) const;
    const Node& nodeOf(StubKey body) const;
    const Answer& answerFor(StubKey) const;
    Answer createAnswer(StubKey) const;

    const StubOptions m_options;
    const StubPolicy m_policy;
    Vector<Node> m_nodes;
    IndexByKey m_nodeOfBody;
    Vector<Answer> m_answers;
    IndexByKey m_answerOfKey;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
