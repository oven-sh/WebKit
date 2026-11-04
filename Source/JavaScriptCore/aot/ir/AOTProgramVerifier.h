/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

// The rules of the program's tables as the front end sends them: everything but the bodies, which AOTBodyVerifier.h checks.
// It runs in every build, behind Program::read() and in front of the bodies' verifier.
bool verifyRecords(const Program&, BuildContext&);
// The part of the rules of the module graph that goes by what code names. verifyBodies() ends with it: it reads every body through cursors, which are safe only on words that have been checked.
bool verifyModulesNamedInBodies(const Program&, BuildContext&);

bool isValidPropertyKey(const PropertyKey&);
bool hoistsFunctionsIntoVariables(ModuleKind);

class ProgramVerifier {
    WTF_MAKE_NONCOPYABLE(ProgramVerifier);
public:
    ProgramVerifier(const Program&, BuildContext&);

    bool run();
    bool runOnModulesNamedInBodies();

private:
    void fail(InternalError&&);
    bool hasFailed() const { return m_hasFailed; }

    // AOTProgramVerifier.cpp
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

    // AOTProgramVerifierModules.cpp
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

    // AOTProgramVerifierFunctions.cpp
    void verifyFunctionHeaders();
    void verifyFunctionHeader(FunctionID, const Function&);

    const Program& m_program;
    BuildContext& m_build;
    bool m_hasFailed { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
