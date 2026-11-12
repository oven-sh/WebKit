/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReactionChain.h"

#if ENABLE(AOT)

#include "AOTClosureTable.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AsyncContextSwapScope.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo ReactionChain::s_info = { "ReactionChain"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ReactionChain) };

Structure* ReactionChain::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
}

ReactionChain* ReactionChain::create(VM& vm, Structure* structure, uint32_t description)
{
    unsigned numberOfWords = description >> numberOfWordsShift;
    RELEASE_ASSERT(numberOfWords <= maxWords);
    auto* chain = new (NotNull, allocateCell<ReactionChain>(vm, allocationSize(numberOfWords))) ReactionChain(vm, structure, description);
    for (unsigned word = 0; word < numberOfWords; ++word)
        chain->words()[word] = JSValue::encode(JSValue());
    chain->finishCreation(vm);
    return chain;
}

template<typename Visitor>
void ReactionChain::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* chain = uncheckedDowncast<ReactionChain>(cell);
    ASSERT_GC_OBJECT_INHERITS(chain, info());
    Base::visitChildren(chain, visitor);
    visitor.append(chain->m_promise);
    unsigned used = 0;
    for (unsigned index = WTF::atomicLoad<MemoryOrder::Relaxed>(&chain->m_index); index <= chain->lastHandler(); ++index) {
        for (unsigned captures = chain->handlerBits(index); captures & captureMask; captures >>= bitsOfCapture)
            used |= 1u << (captures & captureMask);
    }
    for (unsigned word = 0; word < chain->numberOfWords(); ++word) {
        if (used >> (word + 1) & 1)
            visitor.appendUnbarriered(JSValue::decode(chain->words()[word]));
    }
}

DEFINE_VISIT_CHILDREN(ReactionChain);

JSObject* ReactionChain::createHandler(VM& vm) const
{
    Instance& instance = *promise()->realm()->aotInstance();
    uint32_t number = closureNumber();
    unsigned numberOfCaptures = captureCountsOfClosureNumber(number).captures;
    auto* closure = SlimClosure::createForCompiledCode(vm, instance.state->objects->closures.slimClosureStructure(kindOfClosureNumber(number)), SlimClosure::allocationSize(numberOfCaptures));
    auto wordOfClosure = [&](ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(closure) + offset);
    };
    unsigned captures = handlerBits(m_index);
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot, captures >>= bitsOfCapture) {
        unsigned word = captures & captureMask;
        RELEASE_ASSERT(word);
        wordOfClosure(SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = static_cast<uint64_t>(words()[word - 1]);
    }
    WTF::storeStoreFence();
    wordOfClosure(SlimClosure::offsetOfWord()) = instance.functionTables.closureWords[number];
    WTF::storeStoreFence();
    return closure;
}

JSPromise* ReactionChain::createLink(VM& vm)
{
    JSPromise* link = JSPromise::create(vm, WTF::opaque(promise()->realm()->promiseStructure()));
    link->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledReactionChainStep, this, jsUndefined());
    return link;
}

void ReactionChain::queueStep(VM& vm, JSValue value, JSPromise::Status status)
{
    promise()->realm()->queueMicrotask(vm, InternalMicrotask::CompiledReactionChainStep, static_cast<uint8_t>(status), this, value, jsUndefined());
}

void ReactionChain::settle(VM& vm, JSValue value, JSPromise::Status status)
{
    if (status == JSPromise::Status::Rejected || !value.isObject()) {
        queueStep(vm, value, status);
        return;
    }
    createLink(vm)->resolvePromise(promise()->realm(), vm, value);
}

void ReactionChain::startAt(VM& vm, JSPromise* receiver)
{
    JSGlobalObject* realm = promise()->realm();
    JSValue context = AsyncContextSwapScope::current(vm, realm);
    if (!context || context.isUndefined()) [[likely]] {
        receiver->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledReactionChainStep, this, jsUndefined());
        return;
    }
    for (JSPromise* before = receiver;;) {
        bool isLast = isAtLastHandler();
        JSPromise* next = isLast ? promise() : JSPromise::create(vm, realm->promiseStructure());
        JSValue handler = createHandler(vm);
        bool handlesRejections = handlesStatus(JSPromise::Status::Rejected);
        before->performPromiseThen(vm, realm, handlesRejections ? jsUndefined() : handler, handlesRejections ? handler : jsUndefined(), next);
        if (isLast)
            return;
        advance();
        before = next;
    }
}

bool takeStepOfReactionChain(VM& vm, JSValue cell, JSValue value, JSPromise::Status status, JSValue& promise, JSValue& handler)
{
    auto* chain = uncheckedDowncast<ReactionChain>(cell.asCell());
    bool isLast = chain->isAtLastHandler();
    bool handlesStatus = chain->handlesStatus(status);
    if (handlesStatus)
        handler = chain->createHandler(vm);
    if (isLast) {
        promise = chain->promise();
        return handlesStatus;
    }
    chain->advance();
    if (handlesStatus)
        promise = chain->createLink(vm);
    else
        chain->settle(vm, value, status);
    return handlesStatus;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
