/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTEmitterList.h"
#include "AOTHeaps.h"
#include "AOTIR.h"
#include "AOTInstanceLayout.h"
#include "AOTOperand.h"
#include "AOTOperationContext.h"
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class StringConstantNumbering;
class StubCatalog;

namespace Typed {

class BranchCounterNumbers;
class CallingConventions;
class DerivedFunctions;
class EffectSummaries;
class FlowResults;
class FrameAllocation;
class FrameMaps;
class FunctionAnalyses;
class Heat;
class LiteralStringTable;
class Placement;
class StackChecks;
class TypeArena;
struct CodeRecords;

struct CodeGenerationInputs {
    const Program& program;
    const Placement& placement;
    const FlowResults& flow;
    const Heat& heat;
    const EffectSummaries& effects;
    const DerivedFunctions& derivedFunctions;
    const CallingConventions& conventions;
    const StackChecks& stackChecks;
    const FrameAllocation& frameAllocation;
    const LiteralStringTable& literalStrings;
    const StubCatalog& stubCatalog;
    const StringConstantNumbering& stringConstants;
    InstanceSizes instanceSizes;
    const BranchCounterNumbers& branchCounters;
};

struct FunctionInputs {
    BuildContext& build;
    TypeArena& types;
    const CodeGenerationInputs& program;
    FunctionID id;
    const Function& function;
    FunctionAnalyses& analyses;
    const FrameMaps& frameMaps;
};

struct PinnedValues {
    LValue instance { nullptr };
    LValue instancePrefix { nullptr };
    LValue statics { nullptr };
    LValue numberTag { nullptr };
    LValue notCellMask { nullptr };
    LValue cellCageBase { nullptr };
};

enum class GenerationResult : uint8_t { Failed, Generated };

inline B3::Origin b3OriginOf(ValueID id)
{
    return B3::Origin(std::bit_cast<DFG::Node*>(id ? static_cast<uintptr_t>(id.index()) + 1 : 0));
}

inline ValueID valueOfOrigin(B3::Origin origin)
{
    uintptr_t bits = std::bit_cast<uintptr_t>(origin.dfgOrigin());
    return bits ? ValueID(static_cast<uint32_t>(bits - 1)) : ValueID();
}

constexpr B3::FrequencyClass b3FrequencyClassOf(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return B3::FrequencyClass::Normal;
    case BlockFrequency::Unlikely:
        return B3::FrequencyClass::Unlikely;
    case BlockFrequency::Rare:
        return B3::FrequencyClass::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

class CodeGenerator {
    WTF_MAKE_NONCOPYABLE(CodeGenerator);
public:
    CodeGenerator(const FunctionInputs&, B3::Procedure&, CodeRecords&);
    ~CodeGenerator();

    GenerationResult run();

    const FunctionInputs& inputs() const LIFETIME_BOUND { return m_inputs; }
    BuildContext& build() { return m_inputs.build; }
    const Program& program() const { return m_inputs.program.program; }
    const Function& function() const { return m_inputs.function; }
    FunctionAnalyses& analyses() { return m_inputs.analyses; }
    B3::Procedure& procedure() { return m_proc; }
    AOTHeaps& heaps() { return m_heaps; }
    Output& out() LIFETIME_BOUND { return m_out; }
    PinnedValues& pinned() LIFETIME_BOUND { return m_pinned; }
    CodeRecords& records() { return m_records; }
    bool validates() const { return m_validates; }
    bool forcesGCSlowPaths() const { return m_forcesGCSlowPaths; }

    const Type& typeOf(ValueID id) const { return program().at(function().at(id).type); }
    Representation representationOf(ValueID id) const { return function().at(id).representation; }

    Operand get(ValueID) const;
    void define(const ValueContext&, Operand);
    void defineTest(const ValueContext&, Condition);
    void defineResults(const ValueContext&, std::span<const Operand>);
    Operand resultAt(ValueID producer, uint32_t index) const;
    void defineParameter(ValueID parameter, Operand);

    LBasicBlock blockFor(BlockID) const;
    B3::FrequentedBlock edgeTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    void jumpTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    void passHandlerArguments(const Target& handler);

    void generateBranchCondition(const TerminatorContext&, ValueID condition, BranchTargets);
    void generateReturnedCall(const TerminatorContext&, ValueID call);

    template<typename T> T& emitter();

    bool shouldDropGuard(const ValueContext&);

    void fail(const OperationContext&, InternalErrorKind);
    void failAtLimit(const OperationContext&, LimitID, uint64_t needed);
    bool hasFailed() const { return m_hasFailed; }
    bool mayBeUnspecified(ValueID);

    class InitializationScope {
        WTF_MAKE_NONCOPYABLE(InitializationScope);
    public:
        explicit InitializationScope(CodeGenerator&);
        ~InitializationScope();

    private:
        CodeGenerator& m_generator;
        B3::AbstractHeapRepository::DecorationMark m_begin;
    };

    bool isInitializingCell() const { return m_initializationDepth; }

private:
    struct Emitters;
    struct ResultRange {
        uint32_t first;
        uint32_t count;
    };
    struct ExemptStores {
        B3::AbstractHeapRepository::DecorationMark begin;
        B3::AbstractHeapRepository::DecorationMark end;
    };

    bool recordSitesWithoutCode();
    void createBlocks();
    void createPhis();
    void generateBlock(BlockID);
    void generateValue(BlockID, const Block&, ValueID);
    void generateTerminator(BlockID, const Block&);
    void dispatch(const ValueContext&);
    void dispatch(const TerminatorContext&);
    void finishProcedure();

    void generate(const ValueContext&);
    void defineValue(ValueID, Operand);
    void passArguments(Edge, const Target&, std::span<const Operand> declaredResults);
    void passArgument(ValueID parameter, Operand);
    void publishKnownProperties(ValueID, LValue);
    void publishInductionVariables();
    void checkInlineStores(const OperationContext&, const B3::AbstractHeapRepository::DecorationMark&);
    bool currentBlockIsOpen() const;

    Subject subjectOf(const OperationContext&) const;
    void failAtLimit(LimitID, uint64_t needed, const Subject&);

    const FunctionInputs m_inputs;
    B3::Procedure& m_proc;
    AOTHeaps& m_heaps;
    CodeRecords& m_records;
    Output m_out;
    PinnedValues m_pinned;
    Vector<LValue> m_values;
    Vector<std::optional<ResultRange>> m_resultRanges;
    Vector<Operand> m_results;
    Vector<LBasicBlock> m_blocks;
    Vector<ExemptStores, 2> m_exemptStores;
    std::unique_ptr<Emitters> m_emitters;
    std::optional<DroppedGuard> m_droppedGuard;
    std::optional<BitVector> m_unspecifiedValues;
    uint32_t m_numberOfGuardsSeen { 0 };
    unsigned m_initializationDepth { 0 };
    bool m_validates { false };
    bool m_forcesGCSlowPaths { false };
    bool m_hasFailed { false };
};

#define AOT_DECLARE_EMITTER_ACCESSOR(Class) template<> Class& CodeGenerator::emitter<Class>();
FOR_EACH_AOT_EMITTER(AOT_DECLARE_EMITTER_ACCESSOR)
#undef AOT_DECLARE_EMITTER_ACCESSOR

ValueID valueOwnedByTerminator(const Terminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
