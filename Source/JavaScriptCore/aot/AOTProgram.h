/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"
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

using IdentifierIndices = UncheckedKeyHashMap<UniquedStringImpl*, uint32_t>;
JS_EXPORT_PRIVATE void setProgramIdentifierIndices(const IdentifierIndices*);
const IdentifierIndices* programIdentifierIndices();

static constexpr uint32_t invalidConstantIndex = std::numeric_limits<uint32_t>::max();
using ConstantIndices = UncheckedKeyHashMap<UnlinkedCodeBlock*, Vector<uint32_t>>;
JS_EXPORT_PRIVATE void setProgramConstantIndices(const ConstantIndices*);
const Vector<uint32_t>* programConstantIndicesFor(UnlinkedCodeBlock*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
