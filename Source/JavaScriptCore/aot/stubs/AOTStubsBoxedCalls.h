/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"
#include "AOTStubsCalls.h"

namespace JSC { namespace AOT {

using BoxedCallForm = BuiltinBoxedStub;

constexpr BoxedCallForm boxedCallFormOf(BuiltinBoxedStub stub) { return stub; }
bool acceptsArgumentCount(BoxedCallForm, unsigned count);

constexpr StubKey callBoxedBuiltin(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallBoxedBuiltin).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey tryCallMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::TryCallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassAtSite(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSite).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassAtSiteWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSiteWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassByValue(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassByValue).withForm(static_cast<uint8_t>(form)).withCount(count); }
bool leavesPatternAsMade(BoxedCallForm, unsigned count);
constexpr std::optional<BoxedCallForm> formAskedBehind(BoxedCallForm form)
{
    switch (form) {
    case BoxedCallForm::ArrayIncludes: return BoxedCallForm::StringIncludes;
    case BoxedCallForm::ArrayIndexOf: return BoxedCallForm::StringIndexOf;
    case BoxedCallForm::ArraySlice: return BoxedCallForm::StringSlice;
    case BoxedCallForm::MapClear: return BoxedCallForm::SetClear;
    case BoxedCallForm::MapDelete: return BoxedCallForm::SetDelete;
    case BoxedCallForm::MapGet: return BoxedCallForm::WeakMapGet;
    case BoxedCallForm::MapHas: return BoxedCallForm::SetHas;
    case BoxedCallForm::SetHas: return BoxedCallForm::WeakMapHas;
    case BoxedCallForm::WeakMapHas: return BoxedCallForm::WeakSetHas;
    case BoxedCallForm::MapSet: return BoxedCallForm::WeakMapSet;
    default: return std::nullopt;
    }
}
bool servesCallsByName(BoxedCallForm, unsigned count);
constexpr StubKey weakMapSet() { return StubKey(StubFamily::WeakMapSet); }
constexpr StubKey stringStartsWith() { return StubKey(StubFamily::StringStartsWith); }
constexpr StubKey stringIndexOf() { return StubKey(StubFamily::StringIndexOf); }
constexpr StubKey findBoxedCall() { return StubKey(StubFamily::FindBoxedCall); }

void emitBoxedCallDispatch(StubAssembler&, const CallRegisters&, unsigned count, JumpList& hasNoBoxedCall);

static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClass).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::TryCallMethodOfBuiltinClass).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClassAtSite).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClassByValue).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).minCount == BoxedCallEntry::minArgumentCount && descriptorOf(StubFamily::CallBoxedBuiltin).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(numberOfBuiltinBoxedStubs * 2 <= StubWords::numberOfBoxedCallEntries);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
