/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTOpcodeSets.h"

#define AOT_LINK_OF_WALK(link, opcodes) \
    extern const OpcodeSet opcodes; \
    bool link##OfItsOpcodes(Walker&, const BodyValue&); \
    ALWAYS_INLINE bool link(Walker& walker, const BodyValue& value) { return opcodes.contains(value.opcode()) && link##OfItsOpcodes(walker, value); } \
    template<Opcode known> ALWAYS_INLINE bool link(Walker& walker, const BodyValueOf<known>& value) { return opcodes.contains(known) && link##OfItsOpcodes(walker, value); }

#define AOT_LINK_OF_WALK_WITH_INPUTS(link, opcodes) \
    extern const OpcodeSet opcodes; \
    bool link##OfItsOpcodes(Walker&, const BodyValue&, const InputsOfTypedOperation&); \
    ALWAYS_INLINE bool link(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs) { return opcodes.contains(value.opcode()) && link##OfItsOpcodes(walker, value, inputs); } \
    template<Opcode known> ALWAYS_INLINE bool link(Walker& walker, const BodyValueOf<known>& value, const InputsOfTypedOperation& inputs) { return opcodes.contains(known) && link##OfItsOpcodes(walker, value, inputs); }

namespace JSC { namespace AOT { namespace Typed {

class InputsOfTypedOperation;
class Walker;

template<Opcode known> class BodyValueOf final : public BodyValue {
public:
    explicit BodyValueOf(const BodyValue& value)
        : BodyValue(value)
    {
        ASSERT(value.opcode() == known);
    }
    static constexpr Opcode opcode() { return known; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
