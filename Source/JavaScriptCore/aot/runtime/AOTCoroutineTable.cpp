/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutineTable.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "AOTStubsCoroutines.h"
#include "AbstractSlotVisitorInlines.h"
#include "AsyncGeneratorPrototype.h"
#include "GeneratorPrototype.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "SlotVisitorInlines.h"

namespace JSC { namespace AOT {

CoroutineTable::CoroutineTable(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_savedFrameEntrypoint(AOT::savedFrameEntrypoint().taggedPtr())
{
    VM& vm = *m_instance.vm;
    createStructures(vm);
    m_instance.coroutines.emptyGeneratorPrototypeStructureID = createEmptyPrototypeStructure(vm, m_instance.globalObject->generatorPrototype());
    m_instance.coroutines.emptyAsyncGeneratorPrototypeStructureID = createEmptyPrototypeStructure(vm, m_instance.globalObject->asyncGeneratorPrototype());
    state.caches->add(CacheKind::StrongRoots, "coroutine resume state"_s, *this, RootVolatility::GreyedByExecution);
    state.caches->add(CacheKind::WeakByKey, "saved frames under validation"_s, *this);
}

void CoroutineTable::createStructures(VM& vm)
{
    JSGlobalObject* globalObject = m_instance.globalObject;
    m_generatorStructure = Generator::createStructure(vm, globalObject, globalObject->generatorPrototype());
    m_state.immortalCells->add(m_generatorStructure);
    m_savedFrameStructure = SavedFrame::createStructure(vm, globalObject);
    m_state.immortalCells->add(m_savedFrameStructure);
    m_instance.coroutines.savedFrameHeader = cellHeaderFor(m_savedFrameStructure);
}

uint32_t CoroutineTable::createEmptyPrototypeStructure(VM& vm, JSObject* parent)
{
    JSObject* probe = constructEmptyObject(m_instance.globalObject, parent);
    probe->didBecomePrototype(vm);
    Structure* empty = probe->structure();
    m_state.immortalCells->add(empty);
    return empty->id().bits();
}

template<typename Visitor>
void CoroutineTable::visitRoots(Visitor& visitor)
{
    CoroutineWords& words = m_instance.coroutines;
    visitor.appendUnbarriered(words.waiterToResume);
    visitor.appendUnbarriered(JSValue::decode(words.waiterResolution));
    visitor.appendUnbarriered(JSValue::decode(words.waiterAsyncContext));
}

template void CoroutineTable::visitRoots(AbstractSlotVisitor&);
template void CoroutineTable::visitRoots(SlotVisitor&);

size_t CoroutineTable::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t numberOfDeadFrames = m_savedFramesForValidation.removeAllMatching([&](SavedFrame* frame) {
        return !vm.heap.isMarked(frame);
    });
    for (SavedFrame* frame : m_savedFramesForValidation)
        frame->poisonDeadUnlistedWords(vm);
    return numberOfDeadFrames;
}

void CoroutineTable::didCreateSavedFrame(VM& vm, SavedFrame* frame)
{
    if (validatesRuntime(vm)) [[unlikely]]
        m_savedFramesForValidation.append(frame);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
