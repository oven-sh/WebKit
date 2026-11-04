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
    // In place of `immediate`: a number that only Image can count, by its index in the mentions of the function.
    std::optional<NumberedImmediate> numberedImmediate { };
    std::optional<Origin> origin { };
    // The callee reads something and then calls it. What the read throws or runs is shown here.
    std::optional<Origin> readBeforeCall { };
    std::optional<Machine::PureFunction> pureFunction { };
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
    // The callee makes an error, which keeps the frames that it finds and where each of them stands.
    bool capturesStackTrace { false };
    // The call stands at the head of a handler. Whoever threw has allocated what it threw, whatever its effects on the way back say.
    bool followsThrow { false };
    // Whatever the callee writes, no cell that exists has another header, butterfly or closure word behind it.
    bool convertsNoCells { false };
    // The caller is suspended as soon as the callee returns, so it has read what it keeps in its saved registers for the last time.
    bool callerSuspendsAtReturn { false };
    // The one word that the callee returns is an Int32. Where it names a cell the call itself is the narrow reference: no Int64 beside it lives across a safepoint unseen.
    bool returnsWord32 { false };
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
        , m_plan(generator.plan())
        , m_facts(generator.facts())
        , m_code(generator.code())
        , m_unit(generator.unit())
        , m_id(generator.unit().function)
        , m_records(generator.records())
        , m_builtins(generator.program().builtins())
        , m_options(generator.build().options())
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

    // The record of the function in the program's tables. The code is m_code.
    const Function& function() const { return m_generator.function(); }
    const CallingConventions& conventions() const { return m_plan.conventions(); }
    // Plan::fieldAccess() for the members of the set.
    FieldAccess fieldAccessFor(PropertyKeyID, LayoutSetID receivers, bool isStore) const;
    // The operation that made the value, behind any Narrowed.
    ValueID definitionOf(ValueID) const;
    const LiteralStringTable& literalStrings() const { return m_plan.literalStrings(); }
    bool functionSuspends() const { return m_generator.suspends(); }
    bool functionHasHandlers() const { return m_generator.hasHandlers(); }
    // Proven. Workers and the module instances of a test run make several.
    bool hasOneInstance() const;

    Operand get(ValueID id) const { return m_generator.get(id); }
    template<typename T> T in(ValueID id) const { return m_generator.getIn(id, T::representation).template as<T>(); }
    template<typename T> std::optional<T> inOptional(ValueID id) const { return id ? std::optional<T>(in<T>(id)) : std::nullopt; }
    Boxed boxed(ValueID) const;
    // An operand of kind Integer is in Int32 or Int64.
    Integer integerOf(ValueID);

    void define(const ValueContext& value, Operand result) { m_generator.define(value, result); }
    // The result is the operand under a narrower type, which may have another form: findConversion() from the form of the operand to that of the result, for the type of the result. No step tests anything.
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

    void barrierAfterStore(const ValueContext& store, CellPointer owner, Operand stored, const Type& storedType);

    Representation resultRepresentationOf(const InlineCall&) const;
    Type resultTypeOf(const InlineCall&) const;
    // Of a call that can miss. False: the sweep has found nobody who reads the result, so whatever finishInlineCall() is handed for it goes nowhere.
    bool resultIsRead(const InlineCall&) const;
    LBasicBlock missBlockOf(const InlineCall&);
    void finishInlineCall(const InlineCall&, Operand result);

    Pointer argumentBuffer(std::span<const Boxed>);
    // NumberSpace::Name, or NumberSpace::NameOfText for a key that liesInThread().
    uint32_t mentionOfName(PropertyKeyID);
    Word32 identifierNumber(PropertyKeyID);
    // A module and a position for each call through which the operation was inlined, from the outermost in, and then for the operation itself: where each is written.
    // So a helper that is inlined at two calls has two such lists. Nothing: some frame of the operation is written in no module.
    std::optional<Vector<uint32_t, 12>> modulesAndPositionsOf(const OperationContext&) const;
    // The address of the Slot that remembers what an access under the name found where the operation is written: NumberSpace::SiteCache.
    // Nothing: Image cannot number the key, or some frame of the operation is written in no module.
    std::optional<Pointer> siteCacheAddress(const OperationContext&, PropertyKeyID, SlotAccess);
    // The same place for a stub that asks SiteAnswers: what the stub is handed as its immediate.
    std::optional<NumberedImmediate> answersAtSite(const OperationContext&, PropertyKeyID, SiteUse);

    void fail(const OperationContext& context, InternalErrorKind kind) { m_generator.fail(context, kind); }

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
