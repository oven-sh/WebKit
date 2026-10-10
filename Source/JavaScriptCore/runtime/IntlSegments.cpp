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
#include "IntlSegments.h"

#include "IntlObjectInlines.h"
#include "IntlSegmentDataObject.h"
#include "IntlSegmentIterator.h"
#include "IntlWorkaround.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC {

const ClassInfo IntlSegments::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(IntlSegments) };

IntlSegments* IntlSegments::create(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>>&& buffer, JSString* string, IntlSegmenter::Granularity granularity)
{
    auto* object = new (NotNull, allocateCell<IntlSegments>(vm)) IntlSegments(vm, structure, WTF::move(segmenter), WTF::move(buffer), granularity, string);
    object->finishCreation(vm);
    return object;
}

Structure* IntlSegments::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

IntlSegments::IntlSegments(VM& vm, Structure* structure, std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& segmenter, Box<Vector<char16_t>>&& buffer, IntlSegmenter::Granularity granularity, JSString* string)
    : Base(vm, structure)
    , m_segmenter(WTF::move(segmenter))
    , m_buffer(WTF::move(buffer))
    , m_string(string, WriteBarrierEarlyInit)
    , m_granularity(granularity)
{
}

#if USE(BUN_JSC_ADDITIONS)
Box<Vector<char16_t>> IntlSegments::copyCharactersOf(JSGlobalObject* globalObject, JSString* cell)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto string = cell->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto characters = string->charactersWithoutNullTermination();
    if (!characters) {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return Box<Vector<char16_t>>::create(WTF::move(characters.value()));
}

bool IntlSegments::ensureBuffer(JSGlobalObject* globalObject)
{
    if (m_buffer)
        return true;
    m_buffer = copyCharactersOf(globalObject, m_string.get());
    if (!m_buffer)
        return false;
    m_extraMemory += m_buffer->sizeInBytes();
    globalObject->vm().heap.reportExtraMemoryAllocated(this, m_buffer->sizeInBytes());
    return true;
}
#endif

// https://tc39.es/proposal-intl-segmenter/#sec-intl.segmenter.prototype.containing
JSValue IntlSegments::containing(JSGlobalObject* globalObject, JSValue indexValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    double value = indexValue.toIntegerOrInfinity(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

#if USE(BUN_JSC_ADDITIONS)
    if (value < 0 || value >= m_string->length())
        return jsUndefined();
#else
    if (value < 0 || value >= m_buffer->size())
        return jsUndefined();
#endif
    int32_t index = toInt32(value);

#if USE(BUN_JSC_ADDITIONS)
    if (m_owner->cutsSimpleGraphemesItself()) {
        auto view = m_string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned place = index;
        bool beginsThere = !place || IntlSegmenter::areInDifferentGraphemeClustersForCertain(view[place - 1], view[place]);
        bool endsBehindIt = place + 1 == view->length() ? IntlSegmenter::joinsNoNeighbour(view[place]) : IntlSegmenter::areInDifferentGraphemeClustersForCertain(view[place], view[place + 1]);
        if (beginsThere && endsBehindIt)
            RELEASE_AND_RETURN(scope, createSegmentDataObject(globalObject, m_string.get(), index, index + 1, m_owner->breakIterator(), m_granularity));
    }
    if (!ensureBuffer(globalObject))
        return { };
    if (!m_segmenter) {
        UErrorCode status = U_ZERO_ERROR;
        bool isClone;
        auto breakIterator = m_owner->takeBreakIterator(status, isClone);
        if (U_SUCCESS(status))
            ubrk_setText(breakIterator.get(), m_buffer->span().data(), m_buffer->size(), &status);
        if (U_FAILURE(status)) {
            throwTypeError(globalObject, scope, "failed to initialize Segments"_s);
            return { };
        }
        m_segmenter = WTF::move(breakIterator);
        m_extraMemory += IntlSegmenter::estimatedSizeOfBreakIterator;
        if (isClone)
            vm.heap.reportExtraMemoryAllocated(this, IntlSegmenter::estimatedSizeOfBreakIterator);
    }
#endif

    // ubrk_following moves a trail surrogate back to its lead, then to the next boundary.
    // ubrk_previous is the start of the segment that contains index. Step forward again:
    // isWordLike reads the rule status of the boundary that ends the segment.
    int32_t endIndex = ubrk_following(m_segmenter.get(), index);
    if (endIndex == UBRK_DONE)
        endIndex = m_buffer->size();
    int32_t startIndex = ubrk_previous(m_segmenter.get());
    if (startIndex == UBRK_DONE)
        startIndex = 0;
    else {
        int32_t restoredEnd = ubrk_next(m_segmenter.get());
        if (restoredEnd != UBRK_DONE)
            endIndex = restoredEnd;
    }

    scope.release();
    return createSegmentDataObject(globalObject, m_string.get(), startIndex, endIndex, *m_segmenter, m_granularity);
}

// https://tc39.es/proposal-intl-segmenter/#sec-%segmentsprototype%-@@iterator
JSObject* IntlSegments::createSegmentIterator(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    // It takes a break iterator when it has to ask one: IntlSegmentIterator::nextGrapheme().
    if (m_owner->cutsSimpleGraphemesItself()) {
        auto* iterator = IntlSegmentIterator::create(vm, globalObject->segmentIteratorStructure(), nullptr, m_buffer, m_string.get(), m_granularity);
        iterator->setSegmenter(vm, m_owner.get());
        return iterator;
    }
    if (!ensureBuffer(globalObject))
        return nullptr;
#endif
    UErrorCode status = U_ZERO_ERROR;
#if USE(BUN_JSC_ADDITIONS)
    bool isClone;
    auto segmenter = m_owner->takeBreakIterator(status, isClone);
    if (U_SUCCESS(status))
        ubrk_setText(segmenter.get(), m_buffer->span().data(), m_buffer->size(), &status);
#else
    auto segmenter = std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>(cloneUBreakIterator(m_segmenter.get(), &status));
#endif
    if (U_FAILURE(status)) {
        throwTypeError(globalObject, scope, "failed to initialize SegmentIterator"_s);
        return nullptr;
    }

    ubrk_first(segmenter.get());
#if USE(BUN_JSC_ADDITIONS)
    auto* iterator = IntlSegmentIterator::create(vm, globalObject->segmentIteratorStructure(), WTF::move(segmenter), m_buffer, m_string.get(), m_granularity);
    iterator->setSegmenter(vm, m_owner.get());
    if (isClone)
        vm.heap.reportExtraMemoryAllocated(iterator, IntlSegmenter::estimatedSizeOfBreakIterator);
    return iterator;
#else
    return IntlSegmentIterator::create(vm, globalObject->segmentIteratorStructure(), WTF::move(segmenter), m_buffer, m_string.get(), m_granularity);
#endif
}

template<typename Visitor>
void IntlSegments::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<IntlSegments>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_string);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_owner);
    visitor.reportExtraMemoryVisited(thisObject->m_extraMemory);
#endif
}

DEFINE_VISIT_CHILDREN(IntlSegments);

} // namespace JSC
