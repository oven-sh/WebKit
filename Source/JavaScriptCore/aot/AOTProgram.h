/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"
#include "AOTType.h"
#include "DeclaredNamesLink.h"
#include "JSCJSValue.h"
#include "VirtualRegister.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSGlobalObject;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedModuleProgramCodeBlock;
class VM;

namespace AOT {

struct CoveredOperation {
    static constexpr uint8_t isInGenericCopy = 1;
    static constexpr uint8_t isRarelyExecuted = 2;
    static constexpr uint8_t isInLoop = 4;
    static constexpr uint8_t isInlined = 8;
    static constexpr uint8_t isElided = 16;
    static constexpr uint8_t isNotInGraph = 64;
    static constexpr uint32_t noReason = UINT32_MAX;
    static constexpr uint32_t noCounter = UINT32_MAX;
    static constexpr uint32_t countersPerOperation = 3;
    static constexpr uint32_t stubCallsCounter = 1;
    static constexpr uint32_t runtimeCallsCounter = 2;

    UnlinkedCodeBlock* codeBlock { nullptr };
    ImageKey function;
    uint32_t bytecodeOffset { 0 };
    uint32_t divot { 0 };
    uint32_t reason { noReason };
    uint32_t counter { noCounter };
    uint16_t opcode { 0 };
    uint8_t flags { 0 };
    String property;
    Vector<String, 2> outcomes;
};



void noteDeclaredNames(UnlinkedCodeBlock*, RefPtr<DeclaredNamesLink>&&);
void recordFunctionAssignments(UnlinkedCodeBlock*, Vector<FunctionAssignment>&&);
void forgetDeclaredNames();

using IdentifierIndices = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
JS_EXPORT_PRIVATE void setProgramIdentifierIndices(const IdentifierIndices*);
const IdentifierIndices* programIdentifierIndices();

static constexpr uint32_t invalidConstantIndex = std::numeric_limits<uint32_t>::max();
using ConstantIndices = UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<uint32_t>>;
JS_EXPORT_PRIVATE void setProgramConstantIndices(const ConstantIndices*);
const Vector<uint32_t>* programConstantIndicesFor(UnlinkedCodeBlock*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
