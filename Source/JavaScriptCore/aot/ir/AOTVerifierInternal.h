/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include "AOTSharedMemo.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

bool isValidPropertyKey(const PropertyKey&);

struct MethodDefinitionSite {
    ValueID classObject;
    uint32_t index { 0 };
    BlockID block;
    ValueID value;
};

struct ModuleEvaluationSite {
    ModuleID referrer;
    ModuleID target;
    BlockID block;
    ValueID value;
    bool isBare { false };
};

struct SlotInReceivers {
    LayoutID memberWithoutField;
    LayoutID memberWithRemovedField;
    bool isAlwaysFilled { true };
    bool mayHoldDeferredClosure { false };
};

struct SlotInReceiversEntry {
    LayoutSetID receivers;
    PropertyKeyID key;
    SlotInReceivers slot;
};

// What follows from a set of receivers and a key is found once for all the functions of a program that is verified whole.
using SlotsInReceivers = SharedMemo<SlotInReceiversEntry>;

class FunctionVerifier {
    WTF_MAKE_NONCOPYABLE(FunctionVerifier);
public:
    FunctionVerifier(const Program&, FunctionID, const Function&, IRForm, BuildContext&, const VerifierInputs&);
    ~FunctionVerifier();

    void shareAnswers(SlotsInReceivers& slots) { m_slotsInReceivers = &slots; }

    bool run();
    bool runStructure();
    bool runUnspecifiedValues();
    Vector<InternalError> takeErrors();

    const Program& program() const { return m_program; }
    FunctionID id() const { return m_id; }
    const Function& function() const { return m_function; }
    IRForm form() const { return m_form; }
    bool isOptimizing() const { return m_inputs.isOptimizing; }

    void fail(InternalErrorKind, BlockID = { }, ValueID = { }, uint32_t count = 0);
    bool hasFailed() const { return !m_errors.isEmpty(); }

    bool isLegalInForm(IRFormSet forms) const;

    bool isValid(ValueID id) const { return id && id.index() < m_function.values.size(); }
    bool isValid(BlockID id) const { return id && id.index() < m_function.blocks.size(); }
    bool isValid(TypeID id) const { return id && (TypeTable::isProvisional(id) || id.index() < m_program.types.size()); }
    bool isValid(LayoutSetID id) const { return id && (TypeTable::isProvisional(id) || id.index() < m_program.types.numberOfLayoutSets()); }
    bool isValid(CalleeSetID id) const { return id && (TypeTable::isProvisional(id) || id.index() < m_program.types.numberOfCalleeSets()); }
    bool isValid(PropertyKeyID id) const { return id && (TypeTable::isProvisional(id) || id.index() < m_program.types.numberOfPropertyKeys()); }
    bool isValid(StringID id) const { return id && (StringTable::isProvisional(id) || id.index() < m_program.strings.size()); }
    bool isValid(FunctionID id) const { return id && (ProvisionalFunctions::contains(id) || id.index() < m_program.functions.size()); }
    bool isValid(LayoutID id) const { return id && id.index() < m_program.layouts.size(); }
    bool isValid(VariableID id) const { return id && id.index() < m_program.variables.size(); }
    bool isValid(ModuleID id) const { return id && id.index() < m_program.modules.size(); }
    bool isValid(ModuleFailureID id) const { return id && id.index() < m_program.moduleFailures.size(); }
    bool isValid(TypeSiteID id) const { return id && id.index() < m_program.typeSites.size(); }
    bool isValid(EvalScopeID id) const { return id && id.index() < m_program.evalScopes.size(); }
    bool isValid(DataID id) const { return id && id.index() < m_program.data.size(); }
    bool isValid(ExpectedResultID id) const { return id && id.index() < m_program.expectedResults.size(); }
    bool isValid(AccessSiteID) const;
    bool isValid(AllocationSiteID) const;
    bool isValid(AllocationClassID) const;
    bool isValid(FieldAccessID) const;
    bool isValid(AllocationID) const;
    bool isValid(RegExpRecordID) const;
    bool isValid(GuardBitID) const;
    bool isValid(SiteCacheID id) const { return !!id; }
    bool isValid(GlobalBindingID id) const { return !!id; }
    bool isValidField(LayoutID, uint32_t field) const;
    bool isValidInlineFrame(uint32_t frame) const { return !frame || frame < m_function.inlineFrames.size(); }

private:
    void verifyStructure();
    void verifyBlockLists(BlockID, BitVector& isListed);
    void verifyInstructionIDs(BlockID, ValueID, const Instruction&);
    void verifyTargetIDs(BlockID, const Target&);
    void verifyTargetCount(BlockID, const Terminator&);
    void verifyInlineFrames();
    void verifyDetachedValues(const BitVector& isListed);
    void verifyBlockOrder();
    void verifyDefinitionsPrecedeUses();
    void verifyEntries();

    void verifyEdges();
    void verifyEdge(BlockID from, uint32_t targetIndex, const Target&);
    void verifyProofs(BlockID from, uint32_t targetIndex, const Target&);
    void verifyHandler(BlockID, const Block&);
    void verifyBlockRole(BlockID, const Block&);
    void verifyHandlerBlocksAreEnteredByHandlerEdges();

    void verifyTypes();
    void verifyOwnType(BlockID, ValueID, const Value&);
    void verifyParameterTypes(BlockID, const Block&);
    void verifyOperands(BlockID, ValueID, const Value&);
    void verifyOperands(BlockID, const Terminator&);
    void verifyOperandKind(BlockID, ValueID user, uint32_t operandIndex, ValueID operand, OperandKind, LayoutID layoutPayload);
    void verifyRequiredTypes(BlockID, ValueID, const Value&);
    void verifyEntryParameters();
    void verifyReturn(BlockID, const Terminator&);
    void verifyRepresentations(BlockID, ValueID, const Value&);
    void verifyCodeAfterNever(BlockID, const Block&);

    void verifyDominance();
    void verifyUsesOf(ValueID definition);
    void verifyHandlerArguments(BlockID, const Block&);
    void verifyLoopMarks();

    void verifyGuards();
    void verifyGuardOperand(BlockID, ValueID user, const Value&, ProofSet neededFromGuard, ValueID guard);

    void verifyUnspecifiedValues();

    void verifyOperations();
    void verifyOperation(BlockID, uint32_t indexInBlock, ValueID, const Value&);
    void verifyTerminatorOperation(BlockID, const Terminator&);
    void verifyFormRules(BlockID, const Block&);
    void verifyCatch(BlockID, uint32_t indexInBlock, ValueID, const Value&);
    void verifyReturnCall(BlockID, const Terminator&);
    void verifyTestAndBranch(BlockID, const Terminator&);
    void verifyCast(BlockID, ValueID, const Value&);
    void verifyObjectUnderConstruction(BlockID, ValueID, const Value&);
    void verifyMethodIndexes();
    void verifyClosureOperation(BlockID, ValueID, const Value&);
    void verifyDirectCall(BlockID, ValueID, const Value&);
    void verifyCallByValue(BlockID, ValueID, const Value&);
    void verifyArgumentsOperation(BlockID, ValueID, const Value&);
    void verifyClassOperation(BlockID, ValueID, const Value&);
    void verifyPrivateAccess(BlockID, ValueID, const Value&);
    void verifySuspension(BlockID, uint32_t indexInBlock, ValueID, const Value&);
    void verifyGeneratorStart();
    void verifyBuiltinOperation(BlockID, ValueID, const Value&);
    void verifyDirectEval(BlockID, ValueID, const Value&);
    void verifySlotOperation(BlockID, ValueID, const Value&);
    void verifyKeyedTail(BlockID, ValueID, const Value&, size_t numberOfKeys);
    void verifyElementOperation(BlockID, ValueID, const Value&);
    void verifyStringBuilderOperation(BlockID, ValueID, const Value&);
    void verifyResolvedCall(BlockID, ValueID, const Value&);
    void verifyModuleEvaluation(BlockID, ValueID, const Value&);
    void verifyBareEvaluations();
    void verifyRequestsOfEvaluations();
    void runSubsystemCheckers();

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    bool need(InternalErrorKind, BlockID, ValueID user, ValueID operand, const Type& needed);

    FunctionAnalyses& analyses();
    TypingContext typing();
    SlotInReceivers slotInReceivers(LayoutSetID, PropertyKeyID);

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    IRForm m_form;
    BuildContext& m_build;
    VerifierInputs m_inputs;
    std::optional<FunctionAnalyses> m_analyses;
    std::optional<ScratchTypeArena> m_types;
    SlotsInReceivers* m_slotsInReceivers { nullptr };
    Vector<InternalError> m_errors;
    bool m_hasValidStructure { false };
    bool m_hasFrameAllocation { false };
    bool m_hasTypedDataOperations { false };
    ValueID m_initialYield;
    Vector<ValueID, 4> m_yields;
    Vector<MethodDefinitionSite> m_methodDefinitions;
    Vector<ModuleEvaluationSite> m_moduleEvaluations;
};

class ProgramVerifier {
    WTF_MAKE_NONCOPYABLE(ProgramVerifier);
public:
    ProgramVerifier(const Program&, IRForm, BuildContext&, const VerifierInputs&);

    bool run();
    bool runIDs();

private:
    void fail(InternalError&&);
    bool hasFailed() const { return m_hasFailed; }

    bool isAsRead() const { return m_form == IRForm::AsRead && !m_inputs.isOptimizing; }
    bool hasUncheckedRecords() const { return isAsRead() || m_inputs.hasUncheckedRecords; }

    void verifyIDs();
    void verifyTypes();
    void verifyPropertyKeys();
    void verifyDeclaredPositions();

    void verifyLayouts();
    void verifyLayout(LayoutID, const Layout&);
    void verifyFields(LayoutID, const Layout&);
    void verifyLayoutFlow(LayoutID, const Layout&);
    void verifyClasses();
    bool isValidClass(LayoutID, const Layout&) const;
    void verifyVariables();

    void verifyModules();
    void verifyModule(ModuleID, const Module&);
    bool hasValidBody(const Module&) const;
    void verifyModuleGraph();
    void verifyModuleFailures();
    void verifyEntries();
    void verifyData();
    void verifyDataNode(DataID, const DataNode&);
    void verifyDataTypes();
    void verifySmallRecords();

    void verifyFunctionHeaders();
    void verifyFunctionHeader(FunctionID, const Function&);
    bool isValidCreatorRecord(FunctionID, const Function&) const;
    bool isValidDerivation(FunctionID, const Function&) const;
    void verifyDerivationChains();

    void verifyFunctionBodies();

    const Program& m_program;
    IRForm m_form;
    BuildContext& m_build;
    VerifierInputs m_inputs;
    bool m_hasFailed { false };
};

inline bool isAsRead(const FunctionVerifier& verifier)
{
    return verifier.form() == IRForm::AsRead && !verifier.isOptimizing();
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaVerifier.h"

#endif // ENABLE(AOT)
