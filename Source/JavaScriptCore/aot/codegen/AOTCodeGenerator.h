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
#include "AOTVector.h"
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class StringConstantNumbering;
class StubCatalog;

namespace Typed {

class ArrivalSiteNumbers;
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
class PromisedResults;
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
    const ArrivalSiteNumbers& arrivalSites;
    const PromisedResults& promisedResults;
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

// --aot-drop-guard, in the functions that --aot-disable-in names.
std::optional<DroppedGuard> droppedGuardIn(const RecordedOptions&, FunctionID);

inline B3::Origin b3OriginOf(ValueID id)
{
    return B3::Origin(std::bit_cast<DFG::Node*>(id ? static_cast<uintptr_t>(id.index()) + 1 : 0));
}

inline ValueID valueOfOrigin(B3::Origin origin)
{
    uintptr_t bits = std::bit_cast<uintptr_t>(origin.dfgOrigin());
    return bits ? ValueID(static_cast<uint32_t>(bits - 1)) : ValueID();
}

constexpr B3::FrequencyClass b3FrequencyClassOf(Expected<BlockFrequency> frequency)
{
    switch (frequency.predicted()) {
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
    bool checksProofs() const { return m_checksProofs; }
    bool validatesOrChecksProofs() const { return m_validates || m_checksProofs; }
    // Whether a call leaves alone the registers it says it keeps is a question about the back end. The answer takes about a kilobyte of code at each call.
    bool checksKeptRegisters() const { return m_checksKeptRegisters; }
    bool forcesGCSlowPaths() const { return m_forcesGCSlowPaths; }

    const Type& typeOf(ValueID id) const { return program().at(function().at(id).type); }
    // Of an operand: the form in which the operation that is being generated takes it.
    Representation representationOf(ValueID) const;

    // An operand in the form in which the operation that is being generated takes it.
    Operand get(ValueID) const;
    // In that form, whoever asks: an operation reads what another has read, and the other took it in a form of its own.
    Operand getIn(ValueID, Representation);
    Operand getAsDefined(ValueID) const;
    // For the one user of an operation that leaves its code to the user: from here on the user finds the operands of that operation in the forms in which it takes them.
    void takeOperandsOf(ValueID operationLeftToUser);
    void define(const ValueContext&, Operand);
    void defineTest(const ValueContext&, Condition);
    void defineResults(const ValueContext&, std::span<const Operand>);
    Operand resultAt(ValueID producer, uint32_t index) const;
    void defineParameter(ValueID parameter, Operand);

    LBasicBlock blockFor(BlockID) const;
    B3::FrequentedBlock edgeTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    void jumpTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });
    // For a terminator that leads to all of these blocks and to which the code of the first does for every one of them: that code goes here, and nothing enters the blocks.
    void generateInPlaceOfBlocks(std::span<const BlockID>);
    void passHandlerArguments(const Target& handler);

    void generateBranchCondition(const TerminatorContext&, ValueID condition, BranchTargets);
    void generateReturnedCall(const TerminatorContext&, ValueID call);

    // The block ends with the call and a test of its result for a type that the callee promises: the return stands for the test, and the test is made at the landing.
    bool calleeTestsResultOf(const ValueContext& call) const;
    // The landing is empty. It gives the two values that the meeting of the return and the landing takes from either, and jumps there.
    struct LandingOfMisfit {
        LBasicBlock landing { nullptr };
        LBasicBlock meeting { nullptr };
        LValue result { nullptr };
        LValue hasTestedType { nullptr };
    };
    void setLandingOfMisfit(ValueID call, const LandingOfMisfit&);
    std::optional<LandingOfMisfit> takeLandingOfMisfit(ValueID call);

    ValueID nullishCheckBehindFailedTest(BlockID) const;
    void generateNullishCheckBehindFailedTest(BlockID, ValueID check);

    template<typename T> T& emitter();

    // Under --aot-drop-guard every guard is tested as ever, and one that fails says so on stderr, once. The dropped one then goes on as if it had passed.
    struct GuardUnderTest {
        uint32_t ordinal { 0 };
        bool isDropped { false };
    };
    std::optional<GuardUnderTest> takeGuardUnderTest(const ValueContext&);
    BranchTargets targetsOf(const ValueContext&, const GuardUnderTest&);

    void fail(const OperationContext&, InternalErrorKind);
    void failAtLimit(const OperationContext&, LimitID, uint64_t needed);
    bool hasFailed() const { return m_hasFailed; }
    bool isValidatedWhereLookedAt(ValueID);
    bool hasTypeOnlyBehindTest(ValueID parameter);

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
    bool isPassingHandlerArguments() const { return m_isPassingHandlerArguments; }

private:
    struct Emitters;
    class OperationScope;
    struct ResultRange {
        uint32_t first;
        uint32_t count;
    };
    struct ConvertedOperand {
        ValueID id;
        Operand operand;
    };
    // A form of a value that was made where every later operation of the block finds it.
    struct SharedForm {
        ValueID source;
        Operand operand;
        uint32_t previousOfSource;
    };
    struct IncomingOperand {
        Operand operand;
        // None for a result that the terminator declares.
        ValueID source;
        // Of what the edge itself gives: its type is that on the edge.
        std::optional<EdgeResultPosition> result { };
    };
    struct ExemptStores {
        B3::AbstractHeapRepository::DecorationMark begin;
        B3::AbstractHeapRepository::DecorationMark end;
    };

    bool recordSitesWithoutCode();
    void createBlocks();
    void createPhis();
    void generateBlock(BlockID);
    void generateContentsOf(BlockID);
    bool isHandedOnByJumpAlone(BlockID, const Block&, ValueID);
    void generateValue(BlockID, const Block&, ValueID);
    void generateTerminator(BlockID, const Block&);
    void dispatch(const ValueContext&);
    void dispatch(const TerminatorContext&);
    void finishProcedure();

    void generate(const ValueContext&);
    void verifyElementGuard(const ValueContext&);
    void defineValue(ValueID, Operand);
    void passArguments(Edge, const Target&, std::span<const Operand> declaredResults);
    void passArgument(ValueID parameter, const IncomingOperand&);
    IncomingOperand takeIncomingOperand(Edge, uint32_t parameterIndex, std::span<const Operand>& declaredResults) const;

    template<typename Operation> void convertOperandsOf(const OperationContext&, const Operation&);
    enum class EarlierValue : bool { IsNotLookedFor, MayStandIn };
    Operand convert(const OperationContext&, ValueID source, Operand, const Type&, Representation wanted, EarlierValue = EarlierValue::MayStandIn);
    std::optional<Operand> onlyValueOf(const Type&, Representation);
    Operand makeConstantIn(const OperationContext&, ValueID constant, Representation);
    ValueID findSameValueMadeBefore(ValueID source, Representation wanted);
    ValueID onlyValueArrivingAt(ValueID parameter);
    std::optional<Operand> findSharedForm(ValueID source, Representation) const;
    void shareForm(ValueID source, Operand);
    void dropSharedFormsSince(size_t mark);
    void forgetSharedForms() { m_firstSharedFormInReach = m_sharedForms.size(); }
    void publishKnownProperties(ValueID, LValue);
    void publishKnownPropertiesOfConvertedValue(ValueID, Operand);
    void publishKnownProperties(ValueID, Operand, bool isConverted);
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
    bool m_valuesOutliveNoSuspension { false };
    const OperationContext* m_operation { nullptr };
    Vector<ConvertedOperand, 4> m_convertedOperands;
    Vector<SharedForm> m_sharedForms;
    // By the index of the source: one more than the index of its newest form, or zero.
    Vector<uint32_t> m_newestSharedFormOf;
    size_t m_firstSharedFormInReach { 0 };
    bool m_isPassingHandlerArguments { false };
    Vector<BlockID> m_blockOfParameter;
    // Takes what the operation that is being generated defines, in place of the table of values.
    Operand* m_definitionTaken { nullptr };
    Vector<std::optional<ResultRange>> m_resultRanges;
    Vector<Operand> m_results;
    Vector<LBasicBlock> m_blocks;
    Vector<ExemptStores, 2> m_exemptStores;
    std::unique_ptr<Emitters> m_emitters;
    std::optional<DroppedGuard> m_droppedGuard;
    ValueID m_callWithLandingOfMisfit;
    LandingOfMisfit m_landingOfMisfit;
    std::optional<BitVector> m_valuesValidatedWhereLookedAt;
    std::optional<BitVector> m_parametersTypedOnlyBehindTest;
    Vector<uint32_t> m_numberOfGuardsSeen;
    unsigned m_initializationDepth { 0 };
    bool m_validates { false };
    bool m_checksProofs { false };
    bool m_checksKeptRegisters { false };
    bool m_forcesGCSlowPaths { false };
    bool m_hasFailed { false };
    BitVector m_blocksGeneratedElsewhere;
};

#define AOT_DECLARE_EMITTER_ACCESSOR(Class) template<> Class& CodeGenerator::emitter<Class>();
FOR_EACH_AOT_EMITTER(AOT_DECLARE_EMITTER_ACCESSOR)
#undef AOT_DECLARE_EMITTER_ACCESSOR

ValueID valueOwnedByTerminator(const Terminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
