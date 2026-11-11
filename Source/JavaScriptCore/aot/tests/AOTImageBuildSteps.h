/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTConstantPool.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestSupport.h"
#include "AOTLinkedFunctions.h"
#include "AOTRegExpCompiler.h"
#include "AOTStubCalls.h"
#include "AOTVector.h"
#include "GPRInfo.h"
#include <algorithm>
#include <wtf/FastMalloc.h>

namespace JSC::AOT::ImageTests {

struct Steps {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Steps);

    enum class Step : uint8_t { Resolved, Selected, JumpsFound, LaidOut, JumpsRemoved };

    Steps(Vector<CompiledCode>&&, const TestOptions& = { });

    void select();
    void findJumps();
    void layOut(size_t webAssemblyCodeSize = 0, size_t webAssemblyCodeAlignment = 1);
    void removeJumps();

    CodeLayoutInputs layoutInputs() const
    {
        return { functions, callees, sharedCode, regExpSection, testTemplates(), finalJumps.span(), shift, webAssemblyCodeSize, webAssemblyCodeAlignment, constants.bytes.size(), 0, 1, 0, farInstanceWords.span() };
    }

    TestBuild build;
    LinkedFunctions functions;
    ResolvedCallees callees;
    unsigned shift { 0 };
    SharedCode sharedCode;
    ConstantPool constants;
    Vector<FinalJump> finalJumps;
    Vector<FarInstanceWords> farInstanceWords;
    RegExpSection regExpSection;
    size_t webAssemblyCodeSize { 0 };
    size_t webAssemblyCodeAlignment { 1 };
    CodeLayout layout;
    Step done { Step::Resolved };
};

inline unsigned numberOfFunctionsIn(const Vector<CompiledCode>& code)
{
    uint32_t greatest = 0;
    for (auto& function : code)
        greatest = std::max(greatest, function.unit.function.index());
    return greatest + 1;
}

inline Steps::Steps(Vector<CompiledCode>&& code, const TestOptions& options)
    : build(numberOfFunctionsIn(code), options)
    , functions(createLinkedFunctions(WTF::move(code), build.context()))
    , callees(resolveCallees(functions, build.plan(), build.context()))
{
}

inline void Steps::select()
{
    if (done >= Step::Selected)
        return;
    shift = chooseCodeGranuleShift(functions, build.context());
    sharedCode = selectSharedCode(functions, build.stubCatalog(), { }, build.context());
    done = Step::Selected;
}

inline void Steps::findJumps()
{
    if (done >= Step::JumpsFound)
        return;
    select();
    constants = buildConstantPool(functions, build.context());
    finalJumps = findFinalJumps(functions, callees, build.context());
    done = Step::JumpsFound;
}

inline void Steps::layOut(size_t codeSize, size_t codeAlignment)
{
    if (done >= Step::LaidOut)
        return;
    findJumps();
    webAssemblyCodeSize = codeSize;
    webAssemblyCodeAlignment = codeAlignment;
    layout = layOutCode(layoutInputs(), build.context());
    done = Step::LaidOut;
}

inline void Steps::removeJumps()
{
    if (done >= Step::JumpsRemoved)
        return;
    layOut();
    removeFinalJumps(functions, layout.fallsIntoNext, finalJumps.span(), build.context());
    done = Step::JumpsRemoved;
}

inline ArgumentMoves stubArgumentMoves(StubKey key, uint64_t constant)
{
    ArgumentMoves moves;
    moves.append(key.bits());
    moves.append(0);
    appendArgumentMove(moves, GPRInfo::argumentGPR1, ArgumentSource::Constant, constant);
    return moves;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
