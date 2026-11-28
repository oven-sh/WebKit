/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTCode.h"

#include "AOTIRPrinter.h"
#include "AOTTestHarness.h"
#include "AOTTypeArena.h"
#include "AOTTypeText.h"
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct TraitInCodeText {
    ASCIILiteral name;
    bool (EffectTraits::*holds)() const;
};

static constexpr TraitInCodeText traitsInCodeText[] = {
    { "mayThrow"_s, &EffectTraits::mayThrow },
    { "mayThrowByRule"_s, &EffectTraits::mayThrowByRule },
    { "mayCollect"_s, &EffectTraits::mayCollect },
    { "mayCallUnknownCode"_s, &EffectTraits::mayCallUnknownCode },
    { "maySuspend"_s, &EffectTraits::maySuspend },
    { "mayBeTerminated"_s, &EffectTraits::mayBeTerminated },
    { "mayWriteObservableState"_s, &EffectTraits::mayWriteObservableState },
    { "mayTouchMemory"_s, &EffectTraits::mayTouchMemory },
    { "mayWriteOutsideObjects"_s, &EffectTraits::mayWriteOutsideObjects },
};

static ASCIILiteral nameInCodeText(FactSubject::Kind kind)
{
    switch (kind) {
    case FactSubject::Kind::Program:
        return "Program"_s;
    case FactSubject::Kind::Function:
        return "Function"_s;
    case FactSubject::Kind::EntryParameter:
        return "EntryParameter"_s;
    case FactSubject::Kind::Result:
        return "Result"_s;
    case FactSubject::Kind::Thrown:
        return "Thrown"_s;
    case FactSubject::Kind::Variable:
        return "Variable"_s;
    case FactSubject::Kind::Field:
        return "Field"_s;
    case FactSubject::Kind::Layout:
        return "Layout"_s;
    case FactSubject::Kind::Object:
        return "Object"_s;
    case FactSubject::Kind::Cell:
        return "Cell"_s;
    case FactSubject::Kind::Builtin:
        return "Builtin"_s;
    case FactSubject::Kind::Module:
        return "Module"_s;
    case FactSubject::Kind::InputOfOperation:
        return "InputOfOperation"_s;
    case FactSubject::Kind::Operation:
        return "Operation"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInCodeText(SweepMark mark)
{
    switch (mark) {
    case SweepMark::IsNeeded:
        return "IsNeeded"_s;
    case SweepMark::ReleasesOperand:
        return "ReleasesOperand"_s;
    case SweepMark::LaterBarrierCovers:
        return "LaterBarrierCovers"_s;
    case SweepMark::LeavesOwedError:
        return "LeavesOwedError"_s;
    case SweepMark::PollsForNothing:
        return "PollsForNothing"_s;
    case SweepMark::LaterFenceCovers:
        return "LaterFenceCovers"_s;
    case SweepMark::OwnerIsYoung:
        return "OwnerIsYoung"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInCodeText(FateOfPoll fate)
{
    switch (fate) {
    case FateOfPoll::LeavesHandler:
        return "LeavesHandler"_s;
    case FateOfPoll::IsDropped:
        return "IsDropped"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class CodeTextWriter {
    WTF_MAKE_NONCOPYABLE(CodeTextWriter);
public:
    CodeTextWriter(PrintStream&, const CodeTextContext&, const Code&);

    void write();

private:
    static constexpr uint32_t endOfChain = UINT32_MAX;

    void chainFactUsesByRecord();
    void chainEntriesByBlock();
    void collectOperationsOf(BlockID);
    bool isDropped(ValueID id) const { return m_isSwept && !m_code.isNeeded(id); }

    void writeFirstLine();
    void writeInlineFrames();
    void writeConstants();
    void writeBlock(BlockID);
    void writeBlockHead(BlockID, const Block&);
    void writeParameter(ValueID);
    void writeRecord(ValueID);
    void writeTerminator(const Terminator&);
    void writeTarget(ASCIILiteral name, EdgeID);
    void writeType(TypeID);
    void writeTraits(EffectTraits);

    void writeFactUsesOf(ValueID);
    void writeFactUse(ASCIILiteral indentation, const FactUse&);
    void writeFactUsesOfRecordsWithoutLine();
    void writeFatesOfPolls();

    void writeWhatSweepDropped(bool handlerIsDropped, std::span<const ValueID> parameters, std::span<const ValueID> operations);
    void writeSweepMarksOf(ValueID);
    void writeSuspensions();

    PrintStream& m_out;
    const Code& m_code;
    TypeReader m_types;
    IRPrinter m_printer;
    const bool m_isSwept;

    Vector<uint32_t> m_firstFactUseOfRecord;
    Vector<uint32_t> m_nextFactUseOfRecord;
    Vector<uint32_t> m_firstEntryOfBlock;
    Vector<uint32_t> m_nextEntryOfBlock;
    Vector<ValueID> m_operations;
    BitVector m_writtenFactUses;
};

CodeTextWriter::CodeTextWriter(PrintStream& out, const CodeTextContext& context, const Code& code)
    : m_out(out)
    , m_code(code)
    , m_types(context.types)
    , m_printer(out, context.program, TypeReader(context.types))
    , m_isSwept(code.isSwept())
{
}

void CodeTextWriter::write()
{
    chainFactUsesByRecord();
    chainEntriesByBlock();

    writeFirstLine();
    writeInlineFrames();
    writeConstants();
    for (uint32_t index = 0; index < m_code.numberOfBlocks(); ++index)
        writeBlock(BlockID(index));
    writeFatesOfPolls();
    writeFactUsesOfRecordsWithoutLine();
    writeSuspensions();
    m_out.print("}\n"_s);
}

void CodeTextWriter::chainFactUsesByRecord()
{
    std::span<const FactUse> uses = m_code.factUses();
    m_firstFactUseOfRecord.fill(endOfChain, m_code.numberOfValues());
    m_nextFactUseOfRecord.fill(endOfChain, uses.size());
    m_writtenFactUses.ensureSize(uses.size());
    for (uint32_t index = static_cast<uint32_t>(uses.size()); index--;) {
        ValueID record = uses[index].record;
        if (record.index() < m_code.numberOfValues())
            m_nextFactUseOfRecord[index] = std::exchange(m_firstFactUseOfRecord[record.index()], index);
    }
}

void CodeTextWriter::chainEntriesByBlock()
{
    std::span<const EntryOfCode> entries = m_code.entries();
    m_firstEntryOfBlock.fill(endOfChain, m_code.numberOfBlocks());
    m_nextEntryOfBlock.fill(endOfChain, entries.size());
    for (uint32_t index = static_cast<uint32_t>(entries.size()); index--;) {
        BlockID block = entries[index].block;
        if (block.index() < m_code.numberOfBlocks())
            m_nextEntryOfBlock[index] = std::exchange(m_firstEntryOfBlock[block.index()], index);
    }
}

void CodeTextWriter::collectOperationsOf(BlockID block)
{
    m_operations.shrink(0);
    for (ValueID id = m_code.lastOperationOf(block); id; id = m_code.at(id).previous) {
        if (m_code.at(id).opcode != Opcode::Parameter)
            m_operations.append(id);
    }
    m_operations.reverse();
}

void CodeTextWriter::writeFirstLine()
{
    m_printer.beginLine("code"_s);
    m_out.print(" "_s);
    printCodeUnit(m_out, m_code.unit());
    if (uint32_t numberOfFrameWords = m_code.numberOfFrameWords())
        m_printer.field("frameWords"_s, numberOfFrameWords);
    if (m_code.holdsCallThatPlanHasNotCounted())
        m_out.print(" holdsCallThatPlanHasNotCounted=true"_s);
    if (m_code.callsOfFunctionNeverCollect())
        m_out.print(" callsOfFunctionNeverCollect=true"_s);
    m_out.print(" {"_s);
    if (m_isSwept)
        m_out.print(" # swept"_s);
    m_printer.endLine();
}

void CodeTextWriter::writeInlineFrames()
{
    std::span<const InlineFrame> frames = m_code.inlineFrames();
    for (uint32_t index = 0; index < frames.size(); ++index) {
        m_out.print("  "_s);
        m_printer.beginLine("frame"_s);
        m_out.print(" "_s, index);
        printFields(m_printer, frames[index]);
        m_printer.endLine();
    }
}

void CodeTextWriter::writeConstants()
{
    m_operations.shrink(0);
    for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
        if (m_code.isConstant(ValueID(index)))
            m_operations.append(ValueID(index));
    }
    if (m_operations.isEmpty())
        return;

    m_out.print("  constants:"_s);
    writeWhatSweepDropped(false, { }, m_operations.span());
    m_printer.endLine();
    for (ValueID constant : m_operations) {
        if (!isDropped(constant))
            writeRecord(constant);
    }
}

void CodeTextWriter::writeBlock(BlockID id)
{
    if (m_isSwept && !m_code.isReached(id)) {
        m_out.print("  # dropped "_s);
        m_printer.print(id);
        m_printer.endLine();
        return;
    }

    const Block& block = m_code.at(id);
    collectOperationsOf(id);
    writeBlockHead(id, block);
    for (ValueID parameter : block.parameters())
        writeFactUsesOf(parameter);
    std::span<const ValueID> operations = m_isSwept ? m_code.valuesOf(id) : m_operations.span();
    for (ValueID operation : operations)
        writeRecord(operation);
    writeTerminator(block.terminator);
}

void CodeTextWriter::writeBlockHead(BlockID id, const Block& block)
{
    m_out.print("  "_s);
    m_printer.print(id);
    m_out.print("("_s);
    CommaPrinter comma;
    for (ValueID parameter : block.parameters()) {
        m_out.print(comma);
        writeParameter(parameter);
    }
    m_out.print(")"_s);

    if (block.role != BlockRole::Ordinary)
        m_printer.field("role"_s, block.role);
    if (block.subject)
        m_printer.field("subject"_s, block.subject);
    if (block.frequency != BlockFrequency::Normal)
        m_printer.field("frequency"_s, block.frequency);
    if (block.isInLoop)
        m_printer.field("isInLoop"_s, true);
    if (block.isLoopHead)
        m_printer.field("isLoopHead"_s, true);
    if (block.keepsSitesThoughRare)
        m_printer.field("keepsSitesThoughRare"_s, true);
    for (uint32_t entry = m_firstEntryOfBlock[id.index()]; entry != endOfChain; entry = m_nextEntryOfBlock[entry])
        m_printer.field("entry"_s, entry);
    EdgeID handler = m_code.handlerOf(id);
    if (handler) {
        m_printer.beginField("handler"_s);
        writeTarget({ }, handler);
    }
    m_out.print(":"_s);
    writeWhatSweepDropped(block.handler && !handler, block.parameters(), m_operations.span());
    m_printer.endLine();
}

void CodeTextWriter::writeParameter(ValueID id)
{
    const Operation& parameter = m_code.at(id);
    m_printer.print(id);
    m_out.print(": "_s);
    writeType(parameter.type);
    m_out.print(" @"_s);
    m_printer.print(parameter.representation);
    m_printer.printOrigin(parameter.origin());
}

void CodeTextWriter::writeRecord(ValueID id)
{
    const Operation& operation = m_code.at(id);
    m_out.print("    "_s);
    m_printer.print(id);
    if (operation.type) {
        m_out.print(": "_s);
        writeType(operation.type);
    }
    m_out.print(" @"_s);
    m_printer.print(operation.representation);
    m_out.print(" = "_s);
    m_printer.print(operation.opcode);
    printFields(m_printer, operation);
    m_printer.printFlags(flagNamesOf(operation.opcode), operation.flags);
    writeTraits(operation.traits());
    m_printer.printOrigin(operation.origin());
    writeSweepMarksOf(id);
    m_printer.endLine();
    writeFactUsesOf(id);
}

void CodeTextWriter::writeTerminator(const Terminator& terminator)
{
    if (!terminator.isSet)
        return;

    m_out.print("    "_s);
    m_printer.print(terminator.kind);
    printFields(m_printer, terminator);
    m_printer.printFlags(flagNamesOf(terminator.kind), terminator.flags);
    CommaPrinter comma(", "_s, " -> "_s);
    for (uint32_t index = 0; index < terminator.numberOfEdges; ++index) {
        m_out.print(comma);
        const TargetInfo* info = infoOfTarget(terminator.kind, index, terminator.numberOfEdges);
        if (!info)
            m_out.print("<invalid target "_s, index, "> "_s);
        writeTarget(info ? info->name : ASCIILiteral(), terminator.edge(index));
    }
    m_printer.printOrigin(terminator.origin());
    m_printer.endLine();
}

void CodeTextWriter::writeTarget(ASCIILiteral name, EdgeID id)
{
    const Edge& edge = m_code.at(id);
    if (!edge.isOpen()) {
        m_printer.printTarget(name, edge.to, edge.arguments(), { });
        return;
    }
    if (!name.isEmpty())
        m_out.print(name, " "_s);
    m_out.print("-"_s);
}

void CodeTextWriter::writeType(TypeID type)
{
    if (!type) {
        m_out.print("-"_s);
        return;
    }
    printType(m_out, m_types, type);
}

void CodeTextWriter::writeTraits(EffectTraits traits)
{
    if (traits.isPure())
        return;

    m_printer.beginField("traits"_s);
    m_out.print("["_s);
    CommaPrinter comma;
    for (const TraitInCodeText& trait : traitsInCodeText) {
        if ((traits.*trait.holds)())
            m_out.print(comma, trait.name);
    }
    m_out.print("]"_s);
}

void CodeTextWriter::writeFactUsesOf(ValueID record)
{
    std::span<const FactUse> uses = m_code.factUses();
    for (uint32_t index = m_firstFactUseOfRecord[record.index()]; index != endOfChain; index = m_nextFactUseOfRecord[index]) {
        writeFactUse("      "_s, uses[index]);
        m_writtenFactUses.quickSet(index);
    }
}

void CodeTextWriter::writeFactUse(ASCIILiteral indentation, const FactUse& use)
{
    static constexpr FactUse unset { };

    m_out.print(indentation);
    m_printer.beginLine("fact"_s);
    m_out.print(" "_s);
    m_printer.print(use.record);
    m_printer.beginField("kind"_s);
    m_out.print(nameOf(use.kind));
    m_printer.beginField("subjectKind"_s);
    m_out.print(nameInCodeText(use.subjectKind));
    if (use.subject != unset.subject)
        m_printer.field("subject"_s, use.subject);
    if (use.index != unset.index)
        m_printer.field("index"_s, use.index);
    if (use.type) {
        m_printer.beginField("type"_s);
        writeType(use.type);
    }
    if (use.key)
        m_printer.field("key"_s, use.key);
    m_printer.endLine();
}

void CodeTextWriter::writeFactUsesOfRecordsWithoutLine()
{
    std::span<const FactUse> uses = m_code.factUses();
    for (uint32_t index = 0; index < uses.size(); ++index) {
        if (!m_writtenFactUses.quickGet(index))
            writeFactUse("  "_s, uses[index]);
    }
}

void CodeTextWriter::writeFatesOfPolls()
{
    if (m_isSwept)
        return;

    for (PollWithFate listed : m_code.pollsWithFates()) {
        m_out.print("  "_s);
        m_printer.beginLine("poll"_s);
        m_out.print(" "_s);
        m_printer.print(listed.poll);
        m_printer.beginField("fate"_s);
        m_out.print(nameInCodeText(listed.fate));
        if (listed.unlessKept) {
            m_printer.beginField("unlessKept"_s);
            m_printer.print(listed.unlessKept);
        }
        m_printer.endLine();
    }
}

void CodeTextWriter::writeWhatSweepDropped(bool handlerIsDropped, std::span<const ValueID> parameters, std::span<const ValueID> operations)
{
    if (!m_isSwept)
        return;

    CommaPrinter comma(", "_s, " # dropped "_s);
    if (handlerIsDropped)
        m_out.print(comma, "handler"_s);
    for (std::span<const ValueID> records : { parameters, operations }) {
        for (ValueID record : records) {
            if (m_code.isNeeded(record))
                continue;
            m_out.print(comma);
            m_printer.print(record);
        }
    }
}

void CodeTextWriter::writeSweepMarksOf(ValueID id)
{
    if (!m_isSwept)
        return;

    CommaPrinter blank(" "_s, " # "_s);
    if (unsigned useCount = m_code.useCountOf(id))
        m_out.print(blank, "uses="_s, useCount);
    if (ValueID onlyUser = m_code.onlyUserOf(id)) {
        m_out.print(blank, "onlyUser="_s);
        m_printer.print(onlyUser);
    }
    for (SweepMark mark : m_code.marksOf(id) - SweepMark::IsNeeded)
        m_out.print(blank, nameInCodeText(mark));
}

void CodeTextWriter::writeSuspensions()
{
    if (!m_isSwept)
        return;

    for (const LiveAtSuspension& suspension : m_code.suspensions()) {
        m_out.print("  # live at "_s);
        m_printer.print(suspension.suspension);
        m_out.print(" "_s);
        m_printer.list(m_code.liveAt(suspension.suspension), [&](ValueID live) {
            m_printer.print(live);
        });
        m_printer.endLine();
    }
}

void printCode(PrintStream& out, const CodeTextContext& context, const Code& code)
{
    CodeTextWriter(out, context, code).write();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
