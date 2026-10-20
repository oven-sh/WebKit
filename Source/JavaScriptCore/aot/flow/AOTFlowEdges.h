/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTDominators.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowEdgesAssembler.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_FLOW_EDGE_RULE(v) \
    v(Undefined, addConstant) \
    v(Null, addConstant) \
    v(Absent, addConstant) \
    v(Boolean, addConstant) \
    v(Integer, addConstant) \
    v(Double, addConstant) \
    v(String, addConstant) \
    v(BigInt, addConstant) \
    v(KeyConstant, addConstant) \
    v(Builtin, addBuiltin) \
    v(ModuleBuiltin, addModuleBuiltin) \
    v(Data, addData) \
    v(NewClosure, addNewClosure) \
    v(Parameter, addParameter) \
    v(GetModuleVariable, addModuleVariableAccess) \
    v(PutModuleVariable, addModuleVariableAccess) \
    v(GetEnvironmentField, addEnvironmentFieldAccess) \
    v(PutEnvironmentField, addEnvironmentFieldAccess) \
    v(NewObject, addNewObject) \
    v(FinishObject, addSameValue) \
    v(NewArray, addNewArray) \
    v(AppendElement, addAppendElement) \
    v(AppendSpread, addAppendSpread) \
    v(AppendHole, addAppendHole) \
    v(NewRegExp, addNewInstance) \
    v(NewTemplateObject, addNewTemplateObject) \
    v(NewRuntimeError, addNewError) \
    v(NewSuppressedError, addNewError) \
    v(CreateUnmappedArguments, addCreateArguments) \
    v(CreateMappedArguments, addCreateArguments) \
    v(GetMappedArgument, addMappedArgumentAccess) \
    v(SetMappedArgument, addMappedArgumentAccess) \
    v(ScopeHasBinding, addPropertyAccess) \
    v(PropGet, addPropertyAccess) \
    v(PropSet, addPropertyAccess) \
    v(PropDefine, addPropertyAccess) \
    v(PropHas, addPropertyAccess) \
    v(PropDelete, addPropertyAccess) \
    v(GetOwnProperty, addPropertyAccess) \
    v(ElemGet, addElementAccess) \
    v(ElemSet, addElementAccess) \
    v(ElemHas, addElementAccess) \
    v(ElemDelete, addElementAccess) \
    v(ElemDefine, addElementAccess) \
    v(GetByValWithThis, addElementAccess) \
    v(DefineGetter, addAccessorDefinition) \
    v(DefineSetter, addAccessorDefinition) \
    v(SetFunctionName, addSetFunctionName) \
    v(SetLiteralPrototype, addSetLiteralPrototype) \
    v(CopyDataProperties, addCopyDataProperties) \
    v(GetSuperBase, addGetSuperBase) \
    v(SuperGet, addSuperAccess) \
    v(SuperSet, addSuperAccess) \
    v(PrivateGet, addPrivateAccess) \
    v(PrivateSet, addPrivateAccess) \
    v(PrivateDefine, addPrivateAccess) \
    v(PrivateHas, addPrivateAccess) \
    v(ToObject, addToObject) \
    v(ToThis, addToThis) \
    v(Upcast, addCast) \
    v(Claim, addCast) \
    v(Predict, addPredict) \
    v(CheckInitialized, addCast) \
    v(RequireObjectCoercible, addCast) \
    v(RequireObject, addCast) \
    v(Call, addCall) \
    v(CallList, addCall) \
    v(Construct, addConstruct) \
    v(ConstructList, addConstruct) \
    v(SuperConstruct, addSuperConstruct) \
    v(SuperConstructList, addSuperConstruct) \
    v(CallDirect, addCallDirect) \
    v(Results, addResults) \
    v(ResultAt, addResultAt) \
    v(CatchValue, addCaughtValue) \
    v(CatchException, addCaughtValue) \
    v(ResolveHeritage, addResolveHeritage) \
    v(NewClass, addNewClass) \
    v(GetClassPrototype, addGetClassPrototype) \
    v(DefineMethod, addDefineMethod) \
    v(FinishClass, addSameValue) \
    v(GetConstructPrototype, addGetConstructPrototype) \
    v(IteratorOpen, addIteratorOpen) \
    v(IteratorResume, addIteratorResume) \
    v(IteratorClose, addIteratorClose) \
    v(Await, addAwait) \
    v(Yield, addYield) \
    v(ModuleNamespace, addModuleOperation) \
    v(ImportModule, addModuleOperation) \
    v(RequireModule, addModuleOperation) \
    v(ImportDynamic, addModuleOperation) \
    v(RequireDynamic, addModuleOperation) \
    v(RequireBuiltinModule, addModuleOperation) \
    v(ResolveDynamic, addModuleOperation) \
    v(DirectEval, addDirectEval) \
    v(NativeBinding, addNativeBinding) \
    v(ArraySpeciesCreate, addArraySpeciesCreate) \
    v(ResumeGenerator, addGeneratorResumption) \
    v(StepGenerator, addGeneratorResumption) \
    v(StepArrayIterator, addArrayIteratorStep) \
    v(CreateIteratorResult, addCreateIteratorResult) \
    v(GetInternalField, addInternalFieldAccess) \
    v(PutInternalField, addInternalFieldAccess) \
    v(NewPromise, addNewInstance) \
    v(CreatePromise, addCreatePromise) \
    v(ResolvePromise, addResolvePromise) \
    v(RejectPromise, addRejectPromise) \
    v(GetContiguousIndex, addElementSlotAccess) \
    v(PutContiguousIndex, addElementSlotAccess) \
    v(PutDoubleIndex, addElementSlotAccess) \
    v(TryPutByValDirect, addElementSlotAccess) \
    v(TryPutIndex, addElementSlotAccess) \
    v(NewContiguousArray, addNewInstance) \
    v(NewDoubleArray, addNewInstance) \
    v(NewArrayLike, addNewInstance) \
    v(NewArrayWithCapacity, addNewInstance) \
    v(TruncateNewArray, addSameValue) \
    v(MapNext, addCollectionNext) \
    v(SetNext, addCollectionNext)

#define FOR_EACH_FLOW_TERMINATOR_RULE(v) \
    v(Jump, addPlainTerminator) \
    v(Branch, addPlainTerminator) \
    v(Switch, addPlainTerminator) \
    v(SwitchString, addPlainTerminator) \
    v(Unreachable, addPlainTerminator) \
    v(Return, addReturn) \
    v(ReturnCall, addReturnCall) \
    v(Throw, addThrow) \
    v(Rethrow, addThrow)

struct FlowNewSite {
    static constexpr uint32_t noSite = UINT32_MAX;

    uint32_t site { noSite };
    FlowLocalID local;

    bool hasSite() const { return site != noSite; }
};

struct FlowPropertyAccessShape {
    FlowAccessKind kind { FlowAccessKind::Load };
    OptionSet<FlowAccessFlag> flags;
    ValueID receiver;
    ValueID value;
    ValueID thisValue;
};

class FlowSummaryBuilder {
    WTF_MAKE_NONCOPYABLE(FlowSummaryBuilder);
public:
    FlowSummaryBuilder(const FlowSummaryInputs&, FunctionID);

    FlowSummary build();

private:
    template<Opcode> void add(ValueID, const Value&);
    template<TerminatorKind> void addTerminator(const Terminator&);

    void addLocals();
    void addEntry();
    void addRestSite();
    void addCoroutine();
    void addBlock(BlockID);
    void addValue(ValueID);
    void addTerminatorOf(const Block&);

    bool needsLocal(const Value&) const;
    FlowLocalID localOf(ValueID value) const { return value ? m_assembler.localOf(value) : FlowLocalID(); }
    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    bool mayHoldReference(ValueID) const;
    bool isInCycle() const { return m_cycles.isInCycle(m_block); }
    FlowReadUse readUseOf(ValueID) const;
    bool isTailCall(ValueID) const;
    Vector<FlowLocalID, 8> localsOf(std::span<const ValueID>) const;

    void seedTypeAsRead(ValueID);
    void seedPrimitives(FlowLocalID, uint16_t primitives);
    void seedInteger(FlowLocalID, int64_t);
    void filter(FlowLocalID from, FlowLocalID to, TypeID, FlowFilterKind = FlowFilterKind::Inside);
    FlowNewSite addSiteFor(ValueID, const FlowSiteDescription&, FlowLocalID prototype = { });
    FlowNewSite addInstanceSiteFor(ValueID, BuiltinClassID, uint16_t numberOfPositions = 0, bool isCreatedDense = false);
    void storeIntoSite(ValueID origin, const FlowExternalNode& part, FlowLocalID value);
    uint16_t numberOfPositionsFor(size_t numberOfElements) const;

    FlowLocalID thrownTarget();
    FlowLocalID caughtLocalOf(BlockID handler);
    void noteMayThrow(ValueID origin);

    void addFromSchema(ValueID, const Value&);
    void addResultFromSchema(ValueID, const Value&);
    void addOperandUse(ValueID origin, uint32_t operandIndex, ValueID operand, const OperandInfo&);
    void addOperandUsesFromSchema(ValueID, const Value&, const BitVector& coveredOperands);
    void addNaturalOperation(ValueID, const Value&);
    void addTerminatorOperandUses(const Terminator&);

    void addConstant(ValueID, const Value&);
    void addBuiltin(ValueID, const Value&);
    FlowLocalID calleeOfPrivateGlobalCall(const Value&);
    void addModuleBuiltin(ValueID, const Value&);
    void addData(ValueID, const Value&);
    FlowNewSite addDataNode(ValueID origin, const DataNode&, uint32_t ordinal);
    void addNewClosure(ValueID, const Value&);
    FlowClosureUse closureUseOf(ValueID) const;
    FlowClosureCapture closureCaptureOf(ValueID, const Value&) const;
    void addParameter(ValueID, const Value&);
    void addModuleVariableAccess(ValueID, const Value&);
    void addEnvironmentFieldAccess(ValueID, const Value&);

    void addNewObject(ValueID, const Value&);
    void addNewArray(ValueID, const Value&);
    void addAppendElement(ValueID, const Value&);
    void addAppendSpread(ValueID, const Value&);
    void addAppendHole(ValueID, const Value&);
    void addNewInstance(ValueID, const Value&);
    void addCreatePromise(ValueID, const Value&);
    void addNewTemplateObject(ValueID, const Value&);
    void addNewError(ValueID, const Value&);
    void addCreateArguments(ValueID, const Value&);
    void addMappedArgumentAccess(ValueID, const Value&);
    void addCreateIteratorResult(ValueID, const Value&);
    void addArraySpeciesCreate(ValueID, const Value&);
    void addToObject(ValueID, const Value&);
    void addToThis(ValueID, const Value&);
    void addWrapperSeed(ValueID, FlowLocalID from, TypeID primitiveType, BuiltinClassID wrapperClass, uint32_t ordinal);

    FlowPropertyAccessShape propertyAccessShapeOf(const Value&) const;
    void addPropertyAccess(ValueID, const Value&);
    void addNamedAccess(ValueID origin, const FlowPropertyAccessShape&, PropertyKeyID);
    void addComputedAccess(ValueID origin, const FlowPropertyAccessShape&, ValueID key);
    void addElementAccess(ValueID, const Value&);
    void addAccessorDefinition(ValueID, const Value&);
    void addSetFunctionName(ValueID, const Value&);
    void addSetLiteralPrototype(ValueID, const Value&);
    void addCopyDataProperties(ValueID, const Value&);
    void addGetSuperBase(ValueID, const Value&);
    void addSuperAccess(ValueID, const Value&);
    void addPrivateAccess(ValueID, const Value&);
    void addInternalFieldAccess(ValueID, const Value&);
    void addElementSlotAccess(ValueID, const Value&);
    void addCollectionNext(ValueID, const Value&);

    void addCall(ValueID, const Value&);
    void addConstruct(ValueID, const Value&);
    void addSuperConstruct(ValueID, const Value&);
    void addCallStatement(ValueID, FlowCall&&, std::span<const ValueID> arguments, ValueID list);
    void addCallDirect(ValueID, const Value&);
    void addResults(ValueID, const Value&);
    void addResultAt(ValueID, const Value&);
    Vector<FlowLocalID, 4>& resultLocalsOf(ValueID producer, uint32_t numberOfResults);
    void addResolveHeritage(ValueID, const Value&);
    void addNewClass(ValueID, const Value&);
    void addGetClassPrototype(ValueID, const Value&);
    void addDefineMethod(ValueID, const Value&);
    void addGetConstructPrototype(ValueID, const Value&);
    void addPrototypePropertyRead(ValueID origin, ValueID holder, FlowLocalID result);

    void addBlockParameters(BlockID);
    void addIncomingValue(Edge, uint32_t parameterIndex, ValueID parameter);
    void addEdgeResult(const EdgeResultPosition&, ValueID parameter);
    FlowLocalID addOperandBoundedByValues(const EdgeResult&);
    void addSameValue(ValueID, const Value&);
    void addCast(ValueID, const Value&);
    void addPredict(ValueID, const Value&);
    void addCaughtValue(ValueID, const Value&);
    void addPlainTerminator(const Terminator&);
    void addReturn(const Terminator&);
    void addReturnCall(const Terminator&);
    void addThrow(const Terminator&);

    void addIteratorOpen(ValueID, const Value&);
    void addIteratorResume(ValueID, const Value&);
    void addIteratorClose(ValueID, const Value&);
    void addAwait(ValueID, const Value&);
    void addYield(ValueID, const Value&);
    void addGeneratorResumption(ValueID, const Value&);
    void addArrayIteratorStep(ValueID, const Value&);
    void addResolvePromise(ValueID, const Value&);
    void addRejectPromise(ValueID, const Value&);

    void addModuleOperation(ValueID, const Value&);
    void addDirectEval(ValueID, const Value&);
    void addNativeBinding(ValueID, const Value&);
    void addSyntheticExportCaptures();
    void addUsesOfModules();
    FlowLocalID addSyntheticExportSource(const Module&);
    void addCapturedExport(FlowLocalID source, const Module&, const SyntheticExport&);

    const Program& m_program;
    BuildContext& m_build;
    bool m_hasModulesWithoutSideEffects;
    FunctionID m_id;
    const Function& m_function;
    CFG m_cfg;
    BlockCycles m_cycles;
    UseLists m_uses;
    FlowSummaryAssembler m_assembler;
    BlockID m_block;
    ValueID m_value;
    Vector<FlowLocalID> m_caughtLocals;
    BitVector m_localsWithRuntimeError;
    HashMap<ValueID, Vector<FlowLocalID, 4>> m_resultLocals;

    // `x.k === c` or cases of `switch (x.k)`, on the way into a block: one of the types and ranges, or with Outside none of them.
    struct FieldTest {
        ValueID object;
        PropertyKeyID key;
        Vector<TypeID, 1> types;
        Vector<Range, 1> ranges;
        FlowFilterKind kind { FlowFilterKind::Inside };
    };

    const Value* findFieldRead(ValueID) const;
    std::optional<FieldTest> fieldTestOfBranch(const Terminator&, bool isTaken) const;
    std::optional<FieldTest> fieldTestOfSwitch(const Terminator&, uint32_t targetIndex) const;
    std::optional<FieldTest> fieldTestOn(Edge) const;
    std::optional<FieldTest> fieldTestOnEntryOf(BlockID) const;
    void collectFieldTests();
    FlowLocalID addFieldFilter(FlowLocalID from, const FieldTest&);
    FlowLocalID narrowByFieldTests(ValueID);
    FlowLocalID localAtUse(ValueID);

    bool m_hasCollectedFieldTests { false };
    HashMap<BlockID, FieldTest> m_fieldTests;
    std::unique_ptr<Dominators> m_dominators;
    std::unique_ptr<ValueIdentities> m_identities;

    bool isLengthOf(ValueID array, ValueID value) const;
    bool findsKeyWithinLength(Edge, ValueID array, ValueID key) const;
    bool isWithinLengthOf(ValueID receiver, ValueID key);
    Vector<BlockID> m_nearestBlockWithFieldTests;
    HashMap<ValueID, BlockID> m_blocksOfNarrowedValues;
    HashMap<std::pair<ValueID, BlockID>, FlowLocalID> m_localsAtUses;
};

template<Opcode opcode>
void FlowSummaryBuilder::add(ValueID id, const Value& value)
{
    if constexpr (isLegalIn(opcode, IRForm::AsRead) && removedByOf(opcode) != PassID::TypeValues)
        addFromSchema(id, value);
    else
        RELEASE_ASSERT_NOT_REACHED();
}

template<TerminatorKind>
void FlowSummaryBuilder::addTerminator(const Terminator&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

#define DEFINE_FLOW_EDGE_RULE(opcode, rule) \
    template<> inline void FlowSummaryBuilder::add<Opcode::opcode>(ValueID id, const Value& value) { rule(id, value); }
FOR_EACH_FLOW_EDGE_RULE(DEFINE_FLOW_EDGE_RULE)
#undef DEFINE_FLOW_EDGE_RULE

#define DEFINE_FLOW_TERMINATOR_RULE(kind, rule) \
    template<> inline void FlowSummaryBuilder::addTerminator<TerminatorKind::kind>(const Terminator& terminator) { rule(terminator); }
FOR_EACH_FLOW_TERMINATOR_RULE(DEFINE_FLOW_TERMINATOR_RULE)
#undef DEFINE_FLOW_TERMINATOR_RULE

#define CHECK_FLOW_EDGE_RULE(opcode, rule) \
    static_assert(isLegalIn(Opcode::opcode, IRForm::AsRead), "An operation with a rule of its own is legal in the form flow reads.");
FOR_EACH_FLOW_EDGE_RULE(CHECK_FLOW_EDGE_RULE)
#undef CHECK_FLOW_EDGE_RULE

#define CHECK_FLOW_TERMINATOR_RULE(kind, rule) + 1
#define COUNT_TERMINATOR_KIND_AS_READ(kind) + (isLegalIn(TerminatorKind::kind, IRForm::AsRead) ? 1 : 0)
static_assert((0 FOR_EACH_FLOW_TERMINATOR_RULE(CHECK_FLOW_TERMINATOR_RULE)) == (0 FOR_EACH_AOT_TERMINATOR_KIND(COUNT_TERMINATOR_KIND_AS_READ)), "Every terminator of the form flow reads has a rule.");
#undef COUNT_TERMINATOR_KIND_AS_READ
#undef CHECK_FLOW_TERMINATOR_RULE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
