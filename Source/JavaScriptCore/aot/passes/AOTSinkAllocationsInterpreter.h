/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTSinkAllocationsInternal.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class Execution : uint8_t { Kept, Interpreted };

enum class FieldRead : uint8_t { Unknown, Value, ValueOrUndefined, Undefined, HeldObject };

struct LateFieldStore {
    VirtualObjectID holder;
    uint32_t field { 0 };
    VirtualObjectID held;
    ValueID value;
};

struct ConsumedSource {
    VirtualObjectID source;
    VirtualObjectID join;
};

struct CopiedField {
    PropertyKeyID key;
    uint32_t sourceField { 0 };
    uint32_t targetField { 0 };
};

// Where the values of an arguments object are: in the frame of this function from one argument on, or in the parameters and the rest list.
struct ArgumentsParts {
    std::optional<uint32_t> firstFrameArgument;
    std::optional<uint32_t> numberOfPassedParameters;
    bool restListIsEmpty { false };
};

struct EnteredEdge {
    const OutgoingEdge* edge { nullptr };
    const Terminator* terminator { nullptr };
    Vector<AliasEntry, 4> newAliases;
    Vector<ObjectEntry, 1> newJoinStates;
    Vector<ConsumedSource, 2> consumedSources;
};

class ObjectInterpreter {
    WTF_MAKE_NONCOPYABLE(ObjectInterpreter);
public:
    ObjectInterpreter(AllocationSinking&, SinkingMode, SinkingState&, ObjectValueMap* = nullptr);

    SinkingMode mode() const { return m_mode; }
    bool isRewriting() const { return m_mode == SinkingMode::Rewrite; }
    SinkingState& state() { return m_state; }
    const SinkingState& state() const { return m_state; }
    ObjectValueMap& values() { return *m_values; }

    Execution execute(ValueID);
    void executeTerminator(const Terminator&);
    void enterEdge(const OutgoingEdge&, const Terminator*, const SinkingState* headOfTarget);
    bool isDeadEdge(const Terminator&, uint32_t targetIndex) const;
    bool hasInterpretedTest(const Terminator&) const;

    VirtualObjectID objectOf(ValueID) const;
    VirtualObjectID guardedObjectOf(ValueID) const;
    VirtualObjectID destructuredObjectOf(ValueID) const;

    void requireCell(ValueID, const BuildCause&);
    void build(VirtualObjectID, const BuildCause&);

    ValueID resolve(ValueID);
    ValueID cellOf(VirtualObjectID);
    ValueID resolveGuard(ValueID);
    void resolveOperands(Value& detachedValue);
    void resolveOperands(Terminator& detachedTerminator);
    ValueID tagOf(VirtualObjectID, BlockID target);
    ValueID valueOfField(VirtualObjectID, uint32_t field);
    ValueID emitTagTest(VirtualObjectID, uint32_t tagBits);

    void returnParts(const Terminator&);

private:
    const Program& program() const { return m_sinking.program(); }
    const Function& function() const { return m_sinking.function(); }
    RewriteCursor& cursor() { return m_sinking.cursor(); }
    ObjectState* stateOf(VirtualObjectID object) { return m_state.find(object); }
    const ObjectState* stateOf(VirtualObjectID object) const { return m_state.find(object); }
    const VirtualShape& shapeOf(VirtualObjectID object, const ObjectVariant& variant) const LIFETIME_BOUND { return m_sinking.familyOf(object).at(variant.shape); }
    const VirtualShape& singleShapeOf(VirtualObjectID object) const LIFETIME_BOUND { return shapeOf(object, stateOf(object)->variants[0]); }

    Execution executeAllocation(ValueID);
    Execution executeCast(ValueID);
    Execution executeGuards(ValueID);
    Execution executeRuntimeValidation(ValueID);
    Execution executeOther(ValueID);
    void splitThrowingVariants(ValueID cast, VirtualObjectID, uint32_t throwingTagBits);
    void requireCellsOfOperands(ValueID);
    void forgetFieldsWrittenBy(ValueID);
    BuildCause causeOf(ValueID user, uint32_t operandIndex) const;
    BuildCause causeOf(const Terminator&, uint32_t operandIndex) const;
    void keep(ValueID);
    void bind(ValueID, VirtualObjectID, AliasKind = AliasKind::Object);
    void stateDidChange(VirtualObjectID);

    Execution executeRead(ValueID);
    Execution executeStore(ValueID);
    Execution executeDynamicElementRead(ValueID read, VirtualObjectID, ValueID index);
    std::optional<uint32_t> fieldFor(VirtualObjectID, const VirtualField&);
    FieldRead classifyRead(VirtualObjectID, const VirtualFieldAccess&, std::optional<uint32_t> field) const;
    bool canStore(VirtualObjectID, const VirtualFieldAccess&, uint32_t field, ValueID stored) const;
    bool canHold(VirtualObjectID holder, uint32_t field, VirtualObjectID held) const;
    bool fitsRead(ValueID read, VirtualObjectID, std::optional<uint32_t> field, FieldRead) const;
    bool fitsType(VirtualObjectID, TypeID) const;
    bool fitsTypeWithoutCasts(VirtualObjectID, TypeID) const;
    void assumeTypeOfAlias(VirtualObjectID, TypeID);
    void storeField(VirtualObjectID, uint32_t field, ValueID stored);
    void appendElement(VirtualObjectID, ValueID stored);
    void replaceRead(ValueID read, ValueID replacement);
    ValueID emitUndefined();

    Execution executeTest(ValueID);
    std::optional<TestEntry> evaluateTestOn(ValueID test) const;
    std::optional<TestEntry> evaluateComparison(ValueID test) const;
    void replaceTest(ValueID test, const TestEntry&);
    void executeReturn(const Terminator&);
    void buildBeforeBranch(const Terminator&);
    void narrow(VirtualObjectID, uint32_t keptTagBits);
    void narrowOnEdge(const Terminator&, uint32_t targetIndex);
    std::optional<uint32_t> tagBitsOfShapeCase(const Terminator&, VirtualObjectID, uint32_t targetIndex) const;

    void bindParameters(const OutgoingEdge&);
    void bindParameter(ValueID parameter, const IncomingBinding&);
    void enterJoin(VirtualObjectID join, const IncomingBinding&);
    void commitBindings();
    void abandonJoinsOfLiveSources();
    void applyDecisions(const OutgoingEdge&);
    void makeFieldsOfCellsValid(const OutgoingEdge&, const SinkingState* headOfTarget);
    void dropDeadEntries(BlockID target, const SinkingState* headOfTarget);
    void willEmitOnEdge(const OutgoingEdge&);

    Execution executeListUse(ValueID);
    Execution executeAppendElements(ValueID);
    Execution executeCopyDataProperties(ValueID);
    Execution executeCallWithList(ValueID);
    Execution executeDestructuring(ValueID);
    Execution executeResultRead(ValueID);
    Execution abandonDestructuredRead(ValueID read, VirtualObjectID);
    const VirtualObjectDescription* singleDescriptionOf(VirtualObjectID) const;
    std::optional<uint32_t> findListedElement(VirtualObjectID, uint32_t position) const;
    bool canAppendListedElements(VirtualObjectID target, VirtualObjectID source);
    std::optional<Vector<CopiedField, 8>> collectCopiedFields(ValueID copy, VirtualObjectID source) const;
    bool tryResolveTargetFields(VirtualObjectID target, const VirtualObjectDescription& source, std::span<CopiedField>);
    void requireCellOfHeldObject(VirtualObjectID holder, uint32_t field, const BuildCause&);
    bool hasListedElements(VirtualObjectID) const;
    Vector<ValueID, 8> listedElementsOf(VirtualObjectID);
    void requireCellsOfListedElements(VirtualObjectID, const BuildCause&);

    Execution readArgumentList(ValueID read, VirtualObjectID, const VirtualFieldAccess&);
    Execution readArguments(ValueID read, VirtualObjectID, const VirtualFieldAccess&);
    Execution storeArgument(ValueID store, VirtualObjectID, const VirtualFieldAccess&);
    Execution forwardArgumentList(ValueID call, VirtualObjectID);
    Execution executeCallOnArguments(ValueID call);
    Execution forwardArguments(ValueID call, const ForwardedCallPlan&);
    Execution sliceArguments(ValueID call);
    Execution listArguments(ValueID list);
    bool tryCopyArguments(ValueID copy, VirtualObjectID arguments, uint32_t first, const BuildCause&);
    Execution passListedElements(ValueID call);
    ArgumentsParts partsOfArguments(VirtualObjectID arguments) const;
    std::optional<Vector<uint32_t, 8>> requireCellsOfNamedArguments(VirtualObjectID arguments, const ArgumentsParts&, uint32_t first, const BuildCause&);
    Vector<ValueID, 8> namedArgumentsOf(VirtualObjectID arguments, const ArgumentsParts&, std::span<const uint32_t> parameterFields, uint32_t first);
    bool requireCellOfRestList(VirtualObjectID arguments, const BuildCause&);
    Execution destructureArgumentList(ValueID destructuring, VirtualObjectID);
    std::optional<bool> isArgumentPassed(VirtualObjectID arguments, uint32_t index) const;

    Execution executeBuiltinResult(ValueID call, VirtualObjectID);
    uint32_t prefixLengthNeededBy(VirtualObjectID) const;
    ValueID emitPresenceTag(ValueID isPresent, uint8_t tagBitWhenAbsent);

    Vector<VirtualObjectID, 4> collectVirtualMembers(VirtualObjectID) const;
    void buildMembers(std::span<const VirtualObjectID>, const BuildCause&);
    enum class AbsentResult : bool { IsExcluded, IsPossible };
    ValueID emitAllocation(VirtualObjectID, const VirtualShape&, Vector<LateFieldStore, 2>&, AbsentResult = AbsentResult::IsExcluded);
    bool hasBothAllocationOutcomes(VirtualObjectID) const;
    ValueID emitValueOfVariant(VirtualObjectID, const ObjectVariant&, Vector<LateFieldStore, 2>&);
    void buildByTag(VirtualObjectID, Vector<LateFieldStore, 2>&);
    void emitLateStores(std::span<const LateFieldStore>);
    void didBuild(VirtualObjectID, const BuildCause&);
    BlockFrequency frequencyOfBuild() const;

    void returnVariant(uint32_t resultVariant, VirtualObjectID, const VirtualShape*, ValueID cell);
    uint32_t findResultVariant(VirtualObjectID, const VirtualShape&) const;

    AllocationSinking& m_sinking;
    const SinkingMode m_mode;
    SinkingState& m_state;
    ObjectValueMap* m_values;
    EnteredEdge m_enteredEdge;
};

// CallList, ConstructList or SuperConstructList becomes the call that names the elements of its list.

inline constexpr OpcodeSet interpretedReads {
    Opcode::GetEnvironmentField, Opcode::GetMappedArgument, Opcode::PropGet, Opcode::ElemGet, Opcode::PrivateGet, Opcode::GetSlot,
    Opcode::GetSlotOrUndefined, Opcode::GetArrayLength, Opcode::GetArrayElement, Opcode::GetCapture, Opcode::GetStringLength,
};

inline constexpr OpcodeSet interpretedStores {
    Opcode::PutEnvironmentField, Opcode::AppendElement, Opcode::AppendHole, Opcode::SetMappedArgument, Opcode::PropSet, Opcode::PropDefine,
    Opcode::ElemSet, Opcode::ElemDefine, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::PutArrayElement,
};

inline constexpr OpcodeSet interpretedCasts {
    Opcode::FinishObject, Opcode::Upcast, Opcode::Claim, Opcode::Predict, Opcode::AssumeType, Opcode::CheckInitialized,
    Opcode::RequireObjectCoercible, Opcode::RequireObject,
};

inline constexpr OpcodeSet interpretedListUses {
    Opcode::AppendSpread, Opcode::CopyDataProperties, Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList, Opcode::AppendElements,
    Opcode::DestructureIterable, Opcode::ResultAt,
};

inline constexpr OpcodeSet interpretedAllocations {
    Opcode::Data, Opcode::NewClosure, Opcode::NewObject, Opcode::NewArray, Opcode::NewRegExp, Opcode::CreateUnmappedArguments,
    Opcode::CreateMappedArguments, Opcode::NewPromise, Opcode::CreateRest, Opcode::NewGenerator, Opcode::CloneAs,
    Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::CallBuiltin,
};

static_assert((interpretedReads | opcodesInterpretedAs(Interpretation::Read)) == interpretedReads);
static_assert((interpretedStores | opcodesInterpretedAs(Interpretation::Store)) == interpretedStores);
static_assert((interpretedCasts | opcodesInterpretedAs(Interpretation::Cast)) == interpretedCasts);
static_assert((interpretedListUses | opcodesInterpretedAs(Interpretation::List)) == interpretedListUses);
static_assert((interpretedAllocations | opcodesInterpretedAs(Interpretation::Allocation)) == interpretedAllocations);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
