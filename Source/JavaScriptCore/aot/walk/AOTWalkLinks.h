/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTOpcodeSets.h"

// A LINK THAT CANNOT FIRE IS NEVER ENTERED. The walk asks its links one behind the other at every operation, and almost every link takes a few operations of the hundreds.
// So a link is DECLARED WITH THE SET THAT IT TAKES: its head stands here, in line, and says no by one word of the set; its body, `<link>OfItsOpcodes`, is entered for the rest.
// AND THE CHAINS ARE COMPILED ONCE AN OPCODE (Walker::walkThroughChainOf<>, buildTypedOperationOf<>): there the operation is a BodyValueOf<opcode>, whose opcode is a constant, so the head of a link
// whose set is written in a header (`inline constexpr OpcodeSet`) is no code at all where the link does not take the operation. A LINK MORE COSTS THE BUILD NOTHING.
// Whoever calls the link writes its name as ever, in the order as ever. A new link is declared by one of these lines, in the header of its unit, and defines its set beside its body:
//     inline constexpr OpcodeSet opcodesThatMyLinkTakes { Opcode::GetSlot, Opcode::PutSlot };     in the header, in front of:
//     AOT_LINK_OF_WALK(myLink, opcodesThatMyLinkTakes)
//     bool myLinkOfItsOpcodes(Walker& walker, const BodyValue& value) { .. }                      the body
// A set that is made of sets of its own file, or that other units widen there, stays there: nothing in the header but the macro, `constinit const OpcodeSet opcodesThatMyLinkTakes = ..;` in front of the body: one word is tested.
// A set that is too narrow loses a form and changes the code that comes out, which the comparison of images shows. A set that is too wide costs a call.
#define AOT_LINK_OF_WALK(link, opcodes) \
    extern const OpcodeSet opcodes; \
    bool link##OfItsOpcodes(Walker&, const BodyValue&); \
    ALWAYS_INLINE bool link(Walker& walker, const BodyValue& value) { return opcodes.contains(value.opcode()) && link##OfItsOpcodes(walker, value); } \
    template<Opcode known> ALWAYS_INLINE bool link(Walker& walker, const BodyValueOf<known>& value) { return opcodes.contains(known) && link##OfItsOpcodes(walker, value); }

// A link that is also asked ahead, with types in the place of values.
#define AOT_LINK_OF_WALK_WITH_INPUTS(link, opcodes) \
    extern const OpcodeSet opcodes; \
    bool link##OfItsOpcodes(Walker&, const BodyValue&, const InputsOfTypedOperation&); \
    ALWAYS_INLINE bool link(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs) { return opcodes.contains(value.opcode()) && link##OfItsOpcodes(walker, value, inputs); } \
    template<Opcode known> ALWAYS_INLINE bool link(Walker& walker, const BodyValueOf<known>& value, const InputsOfTypedOperation& inputs) { return opcodes.contains(known) && link##OfItsOpcodes(walker, value, inputs); }

namespace JSC { namespace AOT { namespace Typed {

class InputsOfTypedOperation;
class Walker;

// An operation in the copy of a chain that is compiled for its opcode.
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
