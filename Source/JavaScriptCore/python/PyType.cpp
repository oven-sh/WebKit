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
#include "PyType.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyObjects.h"
#include "PythonOperations.h"

namespace JSC {

const ClassInfo PyType::s_info = { "type"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyType) };

template<typename Visitor>
void PyType::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyType>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_metatype);
    visitor.append(thisObject->m_base);
    visitor.append(thisObject->m_bases);
    visitor.append(thisObject->m_mro);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_instanceStructure);
}

DEFINE_VISIT_CHILDREN(PyType);

Structure* PyType::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(PyTypeType, StructureFlags), info());
}

Structure* PyType::createInstanceStructure(VM& vm, JSGlobalObject* globalObject, Layout layout, PyType* prototype)
{
    switch (layout) {
    case Layout::Object:
        return PyInstance::createStructure(vm, globalObject, prototype);
    case Layout::Tuple:
        return PyTuple::createStructure(vm, globalObject, prototype);
    case Layout::Dict:
        return PyDict::createStructure(vm, globalObject, prototype);
    case Layout::Set:
        return PySet::createStructure(vm, globalObject, prototype);
    case Layout::List:
        return JSArray::createStructure(vm, globalObject, prototype, ArrayWithUndecided);
    case Layout::Boxed:
        return PyBoxedValue::createStructure(vm, globalObject, prototype);
    case Layout::Type:
    case Layout::Native:
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PyType* PyType::createBuiltin(VM& vm, JSGlobalObject* globalObject, ASCIILiteral name, PyType* base, Layout layout, unsigned flags)
{
    JSValue prototype = base ? JSValue(base) : JSValue(globalObject->objectPrototype());
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, createStructure(vm, globalObject, prototype));
    type->finishCreation(vm);
    type->m_name.set(vm, type, jsString(vm, String(name)));
    type->m_base.setMayBeNull(vm, type, base);
    type->m_layout = layout;
    type->m_flags = flags;
    return type;
}

void PyType::finishBuiltin(VM& vm, JSGlobalObject* globalObject, PyType* metatype)
{
    m_metatype.set(vm, this, metatype);
    PyType* base = m_base.get();
    unsigned depth = 1;
    for (PyType* type = base; type; type = type->base())
        ++depth;
    PyTuple* mro = PyTuple::create(globalObject, depth);
    unsigned i = 0;
    for (PyType* type = this; type; type = type->base())
        mro->initializeAt(vm, i++, type);
    m_mro.set(vm, this, mro);
    m_bases.set(vm, this, base ? PyTuple::create(globalObject, { base }) : PyTuple::create(globalObject, 0));
    // int's, float's, str's and list's own instances are JavaScript's values.
    if (m_layout != Layout::Boxed && m_layout != Layout::List)
        m_instanceStructure.setMayBeNull(vm, this, createInstanceStructure(vm, globalObject, m_layout, this));
    // So that `x instanceof C` is true in JavaScript.
    putDirect(vm, vm.propertyNames->prototype, this, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
}

PyType* PyType::create(VM& vm, JSGlobalObject* globalObject, PyType* metatype, JSString* name, PyTuple* bases, PyType* base, PyTuple* mro)
{
    // FIXME: With more than one base, JavaScript sees only what comes by the first.
    auto* type = new (NotNull, allocateCell<PyType>(vm)) PyType(vm, createStructure(vm, globalObject, bases->length() ? bases->at(0) : JSValue(base)));
    type->finishCreation(vm);
    type->m_metatype.set(vm, type, metatype);
    type->m_name.set(vm, type, name);
    type->m_base.set(vm, type, base);
    type->m_bases.set(vm, type, bases);
    type->m_layout = base->layout();
    type->m_flags = IsHeapType | IsBaseType | (base->m_flags & (IsExceptionType | IsTypeSubclass | MatchesSelf | IsBytes));
    // Whether it is a sequence or a mapping is for the first of its ancestors that is one or the other to say.
    for (auto& ancestor : mro->span()) {
        if (unsigned collectionFlags = asType(ancestor.get())->m_flags & (IsSequence | IsMapping)) {
            type->m_flags |= collectionFlags;
            break;
        }
    }
    // The class comes first in its own order, and could not be put there before there was one.
    PyTuple* fullOrder = PyTuple::create(globalObject, mro->length() + 1);
    fullOrder->initializeAt(vm, 0, type);
    for (unsigned i = 0; i < mro->length(); ++i)
        fullOrder->initializeAt(vm, i + 1, mro->at(i));
    type->m_mro.set(vm, type, fullOrder);
    Structure* instanceStructure = createInstanceStructure(vm, globalObject, type->m_layout, type);
    // The same kind of cell as the base's instances, whatever that is.
    if (!instanceStructure && type->m_layout == Layout::Native && base->instanceStructure())
        instanceStructure = Structure::create(vm, globalObject, type, base->instanceStructure()->typeInfo(), base->instanceStructure()->classInfoForCells());
    type->m_instanceStructure.setMayBeNull(vm, type, instanceStructure);
    type->putDirect(vm, vm.propertyNames->prototype, type, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    return type;
}

String PyType::nameString(JSGlobalObject* globalObject) const
{
    return m_name->value(globalObject);
}

JSValue PyType::lookup(VM& vm, PropertyName name) const
{
    for (auto& entry : m_mro->span()) {
        if (JSValue value = asObject(entry.get())->getDirect(vm, name))
            return value;
    }
    return { };
}

JSValue PyType::lookupAfter(VM& vm, PyType* after, PropertyName name) const
{
    auto order = m_mro->span();
    size_t i = 0;
    while (i < order.size() && order[i].get().asCell() != after)
        ++i;
    for (++i; i < order.size(); ++i) {
        if (JSValue value = asObject(order[i].get())->getDirect(vm, name))
            return value;
    }
    return { };
}

bool PyType::isSubtypeOf(const PyType* other) const
{
    if (this == other)
        return true;
    for (auto& entry : m_mro->span()) {
        if (entry.get().asCell() == other)
            return true;
    }
    return false;
}

void PyType::setAttribute(VM& vm, PropertyName name, JSValue value)
{
    putDirect(vm, name, value);
    ++vm.pythonNames().typeEpoch;
}

bool PyType::deleteAttribute(VM& vm, JSGlobalObject* globalObject, PropertyName name)
{
    if (!getDirect(vm, name))
        return false;
    ++vm.pythonNames().typeEpoch;
    return JSObject::deleteProperty(globalObject, name);
}

unsigned PyType::hooks(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    if (m_hooksEpoch == names.typeEpoch) [[likely]]
        return m_flags & hookFlags;

    using Function = PyRealm::WellKnownFunction;
    PyRealm* realm = globalObject->pyRealm();
    auto isOneOf = [&] (JSValue value, Function a, Function b) {
        return value && value.isCell() && (value.asCell() == realm->function(a) || value.asCell() == realm->function(b));
    };
    unsigned flags = 0;
    if (!isOneOf(lookup(vm, names.dunder_getattribute), Function::ObjectGetAttribute, Function::TypeGetAttribute))
        flags |= HasCustomGetAttribute;
    if (lookup(vm, names.dunder_getattr))
        flags |= HasGetAttr;
    if (!isOneOf(lookup(vm, names.dunder_setattr), Function::ObjectSetAttr, Function::TypeSetAttr) || !isOneOf(lookup(vm, names.dunder_delattr), Function::ObjectDelAttr, Function::TypeDelAttr))
        flags |= HasCustomSetAttr;
    m_flags = (m_flags & ~hookFlags) | flags;
    m_hooksEpoch = names.typeEpoch;
    return flags;
}

static JSC_DECLARE_HOST_FUNCTION(callType);

// C(...) is type(C).__call__(C, ...), which for nearly every class is type.__call__.
JSC_DEFINE_HOST_FUNCTION(callType, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto* type = uncheckedDowncast<PyType>(callFrame->jsCallee());
    Python::NativeArguments given(callFrame);

    MarkedArgumentBuffer arguments;
    PyType* metatype = type->metatype();
    if (metatype != realm->typeType()) {
        JSValue function = metatype->lookup(vm, vm.pythonNames().dunder_call);
        if (function && function.asCell() != realm->function(PyRealm::WellKnownFunction::TypeCall)) {
            JSValue bound = Python::bindDescriptor(globalObject, function, type, metatype);
            RETURN_IF_EXCEPTION(scope, { });
            for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
                arguments.append(callFrame->uncheckedArgument(i));
            RELEASE_AND_RETURN(scope, JSValue::encode(Python::callWithKeywords(globalObject, bound, arguments, given.keywordNames())));
        }
    }

    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    RELEASE_AND_RETURN(scope, JSValue::encode(Python::instantiate(globalObject, type, arguments, given.keywordNames())));
}

bool PyType::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* type = uncheckedDowncast<PyType>(object);
    JSValue receiver = slot.thisValue();
    // The engine asking for its own purposes is not to run anything, and C.prototype is JavaScript's business.
    if (slot.isVMInquiry() || name.isPrivateName() || name == vm.propertyNames->prototype || !receiver.isObject())
        RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(object, globalObject, name, slot));

    // In Python, what an attribute is is settled when it is got: a function of the class becomes a method bound to the instance, a property is
    // computed, __getattr__ is asked. So the class of the receiver answers with what getattr() would give. The classes beyond it have
    // nothing to add, since that has been through all of them, in Python's order and not in that of the prototypes.
    bool isForClass = receiver == JSValue(type);
    if (!isForClass && asObject(receiver)->getPrototypeDirect() != JSValue(type))
        return false;
    JSValue value = Python::getPropertyForJavaScript(globalObject, receiver, name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    slot.setValue(object, static_cast<unsigned>(PropertyAttribute::DontEnum), value);
    return true;
}

CallData PyType::getCallData(JSCell*)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callType;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

// new C(...) in JavaScript is C(...).
CallData PyType::getConstructData(JSCell* cell)
{
    return getCallData(cell);
}

} // namespace JSC
