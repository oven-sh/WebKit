/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTEffects.h"
#include "AOTExpected.h"
#include "AOTStubDescriptor.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CPUFeatureSet.h"
#include "RegisterSet.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>

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
class SlotPlan;
}

// Everything that may change the code of a stub. The image records every member.
struct StubOptions {
    static StubOptions from(const RecordedOptions&, const Typed::Program&, const Typed::FlowResults&);

    bool useWideReferences { false };
    bool forceGCSlowPaths { false };
    bool imageHasWebAssembly { false };
    bool hasOneInstance { false };
    // CompileOptions::sharesRealms(): a compiled object or a closure of the realm may be of another image, and its numbers say nothing in the tables of this one.
    bool sharesRealms { false };
    // How many instances stand in front of this image's in the list of a realm.
    uint32_t imagesBelow { 0 };
    // ImageNumberRanges.
    uint32_t shapesBelow { 0 };
    uint32_t lastShape { 0 };
    uint32_t firstStringHeader { 0 };
    uint32_t closureNumbersBelow { 0 };
    uint32_t endOfClosureNumbers { 0 };
    // Image::layoutIndicesBelow(), and ProgramTables::numberOfLayoutIndices of this image.
    uint32_t layoutIndicesBelow { 0 };
    uint32_t numberOfLayoutIndices { 0 };
    // Whether the stubs read and call the traps of a proxy themselves. It chooses between two roads to one answer and says nothing about the program: where it is false, the stubs leave an access through a proxy to the run time.
    // So it may rest on a guess that the program meets no proxy.
    Typed::Expected<bool> callsTrapsOfProxies { true };
    Typed::Expected<bool> matchesRegExps { true };
    bool validates { false };
    bool checksProofs { false };
    bool countsAllocations { false };
    bool isPositionIndependent { imageFormOfThisRuntime() == ImageForm::PositionIndependent };
    uint32_t layoutSeed { 0 };
    // Every row of the image's table of fields has an entry for each name up to this one.
    uint32_t maxNameInFieldTable { UINT16_MAX };
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
    AOT_CLASS_THRESHOLD(uint32_t, minUsesForOwnStub, 512);

    const BitVector& namesWithOwnStub(Typed::Access access) const { return namesWithOwnStubByAccess[static_cast<size_t>(access)]; }
    std::optional<uint32_t> expectedFieldTableEntry(uint32_t name) const
    {
        auto found = expectedFieldTableEntries.find(name);
        return found == expectedFieldTableEntries.end() ? std::nullopt : std::optional(found->value);
    }
    RegExpLiteralConstants regExpLiteral(std::optional<uint32_t> record, bool isGlobal) const;

    // Bit i: the name that a stub takes as immediate i.
    std::array<BitVector, numberOfEnumerators<Typed::Access>> namesWithOwnStubByAccess;
    // For a name with its own stub for reads: the entry of the field table that most layouts have for it, where that is a JSValue in the cell or in the Extension. A guess that the stub tests.
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> expectedFieldTableEntries;
    // A lookup in the field table tests the forms one after the other. In a program with many optional fields most reads by name that miss the first form find a JSValue in the Extension.
    bool readsTestOutOfLineValuesSecond { false };
    bool readsTestDoublesSecond { false };
    // [RegExpRecordID::index()], for the literal without the flag g.
    Vector<RegExpLiteralConstants> regExpLiterals;
};

StubPolicy chooseStubPolicy(const KeyUses&, const IdentifierNumbering&, BuildContext&);
void addExpectedFieldTableEntries(StubPolicy&, const Typed::Program&, const Typed::SlotPlan&, const IdentifierNumbering&);
void chooseOrderOfFieldTableForms(StubPolicy&, const Typed::Program&, const Typed::SlotPlan&, const KeyUses&);
void addRegExpLiterals(StubPolicy&, const RegExpRequests&, bool legacyStaticsMayBeRead, BuildContext&);

struct StubSummary {
    StubSignature signature;
    RegisterSet clobbered;
    RegisterSet clobberedInline;
};

// What it says of a stub follows from the code of the stub and of the stubs that it refers to. That code is generated when the first question about the stub comes, from whichever thread.
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
    // The constructor fills these four, and nothing changes them behind it.
    Vector<StubKey> m_bodies;
    IndexByKey m_nodeOfBody;
    IndexByKey m_nodeOfKeyWithoutImmediate;
    Vector<Vector<StubKey, 2>> m_keysWithoutImmediateOfNode;
    // A node is written under the lock and read by whoever has seen that it is closed.
    mutable Lock m_lock;
    mutable Vector<std::unique_ptr<Node>> m_nodes;
    mutable Vector<std::atomic<bool>> m_isClosed;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
