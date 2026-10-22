/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTIR.h"
#include "AOTRelocation.h"
#include "CCallHelpers.h"

namespace JSC {

namespace B3 {
class Procedure;
}

namespace AOT {

class BuildContext;

namespace Typed {

struct CodeGenerationInputs;
struct CodeRecords;
struct StackCheckState;

struct EmittedCode {
    ImageLinkBuffer& buffer;
    B3::Procedure* procedure;
    const StackCheckState& stackChecks;
    CCallHelpers::Label paddingStart;
    Vector<Relocation> relocations;
};

struct DescribedFunction {
    BuildContext& build;
    const Program& program;
    FunctionID id;
    const Function& function;
};

CompiledCode describeCode(BuildContext&, const CodeGenerationInputs&, FunctionID, const Function&, CodeRecords&, EmittedCode&&);
SiteDescription describeSites(const DescribedFunction&, CodeRecords&, LinkBuffer&, CodeBody&);

Vector<ConstantData> copyConstants(const B3::Procedure&);
Vector<AOT::JumpTable> takeJumpTables(B3::Procedure&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
