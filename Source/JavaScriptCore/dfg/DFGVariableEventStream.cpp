/*
 * Copyright (C) 2012-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DFGVariableEventStream.h"

#if ENABLE(DFG_JIT)

#include "CodeBlock.h"
#include "DFGValueSource.h"
#include "InlineCallFrame.h"
#include "JSCJSValueInlines.h"
#include "OperandsInlines.h"
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/LEBDecoder.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace DFG {

void VariableEventStreamBuilder::logEvent(const VariableEvent& event)
{
    dataLog("seq#", static_cast<unsigned>(m_stream.size()), ":");
    event.dump(WTF::dataFile());
    dataLogLn(" ");
}

static_assert(static_cast<unsigned>(InvalidEventKind) < (1u << VariableEventStream::operandKindShift));
static_assert(static_cast<unsigned>(lastOperandKind) < (1u << (CHAR_BIT - VariableEventStream::operandKindShift)));
static_assert(sizeof(MacroAssembler::RegisterID) == 1);
static_assert(sizeof(MacroAssembler::FPRegisterID) == 1);

static ALWAYS_INLINE void encodeUnsigned(Vector<uint8_t>& out, uint32_t value)
{
    while (value >= 0x80) {
        out.append(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.append(static_cast<uint8_t>(value));
}

static ALWAYS_INLINE void encodeSigned(Vector<uint8_t>& out, int32_t value)
{
    for (;;) {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        bool signBit = byte & 0x40;
        if ((!value && !signBit) || (value == -1 && signBit)) {
            out.append(byte);
            return;
        }
        out.append(byte | 0x80);
    }
}

void VariableEventStream::encodeEvent(Vector<uint8_t>& out, const VariableEvent& event)
{
    VariableEventKind kind = event.kind();
    switch (kind) {
    case Reset:
        out.append(static_cast<uint8_t>(kind));
        return;
    case Birth:
    case Death:
        out.append(static_cast<uint8_t>(kind));
        encodeUnsigned(out, event.id().bits());
        return;
    case BirthToFill:
    case Fill: {
        out.append(static_cast<uint8_t>(kind));
        encodeUnsigned(out, event.id().bits());
        DataFormat format = event.dataFormat();
        out.append(static_cast<uint8_t>(format));
        if (format == DataFormatDouble)
            out.append(static_cast<uint8_t>(event.fpr()));
#if USE(JSVALUE32_64)
        else if (format & DataFormatJS) {
            out.append(static_cast<uint8_t>(event.tagGPR()));
            out.append(static_cast<uint8_t>(event.payloadGPR()));
        }
#endif
        else
            out.append(static_cast<uint8_t>(event.gpr()));
        return;
    }
    case BirthToSpill:
    case Spill:
        out.append(static_cast<uint8_t>(kind));
        encodeUnsigned(out, event.id().bits());
        out.append(static_cast<uint8_t>(event.dataFormat()));
        encodeSigned(out, event.spillRegister().offset());
        return;
    case MovHintEvent: {
        Operand operand = event.operand();
        out.append(static_cast<uint8_t>(kind) | (static_cast<uint8_t>(operand.kind()) << operandKindShift));
        encodeUnsigned(out, event.id().bits());
        encodeSigned(out, operand.value());
        return;
    }
    case SetLocalEvent: {
        Operand operand = event.operand();
        out.append(static_cast<uint8_t>(kind) | (static_cast<uint8_t>(operand.kind()) << operandKindShift));
        encodeSigned(out, event.machineRegister().offset());
        out.append(static_cast<uint8_t>(event.dataFormat()));
        encodeSigned(out, operand.value());
        return;
    }
    case InvalidEventKind:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

VariableEvent VariableEventStream::decodeEvent(size_t& offset) const
{
    std::span<const uint8_t> bytes = m_bytes.span();

    auto readByte = [&]() -> uint8_t {
        return bytes[offset++];
    };
    auto readUnsigned = [&]() -> uint32_t {
        uint32_t result = 0;
        bool ok = WTF::LEBDecoder::decodeUInt32(bytes, offset, result);
        RELEASE_ASSERT(ok);
        return result;
    };
    auto readSigned = [&]() -> int32_t {
        int32_t result = 0;
        bool ok = WTF::LEBDecoder::decodeInt32(bytes, offset, result);
        RELEASE_ASSERT(ok);
        return result;
    };

    uint8_t tag = readByte();
    VariableEventKind kind = static_cast<VariableEventKind>(tag & eventKindMask);
    switch (kind) {
    case Reset:
        return VariableEvent::reset();
    case Birth:
        return VariableEvent::birth(MinifiedID::fromBits(readUnsigned()));
    case Death:
        return VariableEvent::death(MinifiedID::fromBits(readUnsigned()));
    case BirthToFill:
    case Fill: {
        MinifiedID id = MinifiedID::fromBits(readUnsigned());
        DataFormat format = static_cast<DataFormat>(readByte());
        if (format == DataFormatDouble)
            return VariableEvent::fillFPR(kind, id, static_cast<MacroAssembler::FPRegisterID>(static_cast<int8_t>(readByte())));
#if USE(JSVALUE32_64)
        if (format & DataFormatJS) {
            auto tagGPR = static_cast<MacroAssembler::RegisterID>(static_cast<int8_t>(readByte()));
            auto payloadGPR = static_cast<MacroAssembler::RegisterID>(static_cast<int8_t>(readByte()));
            return VariableEvent::fillPair(kind, id, tagGPR, payloadGPR);
        }
#endif
        return VariableEvent::fillGPR(kind, id, static_cast<MacroAssembler::RegisterID>(static_cast<int8_t>(readByte())), format);
    }
    case BirthToSpill:
    case Spill: {
        MinifiedID id = MinifiedID::fromBits(readUnsigned());
        DataFormat format = static_cast<DataFormat>(readByte());
        return VariableEvent::spill(kind, id, VirtualRegister(readSigned()), format);
    }
    case MovHintEvent: {
        OperandKind operandKind = static_cast<OperandKind>(tag >> operandKindShift);
        MinifiedID id = MinifiedID::fromBits(readUnsigned());
        return VariableEvent::movHint(id, Operand(operandKind, readSigned()));
    }
    case SetLocalEvent: {
        OperandKind operandKind = static_cast<OperandKind>(tag >> operandKindShift);
        VirtualRegister machineReg(readSigned());
        DataFormat format = static_cast<DataFormat>(readByte());
        return VariableEvent::setLocal(Operand(operandKind, readSigned()), machineReg, format);
    }
    case InvalidEventKind:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return VariableEvent();
}

VariableEventStream::VariableEventStream(Vector<VariableEvent>&& stream)
{
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(stream.size() * 2);
    Vector<Checkpoint> checkpoints;
    for (unsigned i = 0; i < stream.size(); ++i) {
        if (stream[i].kind() == Reset)
            checkpoints.append(Checkpoint { i, static_cast<unsigned>(bytes.size()) });
        encodeEvent(bytes, stream[i]);
    }
    m_bytes = WTF::move(bytes);
    m_checkpoints = WTF::move(checkpoints);
}

namespace {

struct MinifiedGenerationInfo {
    bool filled; // true -> in gpr/fpr, false -> spilled
    bool alive;
    VariableRepresentation u;
    DataFormat format;
    
    MinifiedGenerationInfo()
        : filled(false)
        , alive(false)
        , format(DataFormatNone)
    {
    }
    
    void update(const VariableEvent& event)
    {
        switch (event.kind()) {
        case BirthToFill:
        case Fill:
            filled = true;
            alive = true;
            break;
        case BirthToSpill:
        case Spill:
            filled = false;
            alive = true;
            break;
        case Birth:
            alive = true;
            return;
        case Death:
            format = DataFormatNone;
            alive = false;
            return;
        default:
            return;
        }
        
        u = event.variableRepresentation();
        format = event.dataFormat();
    }
};

} // namespace

static bool tryToSetConstantRecovery(ValueRecovery& recovery, MinifiedNode* node)
{
    if (!node)
        return false;
    
    if (node->hasConstant()) {
        recovery = ValueRecovery::constant(node->constant());
        return true;
    }
    
    if (node->isPhantomDirectArguments()) {
        recovery = ValueRecovery::directArgumentsThatWereNotCreated(node->id());
        return true;
    }
    
    if (node->isPhantomClonedArguments()) {
        recovery = ValueRecovery::clonedArgumentsThatWereNotCreated(node->id());
        return true;
    }
    
    return false;
}

template<VariableEventStream::ReconstructionStyle style>
unsigned VariableEventStream::reconstruct(
    CodeBlock* codeBlock, CodeOrigin codeOrigin, MinifiedGraph& graph,
    unsigned index, Operands<ValueRecovery>& valueRecoveries, Vector<UndefinedOperandSpan>* undefinedOperandSpans) const
{
    constexpr bool verbose = false;
    ASSERT(codeBlock->jitType() == JITType::DFGJIT);
    CodeBlock* baselineCodeBlock = codeBlock->baselineVersion();

    unsigned numVariables;
    unsigned numTmps;
    static constexpr unsigned invalidIndex = std::numeric_limits<unsigned>::max();
    unsigned firstUndefined = invalidIndex;
    bool firstUndefinedIsArgument = false;

    auto flushUndefinedOperandSpan = [&] (unsigned i) {
        if (firstUndefined == invalidIndex)
            return;
        int firstOffset = valueRecoveries.operandForIndex(firstUndefined).virtualRegister().offset();
        int lastOffset = valueRecoveries.operandForIndex(i - 1).virtualRegister().offset();
        int minOffset = std::min(firstOffset, lastOffset);
        undefinedOperandSpans->append({ firstUndefined, minOffset, i - firstUndefined });
        firstUndefined = invalidIndex;
    };
    auto recordUndefinedOperand = [&] (unsigned i) {
        // We want to separate the span of arguments from the span of locals even if they have adjacent operands indexes.
        if (firstUndefined != invalidIndex && firstUndefinedIsArgument != valueRecoveries.operandForIndex(i).isArgument())
            flushUndefinedOperandSpan(i);

        if (firstUndefined == invalidIndex) {
            firstUndefined = i;
            firstUndefinedIsArgument = valueRecoveries.operandForIndex(i).isArgument();
        }
    };

    auto* inlineCallFrame = codeOrigin.inlineCallFrame();
    if (inlineCallFrame) {
        CodeBlock* codeBlock = baselineCodeBlockForInlineCallFrame(inlineCallFrame);
        numVariables = codeBlock->numCalleeLocals() + VirtualRegister(inlineCallFrame->stackOffset).toLocal() + 1;
        numTmps = codeBlock->numTmps() + inlineCallFrame->tmpOffset;
    } else {
        numVariables = baselineCodeBlock->numCalleeLocals();
        numTmps = baselineCodeBlock->numTmps();
    }
    
    // Crazy special case: if we're at index == 0 then this must be an argument check
    // failure, in which case all variables are already set up. The recoveries should
    // reflect this.
    if (!index) {
        // We don't include tmps here because they can't be used yet.
        valueRecoveries = Operands<ValueRecovery>(codeBlock->numParameters(), numVariables, 0);
        for (size_t i = 0; i < valueRecoveries.size(); ++i) {
            valueRecoveries[i] = ValueRecovery::displacedInJSStack(
                valueRecoveries.operandForIndex(i).virtualRegister(), DataFormatJS);
        }
        return numVariables;
    }
    
    // Step 1: Find the last checkpoint at or before index - 1. SpeculativeJIT
    // emits a Reset before any other event for each basic block, so a non-zero
    // index always has one.
    RELEASE_ASSERT(!m_checkpoints.isEmpty());
    const Checkpoint* checkpoint = approximateBinarySearch<const Checkpoint, unsigned>(
        m_checkpoints, m_checkpoints.size(), index - 1,
        [](const Checkpoint* checkpoint) { return checkpoint->eventIndex; });
    if (checkpoint->eventIndex > index - 1 && checkpoint != m_checkpoints.begin())
        --checkpoint;
    RELEASE_ASSERT(checkpoint->eventIndex <= index - 1);

    // Step 2: Create a mock-up of the DFG's state and execute the events.
    Operands<ValueSource> operandSources(codeBlock->numParameters(), numVariables, numTmps);
    for (unsigned i = operandSources.size(); i--;)
        operandSources[i] = ValueSource(SourceIsDead);
    UncheckedKeyHashMap<MinifiedID, MinifiedGenerationInfo> generationInfos;
    size_t byteOffset = checkpoint->byteOffset;
    for (unsigned i = checkpoint->eventIndex; i < index; ++i) {
        VariableEvent event = decodeEvent(byteOffset);
        dataLogLnIf(verbose, "Processing event ", event);
        switch (event.kind()) {
        case Reset:
            // nothing to do.
            break;
        case BirthToFill:
        case BirthToSpill:
        case Birth: {
            MinifiedGenerationInfo info;
            info.update(event);
            generationInfos.add(event.id(), info);
            break;
        }
        case Fill:
        case Spill:
        case Death: {
            UncheckedKeyHashMap<MinifiedID, MinifiedGenerationInfo>::iterator iter = generationInfos.find(event.id());
            ASSERT(iter != generationInfos.end());
            iter->value.update(event);
            break;
        }
        case MovHintEvent:
            if (operandSources.hasOperand(event.operand()))
                operandSources.setOperand(event.operand(), ValueSource(event.id()));
            break;
        case SetLocalEvent:
            if (operandSources.hasOperand(event.operand()))
                operandSources.setOperand(event.operand(), ValueSource::forDataFormat(event.machineRegister(), event.dataFormat()));
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }

    dataLogLnIf(verbose, "Operand sources: ", operandSources);
    
    // Step 3: Compute value recoveries!
    valueRecoveries = Operands<ValueRecovery>(OperandsLike, operandSources);
    for (unsigned i = 0; i < operandSources.size(); ++i) {
        ValueSource& source = operandSources[i];
        if (source.isTriviallyRecoverable()) {
            valueRecoveries[i] = source.valueRecovery();
            if (style == ReconstructionStyle::Separated) {
                if (valueRecoveries[i].isConstant() && valueRecoveries[i].constant() == jsUndefined())
                    recordUndefinedOperand(i);
                else
                    flushUndefinedOperandSpan(i);
            }
            continue;
        }
        
        ASSERT(source.kind() == HaveNode);
        MinifiedNode* node = graph.at(source.id());
        MinifiedGenerationInfo info = generationInfos.get(source.id());
        if (!info.alive) {
            dataLogLnIf(verbose, "Operand ", valueRecoveries.operandForIndex(i), " is dead.");
            if (Options::poisonDeadOSRExitVariables()) [[unlikely]] {
                valueRecoveries[i] = ValueRecovery::constant(JSValue::decode(poisonedDeadOSRExitValue));
                continue;
            }

            valueRecoveries[i] = ValueRecovery::constant(jsUndefined());
            if (style == ReconstructionStyle::Separated)
                recordUndefinedOperand(i);
            continue;
        }

        if (tryToSetConstantRecovery(valueRecoveries[i], node)) {
            dataLogLnIf(verbose, "Operand ", valueRecoveries.operandForIndex(i), " is constant.");
            if (style == ReconstructionStyle::Separated) {
                if (node->hasConstant() && node->constant() == jsUndefined())
                    recordUndefinedOperand(i);
                else
                    flushUndefinedOperandSpan(i);
            }
            continue;
        }
        
        ASSERT(info.format != DataFormatNone);
        if (style == ReconstructionStyle::Separated)
            flushUndefinedOperandSpan(i);

        if (info.filled) {
            if (info.format == DataFormatDouble) {
                valueRecoveries[i] = ValueRecovery::inFPR(info.u.fpr, DataFormatDouble);
                continue;
            }
            valueRecoveries[i] = ValueRecovery::inGPR(info.u.gpr, info.format);
            continue;
        }
        
        valueRecoveries[i] =
            ValueRecovery::displacedInJSStack(info.u.operand.virtualRegister(), info.format);
    }
    if (style == ReconstructionStyle::Separated)
        flushUndefinedOperandSpan(operandSources.size());

    return numVariables;
}

unsigned VariableEventStream::reconstruct(
    CodeBlock* codeBlock, CodeOrigin codeOrigin, MinifiedGraph& graph,
    unsigned index, Operands<ValueRecovery>& valueRecoveries) const
{
    return reconstruct<ReconstructionStyle::Combined>(codeBlock, codeOrigin, graph, index, valueRecoveries, nullptr);
}

unsigned VariableEventStream::reconstruct(
    CodeBlock* codeBlock, CodeOrigin codeOrigin, MinifiedGraph& graph,
    unsigned index, Operands<ValueRecovery>& valueRecoveries, Vector<UndefinedOperandSpan>* undefinedOperandSpans) const
{
    return reconstruct<ReconstructionStyle::Separated>(codeBlock, codeOrigin, graph, index, valueRecoveries, undefinedOperandSpans);
}

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
