/*
 *  Copyright (C) 1999-2002 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2004-2021 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "JSString.h"

#include "JSGlobalObjectFunctions.h"
#include "JSGlobalObjectInlines.h"
#include "JSObjectInlines.h"
#include "StringObject.h"
#include "StrongInlines.h"
#include "StructureCreateInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

const ClassInfo JSString::s_info = { "string"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSString) };

Structure* JSString::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, defaultTypeInfo(), info());
}

JSString* JSString::createEmptyString(VM& vm)
{
    JSString* newString = new (NotNull, allocateCell<JSString>(vm)) JSString(vm, *StringImpl::empty());
    newString->finishCreation(vm);
    return newString;
}

template<>
void JSRopeString::RopeBuilder<RecordOverflow>::expand()
{
    RELEASE_ASSERT(!this->hasOverflowed());
    ASSERT(m_strings.size() == JSRopeString::s_maxInternalRopeLength);
    static_assert(3 == JSRopeString::s_maxInternalRopeLength);
    ASSERT(m_length);
    ASSERT(asString(m_strings.at(0))->length());
    ASSERT(asString(m_strings.at(1))->length());
    ASSERT(asString(m_strings.at(2))->length());

    JSString* string = JSRopeString::create(m_vm, asString(m_strings.at(0)), asString(m_strings.at(1)), asString(m_strings.at(2)));
    ASSERT(string->length() == m_length);
    m_strings.clear();
    m_strings.append(string);
}

void JSString::dumpToStream(const JSCell* cell, PrintStream& out)
{
    const JSString* thisObject = uncheckedDowncast<JSString>(cell);
    out.printf("<%p, %s, [%u], ", thisObject, thisObject->className().characters(), thisObject->length());
    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer) {
        if (pointer & JSRopeString::isSubstringInPointer)
            out.printf("[substring]");
        else
            out.printf("[rope]");
    } else {
        if (WTF::StringImpl* ourImpl = std::bit_cast<StringImpl*>(pointer)) {
            if (ourImpl->is8Bit())
                out.printf("[8 %p]", ourImpl->span8().data());
            else
                out.printf("[16 %p]", ourImpl->span16().data());
        }
    }
    out.printf(">");
}

bool JSString::equalSlowCase(JSGlobalObject* globalObject, JSString* other) const
{
    return equalInline(globalObject, other);
}

size_t JSString::estimatedSize(JSCell* cell, VM& vm)
{
    JSString* thisObject = asString(cell);
    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer)
        return Base::estimatedSize(cell, vm);
    return Base::estimatedSize(cell, vm) + std::bit_cast<StringImpl*>(pointer)->costDuringGC();
}

template<typename Visitor>
void JSString::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSString* thisObject = asString(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    uintptr_t pointer = thisObject->fiberConcurrently();
    if (pointer & isRopeInPointer) {
        if (pointer & JSRopeString::isSubstringInPointer) {
            visitor.appendUnbarriered(static_cast<JSRopeString*>(thisObject)->fiber1());
            return;
        }
        for (unsigned index = 0; index < JSRopeString::s_maxInternalRopeLength; ++index) {
            JSString* fiber = nullptr;
            switch (index) {
            case 0:
                fiber = std::bit_cast<JSString*>(pointer & JSRopeString::stringMask);
                break;
            case 1:
                fiber = static_cast<JSRopeString*>(thisObject)->fiber1();
                break;
            case 2:
                fiber = static_cast<JSRopeString*>(thisObject)->fiber2();
                break;
            default:
                ASSERT_NOT_REACHED();
                return;
            }
            if (!fiber)
                break;
            visitor.appendUnbarriered(fiber);
        }
        return;
    }
    if (StringImpl* impl = std::bit_cast<StringImpl*>(pointer))
        visitor.reportExtraMemoryVisited(impl->costDuringGC());
}

DEFINE_VISIT_CHILDREN(JSString);

template<typename CharacterType>
void JSRopeString::resolveRopeInternalNoSubstring(std::span<CharacterType> buffer, uint8_t* stackLimit) const
{
    resolveToBuffer(fiber0(), fiber1(), fiber2(), buffer, stackLimit);
}

GCOwnedDataScope<AtomStringImpl*> JSRopeString::resolveRopeToAtomString(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto convertToAtomString = [this](const String& string) -> GCOwnedDataScope<AtomStringImpl*> {
        ASSERT(!string.impl() || string.impl()->isAtom());
        return { this, static_cast<AtomStringImpl*>(string.impl()) };
    };

    // GIL-off: snapshot the fiber word once (see resolveRopeWithFunction).
    uintptr_t fiberBits = fiberConcurrently();
    if (vm.gilOff() && !(fiberBits & isRopeInPointer)) [[unlikely]] {
        // Another mutator already resolved this rope between our caller's
        // isRope() check and here. Recover through the non-rope toAtomString
        // path: it atomizes the published impl through the shared atom table
        // and publishes the atom into the cell (swapToAtomString), so the
        // returned pointer is owned by the returned scope's owner cell.
        scope.release();
        return toAtomString(globalObject);
    }

    if (length() > maxLengthForOnStackResolve) {
        scope.release();
        constexpr bool reportAllocation = true;
        const String& result = resolveRopeWithFunction<reportAllocation>(globalObject, [&](Ref<StringImpl>&& newImpl) {
            return AtomStringImpl::add(newImpl.ptr());
        });
        if (result.impl() && !result.impl()->isAtom()) [[unlikely]] {
            // GIL-off: lost the publish race to a non-atom resolver (or
            // early-outed on an already-published non-atom impl). Never cast
            // the published impl blindly; recover through the non-rope
            // toAtomString path (atomize + swapToAtomString).
            ASSERT(vm.gilOff());
            return toAtomString(globalObject);
        }
        return convertToAtomString(result);
    }

    AtomString atomString;
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimitForCurrentThreadSlow());
    if (!(fiberBits & isSubstringInPointer)) {
        if (fiberBits & is8BitInPointer) {
            std::array<Latin1Character, maxLengthForOnStackResolve> buffer;
            resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), std::span { buffer }.first(length()), stackLimit);
            atomString = std::span<const Latin1Character> { buffer }.first(length());
        } else {
            std::array<char16_t, maxLengthForOnStackResolve> buffer;
            resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), std::span { buffer }.first(length()), stackLimit);
            atomString = std::span<const char16_t> { buffer }.first(length());
        }
    } else {
        // TSAN family rope-stringimpl: read the base's published impl through
        // the annotated relaxed load (getValueImpl); a plain String read of
        // the base's m_fiber races a concurrent swapToAtomString republish.
        atomString = StringView { *substringBase()->getValueImpl() }.substring(substringOffset(), length()).toAtomString();
    }

    size_t sizeToReport = atomString.impl()->hasOneRef() ? atomString.impl()->cost() : 0;
    StringImpl* expectedImpl = atomString.impl();
    convertToNonRope(String { atomString.releaseImpl() });
    // TSAN family rope-stringimpl: post-publish read of our own cell goes
    // through the annotated relaxed load too (the cell can be republished by
    // a racing resolver/atomizer), snapshotted once so the check and the
    // return see the same impl. Same single load flag-off.
    StringImpl* publishedImpl = getValueImpl();
    if (publishedImpl != expectedImpl) [[unlikely]] {
        // GIL-off: lost the publish race; the winner may have published a
        // non-atom impl with identical contents, and the cell does not own
        // our atom. Recover through the non-rope toAtomString path so the
        // returned pointer is cell-owned. GIL-on: never taken.
        ASSERT(vm.gilOff());
        return toAtomString(globalObject);
    }
    // If we resolved a string that didn't previously exist, notify the heap that we've grown.
    vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
    return { this, static_cast<AtomStringImpl*>(publishedImpl) };
}

GCOwnedDataScope<AtomStringImpl*> JSRopeString::resolveRopeToExistingAtomString(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // GIL-off: snapshot the fiber word once (see resolveRopeWithFunction).
    uintptr_t fiberBits = fiberConcurrently();
    if (vm.gilOff() && !(fiberBits & isRopeInPointer)) [[unlikely]] {
        // Another mutator already resolved this rope; route through the
        // non-rope path so a returned atom is owned by the cell.
        scope.release();
        return toExistingAtomString(globalObject);
    }

    if (length() > maxLengthForOnStackResolve) {
        RefPtr<AtomStringImpl> existingAtomString;
        constexpr bool reportAllocation = true;
        resolveRopeWithFunction<reportAllocation>(globalObject, [&](Ref<StringImpl>&& newImpl) -> Ref<StringImpl> {
            existingAtomString = AtomStringImpl::lookUp(newImpl.ptr());
            if (existingAtomString)
                return Ref { *existingAtomString };
            return WTF::move(newImpl);
        });
        RETURN_IF_EXCEPTION(scope, { });
        if (vm.gilOff()) [[unlikely]] {
            // The rope is resolved now whether or not we won the publish race
            // (and our lookup lambda may not have run at all on a lost race).
            // Route through the non-rope path: it returns the cell's impl when
            // that impl is the atom, or looks up and publishes the existing
            // atom into the cell (swapToAtomString), so the returned pointer
            // is owned by the returned scope's owner cell -- never a
            // locally-ref'd atom the cell does not own.
            return toExistingAtomString(globalObject);
        }
        return { this, existingAtomString.get() };
    }

    RefPtr<AtomStringImpl> existingAtomString;
    if (!(fiberBits & isSubstringInPointer)) {
        uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimitForCurrentThreadSlow());
        if (fiberBits & is8BitInPointer) {
            std::array<Latin1Character, maxLengthForOnStackResolve> buffer;
            resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), std::span { buffer }.first(length()), stackLimit);
            existingAtomString = AtomStringImpl::lookUp(std::span { buffer }.first(length()));
        } else {
            std::array<char16_t, maxLengthForOnStackResolve> buffer;
            resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), std::span { buffer }.first(length()), stackLimit);
            existingAtomString = AtomStringImpl::lookUp(std::span { buffer }.first(length()));
        }
    } else {
        // TSAN family rope-stringimpl: annotated relaxed read of the base's
        // published impl (see resolveRopeToAtomString above).
        existingAtomString = StringView { *substringBase()->getValueImpl() }.substring(substringOffset(), length()).toExistingAtomString().releaseImpl();
    }

    if (existingAtomString)
        convertToNonRope(*existingAtomString);
    if (vm.gilOff() && !(fiberConcurrently() & isRopeInPointer)) [[unlikely]] {
        // Resolved -- by us, or by a winner we lost the publish race to (in
        // which case the cell may hold a non-atom impl that does not own
        // existingAtomString). Route through the non-rope path (see above).
        return toExistingAtomString(globalObject);
    }
    return { this, existingAtomString.get() };
}

template<bool reportAllocation, typename Function>
const String& JSRopeString::resolveRopeWithFunction(JSGlobalObject* nullOrGlobalObjectForOOM, Function&& function) const
{
    VM& vm = this->vm();

    // GIL-off: snapshot the fiber word once. Multiple mutators can race to
    // resolve the same rope: after another mutator publishes a resolved impl
    // into m_fiber, re-reading fiber0()/isSubstring()/is8Bit() would misread
    // the published StringImpl* as a JSString* and flag bits. All rope-shape
    // decisions below use this snapshot; the publish itself is serialized and
    // made idempotent in convertToNonRope. GIL-on / flag-off: the snapshot is
    // byte-identical to the direct reads it replaces.
    uintptr_t fiberBits = fiberConcurrently();
    if (vm.gilOff() && !(fiberBits & isRopeInPointer)) [[unlikely]] {
        // Another mutator resolved this rope between our caller's isRope()
        // check and here. Already-resolved is success.
        return valueInternal();
    }
    ASSERT(fiberBits & isRopeInPointer);

    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();

    if (fiberBits & isSubstringInPointer) {
        ASSERT(!substringBase()->isRope());
        // TSAN family rope-stringimpl: snapshot the base's published impl via
        // the annotated relaxed load; a plain String read of the base's
        // m_fiber races a concurrent swapToAtomString republish. The local
        // String costs one extra ref/deref pair on this (cold) resolution
        // path only; semantics are identical in both flag states.
        String base { substringBase()->getValueImpl() };
        auto newImpl = base.substringSharingImpl(substringOffset(), length());
        convertToNonRope(function(newImpl.releaseImpl().releaseNonNull()));
        return valueInternal();
    }

    if (fiberBits & is8BitInPointer) {
        std::span<Latin1Character> buffer;
        auto newImpl = StringImpl::tryCreateUninitialized(length(), buffer);
        if (!newImpl) {
            outOfMemory(nullOrGlobalObjectForOOM);
            return nullString();
        }

        size_t sizeToReport = newImpl->cost();
        uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimitForCurrentThreadSlow());
        resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), buffer, stackLimit);
        String resolvedString = function(newImpl.releaseNonNull());
        StringImpl* resolvedImpl = resolvedString.impl();
        convertToNonRope(WTF::move(resolvedString));
        if constexpr (reportAllocation) {
            // GIL-off: a lost publish race drops our impl; do not report
            // memory the cell does not hold. GIL-on: always true. The check
            // reads the cell through the annotated relaxed load (TSAN family
            // rope-stringimpl): a racing resolver can republish concurrently.
            if (getValueImpl() == resolvedImpl)
                vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
        }
        return valueInternal();
    }

    std::span<char16_t> buffer;
    auto newImpl = StringImpl::tryCreateUninitialized(length(), buffer);
    if (!newImpl) {
        outOfMemory(nullOrGlobalObjectForOOM);
        return nullString();
    }

    size_t sizeToReport = newImpl->cost();
    uint8_t* stackLimit = std::bit_cast<uint8_t*>(vm.softStackLimitForCurrentThreadSlow());
    resolveToBuffer(std::bit_cast<JSString*>(fiberBits & stringMask), fiber1(), fiber2(), buffer, stackLimit);
    String resolvedString = function(newImpl.releaseNonNull());
    StringImpl* resolvedImpl = resolvedString.impl();
    convertToNonRope(WTF::move(resolvedString));
    if constexpr (reportAllocation) {
        // GIL-off: a lost publish race drops our impl; do not report memory
        // the cell does not hold. GIL-on: always true. Annotated relaxed read
        // of the cell (TSAN family rope-stringimpl); see above.
        if (getValueImpl() == resolvedImpl)
            vm.heap.reportExtraMemoryAllocated(this, sizeToReport);
    }
    return valueInternal();
}

const String& JSRopeString::resolveRope(JSGlobalObject* nullOrGlobalObjectForOOM) const
{
    constexpr bool reportAllocation = true;
    return resolveRopeWithFunction<reportAllocation>(nullOrGlobalObjectForOOM, [](Ref<StringImpl>&& newImpl) {
        return WTF::move(newImpl);
    });
}

const String& JSRopeString::resolveRopeWithoutGC() const
{
    constexpr bool reportAllocation = false;
    return resolveRopeWithFunction<reportAllocation>(nullptr, [](Ref<StringImpl>&& newImpl) {
        return WTF::move(newImpl);
    });
}

void JSRopeString::outOfMemory(JSGlobalObject* nullOrGlobalObjectForOOM) const
{
    ASSERT(isRope());
    if (nullOrGlobalObjectForOOM) {
        VM& vm = nullOrGlobalObjectForOOM->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope);
    }
}

JSValue JSString::toPrimitive(JSGlobalObject*, PreferredPrimitiveType) const
{
    return const_cast<JSString*>(this);
}

double JSString::toNumber(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = this->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    return jsToNumber(view);
}

inline StringObject* StringObject::create(VM& vm, JSGlobalObject* globalObject, JSString* string)
{
    StringObject* object = new (NotNull, allocateCell<StringObject>(vm)) StringObject(vm, globalObject->stringObjectStructure());
    object->finishCreation(vm, string);
    return object;
}

JSObject* JSString::toObject(JSGlobalObject* globalObject) const
{
    return StringObject::create(globalObject->vm(), globalObject, const_cast<JSString*>(this));
}

bool JSString::getStringPropertyDescriptor(JSGlobalObject* globalObject, PropertyName propertyName, PropertyDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    if (propertyName == vm.propertyNames->length) {
        descriptor.setDescriptor(jsNumber(length()), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return true;
    }

    std::optional<uint32_t> index = parseIndex(propertyName);
    if (index && index.value() < length()) {
        descriptor.setDescriptor(getIndex(globalObject, index.value()), PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return true;
    }

    return false;
}

GCOwnedDataScope<const String&> JSString::tryGetValueWithoutGC() const
{
    if (isRope()) {
        // Pass nullptr for the JSGlobalObject so that resolveRope does not throw in the event of an OOM error.
        return { this, static_cast<const JSRopeString*>(this)->resolveRopeWithoutGC() };
    }
    return { this, valueInternal() };
}

JSString* jsStringWithCacheSlowCase(VM& vm, StringImpl& stringImpl)
{
    ASSERT(stringImpl.length() > 1 || (stringImpl.length() == 1 && stringImpl[0] > maxSingleCharacterString));
    JSString* string = JSString::create(vm, stringImpl);
    vm.lastCachedString.setWithoutWriteBarrier(string);
    return string;
}

#if USE(BUN_JSC_ADDITIONS)

void JSRopeString::iterRopeInternalNoSubstring(jsstring_iterator* iter) const
{
    for (size_t i = 0; i < s_maxInternalRopeLength && fiber(i) && !iter->stop; ++i) {
        if (fiber(i)->isRope()) {
            iterRopeSlowCase(iter);
            return;
        }
    }

    size_t position = 0;

    for (size_t i = 0; i < s_maxInternalRopeLength && fiber(i) && !iter->stop; ++i) {
        // TSAN family rope-stringimpl: annotated relaxed read of the fiber's
        // published impl (plain String reads race swapToAtomString).
        const StringImpl& fiberString = *fiber(i)->getValueImpl();
        unsigned length = fiberString.length();
        if (fiberString.is8Bit())
            StringImpl::iterCharacters(iter, position, fiberString.span8().data(), length);
        else
            StringImpl::iterCharacters(iter, position, fiberString.span16().data(), length);
        position += length;
    }

    ASSERT(iter->stop || length() == position);
}

void JSRopeString::iterRope(jsstring_iterator *iter) const
{
     ASSERT(isRope());

    if (isSubstring()) {
        ASSERT(!substringBase()->isRope());
        // TSAN family rope-stringimpl: annotated relaxed read of the base's
        // published impl.
        StringImpl* impl = substringBase()->getValueImpl();

        if (impl->is8Bit()) {
            auto ptr = impl->span8().data() + substringOffset();
            size_t end = length();
            iter->append8(iter, (void*)ptr, end);
        } else {
            auto ptr = impl->span16().data() + substringOffset();
            size_t end = length();
            iter->append16(iter, (void*)ptr, end);
        }

        return;
    }


    iterRopeInternalNoSubstring(iter);
}

void JSRopeString::iterRopeSlowCase(jsstring_iterator* iter) const
{
    size_t position = length(); // We will be working backwards over the rope.
    Vector<JSString*, 32, UnsafeVectorOverflow> workQueue; // These strings are kept alive by the parent rope, so using a Vector is OK.

    for (size_t i = 0; i < s_maxInternalRopeLength && fiber(i); ++i)
        workQueue.append(fiber(i));

    while (!workQueue.isEmpty() && !iter->stop) {
        JSString* currentFiber = workQueue.last();
        workQueue.removeLast();

        if (currentFiber->isRope()) {
            JSRopeString* currentFiberAsRope = static_cast<JSRopeString*>(currentFiber);
            if (currentFiberAsRope->isSubstring()) {
                ASSERT(!currentFiberAsRope->substringBase()->isRope());
                StringImpl* string = currentFiberAsRope->substringBase()->getValueImpl();
                unsigned offset = currentFiberAsRope->substringOffset();
                unsigned length = currentFiberAsRope->length();
                position -= length;
                if (string->is8Bit())
                    StringImpl::iterCharacters(iter, position, string->span8().data() + offset, length);
                else
                    StringImpl::iterCharacters(iter, position, string->span16().data() + offset, length);
                continue;
            }
            for (size_t i = 0; i < s_maxInternalRopeLength && currentFiberAsRope->fiber(i) && !iter->stop; ++i)
                workQueue.append(currentFiberAsRope->fiber(i));
            continue;
        }

        StringImpl* string = currentFiber->getValueImpl();
        unsigned length = string->length();
        position -= length;
        if (string->is8Bit())
            StringImpl::iterCharacters(iter, position, string->span8().data(), length);
        else
            StringImpl::iterCharacters(iter, position, string->span16().data(), length);
    }

    ASSERT(position == 0 || iter->stop);
}

#endif


} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END