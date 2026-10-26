/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTRuntimeTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrontEnds.h"
#include "RegisterSet.h"
#include <iterator>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

class StubCatalog;

struct OperationCallShape {
    OperationCall::Returns returns;
    OperationCall::FirstArgument firstArgument;
    OperationCall::Saves saves;
    bool checksException;

    friend constexpr bool operator==(const OperationCallShape&, const OperationCallShape&) = default;
};

inline constexpr OperationCallShape operationCallShapes[13] = {
    { OperationCall::Returns::Value, OperationCall::FirstArgument::Instance, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::GlobalObject, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::VM, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::None, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Void, OperationCall::FirstArgument::Instance, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Void, OperationCall::FirstArgument::GlobalObject, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Void, OperationCall::FirstArgument::VM, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Void, OperationCall::FirstArgument::None, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::Instance, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::GlobalObject, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::VM, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::None, OperationCall::Saves::Nothing, true },
    { OperationCall::Returns::Pair, OperationCall::FirstArgument::Instance, OperationCall::Saves::Nothing, true },
};

inline constexpr OperationCallShape preservingCallShapes[9] = {
    { OperationCall::Returns::Void, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedRegisters, true },
    { OperationCall::Returns::Void, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedGPRs, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedRegisters, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedGPRs, true },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedRegisters, false },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedGPRs, false },
    { OperationCall::Returns::Value, OperationCall::FirstArgument::None, OperationCall::Saves::CallerSavedRegisters, false },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::None, OperationCall::Saves::CallerSavedRegisters, false },
    { OperationCall::Returns::Double, OperationCall::FirstArgument::Instance, OperationCall::Saves::CallerSavedRegisters, true },
};

constexpr std::optional<uint8_t> findOperationCallShape(std::span<const OperationCallShape> shapes, const OperationCallShape& shape)
{
    for (size_t index = 0; index < shapes.size(); ++index) {
        if (shapes[index] == shape)
            return static_cast<uint8_t>(index);
    }
    return std::nullopt;
}

// Only for an operation that can throw: any other leaves the register that the stub tests for an exception undefined.
constexpr StubKey callOperation(OperationCall::Returns returns, OperationCall::FirstArgument firstArgument)
{
    std::optional<uint8_t> form = findOperationCallShape(operationCallShapes, { returns, firstArgument, OperationCall::Saves::Nothing, true });
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(form);
    return StubKey(StubFamily::CallOperation).withForm(*form);
}

constexpr StubKey tailCallOperation(OperationCall::FirstArgument firstArgument)
{
    return StubKey(StubFamily::TailCallOperation).withForm(static_cast<uint8_t>(firstArgument));
}

constexpr StubKey callOperationFor(Entry entry, OperationCall::Returns returns, OperationCall::FirstArgument firstArgument)
{
    return effectsOf(entry).mayThrow ? callOperation(returns, firstArgument) : tailCallOperation(firstArgument);
}

// The stub may save and test more than the call asks for. A void call without a test does not get returnValueGPR back.
constexpr std::optional<StubKey> tryCallOperationPreservingRegisters(const OperationCall& call)
{
    if (call.secondArgumentIn || call.thirdArgumentIn || call.fourthArgumentIn || call.fifthArgumentIn || call.valueDestination || call.spilledArguments)
        return std::nullopt;
    if (call.constantArguments[0] || call.constantArguments[1])
        return std::nullopt;
    if (call.saves == OperationCall::Saves::Nothing)
        return std::nullopt;
    if (call.returns == OperationCall::Returns::Double && call.saves == OperationCall::Saves::CallerSavedGPRs)
        return std::nullopt;

    OperationCallShape shape { call.returns, call.firstArgument, call.saves, call.checksException };
    switch (call.firstArgument) {
    case OperationCall::FirstArgument::GlobalObject:
    case OperationCall::FirstArgument::VM:
        return std::nullopt;
    case OperationCall::FirstArgument::None:
        if (call.checksException)
            return std::nullopt;
        shape.saves = OperationCall::Saves::CallerSavedRegisters;
        break;
    case OperationCall::FirstArgument::Instance:
        if (call.returns == OperationCall::Returns::Double)
            shape.checksException = true;
        break;
    }
    if (shape.returns == OperationCall::Returns::Void && !shape.checksException)
        shape.returns = OperationCall::Returns::Value;

    std::optional<uint8_t> form = findOperationCallShape(preservingCallShapes, shape);
    if (!form)
        return std::nullopt;
    return StubKey(StubFamily::CallOperationPreservingRegisters).withForm(*form);
}

constexpr StubKey callOperationPreservingRegisters(const OperationCall& call)
{
    std::optional<StubKey> key = tryCallOperationPreservingRegisters(call);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(key);
    return *key;
}

// Over every variant a site may pick once it knows which registers are live. OperationCall::saves is ignored.
RegisterSet registersClobberedByPreservingCall(const StubCatalog&, OperationCall);

static_assert(!static_cast<uint32_t>(ThrowSiteSource::WordsAfterCall) && static_cast<uint32_t>(ThrowSiteSource::WordInRegister) == 1);

constexpr StubKey throwAtSite(ThrowSiteSource source, CallerFrame callerFrame)
{
    switch (source) {
    case ThrowSiteSource::WordsAfterCall:
    case ThrowSiteSource::WordInRegister:
        ASSERT_UNDER_CONSTEXPR_CONTEXT(callerFrame == CallerFrame::HasFrame);
        return StubKey(StubFamily::ThrowAtSite).withForm(static_cast<uint8_t>(source));
    case ThrowSiteSource::FaultSite:
        return StubKey(StubFamily::ThrowAtFault).withCallerFrame(callerFrame);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

PreludeExit emitCatchPrelude(StubAssembler&);
PreludeExit emitThrowPrelude(StubAssembler&);
PreludeExit emitThrowAfterCleanupPrelude(StubAssembler&);

static_assert(std::size(operationCallShapes) == descriptorOf(StubFamily::CallOperation).numberOfForms);
static_assert(std::size(preservingCallShapes) == descriptorOf(StubFamily::CallOperationPreservingRegisters).numberOfForms);
static_assert(exists(tailCallOperation(OperationCall::FirstArgument::None)) && !exists(tailCallOperation(OperationCall::FirstArgument::None).withForm(4)));
static_assert(exists(throwAtSite(ThrowSiteSource::FaultSite, CallerFrame::Frameless)));
static_assert(exists(throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame)));
static_assert(!tryCallOperationPreservingRegisters({ .firstArgument = OperationCall::FirstArgument::VM, .saves = OperationCall::Saves::CallerSavedRegisters }));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
