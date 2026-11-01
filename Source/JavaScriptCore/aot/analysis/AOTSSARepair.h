/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class SSARepair {
    WTF_MAKE_NONCOPYABLE(SSARepair);
public:
    SSARepair(IRBuilder&, FunctionAnalyses&);
    ~SSARepair();

    using Variable = unsigned;
    Variable addVariable(ValueID definition);
    void addDefinition(Variable, ValueID definition);
    std::optional<Variable> findVariable(ValueID definition) const;
    void run();

private:
    struct Definition {
        BlockID block;
        uint32_t order { 0 };
        ValueID value;
    };

    struct RecordedUse {
        Use use;
        ProgramPoint point;
        ValueID operand;
        bool isDominated { false };
    };

    struct VariableData {
        Vector<ValueID, 2> values;
        Vector<Definition, 2> definitions;
        Vector<RecordedUse, 4> uses;
        TypeID type;
        TypeID expected;
    };

    void setIsPending(bool);
    void separateUsesAsGuards();
    void splitCoveredBlocksAfterDefinitions();

    void rematerializeConstants(std::span<const Variable>);
    void insertParameters(std::span<const Variable>);
    bool isInOrderAsItStands(const VariableData&);

    static ValueID findLocalDefinition(const VariableData&, const RecordedUse&);
    static const Definition* findLastDefinitionBefore(const VariableData&, BlockID, uint32_t order);
    static void sortDefinitions(VariableData&);

    IRBuilder& m_builder;
    FunctionAnalyses& m_analyses;
    Vector<VariableData> m_variables;
    BitVector m_isDefinition;
    Vector<std::pair<ValueID, Variable>, 8> m_definitions;
    // Of the first of them, once somebody asks and they are many.
    mutable UncheckedKeyHashMap<uint32_t, Variable, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variableOfDefinition;
    mutable size_t m_numberOfIndexedDefinitions { 0 };
    bool m_didRun { false };
    bool m_isPending { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
