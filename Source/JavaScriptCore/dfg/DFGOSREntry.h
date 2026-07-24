/*
 * Copyright (C) 2011-2018 Apple Inc. All rights reserved.
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

#include "CodeLocation.h"
#include "DFGAbstractValue.h"
#include "DFGFlushFormat.h"
#include "MacroAssemblerCodeRef.h"
#include "Operands.h"
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

class CallFrame;
class CodeBlock;

namespace DFG {

#if ENABLE(DFG_JIT)
struct OSREntryReshuffling {
    OSREntryReshuffling() { }
    
    OSREntryReshuffling(int fromOffset, int toOffset)
        : fromOffset(fromOffset)
        , toOffset(toOffset)
    {
    }
    
    int fromOffset;
    int toOffset;
};

// Sparse storage for the per-OSR-entry expected value table. The dense
// Operands<AbstractValue> captured at each entrypoint is dominated by
// bytecode-top entries (dead operands are forced to bytecode-top in
// JITCompiler::noticeOSREntry, and tmps stay at full-top from block
// construction and are never consulted by prepareOSREntry). Storing only the
// constrained operands and answering bytecode-top for everything else keeps
// the observable behaviour of prepareOSREntry unchanged: validateOSREntryValue
// early-outs on bytecode-top, isType(SpecInt32Only) is false for it, and
// AbstractValue::validateReferences is a no-op for it.
class OSREntryExpectedValues {
public:
    OSREntryExpectedValues() = default;

    template<typename U>
    explicit OSREntryExpectedValues(const Operands<AbstractValue, U>& source)
        : m_numberOfArguments(source.numberOfArguments())
        , m_numberOfLocals(source.numberOfLocals())
    {
        // prepareOSREntry never looks at tmps, so drop them here.
        unsigned end = m_numberOfArguments + m_numberOfLocals;
        unsigned count = 0;
        for (unsigned i = 0; i < end; ++i) {
            if (!source[i].isBytecodeTop())
                ++count;
        }
        Vector<Entry, 8> entries;
        entries.reserveInitialCapacity(count);
        for (unsigned i = 0; i < end; ++i) {
            if (!source[i].isBytecodeTop())
                entries.append(Entry { i, source[i] });
        }
        m_constrained = WTF::move(entries);
    }

    unsigned numberOfArguments() const { return m_numberOfArguments; }
    unsigned numberOfLocals() const { return m_numberOfLocals; }

    const AbstractValue& argument(unsigned argument) const
    {
        ASSERT(argument < m_numberOfArguments);
        return forIndex(argument);
    }

    const AbstractValue& local(unsigned local) const
    {
        ASSERT(local < m_numberOfLocals);
        return forIndex(m_numberOfArguments + local);
    }

    const AbstractValue& operand(VirtualRegister reg) const
    {
        if (reg.isArgument())
            return argument(reg.toArgument());
        return local(reg.toLocal());
    }

    template<typename Functor>
    void forEachValue(const Functor& functor)
    {
        for (auto& entry : m_constrained)
            functor(entry.value);
    }

    size_t byteSize() const { return m_constrained.byteSize(); }

private:
    struct Entry {
        unsigned index;
        AbstractValue value;
    };

    const AbstractValue& forIndex(unsigned index) const
    {
        if (const Entry* entry = tryBinarySearch<const Entry, unsigned>(m_constrained, m_constrained.size(), index, [](const Entry* entry) { return entry->index; }))
            return entry->value;
        return sharedBytecodeTop();
    }

    static const AbstractValue& sharedBytecodeTop()
    {
        static LazyNeverDestroyed<AbstractValue> value;
        static std::once_flag once;
        std::call_once(once, [] {
            value.construct();
            value.get().makeBytecodeTop();
        });
        return value.get();
    }

    unsigned m_numberOfArguments { 0 };
    unsigned m_numberOfLocals { 0 };
    FixedVector<Entry> m_constrained;
};

struct OSREntryData {
    BytecodeIndex m_bytecodeIndex;
    CodeLocationLabel<OSREntryPtrTag> m_machineCode;
    OSREntryExpectedValues m_expectedValues;
    // Use bitvectors here because they tend to only require one word.
    BitVector m_localsForcedDouble;
    BitVector m_localsForcedAnyInt;
    FixedVector<OSREntryReshuffling> m_reshufflings;
    BitVector m_machineStackUsed;
    
    void dumpInContext(PrintStream&, DumpContext*) const;
    void dump(PrintStream&) const;
};

inline BytecodeIndex getOSREntryDataBytecodeIndex(OSREntryData* osrEntryData)
{
    return osrEntryData->m_bytecodeIndex;
}

struct CatchEntrypointData {
    // We use this when doing OSR entry at catch. We prove the arguments
    // are of the expected type before entering at a catch block.
    CodePtr<ExceptionHandlerPtrTag> machineCode;
    FixedVector<FlushFormat> argumentFormats;
    BytecodeIndex bytecodeIndex;
};

// Returns a pointer to a data buffer that the OSR entry thunk will recognize and
// parse. If this returns null, it means 
void* prepareOSREntry(VM&, CallFrame*, CodeBlock*, BytecodeIndex);

// If null is returned, we can't OSR enter. If it's not null, it's the PC to jump to.
CodePtr<ExceptionHandlerPtrTag> prepareCatchOSREntry(VM&, CallFrame*, CodeBlock* baselineCodeBlock, CodeBlock* optimizedCodeBlock, BytecodeIndex);
#else
inline CodePtr<ExceptionHandlerPtrTag> prepareOSREntry(VM&, CallFrame*, CodeBlock*, BytecodeIndex) { return nullptr; }
#endif

} } // namespace JSC::DFG
