/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3CheckKnownProperties.h"
#include "B3CheckValue.h"
#include "B3ExtractValue.h"
#include "B3FoldPathConstants.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3InductionRecords.h"
#include "B3IntRange.h"
#include "B3KnownProperties.h"
#include "B3RegionCloner.h"
#include <limits>

namespace {

struct PublishedInduction {
    explicit PublishedInduction(Procedure& proc)
    {
        Value* step = proc.add<Const64Value>(Origin(), 1);
        Value* bound = proc.add<ArgumentRegValue>(Origin(), GPRInfo::argumentGPR0);
        phi = proc.add<Value>(Phi, Int64, Origin());
        next = proc.add<Value>(Add, Origin(), phi, step);
        test = proc.add<Value>(LessThan, Origin(), phi, bound);
        proc.setKnownInduction({ phi, next, test, 1, true, true });
    }

    Value* phi;
    Value* next;
    Value* test;
};

struct StoredKinds {
    bool hasRange;
    bool hasDereferenceableBytes;
    bool hasInduction;
    bool hasDemotedInduction;
};

} // anonymous namespace

static Policy trustingPolicy()
{
    return policyWith([](Policy&) { });
}

static InductionRecord recordOf(Procedure& proc, Value* phi)
{
    std::optional<InductionRecord> record = proc.knownInduction(phi);
    CHECK(record);
    CHECK(record->phi == phi);
    return *record;
}

static unsigned numberOfLiveInductions(Procedure& proc)
{
    unsigned result = 0;
    proc.knownInductions().forEach([&](const InductionRecord&) {
        ++result;
    });
    return result;
}

static void testKnownDereferenceableBytes()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t*>(proc, root);
    Value* pointer = arguments[0];
    Value* largeObject = arguments[1];
    CHECK(!proc.knownDereferenceableBytes(pointer));
    proc.setKnownDereferenceableBytes(pointer, 16);
    proc.setKnownDereferenceableBytes(largeObject, std::numeric_limits<uint32_t>::max());
    CHECK_EQ(proc.knownDereferenceableBytes(pointer), 16u);

    auto isKnown = [&](Type type, Value* base, int32_t offset) {
        return proc.isKnownDereferenceable(graph.load(root, type, base, offset, firstHeap));
    };
    auto isByteKnown = [&](int32_t offset) {
        return proc.isKnownDereferenceable(root->appendNew<MemoryValue>(proc, Load8Z, Origin(), pointer, offset, firstHeap));
    };
    CHECK(isKnown(Int64, pointer, 0));
    CHECK(isKnown(Int64, pointer, 8));
    CHECK(!isKnown(Int64, pointer, 9));
    CHECK(!isKnown(Int64, pointer, 16));
    CHECK(!isKnown(Int64, pointer, -8));
    CHECK(isKnown(Int32, pointer, 12));
    CHECK(!isKnown(Int32, pointer, 13));
    CHECK(isByteKnown(15));
    CHECK(!isByteKnown(16));
    CHECK(!isByteKnown(-1));
    CHECK(isKnown(Int64, graph.make(root, Identity, pointer), 8));
    CHECK(!isKnown(Int64, graph.make(root, Add, pointer, graph.int64(root, 8)), 0));
    CHECK(isKnown(Int64, largeObject, std::numeric_limits<int32_t>::max()));
}

static void testEffectsOfClearsOnlyControlDependence()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t*>(proc, root);
    Value* knownPointer = arguments[0];
    Value* unknownPointer = arguments[1];
    proc.setKnownDereferenceableBytes(knownPointer, 16);

    MemoryValue* load = graph.load(root, Int64, knownPointer, 8, firstHeap);
    Effects expected = load->effects();
    CHECK(expected.controlDependent);
    expected.controlDependent = false;
    CHECK(proc.effectsOf(load) == expected);

    Value* trappingLoad = root->appendNew<MemoryValue>(proc, trapping(Load), Int64, Origin(), knownPointer, 8, firstHeap);
    Value* store = root->appendNew<MemoryValue>(proc, Store, Origin(), load, knownPointer, 8, firstHeap);
    Value* loadOfUnknownPointer = graph.load(root, Int64, unknownPointer, 8, firstHeap);
    Value* loadBeyondKnownBytes = graph.load(root, Int64, knownPointer, 16, firstHeap);
    Value* quotient = graph.make(root, Div, load, load);
    for (Value* value : { trappingLoad, store, loadOfUnknownPointer, loadBeyondKnownBytes, quotient }) {
        CHECK(value->effects().controlDependent);
        CHECK(proc.effectsOf(value) == value->effects());
    }

    Value* sum = graph.make(root, Add, load, load);
    CHECK(proc.effectsOf(sum) == Effects::none());
}

static void testKnownPropertiesFollowCloneAndDelete()
{
    Procedure proc(trustingPolicy());
    Value* pointer = proc.add<ArgumentRegValue>(Origin(), GPRInfo::argumentGPR0);

    CHECK(!proc.knownDereferenceableBytes(pointer));
    proc.setKnownDereferenceableBytes(pointer, 16);
    CHECK_EQ(proc.knownDereferenceableBytes(pointer), 16u);
    CHECK_EQ(proc.knownDereferenceableBytes(proc.clone(pointer)), 16u);
    proc.setKnownDereferenceableBytes(pointer, 32);
    CHECK_EQ(proc.knownDereferenceableBytes(pointer), 32u);

    proc.setKnownRange(pointer, IntRange(4096, 1 << 20));
    Value* copy = proc.clone(pointer);
    CHECK_EQ(proc.knownDereferenceableBytes(copy), 32u);
    CHECK(proc.knownRange(copy) == IntRange(4096, 1 << 20));

    unsigned index = pointer->index();
    proc.deleteValue(pointer);
    Value* reused = nullptr;
    for (unsigned attempt = 0; attempt < 2 && !reused; ++attempt) {
        Value* candidate = proc.add<ArgumentRegValue>(Origin(), GPRInfo::argumentGPR0);
        if (candidate->index() == index)
            reused = candidate;
    }
    CHECK(reused);
    CHECK(!proc.knownDereferenceableBytes(reused));
    CHECK(!proc.knownRange(reused));
    CHECK_EQ(proc.knownDereferenceableBytes(copy), 32u);
}

static void testKnownPropertiesDieWithOpcode()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];

    Value* field = graph.load(root, pointerType(), pointer, 0, firstHeap);
    proc.setKnownDereferenceableBytes(field, 16);
    MemoryValue* access = graph.load(root, Int64, field, 8, firstHeap);
    CHECK(proc.isKnownDereferenceable(access));
    Value* copy = proc.clone(field);
    CHECK_EQ(proc.knownDereferenceableBytes(copy), 16u);
    copy->replaceWithIdentity(pointer);
    CHECK(!proc.knownDereferenceableBytes(copy));
    CHECK_EQ(proc.knownDereferenceableBytes(field), 16u);
    field->replaceWithIdentity(pointer);
    CHECK(!proc.knownDereferenceableBytes(field));
    CHECK(!proc.isKnownDereferenceable(access));

    Value* interior = graph.make(root, Add, pointer, graph.int64(root, 32));
    proc.setKnownDereferenceableBytes(interior, 8);
    CHECK_EQ(proc.knownDereferenceableBytes(interior), 8u);
    interior->setOpcodeUnsafely(Sub);
    CHECK(!proc.knownDereferenceableBytes(interior));

    Value* word = graph.load(root, Int64, pointer, 8, firstHeap);
    proc.setKnownRange(word, IntRange(0, 100));
    word->replaceWithIdentity(pointer);
    proc.setKnownDereferenceableBytes(word, 8);
    CHECK_EQ(proc.knownDereferenceableBytes(word), 8u);
    CHECK(!proc.knownRange(word));
}

static Vector<Value*> phisOfType(Procedure& proc, Type type)
{
    Vector<Value*> result;
    for (Value* value : proc.values()) {
        if (value->opcode() == Phi && value->type() == type)
            result.append(value);
    }
    return result;
}

static void testKnownPropertiesOfPhiFollowRotation()
{
    for (bool publishes : { true, false }) {
        Procedure proc(trustingPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
        loop.endBody(loop.body);
        Value* counter = loop.counter();
        CHECK_EQ(phisOfType(proc, Int64).size(), 2u);
        if (publishes) {
            proc.setKnownRange(counter, IntRange(0, 1000));
            proc.setKnownDereferenceableBytes(counter, 16);
        }

        loop.rotate();
        unsigned numberOfCounters = 0;
        for (Value* phi : phisOfType(proc, Int64)) {
            bool isCounter = proc.knownInduction(phi).has_value();
            numberOfCounters += isCounter;
            if (isCounter && publishes) {
                CHECK(proc.knownRange(phi) == IntRange(0, 1000));
                CHECK_EQ(proc.knownDereferenceableBytes(phi), 16u);
            }
            if (!publishes) {
                CHECK(!proc.knownRange(phi));
                CHECK(!proc.knownDereferenceableBytes(phi));
            }
        }
        CHECK_EQ(numberOfCounters, 1u);
        checkSumsIntegersBelowBound(proc);
    }
}

static void testKnownPropertiesFollowDemotion()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* other = proc.addBlock();
    BasicBlock* middle = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t>(proc, root);
    Value* pointer = arguments[0];
    graph.branch(root, arguments[1], middle, other);
    graph.jump(other, middle);
    Value* published = graph.load(middle, Int64, pointer, 0, firstHeap);
    Value* unpublished = graph.load(middle, Int64, pointer, 8, firstHeap);
    proc.setKnownRange(published, IntRange(1, 9));
    proc.setKnownDereferenceableBytes(published, 24);
    graph.jump(middle, done);
    graph.ret(done, graph.make(done, Add, published, unpublished));
    proc.resetReachability();

    RegionCloner cloner(proc);
    cloner.addBlock(middle);
    cloner.demoteEscapingValues();
    other->replaceSuccessor(middle, cloner.cloneBlocks(singleElementSpan(middle)).get(middle));
    CHECK(cloner.finish());
    CHECK(fixSSA(proc));
    validate(proc);

    proc.resetValueOwners();
    Vector<Value*> phis = phisOfType(proc, Int64);
    CHECK_EQ(phis.size(), 2u);
    unsigned numberOfPublishedPhis = 0;
    for (Value* phi : phis) {
        CHECK(phi->owner == done);
        if (!proc.knownRange(phi)) {
            CHECK(!proc.knownDereferenceableBytes(phi));
            continue;
        }
        ++numberOfPublishedPhis;
        CHECK(proc.knownRange(phi) == IntRange(1, 9));
        CHECK_EQ(proc.knownDereferenceableBytes(phi), 24u);
    }
    CHECK_EQ(numberOfPublishedPhis, 1u);
    CHECK(proc.knownRange(published) == IntRange(1, 9));

    auto code = compileProc(proc);
    int64_t memory[2] = { 3, 40 };
    for (int64_t takesOriginal : { 0, 1 })
        CHECK_EQ(invoke<int64_t>(*code, memory, takesOriginal), 43);
}

static void testKnownPropertiesOfPartOfTuple()
{
    for (bool publishes : { true, false }) {
        Procedure proc(strengthReductionPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        PatchpointValue* patchpoint = graph.emptyPatchpoint(root, proc.addTuple({ Int64, Int64 }));
        patchpoint->resultConstraints = { ValueRep::SomeRegister, ValueRep::SomeRegister };
        patchpoint->effects.writes = firstHeap;
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(CCallHelpers::TrustedImm32(7), params[0].gpr());
            jit.move(CCallHelpers::TrustedImm32(7), params[1].gpr());
        });
        Value* first = root->appendNew<ExtractValue>(proc, Origin(), Int64, patchpoint, 0);
        Value* second = root->appendNew<ExtractValue>(proc, Origin(), Int64, patchpoint, 1);
        if (publishes) {
            proc.setKnownRange(first, IntRange(0, 9));
            proc.setKnownDereferenceableBytes(first, 16);
            Value* copy = proc.clone(first);
            CHECK(proc.knownRange(copy) == IntRange(0, 9));
            proc.deleteValue(copy);
        }
        CHECK_EQ(rangeFor(proc, first), publishes ? IntRange(0, 9) : IntRange::top<int64_t>());
        CHECK_EQ(rangeFor(proc, second), IntRange::top<int64_t>());
        CHECK_EQ(proc.knownDereferenceableBytes(first), publishes ? 16u : 0u);
        CHECK(!proc.knownDereferenceableBytes(second));

        Value* isFirstSmall = graph.make(root, LessThan, first, graph.int64(root, 10));
        Value* isSecondSmall = graph.make(root, LessThan, second, graph.int64(root, 10));
        graph.ret(root, graph.make(root, ZExt32, graph.make(root, Add, isFirstSmall, isSecondSmall)));
        resetAndValidate(proc);
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, LessThan), publishes ? 1u : 2u);
        CHECK_EQ(compileAndRun<int64_t>(proc), 2);
    }
}

static void testVariableOfLaterRepairHasNoKnownProperties()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    for (Value* phi : phisOfType(proc, Int64))
        proc.setKnownRange(phi, IntRange(0, 1000));
    loop.rotate();
    CHECK(proc.variables().isEmpty());

    BasicBlock* exit = loop.exit;
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    Value* result = exit->last()->child(0);
    exit->removeLast(proc);
    Vector<Variable*> variables;
    for (unsigned i = 0; i < 4; ++i)
        variables.append(proc.addVariable(Int64));
    graph.branch(exit, result, left, right);
    for (Variable* variable : variables) {
        graph.set(left, variable, result);
        graph.set(right, variable, graph.int64(right, -5));
    }
    graph.jump(left, done);
    graph.jump(right, done);
    Value* sum = graph.get(done, variables[0]);
    for (unsigned i = 1; i < variables.size(); ++i)
        sum = graph.make(done, Add, sum, graph.get(done, variables[i]));
    graph.ret(done, sum);
    proc.resetReachability();
    proc.invalidateCFG();

    CHECK(fixSSA(proc));
    validate(proc);
    unsigned numberOfPhisAtEnd = 0;
    for (Value* value : *done) {
        if (value->opcode() != Phi)
            continue;
        ++numberOfPhisAtEnd;
        CHECK(!proc.knownRange(value));
    }
    CHECK_EQ(numberOfPhisAtEnd, 4u);
}

static StoredKinds publishEveryKind(Procedure& proc)
{
    Value* pointer = proc.add<ArgumentRegValue>(Origin(), GPRInfo::argumentGPR0);
    proc.setKnownRange(pointer, IntRange(0, 100));
    proc.setKnownDereferenceableBytes(pointer, 16);
    PublishedInduction induction(proc);
    proc.setKnownInduction(DemotedInduction { proc.addVariable(Int64), proc.addBlock(), induction.next, induction.test, 1, false });
    return { proc.knownRange(pointer).has_value(), !!proc.knownDereferenceableBytes(pointer), proc.knownInduction(induction.phi).has_value(), proc.knownInductions().hasDemoted() };
}

static void testUntrustedPropertiesAreNotStored()
{
    Procedure untrusting;
    StoredKinds untrusted = publishEveryKind(untrusting);
    CHECK(!untrusted.hasRange);
    CHECK(!untrusted.hasDereferenceableBytes);
    CHECK(!untrusted.hasInduction);
    CHECK(!untrusted.hasDemotedInduction);
    CHECK(untrusting.knownInductions().isEmpty());

    Procedure trusting(trustingPolicy());
    StoredKinds trusted = publishEveryKind(trusting);
    CHECK(trusted.hasRange);
    CHECK(trusted.hasDereferenceableBytes);
    CHECK(trusted.hasInduction);
    CHECK(trusted.hasDemotedInduction);

    for (PolicySwitch disabled : { PolicySwitch::KnownRanges, PolicySwitch::KnownDereferenceableBytes, PolicySwitch::KnownInductions }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.disable(disabled);
        }));
        StoredKinds stored = publishEveryKind(proc);
        CHECK_EQ(stored.hasRange, disabled != PolicySwitch::KnownRanges);
        CHECK_EQ(stored.hasDereferenceableBytes, disabled != PolicySwitch::KnownDereferenceableBytes);
        CHECK_EQ(stored.hasInduction, disabled != PolicySwitch::KnownInductions);
        CHECK_EQ(stored.hasDemotedInduction, disabled != PolicySwitch::KnownInductions);
    }
}

static void testSubstitutedOperandIsAskedAgain()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* guarded = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int64_t*, int64_t>(proc, root);
    Value* knownPointer = arguments[0];
    Value* unknownPointer = arguments[1];
    proc.setKnownDereferenceableBytes(knownPointer, 16);
    graph.branch(root, arguments[2], guarded, done);
    MemoryValue* load = graph.load(guarded, Int64, knownPointer, 8, firstHeap);
    graph.ret(guarded, load);
    graph.ret(done, graph.int64(done, 0));
    resetAndValidate(proc);
    CHECK(proc.isKnownDereferenceable(load));
    CHECK(!proc.effectsOf(load).controlDependent);

    load->lastChild() = unknownPointer;
    validate(proc);
    CHECK(!proc.isKnownDereferenceable(load));
    CHECK(proc.effectsOf(load).controlDependent);
    CHECK(proc.effectsOf(load) == load->effects());

    load->lastChild() = knownPointer;
    CHECK(!proc.effectsOf(load).controlDependent);
}

static void buildLoopWithGuardedLoad(Procedure& proc, uint32_t dereferenceableBytes)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* guarded = proc.addBlock();
    BasicBlock* latch = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* index = proc.addVariable(Int64);
    Variable* sum = proc.addVariable(Int64);
    auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root);
    Value* bound = arguments[0];
    Value* pointer = arguments[1];
    if (dereferenceableBytes)
        proc.setKnownDereferenceableBytes(pointer, dereferenceableBytes);
    graph.set(root, index, graph.int64(root, 0));
    graph.set(root, sum, graph.int64(root, 0));
    graph.jump(root, header);
    graph.branch(header, graph.make(header, Above, graph.get(header, index), graph.int64(header, 0)), FrequentedBlock(guarded), rarely(latch));
    graph.increase(guarded, sum, graph.make(guarded, Mul, graph.load(guarded, Int64, pointer, 8, firstHeap), bound));
    graph.jump(guarded, latch);
    graph.increase(latch, index, 1);
    graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, index), bound), header, exit);
    graph.ret(exit, graph.get(exit, sum));
    proc.resetReachability();
    fixSSA(proc);
    validate(proc);
}

static int64_t sumOfGuardedLoads(int64_t bound, int64_t word)
{
    int64_t sum = 0;
    int64_t index = 0;
    do {
        if (index > 0)
            sum += word * bound;
        ++index;
    } while (index < bound);
    return sum;
}

static void testHoistsKnownDereferenceableLoadAboveGuard()
{
    enum class Pass : uint8_t { HoistLoopInvariantValues, VersionLoops };
    struct Case {
        uint32_t dereferenceableBytes;
        bool isHoisted;
    };
    for (Pass pass : { Pass::HoistLoopInvariantValues, Pass::VersionLoops }) {
        for (Case testCase : { Case { 16, true }, Case { 8, false }, Case { 0, false } }) {
            Procedure proc(policyWith([](Policy& policy) {
                policy.loops.hoistsOnlyFromLikelyPaths = true;
            }));
            buildLoopWithGuardedLoad(proc, testCase.dereferenceableBytes);
            CHECK_EQ(valueCountInLoops(proc, Load), 1u);

            if (pass == Pass::VersionLoops)
                CHECK_EQ(versionLoopsAndValidate(proc), testCase.isHoisted);
            else {
                hoistLoopInvariantValues(proc);
                validate(proc);
            }
            CHECK_EQ(valueCountInLoops(proc, Load), testCase.isHoisted ? 0u : 1u);
            CHECK_EQ(numberOfValues(proc, Load), 1u);

            auto code = compileProc(proc);
            int64_t memory[2] = { 0, 5 };
            for (int64_t bound : { 0, 1, 2, 9 })
                CHECK_EQ(run(*code, bound, memory), sumOfGuardedLoads(bound, memory[1]));
            if (!testCase.dereferenceableBytes)
                CHECK_EQ(run(*code, 1), 0);
        }
    }
}

static void testKnownPropertiesDieWithOperandOfPath()
{
    // for (index = 0; index < bound; ++index) sum += pointer ? 1 : *copy(pointer + 8). The side on which the pointer is null never runs, and is the likely one.
    for (bool foldsImpliedComparisons : { true, false }) {
        for (bool isCompiled : { true, false }) {
            Policy policy = optimizingPolicy();
            if (!foldsImpliedComparisons)
                policy.disable(PolicySwitch::FoldImpliedComparisons);
            Procedure proc(policy);
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            BasicBlock* header = proc.addBlock();
            BasicBlock* body = proc.addBlock();
            BasicBlock* isNull = proc.addBlock();
            BasicBlock* isNotNull = proc.addBlock();
            BasicBlock* latch = proc.addBlock();
            BasicBlock* exit = proc.addBlock();
            Variable* index = proc.addVariable(Int64);
            Variable* sum = proc.addVariable(Int64);
            auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
            Value* pointer = arguments[1];
            graph.set(root, index, graph.int64(root, 0));
            graph.set(root, sum, graph.int64(root, 0));
            graph.jump(root, header);
            graph.branch(header, graph.make(header, LessThan, graph.get(header, index), arguments[0]), body, exit);
            graph.branch(body, pointer, rarely(isNotNull), FrequentedBlock(isNull));
            Value* address = graph.make(isNull, Add, pointer, graph.int64(isNull, 8));
            PatchpointValue* copy = isNull->appendNew<PatchpointValue>(proc, Int64, Origin());
            copy->effects = Effects::none();
            copy->appendSomeRegister(address);
            copy->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                jit.move(params[1].gpr(), params[0].gpr());
            });
            MemoryValue* load = graph.load(isNull, Int64, copy, 0, firstHeap);
            proc.setKnownDereferenceableBytes(pointer, 256);
            proc.setKnownDereferenceableBytes(address, 248);
            proc.setKnownDereferenceableBytes(copy, 248);
            proc.setKnownRange(load, IntRange(0, 100));
            graph.increase(isNull, sum, load);
            graph.jump(isNull, latch);
            graph.increase(isNotNull, sum, 1);
            graph.jump(isNotNull, latch);
            graph.increase(latch, index, 1);
            graph.jump(latch, header);
            graph.ret(exit, graph.get(exit, sum));
            proc.resetReachability();
            validate(proc);

            if (isCompiled) {
                auto code = compileProc(proc, 2);
                int64_t memory[32] = { };
                CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(10), memory, static_cast<int64_t>(3)), 10);
                continue;
            }
            fixSSA(proc);
            foldPathConstants(proc);
            validate(proc);
            CHECK(address->child(0)->isInt64(0));
            CHECK_EQ(proc.knownDereferenceableBytes(pointer), 256u);
            CHECK(!proc.knownDereferenceableBytes(address));
            CHECK(!proc.knownDereferenceableBytes(copy));
            // The load stays where it is, so what it gives is what it always gave.
            CHECK(proc.effectsOf(load).controlDependent);
            CHECK(proc.knownRange(load) == IntRange(0, 100));
        }
    }
}

static void testInductionRecordFollowsIdentity()
{
    Procedure proc(trustingPolicy());
    PublishedInduction induction(proc);
    Value* nextReplacement = proc.clone(induction.next);
    Value* testReplacement = proc.clone(induction.test);
    auto checkRecordRefersToReplacements = [&] {
        InductionRecord record = recordOf(proc, induction.phi);
        CHECK(record.next == nextReplacement);
        CHECK(record.test == testReplacement);
        CHECK_EQ(record.step, 1);
        CHECK(record.doesNotWrap);
        CHECK(record.relationHoldsOnEntry);
    };

    induction.next->replaceWithIdentity(nextReplacement);
    induction.test->replaceWithIdentity(testReplacement);
    checkRecordRefersToReplacements();

    proc.deleteValue(induction.next);
    proc.deleteValue(induction.test);
    checkRecordRefersToReplacements();
}

static void testInductionRecordDiesWithPhi()
{
    {
        Procedure proc(trustingPolicy());
        PublishedInduction induction(proc);
        CHECK_EQ(proc.knownInductions().size(), 1u);
        proc.deleteValue(induction.phi);
        CHECK(proc.knownInductions().isEmpty());
    }

    Procedure proc(trustingPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    loop.endBody(loop.body);
    Value* phi = loop.counter();
    CHECK(proc.knownInduction(phi));
    CHECK_EQ(numberOfLiveInductions(proc), 1u);

    IndexSet<Value*> valuesToDemote;
    valuesToDemote.add(phi);
    demoteValues(proc, valuesToDemote);
    validate(proc);
    CHECK(phi->opcode() == Identity);
    CHECK(!proc.knownInduction(phi));
    CHECK_EQ(numberOfLiveInductions(proc), 0u);

    fixSSA(proc);
    validate(proc);
    CHECK(loop.counter() != phi);
    CHECK(loop.counter()->opcode() == Phi);
    CHECK(!proc.knownInduction(loop.counter()));
    CHECK_EQ(numberOfLiveInductions(proc), 0u);
    checkSumsIntegersBelowBound(proc);
}

static void testInductionRecordDiesWithNext()
{
    for (bool followsIdentityFirst : { false, true }) {
        Procedure proc(trustingPolicy());
        PublishedInduction induction(proc);
        Value* next = induction.next;
        if (followsIdentityFirst) {
            next = proc.clone(induction.next);
            induction.next->replaceWithIdentity(next);
            proc.deleteValue(induction.next);
            CHECK(recordOf(proc, induction.phi).next == next);
        }
        proc.deleteValue(next);
        CHECK(proc.knownInductions().isEmpty());
        CHECK(!proc.knownInduction(induction.phi));
    }
}

static void testInductionRecordLosesTest()
{
    for (bool isDeleted : { true, false }) {
        Procedure proc(trustingPolicy());
        PublishedInduction induction(proc);
        if (isDeleted)
            proc.deleteValue(induction.test);
        else
            induction.test->replaceWithIdentity(proc.add<Const32Value>(Origin(), 1));

        InductionRecord record = recordOf(proc, induction.phi);
        CHECK(!record.test);
        CHECK(!record.relationHoldsOnEntry);
        CHECK(record.next == induction.next);
        CHECK_EQ(record.step, 1);
        CHECK(record.doesNotWrap);
    }
}

static void testCloneCopiesNoRecord()
{
    Procedure proc(trustingPolicy());
    PublishedInduction induction(proc);
    proc.setKnownRange(induction.phi, IntRange(0, 100));

    Value* phiCopy = proc.clone(induction.phi);
    CHECK(proc.knownRange(phiCopy) == IntRange(0, 100));
    CHECK(!proc.knownInduction(phiCopy));
    proc.clone(induction.next);
    proc.clone(induction.test);
    CHECK_EQ(proc.knownInductions().size(), 1u);

    InductionRecord record = recordOf(proc, induction.phi);
    CHECK(record.next == induction.next);
    CHECK(record.test == induction.test);
}

static Value* endBodyWithoutFixSSA(Graph& graph, TopTestedLoop& loop)
{
    graph.increase(loop.body, loop.sum, graph.get(loop.body, loop.index));
    Value* next = graph.make(loop.body, Add, graph.get(loop.body, loop.index), graph.int64(loop.body, loop.step));
    graph.set(loop.body, loop.index, next);
    graph.jump(loop.body, loop.header);
    graph.proc.resetReachability();
    return next;
}

static void testVariableInductionBecomesRecord()
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    Value* next = endBodyWithoutFixSSA(graph, loop);
    Value* test = loop.header->last()->child(0);
    proc.setKnownInduction(DemotedInduction { loop.index, loop.header, next, test, loop.step, true });
    CHECK(proc.knownInductions().hasDemoted());
    CHECK(proc.knownInductions().isEmpty());

    CHECK(fixSSA(proc));
    validate(proc);
    CHECK(!proc.knownInductions().hasDemoted());
    CHECK_EQ(proc.knownInductions().size(), 1u);
    CHECK(loop.counter()->opcode() == Phi);
    CHECK(loop.header->values().contains(loop.counter()));
    InductionRecord record = recordOf(proc, loop.counter());
    CHECK(record.next == next);
    CHECK(record.test == test);
    CHECK_EQ(record.step, loop.step);
    CHECK(record.doesNotWrap);
    CHECK(!record.relationHoldsOnEntry);
    checkSumsIntegersBelowBound(proc);
}

static void testDemotedInductionIsDropped()
{
    {
        Procedure proc(trustingPolicy());
        Graph graph { proc };
        TopTestedLoop loop(graph);
        Value* next = endBodyWithoutFixSSA(graph, loop);
        proc.setKnownInduction(DemotedInduction { loop.index, loop.body, next, loop.header->last()->child(0), loop.step, true });
        CHECK(proc.knownInductions().hasDemoted());

        CHECK(fixSSA(proc));
        validate(proc);
        CHECK(!proc.knownInductions().hasDemoted());
        CHECK(proc.knownInductions().isEmpty());
        CHECK(!proc.knownInduction(loop.counter()));
        checkSumsIntegersBelowBound(proc);
    }

    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Variable* unreadVariable = proc.addVariable(Int64);
    Value* next = graph.int64(root, 1);
    graph.set(root, unreadVariable, next);
    graph.ret(root, next);
    proc.resetReachability();
    proc.setKnownInduction(DemotedInduction { unreadVariable, root, next, nullptr, 1, false });
    CHECK(proc.knownInductions().hasDemoted());

    CHECK(!fixSSA(proc));
    validate(proc);
    CHECK(!proc.knownInductions().hasDemoted());
    CHECK(proc.knownInductions().isEmpty());
}

static unsigned numberOfChecksForRange(Type type, IntRange range)
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int64_t*>(proc, root)[0];
    Value* word = graph.load(root, type, pointer, 0, firstHeap);
    proc.setKnownRange(word, range);
    graph.ret(root, type == Int32 ? graph.make(root, SExt32, word) : word);
    resetAndValidate(proc);

    bool changed = checkKnownProperties(proc);
    unsigned result = numberOfValues(proc, Check);
    CHECK_EQ(changed, !!result);
    CHECK_EQ(numberOfValues(proc, Opaque), result);

    auto code = compileProc(proc);
    for (int64_t value : { range.min(), range.max() }) {
        int64_t memory[1] = { value };
        CHECK_EQ(invoke<int64_t>(*code, memory), value);
    }
    return result;
}

static Vector<int32_t> probeOffsetsFor(uint32_t dereferenceableBytes)
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<uint8_t*>(proc, root)[0];
    proc.setKnownDereferenceableBytes(pointer, dereferenceableBytes);
    graph.ret(root, pointer);
    resetAndValidate(proc);

    CHECK_EQ(checkKnownProperties(proc), !!dereferenceableBytes);
    CHECK_EQ(numberOfValues(proc, Patchpoint), dereferenceableBytes ? 1u : 0u);
    CHECK_EQ(numberOfValues(proc, Check), 0u);
    Vector<int32_t> offsets;
    for (Value* value : *root) {
        if (value->opcode() == Load8Z)
            offsets.append(value->as<MemoryValue>()->offset());
    }

    uint8_t memory[16] = { };
    CHECK(compileAndRun<uint8_t*>(proc, memory) == memory);
    return offsets;
}

static void checkRunTimeChecksOfInduction(bool doesNotWrap, bool relationHoldsOnEntry, bool hasSecondLatch)
{
    Procedure proc(trustingPolicy());
    Graph graph { proc };
    TopTestedLoop loop(graph);
    BasicBlock* latch = loop.body;
    if (hasSecondLatch) {
        BasicBlock* secondLatch = proc.addBlock();
        latch = proc.addBlock();
        graph.branch(loop.body, loop.extra, secondLatch, latch);
        graph.increase(secondLatch, loop.index, loop.step);
        graph.jump(secondLatch, loop.header);
    }
    graph.increase(latch, loop.sum, graph.get(latch, loop.index));
    loop.endBody(latch);
    loop.publishInduction(doesNotWrap, relationHoldsOnEntry);

    CHECK(checkKnownProperties(proc));
    unsigned numberOfUpsilonsInLoop = hasSecondLatch ? 2 : 1;
    unsigned numberOfChecksInLoop = numberOfUpsilonsInLoop * (doesNotWrap ? 2 : 1);
    unsigned numberOfChecksOnEntry = relationHoldsOnEntry ? 1 : 0;
    CHECK_EQ(valueCountInLoops(proc, Check), numberOfChecksInLoop);
    CHECK_EQ(numberOfValues(loop.root, Check), numberOfChecksOnEntry);
    CHECK_EQ(numberOfValues(proc, Check), numberOfChecksInLoop + numberOfChecksOnEntry);
}

static void buildProgramWithTruePromises(Procedure& proc)
{
    Graph graph { proc };
    TopTestedLoop loop(graph);
    proc.setKnownRange(loop.bound, IntRange(1, 1000));
    proc.setKnownDereferenceableBytes(loop.pointer, 16);
    Value* word = graph.load(loop.body, Int64, loop.pointer, 8, firstHeap);
    proc.setKnownRange(word, IntRange(0, 100));
    graph.increase(loop.body, loop.sum, word);
    loop.endBody(loop.body);
    loop.publishInduction(true, true);
}

static void checkProgramWithTruePromises(const Policy& basePolicy)
{
    for (bool checksAtRunTime : { false, true }) {
        unsigned numberOfChecks = 0;
        unsigned numberOfProbes = 0;
        Policy policy = basePolicy;
        policy.validation.checksKnownPropertiesAtRunTime = checksAtRunTime;
        policy.clientPhases.append(ClientPhase { ClientPhase::Point::BeforeOptimization, "countRunTimeChecks"_s, createSharedTask<bool(Procedure&)>([&](Procedure& procedure) {
            numberOfChecks = numberOfValues(procedure, Check);
            numberOfProbes = numberOfValues(procedure, Load8Z);
            return false;
        }) });

        Procedure proc(policy);
        buildProgramWithTruePromises(proc);
        auto code = compileProc(proc);
        CHECK_EQ(numberOfChecks, checksAtRunTime ? 5u : 0u);
        CHECK_EQ(numberOfProbes, checksAtRunTime ? 2u : 0u);
        int64_t memory[2] = { 0, 7 };
        for (int64_t bound : { 1, 2, 10, 1000 })
            CHECK_EQ(run(*code, bound, memory), 7 * bound);
    }
}

static void testRunTimeChecksOfKnownProperties()
{
    CHECK_EQ(numberOfChecksForRange(Int64, IntRange(0, 100)), 1u);
    CHECK_EQ(numberOfChecksForRange(Int64, IntRange(-5, 100)), 1u);
    CHECK_EQ(numberOfChecksForRange(Int64, IntRange(std::numeric_limits<int64_t>::min(), 0)), 1u);
    CHECK_EQ(numberOfChecksForRange(Int64, IntRange::top<int64_t>()), 0u);
    CHECK_EQ(numberOfChecksForRange(Int32, IntRange(0, 100)), 1u);
    CHECK_EQ(numberOfChecksForRange(Int32, IntRange::top<int32_t>()), 0u);

    CHECK(probeOffsetsFor(16) == (Vector<int32_t> { 0, 15 }));
    CHECK(probeOffsetsFor(1) == (Vector<int32_t> { 0 }));
    CHECK(probeOffsetsFor(0).isEmpty());

    for (bool doesNotWrap : { false, true }) {
        for (bool relationHoldsOnEntry : { false, true }) {
            for (bool hasSecondLatch : { false, true })
                checkRunTimeChecksOfInduction(doesNotWrap, relationHoldsOnEntry, hasSecondLatch);
        }
    }

    for (const Policy& policy : { trustingPolicy(), optimizingPolicy() })
        checkProgramWithTruePromises(policy);
}

static void testRunTimeRangeCheckSurvivesRangeReductions()
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int32_t*>(proc, root)[0];
    Value* word = graph.load(root, Int32, pointer, 0, firstHeap);
    proc.setKnownRange(word, IntRange(0, 100));
    CheckValue* foldableCheck = root->appendNew<CheckValue>(proc, Check, Origin(), graph.make(root, Above, word, graph.int32(root, 100)));
    foldableCheck->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams&) {
        jit.breakpoint();
    });
    graph.ret(root, word);
    resetAndValidate(proc);

    CHECK(checkKnownProperties(proc));
    CHECK_EQ(numberOfValues(proc, Check), 2u);
    CHECK_EQ(numberOfValues(proc, Opaque), 1u);

    CHECK(reduceStrengthOnce(proc));
    CHECK_EQ(numberOfValues(proc, Check), 1u);
    CHECK_EQ(numberOfValues(proc, Opaque), 1u);

    auto code = compileProc(proc);
    for (int32_t value : { 0, 100 }) {
        int32_t memory[1] = { value };
        CHECK_EQ(invoke<int32_t>(*code, memory), value);
    }
}

static void testTrapOriginOfFusedLoad()
{
    if (!isX86_64())
        return;
    for (bool attributesTrapsToAccesses : { true, false }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.selection.attributesTrapsToAccesses = attributesTrapsToAccesses;
            policy.allocation.keepsValuesAfterLowering = true;
        }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t*>(proc, root);
        Value* addend = arguments[0];
        Value* load = root->appendNew<MemoryValue>(proc, trapping(Load), Int64, Origin(), arguments[1], 0, firstHeap);
        graph.ret(root, graph.make(root, Add, addend, load));
        generateToAir(proc);

        Vector<Value*> originsOfTrappingInstructions;
        for (Air::BasicBlock* block : proc.code()) {
            for (Air::Inst& inst : *block) {
                if (inst.kind.effects)
                    originsOfTrappingInstructions.append(inst.origin);
            }
        }
        CHECK_EQ(originsOfTrappingInstructions.size(), 1u);
        CHECK_EQ(originsOfTrappingInstructions[0]->opcode(), attributesTrapsToAccesses ? Load : Add);
    }
}

static void testAddressOfTrappingAccessNamesWhatIsDereferenced()
{
    for (bool attributesTrapsToAccesses : { true, false }) {
        Procedure proc(policyWith([&](Policy& policy) {
            policy.selection.attributesTrapsToAccesses = attributesTrapsToAccesses;
            policy.allocation.keepsValuesAfterLowering = true;
        }));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t>(proc, root);
        Value* address = graph.make(root, Add, arguments[0], arguments[1]);
        graph.ret(root, root->appendNew<MemoryValue>(proc, trapping(Load), Int64, Origin(), address, 0, firstHeap));
        generateToAir(proc);

        unsigned numberOfTrappingInstructions = 0;
        for (Air::BasicBlock* block : proc.code()) {
            for (Air::Inst& inst : *block) {
                if (!inst.kind.effects)
                    continue;
                ++numberOfTrappingInstructions;
                bool hasIndex = false;
                for (const Air::Arg& arg : inst.args())
                    hasIndex |= arg.isIndex();
                CHECK_EQ(hasIndex, !attributesTrapsToAccesses);
            }
        }
        CHECK_EQ(numberOfTrappingInstructions, 1u);
    }
}

void addBunKnownPropertyTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testKnownDereferenceableBytes());
    RUN(testEffectsOfClearsOnlyControlDependence());
    RUN(testKnownPropertiesFollowCloneAndDelete());
    RUN(testKnownPropertiesDieWithOpcode());
    RUN(testKnownPropertiesOfPhiFollowRotation());
    RUN(testKnownPropertiesFollowDemotion());
    RUN(testVariableOfLaterRepairHasNoKnownProperties());
    RUN(testKnownPropertiesOfPartOfTuple());
    RUN(testUntrustedPropertiesAreNotStored());
    RUN(testSubstitutedOperandIsAskedAgain());
    RUN(testHoistsKnownDereferenceableLoadAboveGuard());
    RUN(testKnownPropertiesDieWithOperandOfPath());
    RUN(testInductionRecordFollowsIdentity());
    RUN(testInductionRecordDiesWithPhi());
    RUN(testInductionRecordDiesWithNext());
    RUN(testInductionRecordLosesTest());
    RUN(testCloneCopiesNoRecord());
    RUN(testVariableInductionBecomesRecord());
    RUN(testDemotedInductionIsDropped());
    RUN(testRunTimeChecksOfKnownProperties());
    RUN(testRunTimeRangeCheckSurvivesRangeReductions());
    RUN(testTrapOriginOfFusedLoad());
    RUN(testAddressOfTrappingAccessNamesWhatIsDereferenced());
}

#endif // ENABLE(B3_JIT)
