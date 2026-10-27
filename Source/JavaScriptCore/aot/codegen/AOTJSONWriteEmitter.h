/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderCursor.h"
#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class JSONWriteEmitter final : public OperationEmitter {
public:
    explicit JSONWriteEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateStringBuilderReserve(const ValueContext& value) { define(value, builders().reserveAtMost(value, builderOf(value), value->size())); }
    void generateStringBuilderCommit(const ValueContext& value) { builders().commit(builderOf(value), cursorOf(value)); }
    void generateJSONResetHolders(const ValueContext&);
    void generateJSONEnterHolder(const ValueContext&);
    void generateJSONLeaveHolder(const ValueContext&);
    void generateJSONListedHolders(const ValueContext&);
    void generateJSONRestoreHolders(const ValueContext&);
    void generateJSONEnterWatchedLevel(const ValueContext&);
    void generateJSONAppendKey(const ValueContext&);
    void generateJSONAppendToken(const ValueContext&);
    void generateJSONAppendBoolean(const ValueContext&);
    void generateJSONAppendInteger(const ValueContext& value) { appendNumber(value, true); }
    void generateJSONAppendNumber(const ValueContext& value) { appendNumber(value, false); }
    void generateJSONAppendDate(const ValueContext& value) { define(value, callLeaf<Pointer>(RuntimeOperation::JSONAppendDate, { cursorOf(value), in<Number>(value->valueOperand()) })); }
    void generateJSONAppendString(const ValueContext&);
    void generateJSONAppendValue(const ValueContext&);

private:
    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    CellPointer builderOf(const ValueContext& value) const { return in<CellPointer>(value->builderOperand()); }
    Pointer cursorOf(const ValueContext& value) const { return in<Pointer>(value->cursorOperand()); }
    Pointer holdersOf(const ValueContext& value) const { return in<Pointer>(value->holdersOperand()); }
    TypedPointer listedHoldersOf(const ValueContext&);
    void appendNumber(const ValueContext&, bool isInteger);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
