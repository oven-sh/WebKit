/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "JSCell.h"
#include "JSPromise.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class ReactionChain final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static ReactionChain* create(VM&, Structure*, uint32_t description);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static constexpr unsigned maxHandlers = 8;
    static constexpr unsigned maxWords = 4;
    static constexpr unsigned maxWordsInRegisters = 3;
    static constexpr unsigned maxCapturesOfHandler = 4;

    static constexpr unsigned lastHandlerShift = ClosureWord::numberOfFunctionBits;
    static constexpr unsigned lastHandlerBits = 3;
    static constexpr unsigned numberOfWordsShift = lastHandlerShift + lastHandlerBits;
    static_assert(maxHandlers == 1 << lastHandlerBits && numberOfWordsShift + 3 < 31);
    static constexpr uint32_t description(unsigned numberOfHandlers, unsigned numberOfWords) { return (numberOfHandlers - 1) << lastHandlerShift | numberOfWords << numberOfWordsShift; }

    static constexpr unsigned bitsOfHandler = 16;
    static constexpr unsigned bitsOfCapture = 4;
    static constexpr unsigned captureMask = 7;
    static constexpr uint16_t isRejectHandlerBit = 1 << 15;
    static constexpr unsigned handlersPerWord = 64 / bitsOfHandler;
    static_assert(maxCapturesOfHandler * bitsOfCapture == bitsOfHandler && maxWords <= captureMask);

    JSPromise* promise() const { return m_promise.get(); }
    bool isAtLastHandler() const { return m_index == lastHandler(); }
    bool handlesStatus(JSPromise::Status status) const { return !!(handlerBits(m_index) & isRejectHandlerBit) == (status == JSPromise::Status::Rejected); }
    void advance() { WTF::atomicStore<MemoryOrder::Relaxed>(&m_index, m_index + 1); }
    JSObject* createHandler(VM&) const;
    JSPromise* createLink(VM&);
    JS_EXPORT_PRIVATE void settle(VM&, JSValue, JSPromise::Status);
    void startAt(VM&, JSPromise*);

    static constexpr size_t allocationSize(unsigned numberOfWords) { return sizeof(ReactionChain) + numberOfWords * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfPromise() { return OBJECT_OFFSETOF(ReactionChain, m_promise); }
    static constexpr ptrdiff_t offsetOfDescription() { return OBJECT_OFFSETOF(ReactionChain, m_description); }
    static constexpr ptrdiff_t offsetOfIndex() { return OBJECT_OFFSETOF(ReactionChain, m_index); }
    static constexpr ptrdiff_t offsetOfHandlers() { return OBJECT_OFFSETOF(ReactionChain, m_handlers); }
    static constexpr ptrdiff_t offsetOfWords() { return sizeof(ReactionChain); }

private:
    ReactionChain(VM& vm, Structure* structure, uint32_t description)
        : Base(vm, structure)
        , m_description(description)
    {
    }

    unsigned lastHandler() const { return m_description >> lastHandlerShift & ((1 << lastHandlerBits) - 1); }
    unsigned numberOfWords() const { return m_description >> numberOfWordsShift; }
    uint32_t closureNumber() const { return (m_description & ClosureWord::functionMask) + m_index; }
    uint16_t handlerBits(unsigned index) const { return static_cast<uint16_t>(m_handlers[index / handlersPerWord] >> bitsOfHandler * (index % handlersPerWord)); }
    const EncodedJSValue* words() const { return reinterpret_cast<const EncodedJSValue*>(this + 1); }
    EncodedJSValue* words() { return reinterpret_cast<EncodedJSValue*>(this + 1); }
    void queueStep(VM&, JSValue, JSPromise::Status);

    WriteBarrier<JSPromise> m_promise;
    uint32_t m_description;
    uint32_t m_index { 0 };
    uint64_t m_handlers[maxHandlers / handlersPerWord] { };
};
static_assert(sizeof(ReactionChain) == 40);

JS_EXPORT_PRIVATE bool takeStepOfReactionChain(VM&, JSValue chain, JSValue value, JSPromise::Status, JSValue& promise, JSValue& handler);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
