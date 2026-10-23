/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTIR.h"
#include "AOTIRForm.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

bool hasTypedDataRules(Opcode);
bool hasCursorInSignature(const Program&, const Function&);
bool referencesFunctionWithCursor(const Program&, const Value&);

void verifyTypedDataRules(const Program&, FunctionID, const Function&, FunctionAnalyses&, IRForm, Vector<InternalError>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
