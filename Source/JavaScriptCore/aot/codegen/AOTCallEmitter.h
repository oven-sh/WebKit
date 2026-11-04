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

namespace JSC { namespace AOT { namespace Typed {

struct ValueCall {
    Boxed callee;
    Boxed thisValue;
    std::span<const Boxed> arguments;
    bool calleeIsCell { false };
    // Bit i: argument i is a closure of the frame that is lent to the callee (NewClosure[isLent]).
    uint8_t lentArguments { 0 };
    // The callee was read by a name off `thisValue`, and a receiver that is as its built-in class makes it finds this under the name: Operation::methodOfReceiverAsMade().
    std::optional<BuiltinBoxedStub> methodOfBuiltinClass;
};

struct ConventionCall {
    StubKey stub;
    std::span<const LValue> stubOperands { };
    LValue callee { nullptr };
    LValue thisValue { nullptr };
    LValue count { nullptr };
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
    // A Call of what was read under the key off its own receiver. False: the receiver may be of a layout without a row, or nobody planned such a selector.
    bool tryCallThroughMethodTable(const ValueContext& call, PropertyKeyID);
    void generateResults(const ValueContext&);
    void generateResultAt(const ValueContext&);

    Boxed callValue(const OperationContext&, const ValueCall&, const CallOptions& = { });
    // Of a call or a construction by value. Where the type of the callee names one built-in, the type of the record is what its rows write, and native code is held to that here as it is in a CallBuiltin.
    void defineResultOfCallByValue(const ValueContext&, Boxed result);
    static CallPosition positionOf(const OperationContext& context, const CallOptions& options = { }) { return options.isTailCall || context.origin().isTailCall ? CallPosition::Tail : CallPosition::Ordinary; }
    Boxed callByConvention(const OperationContext&, const ConventionCall&, const CallOptions& = { });
    Boxed constructValue(const OperationContext&, ConstructSyntax, Boxed callee, Boxed newTarget, std::span<const ValueID> arguments, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt);
    Boxed superConstructWithList(const OperationContext&, Boxed parent, Boxed newTarget, Word32 count, Pointer list);
    Boxed superConstructWithArray(const OperationContext&, Boxed parent, Boxed newTarget, CellPointer list);
    Boxed callWithList(const OperationContext&, Boxed callee, Boxed thisValue, Word32 count, Pointer list, const CallOptions& = { });
    // Entry 0 of a unit with one result, in the direct convention.
    Operand callDirect(const OperationContext&, CodeUnit, std::span<const Operand> arguments, const CallOptions& = { });
    // Any entry, by the locations that the caller names.
    Vector<Operand, 2> callEntry(const OperationContext&, CodeUnit, uint16_t entry, const ArgumentLocations&, std::span<const Operand> arguments, const CallOptions& = { });
    // The same as a tail call, for a unit that returns what the callee returns and keeps nothing in its frame.
    void tailCallEntry(const OperationContext&, CodeUnit, uint16_t entry, const ArgumentLocations&, std::span<const Operand> arguments);
    // What such a call is handed, each value in the form of its location. No value where the location has the kind None, which no caller fills. A word that says nothing is zero.
    Vector<Operand, 8> operandsOf(std::span<const ValueID>, const ArgumentLocations&);

    void tailCallByConvention(const OperationContext&, const ConventionCall&);
    void requireCallable(const OperationContext&, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee, bool isAtTailCall);
    void requireCallableBeforeTailCall(const OperationContext& context, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee = std::nullopt) { requireCallable(context, callee, siteThatNamesCallee, true); }
    void requireCallableBeforeTailCall(const ValueContext&, Boxed callee, Boxed thisValue);
    void didEmitTailCall();
    // Says what the program calls the callee, where it calls it anything.
    std::optional<ThrowSite> siteThatNamesCallee(const ValueContext&);

    void returnValues(const TerminatorContext&, std::span<const Operand>);
    void returnValues(std::span<const Operand>);

private:
    bool mayForwardCallToReceiver(const ValueContext&);

    enum class TargetKind : uint8_t { Function, Stub, Address };

    struct CallTarget {
        static CallTarget unit(CodeUnit calledUnit, uint16_t entry) { return { .kind = TargetKind::Function, .calledUnit = calledUnit, .entry = entry }; }
        static CallTarget stub(StubKey calledStub, std::optional<uint32_t> immediate) { return { .kind = TargetKind::Stub, .calledStub = calledStub, .immediate = immediate }; }
        // The address of the primary entry of some unit that takes and returns as `takesAndReturnsAlike` does. It is handed over in valueEntryGPR, which no direct convention passes anything in.
        static CallTarget address(LValue calledAddress, CodeUnit takesAndReturnsAlike) { return { .kind = TargetKind::Address, .calledUnit = takesAndReturnsAlike, .calledAddress = calledAddress }; }

        TargetKind kind { TargetKind::Function };
        CodeUnit calledUnit;
        LValue calledAddress { nullptr };
        uint16_t entry { primaryEntry };
        StubKey calledStub;
        std::optional<uint32_t> immediate;
        bool passesCallee { false };
    };

    struct PlacedArguments {
        Vector<Machine::ConstrainedValue, 8> inRegisters;
        Vector<LValue, 4> onStack;
        // CallingConventions::letsGoOfParameter().
        Vector<LValue, 2> letGoByCallee;
        uint32_t stackWordsToReserve { 0 };
        // What the call enters: the entry that was asked for, or in its place one that fills the registers which are left out here.
        uint16_t entry { primaryEntry };
    };

    // MayBeOmitted: of a call of the primary entry that is returned to. CallingConventions::findEntryOmittingArguments().
    enum class OmittedArguments : bool { MustBePassed, MayBeOmitted };

    struct StackWord {
        uint32_t index;
        LValue value;
    };

    static bool isUndefinedConstant(LValue);
    Boxed thisOrUndefined(ValueID);
    Vector<Boxed, 8> boxedOperands(std::span<const ValueID>);

    // The unit that CallDirect or AwaitCallBySavingFrame enters: CallingConventions::unitEnteredBy().
    CodeUnit calleeOf(const ValueContext& callDirect) const;
    PlacedArguments placeArguments(CodeUnit callee, uint16_t entry, const ArgumentLocations&, std::span<const Operand>, OmittedArguments = OmittedArguments::MustBePassed);
    PlacedArguments placeConventionArguments(const ConventionCall&);
    // Once every operand of the call is appended.
    void markArgumentsLetGoByCallee(Machine::PatchOperation*, const PlacedArguments&);
    // PatchOperation::childrenLetGoByCallee().
    static void emitClearOfReleasedRegisters(CCallHelpers&, const Machine::PatchParameters&, const RegisterSet& among);
    // A CallDirect of CodeRole::ModuleStart, which has no code: the stub StartModule with the number of the start. Whatever the call hands over is `this` of a module, which the stub knows to be undefined.
    void callStartOfModule(const ValueContext&, CodeUnit start, const ArgumentLocations&);

    Machine::PatchOperation* emitCall(const OperationContext&, const CallTarget&, const PlacedArguments&, std::span<const ArgumentLocation> results, const CallOptions&);
    void defineResultsOfCall(const ValueContext&, std::span<const ArgumentLocation> results, Machine::PatchOperation*);
    void callThroughMethodTable(const ValueContext&, uint32_t placeOfSelector, std::span<const ValueID> receiverAndArguments);
    Operand resultOfCall(const ArgumentLocation&, LValue);
    void chooseStubThatCallsValue(ConventionCall&, LValue& countOperand, bool calleeIsCell);

    void emitTailCall(const OperationContext&, const CallTarget&, const PlacedArguments&);
    Machine::PatchOperation* leaveAfterFrameTeardown(std::span<const Machine::ConstrainedValue> inRegisters, std::span<const StackWord>, const Invocable<void(CCallHelpers&)> auto& leave);
    void jumpAfterFrameTeardown(const CallTarget&, const PlacedArguments&);
    void callFromArgumentListFrame(const CallTarget&, const PlacedArguments&);
    void callAndReturnIfCalleeEndsItsTailCalls(const ValueContext&, const ValueCall&);
    void tailCallValue(const ValueContext&, const ValueCall&);
    bool hasStackWordsOfCaller(uint32_t numberOfWords) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
