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
    { Load, Store, Int64, 8 },
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

std::unique_ptr<Compilation> compileForTarget(Procedure& proc)
{
    prepareForGeneration(proc);
    CCallHelpers jit;
    AssemblerPolicy policy = AssemblerPolicy::forJIT();
    policy.features = proc.policy().targetFeatures;
    jit.setPolicy(policy);
    generate(proc, jit);
    LinkBuffer linkBuffer(jit, nullptr);
    return makeUnique<Compilation>(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "Compilation"), proc.releaseByproducts());
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
            if (kind.type == Int32 || kind.type == Float)
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
            checkArithmetic<uint64_t>(features, elementKinds[5], opcode);
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
                checkArithmeticOnNumbers<float, uint32_t>(features, elementKinds[6], opcode, ordinaryIsOnLeft);
                checkArithmeticOnNumbers<double, uint64_t>(features, elementKinds[7], opcode, ordinaryIsOnLeft);
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
        loop.setElement(loop.to(), elementKinds[5], loop.index(), loop.index());
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
    RUN(testVectorizeLoopsFillsElements());
    RUN(testVectorizeLoopsComputesWithIntegersOfEveryWidth());
    RUN(testVectorizeLoopsComputesWithNumbersBitForBit());
    RUN(testVectorizeLoopsKeepsLoopWhoseTurnReadsWhatTurnShortlyBeforeStored());
    RUN(testVectorizeLoopsTestsNoDistanceOfArraysThatAreKnownToLieApart());
    RUN(testVectorizeLoopsEveryRelationAndStart());
    RUN(testVectorizeLoopsSkipsLoopsWhoseTurnsAreNotAlike());
    RUN(testVectorizeLoopsSkipsLoopWithSecondWayOut());
    RUN(testVectorizeLoopsTakesBranchOnConstantForJump());
    RUN(testVectorizeLoopsKeepsWhatHoldsValuesAlive());
    RUN(testVectorizeLoopsCountsWithIntegersOf32Bits());
    RUN(testPipelineVectorizesLoopOverElements());
    RUN(testPipelineVectorizesNothingUnlessAsked());
}

#endif // ENABLE(B3_JIT)
