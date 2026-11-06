/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Opcode.h"
#include "OpcodeSize.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// An instruction of Python code whose opcode op_py_instrumented has taken the place of is, to all but the interpreter, the
// instruction it was.
JS_EXPORT_PRIVATE OpcodeID originalOpcodeOfInstrumentedInstruction(const void* instruction);

struct JSOpcodeTraits {
    using OpcodeID = ::JSC::OpcodeID;
    static constexpr OpcodeID instrumented = op_py_instrumented;
    static constexpr OpcodeID numberOfBytecodesWithCheckpoints = static_cast<OpcodeID>(NUMBER_OF_BYTECODE_WITH_CHECKPOINTS);
    static constexpr OpcodeID numberOfBytecodesWithMetadata = static_cast<OpcodeID>(NUMBER_OF_BYTECODE_WITH_METADATA);
    static_assert(numberOfBytecodesWithCheckpoints <= numberOfBytecodesWithMetadata);
    static constexpr OpcodeID wide16 = op_wide16;
    static constexpr OpcodeID wide32 = op_wide32;
    static constexpr OpcodeID page2 = op_page2;
    static constexpr OpcodeID firstIDThatIsNoOpcode = op_wide16; // That, wide32, instrumented and page2 are the last ids of page 1, in this order.
    static constexpr unsigned firstIDOfPage2 = firstPage2OpcodeID;
    static constexpr unsigned numberOfIDs = numOpcodeIDs;
    static constexpr const unsigned* opcodeLengths = ::JSC::opcodeLengths;
    static constexpr const ASCIILiteral* opcodeNames = ::JSC::opcodeNames;
    static constexpr auto checkpointCountTable = bytecodeCheckpointCountTable;
    static constexpr OpcodeSize maxOpcodeIDWidth = maxJSOpcodeIDWidth;
};

template<typename Traits>
struct BaseInstruction {
    struct Metadata { };

protected:
    BaseInstruction()
    { }

private:
    template<OpcodeSize Width>
    class Impl {
    public:
        typename Traits::OpcodeID opcodeID() const { return static_cast<typename Traits::OpcodeID>(m_opcode); }

    private:
        typename OpcodeIDWidthBySize<Traits, Width>::opcodeType m_opcode;
    };

public:
    typename Traits::OpcodeID opcodeID() const
    {
        typename Traits::OpcodeID opcodeID = narrow()->opcodeID();
        if (opcodeID < Traits::firstIDThatIsNoOpcode) [[likely]]
            return opcodeID;
        if (opcodeID < Traits::instrumented) {
            // Behind either wide prefix the opcode is the next byte.
            static_assert(OpcodeIDWidthBySize<Traits, OpcodeSize::Wide16>::opcodeIDSize == OpcodeSize::Narrow && OpcodeIDWidthBySize<Traits, OpcodeSize::Wide32>::opcodeIDSize == OpcodeSize::Narrow);
            opcodeID = (this + 1)->narrow()->opcodeID();
            if (opcodeID < Traits::instrumented) [[likely]]
                return opcodeID;
        }
        return opcodeIDWhereThereIs(opcodeID);
    }

    // Where an opcode would stand there is op_page2 or op_py_instrumented. Anything else, or no opcode of page 2 behind op_page2, and this is not
    // the start of an instruction. To ask there is a mistake, but one that code of JS could make without harm when every byte spelled an opcode.
    // So it can now: the answer is an id that has a name and a length in the tables, as the wrong opcode had that it was then.
    NEVER_INLINE typename Traits::OpcodeID opcodeIDWhereThereIs(typename Traits::OpcodeID found) const
    {
        if (found == Traits::instrumented)
            return originalOpcodeOfInstrumentedInstruction(this);
        if (found == Traits::page2) {
            unsigned opcodeID = Traits::firstIDOfPage2 + static_cast<unsigned>((this + (narrow()->opcodeID() == Traits::page2 ? 1 : 2))->narrow()->opcodeID());
            if (opcodeID < Traits::numberOfIDs) [[likely]]
                return static_cast<typename Traits::OpcodeID>(opcodeID);
        }
        ASSERT_NOT_REACHED();
        return Traits::page2;
    }

    const ASCIILiteral name() const
    {
        return Traits::opcodeNames[opcodeID()];
    }

    bool isWide16() const
    {
        return narrow()->opcodeID() == Traits::wide16;
    }

    bool isWide32() const
    {
        return narrow()->opcodeID() == Traits::wide32;
    }

    OpcodeSize width() const
    {
        if (isWide32())
            return OpcodeSize::Wide32;
        return isWide16() ? OpcodeSize::Wide16 : OpcodeSize::Narrow;
    }

    bool hasMetadata() const
    {
        return opcodeID() < Traits::numberOfBytecodesWithMetadata;
    }

    bool hasCheckpoints() const
    {
        return opcodeID() < Traits::numberOfBytecodesWithCheckpoints;
    }

    unsigned numberOfCheckpoints() const
    {
        if (!hasCheckpoints())
            return 1;
        return Traits::checkpointCountTable[opcodeID()];
    }

    int sizeShiftAmount() const
    {
        if (isWide32())
            return 2;
        if (isWide16())
            return 1;
        return 0;
    }

    OpcodeSize opcodeIDWidth() const
    {
        if (isWide32())
            return OpcodeIDWidthBySize<Traits, OpcodeSize::Wide32>::opcodeIDSize;
        if (isWide16())
            return OpcodeIDWidthBySize<Traits, OpcodeSize::Wide16>::opcodeIDSize;
        return OpcodeIDWidthBySize<Traits, OpcodeSize::Narrow>::opcodeIDSize;
    }

    unsigned opcodeIDBytes() const
    {
        return (unsigned) opcodeIDWidth() + (opcodeID() >= Traits::firstIDOfPage2 ? 1 : 0);
    }

    size_t size() const
    {
        typename Traits::OpcodeID opcodeID = narrow()->opcodeID();
        if (opcodeID < Traits::firstIDThatIsNoOpcode) [[likely]]
            return 1 + Traits::opcodeLengths[opcodeID];
        auto sizeShiftAmount = this->sizeShiftAmount();
        size_t prefixSize = sizeShiftAmount ? 1 : 0;
        size_t operandSize = static_cast<size_t>(1) << sizeShiftAmount;
        if (sizeShiftAmount)
            opcodeID = (this + 1)->narrow()->opcodeID();
        if (opcodeID >= Traits::instrumented) [[unlikely]] {
            opcodeID = opcodeIDWhereThereIs(opcodeID);
            if (opcodeID >= Traits::firstIDOfPage2)
                ++prefixSize;
        }
        return 1 + Traits::opcodeLengths[opcodeID] * operandSize + prefixSize;
    }

    template<class T>
    bool is() const
    {
        return opcodeID() == T::opcodeID;
    }

    template<class T>
    T as() const
    {
        ASSERT((is<T>()));
        return T::decode(reinterpret_cast<const uint8_t*>(this));
    }

    template<class T, OpcodeSize width>
    T asKnownWidth() const
    {
        ASSERT((is<T>()));
        return T(reinterpret_cast_ptr<const typename TypeBySize<width>::unsignedType*>(this + (width == OpcodeSize::Narrow ? 1 : 2) + T::prefixBytes));
    }

    template<class T>
    T* cast()
    {
        ASSERT((is<T>()));
        return std::bit_cast<T*>(this);
    }

    template<class T>
    const T* cast() const
    {
        ASSERT((is<T>()));
        return reinterpret_cast<const T*>(this);
    }

    const Impl<OpcodeSize::Narrow>* narrow() const
    {
        return reinterpret_cast<const Impl<OpcodeSize::Narrow>*>(this);
    }

    const Impl<OpcodeSize::Wide16>* wide16() const
    {
        static_assert(sizeof(*this) == 1, "Pointer math");
        ASSERT(isWide16());
        return reinterpret_cast<const Impl<OpcodeSize::Wide16>*>(this + 1);
    }

    const Impl<OpcodeSize::Wide32>* wide32() const
    {
        static_assert(sizeof(*this) == 1, "Pointer math");
        ASSERT(isWide32());
        return reinterpret_cast<const Impl<OpcodeSize::Wide32>*>(this + 1);
    }
};

using JSInstruction = BaseInstruction<JSOpcodeTraits>;
static_assert(sizeof(JSInstruction) == 1, "So pointer math is the same as byte math");

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
