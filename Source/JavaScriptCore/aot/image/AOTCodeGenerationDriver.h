/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrivalSiteNumbers.h"
#include "AOTBodyFactsForTables.h"
#include "AOTBranchCounterNumbers.h"
#include "AOTCodeGenerator.h"
#include "AOTInstanceLayout.h"
#include "AOTLinkedFunctions.h"
#include "AOTPromisedResults.h"
#include "AOTStringUses.h"
#include "AOTStubCatalog.h"

namespace JSC { namespace AOT {

using OperationCounts = Vector<std::pair<Typed::Opcode, uint32_t>>;

struct CodeGenerationPlan {
    std::unique_ptr<const StubCatalog> stubCatalog;
    ProgramUses uses;
    StringConstantNumbering stringConstants;
    InstanceSizes instanceSizes { };
    Typed::BodyFactsForTables bodyFactsForTables;
    BitVector bodiesWithoutCode;
    Typed::BranchCounterNumbers branchCounters;
    Typed::ArrivalSiteNumbers arrivalSites;
    Typed::PromisedResults promisedResults;
};

struct GeneratedCode {
    LinkedFunctions functions;
    WTF::BitSet<numberOfEntries> entriesUsed;
    Vector<OperationCounts> operationCountsByFunction;
};

CodeGenerationPlan prepareCodeGeneration(const Typed::PipelineOutput&, BuildContext&);
Typed::CodeGenerationInputs createCodeGenerationInputs(const Typed::PipelineOutput&, const CodeGenerationPlan&);

class CodeGenerationJob {
    WTF_MAKE_NONCOPYABLE(CodeGenerationJob);
public:
    // Takes the bodies out of the program, so no other thread may be reading its functions.
    CodeGenerationJob(const Typed::CodeGenerationInputs&, Typed::Program&, BuildContext&, const BitVector* bodiesWithoutCode = nullptr);
    ~CodeGenerationJob();

    GeneratedCode run();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

OperationCounts countOperations(const Typed::Function&);

void dropUnitsWithoutOutlinableCode(CodeBody&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
