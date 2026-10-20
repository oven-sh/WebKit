/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/SSACalculator.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class SSARepairAdapter {
public:
    using CFG = Typed::CFG;
    using Value = ValueID;

    explicit SSARepairAdapter(FunctionAnalyses& analyses)
        : m_analyses(analyses)
    {
    }

    CFG& cfg() const { return m_analyses.cfg(); }
    Dominators& dominators() { return m_analyses.dominators(); }

    static void dumpValue(PrintStream& out, ValueID value) { out.print("v", value.index()); }

private:
    FunctionAnalyses& m_analyses;
};

class SSARepair {
    WTF_MAKE_NONCOPYABLE(SSARepair);
public:
    SSARepair(IRBuilder&, FunctionAnalyses&);

    using Variable = unsigned;
    Variable addVariable(ValueID definition);
    void addDefinition(Variable, ValueID definition);
    std::optional<Variable> findVariable(ValueID definition) const;
    void run();

private:
    using Calculator = WTF::SSACalculator<SSARepairAdapter>;

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
        TypeID prediction;
    };

    void splitCoveredBlocksAfterDefinitions();

    void collectUses(VariableData&);
    void collectDefinitions(VariableData&);
    bool hasOneConstant(const VariableData&) const;
    void findDominatedUses(VariableData&);

    void rematerializeConstants(std::span<const Variable>);
    void rematerializeConstant(Variable);
    ValueID insertConstant(const VariableData&, ProgramPoint);
    void setConstantAtTail(Variable, BlockID, ValueID constant);
    ValueID findConstantAtHead(Variable, BlockID);

    void insertParameters(std::span<const Variable>);
    void computeLiveness(Variable);
    bool isLiveAtHead(Variable variable, BlockID block) const { return m_liveAtHeadStamp[block.index()] == variable + 1; }
    void computeParameterType(VariableData&);
    ValueID addMergeParameter(VariableData&, BlockID);
    void appendArguments(Calculator&, std::span<const Variable>);
    ValueID reachingDefinitionOnEdge(Calculator&, Calculator::Variable*, const VariableData&, Edge);
    void rewriteUses(Calculator&, Calculator::Variable*, const VariableData&);

    ValueID findLocalDefinition(const VariableData&, const RecordedUse&) const;
    static const Definition* findLastDefinitionBefore(const VariableData&, BlockID, uint32_t order);
    static void sortDefinitions(VariableData&);

    IRBuilder& m_builder;
    FunctionAnalyses& m_analyses;
    Vector<VariableData> m_variables;
    BitVector m_isDefinition;
    bool m_didRun { false };
    Vector<unsigned> m_constantAtTailStamp;
    Vector<ValueID> m_constantAtTail;
    Vector<unsigned> m_liveAtHeadStamp;
    Vector<unsigned> m_hasDefinitionStamp;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
