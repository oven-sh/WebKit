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
// `table.get(key)` as one call: takes what CallMethodById takes, the name too. A receiver of the class that is as its class makes it finds the built-in under the name, and the stub goes on as the boxed call of that built-in does.
// Every other receiver, and whatever the boxed call would leave to the host function, goes on to CallMethodById.
constexpr StubKey callMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
// `return table.get(key)`: the same, in front of the read and the tail call as they are written. It takes no name, and answers the empty value, with nothing done, where the other goes on to CallMethodById.
constexpr StubKey tryCallMethodOfBuiltinClass(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::TryCallMethodOfBuiltinClass).withForm(static_cast<uint8_t>(form)).withCount(count); }
// `x.get(key)`, nothing known of `x`: an entry of the same stub that takes what CallMethodAtSite takes. A receiver of another class goes on to the entry of formAskedBehind(), and behind the last to CallMethodAtSite.
// So whoever names a form has seen to it that each form behind it is what its class gives under the same key.
constexpr StubKey callMethodOfBuiltinClassAtSite(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSite).withForm(static_cast<uint8_t>(form)).withCount(count); }
// `text.replace(/a/g, "b")`, nothing proven of `text`: the same two, handed as their first argument the one object that all evaluations of the literal share.
// Where they go on by the name the program may tell the evaluations apart, so an object of its own takes the place of the shared one first.
constexpr StubKey callMethodOfBuiltinClassWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey callMethodOfBuiltinClassAtSiteWithSharedPattern(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassAtSiteWithSharedPattern).withForm(static_cast<uint8_t>(form)).withCount(count); }
// `list.push(f(x))`: the method was read off the receiver further up, and what was read is at hand: takes what CallValue takes. A receiver of the class that is as its class makes it was so at the read, so what was read is the built-in,
// and the stub goes on as the boxed call of that built-in does. A receiver of another class goes on to the stub of formAskedBehind(), and behind the last to CallValue.
constexpr StubKey callMethodOfBuiltinClassByValue(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallMethodOfBuiltinClassByValue).withForm(static_cast<uint8_t>(form)).withCount(count); }
// What the stub does itself with a pattern that is its first argument hands the object to nobody, and leaves one that is not sticky as it was made.
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
// The handler neither reads the callee, which nobody has looked up, nor writes the register of the name.
bool servesCallsByName(BoxedCallForm, unsigned count);
constexpr StubKey weakMapSet() { return StubKey(StubFamily::WeakMapSet); }
constexpr StubKey stringStartsWith() { return StubKey(StubFamily::StringStartsWith); }
constexpr StubKey stringIndexOf() { return StubKey(StubFamily::StringIndexOf); }
constexpr StubKey findBoxedCall() { return StubKey(StubFamily::FindBoxedCall); }

// CallRegisters::scratch holds the NativeExecutable of the callee, and still holds it at hasNoBoxedCall.
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
