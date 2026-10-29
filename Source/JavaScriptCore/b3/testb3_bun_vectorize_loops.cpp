/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3Compile.h"
#include "B3EliminateDeadCode.h"
#include "B3Generate.h"
#include "B3LoopUtilities.h"
#include "B3VectorizeLoops.h"
#include "LinkBuffer.h"

namespace {

enum class TestPlace : uint8_t { Top, Bottom };

struct ElementKind {
    B3::Opcode load;
    B3::Opcode store;
    Type type;
    unsigned bytes;
};

const ElementKind elementKinds[] = {
    { Load8Z, Store8, Int32, 1 },
    { Load8S, Store8, Int32, 1 },
    { Load16Z, Store16, Int32, 2 },
    { Load16S, Store16, Int32, 2 },
    { Load, Store, Int32, 4 },
    { Load, Store, Float, 4 },
    { Load, Store, Double, 8 },
};

Policy vectorPolicy(CPUFeatureSet features = CPUFeatureSet::host())
{
    Policy policy = loopPolicy();
    policy.upstream.allowsSIMD = true;
    policy.targetFeatures = features;
    return policy;
}

Vector<CPUFeatureSet> featureSets()
{
    Vector<CPUFeatureSet> result { CPUFeatureSet::host() };
    if (CPUFeatureSet::buildTarget() != CPUFeatureSet::host())
        result.append(CPUFeatureSet::buildTarget());
    return result;
}

#if CPU(X86_64) && ENABLE(DISASSEMBLER)
// Each line of the disassembly is an address, a colon and an instruction. Every instruction in a VEX form has a name that begins with v, and no other that a compiler emits does.
void checkUsesOnlyWhatTargetHas(Compilation& code, CPUFeatureSet features)
{
    auto disassembly = code.disassembly();
    CHECK(disassembly.length());
    auto uses = [&](const char* text) {
        return contains(disassembly.span(), byteCast<char8_t>(unsafeSpan(text)));
    };
    auto checkDoesNotUse = [&](const char* text) {
        if (!uses(text))
            return;
        crashLock.lock();
        dataLogLn("The target with the features ", features.bits(), " does not have", text);
        dataLog(disassembly);
        CRASH();
    };
    CHECK(uses(": mov") || uses(": push"));
    if (!features.contains(CPUFeature::AVX))
        checkDoesNotUse(": v");
    if (!features.contains(CPUFeature::SSE4_1)) {
        for (const char* name : { ": pinsrb", ": pinsrd", ": pinsrq", ": pextr", ": pmulld", ": pmovzx", ": pmovsx", ": pblend", ": blend", ": round", ": ptest", ": pminsb", ": pminsd", ": pmaxsb", ": pmaxsd", ": pminuw", ": pminud", ": pmaxuw", ": pmaxud", ": packusdw", ": insertps", ": extractps" })
            checkDoesNotUse(name);
    }
    if (!features.contains(CPUFeature::SupplementalSSE3)) {
        for (const char* name : { ": pshufb", ": palignr", ": pabs", ": phadd", ": phsub", ": pmaddubsw", ": pmulhrsw", ": psign" })
            checkDoesNotUse(name);
    }
    if (!features.contains(CPUFeature::SSE3)) {
        for (const char* name : { ": movddup", ": movshdup", ": movsldup", ": haddp", ": hsubp", ": addsubp", ": lddqu" })
            checkDoesNotUse(name);
    }
}
#else
void checkUsesOnlyWhatTargetHas(Compilation&, CPUFeatureSet) { }
#endif

std::unique_ptr<Compilation> compileForTarget(Procedure& proc)
{
    prepareForGeneration(proc);
    CCallHelpers jit;
    AssemblerPolicy policy = AssemblerPolicy::forJIT();
    policy.features = proc.policy().targetFeatures;
    jit.setPolicy(policy);
    generate(proc, jit);
    LinkBuffer linkBuffer(jit, nullptr);
    auto code = makeUnique<Compilation>(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "Compilation"), proc.releaseByproducts());
    checkUsesOnlyWhatTargetHas(*code, proc.policy().targetFeatures);
    return code;
}

// for (index = first; index REL bound; index += step) { .. }. The second and third arguments are the addresses of two arrays. The counter behind the loop is returned.
struct LoopOverElements {
    explicit LoopOverElements(const Policy& policy = vectorPolicy(), B3::Opcode relation = LessThan, int64_t step = 1, int64_t firstIndex = 0)
        : proc(policy)
        , loop(graph, relation, step, firstIndex)
    {
        for (Value* value : *loop.exit)
            proc.deleteValue(value);
        loop.exit->values().clear();
        graph.ret(loop.exit, graph.get(loop.exit, loop.index));
    }

    Value* index() { return graph.get(loop.body, loop.index); }
    Value* to() { return loop.pointer; }
    Value* from() { return loop.extra; }

    Value* addressOf(Value* array, const ElementKind& kind, Value* index)
    {
        Value* distance = kind.bytes == 1 ? index : graph.make(loop.body, Shl, index, graph.int32(loop.body, WTF::fastLog2(kind.bytes)));
        return graph.make(loop.body, Add, array, distance);
    }

    Value* element(Value* array, const ElementKind& kind, Value* index, int32_t offset = 0, HeapRange range = firstHeap)
    {
        if (kind.load == Load)
            return loop.body->appendNew<MemoryValue>(proc, Load, kind.type, Origin(), addressOf(array, kind, index), offset, range);
        return loop.body->appendNew<MemoryValue>(proc, kind.load, Origin(), addressOf(array, kind, index), offset, range);
    }

    void setElement(Value* array, const ElementKind& kind, Value* index, Value* value, int32_t offset = 0, HeapRange range = firstHeap)
    {
        loop.body->appendNew<MemoryValue>(proc, kind.store, Origin(), value, addressOf(array, kind, index), offset, range);
    }

    void finish(TestPlace place = TestPlace::Top)
    {
        loop.endBody(loop.body);
        if (place == TestPlace::Bottom)
            loop.rotate();
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
};

bool vectorizeLoopsAndValidate(Procedure& proc)
{
    bool result = vectorizeLoops(proc);
    fixSSA(proc);
    validate(proc);
    return result;
}

unsigned numberOfVectorAccesses(Procedure& proc)
{
    return numberOfValues(proc, [](Value* value) {
        MemoryValue* memory = value->as<MemoryValue>();
        return memory && (memory->type() == V128 || (memory->isStore() && memory->child(0)->type() == V128));
    });
}

void fillWithBytes(std::span<uint8_t> bytes, unsigned seed)
{
    uint32_t state = seed * 2654435761u + 1;
    for (uint8_t& byte : bytes) {
        state = state * 1103515245u + 12345u;
        byte = state >> 16;
    }
}

constexpr unsigned bytesOfBuffer = 2048;

// What the elements hold after the loop is what they hold after a loop that takes one turn after the other.
template<typename Turn>
void checkAgainstTurns(Compilation& code, unsigned seed, int64_t bound, size_t toOffset, size_t fromOffset, int64_t firstIndex, const Turn& turn, bool includesEqual = false)
{
    alignas(16) uint8_t found[bytesOfBuffer];
    alignas(16) uint8_t expected[bytesOfBuffer];
    fillWithBytes(found, seed);
    fillWithBytes(expected, seed);
    int64_t counter = firstIndex;
    for (; counter < bound + includesEqual; ++counter)
        turn(expected + toOffset, expected + fromOffset, counter);
    CHECK_EQ(invoke<int64_t>(code, bound, found + toOffset, found + fromOffset), counter);
    if (!memcmp(found, expected, bytesOfBuffer))
        return;
    crashLock.lock();
    dataLogLn("The elements differ for bound ", bound, ", the target at ", toOffset, " and the source at ", fromOffset);
    CRASH();
}

} // anonymous namespace

static void testVectorizeLoopsCopiesElementsOfEveryKind()
{
    for (CPUFeatureSet features : featureSets()) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            for (const ElementKind& kind : elementKinds) {
                LoopOverElements loop { vectorPolicy(features) };
                loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
                loop.finish(place);
                CHECK(vectorizeLoopsAndValidate(loop.proc));
                CHECK(hasRemark(loop.proc, RemarkKind::LoopVectorized, GaveUpReason::None));
                CHECK_EQ(loop.proc.report().countOf(Counter::LoopsVectorized), 1u);
                CHECK_EQ(loop.proc.naturalLoops().numLoops(), 2u);
                CHECK_EQ(numberOfVectorAccesses(loop.proc), 2u);
                auto code = compileForTarget(loop.proc);
                // From forty elements below to forty above: the arrays lie over each other at every distance, at addresses of every alignment.
                for (int distance = -40; distance <= 40; ++distance) {
                    for (int64_t bound = -1; bound <= 70; ++bound) {
                        for (size_t first : { 40, 41, 43 }) {
                            checkAgainstTurns(*code, distance * 71 + bound, bound, (first + distance) * kind.bytes, first * kind.bytes, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
                                memmove(to + index * kind.bytes, from + index * kind.bytes, kind.bytes);
                            });
                        }
                    }
                }
            }
        }
    }
}

static void testVectorizeLoopsLeavesLoopThatOnlyMovesTwoElementsToVectorInShortArrays()
{
    const ElementKind& numbers = elementKinds[6];
    const ElementKind& words = elementKinds[4];
    auto vectorizes = [&](const ElementKind& kind, bool arraysAreShort, bool computes) {
        Policy policy = vectorPolicy();
        policy.holdsMostlyShortArrays = createSharedTask<bool(Procedure&, const HeapRange&)>([=](Procedure&, const HeapRange& range) {
            return arraysAreShort && range == firstHeap;
        });
        LoopOverElements loop { policy };
        Value* element = loop.element(loop.from(), kind, loop.index());
        loop.setElement(loop.to(), kind, loop.index(), computes ? loop.graph.make(loop.loop.body, Add, element, element) : element);
        loop.finish();
        return vectorizeLoopsAndValidate(loop.proc);
    };
    CHECK(!vectorizes(numbers, true, false));
    CHECK(vectorizes(numbers, false, false));
    CHECK(vectorizes(numbers, true, true));
    CHECK(vectorizes(words, true, false));
}

static void testVectorizeLoopsFillsElements()
{
    for (CPUFeatureSet features : featureSets()) {
        for (const ElementKind& kind : elementKinds) {
            LoopOverElements loop { vectorPolicy(features) };
            Value* filler = loop.from();
            auto convert = [&](B3::Opcode opcode) {
                filler = loop.proc.add<Value>(opcode, Origin(), filler);
                loop.loop.root->appendNonTerminal(filler);
            };
            if (kind.bytes <= 4)
                convert(Trunc);
            if (kind.type.isFloat())
                convert(BitwiseCast);
            loop.setElement(loop.to(), kind, loop.index(), filler);
            loop.finish();
            CHECK(vectorizeLoopsAndValidate(loop.proc));
            CHECK_EQ(numberOfVectorAccesses(loop.proc), 1u);
            CHECK_EQ(numberOfValues(loop.proc, VectorSplat), 1u);
            auto code = compileForTarget(loop.proc);
            for (int64_t bound = 0; bound <= 70; ++bound) {
                alignas(16) uint8_t found[bytesOfBuffer];
                alignas(16) uint8_t expected[bytesOfBuffer];
                fillWithBytes(found, bound);
                fillWithBytes(expected, bound);
                uint64_t word = 0x8877665544332211ull;
                for (int64_t index = 0; index < bound; ++index)
                    memcpy(expected + 3 * kind.bytes + index * kind.bytes, &word, kind.bytes);
                invoke<int64_t>(*code, bound, found + 3 * kind.bytes, word);
                CHECK(!memcmp(found, expected, bytesOfBuffer));
            }
        }
    }
}

template<typename Lane>
static void checkArithmetic(CPUFeatureSet features, const ElementKind& kind, B3::Opcode opcode)
{
    LoopOverElements loop { vectorPolicy(features) };
    Value* left = loop.element(loop.to(), kind, loop.index());
    Value* right = loop.element(loop.from(), kind, loop.index());
    loop.setElement(loop.to(), kind, loop.index(), loop.graph.make(loop.loop.body, opcode, left, right));
    loop.finish();
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    CHECK_EQ(numberOfVectorAccesses(loop.proc), 3u);
    auto code = compileForTarget(loop.proc);
    for (int distance : { -40, -17, -16, -15, -3, -1, 0, 1, 2, 5, 15, 16, 17, 40 }) {
        for (int64_t bound = 0; bound <= 70; ++bound) {
            checkAgainstTurns(*code, distance * 71 + bound, bound, (41 + distance) * kind.bytes, 41 * kind.bytes, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
                Lane left;
                Lane right;
                memcpy(&left, to + index * sizeof(Lane), sizeof(Lane));
                memcpy(&right, from + index * sizeof(Lane), sizeof(Lane));
                Lane result = 0;
                switch (opcode) {
                case Add:
                    result = left + right;
                    break;
                case Sub:
                    result = left - right;
                    break;
                case BitAnd:
                    result = left & right;
                    break;
                case BitOr:
                    result = left | right;
                    break;
                case BitXor:
                    result = left ^ right;
                    break;
                case Mul:
                    result = left * right;
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
                memcpy(to + index * sizeof(Lane), &result, sizeof(Lane));
            });
        }
    }
}

static void testVectorizeLoopsComputesWithIntegersOfEveryWidth()
{
    for (CPUFeatureSet features : featureSets()) {
        for (B3::Opcode opcode : { Add, Sub, BitAnd, BitOr, BitXor }) {
            checkArithmetic<uint8_t>(features, elementKinds[0], opcode);
            checkArithmetic<uint8_t>(features, elementKinds[1], opcode);
            checkArithmetic<uint16_t>(features, elementKinds[2], opcode);
            checkArithmetic<uint16_t>(features, elementKinds[3], opcode);
            checkArithmetic<uint32_t>(features, elementKinds[4], opcode);
        }
        checkArithmetic<uint16_t>(features, elementKinds[2], Mul);
        checkArithmetic<uint16_t>(features, elementKinds[3], Mul);
        checkArithmetic<uint32_t>(features, elementKinds[4], Mul);
    }
}

template<typename Lane>
static Lane computeNumber(B3::Opcode opcode, Lane left, Lane right)
{
    switch (opcode) {
    case Add:
        return left + right;
    case Sub:
        return left - right;
    case Mul:
        return left * right;
    case Div:
        return left / right;
    case Neg:
        return -left;
    case Abs:
        return std::abs(left);
    case Sqrt:
        return std::sqrt(left);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// The bytes on the left spell every kind of number: NaNs with all their bits, infinities, both zeros, numbers too small for the usual form. Those on the right spell ordinary numbers,
// so that a NaN that comes out says which NaN went in.
template<typename Lane, typename Bits>
static void checkArithmeticOnNumbers(CPUFeatureSet features, const ElementKind& kind, B3::Opcode opcode, bool ordinaryIsOnLeft)
{
    bool isUnary = opcode == Neg || opcode == Abs || opcode == Sqrt;
    LoopOverElements loop { vectorPolicy(features) };
    Value* any = loop.element(loop.to(), kind, loop.index());
    Value* result = nullptr;
    if (isUnary)
        result = loop.graph.make(loop.loop.body, opcode, any);
    else {
        Value* ordinary = loop.element(loop.from(), kind, loop.index(), 0, secondHeap);
        result = ordinaryIsOnLeft ? loop.graph.make(loop.loop.body, opcode, ordinary, any) : loop.graph.make(loop.loop.body, opcode, any, ordinary);
    }
    loop.setElement(loop.to(), kind, loop.index(), result);
    loop.finish();
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    auto code = compileForTarget(loop.proc);
    constexpr unsigned numberOfLanes = 600;
    constexpr unsigned exponentShift = sizeof(Lane) == 4 ? 23 : 52;
    constexpr Bits exponentMask = static_cast<Bits>(sizeof(Lane) == 4 ? 0xff : 0x7ff) << exponentShift;
    constexpr Bits exponentOfOne = static_cast<Bits>(sizeof(Lane) == 4 ? 0x7f : 0x3ff) << exponentShift;
    for (int64_t bound : { 0, 1, 2, 3, 4, 5, 7, 8, 9, 31, 600 }) {
        Vector<Bits> found(numberOfLanes);
        Vector<Bits> ordinary(numberOfLanes);
        fillWithBytes(std::span { reinterpret_cast<uint8_t*>(found.mutableSpan().data()), numberOfLanes * sizeof(Bits) }, bound + 1);
        fillWithBytes(std::span { reinterpret_cast<uint8_t*>(ordinary.mutableSpan().data()), numberOfLanes * sizeof(Bits) }, bound + 2);
        for (unsigned index = 0; index < numberOfLanes; ++index) {
            ordinary[index] = (ordinary[index] & ~exponentMask) | exponentOfOne;
            switch (index % 8) {
            case 1:
                found[index] |= exponentMask;
                break;
            case 2:
                found[index] &= ~exponentMask;
                break;
            case 3:
                found[index] &= static_cast<Bits>(1) << (sizeof(Bits) * 8 - 1);
                break;
            case 4:
                found[index] = (found[index] & (static_cast<Bits>(1) << (sizeof(Bits) * 8 - 1))) | exponentMask;
                break;
            }
        }
        Vector<Bits> expected = found;
        for (int64_t index = 0; index < bound; ++index) {
            Lane any = std::bit_cast<Lane>(expected[index]);
            Lane other = std::bit_cast<Lane>(ordinary[index]);
            expected[index] = std::bit_cast<Bits>(ordinaryIsOnLeft && !isUnary ? computeNumber(opcode, other, any) : computeNumber(opcode, any, other));
        }
        invoke<int64_t>(*code, bound, found.mutableSpan().data(), ordinary.mutableSpan().data());
        for (unsigned index = 0; index < numberOfLanes; ++index) {
            if (found[index] == expected[index])
                continue;
            crashLock.lock();
            dataLogLn(opcode, " of lanes of ", sizeof(Lane), " bytes, bound ", bound, ", lane ", index, ": found ", found[index], ", expected ", expected[index], ", the ordinary number is ", ordinary[index], ordinaryIsOnLeft ? " on the left" : " on the right");
            CRASH();
        }
    }
}

static void testVectorizeLoopsComputesWithNumbersBitForBit()
{
    for (CPUFeatureSet features : featureSets()) {
        for (B3::Opcode opcode : { Add, Sub, Mul, Div, Neg, Abs, Sqrt }) {
            for (bool ordinaryIsOnLeft : { false, true }) {
                checkArithmeticOnNumbers<float, uint32_t>(features, elementKinds[5], opcode, ordinaryIsOnLeft);
                checkArithmeticOnNumbers<double, uint64_t>(features, elementKinds[6], opcode, ordinaryIsOnLeft);
            }
        }
    }
}

// list[index + storedAt] = list[index + loadedAt]
static bool vectorizesMoveWithinOneArray(const ElementKind& kind, int32_t storedAt, int32_t loadedAt)
{
    LoopOverElements loop;
    loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.to(), kind, loop.index(), loadedAt * kind.bytes), storedAt * kind.bytes);
    loop.finish();
    bool result = vectorizeLoopsAndValidate(loop.proc);
    auto code = compileForTarget(loop.proc);
    for (int64_t bound = 0; bound <= 70; ++bound) {
        checkAgainstTurns(*code, bound, bound, 64 * kind.bytes, 0, 0, [&](uint8_t* to, uint8_t*, int64_t index) {
            memmove(to + (index + storedAt) * kind.bytes, to + (index + loadedAt) * kind.bytes, kind.bytes);
        });
    }
    return result;
}

static void testVectorizeLoopsKeepsLoopWhoseTurnReadsWhatTurnShortlyBeforeStored()
{
    for (const ElementKind& kind : elementKinds) {
        int lanes = 16 / kind.bytes;
        CHECK(vectorizesMoveWithinOneArray(kind, 0, 0));
        for (int distance = 1; distance <= 20; ++distance) {
            CHECK(vectorizesMoveWithinOneArray(kind, 0, distance));
            CHECK_EQ(vectorizesMoveWithinOneArray(kind, distance, 0), distance >= lanes);
            CHECK_EQ(vectorizesMoveWithinOneArray(kind, 0, -distance), distance >= lanes);
        }
    }
}

static void testVectorizeLoopsTestsNoDistanceOfArraysThatAreKnownToLieApart()
{
    const ElementKind& kind = elementKinds[4];
    auto numberOfBranchesInFront = [&](HeapRange rangeOfSource) {
        LoopOverElements loop;
        loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index(), 0, rangeOfSource));
        loop.finish();
        CHECK(vectorizeLoopsAndValidate(loop.proc));
        return numberOfValues(loop.proc, Branch);
    };
    CHECK_EQ(numberOfBranchesInFront(firstHeap), numberOfBranchesInFront(secondHeap) + 1);
}

// What the phase takes untested is checked in front of the vector loop where moved loads are logged: that accesses of heaps that do not overlap lie apart, and, where the phase is broken on purpose, that all do.
static void testVectorizeLoopsReportsAccessesThatAreTakenToLieApartAndDoNot()
{
    constexpr int64_t reported = -77;
    for (const ElementKind& kind : elementKinds) {
        for (bool isBroken : { false, true }) {
            for (bool logsMovedLoads : { false, true }) {
                Policy policy = vectorPolicy(CPUFeatureSet::buildTarget());
                policy.validation.logsMovedLoads = logsMovedLoads;
                policy.validation.takesAccessesOfVectorTurnsToLieApart = isBroken;
                policy.reportsMovedLoad = createSharedTask<void(CCallHelpers&, const MovedLoad&, GPRReg, GPRReg)>([](CCallHelpers& jit, const MovedLoad&, GPRReg, GPRReg) {
                    jit.move(CCallHelpers::TrustedImm64(reported), GPRInfo::returnValueGPR);
                    jit.emitFunctionEpilogue();
                    jit.ret();
                });
                LoopOverElements loop { policy };
                loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index(), 0, isBroken ? firstHeap : secondHeap));
                loop.finish();
                CHECK(vectorizeLoopsAndValidate(loop.proc));
                CHECK_EQ(numberOfValues(loop.proc, Patchpoint), logsMovedLoads ? 1u : 0u);
                CHECK(!findPatchpointWhereVectorIsAlive(loop.proc));
                if (!logsMovedLoads)
                    continue;
                auto code = compileForTarget(loop.proc);
                int64_t lanes = 16 / kind.bytes;
                for (int64_t distance = -20; distance <= 20; ++distance) {
                    for (int64_t bound = 0; bound <= 40; ++bound) {
                        alignas(16) uint8_t bytes[bytesOfBuffer];
                        fillWithBytes(bytes, bound);
                        int64_t result = invoke<int64_t>(*code, bound, bytes + (40 + distance) * kind.bytes, bytes + 40 * kind.bytes);
                        bool meets = bound >= lanes && distance > 0 && distance < lanes;
                        CHECK_EQ(result, meets ? reported : bound);
                    }
                }
            }
        }
    }
}

// A turn that polls. The vector loop polls once between two stretches of Limits::maxTurnsWithoutPoll turns, and at no poll is a vector alive.
static void testVectorizeLoopsPollsBetweenStretches()
{
    const ElementKind& kind = elementKinds[0];
    constexpr int64_t bytesPerStretch = Limits::maxTurnsWithoutPoll * 16;
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (bool mayLeavePollOut : { false, true }) {
            Policy policy = vectorPolicy(CPUFeatureSet::buildTarget());
            policy.loops.pollsBetweenStretches = mayLeavePollOut;
            LoopOverElements loop { policy };
            static int64_t numberOfPolls;
            PatchpointValue* poll = loop.graph.emptyPatchpoint(loop.loop.body);
            poll->isLoopPoll = true;
            poll->iterationsAreShort = true;
            poll->effects = Effects::none();
            poll->effects.writesLocalState = true;
            poll->effects.exitsSideways = true;
            poll->effects.reads = HeapRange::top();
            poll->numGPScratchRegisters = 1;
            poll->clobber(RegisterSet::macroClobberedGPRs());
            poll->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                jit.move(CCallHelpers::TrustedImmPtr(&numberOfPolls), params.gpScratch(0));
                jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params.gpScratch(0)));
            });
            loop.setElement(loop.to(), kind, loop.index(), loop.graph.make(loop.loop.body, Add, loop.element(loop.from(), kind, loop.index()), loop.graph.int32(loop.loop.body, 3)));
            loop.finish(place);
            CHECK_EQ(vectorizeLoopsAndValidate(loop.proc), mayLeavePollOut);
            CHECK(!findPatchpointWhereVectorIsAlive(loop.proc));
            if (!mayLeavePollOut)
                continue;
            CHECK_EQ(numberOfValues(loop.proc, Patchpoint), 2u);
            auto code = compileForTarget(loop.proc);
            Vector<uint8_t> to(3 * bytesPerStretch);
            Vector<uint8_t> from(3 * bytesPerStretch);
            for (size_t position = 0; position < from.size(); ++position)
                from[position] = position * 7 + (position >> 12);
            for (int64_t bound : { int64_t { 1 }, int64_t { 15 }, int64_t { 16 }, int64_t { 100 }, bytesPerStretch - 1, bytesPerStretch, bytesPerStretch + 15, bytesPerStretch + 16, bytesPerStretch + 40, 2 * bytesPerStretch + 5, 3 * bytesPerStretch - 2 }) {
                memset(to.mutableSpan().data(), 0, to.size());
                numberOfPolls = 0;
                CHECK_EQ(invoke<int64_t>(*code, bound, to.mutableSpan().data(), from.mutableSpan().data()), bound);
                for (int64_t position = 0; position < static_cast<int64_t>(to.size()); ++position)
                    CHECK_EQ(to[position], position < bound ? static_cast<uint8_t>(from[position] + 3) : 0);
                // One for each turn of the loop itself, which takes what is left of fewer than sixteen elements, and one between two stretches.
                int64_t turnsOfVectorLoop = bound / 16;
                int64_t stretches = (turnsOfVectorLoop + Limits::maxTurnsWithoutPoll - 1) / Limits::maxTurnsWithoutPoll;
                CHECK_EQ(numberOfPolls, bound % 16 + std::max<int64_t>(stretches - 1, 0));
            }
        }
    }
}

static void testVectorizeLoopsEveryRelationAndStart()
{
    const ElementKind& kind = elementKinds[2];
    for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
        for (B3::Opcode relation : { LessThan, LessEqual, Below, BelowEqual }) {
            for (int64_t firstIndex : { 0, 1, 5, 16 }) {
                LoopOverElements loop { vectorPolicy(), relation, 1, firstIndex };
                loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
                loop.finish(place);
                // Where the bound may be the greatest number there is, the counter behind the last turn may not exist.
                if (relation == LessEqual || relation == BelowEqual)
                    loop.proc.setKnownRange(loop.loop.bound, IntRange(0, 1ll << 40));
                CHECK(vectorizeLoopsAndValidate(loop.proc));
                auto code = compileForTarget(loop.proc);
                bool includesEqual = relation == LessEqual || relation == BelowEqual;
                for (int64_t bound = 0; bound <= 90; ++bound) {
                    checkAgainstTurns(*code, bound, bound, 203, 401, firstIndex, [&](uint8_t* to, uint8_t* from, int64_t index) {
                        memmove(to + index * kind.bytes, from + index * kind.bytes, kind.bytes);
                    }, includesEqual);
                }
            }
        }
    }
}

static void testVectorizeLoopsWalksDownward()
{
    for (CPUFeatureSet features : featureSets()) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            for (const ElementKind& kind : elementKinds) {
                for (B3::Opcode relation : { GreaterEqual, GreaterThan }) {
                    // for (index = 69; index REL bound; --index) to[index] = from[index]
                    LoopOverElements loop { vectorPolicy(features), relation, -1, 69 };
                    loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
                    loop.finish(place);
                    loop.proc.setKnownRange(loop.loop.bound, IntRange(-2, 1ll << 40));
                    CHECK(vectorizeLoopsAndValidate(loop.proc));
                    // Every address falls, so the lanes are stored in the order they were loaded in.
                    CHECK(!numberOfValues(loop.proc, VectorSwizzle));
                    auto code = compileForTarget(loop.proc);
                    for (int distance = -40; distance <= 40; ++distance) {
                        for (int64_t bound = -1; bound <= 71; ++bound) {
                            alignas(16) uint8_t found[bytesOfBuffer];
                            alignas(16) uint8_t expected[bytesOfBuffer];
                            fillWithBytes(found, distance * 73 + bound);
                            fillWithBytes(expected, distance * 73 + bound);
                            size_t toOffset = (41 + distance) * kind.bytes;
                            size_t fromOffset = 41 * kind.bytes;
                            int64_t index = 69;
                            for (; relation == GreaterEqual ? index >= bound : index > bound; --index)
                                memmove(expected + toOffset + index * kind.bytes, expected + fromOffset + index * kind.bytes, kind.bytes);
                            CHECK_EQ(invoke<int64_t>(*code, bound, found + toOffset, found + fromOffset), index);
                            CHECK(!memcmp(found, expected, bytesOfBuffer));
                        }
                    }
                }
            }
        }
    }
}

namespace {

// for (low = 0, high = length - 1; low REL high; ++low, --high) { .. }. The second and third arguments are the addresses of two arrays. low + high behind the loop is returned.
struct LoopOverBothEnds {
    enum class Tests : uint8_t { LowerAgainstHigher, HigherAgainstLower };

    explicit LoopOverBothEnds(const Policy& policy, B3::Opcode relation = LessThan, Tests tests = Tests::LowerAgainstHigher)
        : proc(policy)
        , root(proc.addBlock())
        , header(proc.addBlock())
        , body(proc.addBlock())
        , exit(proc.addBlock())
        , low(proc.addVariable(Int64))
        , high(proc.addVariable(Int64))
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        first = arguments[1];
        second = arguments[2];
        proc.setKnownRange(arguments[0], IntRange(0, 1ll << 40));
        graph.set(root, low, graph.int64(root, 0));
        graph.set(root, high, graph.make(root, Add, arguments[0], graph.int64(root, -1)));
        graph.jump(root, header);
        bool testsLower = tests == Tests::LowerAgainstHigher;
        graph.branch(header, graph.make(header, relation, graph.get(header, testsLower ? low : high), graph.get(header, testsLower ? high : low)), body, exit);
        graph.ret(exit, graph.make(exit, Add, graph.make(exit, Shl, graph.get(exit, low), graph.int32(exit, 16)), graph.get(exit, high)));
    }

    Value* addressOf(Value* array, const ElementKind& kind, Variable* index)
    {
        return graph.make(body, Add, array, graph.make(body, Shl, graph.get(body, index), graph.int32(body, WTF::fastLog2(kind.bytes))));
    }

    Value* element(Value* array, const ElementKind& kind, Variable* index)
    {
        if (kind.load == Load)
            return body->appendNew<MemoryValue>(proc, Load, kind.type, Origin(), addressOf(array, kind, index), 0, firstHeap);
        return body->appendNew<MemoryValue>(proc, kind.load, Origin(), addressOf(array, kind, index), 0, firstHeap);
    }

    void setElement(Value* array, const ElementKind& kind, Variable* index, Value* value)
    {
        body->appendNew<MemoryValue>(proc, kind.store, Origin(), value, addressOf(array, kind, index), 0, firstHeap);
    }

    void finish(TestPlace place)
    {
        graph.increase(body, low, 1);
        graph.increase(body, high, -1);
        graph.jump(body, header);
        proc.resetReachability();
        fixSSA(proc);
        if (place == TestPlace::Bottom) {
            CHECK(rotateLoops(proc));
            fixSSA(proc);
        }
        for (Value* value : proc.values()) {
            if (value->opcode() == Phi)
                proc.setKnownRange(value, IntRange(-2, 1ll << 40));
        }
        validate(proc);
    }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* body;
    BasicBlock* exit;
    Variable* low;
    Variable* high;
    Value* first { nullptr };
    Value* second { nullptr };
};

} // anonymous namespace

static void testVectorizeLoopsReversesInPlace()
{
    for (CPUFeatureSet features : featureSets()) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            for (const ElementKind& kind : elementKinds) {
                LoopOverBothEnds loop { vectorPolicy(features) };
                Value* kept = loop.element(loop.first, kind, loop.low);
                loop.setElement(loop.first, kind, loop.low, loop.element(loop.first, kind, loop.high));
                loop.setElement(loop.first, kind, loop.high, kept);
                loop.finish(place);
                CHECK(vectorizeLoopsAndValidate(loop.proc));
                CHECK_EQ(numberOfVectorAccesses(loop.proc), 4u);
                CHECK_EQ(numberOfValues(loop.proc, VectorSwizzle), 2u);
                auto code = compileForTarget(loop.proc);
                for (int64_t length = 0; length <= 150; ++length) {
                    alignas(16) uint8_t found[bytesOfBuffer];
                    alignas(16) uint8_t expected[bytesOfBuffer];
                    fillWithBytes(found, length);
                    fillWithBytes(expected, length);
                    uint8_t* list = expected + 5 * kind.bytes;
                    int64_t low = 0;
                    int64_t high = length - 1;
                    for (; low < high; ++low, --high) {
                        for (unsigned byte = 0; byte < kind.bytes; ++byte)
                            std::swap(list[low * kind.bytes + byte], list[high * kind.bytes + byte]);
                    }
                    CHECK_EQ(invoke<int64_t>(*code, length, found + 5 * kind.bytes, 0), (low << 16) + high);
                    CHECK(!memcmp(found, expected, bytesOfBuffer));
                }
            }
        }
    }
}

static void testVectorizeLoopsSkipsEndsThatAreNotKnownToLieApart()
{
    const ElementKind& kind = elementKinds[4];
    // Two arrays, of which nothing says where they lie.
    {
        LoopOverBothEnds loop { vectorPolicy() };
        loop.setElement(loop.first, kind, loop.low, loop.element(loop.second, kind, loop.high));
        loop.finish(TestPlace::Top);
        CHECK(!vectorizeLoopsAndValidate(loop.proc));
    }
    // The two ends may be one element.
    {
        LoopOverBothEnds loop { vectorPolicy(), LessEqual };
        Value* kept = loop.element(loop.first, kind, loop.low);
        loop.setElement(loop.first, kind, loop.low, loop.element(loop.first, kind, loop.high));
        loop.setElement(loop.first, kind, loop.high, kept);
        loop.finish(TestPlace::Top);
        CHECK(!vectorizeLoopsAndValidate(loop.proc));
    }
}

static void testVectorizeLoopsCopiesInOppositeOrderBetweenArraysThatLieApart()
{
    for (CPUFeatureSet features : featureSets()) {
        for (const ElementKind& kind : elementKinds) {
            LoopOverBothEnds loop { vectorPolicy(features) };
            Value* element = kind.load == Load
                ? loop.body->appendNew<MemoryValue>(loop.proc, Load, kind.type, Origin(), loop.addressOf(loop.second, kind, loop.high), 0, secondHeap)
                : loop.body->appendNew<MemoryValue>(loop.proc, kind.load, Origin(), loop.addressOf(loop.second, kind, loop.high), 0, secondHeap);
            loop.setElement(loop.first, kind, loop.low, element);
            loop.finish(TestPlace::Top);
            CHECK(vectorizeLoopsAndValidate(loop.proc));
            CHECK_EQ(numberOfValues(loop.proc, VectorSwizzle), 1u);
            auto code = compileForTarget(loop.proc);
            for (int64_t length = 0; length <= 100; ++length) {
                alignas(16) uint8_t found[bytesOfBuffer];
                alignas(16) uint8_t expected[bytesOfBuffer];
                fillWithBytes(found, length);
                fillWithBytes(expected, length);
                for (int64_t low = 0, high = length - 1; low < high; ++low, --high)
                    memcpy(expected + (3 + low) * kind.bytes, expected + 1024 + high * kind.bytes, kind.bytes);
                invoke<int64_t>(*code, length, found + 3 * kind.bytes, found + 1024);
                CHECK(!memcmp(found, expected, bytesOfBuffer));
            }
        }
    }
}

// A list of bytes is turned round in place by a turn that polls. The two ends lie more than one stretch apart.
static void testVectorizeLoopsPollsBetweenStretchesOfCountersThatMeet()
{
    const ElementKind& kind = elementKinds[0];
    Policy policy = vectorPolicy(CPUFeatureSet::buildTarget());
    policy.loops.pollsBetweenStretches = true;
    LoopOverBothEnds loop { policy };
    static int64_t numberOfPolls;
    PatchpointValue* poll = loop.graph.emptyPatchpoint(loop.body);
    poll->isLoopPoll = true;
    poll->iterationsAreShort = true;
    poll->effects = Effects::none();
    poll->effects.writesLocalState = true;
    poll->effects.exitsSideways = true;
    poll->effects.reads = HeapRange::top();
    poll->numGPScratchRegisters = 1;
    poll->clobber(RegisterSet::macroClobberedGPRs());
    poll->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImmPtr(&numberOfPolls), params.gpScratch(0));
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params.gpScratch(0)));
    });
    Value* kept = loop.element(loop.first, kind, loop.low);
    loop.setElement(loop.first, kind, loop.low, loop.element(loop.first, kind, loop.high));
    loop.setElement(loop.first, kind, loop.high, kept);
    loop.finish(TestPlace::Top);
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    CHECK(!findPatchpointWhereVectorIsAlive(loop.proc));
    auto code = compileForTarget(loop.proc);
    constexpr int64_t bytesPerStretch = Limits::maxTurnsWithoutPoll * 16;
    Vector<uint8_t> bytes(5 * bytesPerStretch);
    for (int64_t length : { int64_t { 40 }, 2 * bytesPerStretch - 3, 2 * bytesPerStretch + 31, 2 * bytesPerStretch + 70, 4 * bytesPerStretch + 37, 5 * bytesPerStretch }) {
        for (int64_t position = 0; position < length; ++position)
            bytes[position] = position * 11 + (position >> 13);
        numberOfPolls = 0;
        invoke<int64_t>(*code, length, bytes.mutableSpan().data(), 0);
        for (int64_t position = 0; position < length; ++position) {
            int64_t source = length - 1 - position;
            CHECK_EQ(bytes[position], static_cast<uint8_t>(source * 11 + (source >> 13)));
        }
        int64_t low = 0;
        int64_t high = length - 1;
        int64_t turnsOfVectorLoop = 0;
        for (; low + 30 < high; low += 16, high -= 16)
            ++turnsOfVectorLoop;
        int64_t turnsOfLoopItself = 0;
        for (; low < high; ++low, --high)
            ++turnsOfLoopItself;
        int64_t stretches = (turnsOfVectorLoop + Limits::maxTurnsWithoutPoll - 1) / Limits::maxTurnsWithoutPoll;
        CHECK_EQ(numberOfPolls, turnsOfLoopItself + std::max<int64_t>(stretches - 1, 0));
    }
}

// low < high, low <= high, high > low, high >= low: the vector loop tests one counter against a limit that it computes once.
static void testVectorizeLoopsEveryRelationOfCountersThatMeet()
{
    using Tests = LoopOverBothEnds::Tests;
    struct Form {
        B3::Opcode relation;
        Tests tests;
        bool includesEqual;
    };
    const Form forms[] = {
        { LessThan, Tests::LowerAgainstHigher, false },
        { LessEqual, Tests::LowerAgainstHigher, true },
        { GreaterThan, Tests::HigherAgainstLower, false },
        { GreaterEqual, Tests::HigherAgainstLower, true },
    };
    for (const Form& form : forms) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            for (const ElementKind& kind : elementKinds) {
                LoopOverBothEnds loop { vectorPolicy(CPUFeatureSet::buildTarget()), form.relation, form.tests };
                Value* element = kind.load == Load
                    ? loop.body->appendNew<MemoryValue>(loop.proc, Load, kind.type, Origin(), loop.addressOf(loop.second, kind, loop.high), 0, secondHeap)
                    : loop.body->appendNew<MemoryValue>(loop.proc, kind.load, Origin(), loop.addressOf(loop.second, kind, loop.high), 0, secondHeap);
                loop.setElement(loop.first, kind, loop.low, element);
                loop.finish(place);
                CHECK(vectorizeLoopsAndValidate(loop.proc));
                auto code = compileForTarget(loop.proc);
                for (int64_t length = 0; length <= 120; ++length) {
                    alignas(16) uint8_t found[bytesOfBuffer];
                    alignas(16) uint8_t expected[bytesOfBuffer];
                    fillWithBytes(found, length);
                    fillWithBytes(expected, length);
                    int64_t low = 0;
                    int64_t high = length - 1;
                    for (; form.includesEqual ? low <= high : low < high; ++low, --high)
                        memcpy(expected + (3 + low) * kind.bytes, expected + 1024 + high * kind.bytes, kind.bytes);
                    CHECK_EQ(invoke<int64_t>(*code, length, found + 3 * kind.bytes, found + 1024), (low << 16) + high);
                    CHECK(!memcmp(found, expected, bytesOfBuffer));
                }
            }
        }
    }
}

static void testVectorizeLoopsCountsBytes()
{
    // for (at = 0; at < bound; at += 4) *(to + at) = *(from + at)
    const ElementKind& kind = elementKinds[4];
    LoopOverElements loop { vectorPolicy(), LessThan, 4 };
    Value* element = loop.graph.load(loop.loop.body, Int32, loop.graph.make(loop.loop.body, Add, loop.from(), loop.index()), 0, firstHeap);
    loop.graph.store(loop.loop.body, element, loop.graph.make(loop.loop.body, Add, loop.to(), loop.index()), 0, firstHeap);
    loop.finish();
    loop.proc.setKnownRange(loop.loop.bound, IntRange(0, 1ll << 40));
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    auto code = compileForTarget(loop.proc);
    for (int distance = -10; distance <= 10; ++distance) {
        for (int64_t bound = 0; bound <= 300; ++bound) {
            alignas(16) uint8_t found[bytesOfBuffer];
            alignas(16) uint8_t expected[bytesOfBuffer];
            fillWithBytes(found, bound);
            fillWithBytes(expected, bound);
            int64_t at = 0;
            for (; at < bound; at += 4)
                memmove(expected + (100 + distance) * kind.bytes + at, expected + 100 * kind.bytes + at, 4);
            CHECK_EQ(invoke<int64_t>(*code, bound, found + (100 + distance) * kind.bytes, found + 100 * kind.bytes), at);
            CHECK(!memcmp(found, expected, bytesOfBuffer));
        }
    }
}

template<typename Functor>
static void checkIsNotVectorized(const Functor& fillBody, const Policy& policy = vectorPolicy(), int64_t step = 1)
{
    LoopOverElements loop { policy, LessThan, step };
    fillBody(loop);
    loop.finish();
    CHECK(!vectorizeLoopsAndValidate(loop.proc));
    CHECK(!numberOfVectorAccesses(loop.proc));
    CHECK(!loop.proc.usesSIMD());
}

// to[index] = the nearest to to[index] * factor + addend that lies between two limits: computed in lanes of twice the width of an element.
static void testVectorizeLoopsComputesInLanesWiderThanElements()
{
    struct Limits {
        int32_t least;
        int32_t greatest;
    };
    auto lesser = [](LoopOverElements& loop, Value* left, Value* right, bool comparesInOrder) {
        BasicBlock* body = loop.loop.body;
        if (comparesInOrder)
            return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, LessThan, left, right), left, right);
        return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, GreaterEqual, left, right), right, left);
    };
    auto greater = [](LoopOverElements& loop, Value* left, Value* right, bool comparesInOrder) {
        BasicBlock* body = loop.loop.body;
        if (comparesInOrder)
            return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, GreaterThan, left, right), left, right);
        return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, LessEqual, left, right), right, left);
    };
    auto read = [](const ElementKind& kind, const uint8_t* at) -> int32_t {
        switch (kind.load) {
        case Load8Z:
            return *at;
        case Load8S:
            return static_cast<int8_t>(*at);
        case Load16Z: {
            uint16_t result;
            memcpy(&result, at, 2);
            return result;
        }
        default: {
            int16_t result;
            memcpy(&result, at, 2);
            return result;
        }
        }
    };
    for (CPUFeatureSet features : featureSets()) {
        for (unsigned kindIndex = 0; kindIndex < 4; ++kindIndex) {
            const ElementKind& kind = elementKinds[kindIndex];
            bool hasSign = kind.load == Load8S || kind.load == Load16S;
            int32_t values = 1 << (kind.bytes * 8);
            Limits whole = hasSign ? Limits { -values / 2, values / 2 - 1 } : Limits { 0, values - 1 };
            for (Limits limits : { whole, Limits { whole.least + 3, whole.greatest - 5 }, Limits { 1, 100 } }) {
                for (int32_t addend : { -values, -300, -9, 0, 9, 300, values }) {
                    for (int32_t factor : { 1, 3 }) {
                        for (bool takesGreaterFirst : { false, true }) {
                            for (bool comparesInOrder : { false, true }) {
                                LoopOverElements loop { vectorPolicy(features) };
                                BasicBlock* body = loop.loop.body;
                                Value* value = loop.element(loop.to(), kind, loop.index());
                                if (factor != 1)
                                    value = loop.graph.make(body, Mul, value, loop.graph.int32(body, factor));
                                value = loop.graph.make(body, Add, value, loop.graph.int32(body, addend));
                                if (takesGreaterFirst)
                                    value = lesser(loop, greater(loop, value, loop.graph.int32(body, limits.least), comparesInOrder), loop.graph.int32(body, limits.greatest), comparesInOrder);
                                else
                                    value = greater(loop, lesser(loop, value, loop.graph.int32(body, limits.greatest), comparesInOrder), loop.graph.int32(body, limits.least), comparesInOrder);
                                loop.setElement(loop.to(), kind, loop.index(), value);
                                loop.finish();
                                // What is computed on the way must fit a lane.
                                int64_t farthest = static_cast<int64_t>(whole.greatest + (hasSign ? 1 : 0)) * factor + std::abs(addend);
                                bool fitsLane = farthest < (int64_t { 1 } << (kind.bytes * 16 - 1));
                                CHECK_EQ(vectorizeLoopsAndValidate(loop.proc), fitsLane);
                                if (!fitsLane)
                                    continue;
                                auto code = compileForTarget(loop.proc);
                                for (int64_t bound : { 0, 1, 3, 4, 7, 8, 9, 15, 16, 17, 31, 33, 70 }) {
                                    checkAgainstTurns(*code, bound + addend, bound, 3 * kind.bytes, 1024, 0, [&](uint8_t* to, uint8_t*, int64_t index) {
                                        int32_t result = std::clamp(read(kind, to + index * kind.bytes) * factor + addend, limits.least, limits.greatest);
                                        memcpy(to + index * kind.bytes, &result, kind.bytes);
                                    });
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// to[index] = the greater of to[index] and from[index].
static void testVectorizeLoopsTakesGreaterOfTwoElements()
{
    for (CPUFeatureSet features : featureSets()) {
        for (unsigned kindIndex = 0; kindIndex < 5; ++kindIndex) {
            const ElementKind& kind = elementKinds[kindIndex];
            LoopOverElements loop { vectorPolicy(features) };
            BasicBlock* body = loop.loop.body;
            Value* left = loop.element(loop.to(), kind, loop.index());
            Value* right = loop.element(loop.from(), kind, loop.index());
            loop.setElement(loop.to(), kind, loop.index(), body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, GreaterThan, left, right), left, right));
            loop.finish();
            CHECK(vectorizeLoopsAndValidate(loop.proc));
            auto code = compileForTarget(loop.proc);
            for (int64_t bound = 0; bound <= 70; ++bound) {
                checkAgainstTurns(*code, bound, bound, 3 * kind.bytes, 1024 + 5 * kind.bytes, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
                    auto read = [&](const uint8_t* at) -> int64_t {
                        uint32_t bits = 0;
                        memcpy(&bits, at, kind.bytes);
                        if (kind.load == Load8S)
                            return static_cast<int8_t>(bits);
                        if (kind.load == Load16S)
                            return static_cast<int16_t>(bits);
                        if (kind.load == Load)
                            return static_cast<int32_t>(bits);
                        return bits;
                    };
                    if (read(from + index * kind.bytes) > read(to + index * kind.bytes))
                        memcpy(to + index * kind.bytes, from + index * kind.bytes, kind.bytes);
                });
            }
        }
    }
}

// sum = start; for (index = 0; index < bound; index++) sum += element(index). The third argument holds the bits of the start. The bits of the sum are returned.
struct LoopThatAddsUp {
    LoopThatAddsUp(const Policy& policy, Type typeOfSum)
        : proc(policy)
        , loop(graph)
        , typeOfSum(typeOfSum)
        , total(proc.addVariable(typeOfSum))
    {
        Value* jump = loop.root->values().takeLast();
        graph.set(loop.root, total, typeOfSum == Double ? graph.make(loop.root, BitwiseCast, loop.extra) : typeOfSum == Int32 ? graph.make(loop.root, Trunc, loop.extra) : loop.extra);
        loop.root->values().append(jump);
        for (Value* value : *loop.exit)
            proc.deleteValue(value);
        loop.exit->values().clear();
        Value* result = graph.get(loop.exit, total);
        graph.ret(loop.exit, typeOfSum == Double ? graph.make(loop.exit, BitwiseCast, result) : typeOfSum == Int32 ? graph.make(loop.exit, ZExt32, result) : result);
    }

    Value* element(const ElementKind& kind)
    {
        Value* index = graph.get(loop.body, loop.index);
        Value* distance = kind.bytes == 1 ? index : graph.make(loop.body, Shl, index, graph.int32(loop.body, WTF::fastLog2(kind.bytes)));
        Value* address = graph.make(loop.body, Add, loop.pointer, distance);
        if (kind.load == Load)
            return loop.body->appendNew<MemoryValue>(proc, Load, kind.type, Origin(), address, 0, firstHeap);
        return loop.body->appendNew<MemoryValue>(proc, kind.load, Origin(), address, 0, firstHeap);
    }

    void add(Value* integer, B3::Opcode opcode = Add)
    {
        Value* added = typeOfSum == Double ? graph.make(loop.body, IToD, integer) : typeOfSum == Int64 ? graph.make(loop.body, SExt32, integer) : integer;
        graph.set(loop.body, total, graph.make(loop.body, opcode, graph.get(loop.body, total), added));
    }

    Procedure proc;
    Graph graph { proc };
    TopTestedLoop loop;
    Type typeOfSum;
    Variable* total;
};

static int64_t readElement(const ElementKind& kind, const uint8_t* at)
{
    uint32_t bits = 0;
    memcpy(&bits, at, kind.bytes);
    if (kind.load == Load8S)
        return static_cast<int8_t>(bits);
    if (kind.load == Load16S)
        return static_cast<int16_t>(bits);
    if (kind.load == Load)
        return static_cast<int32_t>(bits);
    return bits;
}

// One element after the other, as the loop is written.
static uint64_t addUpInTurns(const ElementKind& kind, Type typeOfSum, uint64_t start, const uint8_t* elements, int64_t bound, int64_t addedToEach = 0, bool takesAway = false)
{
    if (typeOfSum == Double) {
        double sum = std::bit_cast<double>(start);
        for (int64_t index = 0; index < bound; ++index) {
            double element = static_cast<double>(readElement(kind, elements + index * kind.bytes) + addedToEach);
            sum = takesAway ? sum - element : sum + element;
        }
        return std::bit_cast<uint64_t>(sum);
    }
    uint64_t sum = start;
    for (int64_t index = 0; index < bound; ++index)
        sum += static_cast<uint64_t>((readElement(kind, elements + index * kind.bytes) + addedToEach) * (takesAway ? -1 : 1));
    return typeOfSum == Int32 ? static_cast<uint32_t>(sum) : sum;
}

static void testVectorizeLoopsAddsUpElementsOfEveryKind()
{
    const double starts[] = { 0, 1, -7, 0.5, -0.0, 4294967296.0, 2251799813685247.0, 2251799813685248.0, -2251799813685248.0, 4503599627370497.0, 9007199254740992.0, 1e300,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::denorm_min() };
    for (CPUFeatureSet features : featureSets()) {
        for (unsigned kindIndex = 0; kindIndex < 5; ++kindIndex) {
            const ElementKind& kind = elementKinds[kindIndex];
            for (Type typeOfSum : { Double, Int32, Int64 }) {
                for (int32_t addedToEach : { 0, 5, -5 }) {
                    // The last takes away what the second adds.
                    bool takesAway = addedToEach < 0;
                    if (takesAway)
                        addedToEach = 5;
                    LoopThatAddsUp loop { vectorPolicy(features), typeOfSum };
                    Value* element = loop.element(kind);
                    // Five more than an integer of 32 bits may be too much for 32 bits.
                    if (addedToEach && kind.bytes == 4)
                        continue;
                    loop.add(addedToEach ? loop.graph.make(loop.loop.body, Add, element, loop.graph.int32(loop.loop.body, addedToEach)) : element, takesAway ? Sub : Add);
                    loop.loop.endBody(loop.loop.body);
                    CHECK(vectorizeLoopsAndValidate(loop.proc));
                    // The loop itself, the turns of a stretch, and the stretches.
                    CHECK_EQ(loop.proc.naturalLoops().numLoops(), 3u);
                    auto code = compileForTarget(loop.proc);
                    alignas(16) uint8_t elements[bytesOfBuffer];
                    fillWithBytes(elements, kindIndex + 1);
                    for (int64_t bound = 0; bound <= 70; ++bound) {
                        for (double number : starts) {
                            uint64_t start = typeOfSum == Double ? std::bit_cast<uint64_t>(number) : static_cast<uint64_t>(static_cast<int64_t>(std::isfinite(number) && std::abs(number) < 1e18 ? number : 12345.0) * 0x100000001ll);
                            if (typeOfSum == Int32)
                                start = static_cast<uint32_t>(start);
                            const uint8_t* at = elements + 3 * kind.bytes;
                            CHECK_EQ(static_cast<uint64_t>(invoke<int64_t>(*code, bound, at, start)), addUpInTurns(kind, typeOfSum, start, at, bound, addedToEach, takesAway));
                        }
                    }
                }
            }
        }
    }
}

// More elements than the lanes of a sum hold, each as far from zero as it can be. Where a sum of numbers leaves the whole numbers that a number tells apart, every addition rounds, in the order of the turns.
static void testVectorizeLoopsAddsUpManyElements()
{
    constexpr int64_t numberOfElements = 5000000;
    for (unsigned kindIndex = 0; kindIndex < 5; ++kindIndex) {
        const ElementKind& kind = elementKinds[kindIndex];
        Vector<uint8_t> elements(numberOfElements * kind.bytes + 64);
        for (Type typeOfSum : { Double, Int32, Int64 }) {
            LoopThatAddsUp loop { vectorPolicy(), typeOfSum };
            loop.add(loop.element(kind));
            loop.loop.endBody(loop.loop.body);
            CHECK(vectorizeLoopsAndValidate(loop.proc));
            auto code = compileForTarget(loop.proc);
            for (unsigned filling = 0; filling < 3; ++filling) {
                // The greatest, the least, and whatever.
                if (filling == 2)
                    fillWithBytes(elements.mutableSpan(), kindIndex);
                else {
                    uint32_t bits = kind.load == Load8Z || kind.load == Load16Z ? (filling ? 0 : 0xffffffffu) : (filling ? 1u << (kind.bytes * 8 - 1) : (1u << (kind.bytes * 8 - 1)) - 1);
                    for (int64_t index = 0; index < numberOfElements; ++index)
                        memcpy(elements.mutableSpan().data() + index * kind.bytes, &bits, kind.bytes);
                }
                for (int64_t bound : { numberOfElements, numberOfElements - 1, static_cast<int64_t>(131072), static_cast<int64_t>(131073), static_cast<int64_t>(2097152 + 3) }) {
                    uint64_t start = typeOfSum == Double ? std::bit_cast<uint64_t>(3.0) : 3;
                    CHECK_EQ(static_cast<uint64_t>(invoke<int64_t>(*code, bound, elements.span().data(), start)), addUpInTurns(kind, typeOfSum, start, elements.span().data(), bound));
                }
            }
        }
    }
}

// Zero has a sign. What is left of it depends on whether all elements are zero or only their sum.
static void testVectorizeLoopsKeepsSignOfZeroSum()
{
    const ElementKind& kind = elementKinds[3];
    for (bool takesAway : { false, true }) {
        LoopThatAddsUp loop { vectorPolicy(), Double };
        loop.add(loop.element(kind), takesAway ? Sub : Add);
        loop.loop.endBody(loop.loop.body);
        CHECK(vectorizeLoopsAndValidate(loop.proc));
        auto code = compileForTarget(loop.proc);
        for (int16_t first : { 0, 5 }) {
            for (double number : { 0.0, -0.0, 5.0, -5.0 }) {
                for (int64_t bound : { 0, 1, 2, 8, 16, 17, 40 }) {
                    alignas(16) int16_t elements[64] = { };
                    elements[0] = first;
                    elements[1] = -first;
                    elements[9] = number ? (takesAway ? number : -number) : 0;
                    uint64_t start = std::bit_cast<uint64_t>(number);
                    const uint8_t* at = reinterpret_cast<const uint8_t*>(elements);
                    CHECK_EQ(static_cast<uint64_t>(invoke<int64_t>(*code, bound, at, start)), addUpInTurns(kind, Double, start, at, bound, 0, takesAway));
                }
            }
        }
    }
}

// found = start; for each element: if (element REL found) found = element. Written with a Select and with a branch.
static void testVectorizeLoopsFindsLeastAndGreatestElement()
{
    const int32_t starts[] = { 0, 1, -5, 300, 70000, -70000, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() };
    for (CPUFeatureSet features : featureSets()) {
        for (unsigned kindIndex = 0; kindIndex < 5; ++kindIndex) {
            const ElementKind& kind = elementKinds[kindIndex];
            for (B3::Opcode relation : { GreaterThan, GreaterEqual, LessThan, LessEqual, Above, AboveEqual, Below, BelowEqual }) {
                for (bool branches : { false, true }) {
                    for (bool elementComesFirst : { true, false }) {
                        LoopThatAddsUp loop { vectorPolicy(features), Int32 };
                        Graph& graph = loop.graph;
                        BasicBlock* body = loop.loop.body;
                        Value* element = loop.element(kind);
                        Value* found = graph.get(body, loop.total);
                        // The element is taken where the relation holds for it, whichever side it is written on.
                        Value* takesElement = elementComesFirst ? graph.make(body, relation, element, found) : graph.make(body, relation, found, element);
                        BasicBlock* last = body;
                        if (branches) {
                            BasicBlock* taken = loop.proc.addBlock();
                            BasicBlock* kept = loop.proc.addBlock();
                            last = loop.proc.addBlock();
                            graph.branch(body, takesElement, taken, kept);
                            graph.set(taken, loop.total, element);
                            graph.jump(taken, last);
                            graph.jump(kept, last);
                        } else
                            graph.set(body, loop.total, graph.select(body, takesElement, element, found));
                        loop.loop.endBody(last);
                        CHECK(vectorizeLoopsAndValidate(loop.proc));
                        auto code = compileForTarget(loop.proc);
                        alignas(16) uint8_t elements[bytesOfBuffer];
                        fillWithBytes(elements, kindIndex + 7);
                        auto holds = [&](int32_t left, int32_t right) {
                            switch (relation) {
                            case GreaterThan:
                                return left > right;
                            case GreaterEqual:
                                return left >= right;
                            case LessThan:
                                return left < right;
                            case LessEqual:
                                return left <= right;
                            case Above:
                                return static_cast<uint32_t>(left) > static_cast<uint32_t>(right);
                            case AboveEqual:
                                return static_cast<uint32_t>(left) >= static_cast<uint32_t>(right);
                            case Below:
                                return static_cast<uint32_t>(left) < static_cast<uint32_t>(right);
                            default:
                                return static_cast<uint32_t>(left) <= static_cast<uint32_t>(right);
                            }
                        };
                        for (int64_t bound = 0; bound <= 70; ++bound) {
                            for (int32_t start : starts) {
                                const uint8_t* at = elements + 3 * kind.bytes;
                                int32_t expected = start;
                                for (int64_t index = 0; index < bound; ++index) {
                                    int32_t value = static_cast<int32_t>(readElement(kind, at + index * kind.bytes));
                                    if (elementComesFirst ? holds(value, expected) : holds(expected, value))
                                        expected = value;
                                }
                                CHECK_EQ(static_cast<uint32_t>(invoke<int64_t>(*code, bound, at, static_cast<uint64_t>(static_cast<uint32_t>(start)))), static_cast<uint32_t>(expected));
                            }
                        }
                    }
                }
            }
        }
    }
}

// to[index] = from[index] > limit ? limit : from[index], written with a branch. What a way hands on is read in the turn, wherever the counter is tested.
static void testVectorizeLoopsTakesBranchWhoseWaysOnlyChoose()
{
    for (CPUFeatureSet features : featureSets()) {
        for (TestPlace place : { TestPlace::Top, TestPlace::Bottom }) {
            for (unsigned kindIndex = 0; kindIndex < 5; ++kindIndex) {
                const ElementKind& kind = elementKinds[kindIndex];
                for (int32_t limit : { 0, 100, -100 }) {
                    LoopOverElements loop { vectorPolicy(features) };
                    Graph& graph = loop.graph;
                    BasicBlock* body = loop.loop.body;
                    Variable* chosen = loop.proc.addVariable(Int32);
                    Value* element = loop.element(loop.from(), kind, loop.index());
                    BasicBlock* over = loop.proc.addBlock();
                    BasicBlock* within = loop.proc.addBlock();
                    BasicBlock* last = loop.proc.addBlock();
                    graph.branch(body, graph.make(body, GreaterThan, element, graph.int32(body, limit)), over, within);
                    graph.set(over, chosen, graph.int32(over, limit));
                    graph.jump(over, last);
                    graph.set(within, chosen, element);
                    graph.jump(within, last);
                    Value* index = graph.get(last, loop.loop.index);
                    Value* distance = kind.bytes == 1 ? index : graph.make(last, Shl, index, graph.int32(last, WTF::fastLog2(kind.bytes)));
                    last->appendNew<MemoryValue>(loop.proc, kind.store, Origin(), graph.get(last, chosen), graph.make(last, Add, loop.to(), distance), 0, firstHeap);
                    loop.loop.endBody(last);
                    if (place == TestPlace::Bottom)
                        loop.loop.rotate();
                    CHECK(vectorizeLoopsAndValidate(loop.proc));
                    auto code = compileForTarget(loop.proc);
                    for (int64_t bound = 0; bound <= 70; ++bound) {
                        checkAgainstTurns(*code, bound, bound, 3 * kind.bytes, 1024 + 5 * kind.bytes, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
                            int32_t value = static_cast<int32_t>(std::min<int64_t>(readElement(kind, from + index * kind.bytes), limit));
                            memcpy(to + index * kind.bytes, &value, kind.bytes);
                        });
                    }
                }
            }
        }
    }
}

// A way that does more than choose stays a branch.
static void testVectorizeLoopsSkipsBranchWhoseWayStores()
{
    const ElementKind& words = elementKinds[4];
    LoopOverElements loop;
    Graph& graph = loop.graph;
    BasicBlock* body = loop.loop.body;
    Value* element = loop.element(loop.from(), words, loop.index());
    Value* address = loop.addressOf(loop.to(), words, loop.index());
    BasicBlock* stores = loop.proc.addBlock();
    BasicBlock* skips = loop.proc.addBlock();
    BasicBlock* last = loop.proc.addBlock();
    graph.branch(body, graph.make(body, GreaterThan, element, graph.int32(body, 0)), stores, skips);
    stores->appendNew<MemoryValue>(loop.proc, Store, Origin(), element, address, 0, firstHeap);
    graph.jump(stores, last);
    graph.jump(skips, last);
    loop.loop.endBody(last);
    CHECK(!vectorizeLoopsAndValidate(loop.proc));
}

static void testVectorizeLoopsSkipsSumThatTurnLooksAt()
{
    const ElementKind& words = elementKinds[4];
    // to[index] = the sum so far.
    LoopThatAddsUp loop { vectorPolicy(), Int32 };
    loop.add(loop.element(words));
    Value* address = loop.graph.make(loop.loop.body, Add, loop.loop.pointer, loop.graph.make(loop.loop.body, Shl, loop.graph.get(loop.loop.body, loop.loop.index), loop.graph.int32(loop.loop.body, 2)));
    loop.loop.body->appendNew<MemoryValue>(loop.proc, Store, Origin(), loop.graph.get(loop.loop.body, loop.total), address, 0, firstHeap);
    loop.loop.endBody(loop.loop.body);
    CHECK(!vectorizeLoopsAndValidate(loop.proc));

    // Numbers that need not be whole are added in the order of the turns.
    LoopThatAddsUp numbers { vectorPolicy(), Double };
    Value* number = numbers.element(elementKinds[6]);
    numbers.graph.set(numbers.loop.body, numbers.total, numbers.graph.make(numbers.loop.body, Add, numbers.graph.get(numbers.loop.body, numbers.total), number));
    numbers.loop.endBody(numbers.loop.body);
    CHECK(!vectorizeLoopsAndValidate(numbers.proc));
}

static void testVectorizeLoopsSkipsLoopsWhoseTurnsAreNotAlike()
{
    const ElementKind& bytes = elementKinds[0];
    const ElementKind& words = elementKinds[4];
    auto copy = [&](LoopOverElements& loop, const ElementKind& kind) {
        loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
    };
    // Every other element.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        copy(loop, words);
    }, vectorPolicy(), 2);
    // A call.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        copy(loop, words);
        loop.graph.callLikePatchpoint(loop.loop.body);
    });
    // Elements of two sizes.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        loop.setElement(loop.to(), words, loop.index(), loop.element(loop.from(), bytes, loop.index()));
    });
    // One place for all turns.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        loop.graph.store(loop.loop.body, loop.element(loop.from(), words, loop.index()), loop.to(), 0, firstHeap);
    });
    // An element that another element chooses.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        Value* chosen = loop.graph.make(loop.loop.body, ZExt32, loop.element(loop.from(), bytes, loop.index()));
        loop.setElement(loop.to(), bytes, loop.index(), loop.element(loop.from(), bytes, chosen));
    });
    // The counter itself is stored.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        loop.setElement(loop.to(), words, loop.index(), loop.graph.make(loop.loop.body, Trunc, loop.index()));
    });
    // Words of 64 bits are stored.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        Value* address = loop.graph.make(loop.loop.body, Shl, loop.index(), loop.graph.int32(loop.loop.body, 3));
        Value* word = loop.graph.load(loop.loop.body, Int64, loop.graph.make(loop.loop.body, Add, loop.from(), address), 0, firstHeap);
        loop.graph.store(loop.loop.body, word, loop.graph.make(loop.loop.body, Add, loop.to(), address), 0, firstHeap);
    });
    // An operation that has no form on vectors here.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        Value* element = loop.element(loop.from(), words, loop.index());
        loop.setElement(loop.to(), words, loop.index(), loop.graph.make(loop.loop.body, Div, element, loop.graph.int32(loop.loop.body, 3)));
    });
    // Nothing is stored.
    checkIsNotVectorized([&](LoopOverElements& loop) {
        loop.element(loop.from(), words, loop.index());
    });
}

static void testVectorizeLoopsSkipsLoopWithSecondWayOut()
{
    const ElementKind& kind = elementKinds[4];
    LoopOverElements loop;
    BasicBlock* rest = loop.proc.addBlock();
    Value* element = loop.element(loop.from(), kind, loop.index());
    loop.graph.branch(loop.loop.body, element, FrequentedBlock(rest), rarely(loop.loop.exit));
    loop.loop.body = rest;
    loop.setElement(loop.to(), kind, loop.index(), element);
    loop.finish();
    CHECK(!vectorizeLoopsAndValidate(loop.proc));
}

static void testVectorizeLoopsTakesBranchOnConstantForJump()
{
    const ElementKind& kind = elementKinds[0];
    LoopOverElements loop;
    BasicBlock* rest = loop.proc.addBlock();
    BasicBlock* neverEntered = loop.proc.addBlock();
    Value* element = loop.element(loop.from(), kind, loop.index());
    loop.graph.branch(loop.loop.body, loop.graph.int32(loop.loop.body, 1), FrequentedBlock(rest), rarely(neverEntered));
    loop.graph.callLikePatchpoint(neverEntered);
    loop.graph.increase(neverEntered, loop.loop.index, 1);
    loop.graph.jump(neverEntered, loop.loop.header);
    loop.loop.body = rest;
    loop.setElement(loop.to(), kind, loop.index(), element);
    loop.finish();
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    auto code = compileForTarget(loop.proc);
    for (int64_t bound = 0; bound <= 70; ++bound) {
        checkAgainstTurns(*code, bound, bound, 100, 300, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
            to[index] = from[index];
        });
    }
}

static void testVectorizeLoopsKeepsWhatHoldsValuesAlive()
{
    const ElementKind& kind = elementKinds[0];
    LoopOverElements loop;
    loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
    PatchpointValue* holder = loop.graph.emptyPatchpoint(loop.loop.body);
    holder->effects.writesLocalState = true;
    holder->effects.reads = HeapRange::top();
    holder->append(ConstrainedValue(loop.to(), ValueRep::ColdAny));
    loop.finish();
    CHECK(vectorizeLoopsAndValidate(loop.proc));
    checkValueCountInEachLoop(loop.proc, Patchpoint, { 1, 1 });
}

static void testVectorizeLoopsCountsWithIntegersOf32Bits()
{
    // for (int32 index = 0; index < bound; ++index) to[zext(index + 1)] = from[zext(index)]
    const ElementKind& kind = elementKinds[0];
    auto build = [&](Procedure& proc, std::optional<IntRange> rangeOfBound) {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* index = proc.addVariable(Int32);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        Value* bound = graph.make(root, Trunc, arguments[0]);
        if (rangeOfBound)
            proc.setKnownRange(bound, *rangeOfBound);
        graph.set(root, index, graph.int32(root, 0));
        graph.jump(root, header);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), bound), body, exit);
        Value* element = body->appendNew<MemoryValue>(proc, kind.load, Origin(), graph.make(body, Add, arguments[2], graph.make(body, ZExt32, graph.get(body, index))), 0, firstHeap);
        Value* next = graph.make(body, Add, graph.get(body, index), graph.int32(body, 1));
        body->appendNew<MemoryValue>(proc, kind.store, Origin(), element, graph.make(body, Add, arguments[1], graph.make(body, ZExt32, next)), 0, secondHeap);
        graph.set(body, index, next);
        graph.jump(body, header);
        graph.ret(exit, graph.int64(exit, 0));
        proc.resetReachability();
        fixSSA(proc);
        validate(proc);
    };
    Procedure proc { vectorPolicy() };
    build(proc, std::nullopt);
    CHECK(vectorizeLoopsAndValidate(proc));
    auto code = compileForTarget(proc);
    for (int64_t bound = 0; bound <= 70; ++bound) {
        alignas(16) uint8_t found[bytesOfBuffer];
        alignas(16) uint8_t expected[bytesOfBuffer];
        fillWithBytes(found, bound);
        fillWithBytes(expected, bound);
        for (int64_t index = 0; index < bound; ++index)
            expected[100 + index + 1] = expected[300 + index];
        invoke<int64_t>(*code, bound, found + 100, found + 300);
        CHECK(!memcmp(found, expected, bytesOfBuffer));
    }
}

// for (round = 0; round < 3; ++round) { patchpoint; for (index = 0; index < bound; ++index) to[index] += constant; }
static void testVectorConstantIsNotHeldAcrossPatchpointThatKeepsLowerHalves()
{
    for (CPUFeatureSet features : featureSets()) {
        Policy policy = optimizingPolicy();
        policy.upstream.allowsSIMD = true;
        policy.targetFeatures = features;
        policy.loops.vectorizes = true;
        policy.air.registers.patchpointsKeepOnlyLowerHalvesOfVectors = true;
        Procedure proc { policy };
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* outer = proc.addBlock();
        BasicBlock* header = proc.addBlock();
        BasicBlock* body = proc.addBlock();
        BasicBlock* latch = proc.addBlock();
        BasicBlock* exit = proc.addBlock();
        Variable* round = proc.addVariable(Int64);
        Variable* index = proc.addVariable(Int64);
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        graph.set(root, round, graph.int64(root, 0));
        graph.jump(root, outer);
        // As a stub does that saves and restores the registers it uses as numbers.
        PatchpointValue* patchpoint = graph.emptyPatchpoint(outer);
        patchpoint->effects.writesLocalState = true;
        patchpoint->effects.writes = secondHeap;
        patchpoint->numGPScratchRegisters = 1;
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            RegisterSet::allFPRs().forEach([&](Reg reg) {
                jit.moveDoubleTo64(reg.fpr(), params.gpScratch(0));
                jit.move64ToDouble(params.gpScratch(0), reg.fpr());
            });
        });
        graph.set(outer, index, graph.int64(outer, 0));
        graph.jump(outer, header);
        graph.branch(header, graph.make(header, LessThan, graph.get(header, index), arguments[0]), body, latch);
        Value* address = graph.make(body, Add, arguments[1], graph.make(body, Shl, graph.get(body, index), graph.int32(body, 2)));
        graph.store(body, graph.make(body, Add, graph.load(body, Int32, address, 0, firstHeap), graph.int32(body, 0x01020304)), address, 0, firstHeap);
        graph.increase(body, index, 1);
        graph.jump(body, header);
        graph.increase(latch, round, 1);
        graph.branch(latch, graph.make(latch, LessThan, graph.get(latch, round), graph.int64(latch, 3)), outer, exit);
        graph.ret(exit, graph.int64(exit, 0));
        proc.resetReachability();
        auto code = compileForTarget(proc);
        CHECK(proc.usesSIMD());
        for (int64_t bound = 0; bound <= 40; ++bound) {
            int32_t words[48];
            for (int32_t& word : words)
                word = 5;
            invoke<int64_t>(*code, bound, words, 0);
            for (int64_t position = 0; position < 48; ++position)
                CHECK_EQ(words[position], position < bound ? 5 + 3 * 0x01020304 : 5);
        }
    }
}

static void testPipelineVectorizesLoopOverElements()
{
    const ElementKind& kind = elementKinds[0];
    for (CPUFeatureSet features : featureSets()) {
        for (bool unrolls : { false, true }) {
            Policy policy = optimizingPolicy();
            policy.upstream.allowsSIMD = true;
            policy.targetFeatures = features;
            policy.collectsRemarks = true;
            policy.loops.vectorizes = true;
            policy.loops.unrolls = unrolls;
            LoopOverElements loop { policy };
            loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
            loop.finish();
            auto code = compileForTarget(loop.proc);
            CHECK(hasRemark(loop.proc, RemarkKind::LoopVectorized, GaveUpReason::None));
            for (int distance = -20; distance <= 20; ++distance) {
                for (int64_t bound = 0; bound <= 300; ++bound) {
                    checkAgainstTurns(*code, bound, bound, 500 + distance, 500, 0, [&](uint8_t* to, uint8_t* from, int64_t index) {
                        to[index] = from[index];
                    });
                }
            }
        }
    }
}

// vector = load; [in the same block | in a block between | on one of two ways | in a loop between] patchpoint; store vector
static void testFindsPatchpointWhereVectorIsAlive()
{
    enum class Place : uint8_t { SameBlock, BlockBetween, OneOfTwoWays, LoopBetween, BehindLastUse, InFrontOfDefinition };
    enum class Kind : uint8_t { KeepsAlive, MakesConstant, OnlyTests, TestsAndLeavesSideways, TakesOperand, HasRegisterForNumbers, Polls, Writes };
    auto find = [&](Place place, Kind kind) {
        Procedure proc { vectorPolicy(CPUFeatureSet::buildTarget()) };
        proc.setUsesSIMD();
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        BasicBlock* between = proc.addBlock();
        BasicBlock* other = proc.addBlock();
        BasicBlock* last = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(proc, root).eager();
        auto addPatchpoint = [&](BasicBlock* block) {
            if (kind == Kind::MakesConstant || kind == Kind::TakesOperand) {
                PatchpointValue* patchpoint = graph.emptyPatchpoint(block, Int64);
                patchpoint->effects = Effects::none();
                if (kind == Kind::TakesOperand)
                    patchpoint->append(ConstrainedValue(arguments[1], ValueRep::SomeRegister));
                patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
                    jit.move(CCallHelpers::TrustedImm32(5), params[0].gpr());
                });
                return patchpoint;
            }
            PatchpointValue* patchpoint = graph.emptyPatchpoint(block);
            patchpoint->effects = Effects::none();
            patchpoint->effects.writesLocalState = true;
            patchpoint->effects.reads = HeapRange::top();
            if (kind != Kind::Polls)
                patchpoint->append(ConstrainedValue(arguments[1], ValueRep::ColdAny));
            if (kind == Kind::OnlyTests || kind == Kind::TestsAndLeavesSideways) {
                patchpoint->append(ConstrainedValue(arguments[0], ValueRep::SomeRegister));
                patchpoint->numGPScratchRegisters = 2;
                patchpoint->clobber(RegisterSet::macroClobberedGPRs());
            }
            if (kind == Kind::HasRegisterForNumbers)
                patchpoint->numFPScratchRegisters = 1;
            if (kind == Kind::TestsAndLeavesSideways || kind == Kind::Polls)
                patchpoint->effects.exitsSideways = true;
            if (kind == Kind::Polls)
                patchpoint->isLoopPoll = true;
            if (kind == Kind::Writes)
                patchpoint->effects.writes = HeapRange::top();
            patchpoint->setGenerator([](CCallHelpers&, const StackmapGenerationParams&) { });
            return patchpoint;
        };
        Value* expected = nullptr;
        if (place == Place::InFrontOfDefinition)
            addPatchpoint(root);
        Value* vector = graph.load(root, V128, arguments[1], 0, firstHeap);
        if (place == Place::SameBlock)
            expected = addPatchpoint(root);
        graph.branch(root, arguments[0], between, other);
        if (place == Place::BlockBetween || place == Place::OneOfTwoWays || place == Place::LoopBetween)
            expected = addPatchpoint(between);
        if (place == Place::BlockBetween)
            addPatchpoint(other);
        if (place == Place::LoopBetween)
            graph.branch(between, arguments[2], between, last);
        else
            graph.jump(between, last);
        graph.jump(other, last);
        graph.store(last, vector, arguments[1], 16, firstHeap);
        if (place == Place::BehindLastUse)
            addPatchpoint(last);
        graph.ret(last, graph.int64(last, 0));
        proc.resetReachability();
        validate(proc);
        Value* found = findPatchpointWhereVectorIsAlive(proc);
        if (place == Place::BlockBetween)
            return !!found;
        CHECK(!found || found == expected);
        return !!found;
    };
    for (Place place : { Place::SameBlock, Place::BlockBetween, Place::OneOfTwoWays, Place::LoopBetween }) {
        CHECK(find(place, Kind::HasRegisterForNumbers));
        CHECK(find(place, Kind::Polls));
        CHECK(find(place, Kind::Writes));
        CHECK(find(place, Kind::TakesOperand));
        CHECK(!find(place, Kind::KeepsAlive));
        CHECK(!find(place, Kind::MakesConstant));
        CHECK(!find(place, Kind::OnlyTests));
        CHECK(!find(place, Kind::TestsAndLeavesSideways));
    }
    for (Place place : { Place::BehindLastUse, Place::InFrontOfDefinition })
        CHECK(!find(place, Kind::Writes));
}

// Each loop is taken by exactly the targets that have what its vector loop is made of, and what is emitted for a target is made of nothing else.
static void testVectorizeLoopsAsksWhatTargetHas()
{
    const ElementKind& bytes = elementKinds[0];
    const ElementKind& shorts = elementKinds[2];
    const ElementKind& words = elementKinds[4];
    CPUFeatureSet nothing;
    Vector<CPUFeatureSet> targets { CPUFeatureSet::buildTarget(), CPUFeatureSet::host() };
#if CPU(X86_64)
    CPUFeatureSet needsSSE4_1 = nothing.with(CPUFeature::SSE4_1);
    CPUFeatureSet needsBoth = needsSSE4_1.with(CPUFeature::SupplementalSSE3);
    for (unsigned bits = 0; bits < 8; ++bits) {
        CPUFeatureSet target;
        if (bits & 1)
            target = target.with(CPUFeature::SSE3);
        if (bits & 2)
            target = target.with(CPUFeature::SupplementalSSE3);
        if (bits & 4)
            target = target.with(CPUFeature::SSE4_1);
        targets.append(target);
    }
#else
    CPUFeatureSet needsSSE4_1;
    CPUFeatureSet needsBoth;
#endif
    auto isTakenBy = [&](CPUFeatureSet target, CPUFeatureSet needed) {
#if CPU(X86_64)
        if (target.contains(CPUFeature::AVX))
            return true;
#endif
        return needed.isSubsetOf(target);
    };

    auto checkLoop = [&](const ElementKind& kind, CPUFeatureSet needed, const auto& makeStored, const auto& turn) {
        for (CPUFeatureSet target : targets) {
            LoopOverElements loop { vectorPolicy(target) };
            loop.setElement(loop.to(), kind, loop.index(), makeStored(loop));
            loop.finish();
            CHECK_EQ(vectorizeLoopsAndValidate(loop.proc), isTakenBy(target, needed));
            auto code = compileForTarget(loop.proc);
            for (int64_t bound = 0; bound <= 70; ++bound)
                checkAgainstTurns(*code, bound, bound, 3 * kind.bytes, 1024 + 5 * kind.bytes, 0, turn);
        }
    };
    auto checkOperation = [&]<typename T>(const ElementKind& kind, CPUFeatureSet needed, B3::Opcode opcode, T (*compute)(T, T)) {
        checkLoop(kind, needed, [&](LoopOverElements& loop) {
            return loop.graph.make(loop.loop.body, opcode, loop.element(loop.to(), kind, loop.index()), loop.element(loop.from(), kind, loop.index()));
        }, [&](uint8_t* to, uint8_t* from, int64_t index) {
            T left;
            T right;
            memcpy(&left, to + index * sizeof(T), sizeof(T));
            memcpy(&right, from + index * sizeof(T), sizeof(T));
            T result = compute(left, right);
            memcpy(to + index * sizeof(T), &result, sizeof(T));
        });
    };
    checkOperation.template operator()<uint8_t>(bytes, nothing, Add, [](uint8_t left, uint8_t right) -> uint8_t { return left + right; });
    checkOperation.template operator()<uint16_t>(shorts, nothing, Mul, [](uint16_t left, uint16_t right) -> uint16_t { return static_cast<uint32_t>(left) * right; });
    checkOperation.template operator()<uint32_t>(words, nothing, Sub, [](uint32_t left, uint32_t right) -> uint32_t { return left - right; });
    checkOperation.template operator()<uint32_t>(words, needsSSE4_1, Mul, [](uint32_t left, uint32_t right) -> uint32_t { return left * right; });
    checkOperation.template operator()<uint32_t>(words, nothing, BitXor, [](uint32_t left, uint32_t right) -> uint32_t { return left ^ right; });

    // Bytes in lanes of sixteen bits.
    checkLoop(bytes, needsSSE4_1, [&](LoopOverElements& loop) {
        BasicBlock* body = loop.loop.body;
        Value* sum = loop.graph.make(body, Add, loop.element(loop.to(), bytes, loop.index()), loop.element(loop.from(), bytes, loop.index()));
        return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, GreaterThan, sum, loop.graph.int32(body, 255)), loop.graph.int32(body, 255), sum);
    }, [&](uint8_t* to, uint8_t* from, int64_t index) {
        to[index] = std::min(to[index] + from[index], 255);
    });
    // The greater of two integers of sixteen bits with a sign.
    checkLoop(elementKinds[3], nothing, [&](LoopOverElements& loop) {
        BasicBlock* body = loop.loop.body;
        Value* left = loop.element(loop.to(), elementKinds[3], loop.index());
        Value* right = loop.element(loop.from(), elementKinds[3], loop.index());
        return body->appendNew<Value>(loop.proc, Select, Origin(), loop.graph.make(body, GreaterThan, left, right), left, right);
    }, [&](uint8_t* to, uint8_t* from, int64_t index) {
        int16_t left;
        int16_t right;
        memcpy(&left, to + index * 2, 2);
        memcpy(&right, from + index * 2, 2);
        if (right > left)
            memcpy(to + index * 2, &right, 2);
    });

    // A value from outside the loop in every lane. Here it is the address of the second array.
    auto checkFill = [&]<typename T>(const ElementKind& kind, CPUFeatureSet needed) {
        for (CPUFeatureSet target : targets) {
            LoopOverElements loop { vectorPolicy(target) };
            Value* filler = loop.graph.make(loop.loop.body, Trunc, loop.from());
            loop.setElement(loop.to(), kind, loop.index(), filler);
            loop.finish();
            CHECK_EQ(vectorizeLoopsAndValidate(loop.proc), isTakenBy(target, needed));
            auto code = compileForTarget(loop.proc);
            for (int64_t bound = 0; bound <= 70; ++bound) {
                alignas(16) T found[128];
                memset(found, 0x5a, sizeof(found));
                CHECK_EQ(invoke<int64_t>(*code, bound, found + 3, static_cast<int64_t>(0x1122334455667788ll)), bound);
                for (int64_t position = 0; position < 128; ++position) {
                    T untouched;
                    memset(&untouched, 0x5a, sizeof(T));
                    CHECK_EQ(found[position], position >= 3 && position < 3 + bound ? static_cast<T>(0x1122334455667788ll) : untouched);
                }
            }
        }
    };
    checkFill.template operator()<uint8_t>(bytes, needsSSE4_1);
    checkFill.template operator()<uint16_t>(shorts, nothing);
    checkFill.template operator()<uint32_t>(words, nothing);

#if CPU(X86_64) && ENABLE(DISASSEMBLER)
    // The form of an instruction follows the target, whatever the machine is that compiles.
    for (CPUFeatureSet target : targets) {
        if (!isTakenBy(target, needsSSE4_1))
            continue;
        LoopOverElements loop { vectorPolicy(target) };
        loop.setElement(loop.to(), words, loop.index(), loop.graph.make(loop.loop.body, Mul, loop.element(loop.to(), words, loop.index()), loop.element(loop.from(), words, loop.index())));
        loop.finish();
        CHECK(vectorizeLoopsAndValidate(loop.proc));
        auto code = compileForTarget(loop.proc);
        bool hasAVX = target.contains(CPUFeature::AVX);
        checkUsesInstruction(*code, hasAVX ? ": vpmulld" : ": pmulld");
        checkUsesInstruction(*code, hasAVX ? ": vmovups" : ": movups");
        checkDoesNotUseInstruction(*code, hasAVX ? ": pmulld" : ": vpmulld");
    }
#endif

    // Lanes in the opposite order.
    for (CPUFeatureSet target : targets) {
        LoopOverBothEnds loop { vectorPolicy(target) };
        Value* kept = loop.element(loop.first, words, loop.low);
        loop.setElement(loop.first, words, loop.low, loop.element(loop.first, words, loop.high));
        loop.setElement(loop.first, words, loop.high, kept);
        loop.finish(TestPlace::Top);
        CHECK_EQ(vectorizeLoopsAndValidate(loop.proc), isTakenBy(target, needsBoth));
        auto code = compileForTarget(loop.proc);
        for (int64_t length = 0; length <= 70; ++length) {
            uint32_t found[80];
            for (unsigned position = 0; position < 80; ++position)
                found[position] = position * 2654435761u;
            invoke<int64_t>(*code, length, found + 3, 0);
            for (int64_t position = 0; position < 80; ++position) {
                int64_t source = position >= 3 && position < 3 + length ? 3 + (length - 1 - (position - 3)) : position;
                CHECK_EQ(found[position], static_cast<uint32_t>(source * 2654435761u));
            }
        }
    }
}

static void testPipelineVectorizesNothingUnlessAsked()
{
    const ElementKind& kind = elementKinds[0];
    Policy policy = optimizingPolicy();
    policy.collectsRemarks = true;
    LoopOverElements loop { policy };
    loop.setElement(loop.to(), kind, loop.index(), loop.element(loop.from(), kind, loop.index()));
    loop.finish();
    compileProc(loop.proc);
    CHECK(!hasRemark(loop.proc, RemarkKind::LoopVectorized));
    CHECK(!loop.proc.usesSIMD());
}

void addBunLoopVectorizingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testVectorizeLoopsCopiesElementsOfEveryKind());
    RUN(testVectorizeLoopsLeavesLoopThatOnlyMovesTwoElementsToVectorInShortArrays());
    RUN(testVectorizeLoopsFillsElements());
    RUN(testVectorizeLoopsComputesWithIntegersOfEveryWidth());
    RUN(testVectorizeLoopsComputesWithNumbersBitForBit());
    RUN(testVectorizeLoopsKeepsLoopWhoseTurnReadsWhatTurnShortlyBeforeStored());
    RUN(testVectorizeLoopsTestsNoDistanceOfArraysThatAreKnownToLieApart());
    RUN(testVectorizeLoopsReportsAccessesThatAreTakenToLieApartAndDoNot());
    RUN(testVectorizeLoopsPollsBetweenStretches());
    RUN(testVectorizeLoopsEveryRelationAndStart());
    RUN(testVectorizeLoopsWalksDownward());
    RUN(testVectorizeLoopsReversesInPlace());
    RUN(testVectorizeLoopsSkipsEndsThatAreNotKnownToLieApart());
    RUN(testVectorizeLoopsCopiesInOppositeOrderBetweenArraysThatLieApart());
    RUN(testVectorizeLoopsPollsBetweenStretchesOfCountersThatMeet());
    RUN(testVectorizeLoopsEveryRelationOfCountersThatMeet());
    RUN(testVectorizeLoopsCountsBytes());
    RUN(testVectorizeLoopsComputesInLanesWiderThanElements());
    RUN(testVectorizeLoopsTakesGreaterOfTwoElements());
    RUN(testVectorizeLoopsAddsUpElementsOfEveryKind());
    RUN(testVectorizeLoopsAddsUpManyElements());
    RUN(testVectorizeLoopsKeepsSignOfZeroSum());
    RUN(testVectorizeLoopsFindsLeastAndGreatestElement());
    RUN(testVectorizeLoopsTakesBranchWhoseWaysOnlyChoose());
    RUN(testVectorizeLoopsSkipsBranchWhoseWayStores());
    RUN(testVectorizeLoopsSkipsSumThatTurnLooksAt());
    RUN(testVectorizeLoopsSkipsLoopsWhoseTurnsAreNotAlike());
    RUN(testVectorizeLoopsSkipsLoopWithSecondWayOut());
    RUN(testVectorizeLoopsTakesBranchOnConstantForJump());
    RUN(testVectorizeLoopsKeepsWhatHoldsValuesAlive());
    RUN(testVectorizeLoopsCountsWithIntegersOf32Bits());
    RUN(testVectorConstantIsNotHeldAcrossPatchpointThatKeepsLowerHalves());
    RUN(testPipelineVectorizesLoopOverElements());
    RUN(testFindsPatchpointWhereVectorIsAlive());
    RUN(testVectorizeLoopsAsksWhatTargetHas());
    RUN(testPipelineVectorizesNothingUnlessAsked());
}

#endif // ENABLE(B3_JIT)
