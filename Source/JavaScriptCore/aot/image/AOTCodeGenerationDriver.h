/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBranchCounterNumbers.h"
#include "AOTCodeGenerator.h"
#include "AOTInstanceLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTStringConstantNumbering.h"
#include "AOTStringUses.h"
#include <memory>
#include <wtf/BitSet.h>

namespace JSC { namespace AOT {

class StubCatalog;

namespace Typed {
class TypeTable;
}

struct CodeGenerationPlan {
    CodeGenerationPlan();
    CodeGenerationPlan(CodeGenerationPlan&&);
    ~CodeGenerationPlan();

    std::unique_ptr<const StubCatalog> stubCatalog;
    ProgramUses uses;
    StringConstantNumbering stringConstants;
    InstanceSizes instanceSizes { };
    Typed::BranchCounterNumbers branchCounters;
};

struct GeneratedCode {
    LinkedFunctions functions;
    WTF::BitSet<numberOfEntries> entriesUsed;
};

CodeGenerationPlan prepareCodeGeneration(const Typed::PipelineOutput&, BuildContext&);
Typed::CodeGenerationInputs createCodeGenerationInputs(const Typed::PipelineOutput&, const CodeGenerationPlan&);
GeneratedCode generateAllCode(const Typed::CodeGenerationInputs&, Typed::TypeTable&, BuildContext&);

void dropUnitsWithoutOutlinableCode(CodeBody&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
