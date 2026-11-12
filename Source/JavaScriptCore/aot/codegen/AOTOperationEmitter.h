/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTIRProgram.h"
#include "AOTOperationData.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeTable.h"
#include "AOTStubKey.h"
#include <initializer_list>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class BuiltinTable;
struct CompileOptions;

namespace Typed {

class InlineCall;
struct TypeTestPlan;

struct CallOptions {
    std::optional<uint32_t> immediate { };
    std::optional<NumberedImmediate> numberedImmediate { };
    std::optional<Origin> origin { };
    std::optional<Origin> readBeforeCall { };
    std::optional<Machine::PureFunction> pureFunction { };
    std::optional<ThrowSite> throwSite { };
    BuiltinID calledBuiltin { };
    FunctionID constructedCallee { };
    bool constructs { false };
    bool callerHasLeft { false };
    bool isOnRarePath { false };
    bool accessesCollectorState { false };
    bool writesArgumentBuffer { false };
    bool runsCodeOfHost { false };
    bool capturesStackTrace { false };
    bool followsThrow { false };
    bool convertsNoCells { false };
    bool callerSuspendsAtReturn { false };
    bool callerDelegates { false };
    bool rejectsOnThrow { false };
    bool returnsWord32 { false };
};

struct StubCallResult {
    LValue first { nullptr };
    LValue second { nullptr };
};

class OperationEmitter : public PrimitiveEmitter {
protected:
    explicit OperationEmitter(CodeGenerator& generator)
        : OperationEmitter(generator, generator.inputs().program, generator.code().unit())
    {
    }

    CodeGenerator& generator() { return m_generator; }
    const CodeGenerationInputs& inputs() const { return m_generator.inputs().program; }
    BuildContext& build() { return m_generator.build(); }
    TypeArena& types() { return m_generator.inputs().types; }
    template<typename T> T& emitter() { return m_generator.emitter<T>(); }

    const Operation& operationAt(ValueID id) const LIFETIME_BOUND { return m_code.at(id); }
    Opcode opcodeOf(ValueID id) const { return m_code.at(id).opcode; }
    TypeID typeIDOf(ValueID id) const { return m_code.at(id).type; }
    Type typeOf(ValueID id) const { return m_generator.typeOf(id); }
    Representation representationOf(ValueID id) const { return m_code.at(id).representation; }
    bool isDeferredClosure(ValueID id) const { return m_generator.isDeferredClosure(id); }
    const Effects& effectsOf(const OperationContext& context LIFETIME_BOUND) const { return m_generator.effectsOf(context); }
    Subject subjectOf(const OperationContext& context) const { return m_generator.subjectOf(context); }

    const Function& function() const { return m_generator.function(); }
    const CallingConventions& conventions() const { return m_plan.conventions(); }
    FieldAccess fieldAccessFor(PropertyKeyID, LayoutSetID receivers, bool isStore) const;
    LayoutSetID membersWithFieldAmong(const FieldAccess& access, LayoutSetID receivers)
    {
        if (types().layoutSets().sizeOf(receivers) == access.membersWithField.size())
            return receivers;
        return types().layoutSets().ofSorted(access.membersWithField.span());
    }
    ValueID definitionOf(ValueID) const;
    bool functionSuspends() const { return m_generator.suspends(); }
    bool functionHasHandlers() const { return m_generator.hasHandlers(); }
    bool hasOneInstance() const;
    bool emittersCheckProofs() const { return AOT::hasFlag(m_generator.build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) || AOT::hasFlag(m_generator.build().options().testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly); }

    Operand get(ValueID id) const { return m_generator.get(id); }
    template<typename T> T in(ValueID id) const { return m_generator.getIn(id, T::representation).template as<T>(); }
    template<typename T> std::optional<T> inOptional(ValueID id) const { return id ? std::optional<T>(in<T>(id)) : std::nullopt; }
    Boxed boxed(ValueID id) const { return in<Boxed>(id); }
    Word32 int32InBox(Boxed);
    Integer integerOf(ValueID);

    void define(const ValueContext& value, Operand result) { m_generator.define(value, result); }
    void defineNarrowed(const ValueContext&, Operand);
    void defineTest(const ValueContext& value, Condition result) { m_generator.defineTest(value, result); }
    void defineTestByBranching(const ValueContext&, NOESCAPE const Invocable<void(BranchTargets)> auto& branchOnTest);
    void defineResults(const ValueContext& value, std::initializer_list<Operand> results) { m_generator.defineResults(value, std::span<const Operand>(results.begin(), results.size())); }
    void defineResults(const ValueContext& value, std::span<const Operand> results) { m_generator.defineResults(value, results); }

    template<typename T> T callOperation(const OperationContext&, RuntimeOperation, std::initializer_list<Operand> arguments, const CallOptions& = { });
    void callVoidOperation(const OperationContext&, RuntimeOperation, std::initializer_list<Operand> arguments, const CallOptions& = { });
    Operand callOperation(const OperationContext&, RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    template<typename T> T callWithoutSite(RuntimeOperation, std::initializer_list<Operand> arguments, const CallOptions& = { });
    Operand callWithoutSite(RuntimeOperation, std::span<const Operand> arguments, Representation result, const CallOptions&);
    template<typename T> T callLeaf(RuntimeOperation, std::initializer_list<Operand> arguments);
    Operand callLeaf(RuntimeOperation, std::span<const Operand> arguments, Representation result);

    BuiltinID functionOf(BuiltinOverloadID) const;
    StubCallResult callStub(const OperationContext&, StubKey, std::initializer_list<LValue> operands, const CallOptions& = { });
    StubCallResult callStub(const OperationContext&, StubKey, std::span<const LValue> operands, const CallOptions& = { });

    void throwAt(const OperationContext&, ThrowKind, std::optional<Operand>, uint32_t a, std::optional<uint32_t> b = std::nullopt);
    void requireOrThrow(const OperationContext&, Condition mustHold, ThrowKind, std::optional<Operand>, uint32_t a, std::optional<uint32_t> b = std::nullopt);

    const TypeTestPlan& typeTestPlanFor(TypeID from, Representation, TypeID to);
    Condition hasType(Operand, const TypeTestPlan&);
    void branchOnType(Operand, const TypeTestPlan&, BranchTargets);

    void barrierAfterStore(const ValueContext& store, CellPointer owner, const Type& storedType);

    Representation resultRepresentationOf(const InlineCall&) const;
    Type resultTypeOf(const InlineCall&) const;
    TypeID resultTypeIDOf(const InlineCall&) const;
    bool resultIsRead(const InlineCall&) const;
    LBasicBlock missBlockOf(const InlineCall&);
    void finishInlineCall(const InlineCall&, Operand result);

    Pointer argumentBuffer(std::span<const Boxed>);
    uint32_t mentionOfName(PropertyKeyID);
    Word32 identifierNumber(PropertyKeyID);
    std::optional<Vector<uint32_t, 12>> modulesAndPositionsOf(const OperationContext&) const;
    std::optional<Vector<uint32_t, 12>> modulesAndPositionsOf(Origin) const;
    std::optional<Pointer> siteCacheAddress(const OperationContext&, PropertyKeyID, SlotAccess);
    std::optional<NumberedImmediate> answersAtSite(const OperationContext&, PropertyKeyID, SiteUse);
    std::optional<NumberedImmediate> answersAtSite(Origin, PropertyKeyID, SiteUse);
    std::optional<NumberedImmediate> presenceTestSiteAnswers(Origin, PropertyKeyID);
    std::optional<NumberedImmediate> isAnsweredAtUse(Origin, PropertyKeyID, uint32_t use);

    COLD void fail(const OperationContext& context, InternalErrorKind kind) { m_generator.fail(context, kind); }

    CodeGenerator& m_generator;
    const Program& m_program;
    const Plan& m_plan;
    const Facts& m_facts;
    const Code& m_code;
    const CodeUnit m_unit;
    const FunctionID m_id;
    CodeRecords& m_records;
    const BuiltinTable& m_builtins;
    const AOT::CompileOptions& m_options;

private:
    OperationEmitter(CodeGenerator& generator, const CodeGenerationInputs& inputs, CodeUnit unit)
        : PrimitiveEmitter(generator)
        , m_generator(generator)
        , m_program(inputs.program)
        , m_plan(inputs.plan)
        , m_facts(inputs.facts)
        , m_code(generator.code())
        , m_unit(unit)
        , m_id(unit.function)
        , m_records(generator.records())
        , m_builtins(inputs.program.builtins())
        , m_options(generator.build().options())
    {
    }
};

void OperationEmitter::defineTestByBranching(const ValueContext& value, NOESCAPE const Invocable<void(BranchTargets)> auto& branchOnTest)
{
    if (!value.isBranchCondition()) {
        defineTest(value, asBoolean(branchOnTest));
        return;
    }
    branchOnTest(value.branchTargets());
}

template<typename T>
T OperationEmitter::callOperation(const OperationContext& context, RuntimeOperation operation, std::initializer_list<Operand> arguments, const CallOptions& options)
{
    return callOperation(context, operation, std::span<const Operand>(arguments.begin(), arguments.size()), T::representation, options).template as<T>();
}

inline void OperationEmitter::callVoidOperation(const OperationContext& context, RuntimeOperation operation, std::initializer_list<Operand> arguments, const CallOptions& options)
{
    callOperation(context, operation, std::span<const Operand>(arguments.begin(), arguments.size()), Representation::Void, options);
}

template<typename T>
T OperationEmitter::callWithoutSite(RuntimeOperation operation, std::initializer_list<Operand> arguments, const CallOptions& options)
{
    return callWithoutSite(operation, std::span<const Operand>(arguments.begin(), arguments.size()), T::representation, options).template as<T>();
}

template<typename T>
T OperationEmitter::callLeaf(RuntimeOperation operation, std::initializer_list<Operand> arguments)
{
    return callLeaf(operation, std::span<const Operand>(arguments.begin(), arguments.size()), T::representation).template as<T>();
}

inline StubCallResult OperationEmitter::callStub(const OperationContext& context, StubKey key, std::initializer_list<LValue> operands, const CallOptions& options)
{
    return callStub(context, key, std::span<const LValue>(operands.begin(), operands.size()), options);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
