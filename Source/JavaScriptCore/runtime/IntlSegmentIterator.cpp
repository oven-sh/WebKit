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
#include "IntlSegments.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC {

const ClassInfo IntlSegmentIterator::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(IntlSegmentIterator) };

IntlSegmentIterator* IntlSegmentIterator::create(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>> buffer, JSString* string, IntlSegmenter::Granularity granularity)
{
    auto* object = new (NotNull, allocateCell<IntlSegmentIterator>(vm)) IntlSegmentIterator(vm, structure, WTF::move(segmenter), WTF::move(buffer), granularity, string);
    object->finishCreation(vm);
#if USE(BUN_JSC_ADDITIONS)
    if (object->m_segmenter)
        object->m_extraMemory = IntlSegmenter::estimatedSizeOfBreakIterator;
#endif
    return object;
}

Structure* IntlSegmentIterator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

IntlSegmentIterator::IntlSegmentIterator(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>>&& buffer, IntlSegmenter::Granularity granularity, JSString* string)
    : Base(vm, structure)
    , m_segmenter(WTF::move(segmenter))
    , m_buffer(WTF::move(buffer))
    , m_string(string, WriteBarrierEarlyInit)
    , m_granularity(granularity)
{
}

template<typename Visitor>
void IntlSegmentIterator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<IntlSegmentIterator>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_string);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_owner);
    visitor.reportExtraMemoryVisited(thisObject->m_extraMemory);
#endif
}

DEFINE_VISIT_CHILDREN(IntlSegmentIterator);

#if USE(BUN_JSC_ADDITIONS)
JSObject* IntlSegmentIterator::nextGrapheme(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSString* string = m_string.get();
    unsigned length = string->length();
    unsigned start = m_position;
    if (start >= length) {
        if (m_segmenter) {
            m_owner->giveBack(WTF::move(m_segmenter));
            m_extraMemory -= IntlSegmenter::estimatedSizeOfBreakIterator;
        }
        return nullptr;
    }
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned end = start + 1;
    if (end == length ? !IntlSegmenter::joinsNoNeighbour(view[start]) : !IntlSegmenter::areInDifferentGraphemeClustersForCertain(view[start], view[end])) {
        if (!m_segmenter) {
            if (!m_buffer) {
                m_buffer = IntlSegments::copyCharactersOf(globalObject, string);
                RETURN_IF_EXCEPTION(scope, nullptr);
                m_extraMemory += m_buffer->sizeInBytes();
                vm.heap.reportExtraMemoryAllocated(this, m_buffer->sizeInBytes());
            }
            UErrorCode status = U_ZERO_ERROR;
            bool isClone;
            auto breakIterator = m_owner->takeBreakIterator(status, isClone);
            if (U_SUCCESS(status))
                ubrk_setText(breakIterator.get(), m_buffer->span().data(), m_buffer->size(), &status);
            if (U_FAILURE(status)) {
                throwTypeError(globalObject, scope, "failed to initialize SegmentIterator"_s);
                return nullptr;
            }
            m_segmenter = WTF::move(breakIterator);
            m_positionOfBreakIterator = 0;
            m_extraMemory += IntlSegmenter::estimatedSizeOfBreakIterator;
            if (isClone)
                vm.heap.reportExtraMemoryAllocated(this, IntlSegmenter::estimatedSizeOfBreakIterator);
        }
        int32_t following = m_positionOfBreakIterator == start ? ubrk_next(m_segmenter.get()) : ubrk_following(m_segmenter.get(), start);
        end = following == UBRK_DONE ? length : following;
        m_positionOfBreakIterator = end;
    }
    m_position = end;
    RELEASE_AND_RETURN(scope, createSegmentDataObject(globalObject, string, start, end, m_owner->breakIterator(), m_granularity));
}
#endif

JSObject* IntlSegmentIterator::next(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    if (m_owner->cutsSimpleGraphemesItself()) {
        JSObject* data = nextGrapheme(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, createIteratorResultObject(globalObject, data ? JSValue(data) : jsUndefined(), !data));
    }
    if (!m_segmenter)
        return createIteratorResultObject(globalObject, jsUndefined(), true);
#endif
    int32_t startIndex = ubrk_current(m_segmenter.get());
    int32_t endIndex = ubrk_next(m_segmenter.get());
#if USE(BUN_JSC_ADDITIONS)
    if (endIndex == UBRK_DONE) {
        m_owner->giveBack(WTF::move(m_segmenter));
        m_extraMemory -= IntlSegmenter::estimatedSizeOfBreakIterator;
    }
#endif
    if (endIndex == UBRK_DONE)
        return createIteratorResultObject(globalObject, jsUndefined(), true);
    JSObject* object = createSegmentDataObject(globalObject, m_string.get(), startIndex, endIndex, *m_segmenter, m_granularity);
    RETURN_IF_EXCEPTION(scope, { });
    return createIteratorResultObject(globalObject, object, false);
}

} // namespace JSC
