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

#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include <wtf/HexNumber.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace Python {

// For the files that define the built-in types and functions.

struct MethodDefinition {
    ASCIILiteral name;
    NativeFunction function;
    PyNativeFunction::Kind kind { PyNativeFunction::Kind::Method };
    unsigned data { 0 };
    ASCIILiteral signature { }; // For what CPython does not have. See PyNativeFunction::create().
    PyNativeFunction::Arguments arguments { PyNativeFunction::Arguments::AreChecked };
};

// Up to three small values, for PyNativeFunction::data().
template<typename A>
constexpr unsigned pack(A a) { return static_cast<unsigned>(a) & 0xFF; }
template<typename A, typename B>
constexpr unsigned pack(A a, B b) { return pack(a) | pack(b) << 8; }
template<typename A, typename B, typename C>
constexpr unsigned pack(A a, B b, C c) { return pack(a, b) | pack(c) << 16; }

template<typename T>
inline T unpack(CallFrame* callFrame, unsigned position)
{
    return static_cast<T>((uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->data() >> (position * 8)) & 0xFF);
}

void addMethods(JSGlobalObject*, PyType*, std::initializer_list<MethodDefinition>);
// For what is added to several classes at once, not all of which have all of it.
void addMethodsThatCPythonHas(JSGlobalObject*, PyType*, std::initializer_list<MethodDefinition>);
// The name that a __getattribute__ has been given. If it is no string, TypeError has been raised.
std::optional<Identifier> attributeName(JSGlobalObject*, ThrowScope&, JSValue);
void initializeAnnotations(JSGlobalObject*);
void initializeGenericAliasAndUnion(JSGlobalObject*);
void initializeTypeParameters(JSGlobalObject*);
void initializeTemplateStrings(JSGlobalObject*);
void initializeProperty(JSGlobalObject*);
void initializeReduce(JSGlobalObject*);
void initializeStructSequences(JSGlobalObject*);
// PySys_Audit(): tells the hooks of sys.addaudithook(), if there are any, that something is about to be done. False if one of them raised, and then it is not done.
template<typename... Arguments>
inline bool audit(JSGlobalObject* globalObject, ASCIILiteral event, Arguments... arguments)
{
    if (!globalObject->pyRealm()->auditHooks()) [[likely]]
        return true;
    MarkedArgumentBuffer buffer;
    (buffer.append(arguments), ...);
    return auditSlow(globalObject, event, buffer);
}

// What compile(), eval(), exec() and symtable() are given to go by, ready to be parsed. `function` is which of them, for what is said of what will not do. It may raise.
SourceCode sourceOfArgument(JSGlobalObject*, ThrowScope&, JSValue source, const String& filename, ASCIILiteral function);

// Code objects: PythonCode.cpp
FunctionExecutable* executableOfCode(JSValue code);
const FunctionInfo& infoOfExecutable(FunctionExecutable*);
bool isCode(JSGlobalObject*, JSValue);
JSValue newCodeFromParts(JSGlobalObject*, const ArgList&); // What code() makes of its eighteen arguments, with nobody told of it: _PyCode_New().
Vector<Identifier> sortedFreeVariables(const FunctionInfo&); // In the order of co_freevars.
void initializeCodeType(JSGlobalObject*);

void forEachBranch(UnlinkedCodeBlock*, const ScopedLambda<void(unsigned offset, unsigned notTaken, unsigned taken)>&);
void addMonitoring(JSGlobalObject*, JSObject* sys); // sys.monitoring, sys.settrace() and sys.setprofile(): PythonMonitoring.cpp
JSObject* createSysModule(JSGlobalObject*);

// While something is being written out, so that a list that contains itself comes out as [[...]]: Py_ReprEnter() and Py_ReprLeave().
class ReprGuard {
    WTF_MAKE_NONCOPYABLE(ReprGuard);
public:
    ReprGuard(JSGlobalObject* globalObject, JSCell* cell)
        : m_stack(globalObject->pyRealm()->objectsBeingWrittenOut())
        , m_isRecursive(m_stack.contains(cell))
    {
        if (!m_isRecursive)
            m_stack.append(cell);
    }
    ~ReprGuard()
    {
        if (!m_isRecursive)
            m_stack.removeLast();
    }
    bool isRecursive() const { return m_isRecursive; }

private:
    Vector<JSCell*, 16>& m_stack;
    bool m_isRecursive;
};

// A struct sequence: a tuple whose items have names, and which can have more that have names and are not among its items. sys.float_info is one.
// This makes one of a class, which is a tuple-like built-in class that has no attributes yet. A field with no name is one that can only be got at by where it is.
void makeStructSequenceType(JSGlobalObject*, PyType*, std::span<const ASCIILiteral> fields, unsigned countInSequence);
// As many values as the class has fields.
JSValue newStructSequence(JSGlobalObject*, PyType*, const ArgList& values);
// types.SimpleNamespace(), with nothing in it
JSObject* newSimpleNamespace(JSGlobalObject*);
void initializeDictViews(JSGlobalObject*);
void addIteratorProtocol(JSGlobalObject*, PyType*); // __length_hint__(), __reduce__() and __setstate__(), those of them that it has in CPython
JSValue dirOf(JSGlobalObject*, JSValue); // dir(object)
JSValue getBuiltin(JSGlobalObject*, ASCIILiteral name); // _PyEval_GetBuiltin(): what the code that is running would find by the name, if its globals had nothing by it. AttributeError if there is none.
// list.sort(key=keyFunction, reverse=reverse). False if it raised.
bool sortList(JSGlobalObject*, JSArray*, JSValue keyFunction, bool reverse);
// PyList_Sort(), of what is not yet a list. False if it raised.
bool sortValues(JSGlobalObject*, MarkedArgumentBuffer& values, MarkedArgumentBuffer& sorted);
JSC_DECLARE_HOST_FUNCTION(typeOr); // __or__ and __ror__ of what there can be a union of
JSC_DECLARE_HOST_FUNCTION(javaScriptClassNew); // __new__ of a class of JavaScript's, which has JavaScript construct it
void addGetSet(JSGlobalObject*, PyType*, ASCIILiteral name, PyGetSetDescriptor::Getter, PyGetSetDescriptor::Setter = nullptr);
void addMember(JSGlobalObject*, PyType*, ASCIILiteral name, PyGetSetDescriptor::Getter, PyGetSetDescriptor::Setter = nullptr);
PyNativeFunction* addFunction(JSGlobalObject*, JSObject* module, ASCIILiteral name, NativeFunction, unsigned data = 0, ASCIILiteral signature = { }, PyNativeFunction::Arguments = PyNativeFunction::Arguments::AreChecked);

// Whether a function written in C++ has been given arguments that it can work on. The first is, for a method, an instance of the class that it is a
// method of, and for __new__, a class derived from that one. The rest are as its signature has them. If not, TypeError has been raised. What follows
// can then take the first for what it is, and those that are required for being there.
bool checkArguments(JSGlobalObject*, CallFrame*);

#define PYTHON_NATIVE_WITH_LINKAGE(linkage, name) \
    static EncodedJSValue name##Checked(JSGlobalObject*, CallFrame*); \
    linkage JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        if (!checkArguments(globalObject, callFrame)) [[unlikely]] \
            return { }; \
        return name##Checked(globalObject, callFrame); \
    } \
    static EncodedJSValue name##Checked(JSGlobalObject* globalObject, CallFrame* callFrame)

#define PYTHON_NATIVE(name) PYTHON_NATIVE_WITH_LINKAGE(static, name)
// One that other files use.
#define PYTHON_SHARED_NATIVE(name) PYTHON_NATIVE_WITH_LINKAGE(, name)

#define NATIVE_PROLOGUE() \
    VM& vm = globalObject->vm(); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    [[maybe_unused]] PyRealm* realm = globalObject->pyRealm(); \
    [[maybe_unused]] auto& names = vm.pythonNames(); \
    NativeArguments args(callFrame);

#define RETURN_NONE() return JSValue::encode(jsUndefined())
#define RETURN_NOT_IMPLEMENTED() return JSValue::encode(realm->notImplemented())

// Each sets up some of the built-in types.
void initializeObjectAndType(JSGlobalObject*);
void initializeNumberTypes(JSGlobalObject*);
void initializeComplexType(JSGlobalObject*);
void initializeFunctionTypes(JSGlobalObject*);
void initializeCodeTypes(JSGlobalObject*, JSObject* builtinsNamespace);
void initializeAsyncTypes(JSGlobalObject*, JSObject* builtinsNamespace);
void initializeExceptionGroups(JSGlobalObject*, JSObject* builtinsNamespace);
void initializeContextVarTypes(JSGlobalObject*);
JSObject* createContextVarsModule(JSGlobalObject*);
// A class that is built in and is made when the module that it is in is first imported, rather than with the realm. It is for whoever asks for it to keep. What is in it is still to be put there.
PyType* createBuiltinType(JSGlobalObject*, ASCIILiteral name, PyType* base, PyType::Layout, unsigned flags);
void addClassGetItemIfGeneric(JSGlobalObject*, PyType*); // C[int], if CPython's has that
JSObject* createThreadModule(JSGlobalObject*);
JSObject* createMarshalModule(JSGlobalObject*);
JSObject* createErrnoModule(JSGlobalObject*);
JSObject* createAtExitModule(JSGlobalObject*);
JSObject* createCollectionsModule(JSGlobalObject*);
JSObject* createImpModule(JSGlobalObject*);
JSObject* createOpcodeModule(JSGlobalObject*);
JSObject* createRandomModule(JSGlobalObject*);
JSObject* createSREModule(JSGlobalObject*);
JSObject* createABCModule(JSGlobalObject*);
JSObject* createOperatorModule(JSGlobalObject*);
JSObject* createFunctoolsModule(JSGlobalObject*);
JSObject* createGCModule(JSGlobalObject*);
JSObject* createMD5Module(JSGlobalObject*);
JSObject* createSHA1Module(JSGlobalObject*);
JSObject* createSHA2Module(JSGlobalObject*);
JSObject* createSHA3Module(JSGlobalObject*);
JSObject* createBlake2Module(JSGlobalObject*);
JSObject* createLsprofModule(JSGlobalObject*);
JSObject* createHeapqModule(JSGlobalObject*);
JSObject* createBisectModule(JSGlobalObject*);
JSObject* createCMathModule(JSGlobalObject*);
JSObject* createSymtableModule(JSGlobalObject*);
JSObject* createCSVModule(JSGlobalObject*);
void updateSysFromConfiguration(JSGlobalObject*, JSObject* sysModule);
// The part of that which is sys.flags: config_set_sys_flag()
void updateSysFlagsFromConfiguration(JSGlobalObject*, JSObject* sysModule);
JSC_DECLARE_HOST_FUNCTION(builtinInput);
JSC_DECLARE_HOST_FUNCTION(sysBaseREPL);
JSObject* createArrayModule(JSGlobalObject*);
JSObject* createBinasciiModule(JSGlobalObject*);
JSObject* createStringModule(JSGlobalObject*);
JSObject* createStructModule(JSGlobalObject*);
JSObject* createUnicodeDataModule(JSGlobalObject*);
JSObject* createMultibyteCodecModule(JSGlobalObject*);
JSObject* createCodecsCNModule(JSGlobalObject*);
JSObject* createCodecsHKModule(JSGlobalObject*);
JSObject* createCodecsISO2022Module(JSGlobalObject*);
JSObject* createCodecsJPModule(JSGlobalObject*);
JSObject* createCodecsKRModule(JSGlobalObject*);
JSObject* createCodecsTWModule(JSGlobalObject*);
JSObject* createTokenizeModule(JSGlobalObject*);
JSValue getObjectState(JSGlobalObject*, JSValue); // _PyObject_GetState()
// {Py_tp_getattro, PyObject_GenericGetAttr}: the class has a __getattribute__ in its own name, which does what object's does.
// PyCapsule_New(). What is pointed at is to last as long as the realm.
JSValue newCapsule(JSGlobalObject*, ASCIILiteral name, const void* pointer = nullptr);
// PyCapsule_GetPointer(), if PyCapsule_IsValid(): null unless it is a capsule of that name.
const void* capsulePointer(JSValue, ASCIILiteral name);
void addGenericGetAttribute(JSGlobalObject*, PyType*);
// The same for PyObject_GenericSetAttr in tp_setattro: a __setattr__ and a __delattr__ of its own.
void addGenericSetAttribute(JSGlobalObject*, PyType*);
JSObject* createItertoolsModule(JSGlobalObject*);
JSObject* createMathModule(JSGlobalObject*);
JSObject* createTimeModule(JSGlobalObject*);
JSObject* createCodecsModule(JSGlobalObject*);
JSObject* createIOModule(JSGlobalObject*);
JSValue marshalDumps(JSGlobalObject*, JSValue); // PyMarshal_WriteObjectToString()
JSValue marshalLoads(JSGlobalObject*, std::span<const uint8_t>); // PyMarshal_ReadObjectFromString()
void initializeWeakReferenceTypes(JSGlobalObject*);
JSObject* createWeakrefModule(JSGlobalObject*);
JSValue newWeakReference(JSGlobalObject*, JSValue object, JSValue callback = JSValue()); // PyWeakref_NewRef()
void initializeWarnings(JSGlobalObject*);
JSObject* createWarningsModule(JSGlobalObject*);
JSObject* createASTModule(JSGlobalObject*);
void initializeTracebackTypes(JSGlobalObject*);
JSObject* createFrameModule(JSGlobalObject*);
JSObject* createJavaScriptModule(JSGlobalObject*);
JSObject* createAsyncioModule(JSGlobalObject*);
void executeAsyncioModule(JSGlobalObject*, JSObject* module);

void initializeRangeType(JSGlobalObject*);
// What the __dict__ and __weakref__ of a class whose instances have such things get and set.
// A dict in any case. That of an object of JavaScript's is how it is now.
JSValue getInstanceDict(JSGlobalObject*, JSValue self);
JSValue getInstanceDictOrProxy(JSGlobalObject*, JSValue self);
void setInstanceDict(JSGlobalObject*, JSValue self, JSValue value);
JSValue getWeakReferences(JSGlobalObject*, JSValue self);
JSC_DECLARE_HOST_FUNCTION(operatorCompareDigest); // _operator._compare_digest(), and _hashlib.compare_digest()
JSC_DECLARE_HOST_FUNCTION(sliceIndices);

inline JSFunction* asFunction(JSValue value) { return uncheckedDowncast<JSFunction>(value.asCell()); }

// ---- Looking into what is running

// Null if it is not a frame of Python code.
const FunctionInfo* pythonInfoOfFrame(CallFrame*);
// Whether Python counts it as a frame: it is running what somebody wrote in Python.
bool isFrameToPython(CallFrame*, BytecodeIndex);
// The environment that has a variable of the name, going outward from the scope, and where in it. Null if there is none.
JSLexicalEnvironment* findVariable(JSScope*, UniquedStringImpl* name, ScopeOffset&);
JSObject* globalsOfScope(VM&, JSScope*);
// The globals of the code in Python that is running: of whatever called the function written in C++ that asks. Null if there is none.
JSObject* globalsOfCaller(JSGlobalObject*);
JSObject* builtinsOfScope(VM&, JSScope*);
// What is only known about code once it has been compiled, which it is now if it had not been.
void ensureCodeDetails(VM&, FunctionExecutable*);
// Of code that has been compiled: CodeBlock::registersSeenFromOutside(), and where in the code they begin to be.
FixedVector<VirtualRegister> registersThatFrameObjectSees(FunctionExecutable*, unsigned& fromOffset, unsigned& valueProfilesBefore);
// The bytecode, which is generated again if it has been thrown away. It comes out the same.
UnlinkedCodeBlock* unlinkedCodeBlockOf(VM&, FunctionExecutable*);
unsigned offsetWhereSourceBegins(const CodeDetails&, UnlinkedCodeBlock*);
JSValue nameObjectOfFunction(JSGlobalObject*, JSFunction*, bool qualified); // f.__name__ or f.__qualname__
// function.__code__. There is one for each piece of code.
JSObject* codeObjectFor(JSGlobalObject*, FunctionExecutable*);
// One of the co_consts of the code that a function is compiled to. See PyCodeConstant.h.
JSValue constantOfCode(JSGlobalObject*, FunctionExecutable*, unsigned index);
void addFrameFunctions(JSGlobalObject*, JSObject* sysNamespace);
void initializeStrType(JSGlobalObject*);
void initializeContainerTypes(JSGlobalObject*);
void initializeIteratorTypes(JSGlobalObject*);
void initializeExceptionTypes(JSGlobalObject*);
void initializeBuiltinFunctions(JSGlobalObject*, JSObject* namespaceObject);
// Last of all, since it runs Python: what is written in it.
void initializeLibrary(JSGlobalObject*);
JSObject* createJavaScriptFunctions(VM&, JSGlobalObject*);
void initializeJavaScriptTypes(JSGlobalObject*);

// What is in other files, and shared among these.
String builtinRepr(JSGlobalObject*, JSValue);
JSValue builtinFormat(JSGlobalObject*, JSValue, const String& specification);
String strOfException(JSGlobalObject*, JSValue);
String qualifiedNameOfType(JSGlobalObject*, PyType*); // module.__qualname__, or without the module if that is builtins.
String qualifiedNameWithoutModule(JSGlobalObject*, PyType*); // type.__qualname__
String nameOfFunction(JSGlobalObject*, JSFunction*, bool qualified);
std::optional<bool> builtinContains(JSGlobalObject*, JSValue container, JSValue);
int64_t builtinLength(JSGlobalObject*, JSValue); // -1 if it has none.

// Special methods that do the same for every built-in type that has them: they look at what kind of cell they are given.
JSC_DECLARE_HOST_FUNCTION(nativeRepr);
JSC_DECLARE_HOST_FUNCTION(nativeHash);
JSC_DECLARE_HOST_FUNCTION(nativeLen);
JSC_DECLARE_HOST_FUNCTION(nativeGetItem);
JSC_DECLARE_HOST_FUNCTION(nativeSetItem);
JSC_DECLARE_HOST_FUNCTION(nativeDelItem);
JSC_DECLARE_HOST_FUNCTION(nativeContains);
JSC_DECLARE_HOST_FUNCTION(nativeIter);
JSC_DECLARE_HOST_FUNCTION(nativeNext);
JSC_DECLARE_HOST_FUNCTION(nativeSelf);
inline bool isEquality(ComparisonOperator op) { return op == ComparisonOperator::Eq || op == ComparisonOperator::NotEq; }
void addComparisons(JSGlobalObject*, PyType*);
// The same, with a function of the class's own, which finds which comparison it is with unpack<ComparisonOperator>(callFrame, 0).
void addComparisons(JSGlobalObject*, PyType*, NativeFunction);
// __add__ and __radd__ and so on for these operators, by builtinBinaryOperation().
void addBinaryOperators(JSGlobalObject*, PyType*, std::initializer_list<BinaryOperator>, bool reflected, bool inPlace);

// An instance of `type`, which is `builtin` or derived from it, holding a value that is not a cell.
JSValue boxIfDerived(JSGlobalObject*, PyType* type, PyType* builtin, JSValue);

} } // namespace JSC::Python
