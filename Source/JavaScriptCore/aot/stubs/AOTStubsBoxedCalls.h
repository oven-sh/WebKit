/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTOperationData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCollections.h"

namespace JSC { namespace AOT {

using BoxedCallForm = BuiltinBoxedStub;

constexpr BoxedCallForm boxedCallFormOf(BuiltinBoxedStub stub) { return stub; }
bool acceptsArgumentCount(BoxedCallForm, unsigned count);

constexpr StubKey callBoxedBuiltin(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallBoxedBuiltin).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey tryCallMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::TryCallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassAtSite(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSite).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callFunctionOfBuiltinAtSite(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallFunctionOfBuiltinAtSite).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassAtSiteWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSiteWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassByValue(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassByValue).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfProvenBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfProvenBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfProvenBuiltinClassByValue(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfProvenBuiltinClassByValue).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodThatMayBeUnread(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodThatMayBeUnread).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodThatMayBeUnreadInTailPosition(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodThatMayBeUnreadInTailPosition).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfAnyReceiverThatMayBeUnread(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfAnyReceiverThatMayBeUnread).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey readMethodAtSiteUnlessReceiverFindsIt(BoxedCallForm form, bool someLayoutHasField) { return StubKey(StubFamily::ReadMethodAtSiteUnlessReceiverFindsIt).withForm(static_cast<uint8_t>(form)).withCount(someLayoutHasField); }
constexpr std::optional<TableMethod> tableMethodOf(BoxedCallForm form)
{
    switch (form) {
    case BoxedCallForm::MapDelete: return TableMethod::MapDelete;
    case BoxedCallForm::MapGet: return TableMethod::MapGet;
    case BoxedCallForm::MapHas: return TableMethod::MapHas;
    case BoxedCallForm::MapSet: return TableMethod::MapSet;
    case BoxedCallForm::SetAdd: return TableMethod::SetAdd;
    case BoxedCallForm::SetDelete: return TableMethod::SetDelete;
    case BoxedCallForm::SetHas: return TableMethod::SetHas;
    default: return std::nullopt;
    }
}
std::optional<JSTypeRange> cellTypesOfReceiversOf(BoxedCallForm);
bool looksWhetherReceiverIsAsMade(BoxedCallForm);
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
constexpr std::optional<HostResult> checkedResultOf(BoxedCallForm form)
{
    switch (form) {
    case BoxedCallForm::ArrayIncludes:
    case BoxedCallForm::ArrayIsArray:
    case BoxedCallForm::IsNaN:
    case BoxedCallForm::MapDelete:
    case BoxedCallForm::MapHas:
    case BoxedCallForm::ObjectHasOwnProperty:
    case BoxedCallForm::ObjectIs:
    case BoxedCallForm::ObjectIsExtensible:
    case BoxedCallForm::ObjectIsFrozen:
    case BoxedCallForm::ObjectIsSealed:
    case BoxedCallForm::RegExpTest:
    case BoxedCallForm::SetDelete:
    case BoxedCallForm::SetHas:
    case BoxedCallForm::StringEndsWith:
    case BoxedCallForm::StringIncludes:
    case BoxedCallForm::StringIsWellFormed:
    case BoxedCallForm::StringStartsWith:
    case BoxedCallForm::WeakMapHas:
    case BoxedCallForm::WeakSetHas:
        return HostResult::Boolean;
    case BoxedCallForm::StringCharAt:
    case BoxedCallForm::StringSlice:
    case BoxedCallForm::StringSubstr:
    case BoxedCallForm::StringSubstring:
    case BoxedCallForm::StringToLowerCase:
    case BoxedCallForm::StringToUpperCase:
    case BoxedCallForm::StringTrim:
    case BoxedCallForm::StringTrimEnd:
    case BoxedCallForm::StringTrimStart:
        return HostResult::String;
    default:
        return std::nullopt;
    }
}
bool servesCallsByName(BoxedCallForm, unsigned count);
bool fetchesFunctionOnlyToLeave(BoxedCallForm, unsigned count);
StubKey hostCallLeftTo(BoxedCallForm, unsigned count);
constexpr StubKey weakMapSet() { return StubKey(StubFamily::WeakMapSet); }
constexpr StubKey stringStartsWith() { return StubKey(StubFamily::StringStartsWith); }
constexpr StubKey stringIndexOf() { return StubKey(StubFamily::StringIndexOf); }
constexpr StubKey findBoxedCall() { return StubKey(StubFamily::FindBoxedCall); }

void emitBoxedCallDispatch(StubAssembler&, const CallRegisters&, unsigned count, JumpList& hasNoBoxedCall);

static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClass).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::TryCallMethodOfBuiltinClass).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClassAtSite).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallFunctionOfBuiltinAtSite).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfBuiltinClassByValue).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfProvenBuiltinClass).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfProvenBuiltinClassByValue).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodThatMayBeUnread).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodOfAnyReceiverThatMayBeUnread).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallMethodThatMayBeUnreadInTailPosition).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::ReadMethodAtSiteUnlessReceiverFindsIt).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).minCount == BoxedCallEntry::minArgumentCount && descriptorOf(StubFamily::CallBoxedBuiltin).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(numberOfBuiltinBoxedStubs * 4 <= StubWords::numberOfBoxedCallEntries * 3);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
