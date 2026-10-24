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
    for (StubFamily family : { StubFamily::EnterClosure, StubFamily::EnterSlimClosure, StubFamily::EnterModuleBody, StubFamily::EnterClosureForConstruct }) {
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

CodePtr<JSEntryPtrTag> entrypointInCurrentImage(Image::FrequentStub stub, StubKey key)
{
    const Image* image = Image::current();
    RELEASE_ASSERT_WITH_MESSAGE(image, "No AOT image with code is registered.");
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(image->frequentStub(stub, [&] {
        return tagCodePtr<JSEntryPtrTag>(addressOfStubInCurrentImage(key));
    }));
}

CodePtr<JSEntryPtrTag> closureEntrypoint()
{
    return entrypointInCurrentImage(Image::FrequentStub::EnterClosure, StubKey(StubFamily::EnterClosure));
}

CodePtr<JSEntryPtrTag> slimClosureEntrypoint()
{
    return entrypointInCurrentImage(Image::FrequentStub::EnterSlimClosure, StubKey(StubFamily::EnterSlimClosure));
}

CodePtr<JSEntryPtrTag> moduleBodyEntrypoint()
{
    return entrypointInCurrentImage(Image::FrequentStub::EnterModuleBody, StubKey(StubFamily::EnterModuleBody));
}

CodePtr<JSEntryPtrTag> constructEntrypoint()
{
    return entrypointInCurrentImage(Image::FrequentStub::EnterClosureForConstruct, StubKey(StubFamily::EnterClosureForConstruct));
}

static BoxedCallEntry& findBoxedCallEntry(std::span<BoxedCallEntry> entries, const void* nativeFunction)
{
    unsigned index = StubWords::indexOfBoxedCall(std::bit_cast<uintptr_t>(nativeFunction));
    while (entries[index].nativeFunction && entries[index].nativeFunction != nativeFunction)
        index = (index + 1) % StubWords::numberOfBoxedCallEntries;
    return entries[index];
}

void initializeStubWords(Instance& instance)
{
    instance.stubs.closureEntrypoint = closureEntrypoint().taggedPtr();
    instance.stubs.slimClosureEntrypoint = slimClosureEntrypoint().taggedPtr();

    constexpr unsigned numberOfCounts = BoxedCallEntry::maxArgumentCount - BoxedCallEntry::minArgumentCount + 1;
    const Image& image = *Image::current();
    const uint8_t* sharedCode = image.sharedCodeCopy(0);
    std::span<const StubAddressInImage> handlers = image.stubAddressesOf(StubFamily::CallBoxedBuiltin);
    RELEASE_ASSERT_WITH_MESSAGE(handlers.size() == descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms * numberOfCounts, "The AOT image has no such stub.");

    unsigned numberOfUsedEntries = 0;
    for (const BuiltinBoxedCall& call : BuiltinTable::singleton().boxedCalls()) {
        std::optional<BuiltinNativeFunction> binding = BuiltinBindings::nativeFunctionOf(call.builtin);
        if (!binding || !binding->function)
            continue;

        const void* nativeFunction = TaggedNativeFunction(binding->function).taggedPtr();
        BoxedCallEntry& entry = findBoxedCallEntry(instance.boxedCalls(), nativeFunction);
        bool isFree = !entry.nativeFunction;
        if (isFree) {
            // A probe, here and in FindBoxedCall, ends at a free entry.
            ++numberOfUsedEntries;
            RELEASE_ASSERT(numberOfUsedEntries * 2 <= StubWords::numberOfBoxedCallEntries);
            entry.nativeFunction = nativeFunction;
        }

        BoxedCallForm form = boxedCallFormOf(call.stub);
        std::span handlersOfForm = handlers.subspan(static_cast<unsigned>(form) * numberOfCounts, numberOfCounts);
        std::span targets { entry.targets };
        for (unsigned index = 0; index < numberOfCounts; ++index) {
            ASSERT(handlersOfForm[index].key == callBoxedBuiltin(form, BoxedCallEntry::minArgumentCount + index).bits());
            const void* handler = sharedCode + handlersOfForm[index].offset;
            RELEASE_ASSERT(isFree || targets[index] == handler);
            targets[index] = handler;
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
