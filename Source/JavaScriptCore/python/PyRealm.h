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

#pragma once

#include "JSObject.h"
#include "PyType.h"

namespace JSC {

// v(name in C++, name in Python, base, layout, flags)
#define FOR_EACH_PYTHON_BUILTIN_TYPE(v) \
    v(Object, "object", None, Object, PyType::IsBaseType) \
    v(Type, "type", Object, Type, PyType::IsBaseType | PyType::IsTypeSubclass) \
    v(NoneType, "NoneType", Object, Native, 0) \
    v(NotImplementedType, "NotImplementedType", Object, Native, 0) \
    v(Ellipsis, "ellipsis", Object, Native, 0) \
    v(Int, "int", Object, Boxed, PyType::IsBaseType | PyType::MatchesSelf) \
    v(Bool, "bool", Int, Native, PyType::MatchesSelf) \
    v(Float, "float", Object, Boxed, PyType::IsBaseType | PyType::MatchesSelf) \
    v(Complex, "complex", Object, Native, PyType::IsBaseType) \
    v(Str, "str", Object, Boxed, PyType::IsBaseType | PyType::MatchesSelf) \
    v(Bytes, "bytes", Object, Native, PyType::IsBaseType | PyType::MatchesSelf | PyType::IsBytes) \
    v(ByteArray, "bytearray", Object, Native, PyType::IsBaseType | PyType::MatchesSelf) \
    v(MemoryView, "memoryview", Object, Native, PyType::IsSequence) \
    v(List, "list", Object, List, PyType::IsBaseType | PyType::MatchesSelf | PyType::IsSequence) \
    v(Tuple, "tuple", Object, Tuple, PyType::IsBaseType | PyType::MatchesSelf | PyType::IsSequence) \
    v(Dict, "dict", Object, Dict, PyType::IsBaseType | PyType::MatchesSelf | PyType::IsMapping) \
    v(Set, "set", Object, Set, PyType::IsBaseType | PyType::MatchesSelf) \
    v(FrozenSet, "frozenset", Object, Set, PyType::IsBaseType | PyType::MatchesSelf) \
    v(Range, "range", Object, Native, PyType::IsSequence) \
    v(Slice, "slice", Object, Native, 0) \
    v(Function, "function", Object, Native, 0) \
    v(BuiltinFunction, "builtin_function_or_method", Object, Native, 0) \
    v(MethodDescriptor, "method_descriptor", Object, Native, 0) \
    v(ClassMethodDescriptor, "classmethod_descriptor", Object, Native, 0) \
    v(GetSetDescriptor, "getset_descriptor", Object, Native, 0) \
    v(MemberDescriptor, "member_descriptor", Object, Native, 0) \
    v(Method, "method", Object, Native, 0) \
    v(Property, "property", Object, Native, PyType::IsBaseType) \
    v(StaticMethod, "staticmethod", Object, Native, PyType::IsBaseType) \
    v(ClassMethod, "classmethod", Object, Native, PyType::IsBaseType) \
    v(Super, "super", Object, Native, PyType::IsBaseType) \
    v(Module, "module", Object, Native, PyType::IsBaseType) \
    v(Generator, "generator", Object, Native, 0) \
    v(Coroutine, "coroutine", Object, Native, 0) \
    v(AsyncGenerator, "async_generator", Object, Native, 0) \
    v(CoroutineWrapper, "coroutine_wrapper", Object, Native, 0) \
    v(AsyncGeneratorASend, "async_generator_asend", Object, Native, 0) \
    v(AsyncGeneratorAThrow, "async_generator_athrow", Object, Native, 0) \
    v(AsyncGeneratorWrappedValue, "async_generator_wrapped_value", Object, Native, 0) \
    v(ANextAwaitable, "anext_awaitable", Object, Native, 0) \
    v(Cell, "cell", Object, Native, 0) \
    v(Code, "code", Object, Native, 0) \
    v(Frame, "frame", Object, Native, 0) \
    v(MappingProxy, "mappingproxy", Object, Native, PyType::IsMapping) \
    v(ListIterator, "list_iterator", Object, Native, 0) \
    v(ListReverseIterator, "list_reverseiterator", Object, Native, 0) \
    v(TupleIterator, "tuple_iterator", Object, Native, 0) \
    v(RangeIterator, "range_iterator", Object, Native, 0) \
    v(StrIterator, "str_ascii_iterator", Object, Native, 0) \
    v(BytesIterator, "bytes_iterator", Object, Native, 0) \
    v(DictKeyIterator, "dict_keyiterator", Object, Native, 0) \
    v(DictValueIterator, "dict_valueiterator", Object, Native, 0) \
    v(DictItemIterator, "dict_itemiterator", Object, Native, 0) \
    v(DictReverseKeyIterator, "dict_reversekeyiterator", Object, Native, 0) \
    v(SetIterator, "set_iterator", Object, Native, 0) \
    v(SequenceIterator, "iterator", Object, Native, 0) \
    v(CallableIterator, "callable_iterator", Object, Native, 0) \
    v(DictKeys, "dict_keys", Object, Native, 0) \
    v(DictValues, "dict_values", Object, Native, 0) \
    v(DictItems, "dict_items", Object, Native, 0) \
    v(Enumerate, "enumerate", Object, Native, PyType::IsBaseType) \
    v(Zip, "zip", Object, Native, PyType::IsBaseType) \
    v(Map, "map", Object, Native, PyType::IsBaseType) \
    v(Filter, "filter", Object, Native, PyType::IsBaseType) \
    v(Reversed, "reversed", Object, Native, PyType::IsBaseType) \
    v(JSObject, "JSObject", Object, Native, 0) \
    v(JSSymbol, "JSSymbol", Object, Native, 0) \
    FOR_EACH_PYTHON_EXCEPTION_TYPE(v)

#define PYTHON_EXCEPTION_FLAGS (PyType::IsBaseType | PyType::IsExceptionType)

#define FOR_EACH_PYTHON_EXCEPTION_TYPE(v) \
    v(BaseException, "BaseException", Object, Object, PYTHON_EXCEPTION_FLAGS) \
    v(BaseExceptionGroup, "BaseExceptionGroup", BaseException, Object, PYTHON_EXCEPTION_FLAGS) \
    v(GeneratorExit, "GeneratorExit", BaseException, Object, PYTHON_EXCEPTION_FLAGS) \
    v(KeyboardInterrupt, "KeyboardInterrupt", BaseException, Object, PYTHON_EXCEPTION_FLAGS) \
    v(SystemExit, "SystemExit", BaseException, Object, PYTHON_EXCEPTION_FLAGS) \
    v(Exception, "Exception", BaseException, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ArithmeticError, "ArithmeticError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(FloatingPointError, "FloatingPointError", ArithmeticError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(OverflowError, "OverflowError", ArithmeticError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ZeroDivisionError, "ZeroDivisionError", ArithmeticError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(AssertionError, "AssertionError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(AttributeError, "AttributeError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(BufferError, "BufferError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(EOFError, "EOFError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ImportError, "ImportError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ModuleNotFoundError, "ModuleNotFoundError", ImportError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(LookupError, "LookupError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(IndexError, "IndexError", LookupError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(KeyError, "KeyError", LookupError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(MemoryError, "MemoryError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(NameError, "NameError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnboundLocalError, "UnboundLocalError", NameError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(OSError, "OSError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(BlockingIOError, "BlockingIOError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ChildProcessError, "ChildProcessError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionError, "ConnectionError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(BrokenPipeError, "BrokenPipeError", ConnectionError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionAbortedError, "ConnectionAbortedError", ConnectionError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionRefusedError, "ConnectionRefusedError", ConnectionError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionResetError, "ConnectionResetError", ConnectionError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(FileExistsError, "FileExistsError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(FileNotFoundError, "FileNotFoundError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(InterruptedError, "InterruptedError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(IsADirectoryError, "IsADirectoryError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(NotADirectoryError, "NotADirectoryError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(PermissionError, "PermissionError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ProcessLookupError, "ProcessLookupError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(TimeoutError, "TimeoutError", OSError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ReferenceError, "ReferenceError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(RuntimeError, "RuntimeError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(NotImplementedError, "NotImplementedError", RuntimeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(PythonFinalizationError, "PythonFinalizationError", RuntimeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(RecursionError, "RecursionError", RuntimeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(StopAsyncIteration, "StopAsyncIteration", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(StopIteration, "StopIteration", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(SyntaxError, "SyntaxError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(IndentationError, "IndentationError", SyntaxError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(TabError, "TabError", IndentationError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(SystemError, "SystemError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(TypeError, "TypeError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ValueError, "ValueError", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeError, "UnicodeError", ValueError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeDecodeError, "UnicodeDecodeError", UnicodeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeEncodeError, "UnicodeEncodeError", UnicodeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeTranslateError, "UnicodeTranslateError", UnicodeError, Object, PYTHON_EXCEPTION_FLAGS) \
    v(Warning, "Warning", Exception, Object, PYTHON_EXCEPTION_FLAGS) \
    v(BytesWarning, "BytesWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(DeprecationWarning, "DeprecationWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(EncodingWarning, "EncodingWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(FutureWarning, "FutureWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ImportWarning, "ImportWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(PendingDeprecationWarning, "PendingDeprecationWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(ResourceWarning, "ResourceWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(RuntimeWarning, "RuntimeWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(SyntaxWarning, "SyntaxWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeWarning, "UnicodeWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(UserWarning, "UserWarning", Warning, Object, PYTHON_EXCEPTION_FLAGS) \
    v(JSError, "JSError", Exception, Native, PyType::IsExceptionType)

enum class BuiltinType : uint8_t {
#define DECLARE(name, pythonName, base, layout, flags) name,
    FOR_EACH_PYTHON_BUILTIN_TYPE(DECLARE)
#undef DECLARE
    None, // Not one: what object has for a base.
};
static constexpr unsigned numberOfBuiltinTypes = static_cast<unsigned>(BuiltinType::None);

// What Python has one of for each global object: its built-in types, the builtins module, and the modules that have been imported.
class PyRealm final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    // Empty. initialize() fills it in, once the global object knows of it.
    JS_EXPORT_PRIVATE static PyRealm* create(VM&, JSGlobalObject*);
    JS_EXPORT_PRIVATE void initialize(VM&, JSGlobalObject*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    PyType* type(BuiltinType type) const { return m_types[static_cast<unsigned>(type)].get(); }
#define DECLARE(name, pythonName, base, layout, flags) PyType* type##name() const { return type(BuiltinType::name); }
    FOR_EACH_PYTHON_BUILTIN_TYPE(DECLARE)
#undef DECLARE

    Structure* tupleStructure() const { return m_tupleStructure.get(); }
    Structure* nativeFunctionStructure() const { return m_nativeFunctionStructure.get(); }
    Structure* namespaceStructure() const { return m_namespaceStructure.get(); }
    Structure* hashStorageStructure() const { return m_hashStorageStructure.get(); }
    // Of what a cell class makes for the built-in type it is the cell of. They are made by the class when the type is set up.
    Structure* structureFor(BuiltinType type) const { return this->type(type)->instanceStructure(); }

    // Methods of object and of type that a class has unless it says otherwise, and then things are simpler.
    enum class WellKnownFunction : uint8_t {
        ObjectNew,
        ObjectInit,
        ObjectGetAttribute,
        ObjectSetAttr,
        ObjectDelAttr,
        ObjectEq,
        ObjectNe,
        ObjectHash,
        ObjectRepr,
        ObjectStr,
        ObjectFormat,
        TypeCall,
        TypeGetAttribute,
        TypeSetAttr,
        TypeDelAttr,
        Count,
    };
    JSObject* function(WellKnownFunction function) const { return m_functions[static_cast<unsigned>(function)].get(); }
    void setFunction(VM& vm, WellKnownFunction function, JSObject* object) { m_functions[static_cast<unsigned>(function)].set(vm, this, object); }

    PyTuple* emptyTuple() const { return m_emptyTuple.get(); }
    JSObject* notImplemented() const { return m_notImplemented.get(); }
    JSObject* ellipsis() const { return m_ellipsis.get(); }

    // `this`, in a call whose arguments are the function's parameters, one for one.
    JSObject* boundArgumentsMarker() const { return m_boundArgumentsMarker.get(); }
    // What compiled code calls for what there is no opcode for. See PythonRuntimeFunctions.h.
    JSObject* runtimeFunctions() const { return m_runtimeFunctions.get(); }
    // The namespace of the builtins module, which is where a global name is looked for after the module's own.
    JSObject* builtinsNamespace() const { return m_builtinsNamespace.get(); }
    // sys.modules
    JSObject* modules() const { return m_modules.get(); }

    // The exception being handled: what sys.exception() gives, and what a new exception's __context__ is.
    // The exception that is being handled: what sys.exception() gives, a bare `raise` raises again, and a new exception has for its context.
    // A generator has its own, which is put away with it when it yields. While it has none, it is that of whatever resumed it.
    JSValue handledException() const { return m_handledException ? m_handledException.get() : m_outerHandledException.get(); }
    JSValue ownHandledException() const { return m_handledException.get(); }
    void setOwnHandledException(VM& vm, JSValue exception) { m_handledException.set(vm, this, exception); }
    JSValue outerHandledException() const { return m_outerHandledException.get(); }
    void setOuterHandledException(VM& vm, JSValue exception) { m_outerHandledException.set(vm, this, exception); }

    // What the iterator that a `yield from` was going through returned, on its way from the runtime to the code that wants it.
    JSValue takeReturnValue() { return std::exchange(m_returnValue, WriteBarrier<Unknown>()).get(); }
    void setReturnValue(VM& vm, JSValue value) { m_returnValue.set(vm, this, value); }

private:
    PyRealm(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    WriteBarrier<PyType> m_types[numberOfBuiltinTypes];
    WriteBarrier<JSObject> m_functions[static_cast<unsigned>(WellKnownFunction::Count)];
    WriteBarrier<Structure> m_tupleStructure;
    WriteBarrier<Structure> m_nativeFunctionStructure;
    WriteBarrier<Structure> m_namespaceStructure;
    WriteBarrier<Structure> m_hashStorageStructure;
    WriteBarrier<PyTuple> m_emptyTuple;
    WriteBarrier<JSObject> m_notImplemented;
    WriteBarrier<JSObject> m_ellipsis;
    WriteBarrier<JSObject> m_boundArgumentsMarker;
    WriteBarrier<JSObject> m_runtimeFunctions;
    WriteBarrier<JSObject> m_builtinsNamespace;
    WriteBarrier<JSObject> m_modules;
    WriteBarrier<Unknown> m_handledException;
    WriteBarrier<Unknown> m_outerHandledException;
    WriteBarrier<Unknown> m_returnValue;
};

} // namespace JSC
