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
#include "PyRealm.h"

#include "FunctionPrototype.h"
#include "ObjectConstructor.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonRuntimeFunctions.h"

namespace JSC {

const ClassInfo PyRealm::s_info = { "PyRealm"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyRealm) };

template<typename Visitor>
void PyRealm::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyRealm>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    for (auto& type : thisObject->m_types)
        visitor.append(type);
    for (auto& function : thisObject->m_functions)
        visitor.append(function);
    visitor.append(thisObject->m_tupleStructure);
    visitor.append(thisObject->m_nativeFunctionStructure);
    visitor.append(thisObject->m_hashStorageStructure);
    visitor.append(thisObject->m_emptyTuple);
    visitor.append(thisObject->m_notImplemented);
    visitor.append(thisObject->m_ellipsis);
    visitor.append(thisObject->m_boundArgumentsMarker);
    visitor.append(thisObject->m_runtimeFunctions);
    visitor.append(thisObject->m_javaScriptFunctions);
    visitor.append(thisObject->m_frameLocalsProxyType);
    visitor.append(thisObject->m_builtinsModule);
    visitor.append(thisObject->m_modules);
    visitor.append(thisObject->m_handledException);
    visitor.append(thisObject->m_outerHandledException);
    visitor.append(thisObject->m_returnValue);
}

DEFINE_VISIT_CHILDREN(PyRealm);

Structure* PyRealm::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

PyRealm* PyRealm::create(VM& vm, JSGlobalObject* globalObject)
{
    auto* realm = new (NotNull, allocateCell<PyRealm>(vm)) PyRealm(vm, createStructure(vm, globalObject, jsNull()));
    realm->finishCreation(vm);
    return realm;
}

void PyRealm::initialize(VM& vm, JSGlobalObject* globalObject)
{
    DeferGC deferGC(vm);

    // The types first, bare. A type has a tuple of bases, and a tuple has a type.
#define CREATE(name, pythonName, base, layout, flags) \
    m_types[static_cast<unsigned>(BuiltinType::name)].set(vm, this, PyType::createBuiltin(vm, globalObject, pythonName ""_s, BuiltinType::base == BuiltinType::None ? nullptr : type(BuiltinType::base), PyType::Layout::layout, flags));
    FOR_EACH_PYTHON_BUILTIN_TYPE(CREATE)
#undef CREATE

    m_tupleStructure.set(vm, this, PyTuple::createStructure(vm, globalObject, typeTuple()));
    m_hashStorageStructure.set(vm, this, PyHashStorage::createStructure(vm, globalObject, jsNull()));
    m_nativeFunctionStructure.set(vm, this, PyNativeFunction::createStructure(vm, globalObject, globalObject->functionPrototype()));
    m_emptyTuple.set(vm, this, PyTuple::create(vm, m_tupleStructure.get(), 0));
    for (auto& type : m_types)
        type->finishBuiltin(vm, globalObject, typeType());
    // The one that has been made already is the one that tuples are made with.
    typeTuple()->setInstanceStructure(vm, m_tupleStructure.get());

    m_boundArgumentsMarker.set(vm, this, constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure()));

    Python::initializeFunctionTypes(globalObject);
    Python::initializeObjectAndType(globalObject);
    m_notImplemented.set(vm, this, PyNativeObject::create(globalObject, BuiltinType::NotImplementedType));
    m_ellipsis.set(vm, this, PyNativeObject::create(globalObject, BuiltinType::Ellipsis));
    Python::initializeNumberTypes(globalObject);
    Python::initializeComplexType(globalObject);
    Python::initializeStrType(globalObject);
    Python::initializeContainerTypes(globalObject);
    Python::initializeBytesTypes(globalObject);
    Python::initializeIteratorTypes(globalObject);
    Python::initializeExceptionTypes(globalObject);

    JSObject* builtins = Python::newModule(globalObject, "builtins"_s);
    m_builtinsModule.set(vm, this, builtins);

    Python::initializeBuiltinFunctions(globalObject, builtins);
    Python::initializeCodeTypes(globalObject, builtins);
    Python::initializeAsyncTypes(globalObject, builtins);
    Python::initializeTracebackTypes(globalObject);
    Python::initializeJavaScriptTypes(globalObject);
    static constexpr BuiltinType publicTypes[] = {
        BuiltinType::Object, BuiltinType::Type, BuiltinType::Int, BuiltinType::Bool, BuiltinType::Float, BuiltinType::Complex, BuiltinType::Str, BuiltinType::Bytes, BuiltinType::ByteArray, BuiltinType::MemoryView, BuiltinType::List, BuiltinType::Tuple, BuiltinType::Dict,
        BuiltinType::Set, BuiltinType::FrozenSet, BuiltinType::Range, BuiltinType::Slice, BuiltinType::Property, BuiltinType::StaticMethod, BuiltinType::ClassMethod, BuiltinType::Super,
        BuiltinType::Enumerate, BuiltinType::Zip, BuiltinType::Map, BuiltinType::Filter, BuiltinType::Reversed,
    };
    auto publish = [&] (PyType* type) {
        builtins->putDirect(vm, Identifier::fromString(vm, type->nameString(globalObject)), type);
    };
    for (BuiltinType type : publicTypes)
        publish(this->type(type));
#define PUBLISH(name, pythonName, base, layout, flags) publish(type##name());
    FOR_EACH_PYTHON_EXCEPTION_TYPE(PUBLISH)
#undef PUBLISH
    builtins->putDirect(vm, Identifier::fromString(vm, "EnvironmentError"_s), typeOSError());
    builtins->putDirect(vm, Identifier::fromString(vm, "IOError"_s), typeOSError());
    builtins->putDirect(vm, Identifier::fromString(vm, "NotImplemented"_s), m_notImplemented.get());
    builtins->putDirect(vm, Identifier::fromString(vm, "Ellipsis"_s), m_ellipsis.get());
    builtins->putDirect(vm, vm.pythonNames().dunder_debug, jsBoolean(true));
    builtins->putDirect(vm, vm.pythonNames().dunder_name, jsNontrivialString(vm, "builtins"_s));

    m_modules.set(vm, this, PyDict::create(globalObject));
    Python::registerModule(globalObject, "builtins"_s, builtins);
    m_runtimeFunctions.set(vm, this, Python::createRuntimeFunctions(vm, globalObject));
    m_javaScriptFunctions.set(vm, this, Python::createJavaScriptFunctions(vm, globalObject));
    // To JavaScript an exception is an Error, and one of a class that JavaScript has too is one of those. It is the other way about in typeOf().
    // Beyond a class of Python's, what JavaScript finds is what it would find for one of its own.
    typeBaseException()->setPrototypeDirect(vm, globalObject->errorPrototype());
    for (auto [type, errorType] : { std::pair { typeTypeError(), ErrorType::TypeError }, std::pair { typeSyntaxError(), ErrorType::SyntaxError }, std::pair { typeNameError(), ErrorType::ReferenceError }, std::pair { typeValueError(), ErrorType::RangeError }, std::pair { typeRecursionError(), ErrorType::RangeError }, std::pair { typeMemoryError(), ErrorType::RangeError } }) {
        type->setPrototypeDirect(vm, globalObject->errorStructure(errorType)->storedPrototype());
        for (unsigned i = 0; i < numberOfBuiltinTypes; ++i) {
            if (m_types[i]->isSubtypeOf(type))
                m_types[i]->setErrorType(errorType);
        }
    }
    Python::initializeLibrary(globalObject);
}

} // namespace JSC
