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

#include "AOTStackTrace.h"
#include "AOTUnwinding.h"
#include "CallFrameInlines.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "Interpreter.h"
#include "JSCJSValueInlines.h"
#include "JSObjectInlines.h"
#include "StructureCreateInlines.h"
#include "JSWebAssemblyException.h"
#include "StructureInlines.h"
#include "VMInlines.h"
#include "WasmTag.h"

namespace JSC {

const ClassInfo Exception::s_info = { "Exception"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Exception) };

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
    Locker locker { thisObject->cellLock() };
    for (StackFrame& frame : thisObject->m_stack)
        frame.visitAggregate(visitor);
#if ENABLE(AOT)
    if (auto* returnAddresses = thisObject->returnAddresses()) {
        visitor.markAuxiliary(returnAddresses);
        visitor.appendUnbarriered(AOT::livenessToken(returnAddresses->instance));
    }
#endif
    visitor.reportExtraMemoryVisited(thisObject->m_stack.sizeInBytes());
}

size_t Exception::estimatedSize(JSCell* cell, VM& vm)
{
    Exception* thisObject = uncheckedDowncast<Exception>(cell);
    Locker locker { thisObject->cellLock() };
    size_t size = thisObject->m_stack.sizeInBytes();
#if ENABLE(AOT)
    if (auto* returnAddresses = thisObject->returnAddresses())
        size += returnAddresses->sizeInBytes();
#endif
    return Base::estimatedSize(cell, vm) + size;
}

DEFINE_VISIT_CHILDREN(Exception);

Exception::Exception(VM& vm, JSValue thrownValue)
    : Base(vm, vm.exceptionStructure.get())
    , m_value(thrownValue, WriteBarrierEarlyInit)
{
}

Exception::~Exception() = default;

#if ENABLE(AOT)
void Exception::retainReturnAddresses(VM& vm, AOT::ReturnAddressTrace* returnAddresses, Vector<StackFrame>&& otherFrames)
{
    {
        Locker locker { cellLock() };
        m_stack = WTF::move(otherFrames);
        setReturnAddresses(returnAddresses);
    }
    vm.writeBarrier(this);
    vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
}

// Two readers tell the stack of an Error that was made on the spot from the stack of the throw: an inspector lists the frames, and node:vm marks the throw in the source text of the bytecode on top.
ErrorInstance* Exception::errorToTakeStackFrom(VM& vm, JSValue thrownValue)
{
    auto* error = dynamicDowncast<ErrorInstance>(thrownValue);
    if (!error || !error->returnAddresses() || error->realm()->debugger())
        return nullptr;
    CallFrame* topFrame = vm.topJSCallFrame();
    if (topFrame && topFrame->codeBlock())
        return nullptr;
    return error;
}

void Exception::takeStackFrom(VM& vm, ErrorInstance& error)
{
    const Vector<StackFrame>* otherFrames = error.framesBesideReturnAddresses();
    retainReturnAddresses(vm, error.returnAddresses(), otherFrames ? Vector<StackFrame>(*otherFrames) : Vector<StackFrame>());
}

Exception* Exception::tryCreateWithStackOfErrorMadeHere(VM& vm, JSValue thrownValue)
{
    ErrorInstance* error = errorToTakeStackFrom(vm, thrownValue);
    if (!error || !AOT::beginsInTopCompiledFrame(vm, *error->returnAddresses()))
        return nullptr;
    // The end of a collection may turn the stack of the Error into text.
    DeferGC deferGC(vm);
    Exception* result = new (NotNull, allocateCell<Exception>(vm)) Exception(vm, thrownValue);
    result->Base::finishCreation(vm);
    result->takeStackFrom(vm, *error);
    return result;
}

NEVER_INLINE bool Exception::tryRetainReturnAddresses(VM& vm, StackCaptureAction action)
{
    if (ErrorInstance* error = action == StackCaptureAction::CaptureStack ? errorToTakeStackFrom(vm, value()) : nullptr) {
        takeStackFrom(vm, *error);
        return true;
    }
    std::unique_ptr<Vector<StackFrame>> otherFrames;
    auto* returnAddresses = AOT::tryCaptureStackTrace(vm, this, Options::exceptionStackTraceLimit(), otherFrames);
    if (!returnAddresses)
        return false;
    retainReturnAddresses(vm, returnAddresses, otherFrames ? WTF::move(*otherFrames) : Vector<StackFrame>());
    return true;
}
#endif

void Exception::finishCreation(VM& vm, StackCaptureAction action)
{
    Base::finishCreation(vm);

#if ENABLE(AOT)
    if (action != StackCaptureAction::DoNotCaptureStack && vm.hasCompiledCode() && tryRetainReturnAddresses(vm, action))
        return;
#endif
    Vector<StackFrame> stackTrace;
    if (action != StackCaptureAction::DoNotCaptureStack)
        vm.interpreter.getStackTrace(this, stackTrace, 0, Options::exceptionStackTraceLimit());

    m_stack = WTF::move(stackTrace);
    vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
}

void Exception::materializeStack()
{
#if ENABLE(AOT)
    VM& vm = this->vm();
    auto* returnAddresses = this->returnAddresses();
    if (!returnAddresses)
        return;
    DeferGCForAWhile deferGC(vm);
    Vector<StackFrame> stack;
    unsigned onTop = returnAddresses->numberOfFramesOnTop;
    stack.append(m_stack.span().first(onTop));
    StackFrame::appendCompiledFrames(*returnAddresses, stack);
    stack.append(m_stack.span().subspan(onTop));
    {
        Locker locker { cellLock() };
        m_stack = WTF::move(stack);
        setReturnAddresses(nullptr);
    }
    vm.writeBarrier(this);
    vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
#endif
}

const Vector<StackFrame>& Exception::stackForReporting(VM& vm)
{
    if (stack().isEmpty()) {
        if (auto* error = dynamicDowncast<ErrorInstance>(value())) {
            if (auto* stackTrace = error->stackTrace(); stackTrace && !stackTrace->isEmpty()) {
                Vector<StackFrame> stack = *stackTrace;
                {
                    Locker locker { cellLock() };
                    m_stack = WTF::move(stack);
                }
                vm.writeBarrier(this);
                vm.heap.reportExtraMemoryAllocated(this, m_stack.sizeInBytes());
            }
        }
    }
    return m_stack;
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
