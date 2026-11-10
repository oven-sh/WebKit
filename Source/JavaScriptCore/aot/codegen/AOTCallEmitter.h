/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTOperationEmitter.h"
#include "AOTRelocation.h"
#include "AOTStubsConstruct.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

struct StubSignature;
struct StubSummary;

namespace Typed {

struct ValueCall {
    Boxed callee;
    Boxed thisValue;
    std::span<const Boxed> arguments;
    bool calleeIsCell { false };
    uint8_t lentArguments { 0 };
    std::optional<BuiltinBoxedStub> methodOfBuiltinClass;
    bool receiverIsProvenOfBuiltinClass { false };
    bool methodMayBeUnread { false };
};

struct ConventionCall {
    StubKey stub;
    std::span<const LValue> stubOperands { };
    LValue callee { nullptr };
    LValue thisValue { nullptr };
    std::span<const LValue> arguments { };
};

class CallEmitter final : public OperationEmitter {
public:
    explicit CallEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateCall(const ValueContext&);
    void generateConstruct(const ValueContext&);
    void generateCallList(const ValueContext&);
    void generateConstructList(const ValueContext&);
    void generateCallDirect(const ValueContext&);
    void generateCallTable(const ValueContext&);
    bool tryCallThroughMethodTable(const ValueContext& call, PropertyKeyID);
    void generateResults(const ValueContext&);
    void generateResultAt(const ValueContext&);

    Boxed callValue(const OperationContext&, const ValueCall&, const CallOptions& = { });
    void defineResultOfCallByValue(const ValueContext&, Boxed result);
    static CallPosition positionOf(const OperationContext& context) { return context.origin().isTailCall ? CallPosition::Tail : CallPosition::Ordinary; }
    Boxed callByConvention(const OperationContext&, const ConventionCall&, const CallOptions& = { });
    Boxed callAddressByConvention(const OperationContext&, Pointer address, const ConventionCall&, const CallOptions& = { });
    Boxed constructValue(const OperationContext&, ConstructSyntax, Boxed callee, Boxed newTarget, std::span<const ValueID> arguments, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt);
    Boxed superConstructWithArray(const OperationContext&, Boxed parent, Boxed newTarget, CellPointer list);
    Operand callDirect(const OperationContext&, CodeUnit, std::span<const Operand> arguments, const CallOptions& = { });
    Vector<Operand, 2> callEntry(const OperationContext&, CodeUnit, uint16_t entry, const ArgumentLocations&, std::span<const Operand> arguments, const CallOptions& = { });
    void tailCallEntry(const OperationContext&, CodeUnit, uint16_t entry, const ArgumentLocations&, std::span<const Operand> arguments);
    Vector<Operand, 8> operandsOf(std::span<const ValueID>, const ArgumentLocations&);

    void tailCallByConvention(const OperationContext&, const ConventionCall&);
    void requireCallable(const OperationContext&, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee, bool isAtTailCall, std::optional<Boxed> listOfCall = std::nullopt);
    void requireCallableBeforeTailCall(const OperationContext& context, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt, std::optional<Boxed> listOfCall = std::nullopt) { requireCallable(context, callee, siteThatNamesCallee, true, listOfCall); }
    void requireCallableBeforeTailCall(const ValueContext&, Boxed callee, Boxed thisValue, std::optional<Boxed> listOfCall = std::nullopt);
    bool mayForwardCallToReceiver(const Operation& call);
    void requireForwardedCallCanBeMade(const ValueContext&, Boxed callee, Boxed thisValue, bool isAtTailCall);
    void didEmitTailCall();
    std::optional<ThrowSite> siteThatNamesCallee(const ValueContext&);
    bool isProvenReceiverOf(BuiltinBoxedStub, const Type& receiver);

    void returnValues(const TerminatorContext&, std::span<const Operand>);
    void returnValues(std::span<const Operand>);

private:
    ValueID listThatApplyWouldRead(const Operation& call);

    enum class TargetKind : uint8_t { Function, Stub, Address };

    struct CallTarget {
        static CallTarget unit(CodeUnit calledUnit, uint16_t entry) { return { .kind = TargetKind::Function, .calledUnit = calledUnit, .entry = entry }; }
        static CallTarget stub(StubKey calledStub, std::optional<uint32_t> immediate) { return { .kind = TargetKind::Stub, .calledStub = calledStub, .immediate = immediate }; }
        static CallTarget address(LValue calledAddress, CodeUnit takesAndReturnsAlike) { return { .kind = TargetKind::Address, .calledUnit = takesAndReturnsAlike, .calledAddress = calledAddress }; }

        TargetKind kind { TargetKind::Function };
        CodeUnit calledUnit;
        LValue calledAddress { nullptr };
        uint16_t entry { primaryEntry };
        StubKey calledStub;
        std::optional<uint32_t> immediate;
    };

    struct PlacedArguments {
        Vector<Machine::ConstrainedValue, 8> inRegisters;
        Vector<LValue, 4> onStack;
        uint32_t stackWordsToReserve { 0 };
    };

    struct StackWord {
        uint32_t index;
        LValue value;
    };

    struct AnswersOfCalledStub {
        const StubSummary* summary { nullptr };
        Effects effects;
    };

    static bool isUndefinedConstant(LValue);
    Boxed thisOrUndefined(ValueID);
    Vector<Boxed, 8> boxedOperands(std::span<const ValueID>);

    CodeUnit calleeOf(const ValueContext& callDirect) const;
    const ArgumentLocations& ownLocations();
    PlacedArguments placeArguments(CodeUnit callee, const ArgumentLocations&, std::span<const Operand>);
    PlacedArguments placeConventionArguments(const ConventionCall&);
    PlacedArguments placeConventionArguments(const ConventionCall&, const StubSignature&);
    void callStartOfModule(const ValueContext&, CodeUnit start, const ArgumentLocations&);

    Machine::PatchOperation* emitCall(const OperationContext&, const CallTarget&, const PlacedArguments&, std::span<const ArgumentLocation> results, const CallOptions&, const AnswersOfCalledStub* = nullptr);
    void defineResultOfCallByValue(const ValueContext&, Boxed result, const Type& callee);
    void defineResultsOfCall(const ValueContext&, std::span<const ArgumentLocation> results, Machine::PatchOperation*);
    enum class ReceiverOfTableCall : bool { IsShown, IsAsked };
    void callThroughMethodTable(const ValueContext&, uint32_t placeOfSelector, std::span<const ValueID> receiverAndArguments, ReceiverOfTableCall);
    Operand resultOfCall(const ArgumentLocation&, LValue);
    void chooseStubThatCallsValue(ConventionCall&, LValue& countOperand, bool calleeIsCell);

    void emitTailCall(const OperationContext&, const CallTarget&, const PlacedArguments&, const ArgumentLocations* locationsOfCalledUnit = nullptr);
    Machine::PatchOperation* leaveAfterFrameTeardown(std::span<const Machine::ConstrainedValue> inRegisters, std::span<const StackWord>, const Invocable<void(CCallHelpers&)> auto& leave);
    void jumpAfterFrameTeardown(const CallTarget&, const PlacedArguments&);
    void callFromArgumentListFrame(const CallTarget&, const PlacedArguments&);
    void tailCallValue(const ValueContext&, const ValueCall&);
    bool hasStackWordsOfCaller(uint32_t numberOfWords) const;

    std::optional<ArgumentLocations> m_ownLocations;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
