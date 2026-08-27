/*
 * Copyright (C) 2013-2018 Apple Inc. All rights reserved.
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
#include "FTLOSRExit.h"

#if ENABLE(FTL_JIT)

#include "B3StackmapGenerationParams.h"
#include "FTLJITCode.h"
#include "FTLSlowPathCall.h"
#include "FTLState.h"
#include "OperandsInlines.h"
#include <wtf/LEBDecoder.h>

namespace JSC { namespace FTL {

using namespace B3;
using namespace DFG;

static constexpr unsigned exitValueKindBits = 4;
static constexpr uint8_t exitValueKindMask = (1u << exitValueKindBits) - 1;
static constexpr unsigned maxShortDeadRun = (1u << (CHAR_BIT - exitValueKindBits)) - 1;
static_assert(static_cast<unsigned>(ExitValueMaterializeNewObject) <= exitValueKindMask);
static_assert(sizeof(DataFormat) == 1);

static ALWAYS_INLINE void encodeVarint(Vector<uint8_t>& out, uint32_t value)
{
    while (value >= 0x80) {
        out.append(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.append(static_cast<uint8_t>(value));
}

static ALWAYS_INLINE uint32_t decodeVarint(std::span<const uint8_t> bytes, size_t& offset)
{
    uint32_t result = 0;
    bool ok = WTF::LEBDecoder::decodeUInt32(bytes, offset, result);
    RELEASE_ASSERT(ok);
    return result;
}

static ALWAYS_INLINE uint32_t zigZagEncode(int32_t value)
{
    return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
}

static ALWAYS_INLINE int32_t zigZagDecode(uint32_t value)
{
    return static_cast<int32_t>(value >> 1) ^ -static_cast<int32_t>(value & 1);
}

OSRExitDescriptor::OSRExitDescriptor(
    DataFormat profileDataFormat, MethodOfGettingAValueProfile valueProfile,
    unsigned numberOfArguments, unsigned numberOfLocals, unsigned numberOfTmps)
    : m_profileDataFormat(profileDataFormat)
    , m_valueProfile(valueProfile)
    , m_numberOfArguments(numberOfArguments)
    , m_numberOfLocals(numberOfLocals)
    , m_numberOfTmps(numberOfTmps)
{
}

void OSRExitDescriptor::setValues(const Operands<ExitValue>& values)
{
    ASSERT(values.numberOfArguments() == m_numberOfArguments);
    ASSERT(values.numberOfLocals() == m_numberOfLocals);
    ASSERT(values.numberOfTmps() == m_numberOfTmps);

    Vector<uint8_t> bytes;
    Vector<ExitTimeObjectMaterialization*> materializations;

    auto emitDeadRun = [&](unsigned run) {
        ASSERT(run);
        unsigned shortRun = std::min(run, maxShortDeadRun);
        bytes.append(static_cast<uint8_t>(ExitValueDead) | static_cast<uint8_t>(shortRun << exitValueKindBits));
        if (shortRun == maxShortDeadRun)
            encodeVarint(bytes, run - maxShortDeadRun);
    };

    unsigned deadRun = 0;
    for (unsigned i = 0; i < values.size(); ++i) {
        const ExitValue& value = values[i];
        if (value.kind() == ExitValueDead) {
            ++deadRun;
            continue;
        }
        if (deadRun) {
            emitDeadRun(deadRun);
            deadRun = 0;
        }

        switch (value.kind()) {
        case ExitValueInJSStack:
        case ExitValueInJSStackAsInt32:
        case ExitValueInJSStackAsInt52:
        case ExitValueInJSStackAsDouble:
            bytes.append(static_cast<uint8_t>(value.kind()));
            encodeVarint(bytes, zigZagEncode(value.virtualRegister().offset()));
            break;

        case ExitValueArgument:
            bytes.append(static_cast<uint8_t>(value.kind()));
            bytes.append(static_cast<uint8_t>(value.exitArgument().format()));
            encodeVarint(bytes, value.exitArgument().argument());
            break;

        case ExitValueConstant: {
            bytes.append(static_cast<uint8_t>(value.kind()));
            EncodedJSValue encoded = JSValue::encode(value.constant());
            bytes.append(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&encoded), sizeof(encoded)));
            break;
        }

        case ExitValueMaterializeNewObject:
            bytes.append(static_cast<uint8_t>(value.kind()));
            encodeVarint(bytes, materializations.size());
            materializations.append(value.objectMaterialization());
            break;

        case ExitValueDead:
        case InvalidExitValue:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }
    if (deadRun)
        emitDeadRun(deadRun);

    m_encodedValues = WTF::move(bytes);
    m_valueMaterializations = WTF::move(materializations);

#if ASSERT_ENABLED
    ASSERT(!m_localsOffset);
    Operands<ExitValue> roundTrip = decodeValues();
    ASSERT(roundTrip.size() == values.size());
    for (unsigned i = 0; i < values.size(); ++i) {
        const ExitValue& a = values[i];
        const ExitValue& b = roundTrip[i];
        ASSERT(a.kind() == b.kind());
        switch (a.kind()) {
        case ExitValueDead:
            break;
        case ExitValueInJSStack:
        case ExitValueInJSStackAsInt32:
        case ExitValueInJSStackAsInt52:
        case ExitValueInJSStackAsDouble:
            ASSERT(a.virtualRegister() == b.virtualRegister());
            break;
        case ExitValueArgument:
            ASSERT(a.exitArgument().format() == b.exitArgument().format());
            ASSERT(a.exitArgument().argument() == b.exitArgument().argument());
            break;
        case ExitValueConstant:
            ASSERT(JSValue::encode(a.constant()) == JSValue::encode(b.constant()));
            break;
        case ExitValueMaterializeNewObject:
            ASSERT(a.objectMaterialization() == b.objectMaterialization());
            break;
        case InvalidExitValue:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif
}

Operands<ExitValue> OSRExitDescriptor::decodeValues() const
{
    Operands<ExitValue> result(m_numberOfArguments, m_numberOfLocals, m_numberOfTmps);
    std::span<const uint8_t> bytes = m_encodedValues.span();

    size_t offset = 0;
    unsigned index = 0;
    while (index < result.size()) {
        uint8_t tag = bytes[offset++];
        ExitValueKind kind = static_cast<ExitValueKind>(tag & exitValueKindMask);

        switch (kind) {
        case ExitValueDead: {
            unsigned run = tag >> exitValueKindBits;
            if (run == maxShortDeadRun)
                run += decodeVarint(bytes, offset);
            for (unsigned j = 0; j < run; ++j)
                result[index++] = ExitValue::dead();
            break;
        }

        case ExitValueInJSStack:
        case ExitValueInJSStackAsInt32:
        case ExitValueInJSStackAsInt52:
        case ExitValueInJSStackAsDouble: {
            VirtualRegister reg(zigZagDecode(decodeVarint(bytes, offset)));
            if (reg.isLocal())
                reg += m_localsOffset;
            ExitValue value;
            switch (kind) {
            case ExitValueInJSStack:
                value = ExitValue::inJSStack(reg);
                break;
            case ExitValueInJSStackAsInt32:
                value = ExitValue::inJSStackAsInt32(reg);
                break;
            case ExitValueInJSStackAsInt52:
                value = ExitValue::inJSStackAsInt52(reg);
                break;
            case ExitValueInJSStackAsDouble:
                value = ExitValue::inJSStackAsDouble(reg);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            result[index++] = value;
            break;
        }

        case ExitValueArgument: {
            DataFormat format = static_cast<DataFormat>(bytes[offset++]);
            unsigned argument = decodeVarint(bytes, offset);
            result[index++] = ExitValue::exitArgument(ExitArgument(format, argument));
            break;
        }

        case ExitValueConstant: {
            EncodedJSValue encoded;
            memcpy(&encoded, bytes.data() + offset, sizeof(encoded));
            offset += sizeof(encoded);
            result[index++] = ExitValue::constant(JSValue::decode(encoded));
            break;
        }

        case ExitValueMaterializeNewObject:
            result[index++] = ExitValue::materializeNewObject(m_valueMaterializations[decodeVarint(bytes, offset)]);
            break;

        case InvalidExitValue:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }
    RELEASE_ASSERT(index == result.size());
    RELEASE_ASSERT(offset == bytes.size());
    return result;
}

void OSRExitDescriptor::validateReferences(const TrackedReferences& trackedReferences)
{
    Operands<ExitValue> values = decodeValues();
    for (unsigned i = values.size(); i--;)
        values[i].validateReferences(trackedReferences);

    for (ExitTimeObjectMaterialization* materialization : m_materializations)
        materialization->validateReferences(trackedReferences);
}

Ref<OSRExitHandle> OSRExitDescriptor::emitOSRExit(
    State& state, ExitKind exitKind, const NodeOrigin& nodeOrigin, CCallHelpers& jit,
    const StackmapGenerationParams& params, uint32_t dfgNodeIndex, unsigned offset)
{
    Ref<OSRExitHandle> handle =
        prepareOSRExitHandle(state, exitKind, nodeOrigin, params, dfgNodeIndex, offset);
    handle->emitExitThunk(state, jit);
    return handle;
}

Ref<OSRExitHandle> OSRExitDescriptor::emitOSRExitLater(
    State& state, ExitKind exitKind, const NodeOrigin& nodeOrigin,
    const StackmapGenerationParams& params, uint32_t dfgNodeIndex, unsigned offset)
{
    RefPtr<OSRExitHandle> handle =
        prepareOSRExitHandle(state, exitKind, nodeOrigin, params, dfgNodeIndex, offset);
    params.addLatePath(
        [handle, &state] (CCallHelpers& jit) {
            handle->emitExitThunk(state, jit);
        });
    return handle.releaseNonNull();
}

Ref<OSRExitHandle> OSRExitDescriptor::prepareOSRExitHandle(
    State& state, ExitKind exitKind, const NodeOrigin& nodeOrigin,
    const StackmapGenerationParams& params, uint32_t dfgNodeIndex, unsigned offset)
{
    FixedVector<B3::ValueRep> valueReps(params.size() - offset);
    for (unsigned i = offset, indexInValueReps = 0; i < params.size(); ++i, ++indexInValueReps)
        valueReps[indexInValueReps] = params[i];
    OSRExit exit(this, exitKind, nodeOrigin.forExit, nodeOrigin.semantic, nodeOrigin.wasHoisted, dfgNodeIndex, WTF::move(valueReps));
    if (exitKind == WillThrowOutOfMemoryError)
        exit.m_exitCallSiteIndex = callSiteIndexForCodeOrigin(state, nodeOrigin.semantic);

    unsigned index = state.jitCode->m_osrExit.size();
    state.jitCode->m_osrExit.append(WTF::move(exit));
    return adoptRef(*new OSRExitHandle(index, state.jitCode.get()));
}

OSRExit::OSRExit(
    OSRExitDescriptor* descriptor, ExitKind exitKind, CodeOrigin codeOrigin,
    CodeOrigin codeOriginForExitProfile, bool wasHoisted, uint32_t dfgNodeIndex, FixedVector<B3::ValueRep>&& valueReps)
    : OSRExitBase(exitKind, codeOrigin, codeOriginForExitProfile, wasHoisted, dfgNodeIndex)
    , m_descriptor(descriptor)
    , m_valueReps(WTF::move(valueReps))
{
}

CodeLocationJump<JSInternalPtrTag> OSRExit::codeLocationForRepatch(CodeBlock* ftlCodeBlock) const
{
    UNUSED_PARAM(ftlCodeBlock);
    return m_patchableJump;
}

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)
