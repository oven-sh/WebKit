/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTOutliner.h"
#include "GPRInfo.h"
#include <algorithm>
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Vector.h>

namespace JSC::AOT::ImageTests {

struct Steps {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Steps);

    enum class Step : uint8_t { Resolved, Chosen, Rewritten, Selected, JumpsFound, LaidOut, JumpsRemoved };

    Steps(Vector<CompiledCode>&&, const TestOptions& = { }, Vector<EntryAlias>&& = { });

    void choose();
    void useOutlining(Outlining&&);
    void rewrite();
    void select();
    void findJumps();
    void layOut(size_t webAssemblyCodeSize = 0, size_t webAssemblyCodeAlignment = 1);
    void removeJumps();

    OutlinerInputs outlinerInputs() const { return { functions, callees, aliases.span(), testTemplates(), numberOfSharedCodeCopies }; }
    CodeLayoutInputs layoutInputs() const
    {
        return { functions, callees, aliases.span(), outlinedCode, sharedCode, regExpSection, testTemplates(), finalJumps.span(), shift, webAssemblyCodeSize, webAssemblyCodeAlignment, constants.bytes.size() };
    }

    TestBuild build;
    LinkedFunctions functions;
    Vector<EntryAlias> aliases;
    std::optional<CodeIndex> index;
    ResolvedCallees callees;
    unsigned numberOfSharedCodeCopies { 1 };
    Outlining outlining;
    OutlinedCode outlinedCode;
    unsigned shift { 0 };
    SharedCode sharedCode;
    ConstantPool constants;
    Vector<FinalJump> finalJumps;
    RegExpSection regExpSection;
    size_t webAssemblyCodeSize { 0 };
    size_t webAssemblyCodeAlignment { 1 };
    CodeLayout layout;
    Step done { Step::Resolved };
};

inline unsigned numberOfFunctionsIn(const Vector<CompiledCode>& code, const Vector<EntryAlias>& aliases)
{
    uint32_t greatest = 0;
    for (auto& function : code)
        greatest = std::max(greatest, function.function.index());
    for (auto& alias : aliases)
        greatest = std::max(greatest, alias.function.index());
    return greatest + 1;
}

inline Steps::Steps(Vector<CompiledCode>&& code, const TestOptions& options, Vector<EntryAlias>&& entryAliases)
    : build(numberOfFunctionsIn(code, entryAliases), options)
    , functions(createLinkedFunctions(WTF::move(code), build.context()))
    , aliases(WTF::move(entryAliases))
{
    index.emplace(functions.span());
    callees = resolveCallees(functions, *index, aliases.span(), build.context());
    numberOfSharedCodeCopies = countSharedCodeCopies(functions, build.context());
}

inline void Steps::choose()
{
    if (done >= Step::Chosen)
        return;
    outlining = chooseOutlinedSequences(outlinerInputs(), build.context());
    done = Step::Chosen;
}

inline void Steps::useOutlining(Outlining&& chosen)
{
    RELEASE_ASSERT(done < Step::Chosen);
    outlining = WTF::move(chosen);
    done = Step::Chosen;
}

inline void Steps::rewrite()
{
    if (done >= Step::Rewritten)
        return;
    choose();
    outlinedCode = applyOutlining(functions, aliases, { *index, callees, outlining, testTemplates() }, build.context());
    done = Step::Rewritten;
}

inline void Steps::select()
{
    if (done >= Step::Selected)
        return;
    rewrite();
    shift = chooseCodeGranuleShift(functions, build.context());
    sharedCode = selectSharedCode(functions, outlinedCode, build.stubCatalog(), { }, build.context());
    done = Step::Selected;
}

inline void Steps::findJumps()
{
    if (done >= Step::JumpsFound)
        return;
    select();
    constants = buildConstantPool(functions, build.context());
    finalJumps = findFinalJumps(functions, callees, outlinedCode, build.context());
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

inline void appendSequence(CodeSketch& sketch, uint32_t firstWord = 1)
{
    for (uint32_t word = firstWord; word < firstWord + 4; ++word)
        sketch.plain(word);
}

inline CompiledCode repeated(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.plain(0x1000 + function);
    appendSequence(sketch);
    sketch.end();
    return sketch.take();
}

inline Vector<CompiledCode> repeatedBodies(unsigned count = 40)
{
    Vector<CompiledCode> bodies;
    bodies.reserveInitialCapacity(count);
    for (uint32_t function = 1; function <= count; ++function)
        bodies.append(repeated(function));
    return bodies;
}

inline constexpr unsigned numberOfSingleCallerSites = 12;

inline CodeSketch singleCallerSketch(uint32_t function, uint32_t firstWord = 1)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    for (uint32_t site = 0; site < numberOfSingleCallerSites; ++site) {
        sketch.pinned(100 + site);
        appendSequence(sketch, firstWord);
    }
    return sketch;
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
