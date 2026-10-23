/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTUnspecifiedValuePaths.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void FunctionVerifier::verifyUnspecifiedValues()
{
    UsesOfUnspecifiedValues found = findUsesOfUnspecifiedValues(analyses(), 1, m_inputs.otherFunctionsMayChange ? OtherFunctions::MayChange : OtherFunctions::AreAtRest);
    for (const UseOfUnspecifiedValue& use : found.uses)
        fail(InternalErrorKind::UnspecifiedValueInspected, use.block, use.user, use.operand.index());
    if (found.uses.isEmpty() && !found.wasEveryPathFollowed)
        fail(InternalErrorKind::UnspecifiedValueInspected);
}

bool verifyUnspecifiedValues(const Program& program, BuildContext& build)
{
    Vector<InternalError> errors = collectInParallel<InternalError>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        const Function& function = program.functions[index];
        bool makesUnspecifiedValue = function.hasBody() && std::ranges::any_of(function.values, [](const Value& value) {
            return value.opcode == Opcode::Unspecified;
        });
        if (!makesUnspecifiedValue)
            return;
        FunctionVerifier verifier(program, FunctionID(index), function, IRForm::Prepared, build, { });
        if (!verifier.runUnspecifiedValues())
            found.appendVector(verifier.takeErrors());
    });
    if (errors.isEmpty())
        return true;
    build.diagnostics().append(WTF::move(errors));
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
