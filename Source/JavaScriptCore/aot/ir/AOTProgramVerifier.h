/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

bool verifyRecords(const Program&, BuildContext&);
bool verifyModulesNamedInBodies(const Program&, BuildContext&);

bool isValidPropertyKey(const PropertyKey&);
bool hoistsFunctionsIntoVariables(ModuleKind);
BitVector variablesThatRunTimeMayFill(const Program&);

class ProgramVerifier {
    WTF_MAKE_NONCOPYABLE(ProgramVerifier);
public:
    ProgramVerifier(const Program&, BuildContext&);

    bool run();
    bool runOnModulesNamedInBodies();

private:
    COLD NEVER_INLINE void fail(InternalError&&);
    bool hasFailed() const { return m_hasFailed; }

    void verifyIDs();
    void verifyTypes();
    void verifyPropertyKeys();
    void verifyDeclaredPositions();
    void verifyLayouts();
    void verifyLayout(LayoutID, const Layout&);
    void verifyFields(LayoutID, const Layout&);
    void verifyClasses();
    bool isValidClass(LayoutID, const Layout&) const;
    void verifyVariables();

    void verifyModules();
    void verifyModule(ModuleID, const Module&);
    bool hasValidBody(const Module&) const;
    void verifyModuleGraph();
    void verifyModulesNamedInBodies();
    void verifyModuleFailures();
    void verifyEntries();
    void verifyData();
    void verifyDataNode(DataID, const DataNode&);
    void verifySmallRecords();

    void verifyFunctionHeaders();
    void verifyFunctionHeader(FunctionID, const Function&);

    const Program& m_program;
    BuildContext& m_build;
    bool m_hasFailed { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
