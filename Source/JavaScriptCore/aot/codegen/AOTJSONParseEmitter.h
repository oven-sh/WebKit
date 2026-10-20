/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr int64_t stackReservedForJSONParsers = 64 * KB;

class JSONParseEmitter final : public OperationEmitter {
public:
    explicit JSONParseEmitter(CodeGenerator&);

    void generateJSONSourceOpen(const ValueContext&);
    void generateJSONSourceStart(const ValueContext&);
    void generateJSONSourceClose(const ValueContext&);
    void generateJSONSourceAbandon(const ValueContext&);
    void generateJSONDecodeBytes(const ValueContext&);
    void generateJSONGetByte(const ValueContext&);
    void generateJSONStartsWith(const ValueContext&);
    void generateJSONAdvance(const ValueContext&);
    void generateJSONSkipByte(const ValueContext&);
    void generateJSONSkipWhitespace(const ValueContext&);
    void generateJSONParseString(const ValueContext&);
    void generateJSONParseKey(const ValueContext&);
    void generateJSONParseNumber(const ValueContext&);
    void generateJSONParseInteger(const ValueContext&);
    void generateJSONParseValue(const ValueContext&);
    void generateJSONFindMember(const ValueContext&);
    void generateJSONMemberRepeats(const ValueContext&);
    void generateJSONKeyRepeats(const ValueContext&);
    void generateJSONKeyHasEscapes(const ValueContext&);
    void generateJSONHasStackSpace(const ValueContext&);
    void generateJSONObjectBegin(const ValueContext&);
    void generateJSONObjectKey(const ValueContext&);
    void generateJSONObjectFinish(const ValueContext&);
    void generateJSONParseExtraMembers(const ValueContext&);
    void generateJSONAddExtraMember(const ValueContext&);

private:
    struct ExtraMemberState {
        Boxed extra;
        Pointer order;
    };

    Pointer cursorOf(ValueID) const;
    TypedPointer word(ptrdiff_t offsetInInstance);
    TypedPointer at(Pointer cursor, ptrdiff_t offset = 0);
    Pointer sourceEnd();
    Pointer framePointerAtThisPoint();
    Pointer skipWhitespace(Pointer cursor);
    Condition startsWith(Pointer cursor, std::span<const uint8_t> text, bool isLikely);
    StubCallResult callReturningPair(const ValueContext&, RuntimeOperation, std::initializer_list<Operand> arguments);
    Pointer cursorOrSourceEnd(Condition isParsed, LValue next);
    void defineParsed(const ValueContext&, Operand value, LValue next);
    void parseDigits(const ValueContext&, bool isInteger);
    uint32_t shapeOfParsedObject(LayoutID) const;
    Pointer advanceKeyOrder(const ValueContext&, Pointer order, LayoutID, uint32_t field);
    ExtraMemberState updateInBuffer(ExtraMemberState, NOESCAPE const Invocable<void(Pointer buffer)> auto& call);
};

static_assert(sizeof(JSONParseEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
