/*
 * Copyright (C) 2020-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IntlSegmentIterator.h"

#include "IntlSegmentDataObject.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC {

const ClassInfo IntlSegmentIterator::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(IntlSegmentIterator) };

IntlSegmentIterator* IntlSegmentIterator::create(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>> buffer, JSString* string, IntlSegmenter::Granularity granularity, const String& locale)
{
    auto* object = new (NotNull, allocateCell<IntlSegmentIterator>(vm)) IntlSegmentIterator(vm, structure, WTF::move(segmenter), WTF::move(buffer), granularity, string, locale);
    object->finishCreation(vm);
    return object;
}

Structure* IntlSegmentIterator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

IntlSegmentIterator::IntlSegmentIterator(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>>&& buffer, IntlSegmenter::Granularity granularity, JSString* string, const String& locale)
    : Base(vm, structure)
    , m_segmenter(WTF::move(segmenter))
    , m_buffer(WTF::move(buffer))
    , m_string(string, WriteBarrierEarlyInit)
    , m_granularity(granularity)
{
    m_locale = locale;
#if USE(BUN_JSC_ADDITIONS)
    m_startupSnapshotEpoch = vm.startupSnapshotEpoch();
#endif
}

void IntlSegmentIterator::ensureICUObjectsForThisProcess(JSGlobalObject* globalObject)
{
#if USE(BUN_JSC_ADDITIONS)
    VM& vm = globalObject->vm();
    if (m_startupSnapshotEpoch == vm.startupSnapshotEpoch()) [[likely]]
        return;
    (void)m_segmenter.release(); // opened by the process that built the snapshot: released, never closed
    UErrorCode status = U_ZERO_ERROR;
    m_segmenter = std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>(ubrk_open(IntlSegmenter::breakIteratorTypeFor(m_granularity), m_locale.utf8().data(), nullptr, 0, &status));
    if (m_segmenter) {
        ubrk_setText(m_segmenter.get(), m_buffer->span().data(), m_buffer->size(), &status);
        ubrk_isBoundary(m_segmenter.get(), m_position); // m_position is always a boundary (or 0), so this puts the iterator exactly there
    }
    if (U_SUCCESS(status))
        m_startupSnapshotEpoch = vm.startupSnapshotEpoch();
#else
    UNUSED_PARAM(globalObject);
#endif
}

template<typename Visitor>
void IntlSegmentIterator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<IntlSegmentIterator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_string);
}

DEFINE_VISIT_CHILDREN(IntlSegmentIterator);

JSObject* IntlSegmentIterator::next(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ensureICUObjectsForThisProcess(globalObject);
    int32_t startIndex = ubrk_current(m_segmenter.get());
    int32_t endIndex = ubrk_next(m_segmenter.get());
    if (endIndex == UBRK_DONE)
        return createIteratorResultObject(globalObject, jsUndefined(), true);
    m_position = endIndex;
    JSObject* object = createSegmentDataObject(globalObject, m_string.get(), startIndex, endIndex, *m_segmenter, m_granularity);
    RETURN_IF_EXCEPTION(scope, { });
    return createIteratorResultObject(globalObject, object, false);
}

} // namespace JSC
