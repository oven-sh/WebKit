/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTNarrowReferenceValidation.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRegisters.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3Const64Value.h"
#include "B3EliminateDeadCode.h"
#include "B3FixSSA.h"
#include "B3Generate.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3Validate.h"
#include "B3ValueInlines.h"

namespace JSC { namespace AOT {

namespace {

constexpr uint32_t referenceOrigin = 1;
constexpr uint32_t otherReferenceOrigin = 2;
constexpr uint32_t safepointOrigin = 3;
constexpr uint32_t suspensionOrigin = 4;
constexpr uint32_t otherOrigin = 5;

constexpr HeapsOfCollection heapsOfCollection {
    .collectorState = B3::HeapRange(1, 2),
    .weakTableStorage = B3::HeapRange(2, 3),
    .frameSlots = B3::HeapRange(3, 4),
    .permanentCells = { B3::HeapRange(4, 6), B3::HeapRange(7, 8) },
};

// What is taken away from a graph in which every reference has its pointer.
enum class Mutation : uint8_t {
    None,
    NoKeepAlive,
    KeepAliveBeforeSafepoint,
    PointerBehindSafepoint,
    PointerIsNotOpaque,
    PointerOfOtherReference,
    NoGuard,
};

constexpr Mutation allMutations[] = {
    Mutation::NoKeepAlive,
    Mutation::KeepAliveBeforeSafepoint,
    Mutation::PointerBehindSafepoint,
    Mutation::PointerIsNotOpaque,
    Mutation::PointerOfOtherReference,
    Mutation::NoGuard,
};

class Graph {
public:
    Graph()
        : Graph(B3::Policy { })
    {
    }

    explicit Graph(const B3::Policy& policy)
        : proc(policy)
        , block(proc.addBlock())
    {
        base = block->appendNew<B3::ArgumentRegValue>(proc, originOf(otherOrigin), cellCageBaseGPR);
        object = block->appendNew<B3::ArgumentRegValue>(proc, originOf(otherOrigin), GPRInfo::argumentGPR0);
    }

    static B3::Origin originOf(uint32_t number) { return Typed::b3OriginOf(Typed::ValueID(number)); }

    B3::Value* int32(int32_t value) { return block->appendNew<B3::Const32Value>(proc, originOf(otherOrigin), value); }
    B3::Value* int64(int64_t value) { return block->appendNew<B3::Const64Value>(proc, originOf(otherOrigin), value); }
    B3::Value* unary(B3::Opcode opcode, B3::Value* child) { return block->appendNew<B3::Value>(proc, opcode, originOf(otherOrigin), child); }
    B3::Value* binary(B3::Opcode opcode, B3::Value* left, B3::Value* right) { return block->appendNew<B3::Value>(proc, opcode, originOf(otherOrigin), left, right); }

    B3::Value* loadReference(uint32_t origin = referenceOrigin, int32_t offset = 16, B3::HeapRange range = B3::HeapRange(10, 11))
    {
        return block->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int32, originOf(origin), object, offset, range);
    }

    B3::Value* decode(B3::Value* reference)
    {
        return binary(B3::Add, base, binary(B3::Shl, unary(B3::ZExt32, reference), int32(cell32Shift)));
    }

    B3::Value* compress(B3::Value* pointer, uint32_t origin = referenceOrigin)
    {
        B3::Value* shifted = block->appendNew<B3::Value>(proc, B3::ZShr, originOf(origin), pointer, int32(cell32Shift));
        return unary(B3::Trunc, shifted);
    }

    B3::Value* loadThrough(B3::Value* pointer)
    {
        return block->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int64, originOf(otherOrigin), pointer, 8, B3::HeapRange(11, 12));
    }

    void store(B3::Value* value, int32_t offset = 24)
    {
        block->appendNew<B3::MemoryValue>(proc, B3::Store, originOf(otherOrigin), value, object, offset, B3::HeapRange(12, 13));
    }

    B3::PatchpointValue* patchpoint(uint32_t origin)
    {
        B3::PatchpointValue* result = block->appendNew<B3::PatchpointValue>(proc, B3::Void, originOf(origin));
        result->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
        return result;
    }

    B3::PatchpointValue* safepoint(uint32_t origin = safepointOrigin)
    {
        B3::PatchpointValue* result = patchpoint(origin);
        result->effects.writes = B3::HeapRange::top();
        result->effects.reads = B3::HeapRange::top();
        return result;
    }

    void keepAlive(B3::Value* pointer)
    {
        B3::PatchpointValue* result = patchpoint(otherOrigin);
        result->effects = B3::Effects::none();
        result->effects.writesLocalState = true;
        result->effects.reads = B3::HeapRange::top();
        result->append(pointer, B3::ValueRep::ColdAny);
    }

    void jumpTo(B3::BasicBlock* target)
    {
        block->appendNew<B3::Value>(proc, B3::Jump, originOf(otherOrigin));
        block->setSuccessors(B3::FrequentedBlock(target));
        block = target;
    }

    void branchTo(B3::Value* condition, B3::BasicBlock* taken, B3::BasicBlock* notTaken)
    {
        block->appendNew<B3::Value>(proc, B3::Branch, originOf(otherOrigin), condition);
        block->setSuccessors(B3::FrequentedBlock(taken), B3::FrequentedBlock(notTaken));
    }

    void finish() { block->appendNew<B3::Value>(proc, B3::Return, originOf(otherOrigin)); }

    static NarrowReferenceOrigins origins()
    {
        NarrowReferenceOrigins origins;
        origins.references.set(referenceOrigin);
        origins.references.set(otherReferenceOrigin);
        return origins;
    }

    NarrowReferenceFindings validate()
    {
        proc.resetReachability();
        NarrowReferenceFindings findings;
        findNarrowReferenceViolations(proc, heapsOfCollection, origins(), findings);
        return findings;
    }

    uint32_t insertGuards()
    {
        uint32_t inserted = insertNarrowReferenceGuards(proc, heapsOfCollection, origins());
        proc.resetReachability();
        B3::validate(proc);
        return inserted;
    }

    B3::Procedure proc;
    B3::BasicBlock* block;
    B3::Value* base;
    B3::Value* object;
};

// Emits the pointer and the keep-alive around emitSafepoints as code generation does, less what the mutation takes away.
template<typename Functor>
void guard(Graph& graph, B3::Value* reference, Mutation mutation, const Functor& emitSafepoints)
{
    if (mutation == Mutation::NoGuard) {
        emitSafepoints();
        return;
    }
    auto pointerBeside = [&] {
        B3::Value* guarded = mutation == Mutation::PointerOfOtherReference ? graph.loadReference(otherReferenceOrigin, 32) : reference;
        B3::Value* pointer = graph.decode(guarded);
        return mutation == Mutation::PointerIsNotOpaque ? pointer : graph.unary(B3::Opaque, pointer);
    };
    B3::Value* pointer = mutation == Mutation::PointerBehindSafepoint ? nullptr : pointerBeside();
    if (mutation == Mutation::KeepAliveBeforeSafepoint)
        graph.keepAlive(pointer);
    emitSafepoints();
    if (mutation == Mutation::PointerBehindSafepoint)
        pointer = pointerBeside();
    if (mutation == Mutation::NoKeepAlive || mutation == Mutation::KeepAliveBeforeSafepoint)
        return;
    // A sum that is only an address is folded into the access, so without Opaque nothing holds the pointer.
    if (mutation == Mutation::PointerIsNotOpaque)
        graph.loadThrough(pointer);
    else
        graph.keepAlive(pointer);
}

bool isOrigin(B3::Origin origin, uint32_t number)
{
    return origin.dfgOrigin() == Graph::originOf(number).dfgOrigin();
}

void checkFindsNothing(Graph& graph)
{
    AOT_TEST_CHECK(graph.validate().violations.isEmpty());
}

void checkFindsOne(Graph& graph, NarrowReferenceViolationKind kind = NarrowReferenceViolationKind::HasNoPointer, uint32_t safepoint = safepointOrigin)
{
    NarrowReferenceFindings findings = graph.validate();
    AOT_TEST_CHECK(findings.violations.size() == 1);
    AOT_TEST_CHECK(findings.violations[0].kind == kind);
    AOT_TEST_CHECK(isOrigin(findings.violations[0].reference, referenceOrigin));
    AOT_TEST_CHECK(isOrigin(findings.violations[0].safepoint, safepoint));
    AOT_TEST_CHECK(findings.numberOfReferencesAcrossSafepoints == 1);
}

template<typename Functor>
void checkEveryMutationIsFound(const Functor& build)
{
    {
        Graph graph;
        build(graph, Mutation::None);
        NarrowReferenceFindings findings = graph.validate();
        AOT_TEST_CHECK(findings.violations.isEmpty());
        AOT_TEST_CHECK(findings.numberOfReferencesAcrossSafepoints == 1);
    }
    for (Mutation mutation : allMutations) {
        Graph graph;
        build(graph, mutation);
        checkFindsOne(graph);
        AOT_TEST_CHECK(graph.insertGuards() == 1);
        checkFindsNothing(graph);
        AOT_TEST_CHECK(!graph.insertGuards());
    }
}

} // anonymous namespace

static void testReadThroughReference()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.safepoint();
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.finish();
    });
}

static void testDecodedBeforeSafepointAndFolded()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::Value* reference = graph.loadReference();
        B3::Value* address = graph.decode(reference);
        guard(graph, reference, mutation, [&] {
            graph.safepoint();
            graph.store(graph.loadThrough(address));
        });
        graph.finish();
    });
}

static void testReferenceStoredBehindSafepoint()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.safepoint();
            graph.store(reference, 40);
        });
        graph.finish();
    });
}

static void testBoxedBehindSafepoint()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.safepoint();
            B3::Value* isCell = graph.binary(B3::AboveEqual, reference, graph.int32(minCell32));
            B3::Value* boxed = graph.block->appendNew<B3::Value>(graph.proc, B3::Select, Graph::originOf(otherOrigin), isCell, graph.decode(reference), graph.unary(B3::ZExt32, reference));
            graph.store(boxed);
        });
        graph.finish();
    });
}

static void testTwoSafepoints()
{
    {
        Graph graph;
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, Mutation::None, [&] {
            graph.safepoint();
            graph.safepoint(otherOrigin);
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.finish();
        checkFindsNothing(graph);
    }
    Graph graph;
    B3::Value* reference = graph.loadReference();
    guard(graph, reference, Mutation::NoKeepAlive, [&] {
        graph.safepoint();
        graph.safepoint(otherOrigin);
        graph.store(graph.loadThrough(graph.decode(reference)));
    });
    graph.finish();
    NarrowReferenceFindings findings = graph.validate();
    AOT_TEST_CHECK(findings.violations.size() == 2);
    AOT_TEST_CHECK(findings.numberOfReferencesAcrossSafepoints == 1);
}

static void testSafepointInOtherBlock()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* middle = graph.proc.addBlock();
        B3::BasicBlock* last = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.jumpTo(middle);
            graph.safepoint();
            graph.jumpTo(last);
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.finish();
    });
}

static void testSafepointOnOnePath()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* slow = graph.proc.addBlock();
        B3::BasicBlock* join = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.branchTo(graph.loadReference(otherOrigin, 48), slow, join);
            graph.block = slow;
            graph.safepoint();
            graph.jumpTo(join);
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.finish();
    });
}

static void testSafepointInLoop()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* loop = graph.proc.addBlock();
        B3::BasicBlock* exit = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.jumpTo(loop);
            graph.safepoint();
            graph.branchTo(graph.loadReference(otherOrigin, 48), loop, exit);
            graph.block = exit;
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.finish();
    });
}

static void testLoopBehindLastUse()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* middle = graph.proc.addBlock();
        B3::BasicBlock* last = graph.proc.addBlock();
        B3::BasicBlock* loop = graph.proc.addBlock();
        B3::BasicBlock* exit = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.jumpTo(middle);
            graph.safepoint();
            graph.jumpTo(last);
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.jumpTo(loop);
        graph.branchTo(graph.loadReference(otherOrigin, 48), loop, exit);
        graph.block = exit;
        graph.finish();
    });
}

static void testUseInFrontOfSafepointInLoop()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* header = graph.proc.addBlock();
        B3::BasicBlock* body = graph.proc.addBlock();
        B3::BasicBlock* latch = graph.proc.addBlock();
        B3::BasicBlock* exit = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.jumpTo(header);
            graph.block = body;
            graph.safepoint();
            graph.jumpTo(latch);
            graph.branchTo(graph.loadReference(otherOrigin, 48), header, exit);
            graph.block = header;
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.jumpTo(body);
        graph.block = exit;
        graph.finish();
    });
}

static void testSafepointInInnerLoopAndUseInOuterLoop()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* outer = graph.proc.addBlock();
        B3::BasicBlock* inner = graph.proc.addBlock();
        B3::BasicBlock* innerLatch = graph.proc.addBlock();
        B3::BasicBlock* outerLatch = graph.proc.addBlock();
        B3::BasicBlock* exit = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.jumpTo(outer);
            graph.block = inner;
            graph.safepoint();
            graph.jumpTo(innerLatch);
            graph.branchTo(graph.loadReference(otherOrigin, 48), inner, outerLatch);
            graph.block = outerLatch;
            graph.branchTo(graph.loadReference(otherOrigin, 56), outer, exit);
            graph.block = outer;
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.jumpTo(inner);
        graph.block = exit;
        graph.finish();
    });
}

static void testCycleWithTwoEntries()
{
    checkEveryMutationIsFound([](Graph& graph, Mutation mutation) {
        B3::BasicBlock* user = graph.proc.addBlock();
        B3::BasicBlock* collector = graph.proc.addBlock();
        B3::BasicBlock* exit = graph.proc.addBlock();
        B3::Value* reference = graph.loadReference();
        guard(graph, reference, mutation, [&] {
            graph.branchTo(graph.loadReference(otherOrigin, 48), user, collector);
            graph.block = collector;
            graph.safepoint();
            graph.branchTo(graph.loadReference(otherOrigin, 56), user, exit);
            graph.block = user;
            graph.store(graph.loadThrough(graph.decode(reference)));
        });
        graph.jumpTo(collector);
        graph.block = exit;
        graph.finish();
    });
}

static void testCompressedPointer()
{
    auto build = [](Graph& graph, bool keepsPointer) {
        B3::Value* pointer = graph.loadThrough(graph.object);
        B3::Value* reference = graph.compress(pointer);
        graph.safepoint();
        graph.store(reference, 40);
        if (keepsPointer)
            graph.keepAlive(pointer);
        graph.finish();
    };
    Graph guarded;
    build(guarded, true);
    checkFindsNothing(guarded);

    Graph bare;
    build(bare, false);
    checkFindsOne(bare);
    AOT_TEST_CHECK(bare.insertGuards() == 1);
    checkFindsNothing(bare);
}

// A store of a reference is forwarded to a load of it after a safepoint. The guards replace the sum with the pointer, and the select between it and the word is still a pointer.
static void testCompressedPointerDecodedAfterSafepoint()
{
    Graph graph;
    B3::Value* pointer = graph.loadThrough(graph.object);
    B3::Value* reference = graph.compress(pointer);
    graph.store(reference, 40);
    graph.safepoint();
    B3::Value* namesCell = graph.binary(B3::Above, reference, graph.int32(10));
    B3::Value* decoded = graph.block->appendNew<B3::Value>(graph.proc, B3::Select, Graph::originOf(otherOrigin), namesCell, graph.decode(reference), graph.unary(B3::ZExt32, reference));
    graph.safepoint();
    graph.keepAlive(decoded);
    graph.finish();

    checkFindsOne(graph);
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    checkFindsNothing(graph);
}

// What is emitted has no dead code, so a use by a dead value keeps no pointer alive and displaces no reference.
static void testPointerHeldOnlyByDeadCode()
{
    Graph graph;
    B3::Value* pointer = graph.loadThrough(graph.object);
    B3::Value* reference = graph.compress(pointer);
    graph.safepoint();
    graph.binary(B3::Add, pointer, graph.int64(8));
    graph.store(reference, 40);
    graph.finish();
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    B3::eliminateDeadCode(graph.proc);
    checkFindsNothing(graph);
}

static void testReferenceDisplacedOnlyByDeadCode()
{
    Graph graph;
    B3::Value* reference = graph.loadReference();
    graph.decode(graph.binary(B3::Add, reference, graph.int32(1)));
    graph.safepoint();
    graph.store(reference, 40);
    graph.finish();
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    B3::eliminateDeadCode(graph.proc);
    checkFindsNothing(graph);
}

static void testReferenceInArgumentRegister()
{
    Graph graph;
    B3::Value* argument = graph.block->appendNew<B3::ArgumentRegValue>(graph.proc, Graph::originOf(referenceOrigin), GPRInfo::argumentGPR1);
    graph.safepoint();
    graph.store(graph.loadThrough(graph.decode(graph.unary(B3::Trunc, argument))));
    graph.finish();
    checkFindsOne(graph);
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    checkFindsNothing(graph);
}

static void testReferenceReturnedByCallThatEndsItsBlock()
{
    Graph graph;
    B3::BasicBlock* first = graph.block;
    B3::BasicBlock* continuation = graph.proc.addBlock();
    B3::BasicBlock* landingPad = graph.proc.addBlock();
    B3::PatchpointValue* call = first->appendNew<B3::PatchpointValue>(graph.proc, B3::Int64, Graph::originOf(referenceOrigin));
    call->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
    call->effects.terminal = true;
    first->setSuccessors(B3::FrequentedBlock(continuation), B3::FrequentedBlock(landingPad, B3::FrequencyClass::Rare));
    graph.block = landingPad;
    graph.finish();
    graph.block = continuation;
    graph.safepoint();
    graph.store(graph.loadThrough(graph.decode(graph.unary(B3::Trunc, call))));
    graph.finish();
    checkFindsOne(graph);
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    AOT_TEST_CHECK(first->last() == call);
    checkFindsNothing(graph);
    AOT_TEST_CHECK(!graph.insertGuards());
}

static void testJoinedReferences()
{
    auto build = [](Graph& graph, bool pointersAreSwapped) {
        B3::BasicBlock* left = graph.proc.addBlock();
        B3::BasicBlock* right = graph.proc.addBlock();
        B3::BasicBlock* join = graph.proc.addBlock();
        B3::Value* referencePhi = join->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int32, Graph::originOf(referenceOrigin));
        B3::Value* pointerPhi = join->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int64, Graph::originOf(otherOrigin));
        graph.branchTo(graph.loadReference(otherOrigin, 48), left, right);

        auto fill = [&](B3::BasicBlock* block, int32_t offset, int32_t offsetOfGuarded) {
            graph.block = block;
            B3::Value* reference = graph.loadReference(otherReferenceOrigin, offset);
            B3::Value* guarded = offset == offsetOfGuarded ? reference : graph.loadReference(otherReferenceOrigin, offsetOfGuarded);
            B3::Value* pointer = graph.unary(B3::Opaque, graph.decode(guarded));
            block->appendNew<B3::UpsilonValue>(graph.proc, Graph::originOf(otherOrigin), reference, referencePhi);
            block->appendNew<B3::UpsilonValue>(graph.proc, Graph::originOf(otherOrigin), pointer, pointerPhi);
            block->appendNew<B3::Value>(graph.proc, B3::Jump, Graph::originOf(otherOrigin));
            block->setSuccessors(B3::FrequentedBlock(join));
        };
        fill(left, 16, 16);
        fill(right, 32, pointersAreSwapped ? 16 : 32);

        graph.block = join;
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(referencePhi)));
        graph.keepAlive(pointerPhi);
        graph.finish();
    };
    Graph guarded;
    build(guarded, false);
    checkFindsNothing(guarded);

    Graph swapped;
    build(swapped, true);
    checkFindsOne(swapped);
    AOT_TEST_CHECK(swapped.insertGuards() == 1);
    checkFindsNothing(swapped);
}

static B3::Policy policyThatKeepsOrigins()
{
    B3::Policy policy;
    policy.optimization.keepsOriginsOfDemotedValues = true;
    return policy;
}

static void makeJoinAnew(Graph& graph, B3::Value* phi)
{
    graph.proc.resetReachability();
    IndexSet<B3::Value*> demoted;
    demoted.add(phi);
    B3::demoteValues(graph.proc, demoted);
    B3::fixSSA(graph.proc);
}

// The join that the back end makes in the place of another one says what that one said, whatever its block begins with.
static void testJoinOfIntegersMadeAnew()
{
    Graph graph(policyThatKeepsOrigins());
    B3::BasicBlock* loop = graph.proc.addBlock();
    B3::BasicBlock* exit = graph.proc.addBlock();
    B3::Value* sum = graph.proc.add<B3::Value>(B3::Phi, B3::Int32, Graph::originOf(otherOrigin));
    graph.block->appendNew<B3::UpsilonValue>(graph.proc, Graph::originOf(otherOrigin), graph.int32(0), sum);
    graph.jumpTo(loop);
    graph.store(graph.loadThrough(graph.decode(graph.loadReference())));
    loop->append(sum);
    B3::Value* next = graph.binary(B3::Add, sum, graph.loadReference(otherOrigin, 40));
    graph.safepoint();
    loop->appendNew<B3::UpsilonValue>(graph.proc, Graph::originOf(otherOrigin), next, sum);
    graph.branchTo(graph.loadReference(otherOrigin, 48), loop, exit);
    graph.block = exit;
    graph.store(next);
    graph.finish();

    makeJoinAnew(graph, sum);
    checkFindsNothing(graph);
    AOT_TEST_CHECK(!graph.insertGuards());
}

static void testJoinOfReferencesMadeAnew()
{
    Graph graph(policyThatKeepsOrigins());
    B3::BasicBlock* left = graph.proc.addBlock();
    B3::BasicBlock* right = graph.proc.addBlock();
    B3::BasicBlock* join = graph.proc.addBlock();
    graph.branchTo(graph.loadReference(otherOrigin, 48), left, right);
    graph.block = join;
    graph.int32(7);
    B3::Value* reference = join->appendNew<B3::Value>(graph.proc, B3::Phi, B3::Int32, Graph::originOf(referenceOrigin));
    for (auto [block, offset] : { std::pair { left, 16 }, std::pair { right, 32 } }) {
        graph.block = block;
        block->appendNew<B3::UpsilonValue>(graph.proc, Graph::originOf(otherOrigin), graph.loadReference(otherReferenceOrigin, offset), reference);
        block->appendNew<B3::Value>(graph.proc, B3::Jump, Graph::originOf(otherOrigin));
        block->setSuccessors(B3::FrequentedBlock(join));
    }
    graph.block = join;
    graph.safepoint();
    graph.store(reference);
    graph.finish();

    makeJoinAnew(graph, reference);
    checkFindsOne(graph);
    AOT_TEST_CHECK(graph.insertGuards() == 1);
    checkFindsNothing(graph);
}

static void testDisplacedReference()
{
    auto build = [](Graph& graph, bool isGuarded) {
        B3::Value* entry = graph.loadReference();
        B3::Value* pointer = isGuarded ? graph.unary(B3::Opaque, graph.decode(graph.binary(B3::Add, entry, graph.int32(1)))) : nullptr;
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(graph.binary(B3::Add, entry, graph.int32(1)))));
        if (isGuarded)
            graph.keepAlive(pointer);
        graph.finish();
    };
    Graph bare;
    build(bare, false);
    checkFindsOne(bare, NarrowReferenceViolationKind::IsDisplaced);
    AOT_TEST_CHECK(!bare.insertGuards());
    checkFindsOne(bare, NarrowReferenceViolationKind::IsDisplaced);

    Graph guarded;
    build(guarded, true);
    checkFindsNothing(guarded);
}

static void testSuspension()
{
    enum class Lists : uint8_t { Nothing, Reference, Pointer };
    auto build = [](Graph& graph, Lists lists) {
        B3::Value* reference = graph.loadReference();
        B3::Value* pointer = graph.unary(B3::Opaque, graph.decode(reference));
        // An operation that suspends may call first. The frame is saved after the call.
        graph.safepoint(suspensionOrigin);
        B3::PatchpointValue* suspension = graph.safepoint(suspensionOrigin);
        suspension->setIsSuspension(true);
        if (lists == Lists::Reference)
            suspension->append(reference, B3::ValueRep::LateColdAny);
        if (lists == Lists::Pointer)
            suspension->append(pointer, B3::ValueRep::LateColdAny);
        graph.store(graph.loadThrough(graph.decode(reference)));
        graph.keepAlive(pointer);
        graph.finish();
    };
    Graph listsReference;
    build(listsReference, Lists::Reference);
    checkFindsNothing(listsReference);

    Graph listsPointer;
    build(listsPointer, Lists::Pointer);
    checkFindsNothing(listsPointer);

    // A pointer that the map of the saved frame does not list keeps nothing alive.
    Graph listsNothing;
    build(listsNothing, Lists::Nothing);
    checkFindsOne(listsNothing, NarrowReferenceViolationKind::HasNoPointer, suspensionOrigin);
}

static void testWhatNeedsNoPointer()
{
    {
        Graph graph;
        B3::Value* reference = graph.loadReference();
        graph.store(graph.loadThrough(graph.decode(reference)));
        graph.safepoint();
        graph.finish();
        NarrowReferenceFindings findings = graph.validate();
        AOT_TEST_CHECK(findings.violations.isEmpty() && !findings.numberOfReferencesAcrossSafepoints);
    }
    {
        Graph graph;
        B3::Value* reference = graph.loadReference();
        graph.store(graph.loadThrough(graph.decode(reference)));
        graph.safepoint();
        graph.store(graph.unary(B3::ZExt32, graph.binary(B3::Equal, reference, graph.int32(0))));
        graph.finish();
        checkFindsNothing(graph);
    }
    {
        Graph graph;
        B3::Value* reference = graph.loadReference();
        B3::PatchpointValue* call = graph.patchpoint(safepointOrigin);
        call->effects.writes = heapsOfCollection.collectorState;
        call->effects.reads = B3::HeapRange::top();
        graph.store(graph.loadThrough(graph.decode(reference)));
        graph.finish();
        checkFindsNothing(graph);
    }
    {
        Graph graph;
        B3::Value* pointer = graph.decode(graph.loadReference());
        graph.safepoint();
        graph.store(pointer);
        graph.finish();
        checkFindsNothing(graph);
    }
    for (B3::HeapRange range : { B3::HeapRange(4, 5), B3::HeapRange(7, 8) }) {
        Graph graph;
        B3::Value* reference = graph.loadReference(referenceOrigin, 16, range);
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(reference)));
        graph.finish();
        checkFindsNothing(graph);
    }
    for (B3::HeapRange range : { B3::HeapRange(5, 6), B3::HeapRange(7, 8) }) {
        Graph graph;
        B3::Value* entry = graph.loadReference(referenceOrigin, 16, range);
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(graph.binary(B3::Add, entry, graph.int32(1)))));
        graph.keepAlive(entry);
        graph.finish();
        checkFindsNothing(graph);
    }
    {
        Graph graph;
        B3::Value* entry = graph.loadReference(referenceOrigin, 16, B3::HeapRange(6, 7));
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(graph.binary(B3::Add, entry, graph.int32(1)))));
        graph.finish();
        checkFindsOne(graph, NarrowReferenceViolationKind::IsDisplaced);
    }
}

enum class InsertsGuards : bool { No, Yes };

static NarrowReferenceFindings findInPipeline(Mutation mutation, InsertsGuards insertsGuards = InsertsGuards::No)
{
    NarrowReferenceFindings findings;
    NarrowReferenceOrigins origins;
    B3::Policy policy;
    if (insertsGuards == InsertsGuards::Yes) {
        policy.clientPhases.append(B3::ClientPhase {
            B3::ClientPhase::Point::AfterSinking,
            "guardNarrowReferences"_s,
            createSharedTask<bool(B3::Procedure&)>([&](B3::Procedure& proc) {
                return !!insertNarrowReferenceGuards(proc, heapsOfCollection, origins);
            })
        });
    }
    policy.clientPhases.append(B3::ClientPhase {
        B3::ClientPhase::Point::BeforeLowerToAir,
        "validateNarrowReferences"_s,
        createSharedTask<bool(B3::Procedure&)>([&](B3::Procedure& proc) {
            findNarrowReferenceViolations(proc, heapsOfCollection, origins, findings);
            return false;
        })
    });
    Graph graph(policy);
    graph.proc.pinRegister(cellCageBaseGPR);
    B3::Value* reference = graph.loadReference();
    guard(graph, reference, mutation, [&] {
        graph.safepoint();
        graph.store(graph.loadThrough(graph.decode(reference)));
    });
    graph.finish();
    B3::prepareForGeneration(graph.proc);
    AOT_TEST_CHECK(!graph.proc.report().failure);
    return findings;
}

static void testGuardOutlivesOptimization()
{
    AOT_TEST_CHECK(findInPipeline(Mutation::None).violations.isEmpty());
    AOT_TEST_CHECK(findInPipeline(Mutation::None).numberOfReferencesAcrossSafepoints == 1);
    for (Mutation mutation : allMutations) {
        AOT_TEST_CHECK(findInPipeline(mutation).violations.size() == 1);
        AOT_TEST_CHECK(findInPipeline(mutation, InsertsGuards::Yes).violations.isEmpty());
    }
}

static void testPhase()
{
    NarrowReferenceFindings findings;
    B3::ClientPhase phase = validateNarrowReferences(findings, NarrowReferenceOrigins { });
    AOT_TEST_CHECK(phase.point == B3::ClientPhase::Point::BeforeLowerToAir);
    AOT_TEST_CHECK(phase.name == "validateNarrowReferences"_s);
    AOT_TEST_CHECK(phase.run);
}

std::span<const NativeTest> narrowReferenceValidationTests()
{
    static constexpr NativeTest tests[] = {
        { "narrow-reference-read-through"_s, testReadThroughReference },
        { "narrow-reference-decoded-before-safepoint-and-folded"_s, testDecodedBeforeSafepointAndFolded },
        { "narrow-reference-stored-behind-safepoint"_s, testReferenceStoredBehindSafepoint },
        { "narrow-reference-boxed-behind-safepoint"_s, testBoxedBehindSafepoint },
        { "narrow-reference-two-safepoints"_s, testTwoSafepoints },
        { "narrow-reference-safepoint-in-other-block"_s, testSafepointInOtherBlock },
        { "narrow-reference-safepoint-on-one-path"_s, testSafepointOnOnePath },
        { "narrow-reference-safepoint-in-loop"_s, testSafepointInLoop },
        { "narrow-reference-loop-behind-last-use"_s, testLoopBehindLastUse },
        { "narrow-reference-use-in-front-of-safepoint-in-loop"_s, testUseInFrontOfSafepointInLoop },
        { "narrow-reference-safepoint-in-inner-loop-and-use-in-outer-loop"_s, testSafepointInInnerLoopAndUseInOuterLoop },
        { "narrow-reference-cycle-with-two-entries"_s, testCycleWithTwoEntries },
        { "narrow-reference-compressed-pointer"_s, testCompressedPointer },
        { "narrow-reference-compressed-pointer-decoded-behind-safepoint"_s, testCompressedPointerDecodedAfterSafepoint },
        { "narrow-reference-pointer-held-only-by-dead-code"_s, testPointerHeldOnlyByDeadCode },
        { "narrow-reference-displaced-only-by-dead-code"_s, testReferenceDisplacedOnlyByDeadCode },
        { "narrow-reference-in-argument-register"_s, testReferenceInArgumentRegister },
        { "narrow-reference-returned-by-call-that-ends-its-block"_s, testReferenceReturnedByCallThatEndsItsBlock },
        { "narrow-reference-joined"_s, testJoinedReferences },
        { "narrow-reference-join-of-integers-made-anew"_s, testJoinOfIntegersMadeAnew },
        { "narrow-reference-join-of-references-made-anew"_s, testJoinOfReferencesMadeAnew },
        { "narrow-reference-displaced"_s, testDisplacedReference },
        { "narrow-reference-suspension"_s, testSuspension },
        { "narrow-reference-needs-no-pointer"_s, testWhatNeedsNoPointer },
        { "narrow-reference-guard-outlives-optimization"_s, testGuardOutlivesOptimization },
        { "narrow-reference-phase"_s, testPhase },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
