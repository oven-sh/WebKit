/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void FunctionVerifier::verifyUnspecifiedValues()
{
    for (uint32_t index = 0; index < m_function.blocks.size() && !hasFailed(); ++index) {
        BlockID block(index);
        for (ValueID id : m_function.at(block).values)
            verifyNoOperandIsUnspecified(block, id, m_function.at(id));
        verifyNoOperandIsUnspecified(block, m_function.at(block).terminator);
    }
}

bool verifyUnspecifiedValues(const Program& program, BuildContext& build)
{
    Vector<InternalError> errors = collectInParallel<InternalError>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        const Function& function = program.functions[index];
        bool hasUnspecifiedValue = function.hasBody() && std::ranges::any_of(function.values, [&](const Value& value) {
            return value.type && program.at(value.type).mayBeUnspecified();
        });
        if (!hasUnspecifiedValue)
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
