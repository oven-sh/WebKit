/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTCFG.h"
#include "AOTIR.h"
#include "AOTInsertionSet.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class TypeArena;

class IRBuilder {
    WTF_MAKE_NONCOPYABLE(IRBuilder);
public:
    IRBuilder(const AnalysisContext&, Function&, TypeArena&, FunctionAnalyses* = nullptr);
    ~IRBuilder();

    Function& function() { return m_function; }
    const AnalysisContext& context() const { return m_context; }
    TypeArena& types() { return m_types; }
    void setOrigin(Origin origin) { m_origin = origin; }
    Origin origin() const { return m_origin; }

    void setHandler(std::optional<Target>);
    const std::optional<Target>& handler() const { return m_handler; }
    void moveToEndOf(BlockID);
    BlockID currentBlock() const { return m_block; }

    BlockID addBlock(BlockFrequency = BlockFrequency::Normal);
    BlockID addHandlerBlock(BlockRole, ValueID subject, BlockFrequency = BlockFrequency::Rare);

    ValueID addParameter(BlockID, TypeID);
    void appendArgument(Edge, ValueID);
    uint32_t numberOfParametersForEdgeResults(BlockID);
    ValueID addParameter(BlockID, TypeID, uint32_t numberOfParametersForEdgeResults);
    void appendArgument(Edge, ValueID, uint32_t numberOfParametersForEdgeResults);
    ValueID addEdgeResult(Edge, uint32_t proof);
    void removeParameters(BlockID, const BitVector& parameterIndexes);

    static Value make(Opcode, std::initializer_list<ValueID> operands = { });
    static Terminator makeTerminator(TerminatorKind, std::initializer_list<ValueID> operands = { });
    ValueID addDetached(Value&&);
    ValueID append(Value&&);
    ValueID append(Opcode opcode, std::initializer_list<ValueID> operands) { return append(make(opcode, operands)); }
    ValueID appendWithType(Value&&, TypeID);

    ValueID appendUpcast(ValueID, TypeID);
    ValueID appendTypeTest(ValueID, TypeID testedType);
    ValueID appendTypeTest(ValueID, const Type& tested);
    ValueID appendAllGuards(std::span<const ValueID> guards);
    ValueID appendUndefined();
    ValueID appendBoolean(bool);
    ValueID appendInteger(int64_t);
    ValueID appendDouble(double);

    ValueID insertBefore(ProgramPoint, Value&&);
    bool hasInsertions() const { return !m_insertions.isEmpty(); }
    void applyInsertions();
    void eraseValues(BlockID, const BitVector& indexes);

    void emitJump(BlockID, Arguments&& = { });
    void emitBranch(ValueID condition, Target&& taken, Target&& notTaken);

    struct BranchBlocks {
        BlockID taken;
        BlockID notTaken;
    };
    BranchBlocks emitBranch(ValueID condition, BlockFrequency taken = BlockFrequency::Normal, BlockFrequency notTaken = BlockFrequency::Normal);

    struct ProvenBranch {
        BlockID taken;
        BlockID notTaken;
        ValueID provenOnTaken;
        ValueID provenOnNotTaken;
    };
    ProvenBranch emitBranchWithProof(ValueID test, uint32_t provenOperand = 0, BlockFrequency taken = BlockFrequency::Normal, BlockFrequency notTaken = BlockFrequency::Normal);
    // Blocks that are there already, in place of new ones that only jump to them.
    struct ProvenBranchTargets {
        BlockID enteredOnlyWhenTaken;
        std::optional<Target> whenNotTaken;
    };
    ProvenBranch emitBranchWithProof(ValueID test, uint32_t provenOperand, BlockFrequency taken, BlockFrequency notTaken, ProvenBranchTargets&&);
    void appendValidationOfTestedValue(ValueID test);

    void emitSwitch(ValueID, Vector<std::pair<int64_t, Target>>&& cases, Target&& otherwise);
    void emitSwitchString(ValueID, Vector<std::pair<StringID, Target>>&& cases, Target&& otherwise);
    void emitSwitchOnShape(ValueID, Vector<std::pair<LayoutSetID, Target>>&& cases, Target&& otherwise);
    void emitReturn(std::optional<ValueID>);
    void emitThrow(ValueID);
    void emitRethrow(ValueID exception);
    void emitUnreachable();
    void emitTerminator(Terminator&&);

    BlockID splitEdge(Edge);
    BlockID splitBlock(ProgramPoint);
    Vector<BlockID> splitBlock(BlockID, std::span<const uint32_t> ascendingIndexes);
    void redirectEdge(Edge, BlockID target, Arguments&&);
    void redirectEdge(Edge, Target&&);
    void convertBranchToJump(BlockID, uint32_t targetIndex);
    void demoteReturnCall(BlockID);
    void removeHandler(BlockID);
    size_t emptyUnreachableBlocks();
    void coverWithHandler(std::span<const BlockID>, const Target& handler);

    struct Expansion {
        BlockID continuation;
        Vector<ValueID, 1> results;
        std::optional<Target> handler;
    };
    Expansion beginExpansion(ProgramPoint);
    Expansion beginExpansion(ProgramPoint, std::span<const ValueID> resultReads);

    // For many expansions in one function, each of which would otherwise compute the use lists again.
    class DeferredResultReads {
        WTF_MAKE_NONCOPYABLE(DeferredResultReads);
    public:
        DeferredResultReads(IRBuilder&, std::span<const ValueID> producers);

        void finish();

    private:
        friend class IRBuilder;

        struct Read {
            ValueID producer;
            ValueID read;
        };

        std::span<const Read> readsOf(ValueID producer) const LIFETIME_BOUND;
        void replace(ValueID read, ValueID replacement);

        IRBuilder& m_builder;
        Vector<Read> m_reads;
        ValueSubstitution m_substitution;
        BitVector m_isReplaced;
    };
    Expansion beginExpansion(ProgramPoint, DeferredResultReads&);
    void endExpansion(const Expansion&, std::span<const ValueID> results);
    void endExpansion(const Expansion& expansion, ValueID result) { endExpansion(expansion, singleElementSpan(result)); }
    void mergeContinuation(const Expansion&, BlockID predecessor);

private:
    friend class RegionCloner;
    friend class SSARepair;

    ValueID addValue(Value&&);
    void prepare(Value&);
    ValueID appendPrepared(Value&&);
    Expansion cutForExpansion(ProgramPoint);

    Vector<Edge, 8> incomingEdgesOf(BlockID);
    uint32_t numberOfSharedExplicitParameters(BlockID);

    Terminator& openTerminator();
    void closeBlock();

    void didChangeValues();
    void didChangeControlFlow();
    void didAddDetachedValue(ValueID);
    void didListValues(BlockID, std::span<const ValueID>);
    void didEraseValues(BlockID, std::span<const ValueID>);
    void didChangeBlock(BlockID);
    void didChangeControlFlow(std::span<const BlockID> changedBlocks);

    const AnalysisContext m_context;
    Function& m_function;
    TypeArena& m_types;
    FunctionAnalyses* m_analyses;
    Origin m_origin;
    std::optional<Target> m_handler;
    BlockID m_block;
    InsertionSet m_insertions;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
