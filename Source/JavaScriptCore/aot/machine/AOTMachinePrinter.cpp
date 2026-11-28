/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachinePrinter.h"

#if ENABLE(AOT)

#include "AOTMachineSlots.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Machine {

static void printMachineRegister(PrintStream& out, Reg reg)
{
    if (reg)
        out.print(reg.debugName());
    else
        out.print("none");
}

static void printMachineRegisterList(PrintStream& out, RegisterMask mask)
{
    out.print("[");
    CommaPrinter comma;
    forEachRegister(mask, [&](Reg reg) {
        out.print(comma, reg.debugName());
    });
    out.print("]");
}

static void printMachineOffset(PrintStream& out, int64_t offset)
{
    if (offset > 0)
        out.print(" + ", offset);
    else if (offset < 0)
        out.print(" - ", -offset);
}

static void printMachinePosition(PrintStream& out, Position position)
{
    if (position == noPosition)
        out.print("none");
    else
        out.print(position);
}

MAKE_PRINT_ADAPTOR(MachineRegisterDump, Reg, printMachineRegister);
MAKE_PRINT_ADAPTOR(MachineRegisterListDump, RegisterMask, printMachineRegisterList);
MAKE_PRINT_ADAPTOR(MachineOffsetDump, int64_t, printMachineOffset);
MAKE_PRINT_ADAPTOR(MachinePositionDump, Position, printMachinePosition);

template<typename Number>
static void printMachineNumber(PrintStream& out, Number value)
{
    ASSERT(!std::isnan(value));
    if (!value) {
        out.print(std::signbit(value) ? "-0" : "0");
        return;
    }
    if (std::isinf(value)) {
        out.print(value < 0 ? "-Infinity" : "Infinity");
        return;
    }
    NumberToStringBuffer buffer;
    out.print(numberToStringAndSize(value, buffer));
}

class MachinePrinterLine final : public PrintStream {
public:
    explicit MachinePrinterLine(PrintStream& target)
        : m_target(target)
    {
    }

    void vprintf(const char* format, va_list) final WTF_ATTRIBUTE_PRINTF(2, 0);
    unsigned length() const { return m_length; }

private:
    PrintStream& m_target;
    unsigned m_length { 0 };
};

void MachinePrinterLine::vprintf(const char* format, va_list arguments)
{
    va_list copy;
    va_copy(copy, arguments);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    int length = vsnprintf(nullptr, 0, format, copy);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    va_end(copy);
    if (length > 0)
        m_length += static_cast<unsigned>(length);
    m_target.vprintf(format, arguments);
}

class MachinePrinter {
public:
    explicit MachinePrinter(const Procedure& procedure)
        : m_proc(procedure)
    {
    }

    void printProcedure(PrintStream&);
    void printOperation(PrintStream&, const Operation*);
    void printOrder(PrintStream&);
    void printLiveness(PrintStream&);
    void printAllocation(PrintStream&);
    void printFrame(PrintStream&);
    void printStep(PrintStream&, const Step&);

    static ASCIILiteral nameOf(TypeKind);
    static ASCIILiteral nameOf(EdgeClass);
    static ASCIILiteral nameOf(FailureKind);
    static ASCIILiteral nameOf(StepKind);
    static ASCIILiteral nameOf(FrameCoverage);
    static ASCIILiteral nameOf(SymbolSpace);

private:
    class Leaves {
    public:
        explicit Leaves(const Step& step)
            : m_sources(step.sources)
        {
        }

        Location take() { return m_next < m_sources.size() ? m_sources[m_next++] : Location(); }

    private:
        std::span<const Location> m_sources;
        size_t m_next { 0 };
    };

    static constexpr unsigned columnOfValue = 31;

    static const Block* blockEndedBy(const Operation*);
    unsigned ordinalOf(const PatchGenerator*);

    void printType(PrintStream&, Type);
    void printHeap(PrintStream&, HeapIndex);
    void printHeapSet(PrintStream&, const HeapSet&);
    void printEffects(PrintStream&, const Effects&);
    void printPayload(PrintStream&, CommaPrinter&, const Operation*);
    void printPatch(PrintStream&, CommaPrinter&, const PatchOperation&);
    void printSuccessors(PrintStream&, const Block*);
    void printBlock(PrintStream&, const Block*, std::span<const uint32_t> predecessors);

    void printWithoutPosition(PrintStream&, const Operation*);
    void printPlace(PrintStream&, Location, const Operation*);
    void printOperand(PrintStream&, Leaves&, const Operation* user, unsigned childIndex);
    void printOperands(PrintStream&, Leaves&, const Operation*, CommaPrinter&);
    void printExecute(PrintStream&, const Step&);
    void printTextOfStep(PrintStream&, const Step&);
    void printStepLine(PrintStream&, const Step&);
    void printStoreToSpill(PrintStream&, Operation*);

    const Procedure& m_proc;
    HashMap<const PatchGenerator*, unsigned> m_generatorOrdinals;
};

ASCIILiteral MachinePrinter::nameOf(TypeKind kind)
{
    switch (kind) {
    case Void:
        return "Void"_s;
    case Int32:
        return "Int32"_s;
    case Int64:
        return "Int64"_s;
    case Float:
        return "Float"_s;
    case Double:
        return "Double"_s;
    case Vector128:
        return "Vector128"_s;
    case Tuple:
        return "Tuple"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral MachinePrinter::nameOf(EdgeClass edgeClass)
{
    switch (edgeClass) {
    case EdgeClass::Normal:
        return "normal"_s;
    case EdgeClass::Unlikely:
        return "unlikely"_s;
    case EdgeClass::Rare:
        return "rare"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral MachinePrinter::nameOf(FailureKind kind)
{
    switch (kind) {
    case FailureKind::MalformedProcedure:
        return "MalformedProcedure"_s;
    case FailureKind::OutOfRegisters:
        return "OutOfRegisters"_s;
    case FailureKind::AccessOutsideDeclaredEffects:
        return "AccessOutsideDeclaredEffects"_s;
    case FailureKind::WrongValueInRegister:
        return "WrongValueInRegister"_s;
    case FailureKind::WrongValueInSpillSlot:
        return "WrongValueInSpillSlot"_s;
    case FailureKind::FrameAddressAcrossSuspension:
        return "FrameAddressAcrossSuspension"_s;
    case FailureKind::NarrowReferenceWithoutPointer:
        return "NarrowReferenceWithoutPointer"_s;
    case FailureKind::InnerPointerWithoutOwner:
        return "InnerPointerWithoutOwner"_s;
    case FailureKind::MissingFrameSetup:
        return "MissingFrameSetup"_s;
    case FailureKind::UpperHalfIsNotClear:
        return "UpperHalfIsNotClear"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral MachinePrinter::nameOf(StepKind kind)
{
    switch (kind) {
    case StepKind::Execute:
        return "execute"_s;
    case StepKind::Move:
        return "move"_s;
    case StepKind::Reload:
        return "reload"_s;
    case StepKind::Save:
        return "save"_s;
    case StepKind::Materialize:
        return "materialize"_s;
    case StepKind::Swap:
        return "swap"_s;
    case StepKind::KeepPointer:
        return "keepPointer"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral MachinePrinter::nameOf(FrameCoverage coverage)
{
    switch (coverage) {
    case FrameCoverage::None:
        return "None"_s;
    case FrameCoverage::WholeFunction:
        return "WholeFunction"_s;
    case FrameCoverage::RareRegions:
        return "RareRegions"_s;
    case FrameCoverage::FramedPart:
        return "FramedPart"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral MachinePrinter::nameOf(SymbolSpace space)
{
    switch (space) {
    case SymbolSpace::Function:
        return "Function"_s;
    case SymbolSpace::Stub:
        return "Stub"_s;
    case SymbolSpace::ArgumentThunk:
        return "ArgumentThunk"_s;
    case SymbolSpace::Constant:
        return "Constant"_s;
    case SymbolSpace::JumpTable:
        return "JumpTable"_s;
    case SymbolSpace::Number:
        return "Number"_s;
    case SymbolSpace::InstanceWord:
        return "InstanceWord"_s;
    case SymbolSpace::RegExp:
        return "RegExp"_s;
    case SymbolSpace::WebAssembly:
        return "WebAssembly"_s;
    case SymbolSpace::FFI:
        return "FFI"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Block* MachinePrinter::blockEndedBy(const Operation* operation)
{
    const Block* block = operation->owner();
    return block && block->isTerminated() && block->last() == operation ? block : nullptr;
}

unsigned MachinePrinter::ordinalOf(const PatchGenerator* generator)
{
    if (m_generatorOrdinals.isEmpty()) {
        for (unsigned index = 0; index < m_proc.numberOfOperations(); ++index) {
            const PatchOperation* patch = m_proc.operationAt(index)->as<PatchOperation>();
            if (patch && patch->generator())
                m_generatorOrdinals.add(patch->generator(), m_generatorOrdinals.size());
        }
    }
    return m_generatorOrdinals.get(generator);
}

void MachinePrinter::printType(PrintStream& out, Type type)
{
    if (!type.isTuple()) {
        out.print(type);
        return;
    }
    out.print("Tuple(");
    CommaPrinter comma;
    for (Type member : m_proc.tupleOf(type))
        out.print(comma, member);
    out.print(")");
}

void MachinePrinter::printHeap(PrintStream& out, HeapIndex heap)
{
    const HeapTree& tree = m_proc.heapTree();
    if (heap >= tree.size()) {
        out.print("heap outside the tree: ", heap);
        return;
    }

    ASCIILiteral name = tree.nameOf(heap);
    out.print(name);

    HeapIndex parent = tree.parentOf(heap);
    if (heap == rootHeapIndex || tree.nameOf(parent) != name)
        return;
    unsigned ordinal = 0;
    for (unsigned sibling = parent + 1; sibling < heap; ++sibling) {
        HeapIndex siblingHeap = static_cast<HeapIndex>(sibling);
        ordinal += tree.parentOf(siblingHeap) == parent && tree.nameOf(siblingHeap) == name;
    }
    if (ordinal == IndexedAbstractHeap::numberOfIndexesWithOwnHeap)
        out.print("[others]");
    else
        out.print("[", ordinal, "]");
}

void MachinePrinter::printHeapSet(PrintStream& out, const HeapSet& set)
{
    if (set.isTop()) {
        out.print("Top");
        return;
    }
    out.print("{");
    CommaPrinter comma;
    for (HeapIndex heap : set.heaps()) {
        out.print(comma);
        printHeap(out, heap);
    }
    out.print("}");
}

void MachinePrinter::printEffects(PrintStream& out, const Effects& effects)
{
    CommaPrinter bar("|"_s);
    if (effects.terminal)
        out.print(bar, "Terminal");
    if (effects.exitsSideways)
        out.print(bar, "ExitsSideways");
    if (effects.controlDependent)
        out.print(bar, "ControlDependent");
    if (effects.writesLocalState)
        out.print(bar, "WritesLocalState");
    if (effects.mayCollect)
        out.print(bar, "MayCollect");
    if (!effects.writes.isEmpty()) {
        out.print(bar, "Writes:");
        printHeapSet(out, effects.writes);
    }
    if (!effects.reads.isEmpty()) {
        out.print(bar, "Reads:");
        printHeapSet(out, effects.reads);
    }
    if (!bar.didPrint())
        out.print("none");
}

void MachinePrinter::printPatch(PrintStream& out, CommaPrinter& comma, const PatchOperation& patch)
{
    if (!patch.resultConstraints.isEmpty()) {
        out.print(comma, "resultConstraints = [");
        CommaPrinter constraintComma;
        for (const ValueRep& constraint : patch.resultConstraints.span())
            out.print(constraintComma, constraint);
        out.print("]");
    }
    if (patch.numGPScratchRegisters)
        out.print(comma, "numGPScratchRegisters = ", static_cast<unsigned>(patch.numGPScratchRegisters));
    if (patch.numFPScratchRegisters)
        out.print(comma, "numFPScratchRegisters = ", static_cast<unsigned>(patch.numFPScratchRegisters));

    if (patch.earlyClobberedMask())
        out.print(comma, "earlyClobbered = ", MachineRegisterListDump(patch.earlyClobberedMask()));
    if (patch.lateClobberedMask())
        out.print(comma, "lateClobbered = ", MachineRegisterListDump(patch.lateClobberedMask()));
    if (const Block* block = blockEndedBy(&patch)) {
        for (unsigned index = 0; index < block->numSuccessors(); ++index) {
            if (RegisterMask mask = patch.lateClobberedOnSuccessor(index))
                out.print(comma, "lateClobberedOnSuccessor(", index, ") = ", MachineRegisterListDump(mask));
        }
    }

    out.print(comma, "effects = ");
    printEffects(out, patch.effects);

    if (patch.onlyKeepsChildrenAlive)
        out.print(comma, "OnlyKeepsChildrenAlive");
    if (patch.runsWithoutFrame())
        out.print(comma, "RunsWithoutFrame");
    if (patch.followsFrameSetup())
        out.print(comma, "FollowsFrameSetup");
    if (patch.testsStackAheadOfFramedPart())
        out.print(comma, "TestsStackAheadOfFramedPart");
    if (patch.leavesByReturning())
        out.print(comma, "LeavesByReturning");
    if (patch.needsFrameRecord())
        out.print(comma, "NeedsFrameRecord");
    if (patch.isSuspension())
        out.print(comma, "IsSuspension");
    if (patch.sharesLatePath())
        out.print(comma, "SharesLatePath");
    if (patch.failsWhenPredicateIsZero())
        out.print(comma, "FailsWhenPredicateIsZero");

    if (patch.pureFunction) {
        uint64_t id = patch.pureFunction->id;
        out.print(comma, "pureFunction = ");
        if (id >= PureFunction::firstIDOfBackEnd && id - PureFunction::firstIDOfBackEnd < numberOfOpcodes)
            out.print(static_cast<Opcode>(id - PureFunction::firstIDOfBackEnd));
        else
            out.print(id);
        if (patch.pureFunction->dependsOnChildren)
            out.print(comma, "DependsOnChildren");
        if (patch.pureFunction->truncatesDoubleToInt64)
            out.print(comma, "TruncatesDoubleToInt64");
        if (patch.pureFunction->dependsOnMemory)
            out.print(comma, "DependsOnMemory");
    }

    out.print(comma, "generator = ");
    if (patch.generator())
        out.print(ordinalOf(patch.generator()));
    else
        out.print("none");
}

void MachinePrinter::printPayload(PrintStream& out, CommaPrinter& comma, const Operation* operation)
{
    switch (operation->opcode()) {
    case Const32:
        out.print(comma, operation->asInt32());
        return;
    case Const64:
        out.print(comma, operation->asInt64());
        return;
    case ConstFloat:
        out.print(comma);
        if (std::isnan(operation->asFloat()))
            out.printf("NaN(0x%08x)", static_cast<uint32_t>(operation->constantBits()));
        else
            printMachineNumber(out, operation->asFloat());
        return;
    case ConstDouble:
        out.print(comma);
        if (std::isnan(operation->asDouble()))
            out.printf("NaN(0x%016llx)", static_cast<unsigned long long>(operation->constantBits()));
        else
            printMachineNumber(out, operation->asDouble());
        return;
    case SlotBase:
        out.print(comma, "slot", operation->slot()->index());
        return;
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf: {
        CodeSymbol symbol = operation->symbol();
        out.print(comma, nameOf(symbol.space), ":", symbol.index);
        if (symbol.entry != primaryEntry)
            out.print(":", symbol.entry);
        return;
    }
    case ArgumentReg:
        out.print(comma, MachineRegisterDump(operation->argumentReg()));
        return;
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
        out.print(comma, "offset = ", operation->offset(), ", ");
        printHeap(out, operation->heap());
        return;
    case Upsilon:
        out.print(comma, "phi = ", pointerDump(operation->phi()));
        return;
    case Extract:
        out.print(comma, "index = ", operation->extractedIndex());
        return;
    case Patch:
    case Check:
        printPatch(out, comma, *static_cast<const PatchOperation*>(operation));
        return;
    default:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MachinePrinter::printSuccessors(PrintStream& out, const Block* block)
{
    bool isSwitch = block->isTerminated() && block->last()->opcode() == Switch;
    std::span<const int64_t> caseValues;
    if (isSwitch)
        caseValues = block->last()->switchData().caseValues;

    CommaPrinter comma(", "_s, " -> "_s);
    for (unsigned index = 0; index < block->numSuccessors(); ++index) {
        const Edge& edge = block->successor(index);
        out.print(comma);
        if (isSwitch && index < caseValues.size())
            out.print(caseValues[index], ": ");
        else if (isSwitch)
            out.print("otherwise: ");
        if (edge.edgeClass() != EdgeClass::Normal)
            out.print(edge.edgeClass(), " ");
        out.print(pointerDump(edge.block()));
    }
}

void MachinePrinter::printOperation(PrintStream& out, const Operation* operation)
{
    printType(out, operation->type());
    out.print(" ", *operation, " = ", operation->opcode(), "(");

    CommaPrinter comma;
    const PatchOperation* patch = operation->as<PatchOperation>();
    std::span<Operation* const> children = operation->children();
    for (unsigned index = 0; index < children.size(); ++index) {
        out.print(comma, pointerDump(children[index]));
        bool isPredicateOfCheck = !index && operation->opcode() == Check;
        if (patch && !isPredicateOfCheck)
            out.print(":", patch->constraintOf(index));
    }
    printPayload(out, comma, operation);

    if (operation->has(OperationFlag::Traps))
        out.print(comma, "Traps");
    if (operation->has(OperationFlag::IsNarrowReference))
        out.print(comma, "IsNarrowReference");
    if (operation->has(OperationFlag::IsInnerPointer))
        out.print(comma, "IsInnerPointer");
    if (operation->has(OperationFlag::IsChill))
        out.print(comma, "IsChill");
    if (operation->has(OperationFlag::ReadsImmutableHeap))
        out.print(comma, "ReadsImmutableHeap");
    if (operation->has(OperationFlag::NamesPermanentCell))
        out.print(comma, "NamesPermanentCell");
    if (operation->has(OperationFlag::IsIntegerAddedTo))
        out.print(comma, "IsIntegerAddedTo");
    if (operation->has(OperationFlag::ExtendsWordThatIsNotNegative))
        out.print(comma, "ExtendsWordThatIsNotNegative");
    if (operation->opcode() == Phi && operation->type() == Int64 && operation->useCount() && !operation->has(OperationFlag::UpperHalfIsRead))
        out.print(comma, "UpperHalfIsNotRead");
    if (operation->origin())
        out.print(comma, "origin = ", operation->origin().bits());
    out.print(")");

    if (const Block* block = blockEndedBy(operation))
        printSuccessors(out, block);
}

void MachinePrinter::printBlock(PrintStream& out, const Block* block, std::span<const uint32_t> predecessors)
{
    out.print(*block, ":");
    if (!predecessors.empty())
        out.print(" <-");
    for (uint32_t predecessor : predecessors)
        out.print(" #", predecessor);

    CommaPrinter comma(", "_s, predecessors.empty() ? " "_s : ", "_s);
    if (block->isEntry()) {
        for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
            if (entrypoint.block == block)
                out.print(comma, "entrypoint ", entrypoint.id);
        }
    }
    if (block->isLandingPad())
        out.print(comma, "landing pad");
    if (block->hasOnlyOnePredecessor())
        out.print(comma, "only one predecessor");
    out.println();

    auto printLine = [&](const Operation* operation) {
        out.print("    ");
        printOperation(out, operation);
        out.println();
    };

    bool isTerminated = block->isTerminated();
    std::span<Operation* const> operations = block->operations();
    for (const Operation* phi : block->phis())
        printLine(phi);
    for (const Operation* operation : operations.first(operations.size() - isTerminated))
        printLine(operation);
    for (const Operation* upsilon : block->upsilons())
        printLine(upsilon);
    if (isTerminated)
        printLine(block->last());
    else if (block->numSuccessors()) {
        out.print("   ");
        printSuccessors(out, block);
        out.println();
    }
}

void MachinePrinter::printProcedure(PrintStream& out)
{
    std::span<Block* const> blocks = m_proc.blocks();

    Vector<uint32_t> endOfPredecessors;
    endOfPredecessors.fill(0, blocks.size());
    for (const Block* block : blocks) {
        for (const Edge& edge : block->successors()) {
            if (edge)
                ++endOfPredecessors[edge.block()->index()];
        }
    }
    uint32_t numberOfEdges = 0;
    for (uint32_t& entry : endOfPredecessors) {
        uint32_t count = entry;
        entry = numberOfEdges;
        numberOfEdges += count;
    }
    Vector<uint32_t> predecessors;
    predecessors.fill(0, numberOfEdges);
    for (const Block* block : blocks) {
        for (const Edge& edge : block->successors()) {
            if (edge)
                predecessors[endOfPredecessors[edge.block()->index()]++] = block->index();
        }
    }

    uint32_t begin = 0;
    for (const Block* block : blocks) {
        uint32_t end = endOfPredecessors[block->index()];
        printBlock(out, block, predecessors.span().subspan(begin, end - begin));
        begin = end;
    }

    bool didPrintHeading = false;
    for (unsigned index = 0; index < m_proc.numberOfOperations(); ++index) {
        const Operation* operation = m_proc.operationAt(index);
        if (!operation->hasNoPosition() || !operation->useCount())
            continue;
        if (!std::exchange(didPrintHeading, true))
            out.println("without position:");
        out.print("    ");
        printOperation(out, operation);
        out.println();
    }
}

void MachinePrinter::printOrder(PrintStream& out)
{
    const Order& order = m_proc.order();
    std::span<Block* const> blocks = order.blocksInOrder();

    for (const Block* block : blocks) {
        const BlockPlace& place = order.of(block);
        out.print(*block, ": head = ", MachinePositionDump(place.head));
        if (place.loop != noLoop)
            out.print(", loop ", place.loop);
        if (place.isLoopHead)
            out.print(", loop head");
        if (place.rareRegion != noRareRegion)
            out.print(", rare region ", place.rareRegion);
        if (place.entersRareRegion)
            out.print(", enters it");
        if (place.leavesRareRegion)
            out.print(", leaves it");
        if (!place.predecessors.empty())
            out.print(" <-");
        for (const Block* predecessor : place.predecessors)
            out.print(" ", *predecessor);
        out.println();
    }

    std::span<const Loop> loops = order.loops();
    for (unsigned index = 0; index < loops.size(); ++index) {
        const Loop& loop = loops[index];
        out.print("loop ", index, ":");
        if (loop.firstBlock < loop.endBlock && loop.endBlock <= blocks.size())
            out.print(" ", *blocks[loop.firstBlock], " to ", *blocks[loop.endBlock - 1], ",");
        out.print(" positions ", loop.begin, " to ", loop.end);
        if (loop.parent != noLoop)
            out.print(", in loop ", loop.parent);
        out.println();
    }

    std::span<const RareRegion> regions = order.rareRegions();
    for (unsigned index = 0; index < regions.size(); ++index) {
        out.print("rare region ", index, ": ");
        if (regions[index].onlyEntry)
            out.println("entry ", *regions[index].onlyEntry);
        else
            out.println("several entries");
    }

    out.print("layout:");
    for (const Block* block : order.blocksInLayout())
        out.print(" ", *block);
    out.println();
}

void MachinePrinter::printLiveness(PrintStream& out)
{
    const Order& order = m_proc.order();
    const Liveness& liveness = m_proc.liveness();

    for (unsigned index = 0; index < m_proc.numberOfOperations(); ++index) {
        const Operation* operation = m_proc.operationAt(index);
        const ValueFacts& facts = liveness.of(operation);
        out.print(*operation, ": definition = ", MachinePositionDump(facts.definition), ", lastUse = ", MachinePositionDump(facts.lastUse));
        if (facts.hint)
            out.print(", hint = ", MachineRegisterDump(facts.hint));

        auto printFlag = [&](ValueFlag flag, ASCIILiteral name) {
            if (facts.has(flag))
                out.print(", ", name);
        };
        printFlag(ValueFlag::IsDropped, "IsDropped"_s);
        printFlag(ValueFlag::IsFused, "IsFused"_s);
        printFlag(ValueFlag::IsSunk, "IsSunk"_s);
        printFlag(ValueFlag::LivesAcrossCall, "LivesAcrossCall"_s);
        printFlag(ValueFlag::LivesAcrossSafepoint, "LivesAcrossSafepoint"_s);
        printFlag(ValueFlag::LivesAcrossSuspension, "LivesAcrossSuspension"_s);
        printFlag(ValueFlag::LivesIntoLandingPad, "LivesIntoLandingPad"_s);
        printFlag(ValueFlag::IsRematerialized, "IsRematerialized"_s);
        printFlag(ValueFlag::NeedsPointerBesideIt, "NeedsPointerBesideIt"_s);
        printFlag(ValueFlag::TakesOverTrap, "TakesOverTrap"_s);
        if (facts.has(ValueFlag::TakesOverTrap))
            out.print(" of ", pointerDump(liveness.trapTakenOverBy(operation)));
        printFlag(ValueFlag::WorksInMemory, "WorksInMemory"_s);
        printFlag(ValueFlag::AddsToWholeWord, "AddsToWholeWord"_s);
        printFlag(ValueFlag::IsKeptForCollector, "IsKeptForCollector"_s);
        printFlag(ValueFlag::UsesOverflowFlag, "UsesOverflowFlag"_s);
        printFlag(ValueFlag::HintIsOfRareRegion, "HintIsOfRareRegion"_s);
        if (operation->type() == Int64 && facts.lastUse != noPosition && !facts.has(ValueFlag::UpperHalfIsRead))
            out.print(", UpperHalfIsNotRead");
        out.println();
    }

    std::span<const Loop> loops = order.loops();
    for (unsigned index = 0; index < loops.size(); ++index) {
        const LoopFacts& facts = liveness.of(loops[index], order);
        out.print("loop ", index, ": pressure GP = ", facts.pressure[static_cast<unsigned>(Bank::GP)], ", FP = ", facts.pressure[static_cast<unsigned>(Bank::FP)]);
        if (facts.registersTakenByPatches)
            out.print(", registersTakenByPatches = ", MachineRegisterListDump(facts.registersTakenByPatches), ", pressureAcrossPatches GP = ", facts.pressureAcrossPatches[static_cast<unsigned>(Bank::GP)], ", FP = ", facts.pressureAcrossPatches[static_cast<unsigned>(Bank::FP)]);
        if (!facts.valuesReadFromOutside.empty()) {
            out.print(", valuesReadFromOutside =");
            for (const Operation* value : facts.valuesReadFromOutside)
                out.print(" @", value->index());
        }
        out.println();
    }
}

void MachinePrinter::printWithoutPosition(PrintStream& out, const Operation* operation)
{
    Opcode opcode = operation->opcode();
    CommaPrinter comma;
    if (operation->isConstant() || opcode == SlotBase || opcode == ArgumentReg) {
        printPayload(out, comma, operation);
        return;
    }
    if (namesSymbol(opcode)) {
        out.print(opcode, "(");
        printPayload(out, comma, operation);
        out.print(")");
        return;
    }
    if (opcode == FramePointer) {
        out.print(MachineRegisterDump(MacroAssembler::framePointerRegister));
        return;
    }
    if (opcode == Add && operation->hasNoPosition()) {
        printWithoutPosition(out, operation->child(0));
        out.print(" + ");
        printWithoutPosition(out, operation->child(1));
        return;
    }
    out.print(*operation);
}

void MachinePrinter::printPlace(PrintStream& out, Location location, const Operation* value)
{
    switch (location.kind()) {
    case Location::None:
        out.print(*value);
        return;
    case Location::Immediate:
        printWithoutPosition(out, value);
        return;
    case Location::Register:
    case Location::Spill:
    case Location::SlotBytes:
    case Location::Outgoing:
    case Location::InstanceWord:
        out.print(location);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MachinePrinter::printOperand(PrintStream& out, Leaves& leaves, const Operation* user, unsigned childIndex)
{
    const Operation* child = user->child(childIndex);
    if (!m_proc.liveness().isFused(child)) {
        printPlace(out, leaves.take(), child);
        return;
    }

    out.print(child->opcode());
    if (isMemoryAccess(child->opcode())) {
        CommaPrinter comma(", "_s, " "_s);
        printOperands(out, leaves, child, comma);
        return;
    }
    out.print("(");
    CommaPrinter comma;
    printOperands(out, leaves, child, comma);
    out.print(")");
}

void MachinePrinter::printOperands(PrintStream& out, Leaves& leaves, const Operation* operation, CommaPrinter& comma)
{
    unsigned numberOfChildren = operation->numChildren();
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        out.print(comma);
        if (!isMemoryAccess(operation->opcode()) || index + 1 < numberOfChildren) {
            printOperand(out, leaves, operation, index);
            continue;
        }
        out.print("[");
        printOperand(out, leaves, operation, index);
        out.print(MachineOffsetDump(operation->offset()), "]");
    }
}

void MachinePrinter::printExecute(PrintStream& out, const Step& step)
{
    const Operation* operation = step.operation;
    const PatchOperation* patch = operation->as<PatchOperation>();
    const PatchAssignment* assignment = step.patchAssignment == UINT32_MAX ? nullptr : &m_proc.allocation().patchAssignment(step.patchAssignment);

    std::span<const Location> results;
    std::span<const Location> operands;
    if (patch && assignment) {
        size_t numberOfResults = std::min<size_t>(m_proc.resultCount(patch->type()), assignment->locations.size());
        results = assignment->locations.first(numberOfResults);
        operands = assignment->locations.subspan(numberOfResults);
    }

    CommaPrinter resultComma;
    for (Location result : results)
        out.print(resultComma, result);
    if (results.empty() && step.destination)
        out.print(resultComma, step.destination);
    if (resultComma.didPrint())
        out.print(" = ");
    out.print(operation->opcode());

    Leaves leaves(step);
    CommaPrinter comma(", "_s, " "_s);
    if (patch) {
        unsigned firstOperand = patch->opcode() == Check;
        if (firstOperand) {
            out.print(comma);
            printOperand(out, leaves, patch, 0);
        }
        std::span<Operation* const> children = patch->children();
        for (unsigned index = firstOperand; index < children.size(); ++index) {
            unsigned place = index - firstOperand;
            out.print(comma);
            printPlace(out, place < operands.size() ? operands[place] : Location(), children[index]);
        }
    } else if (m_proc.liveness().worksInMemory(operation)) {
        MemoryUpdate update = memoryUpdateOf(m_proc.liveness(), operation);
        out.print(comma, update.kind == MemoryUpdate::ComparedAhead ? update.relation : update.opcode, " [");
        printPlace(out, leaves.take(), update.load->lastChild());
        out.print(MachineOffsetDump(update.load->offset()), "]", comma);
        if (update.kind == MemoryUpdate::ComparedAhead)
            out.print(update.bound);
        else
            printPlace(out, leaves.take(), update.operand());
    } else if (m_proc.liveness().usesOverflowFlag(operation)) {
        OverflowCheckedSum test = overflowCheckedSumOf(m_proc.liveness(), operation);
        if (operation->opcode() != Branch) {
            out.print(comma);
            printPlace(out, leaves.take(), test.left());
            out.print(comma);
            printPlace(out, leaves.take(), test.right());
            out.print(comma, "32 bits, sets flags");
        } else {
            out.print(comma, "no overflow");
            if (test.branchReadsSum()) {
                out.print(comma);
                printPlace(out, leaves.take(), test.sum);
            }
            if (test.leastResult)
                out.print(comma, "at least ", *test.leastResult);
            if (test.isWidenedOnFailure)
                out.print(comma, "widens");
            if (step.other)
                out.print(comma, "takes back ", pointerDump(step.other));
            if (test.passingSuccessor)
                out.print(comma, "passes to the second");
        }
    } else if (!operation->isUnconditionalJump())
        printOperands(out, leaves, operation, comma);
    if (operation->opcode() == ArgumentReg || operation->opcode() == Extract)
        printPayload(out, comma, operation);

    if (assignment) {
        if (RegisterMask scratch = assignment->gpScratch | assignment->fpScratch)
            out.print(comma, "scratch ", MachineRegisterListDump(scratch));
        if (assignment->usedRegisters)
            out.print(comma, "used ", MachineRegisterListDump(assignment->usedRegisters));
    }

    if (const Block* block = blockEndedBy(operation))
        printSuccessors(out, block);
}

void MachinePrinter::printTextOfStep(PrintStream& out, const Step& step)
{
    switch (step.kind) {
    case StepKind::Execute:
        printExecute(out, step);
        return;
    case StepKind::Move:
    case StepKind::Reload:
    case StepKind::Save:
    case StepKind::KeepPointer:
        out.print(step.kind, " ", step.sources[0], " -> ", step.destination);
        break;
    case StepKind::Materialize:
        out.print(step.kind, " ");
        printWithoutPosition(out, step.operation);
        out.print(" -> ", step.destination);
        break;
    case StepKind::Swap:
        out.print(step.kind, " ", step.sources[0], " <-> ", step.destination);
        break;
    }
    if (step.width != Width64)
        out.print(", ", bytesForWidth(step.width) * 8, " bits");
}

void MachinePrinter::printStep(PrintStream& out, const Step& step)
{
    MachinePrinterLine line(out);
    printTextOfStep(line, step);
    unsigned padding = line.length() < columnOfValue ? columnOfValue - line.length() : 1;
    out.printf("%*s", static_cast<int>(padding), "");
    out.print(pointerDump(step.operation));
    if (step.kind == StepKind::Swap)
        out.print(" ", pointerDump(step.other));
}

void MachinePrinter::printStepLine(PrintStream& out, const Step& step)
{
    out.print("    ");
    printStep(out, step);
    out.println();
}

void MachinePrinter::printStoreToSpill(PrintStream& out, Operation* value)
{
    const Allocation& allocation = m_proc.allocation();
    Reg reg = allocation.registerAtDefinitionOf(value);
    if (!allocation.isStoredBehindDefinition(value))
        return;

    SpillIndex spill = allocation.spillOf(value);
    Step store { StepKind::Save };
    store.width = allocation.spills()[spill].width;
    store.destination = Location::spill(spill);
    store.sources[0] = Location::reg(reg);
    store.operation = value;
    printStepLine(out, store);
}

void MachinePrinter::printAllocation(PrintStream& out)
{
    const Order& order = m_proc.order();
    const Allocation& allocation = m_proc.allocation();
    std::span<Block* const> blocks = order.blocksInOrder();
    std::span<const Step> steps = allocation.steps();

    auto printSteps = [&](uint32_t begin, uint32_t end) {
        for (uint32_t index = begin; index < end && index < steps.size(); ++index) {
            printStepLine(out, steps[index]);
            if (steps[index].kind == StepKind::Execute)
                printStoreToSpill(out, steps[index].operation);
        }
    };

    for (const Block* block : blocks) {
        out.print(*block, ":");
        CommaPrinter comma(", "_s, " "_s);
        for (const RegisterBinding& binding : allocation.registersAtHeadOf(block))
            out.print(comma, MachineRegisterDump(binding.reg), " = ", pointerDump(binding.value));
        out.println();

        if (allocation.isHeadOfFramedPart(block))
            printSteps(allocation.headOfFramedPartAbove(block).beginOfSteps, allocation.headOfFramedPartAbove(block).endOfSteps);
        for (Operation* phi : block->phis())
            printStoreToSpill(out, phi);
        for (const Block* predecessor : order.of(block).predecessors) {
            if (predecessor->isTerminated() && predecessor->last()->opcode() == Patch && predecessor->successor(0).block() == block)
                printStoreToSpill(out, predecessor->last());
        }

        const BlockSteps& range = allocation.stepsOf(block);
        bool allocationStoppedHere = range.terminator == UINT32_MAX && (range.begin || block == blocks.front());
        printSteps(range.begin, allocationStoppedHere ? static_cast<uint32_t>(steps.size()) : range.end);
        printSteps(range.beginOfEdgeMoves, range.endOfEdgeMoves);
        if (range.terminator < steps.size())
            printStepLine(out, steps[range.terminator]);
    }

    std::span<const SpillFacts> spills = allocation.spills();
    for (unsigned index = 0; index < spills.size(); ++index) {
        const SpillFacts& spill = spills[index];
        out.print("s", index, ": ", pointerDump(spill.value), ", positions ", spill.begin, " to ", spill.end, ", ", bytesForWidth(spill.width) * 8, " bits");
        if (spill.hasGap())
            out.print(", gap ", spill.beginOfGap, " to ", spill.endOfGap);
        if (spill.livesAcrossSuspension)
            out.print(", livesAcrossSuspension");
        if (spill.isPointerBesideNarrowReference)
            out.print(", isPointerBesideNarrowReference");
        if (spill.isSaveInRareRegion)
            out.print(", isSaveInRareRegion");
        if (spill.isStoredAtHeadOfFramedPart)
            out.print(", isStoredAtHeadOfFramedPart");
        if (spill.isSharedByPhis)
            out.print(", isSharedByPhis");
        if (spill.isDeadSlot)
            out.print(", isDeadSlot");
        out.println();
    }

    if (allocation.usedCalleeSaves())
        out.println("usedCalleeSaves = ", MachineRegisterListDump(allocation.usedCalleeSaves()));
    if (allocation.needsFrameOutsideRareRegions())
        out.println("needsFrameOutsideRareRegions");
    for (unsigned region = 0; region < order.rareRegions().size(); ++region) {
        if (allocation.needsFrameIn(region))
            out.println("needsFrameIn rare region ", region);
    }
    for (const Allocation::HeadOfFramedPart& head : allocation.headsOfFramedPart())
        out.println("headOfFramedPart = ", *head.block);
    if (allocation.needsFrameAheadOfFramedPart())
        out.println("needsFrameAheadOfFramedPart");
}

void MachinePrinter::printFrame(PrintStream& out)
{
    const Frame& frame = m_proc.frame();
    out.print(frame);

    if (frame.coverage() == FrameCoverage::FramedPart) {
        std::span<Block* const> blocks = m_proc.order().blocksInOrder();
        for (const Block* block : blocks) {
            if (frame.isSetUpAtHeadOf(block))
                out.println("set up at the head of ", *block);
        }
        for (const Block* block : blocks) {
            if (frame.isTornDownAtEndOf(block))
                out.println("torn down at the end of ", *block);
        }
        for (const Block* block : blocks) {
            if (frame.returnsInPlaceOfJumpAtEndOf(block))
                out.println("return in place of the jump of ", *block);
        }
    }

    if (frame.hasFrame()) {
        for (const StackSlot* slot : m_proc.stackSlots()) {
            out.print("slot", slot->index(), " at ", slot->offsetFromFP(), ", ", slot->byteSize(), " bytes");
            if (slot->isDeadAtSuspensions())
                out.print(", dead at suspensions");
            out.println();
        }
    }

    const Allocation& allocation = m_proc.allocation();
    for (uint32_t index = 0; index < allocation.suspensions().size(); ++index) {
        const PatchOperation* suspension = allocation.suspensions()[index];
        const std::optional<BitSpan>& deadWords = suspension->frameWordsDeadAcrossSuspension();
        out.print("suspension ", *suspension, ": dead words ");
        if (!deadWords) {
            out.println("unknown");
            continue;
        }
        out.print("[");
        CommaPrinter comma;
        for (size_t word = 0; word < deadWords->size(); ++word) {
            if (deadWords->get(word))
                out.print(comma, word);
        }
        out.print("]");
        CommaPrinter blankOfSpillsHeld(" "_s, ", holds "_s);
        for (SpillIndex spill = 0; spill < allocation.spills().size(); ++spill) {
            if (allocation.liveSpillsAt(index).get(spill))
                out.print(blankOfSpillsHeld, "s", spill);
        }
        out.println();
    }
    if (!allocation.suspensions().empty())
        out.println("bytesKeptAtSuspensions = ", m_proc.slots().bytesKeptAtSuspensions());
}

void printProcedure(PrintStream& out, const Procedure& procedure)
{
    MachinePrinter(procedure).printProcedure(out);
}

void printOperation(PrintStream& out, const Procedure& procedure, const Operation* operation)
{
    MachinePrinter(procedure).printOperation(out, operation);
}

void printFailure(PrintStream& out, const Procedure& procedure)
{
    const Failure& failure = *procedure.failure();
    out.print(failure.kind, ", by the check in ", failure.check.function, " at ", failure.check.file, ":", failure.check.line);
    if (failure.operation < procedure.numberOfOperations()) {
        out.print(": ");
        printOperation(out, procedure, procedure.operationAt(failure.operation));
    }
}

void printOrder(PrintStream& out, const Procedure& procedure)
{
    MachinePrinter(procedure).printOrder(out);
}

void printLiveness(PrintStream& out, const Procedure& procedure)
{
    MachinePrinter(procedure).printLiveness(out);
}

void printAllocation(PrintStream& out, const Procedure& procedure)
{
    MachinePrinter(procedure).printAllocation(out);
}

void printFrame(PrintStream& out, const Procedure& procedure)
{
    MachinePrinter(procedure).printFrame(out);
}

void printStep(PrintStream& out, const Procedure& procedure, const Step& step)
{
    MachinePrinter(procedure).printStep(out, step);
}

void ValueRep::dump(PrintStream& out) const
{
    switch (m_kind) {
    case WarmAny:
        out.print("WarmAny");
        return;
    case ColdAny:
        out.print("ColdAny");
        return;
    case LateColdAny:
        out.print("LateColdAny");
        return;
    case SomeRegister:
        out.print("SomeRegister");
        return;
    case SomeEarlyRegister:
        out.print("SomeEarlyRegister");
        return;
    case SomeLateRegister:
        out.print("SomeLateRegister");
        return;
    case Register:
        out.print("Register(", MachineRegisterDump(m_reg), ")");
        return;
    case StackArgument:
        out.print("StackArgument(", offsetFromSP(), ")");
        return;
    case Stack:
        out.print("Stack(", offsetFromFP(), ")");
        return;
    case Constant:
        out.print("Constant(", value(), ")");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Operation::dump(PrintStream& out) const
{
    out.print("@", m_index);
}

void Block::dump(PrintStream& out) const
{
    out.print("#", m_index);
}

void Procedure::dump(PrintStream& out) const
{
    printProcedure(out, *this);
    if (m_order) {
        out.println("order:");
        printOrder(out, *this);
    }
    if (m_liveness) {
        out.println("liveness:");
        printLiveness(out, *this);
    }
    if (m_allocation) {
        out.println("allocation:");
        printAllocation(out, *this);
    }
    if (m_frame) {
        out.println("frame:");
        printFrame(out, *this);
    }
    if (m_failure) {
        out.print("failure: ", m_failure->kind);
        if (m_failure->operation != UINT32_MAX)
            out.print(" at @", m_failure->operation);
        if (m_failure->origin)
            out.print(", origin = ", m_failure->origin.bits());
        out.println();
    }
}

void Location::dump(PrintStream& out) const
{
    switch (kind()) {
    case None:
        out.print("none");
        return;
    case Register:
        out.print(MachineRegisterDump(reg()));
        return;
    case Spill:
        out.print("s", spillIndex());
        return;
    case SlotBytes:
        out.print("slot", indexOfStackSlot());
        return;
    case Outgoing:
        out.print("[", MachineRegisterDump(MacroAssembler::stackPointerRegister), MachineOffsetDump(offsetFromSP()), "]");
        return;
    case InstanceWord:
        out.print("[", MachineRegisterDump(instanceGPR), MachineOffsetDump(offsetInInstance()), "]");
        return;
    case Immediate:
        out.print("immediate");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Frame::dump(PrintStream& out) const
{
    out.println("coverage = ", m_coverage);
    out.println("size = ", m_bytesBelowFrameRecord);
    if (!m_hasFrameRecord)
        out.println("no frame record");
    if (m_savesLieAtBottom)
        out.println("callee saves lie at the bottom");
    if (m_bytesDeadAtSuspensions)
        out.println("bytesDeadAtSuspensions = ", m_bytesDeadAtSuspensions);
    if (m_calleeSaveOffsets.registerCount()) {
        out.print("calleeSaves = [");
        CommaPrinter comma;
        for (const RegisterAtOffset& entry : m_calleeSaveOffsets)
            out.print(comma, MachineRegisterDump(entry.reg()), " at ", entry.offset());
        out.println("]");
    }
    if (m_calleeSavesClobberedAtSuspensions)
        out.println("calleeSavesClobberedAtSuspensions = ", MachineRegisterListDump(m_calleeSavesClobberedAtSuspensions));
    if (m_beginOfWordsClearedAtSetup != m_endOfWordsClearedAtSetup)
        out.println("wordsClearedAtSetup = [", m_beginOfWordsClearedAtSetup, ", ", m_endOfWordsClearedAtSetup, ")");
    for (size_t spill = 0; spill < m_offsetOfSpill.size(); ++spill)
        out.println("s", spill, " at ", offsetFromFPOf(static_cast<SpillIndex>(spill)));
}

} } } // namespace JSC::AOT::Machine

namespace WTF {

void printInternal(PrintStream& out, JSC::AOT::Machine::Opcode opcode)
{
    out.print(JSC::AOT::Machine::descriptionOf(opcode).name);
}

void printInternal(PrintStream& out, JSC::AOT::Machine::Type type)
{
    out.print(JSC::AOT::Machine::MachinePrinter::nameOf(type.kind()));
    if (type.isTuple())
        out.print(type.tupleIndex());
}

void printInternal(PrintStream& out, JSC::AOT::Machine::EdgeClass edgeClass)
{
    out.print(JSC::AOT::Machine::MachinePrinter::nameOf(edgeClass));
}

void printInternal(PrintStream& out, JSC::AOT::Machine::FailureKind kind)
{
    out.print(JSC::AOT::Machine::MachinePrinter::nameOf(kind));
}

void printInternal(PrintStream& out, JSC::AOT::Machine::StepKind kind)
{
    out.print(JSC::AOT::Machine::MachinePrinter::nameOf(kind));
}

void printInternal(PrintStream& out, JSC::AOT::Machine::FrameCoverage coverage)
{
    out.print(JSC::AOT::Machine::MachinePrinter::nameOf(coverage));
}

} // namespace WTF

#endif // ENABLE(AOT)
