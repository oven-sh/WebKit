/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTB3Dump.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirNaturalLoops.h"
#include "AirStackSlot.h"
#include "B3AbstractHeapRepository.h"
#include "B3BasicBlockInlines.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include <cstring>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LoopCounter {
    B3::Value* phi;
    int64_t step;
};

struct AirLoopCounter {
    B3::Air::Tmp tmp;
    int64_t step;
};

class HeapNamePrinter {
public:
    explicit HeapNamePrinter(const B3::AbstractHeapRepository& heaps)
        : m_heaps(heaps)
    {
    }

    String describe(const B3::HeapSet& set) const
    {
        Vector<const B3::AbstractHeap*, 8> heaps;
        for (const B3::HeapRange& range : set.ranges())
            collectHeapsWithin(m_heaps.root, range, heaps);

        StringPrintStream out;
        CommaPrinter comma;
        if (heaps.size() != 1)
            out.print("{");
        for (const B3::AbstractHeap* heap : heaps) {
            out.print(comma);
            printName(out, *heap);
        }
        if (heaps.size() != 1)
            out.print("}");
        return out.toString();
    }

private:
    void collectHeapsWithin(const B3::AbstractHeap& heap, const B3::HeapRange& range, Vector<const B3::AbstractHeap*, 8>& result) const
    {
        if (!heap.staysAcrossProcedures() && !heap.hasRange())
            return;
        B3::HeapRange heapRange = m_heaps.rangeOf(heap);
        if (!heapRange.overlaps(range))
            return;
        if (range.begin() <= heapRange.begin() && heapRange.end() <= range.end()) {
            result.append(&heap);
            return;
        }
        for (const B3::AbstractHeap* child : heap.children())
            collectHeapsWithin(*child, range, result);
    }

    static void printName(PrintStream& out, const B3::AbstractHeap& heap)
    {
        const B3::AbstractHeap* parent = heap.parent();
        size_t numberOfSiblingsWithName = 0;
        size_t indexAmongSiblings = 0;
        if (parent) {
            for (const B3::AbstractHeap* sibling : parent->children()) {
                if (sibling == &heap)
                    indexAmongSiblings = numberOfSiblingsWithName;
                if (!std::strcmp(sibling->heapName(), heap.heapName()))
                    ++numberOfSiblingsWithName;
            }
        }
        if (numberOfSiblingsWithName < 2) {
            out.print(heap.heapName());
            return;
        }
        printName(out, *parent);
        out.print(".", indexAmongSiblings);
    }

    const B3::AbstractHeapRepository& m_heaps;
};

} // anonymous namespace

static String removeGeneratorAddress(const String& text)
{
    constexpr ASCIILiteral label = "generator = "_s;
    constexpr ASCIILiteral separator = ", "_s;

    size_t begin = text.find(label);
    if (begin == notFound)
        return text;
    size_t end = text.find(separator, begin);
    if (end != notFound)
        return makeString(StringView(text).left(begin), StringView(text).substring(end + separator.length()));
    end = text.find(')', begin);
    RELEASE_ASSERT(end != notFound && begin >= separator.length());
    return makeString(StringView(text).left(begin - separator.length()), StringView(text).substring(end));
}

static String nameHeaps(const String& text, ASCIILiteral label, const B3::HeapSet& set, const HeapNamePrinter& heapNames)
{
    if (!set || set == B3::HeapRange::top())
        return text;
    String numbered = makeString(label, toString(set));
    size_t begin = text.reverseFind(numbered);
    RELEASE_ASSERT(begin != notFound);
    return makeString(StringView(text).left(begin), label, heapNames.describe(set), StringView(text).substring(begin + numbered.length()));
}

static void dumpValue(PrintStream& out, const B3::Procedure& procedure, const HeapNamePrinter& heapNames, B3::Value* value)
{
    String text = removeGeneratorAddress(toString(B3::deepDump(procedure, value)));
    B3::Effects effects = value->effects();
    text = nameHeaps(text, "Reads:"_s, effects.reads, heapNames);
    text = nameHeaps(text, "Writes:"_s, effects.writes, heapNames);
    if (auto* memory = value->as<B3::MemoryValue>(); memory && text.contains("range = "_s))
        text = nameHeaps(text, "range = "_s, memory->range(), heapNames);
    out.println("    ", text);
}

static Vector<B3::BasicBlock*> orderBlocksLikelyPathFirst(const B3::Procedure& procedure)
{
    Vector<B3::BasicBlock*> order;
    BitVector isVisited(procedure.size());
    Vector<B3::BasicBlock*, 16> worklist { procedure.at(0) };
    while (!worklist.isEmpty()) {
        B3::BasicBlock* block = worklist.takeLast();
        if (isVisited.quickSet(block->index()))
            continue;
        order.append(block);
        for (unsigned rarity = 3; rarity--;) {
            for (unsigned index = block->numSuccessors(); index--;) {
                if (rarityOf(block->successor(index).frequency()) == rarity)
                    worklist.append(block->successorBlock(index));
            }
        }
    }
    return order;
}

static void dumpBlock(PrintStream& out, const B3::Procedure& procedure, const HeapNamePrinter& heapNames, B3::BasicBlock* block)
{
    out.println("BB", *block, ": ; frequency = ", block->frequency());
    if (block->numPredecessors())
        out.println("  Predecessors: ", pointerListDump(block->predecessors()));
    for (B3::Value* value : *block)
        dumpValue(out, procedure, heapNames, value);
    if (!block->numSuccessors())
        return;
    out.print("  Successors: ");
    block->last()->dumpSuccessors(block, out);
    out.println();
}

static void dumpKnownProperties(PrintStream& out, const B3::Procedure& procedure)
{
    bool didPrintHeading = false;
    auto printHeadingOnce = [&] {
        if (!std::exchange(didPrintHeading, true))
            out.println("Known properties:");
    };
    for (B3::Value* value : procedure.values()) {
        std::optional<B3::IntRange> range = procedure.knownRange(value);
        uint32_t dereferenceableBytes = procedure.knownDereferenceableBytes(value);
        if (!range && !dereferenceableBytes)
            continue;
        printHeadingOnce();
        out.print("    ", *value, ":");
        if (range)
            out.print(" range = ", *range);
        if (dereferenceableBytes)
            out.print(" dereferenceableBytes = ", dereferenceableBytes);
        out.println();
    }
    procedure.knownInductions().forEach([&](const B3::InductionRecord& record) {
        printHeadingOnce();
        out.print("    ", *record.phi, ": induction, next = ", *record.next, ", step = ", record.step);
        if (record.test)
            out.print(", test = ", *record.test);
        if (record.doesNotWrap)
            out.print(", doesNotWrap");
        if (record.relationHoldsOnEntry)
            out.print(", relationHoldsOnEntry");
        out.println();
    });
}

static void dumpWholeProcedure(PrintStream& out, const B3::Procedure& procedure, const HeapNamePrinter& heapNames)
{
    for (B3::BasicBlock* block : orderBlocksLikelyPathFirst(procedure))
        dumpBlock(out, procedure, heapNames, block);
    if (procedure.variables().size()) {
        out.println("Variables:");
        for (B3::Variable* variable : procedure.variables())
            out.println("    ", deepDump(variable));
    }
    if (procedure.stackSlots().size()) {
        out.println("Stack slots:");
        for (B3::Air::StackSlot* slot : procedure.stackSlots())
            out.println("    ", pointerDump(slot), ": ", deepDump(slot));
    }
    dumpKnownProperties(out, procedure);
}

static std::optional<int64_t> stepFrom(B3::Value* phi, B3::Value* next)
{
    if (next->numChildren() != 2)
        return std::nullopt;
    B3::Value* left = next->child(0);
    B3::Value* right = next->child(1);
    if (next->opcode() == B3::Add && right == phi)
        std::swap(left, right);
    if (left != phi || !right->hasInt())
        return std::nullopt;
    if (next->opcode() == B3::Add)
        return right->asInt();
    if (next->opcode() == B3::Sub)
        return -right->asInt();
    return std::nullopt;
}

static Vector<LoopCounter> findCounters(const B3::Procedure& procedure, const B3::NaturalLoop& loop)
{
    Vector<LoopCounter> counters;
    for (B3::Value* phi : *loop.header()) {
        if (phi->opcode() != B3::Phi)
            continue;
        if (std::optional<B3::InductionRecord> induction = procedure.knownInduction(phi)) {
            counters.append({ phi, induction->step });
            continue;
        }
        std::optional<int64_t> step;
        bool hasOneStep = true;
        for (unsigned index = 0; index < loop.size(); ++index) {
            for (B3::Value* value : *loop.at(index)) {
                auto* upsilon = value->as<B3::UpsilonValue>();
                if (!upsilon || upsilon->phi() != phi)
                    continue;
                std::optional<int64_t> stepOnThisEdge = stepFrom(phi, upsilon->child(0));
                hasOneStep &= stepOnThisEdge && (!step || step == stepOnThisEdge);
                step = stepOnThisEdge;
            }
        }
        if (step && hasOneStep)
            counters.append({ phi, *step });
    }
    return counters;
}

static void dumpLoops(PrintStream& out, B3::Procedure& procedure, const HeapNamePrinter& heapNames)
{
    B3::Dominators dominators(procedure);
    B3::NaturalLoops loops(procedure, dominators);
    Vector<B3::BasicBlock*> order = orderBlocksLikelyPathFirst(procedure);

    for (B3::BasicBlock* header : order) {
        const B3::NaturalLoop* loop = loops.headerOf(header);
        if (!loop)
            continue;
        Vector<B3::BasicBlock*> blocks;
        for (B3::BasicBlock* block : order) {
            if (loops.innerMostLoopOf(block) == loop)
                blocks.append(block);
        }
        bool isInnermost = blocks.size() == loop->size();
        if (!isInnermost)
            continue;

        out.print("loop ", *loop->header());
        if (const B3::NaturalLoop* outerLoop = loops.innerMostOuterLoop(*loop))
            out.print(" in loop ", *outerLoop->header());
        for (const LoopCounter& counter : findCounters(procedure, *loop))
            out.print(", ", *counter.phi, " steps by ", counter.step);
        out.println();
        for (B3::BasicBlock* block : blocks)
            dumpBlock(out, procedure, heapNames, block);
        out.println("end of loop ", *loop->header());
    }
}

static void dumpAirBlock(PrintStream& out, const B3::Air::BasicBlock& block)
{
    out.println("BB", block, ": ; frequency = ", block.frequency());
    if (block.numPredecessors())
        out.println("  Predecessors: ", pointerListDump(block.predecessors()));
    for (const B3::Air::Inst& inst : block)
        out.println("    ", inst);
    if (block.numSuccessors())
        out.println("  Successors: ", listDump(block.successors()));
}

static std::optional<AirLoopCounter> counterSteppedBy(const B3::Air::Inst& inst)
{
    using namespace B3::Air;

    bool isAdd = inst.kind.opcode == Add32 || inst.kind.opcode == Add64;
    bool isSub = inst.kind.opcode == Sub32 || inst.kind.opcode == Sub64;
    auto args = inst.args();
    if ((!isAdd && !isSub) || !args[0].isImm())
        return std::nullopt;
    const Arg& destination = args[args.size() - 1];
    if (!destination.isTmp() || args[1] != destination)
        return std::nullopt;
    return AirLoopCounter { destination.tmp(), isAdd ? args[0].value() : -args[0].value() };
}

static Vector<AirLoopCounter> findAirCounters(std::span<B3::Air::BasicBlock* const> blocks)
{
    using namespace B3::Air;

    auto numberOfDefinitions = [&](Tmp counter) {
        unsigned count = 0;
        for (BasicBlock* block : blocks) {
            for (Inst& inst : *block) {
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, B3::Bank, Width) {
                    count += tmp == counter && Arg::isAnyDef(role);
                });
            }
        }
        return count;
    };

    Vector<AirLoopCounter> counters;
    for (BasicBlock* block : blocks) {
        for (const Inst& inst : *block) {
            std::optional<AirLoopCounter> counter = counterSteppedBy(inst);
            if (counter && numberOfDefinitions(counter->tmp) == 1)
                counters.append(*counter);
        }
    }
    return counters;
}

static BitVector findBlocksReachedWithoutRareEdge(const B3::Air::Code& code, B3::Air::BasicBlock* entry, std::span<B3::Air::BasicBlock* const> region)
{
    BitVector isInRegion(code.size());
    for (B3::Air::BasicBlock* block : region)
        isInRegion.quickSet(block->index());

    BitVector isReached(code.size());
    isReached.quickSet(entry->index());
    Vector<B3::Air::BasicBlock*, 16> worklist { entry };
    while (!worklist.isEmpty()) {
        for (const B3::Air::FrequentedBlock& successor : worklist.takeLast()->successors()) {
            unsigned index = successor.block()->index();
            if (!successor.isRare() && isInRegion.quickGet(index) && !isReached.quickSet(index))
                worklist.append(successor.block());
        }
    }
    return isReached;
}

// Of a loop: the blocks of a turn that goes round without a rare edge. What only leads out of the loop, or back to its top by a rare edge, is no part of such a turn.
static BitVector findBlocksOfLikelyTurns(const B3::Air::Code& code, B3::Air::BasicBlock* header, std::span<B3::Air::BasicBlock* const> region)
{
    BitVector isReached = findBlocksReachedWithoutRareEdge(code, header, region);
    BitVector leadsBack(code.size());
    bool changed = true;
    while (changed) {
        changed = false;
        for (B3::Air::BasicBlock* block : region) {
            if (!isReached.quickGet(block->index()) || leadsBack.quickGet(block->index()))
                continue;
            for (const B3::Air::FrequentedBlock& successor : block->successors()) {
                if (!successor.isRare() && (successor.block() == header || leadsBack.quickGet(successor.block()->index()))) {
                    leadsBack.quickSet(block->index());
                    changed = true;
                    break;
                }
            }
        }
    }
    return leadsBack;
}

static void dumpAirSize(PrintStream& out, std::span<B3::Air::BasicBlock* const> blocks, const BitVector& isLikely)
{
    unsigned numberOfInstructions = 0;
    unsigned numberOfPatchpoints = 0;
    unsigned numberOfRareInstructions = 0;
    for (B3::Air::BasicBlock* block : blocks) {
        if (!isLikely.quickGet(block->index())) {
            numberOfRareInstructions += block->size();
            continue;
        }
        for (const B3::Air::Inst& inst : *block)
            ++(inst.kind.opcode == B3::Air::Patch ? numberOfPatchpoints : numberOfInstructions);
    }
    out.print("instructions: ", numberOfInstructions, ", patchpoints: ", numberOfPatchpoints);
    if (numberOfRareInstructions)
        out.print(", rare instructions: ", numberOfRareInstructions);
}

static Vector<B3::Air::BasicBlock*> collectBlocks(B3::Air::Code& code)
{
    Vector<B3::Air::BasicBlock*> blocks;
    for (B3::Air::BasicBlock* block : code)
        blocks.append(block);
    return blocks;
}

enum class PrintedLoopParts : uint8_t { HeadingOnly, HeadingAndBlocks };

// The moves on the way into a loop may be the same as those of its latch, and then one block serves both: a cycle with two ways in, which is no natural loop.
// Here the way in from outside gets a block of its own again.
static void splitLatchesThatAreEnteredFromOutside(B3::Air::Code& code)
{
    using namespace B3::Air;

    Dominators dominators(code);
    NaturalLoops loops(code, dominators);
    Vector<BasicBlock*> latches;
    for (BasicBlock* block : code) {
        if (block->numSuccessors() != 1 || dominators.dominates(block->successorBlock(0), block) || loops.headerOf(block->successorBlock(0)))
            continue;
        bool isEnteredFromInside = false;
        for (BasicBlock* predecessor : block->predecessors())
            isEnteredFromInside |= dominators.dominates(block->successorBlock(0), predecessor);
        if (isEnteredFromInside)
            latches.append(block);
    }
    for (BasicBlock* latch : latches) {
        BasicBlock* header = latch->successorBlock(0);
        BasicBlock* wayIn = code.addBlock(latch->frequency());
        wayIn->insts() = latch->insts();
        wayIn->setSuccessors(latch->successor(0));
        for (BasicBlock* predecessor : latch->predecessors()) {
            if (dominators.dominates(header, predecessor))
                continue;
            for (B3::Air::FrequentedBlock& successor : predecessor->successors()) {
                if (successor.block() == latch)
                    successor.block() = wayIn;
            }
        }
    }
    if (!latches.isEmpty())
        code.resetReachability();
}

static void dumpAirLoops(PrintStream& out, B3::Air::Code& code, PrintedLoopParts parts)
{
    using namespace B3::Air;

    splitLatchesThatAreEnteredFromOutside(code);
    Dominators dominators(code);
    NaturalLoops loops(code, dominators);
    BitVector isLikelyInFunction = findBlocksReachedWithoutRareEdge(code, code[0], collectBlocks(code).span());
    for (BasicBlock* header : code) {
        const NaturalLoop* loop = loops.headerOf(header);
        if (!loop)
            continue;
        Vector<BasicBlock*> blocks;
        for (BasicBlock* block : code) {
            if (loops.innerMostLoopOf(block) == loop)
                blocks.append(block);
        }
        bool isInnermost = blocks.size() == loop->size();
        if (!isInnermost)
            continue;

        out.print("loop ", *header, ", ");
        BitVector isInLikelyTurn = findBlocksOfLikelyTurns(code, header, blocks.span());
        dumpAirSize(out, blocks.span(), isInLikelyTurn);
        Vector<BasicBlock*> blocksOfLikelyTurns;
        for (BasicBlock* block : blocks) {
            if (isInLikelyTurn.quickGet(block->index()))
                blocksOfLikelyTurns.append(block);
        }
        for (const AirLoopCounter& counter : findAirCounters(blocksOfLikelyTurns.span()))
            out.print(", ", counter.tmp, " steps by ", counter.step);
        if (!isLikelyInFunction.quickGet(header->index()))
            out.print(", entered rarely");
        out.println();
        if (parts == PrintedLoopParts::HeadingOnly)
            continue;
        for (BasicBlock* block : blocks)
            dumpAirBlock(out, *block);
        out.println("end of loop ", *header);
    }
}

void dumpAir(PrintStream& out, B3::Air::Code& code, B3DumpScope scope)
{
    switch (scope) {
    case B3DumpScope::WholeProcedure:
        for (B3::Air::BasicBlock* block : code)
            dumpAirBlock(out, *block);
        return;
    case B3DumpScope::LoopsOnly:
        dumpAirLoops(out, code, PrintedLoopParts::HeadingAndBlocks);
        return;
    case B3DumpScope::SizeOnly:
    case B3DumpScope::SizeOfFunctionAndLoops:
        dumpAirSize(out, collectBlocks(code).span(), findBlocksReachedWithoutRareEdge(code, code[0], collectBlocks(code).span()));
        out.println();
        if (scope == B3DumpScope::SizeOfFunctionAndLoops)
            dumpAirLoops(out, code, PrintedLoopParts::HeadingOnly);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printOriginsAsOperations(B3::Procedure& procedure, const Function& function)
{
    procedure.setOriginPrinter([&function](PrintStream& out, B3::Origin origin) {
        ValueID id = valueOfOrigin(origin);
        out.print("v", id.index());
        if (id.index() < function.values.size())
            out.print(":", nameOf(function.at(id).opcode));
    });
}

void dumpB3(PrintStream& out, B3::Procedure& procedure, B3DumpScope scope)
{
    HeapNamePrinter heapNames(procedure.heaps());
    switch (scope) {
    case B3DumpScope::WholeProcedure:
        dumpWholeProcedure(out, procedure, heapNames);
        return;
    case B3DumpScope::LoopsOnly:
        dumpLoops(out, procedure, heapNames);
        return;
    case B3DumpScope::SizeOnly:
    case B3DumpScope::SizeOfFunctionAndLoops:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
