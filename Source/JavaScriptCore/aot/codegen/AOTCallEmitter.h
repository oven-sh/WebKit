/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTNativeCallExpectation.h"
#include "AOTOperationEmitter.h"
#include "AOTRelocation.h"
#include "AOTStubsConstruct.h"
#include "AOTVector.h"
#include "B3ConstrainedValue.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// The entry of the table that the callee is known to be. Validation compares the call with it.
struct CalledBuiltin {
    BuiltinOverloadID overload;
    bool matchesAnyOverload { false };
};

struct ValueCall {
    Boxed callee;
    Boxed thisValue;
    std::span<const Boxed> arguments;
    CalledBuiltin builtin { };
    // AOTStubsCalls.h, callValueLending().
    uint8_t lentArguments { 0 };
    bool calleeIsCell { false };
};

struct ConventionCall {
    StubKey stub;
    std::span<const LValue> stubOperands { };
    LValue callee { nullptr };
    LValue thisValue { nullptr };
    LValue count { nullptr };
    std::span<const LValue> arguments { };
    CalledBuiltin builtin { };
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
    void generateResults(const ValueContext&);
    void generateResultAt(const ValueContext&);

    Boxed callValue(const OperationContext&, const ValueCall&, const CallOptions& = { });
    Boxed callByConvention(const OperationContext&, const ConventionCall&, const CallOptions& = { });
    Boxed constructValue(const OperationContext&, ConstructSyntax, Boxed callee, Boxed newTarget, std::span<const ValueID> arguments, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt);
    Boxed superConstructWithList(const OperationContext&, Boxed parent, Boxed newTarget, Word32 count, Pointer list);
    Boxed callWithList(const OperationContext&, Boxed callee, Boxed thisValue, Word32 count, Pointer list, const CallOptions& = { });
    Operand callDirect(const OperationContext&, FunctionID, std::span<const Operand> arguments, const CallOptions& = { });

    void tailCallByConvention(const OperationContext&, const ConventionCall&);
    void requireCallableBeforeTailCall(const OperationContext&, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt);
    void didEmitTailCall();
    // Says what the program calls the callee, where it calls it anything.
    static std::optional<ThrowSite> siteThatNamesCallee(const ValueContext&);

    void returnValues(const TerminatorContext&, std::span<const Operand>);
    void returnMisfit(Boxed);

private:
    enum class OmittedArguments : uint8_t { MustBePassed, MayBeOmitted };
    enum class TargetKind : uint8_t { Function, Address, Stub };

    struct CallTarget {
        static CallTarget function(FunctionID calledFunction, uint16_t entry) { return { .kind = TargetKind::Function, .calledFunction = calledFunction, .entry = entry }; }
        static CallTarget address(LValue calledAddress) { return { .kind = TargetKind::Address, .calledAddress = calledAddress }; }
        static CallTarget stub(StubKey calledStub, std::optional<uint32_t> immediate) { return { .kind = TargetKind::Stub, .calledStub = calledStub, .immediate = immediate }; }

        TargetKind kind { TargetKind::Function };
        FunctionID calledFunction;
        uint16_t entry { primaryEntry };
        LValue calledAddress { nullptr };
        StubKey calledStub;
        std::optional<uint32_t> immediate;
        std::optional<NativeCallExpectation> expectation;
        bool passesCallee { false };
    };

    struct PlacedArguments {
        Vector<B3::ConstrainedValue, 8> inRegisters;
        Vector<LValue, 4> onStack;
        uint32_t stackWordsToReserve { 0 };
        uint16_t entry { primaryEntry };
    };

    struct StackWord {
        uint32_t index;
        LValue value;
    };

    static bool isUndefinedConstant(LValue);
    Boxed thisOrUndefined(ValueID);
    Vector<Boxed, 8> boxedOperands(std::span<const ValueID>);
    Vector<Operand, 8> operandsOf(std::span<const ValueID>, const ArgumentLocations&);

    PlacedArguments placeArguments(FunctionID callee, const ArgumentLocations&, std::span<const Operand>, OmittedArguments);
    PlacedArguments placeConventionArguments(const ConventionCall&);
    NativeCallExpectation expectationOf(const OperationContext&, CalledBuiltin);
    std::span<const BuiltinID> builtinsCalledByNativeCode();

    B3::PatchpointValue* emitCall(const OperationContext&, const CallTarget&, const PlacedArguments&, std::span<const ArgumentLocation> results, const CallOptions&);
    void defineResultsOfCall(const ValueContext&, std::span<const ArgumentLocation> results, B3::PatchpointValue*);
    Operand resultOfCall(const ArgumentLocation&, LValue);
    void chooseStubThatCallsValue(ConventionCall&, LValue& countOperand, bool calleeIsCell);

    void meetLandingOfMisfit(const ValueContext&, LValue returned);
    void emitTailCall(const OperationContext&, const CallTarget&, const PlacedArguments&);
    B3::PatchpointValue* leaveAfterFrameTeardown(std::span<const B3::ConstrainedValue> inRegisters, std::span<const StackWord>, const Invocable<void(CCallHelpers&)> auto& leave);
    void jumpAfterFrameTeardown(const CallTarget&, const PlacedArguments&);
    void callFromArgumentListFrame(const CallTarget&, const PlacedArguments&);
    void tailCallValue(const ValueContext&, const ValueCall&);
    void callAndReturnIfCalleeEndsItsTailCalls(const ValueContext&, const ValueCall&);
    bool hasStackWordsOfCaller(uint32_t numberOfWords) const;

    std::optional<Vector<BuiltinID>> m_builtinsCalledByNativeCode;
    LBasicBlock m_landingOfLastCall { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
