/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Exception.h"

#include "AOTFunction.h"
#include "Interpreter.h"
#include "JSCJSValueInlines.h"
#include "JSObjectInlines.h"
#include "StructureCreateInlines.h"
#include "JSWebAssemblyException.h"
#include "StructureInlines.h"
#include "WasmTag.h"

namespace JSC {

const ClassInfo Exception::s_info = { "Exception"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Exception) };

static bool keepsReturnAddresses()
{
#if ENABLE(AOT)
    if (!Options::useImmutableIntrinsics()) [[likely]]
        return false;
    static const bool keeps = getenv("BUN_AOT_SWITCH_LAZY_TRACES");
    return keeps;
#else
    return false;
#endif
}

Exception* Exception::create(VM& vm, JSValue thrownValue, StackCaptureAction action)
{
    Exception* result = new (NotNull, allocateCell<Exception>(vm)) Exception(vm, thrownValue);
    result->finishCreation(vm, action);
    return result;
}

void Exception::destroy(JSCell* cell)
{
    Exception* exception = static_cast<Exception*>(cell);
    exception->~Exception();
}

Structure* Exception::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

template<typename Visitor>
void Exception::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Exception* thisObject = uncheckedDowncast<Exception>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    visitor.append(thisObject->m_value);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_asyncContext);
#endif
    std::optional<Locker<JSCellLock>> locker;
    if (keepsReturnAddresses())
        locker.emplace(thisObject->cellLock());
    for (StackFrame& frame : thisObject->m_stack)
        frame.visitAggregate(visitor);
    size_t size = thisObject->m_stack.sizeInBytes();
#if ENABLE(AOT)
    if (auto* returnAddresses = thisObject->m_returnAddressesAndDidNotifyInspectorOfThrow.pointer()) {
        visitor.appendUnbarriered(AOT::tokenOf(returnAddresses->instance));
        size += returnAddresses->returnAddresses.sizeInBytes();
    }
#endif
    visitor.reportExtraMemoryVisited(size);
}

size_t Exception::estimatedSize(JSCell* cell, VM& vm)
{
    Exception* thisObject = uncheckedDowncast<Exception>(cell);
    std::optional<Locker<JSCellLock>> locker;
    if (keepsReturnAddresses())
        locker.emplace(thisObject->cellLock());
    size_t size = thisObject->m_stack.sizeInBytes();
#if ENABLE(AOT)
    if (auto* returnAddresses = thisObject->m_returnAddressesAndDidNotifyInspectorOfThrow.pointer())
        size += returnAddresses->returnAddresses.sizeInBytes();
#endif
    return Base::estimatedSize(cell, vm) + size;
}

DEFINE_VISIT_CHILDREN(Exception);

Exception::Exception(VM& vm, JSValue thrownValue)
    : Base(vm, vm.exceptionStructure.get())
    , m_value(thrownValue, WriteBarrierEarlyInit)
{
}

Exception::~Exception()
{
    delete m_returnAddressesAndDidNotifyInspectorOfThrow.pointer();
}

#if ENABLE(AOT)
NEVER_INLINE bool Exception::tryKeepReturnAddresses(VM& vm)
{
    std::unique_ptr<Vector<StackFrame>> otherFrames;
    auto returnAddresses = vm.interpreter.tryGetStackTraceAsReturnAddresses(this, Options::exceptionStackTraceLimit(), otherFrames);
    if (!returnAddresses)
        return false;
    if (otherFrames)
        m_stack = WTF::move(*otherFrames);
    size_t size = m_stack.sizeInBytes() + returnAddresses->returnAddresses.sizeInBytes();
    m_returnAddressesAndDidNotifyInspectorOfThrow.setPointer(returnAddresses.release());
    vm.heap.reportExtraMemoryAllocated(this, size);
    return true;
}
#endif

void Exception::finishCreation(VM& vm, StackCaptureAction action)
{
    Base::finishCreation(vm);

#if ENABLE(AOT)
    if (keepsReturnAddresses() && action == StackCaptureAction::CaptureStack && tryKeepReturnAddresses(vm))
        return;
#endif
    Vector<StackFrame> stackTrace;
    if (action == StackCaptureAction::CaptureStack)
        vm.interpreter.getStackTrace(this, stackTrace, 0, Options::exceptionStackTraceLimit());

    m_stack = WTF::move(stackTrace);
    vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
}

void Exception::materializeStack()
{
    VM& vm = this->vm();
    std::unique_ptr<AOT::ReturnAddressTrace> returnAddresses { m_returnAddressesAndDidNotifyInspectorOfThrow.pointer() };
    Vector<StackFrame> stack;
    StackFrame::appendTypedFrames(*returnAddresses, stack);
    stack.appendVector(m_stack);
    {
        Locker locker { cellLock() };
        m_stack = WTF::move(stack);
        m_returnAddressesAndDidNotifyInspectorOfThrow.setPointer(nullptr);
    }
    vm.writeBarrier(this);
    vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
}

#if ENABLE(WEBASSEMBLY)

void Exception::tryUnwrapValueForJSTag(VM& vm)
{
    if (!m_value)
        return;

    if (auto* exception = dynamicDowncast<JSWebAssemblyException>(m_value.get())) {
        if (&exception->tag() == &Wasm::Tag::jsExceptionTag()) {
            m_value.set(vm, this, JSValue::decode(exception->payload().at(0)));
            return;
        }
    }
}

void Exception::wrapValueForJSTag(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    FixedVector<uint64_t> payload { static_cast<uint64_t>(JSValue::encode(m_value.get())) };
    auto* wrapper = JSWebAssemblyException::create(vm, globalObject->webAssemblyExceptionStructure(), Wasm::Tag::jsExceptionTag(), WTF::move(payload));
    m_value.set(vm, this, wrapper);
}

#endif

} // namespace JSC
