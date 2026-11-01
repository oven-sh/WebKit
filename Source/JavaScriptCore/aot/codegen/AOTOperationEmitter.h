/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTOperationData.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRuntimeTable.h"
#include "AOTStubKey.h"
#include "B3PatchpointValue.h"
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
    std::optional<Origin> origin { };
    // The callee reads something and then calls it. What the read throws or runs is shown here.
    std::optional<Origin> readBeforeCall { };
    std::optional<B3::PureFunction> pureFunction { };
    std::optional<ThrowSite> throwSite { };
    // The callee runs steps of this function, which has no frame of its own. A stack trace lists the function above the caller.
    BuiltinID calledBuiltin { };
    // The callee of a direct construct that may leave its frame by a tail call.
    FunctionID constructedCallee { };
    // RecordedSite::keptCalleePlusOne.
    uint32_t keptCalleePlusOne { 0 };
    bool constructs { false };
    bool isTailCall { false };
    // As the program sees it the caller waits or has returned, so a stack trace lists what the callee runs without it.
    bool callerHasLeft { false };
    bool callsOwnFunctionObject { false };
    bool isOnRarePath { false };
    bool accessesCollectorState { false };
    // The callee runs code of the host, which the effects of the IR say nothing about: it may give a cell another Structure or another butterfly.
    bool runsCodeOfHost { false };
    bool reportMayReturn { false };
    // The callee makes an error, which keeps the frames that it finds and where each of them stands.
    bool capturesStackTrace { false };
    // The call stands at the head of a handler. Whoever threw has allocated what it threw, whatever its effects on the way back say.
    bool followsThrow { false };
    // The callee promises the type of its result, and the caller relies on it.
    bool hasLandingOfMisfit { false };
    // Whatever the callee writes, no cell that exists has another header, butterfly or closure word behind it.
    bool convertsNoCells { false };
    // The caller is suspended as soon as the callee returns, so it has read what it keeps in its saved registers for the last time.
    bool callerSuspendsAtReturn { false };
    // The callee reads and writes what this operation does too, which stands behind the one that the call is emitted for.
    ValueID alsoPerforms { };
};

struct StubCallResult {
    LValue first { nullptr };
    LValue second { nullptr };
};

class OperationEmitter : public PrimitiveEmitter {
protected:
    explicit OperationEmitter(CodeGenerator& generator)
        : PrimitiveEmitter(generator)
        , m_generator(generator)
        , m_program(generator.program())
        , m_placement(generator.inputs().program.placement)
        , m_function(generator.function())
        , m_id(generator.inputs().id)
        , m_records(generator.records())
        , m_builtins(generator.program().builtins())
        , m_options(generator.build().options())
    {
    }

    CodeGenerator& generator() { return m_generator; }
    const CodeGenerationInputs& inputs() const { return m_generator.inputs().program; }
    BuildContext& build() { return m_generator.build(); }
    FunctionAnalyses& analyses() { return m_generator.analyses(); }
    TypeArena& types() { return m_generator.inputs().types; }
    template<typename T> T& emitter() { return m_generator.emitter<T>(); }

    const Type& typeOf(ValueID id) const { return m_generator.typeOf(id); }
    Representation representationOf(ValueID id) const { return m_generator.representationOf(id); }

    Operand get(ValueID id) const { return m_generator.get(id); }
    // An operand that the operation took with any NaN, in the form that says so.
    Operand getInFormOfItsNaN(ValueID) const;
    template<typename T> T in(ValueID id) const { return m_generator.getIn(id, T::representation).template as<T>(); }
    template<typename T> std::optional<T> inOptional(ValueID id) const { return id ? std::optional<T>(in<T>(id)) : std::nullopt; }
    Boxed boxed(ValueID) const;
    // An operand of kind Integer is in Int32 or Int64.
    Integer integerOf(ValueID);

    void define(const ValueContext& value, Operand result) { m_generator.define(value, result); }
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
    Condition hasType(Operand, const TypeTestPlan&, CodeShape = CodeShape::Fast);
    void branchOnType(Operand, const TypeTestPlan&, BranchTargets, CodeShape = CodeShape::Fast);

    void barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType);

    Representation resultRepresentationOf(const InlineCall&) const;
    const Type& resultTypeOf(const InlineCall&) const;
    ValueID resultOf(const InlineCall&) const;
    LBasicBlock missBlockOf(const InlineCall&);
    void finishInlineCall(const InlineCall&, Operand result);

    Pointer argumentBuffer(std::span<const Boxed>);
    Word32 identifierNumber(PropertyKeyID);
    Pointer siteCacheAddress(SiteCacheID);

    void fail(const OperationContext& context, InternalErrorKind kind) { m_generator.fail(context, kind); }

    // The code that follows acts on something about the cell that it does not test. A validating build tests it first.
    void checkClaim(std::optional<CellPointer> about, NOESCAPE const Invocable<Condition()> auto& holds);
    void reportUnlessClaimHolds(std::optional<CellPointer> about, Condition holds);
    void checkIsCompiledObjectOf(CellPointer, LayoutSetID);

    CodeGenerator& m_generator;
    const Program& m_program;
    const Placement& m_placement;
    const Function& m_function;
    const FunctionID m_id;
    CodeRecords& m_records;
    const BuiltinTable& m_builtins;
    const AOT::CompileOptions& m_options;
};

void OperationEmitter::defineTestByBranching(const ValueContext& value, NOESCAPE const Invocable<void(BranchTargets)> auto& branchOnTest)
{
    if (!value.isBranchCondition()) {
        defineTest(value, asBoolean(branchOnTest));
        return;
    }
    if (std::optional<CodeGenerator::GuardUnderTest> guard = m_generator.takeGuardUnderTest(value)) {
        branchOnTest(m_generator.targetsOf(value, *guard));
        return;
    }
    branchOnTest(value.branchTargets());
}

void OperationEmitter::checkClaim(std::optional<CellPointer> about, NOESCAPE const Invocable<Condition()> auto& holds)
{
    if (m_generator.validatesOrChecksProofs())
        reportUnlessClaimHolds(about, holds());
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
