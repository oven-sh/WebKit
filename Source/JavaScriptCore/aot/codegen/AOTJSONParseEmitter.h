/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOperationEmitter.h"
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr int64_t stackReservedForJSONParsers = 64 * KB;

class JSONParseEmitter final : public OperationEmitter {
public:
    explicit JSONParseEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateJSONSourceOpen(const ValueContext&);
    void generateJSONSourceStart(const ValueContext& value) { define(value, Pointer(m_out.loadPtr(word(AOT_INSTANCE_OFFSET(json.sourceStart))))); }
    void generateJSONSourceClose(const ValueContext&);
    void generateJSONSourceAbandon(const ValueContext& value) { callVoidOperation(value, RuntimeOperation::JSONSourceAbandon, { }); }
    void generateJSONDecodeBytes(const ValueContext& value) { define(value, callOperation<Boxed>(value, RuntimeOperation::JSONDecodeBytes, { boxed(value->valueOperand()) })); }
    void generateJSONGetByte(const ValueContext& value) { define(value, Word32(m_out.load8ZeroExt32(at(cursorOf(value))))); }
    void generateJSONStartsWith(const ValueContext& value) { defineTest(value, startsWith(cursorOf(value), m_program.string(value->string()), value->isLikely())); }
    void generateJSONAdvance(const ValueContext& value) { define(value, offsetBy(cursorOf(value), m_program.string(value->string()).size())); }
    void generateJSONSkipByte(const ValueContext&);
    void generateJSONSkipWhitespace(const ValueContext&);
    void generateJSONParseString(const ValueContext& value) { defineParsedByOperation(value, RuntimeOperation::JSONParseString, Representation::Cell); }
    void generateJSONParseKey(const ValueContext& value) { defineParsedByOperation(value, RuntimeOperation::JSONParseKey, Representation::Cell); }
    void generateJSONParseNumber(const ValueContext& value) { parseDigits(value, false); }
    void generateJSONParseInteger(const ValueContext& value) { parseDigits(value, true); }
    void generateJSONParseValue(const ValueContext& value) { defineParsedByOperation(value, RuntimeOperation::JSONParseValue, Representation::JSValue); }
    void generateJSONFindMember(const ValueContext&);
    void generateJSONMemberRepeats(const ValueContext&);
    void generateJSONKeyHasEscapes(const ValueContext&);
    void generateJSONHasStackSpace(const ValueContext&);
    void generateJSONObjectBegin(const ValueContext&);
    void generateJSONObjectKey(const ValueContext& value) { define(value, advanceKeyOrder(value, in<Pointer>(value->orderOperand()))); }
    void generateJSONObjectFinish(const ValueContext&);
    void generateJSONParseExtraMembers(const ValueContext&);

private:
    struct ExtraMemberState {
        Boxed extra;
        Pointer order;
    };

    Pointer cursorOf(const ValueContext& value) const { return in<Pointer>(value->cursorOperand()); }
    TypedPointer word(ptrdiff_t offsetInInstance) { return instanceField(m_heaps.entire(AbstractHeap::ParserState), offsetInInstance); }
    TypedPointer at(Pointer cursor, ptrdiff_t offset = 0) { return m_out.address(m_heaps.entire(AbstractHeap::ParserState), cursor.value(), offset); }
    Pointer sourceEnd() { return Pointer(m_out.loadPtr(word(AOT_INSTANCE_OFFSET(json.sourceEnd)))); }
    Pointer framePointerAtThisPoint();
    Condition startsWith(Pointer cursor, std::span<const uint8_t> text, bool isLikely);
    StubCallResult callReturningPair(const ValueContext&, RuntimeOperation, std::initializer_list<Operand> arguments);
    Pointer cursorOrSourceEnd(Condition isParsed, LValue next) { return select(isParsed, Pointer(next), sourceEnd()); }
    void defineParsed(const ValueContext&, Operand value, LValue next);
    void defineParsedByOperation(const ValueContext&, RuntimeOperation, Representation parsed);
    void parseDigits(const ValueContext&, bool isInteger);
    // Zero where the layout has no shape, which is reported as an internal error.
    uint32_t shapeOfParsedObject(const ValueContext&);
    Pointer findOrderAfter(Pointer numberedOrder, uint32_t shape, uint32_t slotKey, LBasicBlock miss);
    Pointer advanceKeyOrder(const ValueContext&, Pointer order);
    ExtraMemberState updateInBuffer(ExtraMemberState, NOESCAPE const Invocable<void(Pointer buffer)> auto& call);
};

static_assert(sizeof(JSONParseEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
