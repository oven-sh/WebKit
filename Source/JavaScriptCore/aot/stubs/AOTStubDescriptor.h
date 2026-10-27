/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTEffects.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"
#include "CPU.h"
#include "MacroAssembler.h"
#include "Reg.h"
#include "RegisterSet.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/Assertions.h>
#include <wtf/OptionSet.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class StubAssembler;

enum class StubLinkage : uint8_t { CalledByCode, EnteredByRuntime, FrontEnd, Handler, InternalEntry };

enum class StubAxis : uint8_t {
    Form = 1,
    CallerFrame = 2,
    Count = 4,
    OperandRegister = 8,
    Immediate = 16,
    StubFrame = 32,
    NeverReturns = 64,
};

enum class StubISA : uint8_t { X86_64 = 1, ARM64 = 2 };

#if CPU(ARM64)
constexpr StubISA currentStubISA = StubISA::ARM64;
#elif CPU(X86_64)
constexpr StubISA currentStubISA = StubISA::X86_64;
#else
#error "AOT stubs are generated for ARM64 and x86-64 only"
#endif

enum class OperandRegisterChoice : uint8_t { NoChoice, AlternateReceivers };
enum class ImmediateBinding : uint8_t { Never, ByPolicy, OperationOffset };
enum class StubResult : uint8_t { None, GPR, FPR, TwoGPRs, Flags };
enum class StubLeafness : uint8_t { MayCall, Leaf };
enum class StubClobbers : uint8_t { ClobbersLikeCall, ClobbersOnlyScratch };
enum class StubBoundImmediates : uint8_t { None, Every, NamesRead, NamesStored, NamesDefined };

struct StubOwnEffects {
    constexpr Typed::Effects toEffects() const
    {
        Typed::Effects effects = Typed::Effects::none();
        effects.reads = isDeclared ? Typed::LocationMasks(reads) : Typed::LocationMasks::top();
        effects.writes = isDeclared ? Typed::LocationMasks(writes) : Typed::LocationMasks::top();
        return effects;
    }

    friend constexpr StubOwnEffects operator|(StubOwnEffects left, StubOwnEffects right)
    {
        return { left.isDeclared && right.isDeclared, left.reads | right.reads, left.writes | right.writes };
    }

    bool isDeclared { false };
    Typed::AbstractHeapSet reads;
    Typed::AbstractHeapSet writes;
};

struct StubSignature {
    std::array<Reg, 8> operands;
    uint8_t numberOfOperands { 0 };
    bool usesCallConvention { false };
    StubResult result { StubResult::None };
    Reg firstResult;
    Reg secondResult;
    std::optional<GPRReg> immediate;
    StubBoundImmediates boundImmediates { StubBoundImmediates::None };
    MacroAssembler::ResultCondition trueCondition { MacroAssembler::Zero };

    constexpr void addOperand(Reg reg)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(numberOfOperands < operands.size());
        ASSERT_UNDER_CONSTEXPR_CONTEXT(reg.isSet());
        operands[numberOfOperands++] = reg;
    }

    static constexpr StubSignature withGPRResult(std::initializer_list<Reg> operandRegisters)
    {
        StubSignature signature;
        for (Reg reg : operandRegisters)
            signature.addOperand(reg);
        signature.result = StubResult::GPR;
        signature.firstResult = GPRInfo::returnValueGPR;
        return signature;
    }

    static constexpr StubSignature withoutResult(std::initializer_list<Reg> operandRegisters)
    {
        StubSignature signature;
        for (Reg reg : operandRegisters)
            signature.addOperand(reg);
        return signature;
    }
};

struct StubDescriptor {
    StubFamily family;
    StubFamily body;
    ASCIILiteral name;
    StubLinkage linkage;
    std::span<const ASCIILiteral> formNames;
    uint8_t numberOfForms;
    OptionSet<StubAxis> axes;
    uint8_t minCount;
    uint8_t maxCount;
    OptionSet<StubISA> isas;
    OptionSet<StubISA> framelessISAs;
    OperandRegisterChoice operandRegisters;
    ImmediateBinding binding;
    StubLeafness leafness;
    StubClobbers clobbers;
    StubOwnEffects ownEffects;
};

// A compiled program reads the descriptors: its fault handler asks exists(). Only the compiler reads the generators, so the code that emits stubs leaves an executable together with the compiler.
struct StubGenerator {
    void (*generate)(StubAssembler&);
    StubSignature (*signature)(StubKey);
};

#define v(Name, Linkage, forms, axes, minCount, maxCount, isas, framelessISAs, operandRegisters, binding, calls, clobbers, ownEffects, Body) \
    void generate##Body(StubAssembler&); \
    StubSignature signatureOf##Name(StubKey);
#include "AOTStubFamilies.def"
#undef v

inline constexpr std::array<StubGenerator, numberOfStubFamilies> stubGenerators { {
#define v(Name, Linkage, forms, axes, minCount, maxCount, isas, framelessISAs, operandRegisters, binding, calls, clobbers, ownEffects, Body) \
    { StubFamily::Name == StubFamily::Body ? &generate##Body : nullptr, &signatureOf##Name },
#include "AOTStubFamilies.def"
#undef v
} };

constexpr const StubGenerator& generatorOf(StubFamily family)
{
    return stubGenerators[static_cast<size_t>(family)];
}

inline StubSignature signatureOf(StubKey key)
{
    return generatorOf(key.family()).signature(key);
}

namespace StubFamilyTokens {

using enum Typed::AbstractHeap;

template<typename... Names>
constexpr std::array<ASCIILiteral, sizeof...(Names)> createFormNames(Names... names)
{
    return { names... };
}

#define AOT_STUB_FORM_NAME(Form) #Form ""_s
#define AOT_STUB_FORM_NAMES(...) createFormNames(WTF_FOR_EACH(AOT_STUB_FORM_NAME, __VA_ARGS__))
#define v(Name, Linkage, forms, ...) inline constexpr auto formNamesOf##Name = AOT_STUB_FORM_NAMES forms;
#include "AOTStubFamilies.def"
#undef v
#undef AOT_STUB_FORM_NAMES
#undef AOT_STUB_FORM_NAME

inline constexpr OptionSet<StubAxis> NoAxis { };
inline constexpr OptionSet<StubAxis> FormAxis { StubAxis::Form };
inline constexpr OptionSet<StubAxis> CallerFrameAxis { StubAxis::CallerFrame };
inline constexpr OptionSet<StubAxis> CountAxis { StubAxis::Count };
inline constexpr OptionSet<StubAxis> OperandRegisterAxis { StubAxis::OperandRegister };
inline constexpr OptionSet<StubAxis> ImmediateAxis { StubAxis::Immediate };
inline constexpr OptionSet<StubAxis> StubFrameAxis { StubAxis::StubFrame };
inline constexpr OptionSet<StubAxis> NeverReturns { StubAxis::NeverReturns };

inline constexpr OptionSet<StubISA> AllISAs { StubISA::X86_64, StubISA::ARM64 };
inline constexpr OptionSet<StubISA> ARM64Only { StubISA::ARM64 };
inline constexpr OptionSet<StubISA> X86_64Only { StubISA::X86_64 };
inline constexpr OptionSet<StubISA> NoISA { };

inline constexpr StubOwnEffects AsOperation { };
inline constexpr StubOwnEffects NoEffects { true, { }, { } };

template<typename... Heaps>
constexpr StubOwnEffects reads(Heaps... heaps)
{
    return { true, Typed::AbstractHeapSet { heaps... }, { } };
}

template<typename... Heaps>
constexpr StubOwnEffects writes(Heaps... heaps)
{
    return { true, { }, Typed::AbstractHeapSet { heaps... } };
}

inline constexpr unsigned N = numberOfArgumentGPRs;

inline constexpr std::array<StubDescriptor, numberOfStubFamilies> descriptors { {
#define v(Name, Linkage, forms, axes, minCount, maxCount, isas, framelessISAs, operandRegisters, binding, calls, clobbers, ownEffects, Body) \
    { StubFamily::Name, StubFamily::Body, #Name ""_s, StubLinkage::Linkage, formNamesOf##Name, static_cast<uint8_t>(std::max<size_t>(formNamesOf##Name.size(), 1)), axes, minCount, maxCount, isas, framelessISAs, OperandRegisterChoice::operandRegisters, ImmediateBinding::binding, StubLeafness::calls, StubClobbers::clobbers, ownEffects },
#include "AOTStubFamilies.def"
#undef v
} };

} // namespace StubFamilyTokens

constexpr const StubDescriptor& descriptorOf(StubFamily family)
{
    return StubFamilyTokens::descriptors[static_cast<size_t>(family)];
}

constexpr bool hasConsistentColumns(const StubDescriptor& descriptor)
{
    OptionSet<StubAxis> axes = descriptor.axes;
    size_t numberOfFormNames = descriptor.formNames.size();
    if (numberOfFormNames == 1 || numberOfFormNames > StubKey::maxNumberOfForms)
        return false;
    if (descriptor.numberOfForms != std::max<size_t>(numberOfFormNames, 1))
        return false;
    if (axes.contains(StubAxis::Form) != !!numberOfFormNames)
        return false;
    if (descriptor.minCount > descriptor.maxCount || descriptor.maxCount > StubKey::maxCount)
        return false;
    if (descriptor.maxCount && !axes.contains(StubAxis::Count))
        return false;
    if (!descriptor.isas.containsAll(descriptor.framelessISAs))
        return false;
    if (!axes.contains(StubAxis::CallerFrame) && (!descriptor.framelessISAs.isEmpty() || axes.contains(StubAxis::StubFrame)))
        return false;
    if (descriptor.linkage != StubLinkage::EnteredByRuntime && !descriptor.framelessISAs.isEmpty())
        return false;
    if (descriptor.linkage == StubLinkage::FrontEnd && descriptor.clobbers != StubClobbers::ClobbersLikeCall)
        return false;
    if (axes.contains(StubAxis::OperandRegister) != (descriptor.operandRegisters != OperandRegisterChoice::NoChoice))
        return false;
    return axes.contains(StubAxis::Immediate) == (descriptor.binding != ImmediateBinding::Never);
}

// bodyOf() keeps the form, so form i of an entry is form i of its body.
constexpr bool hasFormsOfBody(const StubDescriptor& entry, const StubDescriptor& body)
{
    size_t numberOfFormNames = entry.formNames.size();
    if (numberOfFormNames > body.formNames.size())
        return false;
    return std::ranges::equal(entry.formNames, body.formNames.first(numberOfFormNames), [](ASCIILiteral name, ASCIILiteral nameInBody) {
        return std::ranges::equal(name.span(), nameInBody.span());
    });
}

constexpr bool isWithinBody(const StubDescriptor& entry, const StubDescriptor& body)
{
    return body.body == body.family
        && body.axes.containsAll(entry.axes)
        && body.axes.contains(StubAxis::NeverReturns) == entry.axes.contains(StubAxis::NeverReturns)
        && hasFormsOfBody(entry, body)
        && body.minCount <= entry.minCount
        && body.maxCount >= entry.maxCount
        && body.isas.containsAll(entry.isas)
        && body.framelessISAs.containsAll(entry.framelessISAs)
        && body.leafness == entry.leafness
        && body.clobbers == entry.clobbers
        && (entry.operandRegisters == OperandRegisterChoice::NoChoice || entry.operandRegisters == body.operandRegisters);
}

constexpr bool isWellFormed(StubFamily family)
{
    const StubDescriptor& descriptor = descriptorOf(family);
    return descriptor.family == family
        && hasConsistentColumns(descriptor)
        && isWithinBody(descriptor, descriptorOf(descriptor.body));
}

#define v(Name, ...) static_assert(isWellFormed(StubFamily::Name), "malformed line of a stub family list: " #Name);
#include "AOTStubFamilies.def"
#undef v

constexpr bool neverReturns(StubKey key)
{
    return descriptorOf(key.family()).axes.contains(StubAxis::NeverReturns);
}

constexpr bool isOperationCallHead(StubKey body)
{
    return body.immediate() && descriptorOf(body.family()).binding == ImmediateBinding::OperationOffset;
}

constexpr std::span<const GPRReg> operandRegistersOf(OperandRegisterChoice choice)
{
    switch (choice) {
    case OperandRegisterChoice::NoChoice:
        return { };
    case OperandRegisterChoice::AlternateReceivers:
        return alternateReceiverGPRs();
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool hasCallerFrameVariant(const StubDescriptor& descriptor, StubKey key)
{
    switch (key.callerFrame()) {
    case CallerFrame::HasFrame:
        return true;
    case CallerFrame::Frameless:
        return descriptor.framelessISAs.contains(currentStubISA);
    case CallerFrame::StubFrame:
        return descriptor.axes.contains(StubAxis::StubFrame) && !key.operandRegister();
    }
    return false;
}

constexpr bool hasOperandRegisterVariant(const StubDescriptor& descriptor, GPRReg reg)
{
    std::span<const GPRReg> registers = operandRegistersOf(descriptor.operandRegisters);
    return std::ranges::find(registers, reg) != registers.end();
}

constexpr bool exists(StubKey key)
{
    if (!key.hasFamily())
        return false;
    const StubDescriptor& descriptor = descriptorOf(key.family());
    if (!descriptor.isas.contains(currentStubISA))
        return false;
    if (key.form() >= descriptor.numberOfForms)
        return false;
    if (key.count() < descriptor.minCount || key.count() > descriptor.maxCount)
        return false;
    if (!hasCallerFrameVariant(descriptor, key))
        return false;
    std::optional<GPRReg> operandRegister = key.operandRegister();
    if (operandRegister && !hasOperandRegisterVariant(descriptor, *operandRegister))
        return false;
    if (key.immediate() && (operandRegister || !descriptor.axes.contains(StubAxis::Immediate)))
        return false;
    return true;
}

constexpr StubKey bodyOf(StubKey key)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(exists(key));
    return key.withFamily(descriptorOf(key.family()).body);
}

constexpr std::optional<StubKey> tryWithOperandRegister(StubKey key, GPRReg reg)
{
    StubKey candidate = key.withOperandRegister(reg);
    if (!exists(candidate))
        return std::nullopt;
    return candidate;
}

constexpr std::optional<StubKey> tryWithCallerFrame(StubKey key, CallerFrame callerFrame)
{
    StubKey candidate = key.withCallerFrame(callerFrame);
    if (!exists(candidate))
        return std::nullopt;
    return candidate;
}

void dump(PrintStream&, StubKey);

template<typename Functor>
void forEachRegisterVariant(StubKey key, const Functor& functor)
{
    if (!exists(key))
        return;
    functor(key);
    if (operandRegistersOf(descriptorOf(key.family()).operandRegisters).empty())
        return;
    // In register order, which is key order. The lists of AOTStubRegisters.h are in no order.
    for (Reg reg = Reg::first(); reg.isGPR(); reg = reg.next()) {
        if (std::optional<StubKey> variant = tryWithOperandRegister(key, reg.gpr()))
            functor(*variant);
    }
}

template<typename Functor>
void forEachKeyWithoutImmediate(const Functor& functor)
{
    constexpr std::array callerFrames { CallerFrame::HasFrame, CallerFrame::Frameless, CallerFrame::StubFrame };
    for (const StubDescriptor& descriptor : StubFamilyTokens::descriptors) {
        for (uint8_t form = 0; form < descriptor.numberOfForms; ++form) {
            for (unsigned count = descriptor.minCount; count <= descriptor.maxCount; ++count) {
                StubKey key = StubKey(descriptor.family).withForm(form).withCount(count);
                for (CallerFrame callerFrame : callerFrames)
                    forEachRegisterVariant(key.withCallerFrame(callerFrame), functor);
            }
        }
    }
}

template<typename Functor>
constexpr void forEachEntryOf(StubKey body, const Functor& functor)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(exists(body) && bodyOf(body) == body);
    for (const StubDescriptor& descriptor : StubFamilyTokens::descriptors) {
        if (descriptor.body != body.family())
            continue;
        StubKey candidate = body.withFamily(descriptor.family);
        if (exists(candidate))
            functor(candidate);
    }
}

static_assert(gprOf(StubRegister::T9) == countGPR && gprOf(StubRegister::T10) == calleeGPR);
static_assert(gprOf(stubImmediateRegister) == countGPR);
#if CPU(X86_64)
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, gprOf(StubRegister::T11), gprOf(StubRegister::T13), gprOf(StubRegister::A0), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3)));
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, gprOf(StubRegister::T11), gprOf(StubRegister::EntryT12), gprOf(StubRegister::EntryT13)));
#elif CPU(ARM64)
static_assert(noOverlap(thisGPR, countGPR, calleeGPR, gprOf(StubRegister::T11), gprOf(StubRegister::T12), gprOf(StubRegister::T13), gprOf(StubRegister::T14), gprOf(StubRegister::T15)));
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
