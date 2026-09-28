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
#include "PythonBuiltins.h"

#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "JSGenerator.h"
#include "JSLexicalEnvironment.h"
#include "PyFrame.h"
#include "PythonBytes.h"
#include "PythonCompiler.h"
#include "PythonGenerators.h"
#include "SourceProvider.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

// Code objects, cells and frames: what a program sees when it looks into itself. And compile(), exec(), eval() and locals().

namespace JSC { namespace Python {

// ---- Code objects

// A code object is a PyNativeObject whose first field is the FunctionExecutable.
static FunctionExecutable* executableOf(JSValue code)
{
    return uncheckedDowncast<FunctionExecutable>(uncheckedDowncast<PyNativeObject>(code.asCell())->field(0).asCell());
}

static const FunctionInfo& infoOf(FunctionExecutable* executable)
{
    return *executable->unlinkedExecutable()->pythonInfo();
}

static bool isCode(JSGlobalObject* globalObject, JSValue value)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeCode();
}

JSObject* codeObjectFor(JSGlobalObject* globalObject, FunctionExecutable* executable)
{
    // Of the two functions that a generator is made of, the code is the one that can be called.
    if (FunctionExecutable* generatorFunction = executable->pythonGeneratorFunction())
        executable = generatorFunction;
    if (JSObject* code = executable->pythonCodeObject())
        return code;
    JSObject* code = PyNativeObject::create(globalObject, BuiltinType::Code, executable);
    executable->setPythonCodeObject(globalObject->vm(), code);
    return code;
}

static UnlinkedFunctionCodeBlock* unlinkedCodeBlockOf(VM& vm, UnlinkedFunctionExecutable* unlinked, const SourceCode& source)
{
    ParserError error;
    return unlinked->unlinkedCodeBlockFor(vm, source, CodeSpecializationKind::CodeForCall, { }, error, unlinked->parseMode());
}

UnlinkedCodeBlock* unlinkedCodeBlockOf(VM& vm, FunctionExecutable* executable)
{
    return unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source());
}

void ensureCodeDetails(VM& vm, FunctionExecutable* executable)
{
    if (!infoOf(executable).details)
        unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source());
    RELEASE_ASSERT(infoOf(executable).details);
}

// What is known once it has been compiled, which it is now if it had not been.
static const CodeDetails& detailsOf(VM& vm, FunctionExecutable* executable)
{
    const FunctionInfo& info = infoOf(executable);
    if (info.isGenerator || info.isCoroutine) {
        // What was written is in the function that resumes it, which is the one function that this one makes.
        UnlinkedFunctionExecutable* body = unlinkedCodeBlockOf(vm, executable->unlinkedExecutable(), executable->source())->functionExpr(0);
        if (!body->pythonInfo()->details)
            unlinkedCodeBlockOf(vm, body, body->linkedSourceCode(executable->source()));
        return *body->pythonInfo()->details;
    }
    ensureCodeDetails(vm, executable);
    return *info.details;
}

static JSValue tupleOfNames(JSGlobalObject* globalObject, const Vector<Identifier>& names)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i)
        tuple->initializeAt(vm, i, jsString(vm, names[i].string()));
    return tuple;
}

static Vector<Identifier> sortedFreeVariables(const FunctionInfo& info)
{
    Vector<Identifier> names = info.freeVariables;
    std::ranges::sort(names, [] (auto& a, auto& b) { return codePointCompareLessThan(a.string(), b.string()); });
    return names;
}

static bool isFunctionKind(CodeKind kind)
{
    return kind == CodeKind::Function || kind == CodeKind::Lambda || kind == CodeKind::GeneratorExpression;
}

static unsigned flagsOf(const FunctionInfo& info)
{
    unsigned flags = 0;
    if (isFunctionKind(info.kind))
        flags |= 0x1 | 0x2; // CO_OPTIMIZED | CO_NEWLOCALS
    if (info.hasVariadic)
        flags |= 0x4; // CO_VARARGS
    if (info.hasKeywordVariadic)
        flags |= 0x8; // CO_VARKEYWORDS
    if (info.isNested)
        flags |= 0x10; // CO_NESTED
    if (info.isGenerator && info.isCoroutine)
        flags |= 0x200; // CO_ASYNC_GENERATOR
    else if (info.isGenerator)
        flags |= 0x20; // CO_GENERATOR
    else if (info.isCoroutine)
        flags |= 0x80; // CO_COROUTINE
    if (info.hasDocstring && isFunctionKind(info.kind))
        flags |= 0x4000000; // CO_HAS_DOCSTRING
    if (info.isMethod)
        flags |= 0x8000000; // CO_METHOD
    return flags | info.futureFeatures;
}

PYTHON_NATIVE(codeRepr)
{
    VM& vm = globalObject->vm();
    FunctionExecutable* executable = executableOf(callFrame->argument(0));
    const FunctionInfo& info = infoOf(executable);
    return JSValue::encode(jsString(vm, makeString("<code object "_s, info.name.string(), " at 0x"_s, hex(std::bit_cast<uintptr_t>(executable), Lowercase), ", file \""_s, executable->source().provider()->sourceURL(), "\", line "_s, info.line, '>')));
}

PYTHON_NATIVE(codeEq)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    bool wantsEqual = op == ComparisonOperator::Eq;
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    if (!isEquality(op) || !isCode(globalObject, args.at(1)))
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(jsBoolean((executableOf(args[0])->unlinkedExecutable() == executableOf(args[1])->unlinkedExecutable()) == wantsEqual));
}

PYTHON_NATIVE(codeHash)
{
    return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(executableOf(callFrame->argument(0))->unlinkedExecutable()) >> 4)));
}

// ---- Cells

// A cell is a variable of an environment: a PyNativeObject with the environment and where in it the variable is.
static WriteBarrierBase<Unknown>& variableOfCell(JSValue cell)
{
    auto* object = uncheckedDowncast<PyNativeObject>(cell.asCell());
    return uncheckedDowncast<JSLexicalEnvironment>(object->field(0).asCell())->variableAt(ScopeOffset(object->field(1).asInt32()));
}

// The environment that has a variable of the name, going outward from `scope`, and where in it. Null if there is none.
JSLexicalEnvironment* findVariable(JSScope* scope, UniquedStringImpl* name, ScopeOffset& offset)
{
    for (; scope; scope = scope->next()) {
        auto* environment = dynamicDowncast<JSLexicalEnvironment>(scope);
        if (!environment)
            continue;
        SymbolTableEntry::Fast entry = environment->symbolTable()->get(name);
        if (entry.isNull())
            continue;
        offset = entry.scopeOffset();
        return environment;
    }
    return nullptr;
}

static JSValue getCellContents(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = variableOfCell(self).get();
    if (!value)
        return raiseValueError(globalObject, scope, "Cell is empty"_s);
    return value;
}

static void setCellContents(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto* object = uncheckedDowncast<PyNativeObject>(self.asCell());
    if (value)
        variableOfCell(self).set(globalObject->vm(), object->field(0).asCell(), value);
    else
        variableOfCell(self).clear();
}

PYTHON_NATIVE(cellRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue value = variableOfCell(args.at(0)).get();
    String address = makeString("0x"_s, hex(std::bit_cast<uintptr_t>(&variableOfCell(args[0])), Lowercase));
    if (!value)
        return JSValue::encode(jsString(vm, makeString("<cell at "_s, address, ": empty>"_s)));
    return JSValue::encode(jsString(vm, makeString("<cell at "_s, address, ": "_s, typeName(globalObject, value), " object at 0x"_s, hex(static_cast<uint64_t>(JSValue::encode(value)), Lowercase), '>')));
}

// By what is in them. One with nothing in it comes before one with something.
PYTHON_NATIVE(cellCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue other = args.at(1);
    if (!tryNativeObject(other) || typeOf(globalObject, other) != realm->typeCell())
        RETURN_NOT_IMPLEMENTED();
    JSValue a = variableOfCell(args[0]).get();
    JSValue b = variableOfCell(other).get();
    if (!a || !b)
        RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, jsNumber(!!a), jsNumber(!!b))));
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, a, b)));
}

// ---- Functions


static JSValue getFunctionCode(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSFunction* function = asFunction(self);
    auto& name = vm.pythonNames().private_code;
    if (JSValue code = function->getDirect(vm, name))
        return code;
    return codeObjectFor(globalObject, function->jsExecutable());
}

static JSValue getFunctionClosure(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSFunction* function = asFunction(self);
    auto names = sortedFreeVariables(infoOf(function->jsExecutable()));
    if (names.isEmpty())
        return jsUndefined();
    PyTuple* cells = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i) {
        ScopeOffset offset;
        JSLexicalEnvironment* environment = findVariable(function->scope(), names[i].impl(), offset);
        RELEASE_ASSERT(environment);
        cells->initializeAt(vm, i, PyNativeObject::create(globalObject, BuiltinType::Cell, environment, jsNumber(offset.offset())));
    }
    return cells;
}

JSObject* globalsOfScope(VM& vm, JSScope* scope)
{
    ScopeOffset offset;
    JSLexicalEnvironment* environment = findVariable(scope, vm.pythonNames().globals.impl(), offset);
    return environment ? asObject(environment->variableAt(offset).get()) : nullptr;
}

static JSValue getFunctionGlobals(JSGlobalObject* globalObject, JSValue self)
{
    return PyDict::backedBy(globalObject, globalsOfScope(globalObject->vm(), asFunction(self)->scope()));
}

JSObject* builtinsOfScope(VM& vm, JSScope* scope)
{
    ScopeOffset offset;
    JSLexicalEnvironment* environment = findVariable(scope, vm.pythonNames().builtins.impl(), offset);
    return environment ? asObject(environment->variableAt(offset).get()) : nullptr;
}

static JSValue getFunctionBuiltins(JSGlobalObject* globalObject, JSValue self)
{
    return PyDict::backedBy(globalObject, builtinsOfScope(globalObject->vm(), asFunction(self)->scope()));
}

// ---- Frames

const FunctionInfo* pythonInfoOfFrame(CallFrame* frame)
{
    if (!frame || frame->isNativeCalleeFrame())
        return nullptr;
    CodeBlock* codeBlock = frame->codeBlock();
    if (!codeBlock)
        return nullptr;
    auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable());
    return executable ? executable->unlinkedExecutable()->pythonInfo() : nullptr;
}

bool isFrameToPython(CallFrame* frame, BytecodeIndex bytecodeIndex)
{
    const FunctionInfo* info = pythonInfoOfFrame(frame);
    return info && info->visibility == ImplementationVisibility::Public && bytecodeIndex.offset() >= info->details->firstTraceableOffset;
}

CallFrame* callerOf(CallFrame* callFrame)
{
    VM& vm = callFrame->deprecatedVM();
    EntryFrame* entryFrame = vm.topEntryFrame;
    // Past whatever is written in C++, or in JavaScript, or comes with the engine, or is still giving its arguments to its parameters.
    for (CallFrame* frame = callFrame->callerFrame(entryFrame); frame; frame = frame->callerFrame(entryFrame)) {
        if (isFrameToPython(frame, frame->bytecodeIndex()))
            return frame;
    }
    return nullptr;
}

JSObject* globalsOfFrame(JSGlobalObject* globalObject, CallFrame* frame)
{
    if (!pythonInfoOfFrame(frame))
        return nullptr;
    return globalsOfScope(globalObject->vm(), uncheckedDowncast<JSFunction>(frame->jsCallee())->scope());
}

JSValue localsOfFrame(JSGlobalObject* globalObject, PyFrame* frame)
{
    VM& vm = globalObject->vm();
    if (JSValue mapping = frame->namespaceMapping(vm))
        return mapping;
    if (!isFunctionKind(frame->functionInfo().kind))
        return PyDict::backedBy(globalObject, frame->globals(vm));

    // A picture of the variables as they are now. Changing it changes nothing.
    PyDict* locals = PyDict::create(globalObject);
    for (unsigned i = 0; i < frame->variableCount(); ++i) {
        if (JSValue value = frame->variable(vm, i))
            locals->setString(globalObject, frame->variableName(i).string(), value);
    }
    if (PyDict* extra = frame->extraLocals()) {
        extra->forEach(globalObject, [&] (JSValue key, JSValue value) {
            locals->set(globalObject, key, value);
            return true;
        });
    }
    return locals;
}

JSValue localsOfFrame(JSGlobalObject* globalObject, CallFrame* frame)
{
    return frame ? localsOfFrame(globalObject, PyFrame::forCallFrame(globalObject->vm(), frame)) : JSValue();
}

PYTHON_NATIVE(builtinLocals)
{
    NATIVE_PROLOGUE();
    JSValue locals = localsOfFrame(globalObject, callerOf(callFrame));
    if (!locals)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "frame does not exist"_s));
    return JSValue::encode(locals);
}

JSObject* globalsOfCaller(JSGlobalObject* globalObject)
{
    CallFrame* top = globalObject->vm().topCallFrame;
    return top ? globalsOfFrame(globalObject, callerOf(top)) : nullptr;
}

PYTHON_NATIVE(builtinGlobals)
{
    NATIVE_PROLOGUE();
    JSObject* globals = globalsOfFrame(globalObject, callerOf(callFrame));
    if (!globals)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "frame does not exist"_s));
    return JSValue::encode(PyDict::backedBy(globalObject, globals));
}

// ---- compile(), exec() and eval()

// The source that compile(), exec() and eval() are given, which may be a str or bytes. Null if it raised.
static SourceCode sourceOf(JSGlobalObject* globalObject, ThrowScope& scope, JSValue source, const String& filename, ASCIILiteral function)
{
    // eval() does not mind what it is given being indented.
    bool skipsBlanks = function == "eval"_s;
    auto isBlank = [] (auto c) { return c == ' ' || c == '\t'; };
    if (source.isString()) {
        String text = asString(source)->value(globalObject);
        if (text.contains(static_cast<char16_t>(0))) {
            raise(globalObject, scope, BuiltinType::SyntaxError, "source code string cannot contain null bytes"_s);
            return { };
        }
        unsigned start = 0;
        while (skipsBlanks && start < text.length() && isBlank(text[start]))
            ++start;
        return makeSource(text.substring(start), SourceOrigin(), filename);
    }
    if (auto buffer = tryBufferOf(source)) {
        size_t start = 0;
        while (skipsBlanks && start < buffer->size() && isBlank((*buffer)[start]))
            ++start;
        RELEASE_AND_RETURN(scope, makeSource(globalObject, buffer->subspan(start), SourceOrigin(), filename));
    }
    raiseTypeError(globalObject, scope, makeString(function, "() arg 1 must be a string, bytes or "_s, function == "compile"_s ? "AST"_s : "code"_s, " object"_s));
    return { };
}

static unsigned futureFeaturesOfCaller(CallFrame* callFrame)
{
    const FunctionInfo* info = pythonInfoOfFrame(callerOf(callFrame));
    return info ? info->futureFeatures : 0;
}

// compile(source, filename, mode, flags=0, dont_inherit=False, optimize=-1)
PYTHON_NATIVE(builtinCompile)
{
    NATIVE_PROLOGUE();
    static constexpr ASCIILiteral parameters[] = { "source"_s, "filename"_s, "mode"_s, "flags"_s, "dont_inherit"_s, "optimize"_s };
    JSValue values[6];
    for (unsigned i = 0; i < 6; ++i) {
        values[i] = args.at(i);
        if (!values[i])
            values[i] = args.keyword(globalObject, parameters[i]);
        if (!values[i] && i < 3)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("compile() missing required argument '"_s, parameters[i], "' (pos "_s, i + 1, ')')));
    }
    if (isCode(globalObject, values[0]))
        return JSValue::encode(values[0]);
    String filename = str(globalObject, values[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!values[2].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("compile() argument 'mode' must be str, not "_s, typeName(globalObject, values[2]))));
    String mode = asString(values[2])->value(globalObject);
    CodeKind kind;
    if (mode == "exec"_s)
        kind = CodeKind::Module;
    else if (mode == "eval"_s)
        kind = CodeKind::Expression;
    else if (mode == "single"_s)
        kind = CodeKind::Interactive;
    else
        return JSValue::encode(raiseValueError(globalObject, scope, "compile() mode must be 'exec', 'eval' or 'single'"_s));
    SourceCode source = sourceOf(globalObject, scope, values[0], filename, "compile"_s);
    RETURN_IF_EXCEPTION(scope, { });
    bool inherits = !values[4] || !isTrue(globalObject, values[4]);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned futureFeatures = inherits ? futureFeaturesOfCaller(callFrame) : 0;
    FunctionExecutable* executable = compileSource(globalObject, source, kind, true, futureFeatures);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(codeObjectFor(globalObject, executable));
}

// The object whose properties are the items of a dict that is to be the globals of some code.
static JSObject* namespaceOf(JSGlobalObject* globalObject, PyDict* globals)
{
    JSObject* object = globals->ensureBacking(globalObject);
    if (!globals->getString(globalObject, "__builtins__"_s))
        globals->setString(globalObject, "__builtins__"_s, PyDict::backedBy(globalObject, globalObject->pyRealm()->builtinsModule()));
    return object;
}

// exec(source, globals=None, locals=None) and eval(the same)
PYTHON_NATIVE(builtinExecOrEval)
{
    bool isEval = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    ASCIILiteral function = isEval ? "eval"_s : "exec"_s;
    if (!args.size() || args.size() > 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, args.size() ? makeString(function, "() takes at most 3 arguments ("_s, args.size(), " given)"_s) : makeString(function, "() takes at least 1 positional argument (0 given)"_s)));
    JSValue globalsValue = args.at(1);
    JSValue localsValue = args.at(2);
    bool hasGlobals = globalsValue && !isNone(globalsValue);
    bool hasLocals = localsValue && !isNone(localsValue);

    if (hasGlobals && !isDict(globalsValue)) {
        if (!isEval)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("exec() globals must be a dict, not "_s, typeName(globalObject, globalsValue))));
        bool isMapping = typeOf(globalObject, globalsValue)->lookup(vm, names.dunder_getitem) && !isList(globalsValue) && !isTuple(globalsValue) && !globalsValue.isString();
        return JSValue::encode(raiseTypeError(globalObject, scope, isMapping ? "globals must be a real dict; try eval(expr, {}, mapping)"_s : "globals must be a dict"_s));
    }
    if (hasLocals && !typeOf(globalObject, localsValue)->lookup(vm, names.dunder_getitem))
        return JSValue::encode(raiseTypeError(globalObject, scope, isEval ? "locals must be a mapping"_str : makeString("exec() locals must be a mapping or None, not "_s, typeName(globalObject, localsValue))));

    // What is not given is the caller's.
    JSObject* globals;
    if (hasGlobals) {
        globals = namespaceOf(globalObject, asDict(globalsValue));
        if (!hasLocals)
            localsValue = globalsValue;
    } else {
        CallFrame* caller = callerOf(callFrame);
        globals = globalsOfFrame(globalObject, caller);
        if (!globals)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "globals and locals cannot be NULL"_s));
        if (!hasLocals)
            localsValue = localsOfFrame(globalObject, caller);
    }

    FunctionExecutable* executable;
    if (isCode(globalObject, args[0])) {
        executable = executableOf(args[0]);
        if (!infoOf(executable).usesNamespace || infoOf(executable).kind == CodeKind::Class)
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("code object passed to "_s, function, "() may not contain free variables"_s)));
    } else {
        SourceCode source = sourceOf(globalObject, scope, args[0], "<string>"_s, function);
        RETURN_IF_EXCEPTION(scope, { });
        executable = compileSource(globalObject, source, isEval ? CodeKind::Expression : CodeKind::Module, true, futureFeaturesOfCaller(callFrame));
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = call(globalObject, bindToGlobals(globalObject, executable, globals), localsValue);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(isEval ? result : jsUndefined());
}

// ---- Generators


// The function that is resumed, which knows what the generator is a generator of.
static JSFunction* bodyOf(JSValue generator)
{
    return uncheckedDowncast<JSFunction>(asGenerator(generator)->internalField(static_cast<unsigned>(JSGenerator::Field::Next)).get().asCell());
}

static int32_t stateOf(JSValue generator)
{
    return asGenerator(generator)->internalField(static_cast<unsigned>(JSGenerator::Field::State)).get().asInt32();
}

template<bool qualified>
static JSValue getGeneratorName(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    if (JSValue name = asGenerator(self)->getDirect(vm, qualified ? names.private_qualname : names.private_name))
        return name;
    const FunctionInfo& info = infoOf(bodyOf(self)->jsExecutable());
    return jsString(vm, qualified ? info.qualifiedName : info.name.string());
}

template<bool qualified>
static void setGeneratorName(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isString()) {
        raiseTypeError(globalObject, scope, qualified ? "__qualname__ must be set to a string object"_s : "__name__ must be set to a string object"_s);
        return;
    }
    asGenerator(self)->putDirect(vm, qualified ? vm.pythonNames().private_qualname : vm.pythonNames().private_name, value);
}

static JSValue getGeneratorCode(JSGlobalObject* globalObject, JSValue self)
{
    return codeObjectFor(globalObject, bodyOf(self)->jsExecutable());
}

static JSValue getGeneratorRunning(JSGlobalObject*, JSValue self)
{
    return jsBoolean(stateOf(self) == static_cast<int32_t>(JSGenerator::State::Executing));
}

static JSValue getGeneratorSuspended(JSGlobalObject*, JSValue self)
{
    return jsBoolean(stateOf(self) > static_cast<int32_t>(JSGenerator::State::Init));
}

static JSValue getGeneratorYieldFrom(JSGlobalObject* globalObject, JSValue self)
{
    JSValue iterator = asGenerator(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_yieldFrom);
    return iterator && stateOf(self) > 0 ? iterator : jsUndefined();
}

static JSValue getGeneratorFrame(JSGlobalObject* globalObject, JSValue self)
{
    if (stateOf(self) == static_cast<int32_t>(JSGenerator::State::Completed))
        return jsUndefined();
    return PyFrame::forGenerator(globalObject, asGenerator(self));
}

template<unsigned field>
static JSValue getNativeField(JSGlobalObject*, JSValue self)
{
    JSValue value = uncheckedDowncast<PyNativeObject>(self.asCell())->field(field);
    return value ? value : jsUndefined();
}

// ---- Setting them up

void initializeCodeTypes(JSGlobalObject* globalObject, JSObject* builtins)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    for (PyType* type : { realm->typeCode(), realm->typeCell() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    PyType* code = realm->typeCode();
    addMethods(globalObject, code, {
        { "__repr__"_s, codeRepr },
        { "__hash__"_s, codeHash },
    });
    addComparisons(globalObject, code, codeEq);
    addMember(globalObject, code, "co_name"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), infoOf(executableOf(self)).name.string()); });
    addMember(globalObject, code, "co_qualname"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), infoOf(executableOf(self)).qualifiedName); });
    addMember(globalObject, code, "co_filename"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), executableOf(self)->source().provider()->sourceURL()); });
    addMember(globalObject, code, "co_firstlineno"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(infoOf(executableOf(self)).line); });
    addMember(globalObject, code, "co_flags"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(flagsOf(infoOf(executableOf(self)))); });
    addMember(globalObject, code, "co_argcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
        const FunctionInfo& info = infoOf(executableOf(self));
        return jsNumber(info.usesNamespace ? 0 : info.positionalCount);
    });
    addMember(globalObject, code, "co_posonlyargcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(infoOf(executableOf(self)).positionalOnlyCount); });
    addMember(globalObject, code, "co_kwonlyargcount"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(infoOf(executableOf(self)).keywordOnlyCount); });
    addGetSet(globalObject, code, "co_varnames"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).variableNames); });
    addMember(globalObject, code, "co_nlocals"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsNumber(detailsOf(globalObject->vm(), executableOf(self)).variableNames.size()); });
    addMember(globalObject, code, "co_names"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).names); });
    addGetSet(globalObject, code, "co_cellvars"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, detailsOf(globalObject->vm(), executableOf(self)).cellVariables); });
    addGetSet(globalObject, code, "co_freevars"_s, [] (JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, sortedFreeVariables(infoOf(executableOf(self)))); });

    PyType* cell = realm->typeCell();
    addMethods(globalObject, cell, {
        { "__repr__"_s, cellRepr },
    });
    addComparisons(globalObject, cell, cellCompare);
    cell->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    addGetSet(globalObject, cell, "cell_contents"_s, getCellContents, setCellContents);

    PyType* function = realm->typeFunction();
    addGetSet(globalObject, function, "__code__"_s, getFunctionCode);
    addMember(globalObject, function, "__closure__"_s, getFunctionClosure);
    addMember(globalObject, function, "__globals__"_s, getFunctionGlobals);
    addMember(globalObject, function, "__builtins__"_s, getFunctionBuiltins);

    for (PyType* generator : { realm->typeGenerator(), realm->typeCoroutine(), realm->typeAsyncGenerator() }) {
        addGetSet(globalObject, generator, "__name__"_s, getGeneratorName<false>, setGeneratorName<false>);
        addGetSet(globalObject, generator, "__qualname__"_s, getGeneratorName<true>, setGeneratorName<true>);
    }
    PyType* generator = realm->typeGenerator();
    addGetSet(globalObject, generator, "gi_code"_s, getGeneratorCode);
    addGetSet(globalObject, generator, "gi_running"_s, getGeneratorRunning);
    addGetSet(globalObject, generator, "gi_suspended"_s, getGeneratorSuspended);
    addGetSet(globalObject, generator, "gi_yieldfrom"_s, getGeneratorYieldFrom);
    addGetSet(globalObject, generator, "gi_frame"_s, getGeneratorFrame);
    PyType* coroutine = realm->typeCoroutine();
    addGetSet(globalObject, coroutine, "cr_code"_s, getGeneratorCode);
    addGetSet(globalObject, coroutine, "cr_running"_s, getGeneratorRunning);
    addGetSet(globalObject, coroutine, "cr_suspended"_s, getGeneratorSuspended);
    addGetSet(globalObject, coroutine, "cr_await"_s, getGeneratorYieldFrom);
    addGetSet(globalObject, coroutine, "cr_frame"_s, getGeneratorFrame);
    addMember(globalObject, coroutine, "cr_origin"_s, [] (JSGlobalObject*, JSValue) -> JSValue { return jsUndefined(); });
    PyType* asyncGenerator = realm->typeAsyncGenerator();
    addGetSet(globalObject, asyncGenerator, "ag_code"_s, getGeneratorCode);
    addGetSet(globalObject, asyncGenerator, "ag_suspended"_s, getGeneratorSuspended);
    addGetSet(globalObject, asyncGenerator, "ag_await"_s, getGeneratorYieldFrom);
    addGetSet(globalObject, asyncGenerator, "ag_frame"_s, getGeneratorFrame);
    addMember(globalObject, asyncGenerator, "ag_running"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue value = asGenerator(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_isRunningAsync);
        return jsBoolean(value && value.asBoolean());
    });

    addFunction(globalObject, builtins, "locals"_s, builtinLocals);
    addFunction(globalObject, builtins, "globals"_s, builtinGlobals);
    addFunction(globalObject, builtins, "compile"_s, builtinCompile);
    addFunction(globalObject, builtins, "exec"_s, builtinExecOrEval, pack(false));
    addFunction(globalObject, builtins, "eval"_s, builtinExecOrEval, pack(true));
}

} } // namespace JSC::Python
