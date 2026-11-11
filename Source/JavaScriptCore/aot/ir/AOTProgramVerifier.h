/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTIR.h"
#include <compare>
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;
struct InternalError;

namespace Typed {

class BodyValue;

struct MentionInCode {
    enum class Kind : uint8_t { LoadsModule, NamesVariable };

    auto operator<=>(const MentionInCode&) const = default;

    Kind kind;
    ModuleID file;
    ModuleID other;
};

class MentionsInCode {
    WTF_MAKE_NONCOPYABLE(MentionsInCode);
public:
    explicit MentionsInCode(const Program& program)
        : m_program(program)
    {
    }

    void beginFunction(const Function&);
    void noteWhatValueNames(const BodyValue&);
    void noteWhatFunctionNames(const Function&);
    Vector<MentionInCode> take() { return std::exchange(m_found, { }); }

private:
    void add(MentionInCode::Kind, ModuleID other);

    const Program& m_program;
    ModuleID m_file;
    size_t m_first { 0 };
    Vector<MentionInCode> m_found;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_mentionsOfLongList;
};

bool verifyRecords(const Program&, BuildContext&);
bool verifyModulesNamedInBodies(const Program&, BuildContext&, Vector<MentionInCode>&& mentionsInBodies);

bool isValidPropertyKey(const PropertyKey&);
bool hoistsFunctionsIntoVariables(ModuleKind);
BitVector variablesThatRunTimeMayFill(const Program&);

class ProgramVerifier {
    WTF_MAKE_NONCOPYABLE(ProgramVerifier);
public:
    ProgramVerifier(const Program&, BuildContext&);

    bool run();
    bool runOnModulesNamedInBodies(Vector<MentionInCode>&& mentionsInBodies);

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
    void verifyModuleInstances();

    void verifyModules();
    void verifyModule(ModuleID, const Module&);
    bool hasValidBody(const Module&) const;
    void verifyModuleGraph();
    void verifyModulesNamedInBodies(Vector<MentionInCode>&& mentionsInBodies);
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
