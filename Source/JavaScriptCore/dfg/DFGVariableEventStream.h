/*
 * Copyright (C) 2012-2017 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(DFG_JIT)

#include "DFGMinifiedGraph.h"
#include "DFGVariableEvent.h"
#include "Operands.h"
#include "ValueRecovery.h"
#include <wtf/FixedVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace DFG {

struct UndefinedOperandSpan {
    unsigned firstIndex;
    int minOffset;
    unsigned numberOfRegisters;
};

// The encoded event stream is a byte buffer plus a side table that maps each
// Reset checkpoint's event index to its byte offset. SpeculativeJIT keeps
// appending plain VariableEvents while compiling; we compact them here once the
// stream is finalized, because the only reader (reconstruct()) already seeks
// back to the nearest Reset and replays forward, so random access to individual
// events was never required. Stream indices handed to reconstruct() remain
// event counts, unchanged from the builder's size().
//
// Encoding: one tag byte per event holding the VariableEventKind in the low 4
// bits, with the upper bits carrying the bytecode OperandKind for
// MovHintEvent/SetLocalEvent. MinifiedID, Operand::value() and
// VirtualRegister offsets are written as LEB128 varints; DataFormat and
// machine-register IDs (int8_t on every backend) take one byte each.
class VariableEventStream {
public:
    static constexpr unsigned operandKindShift = 4;
    static constexpr uint8_t eventKindMask = (1u << operandKindShift) - 1;

    VariableEventStream() = default;
    VariableEventStream(Vector<VariableEvent>&&);

    unsigned reconstruct(CodeBlock*, CodeOrigin, MinifiedGraph&, unsigned index, Operands<ValueRecovery>&) const;
    unsigned reconstruct(CodeBlock*, CodeOrigin, MinifiedGraph&, unsigned index, Operands<ValueRecovery>&, Vector<UndefinedOperandSpan>*) const;

    size_t sizeInBytes() const { return m_bytes.byteSize() + m_checkpoints.byteSize(); }

private:
    enum class ReconstructionStyle {
        Combined,
        Separated
    };
    template<ReconstructionStyle style>
    unsigned reconstruct(
        CodeBlock*, CodeOrigin, MinifiedGraph&,
        unsigned index, Operands<ValueRecovery>&, Vector<UndefinedOperandSpan>*) const;

    struct Checkpoint {
        unsigned eventIndex;
        unsigned byteOffset;
    };

    static void encodeEvent(Vector<uint8_t>& out, const VariableEvent&);
    VariableEvent decodeEvent(size_t& offset) const;

    FixedVector<uint8_t> m_bytes;
    FixedVector<Checkpoint> m_checkpoints;
};

class VariableEventStreamBuilder {
    WTF_MAKE_NONCOPYABLE(VariableEventStreamBuilder);
public:
    static constexpr bool verbose = false;

    VariableEventStreamBuilder()
    {
        m_stream.reserveInitialCapacity(128);
    }

    void appendAndLog(const VariableEvent& event)
    {
        if (verbose)
            logEvent(event);
        m_stream.append(event);
    }

    unsigned size() const { return m_stream.size(); }

    Vector<VariableEvent> finalize() { return WTF::move(m_stream); }

private:
    void logEvent(const VariableEvent&);

    Vector<VariableEvent> m_stream;
};

} } // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
