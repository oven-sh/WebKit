/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "PythonPickle.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"

// pickle.PickleBuffer: Objects/picklebufobject.c of CPython. It says of something that has bytes to show that they need not be copied into the pickle, and has none of its own.

namespace JSC { namespace Python {

namespace {

// PyPickleBufferObject
struct PickleBuffer final : NativeState {
    PYTHON_NATIVE_STATE(PickleBuffer);
    // What it was given showed, when it was made. There is none when it has been released.
    WriteBarrier<PyMemoryView> view;
    // What is asked from then on: the `obj` of the Py_buffer. It is what it was given, or for a class of a program's, which is asked only the once, the view.
    WriteBarrier<Unknown> exporter;

    std::optional<JSValue> showsBytesOf() const final { return view ? exporter.get() : JSValue(); }
    void willExportBytes(JSGlobalObject* globalObject) const final
    {
        if (view)
            return;
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseValueError(globalObject, scope, "operation forbidden on released PickleBuffer object"_s);
    }
};

template<typename Visitor>
void PickleBuffer::visit(Visitor& visitor)
{
    visitor.append(view);
    visitor.append(exporter);
}

// picklebuf_new()
PYTHON_NATIVE(pickleBufferNew)
{
    NATIVE_PROLOGUE();
    // PyArg_ParseTupleAndKeywords(), of one argument that has no name to be given by
    size_t given = args.size() - 1;
    if (given + args.keywordCount() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("PickleBuffer() takes at most 1 argument ("_s, given + args.keywordCount(), " given)"_s)));
    if (!given)
        return JSValue::encode(raiseTypeError(globalObject, scope, "PickleBuffer() takes exactly 1 positional argument (0 given)"_s));
    if (!hasBuffer(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("a bytes-like object is required, not '"_s, typeName(globalObject, args[1]), '\'')));
    PyMemoryView* view = memoryViewOf(globalObject, args[1], FullReadOnlyBuffer);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<PickleBuffer>());
    auto& self = object->state<PickleBuffer>();
    self.view.set(vm, object, view);
    self.exporter.set(vm, object, view->exporter() ? JSValue(view) : args[1]);
    return JSValue::encode(object);
}

// picklebuf_raw()
PYTHON_NATIVE(pickleBufferRaw)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<PickleBuffer>(args[0]);
    self.willExportBytes(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PyMemoryView* view = self.view.get();
    if (!view->isCContiguous() && !view->isFortranContiguous())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::BufferError, "cannot extract raw buffer from non-contiguous buffer"_s));
    // The same bytes as they lie, one row of them.
    PyMemoryView::Layout layout = view->layout();
    layout.format = 'B';
    layout.formatHasAtSign = false;
    layout.itemSize = 1;
    PyMemoryView::Dimension row { view->byteLength(), 1 };
    return JSValue::encode(view->derive(globalObject, layout, { &row, 1 }));
}

// picklebuf_release()
PYTHON_NATIVE(pickleBufferRelease)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<PickleBuffer>(args[0]);
    if (PyMemoryView* view = self.view.get()) {
        self.view.clear();
        self.exporter.clear();
        view->release(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

} // namespace

bool isPickleBuffer(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value) == pickleModuleState(globalObject).pickleBufferType.get();
}

void initializePickleBufferType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    PyType* type = createBuiltinType(globalObject, "pickle.PickleBuffer"_s, realm->typeObject(), PyType::Layout::Native, 0);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    pickleModuleState(globalObject).pickleBufferType.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__new__"_s, pickleBufferNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked },
        { "raw"_s, pickleBufferRaw },
        { "release"_s, pickleBufferRelease },
    });
    addBufferMethods(globalObject, type);
}

} } // namespace JSC::Python
