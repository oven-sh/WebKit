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
#include "PythonMonitoring.h"
#include "PythonASTModule.h"
#include "PythonConfiguration.h"
#include "PythonIOState.h"
#include "PythonPosixState.h"
#include "PythonThreadModule.h"
#include "PythonWarnings.h"
#include "WeakGCMap.h"

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
    v(WrapperDescriptor, "wrapper_descriptor", Object, Native, 0) \
    v(MethodWrapper, "method-wrapper", Object, Native, 0) \
    v(ClassMethodDescriptor, "classmethod_descriptor", Object, Native, 0) \
    v(GetSetDescriptor, "getset_descriptor", Object, Native, 0) \
    v(MemberDescriptor, "member_descriptor", Object, Native, 0) \
    v(Method, "method", Object, Native, 0) \
    v(Property, "property", Object, Native, PyType::IsBaseType) \
    v(StaticMethod, "staticmethod", Object, Native, PyType::IsBaseType) \
    v(ClassMethod, "classmethod", Object, Native, PyType::IsBaseType) \
    v(Super, "super", Object, Native, PyType::IsBaseType) \
    v(Module, "module", Object, Object, PyType::IsBaseType) \
    v(SimpleNamespace, "types.SimpleNamespace", Object, Object, PyType::IsBaseType) \
    v(SysFlags, "sys.flags", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysFloatInfo, "sys.float_info", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysIntInfo, "sys.int_info", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysHashInfo, "sys.hash_info", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysVersionInfo, "sys.version_info", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysThreadInfo, "sys.thread_info", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(SysAsyncGeneratorHooks, "asyncgen_hooks", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(UnraisableHookArgs, "UnraisableHookArgs", Tuple, Tuple, PyType::MatchesSelf | PyType::IsSequence) \
    v(Generator, "generator", Object, Native, 0) \
    v(Coroutine, "coroutine", Object, Native, 0) \
    v(AsyncGenerator, "async_generator", Object, Native, 0) \
    v(CoroutineWrapper, "coroutine_wrapper", Object, Native, 0) \
    v(AsyncGeneratorASend, "async_generator_asend", Object, Native, 0) \
    v(AsyncGeneratorAThrow, "async_generator_athrow", Object, Native, 0) \
    v(AsyncGeneratorWrappedValue, "async_generator_wrapped_value", Object, Native, 0) \
    v(ANextAwaitable, "anext_awaitable", Object, Native, 0) \
    v(PromiseAwaiter, "promise_awaiter", Object, Native, 0) \
    v(GenericAlias, "types.GenericAlias", Object, Native, PyType::IsBaseType) \
    v(GenericAliasIterator, "generic_alias_iterator", Object, Native, 0) \
    v(Union, "typing.Union", Object, Native, 0) \
    v(TypeVar, "typing.TypeVar", Object, Native, 0) \
    v(ParamSpec, "typing.ParamSpec", Object, Native, 0) \
    v(ParamSpecArgs, "typing.ParamSpecArgs", Object, Native, 0) \
    v(ParamSpecKwargs, "typing.ParamSpecKwargs", Object, Native, 0) \
    v(TypeVarTuple, "typing.TypeVarTuple", Object, Native, 0) \
    v(TypeAliasType, "typing.TypeAliasType", Object, Native, 0) \
    v(Generic, "typing.Generic", Object, Object, PyType::IsBaseType) \
    v(NoDefaultType, "NoDefaultType", Object, Native, 0) \
    v(ConstEvaluator, "_typing._ConstEvaluator", Object, Native, 0) \
    v(Template, "string.templatelib.Template", Object, Native, 0) \
    v(TemplateIter, "string.templatelib.TemplateIter", Object, Native, 0) \
    v(Interpolation, "string.templatelib.Interpolation", Object, Native, 0) \
    v(Cell, "cell", Object, Native, 0) \
    v(Code, "code", Object, Native, 0) \
    v(Frame, "frame", Object, Native, 0) \
    v(StandardStream, "TextIOWrapper", Object, Object, 0) \
    v(Traceback, "traceback", Object, Native, 0) \
    v(MappingProxy, "mappingproxy", Object, Native, PyType::IsMapping) \
    v(ListIterator, "list_iterator", Object, Native, 0) \
    v(ListReverseIterator, "list_reverseiterator", Object, Native, 0) \
    v(TupleIterator, "tuple_iterator", Object, Native, 0) \
    v(LineIterator, "line_iterator", Object, Native, 0) \
    v(PositionsIterator, "positions_iterator", Object, Native, 0) \
    v(WeakReference, "weakref.ReferenceType", Object, Native, PyType::IsBaseType) \
    v(WeakProxy, "weakref.ProxyType", Object, Native, 0) \
    v(WeakCallableProxy, "weakref.CallableProxyType", Object, Native, 0) \
    v(Context, "_contextvars.Context", Object, Native, PyType::HasWeakReferences) \
    v(ContextVar, "_contextvars.ContextVar", Object, Native, 0) \
    v(Token, "_contextvars.Token", Object, Native, 0) \
    v(TokenMissing, "Token.MISSING", Object, Native, 0) \
    v(ContextKeys, "keys", Object, Native, 0) \
    v(ContextValues, "values", Object, Native, 0) \
    v(ContextItems, "items", Object, Native, 0) \
    v(RangeIterator, "range_iterator", Object, Native, 0) \
    v(LongRangeIterator, "longrange_iterator", Object, Native, 0) \
    v(StrAsciiIterator, "str_ascii_iterator", Object, Native, 0) \
    v(StrIterator, "str_iterator", Object, Native, 0) \
    v(BytesIterator, "bytes_iterator", Object, Native, 0) \
    v(ByteArrayIterator, "bytearray_iterator", Object, Native, 0) \
    v(MemoryIterator, "memory_iterator", Object, Native, 0) \
    v(BufferWrapper, "_buffer_wrapper", Object, Native, 0) \
    v(DictKeyIterator, "dict_keyiterator", Object, Native, 0) \
    v(DictValueIterator, "dict_valueiterator", Object, Native, 0) \
    v(DictItemIterator, "dict_itemiterator", Object, Native, 0) \
    v(DictReverseKeyIterator, "dict_reversekeyiterator", Object, Native, 0) \
    v(DictReverseValueIterator, "dict_reversevalueiterator", Object, Native, 0) \
    v(DictReverseItemIterator, "dict_reverseitemiterator", Object, Native, 0) \
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
    v(JSObject, "Object", Object, JavaScript, PyType::IsBaseType | PyType::IsJavaScript | PyType::HasInstanceDict | PyType::HasWeakReferences) \
    v(JSFunction, "Function", JSObject, Native, PyType::IsJavaScript | PyType::HasWeakReferences) \
    v(JSPromise, "Promise", JSObject, JavaScript, PyType::IsBaseType | PyType::IsJavaScript | PyType::HasInstanceDict | PyType::HasWeakReferences) \
    v(JSSymbol, "JSSymbol", Object, Native, 0) \
    FOR_EACH_PYTHON_EXCEPTION_TYPE(v)

#define PYTHON_EXCEPTION_FLAGS (PyType::IsBaseType | PyType::IsExceptionType)

#define FOR_EACH_PYTHON_EXCEPTION_TYPE(v) \
    v(BaseException, "BaseException", Object, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(BaseExceptionGroup, "BaseExceptionGroup", BaseException, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(GeneratorExit, "GeneratorExit", BaseException, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(KeyboardInterrupt, "KeyboardInterrupt", BaseException, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(SystemExit, "SystemExit", BaseException, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(Exception, "Exception", BaseException, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ArithmeticError, "ArithmeticError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(FloatingPointError, "FloatingPointError", ArithmeticError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(OverflowError, "OverflowError", ArithmeticError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ZeroDivisionError, "ZeroDivisionError", ArithmeticError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(AssertionError, "AssertionError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(AttributeError, "AttributeError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(BufferError, "BufferError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(EOFError, "EOFError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ImportError, "ImportError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ModuleNotFoundError, "ModuleNotFoundError", ImportError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(LookupError, "LookupError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(IndexError, "IndexError", LookupError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(KeyError, "KeyError", LookupError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(MemoryError, "MemoryError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(NameError, "NameError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnboundLocalError, "UnboundLocalError", NameError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(OSError, "OSError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(BlockingIOError, "BlockingIOError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ChildProcessError, "ChildProcessError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionError, "ConnectionError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(BrokenPipeError, "BrokenPipeError", ConnectionError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionAbortedError, "ConnectionAbortedError", ConnectionError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionRefusedError, "ConnectionRefusedError", ConnectionError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ConnectionResetError, "ConnectionResetError", ConnectionError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(FileExistsError, "FileExistsError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(FileNotFoundError, "FileNotFoundError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(InterruptedError, "InterruptedError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(IsADirectoryError, "IsADirectoryError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(NotADirectoryError, "NotADirectoryError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(PermissionError, "PermissionError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ProcessLookupError, "ProcessLookupError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(TimeoutError, "TimeoutError", OSError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ReferenceError, "ReferenceError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(RuntimeError, "RuntimeError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(NotImplementedError, "NotImplementedError", RuntimeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(PythonFinalizationError, "PythonFinalizationError", RuntimeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(RecursionError, "RecursionError", RuntimeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(StopAsyncIteration, "StopAsyncIteration", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(StopIteration, "StopIteration", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(SyntaxError, "SyntaxError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(IndentationError, "IndentationError", SyntaxError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(IncompleteInputError, "_IncompleteInputError", SyntaxError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(TabError, "TabError", IndentationError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(SystemError, "SystemError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(TypeError, "TypeError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ValueError, "ValueError", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeError, "UnicodeError", ValueError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeDecodeError, "UnicodeDecodeError", UnicodeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeEncodeError, "UnicodeEncodeError", UnicodeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeTranslateError, "UnicodeTranslateError", UnicodeError, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(Warning, "Warning", Exception, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(BytesWarning, "BytesWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(DeprecationWarning, "DeprecationWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(EncodingWarning, "EncodingWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(FutureWarning, "FutureWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ImportWarning, "ImportWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(PendingDeprecationWarning, "PendingDeprecationWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(ResourceWarning, "ResourceWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(RuntimeWarning, "RuntimeWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(SyntaxWarning, "SyntaxWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UnicodeWarning, "UnicodeWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(UserWarning, "UserWarning", Warning, Exception, PYTHON_EXCEPTION_FLAGS) \
    v(JSError, "JSError", Exception, Native, PyType::IsExceptionType | PyType::IsJavaScript)

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
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyRealmSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    PYTHON_OVERLOADS_OPERATORS
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
    // Of a PyNativeFunction, which is of one of four classes.
    Structure* nativeFunctionStructure(BuiltinType) const;
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
        ObjectReduce,
        ObjectGetState,
        TypeCall,
        TypeGetAttribute,
        ModuleGetAttribute,
        TypeSetAttr,
        TypeDelAttr,
        Count,
    };
    JSObject* function(WellKnownFunction function) const { return m_functions[static_cast<unsigned>(function)].get(); }
    void setFunction(VM& vm, WellKnownFunction function, JSObject* object) { m_functions[static_cast<unsigned>(function)].set(vm, this, object); }

    PyTuple* emptyTuple() const { return m_emptyTuple.get(); }
    JSObject* notImplemented() const { return m_notImplemented.get(); }
    JSObject* ellipsis() const { return m_ellipsis.get(); }
    JSObject* noDefault() const { return m_noDefault.get(); } // typing.NoDefault

    // `this`, in a call whose arguments are the function's parameters, one for one.
    JSObject* boundArgumentsMarker() const { return m_boundArgumentsMarker.get(); }
    // What compiled code calls for what there is no opcode for. See PythonRuntimeFunctions.h.
    JSObject* runtimeFunctions() const { return m_runtimeFunctions.get(); }
    // What frame.f_locals is an instance of. It is written in Python.
    PyType* frameLocalsProxyType() const { return m_frameLocalsProxyType.get(); }
    // The contextvars.Context that has been entered and not left, if any. See PythonContextVars.cpp.
    JSObject* currentContext() const { return m_currentContext.get(); }
    void setCurrentContext(VM& vm, JSObject* context) { m_currentContext.setMayBeNull(vm, this, context); }
    Structure* asyncContextFrameStructure() const { return m_asyncContextFrameStructure.get(); }
    // It has two bases, and so is not among the types that are made from a table.
    PyType* exceptionGroupType() const { return m_exceptionGroupType.get(); }
    void setExceptionGroupType(VM& vm, PyType* type) { m_exceptionGroupType.set(vm, this, type); }
    void setFrameLocalsProxyType(VM& vm, PyType* type) { m_frameLocalsProxyType.set(vm, this, type); }
    // The functions behind the names that JavaScript uses for what Python calls something else. See PythonJavaScript.cpp.
    JSObject* javaScriptFunctions() const { return m_javaScriptFunctions.get(); }
    JSObject* javaScriptFunction(ASCIILiteral name) const { return asObject(m_javaScriptFunctions->getDirect(vm(), Identifier::fromString(vm(), name))); }
    // The namespace of the builtins module, which is where a global name is looked for after the module's own.
    // The module builtins.
    JSObject* builtinsModule() const { return m_builtinsModule.get(); }
    // sys.intern(): the one str that stands for all that are equal to it, for as long as anything refers to it. As the engine has one Symbol for a SymbolImpl.
    JSString* intern(JSGlobalObject*, JSString*);
    bool isInterned(JSGlobalObject*, JSString*);

    // What sys gets and sets.
    JSArray* auditHooks() const { return m_auditHooks.get(); } // Null until there is one.
    void setAuditHooks(VM& vm, JSArray* hooks) { m_auditHooks.set(vm, this, hooks); }
    JSValue asyncGeneratorFirstIterationHook() const { return m_asyncGeneratorFirstIterationHook.get(); } // Empty if there is none.
    JSValue asyncGeneratorFinalizerHook() const { return m_asyncGeneratorFinalizerHook.get(); }
    void setAsyncGeneratorFirstIterationHook(VM& vm, JSValue hook) { m_asyncGeneratorFirstIterationHook.set(vm, this, hook); }
    void setAsyncGeneratorFinalizerHook(VM& vm, JSValue hook) { m_asyncGeneratorFinalizerHook.set(vm, this, hook); }
    int maximumDigitsOfIntAsString { 4300 }; // 0 for as many as there are.
    int coroutineOriginTrackingDepth { 0 };
    double switchInterval { 0.005 };

    // What is in the middle of being written out. See Python::ReprGuard. Each is on the stack besides, and so is not visited.
    Vector<JSCell*, 16>& objectsBeingWrittenOut() { return m_objectsBeingWrittenOut; }
    Python::MonitoringState& monitoring() { return m_monitoring; }
    Python::WarningsState& warnings() { return m_warnings; }
    Python::ThreadModuleState& threadModule() { return m_threadModule; }
    Python::IOModuleState& ioModule() { return m_ioModule; }
    Python::PosixModuleState& posixModule() { return m_posixModule; }
    // What whoever embeds the engine had to say, which it was asked when this was made.
    const Python::Configuration& configuration() const { return m_configuration; }
    Python::ASTState& ast() { return m_ast; }

    // sys.modules
    JSObject* modules() const { return m_modules.get(); }

    // The exception being handled: what sys.exception() gives, and what a new exception's __context__ is.
    // The exception that is being handled: what sys.exception() gives, a bare `raise` raises again, and a new exception has for its context.
    // A generator has its own, which is put away with it when it yields. While it has none, it is that of whatever resumed it.
    // As it was thrown, so that a bare `raise` throws that again, and not a copy that has forgotten where it was first thrown.
    Exception* handledThrown() const { return m_handledException ? m_handledException.get() : m_outerHandledException.get(); }
    JSValue handledException() const { return handledThrown() ? handledThrown()->value() : JSValue(); }
    Exception* ownHandledException() const { return m_handledException.get(); }
    void setOwnHandledException(VM& vm, Exception* exception) { m_handledException.setMayBeNull(vm, this, exception); }
    Exception* outerHandledException() const { return m_outerHandledException.get(); }
    void setOuterHandledException(VM& vm, Exception* exception) { m_outerHandledException.setMayBeNull(vm, this, exception); }

    // What the iterator that a `yield from` was going through returned, on its way from the runtime to the code that wants it.
    JSValue takeReturnValue() { return std::exchange(m_returnValue, WriteBarrier<Unknown>()).get(); }
    void setReturnValue(VM& vm, JSValue value) { m_returnValue.set(vm, this, value); }

private:
    PyRealm(VM& vm, Structure* structure)
        : Base(vm, structure)
        , m_internedStrings(vm)
    {
    }

    WriteBarrier<PyType> m_types[numberOfBuiltinTypes];
    WriteBarrier<JSObject> m_functions[static_cast<unsigned>(WellKnownFunction::Count)];
    WriteBarrier<Structure> m_tupleStructure;
    WriteBarrier<Structure> m_nativeFunctionStructures[4];
    WriteBarrier<Structure> m_hashStorageStructure;
    WriteBarrier<PyTuple> m_emptyTuple;
    WriteBarrier<JSObject> m_notImplemented;
    WriteBarrier<JSObject> m_ellipsis;
    WriteBarrier<JSObject> m_noDefault;
    WriteBarrier<JSObject> m_boundArgumentsMarker;
    WriteBarrier<JSObject> m_runtimeFunctions;
    WriteBarrier<JSObject> m_javaScriptFunctions;
    WriteBarrier<PyType> m_frameLocalsProxyType;
    WriteBarrier<PyType> m_exceptionGroupType;
    WriteBarrier<JSObject> m_currentContext;
    WriteBarrierStructureID m_asyncContextFrameStructure;
    WriteBarrier<JSObject> m_builtinsModule;
    Vector<JSCell*, 16> m_objectsBeingWrittenOut;
    Python::MonitoringState m_monitoring;
    Python::WarningsState m_warnings;
    Python::ThreadModuleState m_threadModule;
    Python::IOModuleState m_ioModule;
    Python::PosixModuleState m_posixModule;
    Python::Configuration m_configuration;
    Python::ASTState m_ast;
    // By the string in the table of atoms, which the str keeps there.
    WeakGCMap<StringImpl*, JSString, PtrHash<StringImpl*>> m_internedStrings;
    WriteBarrier<JSArray> m_auditHooks;
    WriteBarrier<Unknown> m_asyncGeneratorFirstIterationHook;
    WriteBarrier<Unknown> m_asyncGeneratorFinalizerHook;
    WriteBarrier<JSObject> m_modules;
    WriteBarrier<Exception> m_handledException;
    WriteBarrier<Exception> m_outerHandledException;
    WriteBarrier<Unknown> m_returnValue;
};

} // namespace JSC
