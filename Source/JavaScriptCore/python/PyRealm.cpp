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
#include "GlobalObjectMethodTable.h"
#include "ObjectConstructor.h"
#include "PythonBuiltins.h"
#include "PythonImport.h"
#include "PythonBytes.h"
#include "PythonContextVars.h"
#include "PythonRuntimeFunctions.h"
#include <wtf/CryptographicallyRandomNumber.h>

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
    for (auto& structure : thisObject->m_nativeFunctionStructures)
        visitor.append(structure);
    visitor.append(thisObject->m_hashStorageStructure);
    visitor.append(thisObject->m_emptyTuple);
    visitor.append(thisObject->m_emptyBytes);
    visitor.append(thisObject->m_notImplemented);
    thisObject->m_monitoring.visit(visitor);
    thisObject->m_warnings.visit(visitor);
    thisObject->m_threadModule.visit(visitor);
    thisObject->m_codecRegistry.visit(visitor);
    thisObject->m_importState.visit(visitor);
    {
        Locker locker { thisObject->m_moduleStatesLock };
        for (auto& state : thisObject->m_moduleStates.values())
            state->visitChildren(visitor);
    }
    thisObject->m_ioModule.visit(visitor);
    thisObject->m_posixModule.visit(visitor);
    thisObject->m_ast.visit(visitor);
    visitor.append(thisObject->m_ellipsis);
    visitor.append(thisObject->m_noDefault);
    visitor.append(thisObject->m_boundArgumentsMarker);
    visitor.append(thisObject->m_runtimeFunctions);
    visitor.append(thisObject->m_javaScriptFunctions);
    visitor.append(thisObject->m_frameLocalsProxyType);
    visitor.append(thisObject->m_exceptionGroupType);
    visitor.append(thisObject->m_currentContext);
    visitor.append(thisObject->m_asyncContextFrameStructure);
    visitor.append(thisObject->m_builtinsModule);
    visitor.append(thisObject->m_modules);
    visitor.append(thisObject->m_sysModule);
    visitor.append(thisObject->m_auditHooks);
    visitor.append(thisObject->m_asyncGeneratorFirstIterationHook);
    visitor.append(thisObject->m_asyncGeneratorFinalizerHook);
    visitor.append(thisObject->m_handledExceptions);
    visitor.append(thisObject->m_returnValue);
}

DEFINE_VISIT_CHILDREN(PyRealm);

static constexpr BuiltinType classesOfNativeFunctions[] = { BuiltinType::BuiltinFunction, BuiltinType::MethodDescriptor, BuiltinType::WrapperDescriptor, BuiltinType::ClassMethodDescriptor };

Structure* PyRealm::nativeFunctionStructure(BuiltinType type) const
{
    for (unsigned i = 0; i < std::size(classesOfNativeFunctions); ++i) {
        if (classesOfNativeFunctions[i] == type)
            return m_nativeFunctionStructures[i].get();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PyRealm::destroy(JSCell* cell)
{
    auto* realm = static_cast<PyRealm*>(cell);
    // The VM counts the realms in which something is being told of what is run, and this is one no longer.
    if (realm->m_monitoring.isWatching)
        realm->vm().removePythonWatcher();
    realm->PyRealm::~PyRealm();
}

JSString* PyRealm::intern(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The engine has one of each of these already.
    if (string->length() <= 1) {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!view->length())
            return jsEmptyString(vm);
        if (view[0] <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(view[0]);
    }
    auto atom = string->toAtomString(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (JSString* interned = m_internedStrings.get(atom.data))
        return interned;
    // It is made of the atom now, having been asked for it, and so keeps it in the table for as long as it lasts.
    m_internedStrings.set(atom.data, string);
    return string;
}

bool PyRealm::isInterned(JSGlobalObject* globalObject, JSString* string)
{
    if (string->length() <= 1)
        return intern(globalObject, string) == string;
    if (string->isRope())
        return false;
    StringImpl* impl = string->getValueImpl();
    return impl->isAtom() && m_internedStrings.get(impl) == string;
}

Structure* PyRealm::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

// _Py_HashRandomization_Init()
std::span<const uint8_t, 24> PyRealm::hashSecret()
{
    if (!m_hashSecret) {
        m_hashSecret.emplace();
        m_hashSecret->fill(0);
        if (!m_configuration.hashSeed)
            cryptographicallyRandomValues(std::span<uint8_t>(*m_hashSecret));
        else if (uint32_t x = *m_configuration.hashSeed) {
            // lcg_urandom()
            for (auto& byte : *m_hashSecret) {
                x = x * 214013 + 2531011;
                byte = static_cast<uint8_t>(x >> 16);
            }
        }
    }
    return *m_hashSecret;
}

PyRealm* PyRealm::create(VM& vm, JSGlobalObject* globalObject)
{
    // Before there is a cell for it: making a Structure can set off a collection, which is not to come upon a cell that nothing has been put in.
    Structure* structure = createStructure(vm, globalObject, jsNull());
    InternalFieldTuple* handledExceptions = InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), jsUndefined(), jsUndefined());
    auto* realm = new (NotNull, allocateCell<PyRealm>(vm)) PyRealm(vm, structure);
    realm->finishCreation(vm);
    realm->m_handledExceptions.set(vm, realm, handledExceptions);
    return realm;
}

// Every class has a __doc__, if only None.
static void putDocOfBuiltinType(VM& vm, JSGlobalObject* globalObject, PyType* type)
{
    if (type->getDirect(vm, vm.pythonNames().dunder_doc))
        return;
    auto* description = Python::findTypeDescription(type->nameWithoutModule(globalObject));
    type->putDirect(vm, vm.pythonNames().dunder_doc, description && !description->doc.isNull() ? JSValue(jsString(vm, String(description->doc))) : jsUndefined());
}

Python::NativeState& PyRealm::addModuleState(std::unique_ptr<Python::NativeState>&& state)
{
    Locker locker { m_moduleStatesLock };
    const void* kind = state->kind();
    return *m_moduleStates.add(kind, WTF::move(state)).iterator->value;
}

void PyRealm::initialize(VM& vm, JSGlobalObject* globalObject)
{
    DeferGC deferGC(vm);
    if (auto configure = globalObject->globalObjectMethodTable()->configurePython)
        configure(globalObject, m_configuration);

    // The types first, bare. A type has a tuple of bases, and a tuple has a type.
#define CREATE(name, pythonName, base, layout, flags) \
    m_types[static_cast<unsigned>(BuiltinType::name)].set(vm, this, PyType::createBuiltin(vm, globalObject, pythonName ""_s, BuiltinType::base == BuiltinType::None ? nullptr : type(BuiltinType::base), PyType::Layout::layout, flags));
    FOR_EACH_PYTHON_BUILTIN_TYPE(CREATE)
#undef CREATE

    m_tupleStructure.set(vm, this, PyTuple::createStructure(vm, globalObject, typeTuple()));
    m_hashStorageStructure.set(vm, this, PyHashStorage::createStructure(vm, globalObject, jsNull()));
    for (unsigned i = 0; i < std::size(classesOfNativeFunctions); ++i)
        m_nativeFunctionStructures[i].set(vm, this, PyNativeFunction::createStructure(vm, globalObject, type(classesOfNativeFunctions[i])));
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
    Python::initializeRangeType(globalObject);
    Python::initializeBytesTypes(globalObject);
    Python::initializeIteratorTypes(globalObject);
    Python::initializeExceptionTypes(globalObject);

    JSObject* builtins = Python::newBuiltinModule(globalObject, "builtins"_s);
    m_builtinsModule.set(vm, this, builtins);

    Python::initializeBuiltinFunctions(globalObject, builtins);
    Python::initializeCodeTypes(globalObject, builtins);
    Python::initializeAsyncTypes(globalObject, builtins);
    Python::initializeExceptionGroups(globalObject, builtins);
    m_asyncContextFrameStructure.set(vm, this, Python::createAsyncContextFrameStructure(vm, globalObject));
    Python::initializeWeakReferenceTypes(globalObject);
    Python::initializeContextVarTypes(globalObject);
    Python::initializeWarnings(globalObject);
    Python::initializeProperty(globalObject);
    Python::initializeReduce(globalObject);
    Python::initializeStructSequences(globalObject);
    Python::initializeAnnotations(globalObject);
    Python::initializeTemplateStrings(globalObject);
    Python::initializeGenericAliasAndUnion(globalObject);
    Python::initializeTypeParameters(globalObject);
    m_noDefault.set(vm, this, PyNativeObject::create(globalObject, BuiltinType::NoDefaultType));
    Python::initializeTracebackTypes(globalObject);
    Python::initializeJavaScriptTypes(globalObject);
    static constexpr BuiltinType publicTypes[] = {
        BuiltinType::Object, BuiltinType::Type, BuiltinType::Int, BuiltinType::Bool, BuiltinType::Float, BuiltinType::Complex, BuiltinType::Str, BuiltinType::Bytes, BuiltinType::ByteArray, BuiltinType::MemoryView, BuiltinType::List, BuiltinType::Tuple, BuiltinType::Dict,
        BuiltinType::Set, BuiltinType::FrozenSet, BuiltinType::Range, BuiltinType::Slice, BuiltinType::Property, BuiltinType::StaticMethod, BuiltinType::ClassMethod, BuiltinType::Super,
        BuiltinType::Enumerate, BuiltinType::Zip, BuiltinType::Map, BuiltinType::Filter, BuiltinType::Reversed,
    };
    auto publish = [&] (PyType* type) {
        builtins->putDirect(vm, Identifier::fromString(vm, type->nameWithoutModule(globalObject)), type);
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
    // They are not names that anything looks up, but they are there for whoever looks at what there is.
    builtins->putDirect(vm, Identifier::fromString(vm, "None"_s), jsUndefined());
    builtins->putDirect(vm, Identifier::fromString(vm, "False"_s), jsBoolean(false));
    builtins->putDirect(vm, Identifier::fromString(vm, "True"_s), jsBoolean(true));
    builtins->putDirect(vm, vm.pythonNames().dunder_debug, jsBoolean(!m_configuration.optimizationLevel));
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
    for (auto& type : m_types)
        putDocOfBuiltinType(vm, globalObject, type.get());
    m_sysModule.set(vm, this, Python::createSysModule(globalObject));
    Python::registerModule(globalObject, "sys"_s, m_sysModule.get());
    // import js: JavaScript's global object, as it is. It is there already, so that importing it makes nothing of it.
    Python::registerModule(globalObject, "js"_s, globalObject->globalThis());
    Python::initializeImport(globalObject);
    Python::initializeLibrary(globalObject);
}

namespace Python {

PyType* createBuiltinType(JSGlobalObject* globalObject, ASCIILiteral name, PyType* base, PyType::Layout layout, unsigned flags)
{
    VM& vm = globalObject->vm();
    DeferGC deferGC(vm);
    PyType* type = PyType::createBuiltin(vm, globalObject, name, base, layout, flags);
    type->finishBuiltin(vm, globalObject, globalObject->pyRealm()->typeType());
    putDocOfBuiltinType(vm, globalObject, type);
    addClassGetItemIfGeneric(globalObject, type);
    // One that CPython makes with PyType_FromSpec(), as a class statement makes a class, has where it is from among what is in it. Py_TPFLAGS_HEAPTYPE says which those are.
    constexpr unsigned long cpythonHeapType = 1ul << 9;
    if (String module = type->moduleOfBuiltin(); !module.isNull() && (type->flagsForPython() & cpythonHeapType))
        type->putDirect(vm, vm.pythonNames().dunder_module, jsString(vm, module));
    return type;
}

} // namespace Python

} // namespace JSC
