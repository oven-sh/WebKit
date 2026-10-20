/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubEntrypoints.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTStubsBoxedCalls.h"
#include "NativeFunction.h"
#include <bit>
#include <span>

namespace JSC { namespace AOT {

static_assert([] {
    for (StubFamily family : { StubFamily::EnterClosure, StubFamily::EnterSlimClosure, StubFamily::EnterClosureForConstruct }) {
        if (descriptorOf(family).linkage != StubLinkage::EnteredByRuntime)
            return false;
    }
    return descriptorOf(StubFamily::CallBoxedBuiltin).linkage == StubLinkage::Handler;
}(), "only a stub with one of these two linkages is in every image");

void* addressOfStubInCurrentImage(StubKey key)
{
    const Image* image = Image::current();
    RELEASE_ASSERT_WITH_MESSAGE(image, "No AOT image with code is registered.");
    const void* address = image->stubAddress(key);
    RELEASE_ASSERT_WITH_MESSAGE(address, "The AOT image has no such stub.");
    return const_cast<void*>(address);
}

CodePtr<JSEntryPtrTag> closureEntrypoint()
{
    static void* const entrypoint = tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(StubKey(StubFamily::EnterClosure)));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

CodePtr<JSEntryPtrTag> slimClosureEntrypoint()
{
    static void* const entrypoint = tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(StubKey(StubFamily::EnterSlimClosure)));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

CodePtr<JSEntryPtrTag> constructEntrypoint()
{
    static void* const entrypoint = tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(StubKey(StubFamily::EnterClosureForConstruct)));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

static BoxedCallEntry& findBoxedCallEntry(StubWords& words, const void* nativeFunction)
{
    std::span entries { words.boxedCalls };
    unsigned index = StubWords::indexOfBoxedCall(std::bit_cast<uintptr_t>(nativeFunction));
    while (entries[index].nativeFunction && entries[index].nativeFunction != nativeFunction)
        index = (index + 1) % StubWords::numberOfBoxedCallEntries;
    return entries[index];
}

void initializeStubWords(Instance& instance)
{
    unsigned numberOfUsedEntries = 0;
    for (const BuiltinBoxedCall& call : BuiltinTable::singleton().boxedCalls()) {
        const BuiltinNativeFunctionBinding* binding = BuiltinBindings::nativeFunctionOf(call.builtin);
        if (!binding || !binding->function)
            continue;

        const void* nativeFunction = TaggedNativeFunction(binding->function).taggedPtr();
        BoxedCallEntry& entry = findBoxedCallEntry(instance.stubs, nativeFunction);
        bool isFree = !entry.nativeFunction;
        if (isFree) {
            // A probe, here and in emitBoxedCallDispatch(), ends at a free entry.
            ++numberOfUsedEntries;
            RELEASE_ASSERT(numberOfUsedEntries * 2 <= StubWords::numberOfBoxedCallEntries);
            entry.nativeFunction = nativeFunction;
        }

        BoxedCallForm form = boxedCallFormOf(call.stub);
        std::span targets { entry.targets };
        for (unsigned count = BoxedCallEntry::minArgumentCount; count <= BoxedCallEntry::maxArgumentCount; ++count) {
            const void* handler = addressOfStubInCurrentImage(callBoxedBuiltin(form, count));
            const void*& target = targets[count - BoxedCallEntry::minArgumentCount];
            RELEASE_ASSERT(isFree || target == handler);
            target = handler;
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
