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
#include "PythonSymbolTable.h"
#include "SourceProvider.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

// Code objects, cells and frames: what a program sees when it looks into itself. And compile(), exec(), eval() and locals().

namespace JSC { namespace Python {

// ---- Cells

// A cell is a variable: a PyNativeObject with an environment and where in it the variable is. That is what f.__closure__ is made of. One that a program makes, cell(),
// is a variable of nothing, and what is in it is in the cell.
namespace CellField {
enum Field : unsigned { Environment, Offset, Contents };
}

static bool isCell(JSGlobalObject* globalObject, JSValue value)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeCell();
}

static WriteBarrierBase<Unknown>& variableOfCell(JSValue cell)
{
    auto* object = uncheckedDowncast<PyNativeObject>(cell.asCell());
    JSValue environment = object->field(CellField::Environment);
    if (!environment)
        return object->internalField(CellField::Contents);
    return uncheckedDowncast<JSLexicalEnvironment>(environment.asCell())->variableAt(ScopeOffset(object->field(CellField::Offset).asInt32()));
}

WriteBarrierBase<Unknown>* variableOfCell(JSValue cell, JSCell*& owner)
{
    JSValue environment = uncheckedDowncast<PyNativeObject>(cell.asCell())->field(CellField::Environment);
    owner = environment ? environment.asCell() : cell.asCell();
    return &variableOfCell(cell);
}

JSValue cellForClass(JSGlobalObject* globalObject, JSValue returnedByBody)
{
    auto* scope = dynamicDowncast<JSLexicalEnvironment>(returnedByBody);
    if (!scope)
        return jsUndefined();
    SymbolTableEntry::Fast entry = scope->symbolTable()->get(globalObject->vm().pythonNames().dunder_class.impl());
    if (entry.isNull())
        return jsUndefined();
    return PyNativeObject::create(globalObject, BuiltinType::Cell, scope, jsNumber(entry.scopeOffset().offset()));
}

JSValue contentsOfCell(JSValue cell)
{
    return variableOfCell(cell).get();
}

void setContentsOfCell(VM& vm, JSValue cell, JSValue value)
{
    auto* object = uncheckedDowncast<PyNativeObject>(cell.asCell());
    JSValue environment = object->field(CellField::Environment);
    if (value)
        variableOfCell(cell).set(vm, environment ? environment.asCell() : object, value);
    else
        variableOfCell(cell).clear();
}

// cell([contents])
PYTHON_NATIVE(cellNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "cell"_s))
        return { };
    if (args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("cell expected at most 1 argument, got "_s, args.size() - 1)));
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::Cell, JSValue(), JSValue(), args.size() == 2 ? args[1] : JSValue()));
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
    setContentsOfCell(globalObject->vm(), self, value);
}

PYTHON_NATIVE(cellRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue value = variableOfCell(args.at(0)).get();
    String address = concatenate("0x"_s, hex(std::bit_cast<uintptr_t>(&variableOfCell(args[0])), Lowercase));
    if (!value)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<cell at "_s, address, ": empty>"_s))));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<cell at "_s, address, ": "_s, typeName(globalObject, value), " object at 0x"_s, hex(static_cast<uint64_t>(JSValue::encode(value)), Lowercase), '>'))));
}

// By what is in them. One with nothing in it comes before one with something.
PYTHON_NATIVE(cellCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue other = args.at(1);
    if (!isCell(globalObject, other))
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
    if (!audit(globalObject, "object.__getattr__"_s, function, jsNontrivialString(vm, "__code__"_s)))
        return { };
    auto& name = vm.pythonNames().private_code;
    if (JSValue code = function->getDirect(vm, name))
        return code;
    return codeObjectFor(globalObject, function->jsExecutable());
}

static JSValue getFunctionClosure(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSFunction* function = asFunction(self);
    const FunctionInfo& info = infoOfExecutable(function->jsExecutable());
    auto names = sortedFreeVariables(info);
    if (names.isEmpty())
        return jsUndefined();
    PyTuple* cells = PyTuple::create(globalObject, names.size());
    for (unsigned i = 0; i < names.size(); ++i) {
        ScopeOffset offset;
        JSLexicalEnvironment* environment = findVariable(function->scope(), names[i].impl(), offset);
        RELEASE_ASSERT(environment);
        // One that it was given is the one that it has.
        if (info.variablesGivenAsCells.contains(names[i]))
            cells->initializeAt(vm, i, environment->variableAt(offset).get());
        else
            cells->initializeAt(vm, i, PyNativeObject::create(globalObject, BuiltinType::Cell, environment, jsNumber(offset.offset())));
    }
    return cells;
}

// An executable for the code of a code object, for a function that has its free variables as cells.
static FunctionExecutable* executableTakingCells(JSGlobalObject* globalObject, JSValue code)
{
    FunctionExecutable* original = executableOfCode(code);
    auto info = infoOfExecutable(original).copy();
    info->variablesGivenAsCells = info->freeVariables;
    FunctionExecutable* executable = cloneExecutable(globalObject, original, WTF::move(info));
    // It is what the function has for its __code__, and a frame of it for its f_code.
    executable->setPythonCodeObject(globalObject->vm(), asObject(code));
    return executable;
}

JSObject* namespaceOf(JSGlobalObject*, PyDict* globals);

// function(code, globals, name=None, argdefs=None, closure=None, kwdefaults=None)
PYTHON_NATIVE(functionNew)
{
    NATIVE_PROLOGUE();
    JSValue code = args.at(1);
    JSValue globals = args.at(2);
    auto orNone = [] (JSValue value) { return value ? value : jsUndefined(); };
    JSValue name = orNone(args.at(3));
    JSValue defaults = orNone(args.at(4));
    JSValue closure = orNone(args.at(5));
    JSValue keywordDefaults = orNone(args.at(6));
    auto describe = [&] (JSValue value) { return typeNameOfArgument(globalObject, value); };
    if (!isCode(globalObject, code))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("function() argument 'code' must be code, not "_s, describe(code))));
    if (!isDict(globals))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("function() argument 'globals' must be dict, not "_s, describe(globals))));
    if (!isNone(name) && !stringIn(name))
        return JSValue::encode(raiseTypeError(globalObject, scope, "arg 3 (name) must be None or string"_s));
    if (!isNone(defaults) && !isTuple(defaults))
        return JSValue::encode(raiseTypeError(globalObject, scope, "arg 4 (defaults) must be None or tuple"_s));
    const FunctionInfo& info = infoOfExecutable(executableOfCode(code));
    auto freeVariables = sortedFreeVariables(info);
    if (!isTuple(closure)) {
        if (freeVariables.size() && isNone(closure))
            return JSValue::encode(raiseTypeError(globalObject, scope, "arg 5 (closure) must be tuple"_s));
        if (!isNone(closure))
            return JSValue::encode(raiseTypeError(globalObject, scope, "arg 5 (closure) must be None or tuple"_s));
    }
    if (!isNone(keywordDefaults) && !isDict(keywordDefaults))
        return JSValue::encode(raiseTypeError(globalObject, scope, "arg 6 (kwdefaults) must be None or dict"_s));
    unsigned given = isNone(closure) ? 0 : asTuple(closure)->length();
    if (given != freeVariables.size())
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate(info.name.string(), " requires closure of length "_s, freeVariables.size(), ", not "_s, given)));
    for (unsigned i = 0; i < given; ++i) {
        if (!isCell(globalObject, asTuple(closure)->at(i)))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("arg 5 (closure) expected cell, found "_s, typeName(globalObject, asTuple(closure)->at(i)))));
    }
    if (!audit(globalObject, "function.__new__"_s, code))
        return { };

    JSScope* environment = environmentForGlobals(globalObject, namespaceOf(globalObject, asDict(globals)));
    if (given)
        environment = environmentForCells(globalObject, environment, freeVariables, asTuple(closure));
    JSFunction* function = JSFunction::create(vm, globalObject, executableTakingCells(globalObject, code), environment);
    if (!isNone(name)) {
        setAttribute(globalObject, function, vm.pythonNames().dunder_name, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isNone(defaults))
        function->putDirect(vm, vm.pythonNames().private_defaults, defaults);
    if (!isNone(keywordDefaults))
        function->putDirect(vm, vm.pythonNames().private_kwdefaults, keywordDefaults);
    return JSValue::encode(function);
}

// f.__code__ = code. It goes on with the variables that it has, as the free variables of the new code, in order.
static void setFunctionCode(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSFunction* function = asFunction(self);
    if (!value || !isCode(globalObject, value)) {
        raiseTypeError(globalObject, scope, "__code__ must be set to a code object"_s);
        return;
    }
    if (!audit(globalObject, "object.__setattr__"_s, function, jsNontrivialString(vm, "__code__"_s), value))
        return;
    JSValue closure = getFunctionClosure(globalObject, function);
    unsigned has = isNone(closure) ? 0 : asTuple(closure)->length();
    auto freeVariables = sortedFreeVariables(infoOfExecutable(executableOfCode(value)));
    if (has != freeVariables.size()) {
        raiseValueError(globalObject, scope, concatenate(nameOfFunction(globalObject, function, false), "() requires a code object with "_s, has, " free vars, not "_s, freeVariables.size()));
        return;
    }
    auto kindOf = [] (const FunctionInfo& info) { return std::tuple { info.isGenerator, info.isCoroutine }; };
    if (kindOf(infoOfExecutable(function->jsExecutable())) != kindOf(infoOfExecutable(executableOfCode(value)))) {
        if (!warn(globalObject, BuiltinType::DeprecationWarning, "Assigning a code object of non-matching type is deprecated (e.g., from a generator to a plain function)"_s))
            return;
    }

    // What it is called, and what it says of itself, are its own, and until now were what the code says.
    JSValue doc = getAttribute(globalObject, function, names.dunder_doc);
    RETURN_IF_EXCEPTION(scope, void());
    function->putDirect(vm, names.private_doc, doc);
    function->putDirect(vm, names.private_qualname, jsString(vm, nameOfFunction(globalObject, function, true)));
    setAttribute(globalObject, function, names.dunder_name, jsString(vm, nameOfFunction(globalObject, function, false)));
    RETURN_IF_EXCEPTION(scope, void());

    ScopeOffset offset;
    JSScope* environment = findVariable(function->scope(), names.globals.impl(), offset);
    if (has)
        environment = environmentForCells(globalObject, environment, freeVariables, asTuple(closure));
    function->replaceExecutable(vm, executableTakingCells(globalObject, value));
    function->setScope(vm, environment);
    // How the defaults line up with the parameters is worked out again.
    JSCell::deleteProperty(function, globalObject, names.private_alignedDefaults);
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
    // Each time that the engine is come into from C++ there is a frame that says where the frames from before are, and to get past it is to know which it is. So it is come to from the top, which
    // is where the frame nearly always is.
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* frame = vm.topCallFrame; frame && frame != callFrame;) {
        frame = frame->callerFrame(entryFrame);
        // It is above what the VM has as the top, as when something is on its way out.
        if (!frame)
            entryFrame = vm.topEntryFrame;
    }
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
    VM& vm = globalObject->vm();
    // eval() does not mind what it is given being indented.
    bool skipsBlanks = function == "eval"_s;
    auto isBlank = [] (auto c) { return c == ' ' || c == '\t'; };
    if (JSString* string = stringIn(source)) {
        String text = string->value(globalObject);
        if (text.contains(static_cast<char16_t>(0))) {
            raise(globalObject, scope, BuiltinType::SyntaxError, "source code string cannot contain null bytes"_s);
            return { };
        }
        unsigned start = 0;
        while (skipsBlanks && start < text.length() && isBlank(text[start]))
            ++start;
        if (globalObject->pyRealm()->auditHooks()) {
            auto encoded = encodeString(globalObject, jsString(vm, text.substring(start)), "utf-8"_s, "surrogatepass"_s);
            RETURN_IF_EXCEPTION(scope, { });
            if (!audit(globalObject, "compile"_s, newBytes(globalObject, encoded->span()), jsString(vm, filename)))
                return { };
        }
        return makeSource(text.substring(start), SourceOrigin(), filename);
    }
    if (auto buffer = bufferOrNothing(globalObject, source)) {
        size_t start = 0;
        while (skipsBlanks && start < buffer->size() && isBlank((*buffer)[start]))
            ++start;
        // What a hook does could change what is in it.
        ByteVector bytes;
        bytes.append(buffer->subspan(start));
        JSValue copy = newBytes(globalObject, bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (!audit(globalObject, "compile"_s, copy, jsString(vm, filename)))
            return { };
        RELEASE_AND_RETURN(scope, makeSource(globalObject, bytes.span(), SourceOrigin(), filename));
    }
    raiseTypeError(globalObject, scope, concatenate(function, "() arg 1 must be a string, bytes or "_s, function == "compile"_s ? "AST"_s : "code"_s, " object"_s));
    return { };
}

static unsigned futureFeaturesOfCaller(CallFrame* callFrame)
{
    const FunctionInfo* info = pythonInfoOfFrame(callerOf(callFrame));
    return info ? info->futureFeatures : 0;
}

// compile(source, filename, mode, flags=0, dont_inherit=False, optimize=-1, *, _feature_version=-1)
PYTHON_NATIVE(builtinCompile)
{
    NATIVE_PROLOGUE();
    JSValue given = args.at(0);
    // PyUnicode_FSDecoder()
    auto path = toFileSystemPath(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    String filename = String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(path->span()));
    JSValue modeValue = args.at(2);
    if (!stringIn(modeValue))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("compile() argument 'mode' must be str, not "_s, typeNameOfArgument(globalObject, modeValue))));
    String mode = stringIn(modeValue)->value(globalObject);
    auto number = [&] (unsigned index, int otherwise) -> int {
        JSValue value = args.at(index);
        if (!value)
            return otherwise;
        auto result = toCInt(globalObject, value);
        return result ? *result : 0;
    };
    int flags = number(3, 0);
    RETURN_IF_EXCEPTION(scope, { });
    bool inherits = !args.at(4) || !isTrue(globalObject, args.at(4));
    RETURN_IF_EXCEPTION(scope, { });
    int optimize = number(5, -1);
    RETURN_IF_EXCEPTION(scope, { });
    number(6, -1);
    RETURN_IF_EXCEPTION(scope, { });

    static constexpr int nested = 0x10; // PyCF_MASK_OBSOLETE
    static constexpr int doNotImplyDedent = 0x200;
    static constexpr int onlyAST = 0x400;
    static constexpr int typeComments = 0x1000;
    static constexpr int allowIncompleteInput = 0x4000;
    static constexpr int optimizedAST = 0x8000 | onlyAST;
    if (flags & ~(static_cast<int>(FutureFeaturesMask) | nested | doNotImplyDedent | onlyAST | typeComments | static_cast<int>(AllowTopLevelAwait) | allowIncompleteInput | optimizedAST))
        return JSValue::encode(raiseValueError(globalObject, scope, "compile(): unrecognised flags"_s));
    if (optimize < -1 || optimize > 2)
        return JSValue::encode(raiseValueError(globalObject, scope, "compile(): invalid optimize value"_s));
    CodeKind kind = CodeKind::Module;
    Module::Kind moduleKind = Module::Kind::Module;
    if (mode == "eval"_s) {
        kind = CodeKind::Expression;
        moduleKind = Module::Kind::Expression;
    } else if (mode == "single"_s) {
        kind = CodeKind::Interactive;
        moduleKind = Module::Kind::Interactive;
    } else if (mode == "func_type"_s) {
        if (!(flags & onlyAST))
            return JSValue::encode(raiseValueError(globalObject, scope, "compile() mode 'func_type' requires flag PyCF_ONLY_AST"_s));
        moduleKind = Module::Kind::FunctionType;
    } else if (mode != "exec"_s)
        return JSValue::encode(raiseValueError(globalObject, scope, flags & onlyAST ? "compile() mode must be 'exec', 'eval', 'single' or 'func_type'"_s : "compile() mode must be 'exec', 'eval' or 'single'"_s));

    unsigned futureFeatures = flags & (FutureFeaturesMask | AllowTopLevelAwait | DoNotImplyDedent | AllowIncompleteInput | TypeComments);
    if (inherits)
        futureFeatures |= futureFeaturesOfCaller(callFrame) & FutureFeaturesMask;

    bool isTree = isAST(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    // What the interpreter was started with is no optimization.
    TreeOptions options { futureFeatures, static_cast<unsigned>(std::max(optimize, 0)), !!(flags & onlyAST), (flags & optimizedAST) == optimizedAST };
    if (isTree)
        RELEASE_AND_RETURN(scope, JSValue::encode(compileTree(globalObject, given, filename, moduleKind, options)));

    SourceCode source = sourceOf(globalObject, scope, given, filename, "compile"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (flags & onlyAST)
        RELEASE_AND_RETURN(scope, JSValue::encode(parseSource(globalObject, source, moduleKind, options)));
    FunctionExecutable* executable = compileSource(globalObject, source, kind, true, futureFeatures, ImplementationVisibility::Public, std::max(optimize, 0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(codeObjectFor(globalObject, executable));
}

// The object whose properties are the items of a dict that is to be the globals of some code.
JSObject* namespaceOf(JSGlobalObject* globalObject, PyDict* globals)
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
    JSValue source = args.at(0);
    JSValue globalsValue = args.at(1);
    JSValue localsValue = args.at(2);
    JSValue closure = isEval ? JSValue() : args.at(3);
    bool hasGlobals = globalsValue && !isNone(globalsValue);
    bool hasLocals = localsValue && !isNone(localsValue);
    bool hasClosure = closure && !isNone(closure);
    // PyMapping_Check()
    auto isMapping = [&] (JSValue value) { return !!typeOf(globalObject, value)->lookup(vm, names.dunder_getitem) && !isList(value) && !isTuple(value) && !value.isString() && !builtinBufferOf(value); };
    auto hasItems = [&] (JSValue value) { return !!typeOf(globalObject, value)->lookup(vm, names.dunder_getitem); };

    if (isEval) {
        if (hasLocals && !hasItems(localsValue))
            return JSValue::encode(raiseTypeError(globalObject, scope, "locals must be a mapping"_s));
        if (hasGlobals && !isDict(globalsValue))
            return JSValue::encode(raiseTypeError(globalObject, scope, hasItems(globalsValue) ? "globals must be a real dict; try eval(expr, {}, mapping)"_s : "globals must be a dict"_s));
    }
    UNUSED_VARIABLE(isMapping);

    // What is not given is the caller's.
    CallFrame* caller = hasGlobals ? nullptr : callerOf(callFrame);
    JSObject* globals = nullptr;
    if (!hasGlobals) {
        globals = caller ? globalsOfFrame(globalObject, caller) : nullptr;
        if (!globals)
            return JSValue::encode(isEval ? raiseTypeError(globalObject, scope, "eval must be given globals and locals when called without a frame"_s) : raise(globalObject, scope, BuiltinType::SystemError, "globals and locals cannot be NULL"_s));
    }
    if (!hasLocals) {
        localsValue = hasGlobals ? globalsValue : localsOfFrame(globalObject, caller);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isEval) {
        if (hasGlobals && !isDict(globalsValue))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("exec() globals must be a dict, not "_s, typeName(globalObject, globalsValue))));
        if (!hasItems(localsValue))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("locals must be a mapping or None, not "_s, typeName(globalObject, localsValue))));
    }
    if (hasGlobals)
        globals = namespaceOf(globalObject, asDict(globalsValue));

    JSValue code = source;
    if (isCode(globalObject, source)) {
        unsigned freeVariableCount = infoOfExecutable(executableOfCode(source)).freeVariables.size();
        if (isEval) {
            if (!audit(globalObject, "exec"_s, source))
                return { };
            if (freeVariableCount)
                return JSValue::encode(raiseTypeError(globalObject, scope, "code object passed to eval() may not contain free variables"_s));
        } else {
            if (!freeVariableCount && hasClosure)
                return JSValue::encode(raiseTypeError(globalObject, scope, "cannot use a closure with this code object"_s));
            if (freeVariableCount) {
                bool isRight = hasClosure && isTuple(closure) && typeOf(globalObject, closure) == realm->typeTuple() && asTuple(closure)->length() == freeVariableCount;
                for (unsigned i = 0; isRight && i < freeVariableCount; ++i)
                    isRight = isCell(globalObject, asTuple(closure)->at(i));
                if (!isRight)
                    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("code object requires a closure of exactly length "_s, freeVariableCount)));
            }
            if (!audit(globalObject, "exec"_s, source))
                return { };
        }
    } else {
        if (hasClosure)
            return JSValue::encode(raiseTypeError(globalObject, scope, "closure can only be used when source is a code object"_s));
        SourceCode text = sourceOf(globalObject, scope, source, "<string>"_s, function);
        RETURN_IF_EXCEPTION(scope, { });
        FunctionExecutable* compiled = compileSource(globalObject, text, isEval ? CodeKind::Expression : CodeKind::Module, true, futureFeaturesOfCaller(callFrame) & FutureFeaturesMask);
        RETURN_IF_EXCEPTION(scope, { });
        code = codeObjectFor(globalObject, compiled);
        if (!audit(globalObject, "exec"_s, code))
            return { };
    }

    // What compile() makes is in nothing but its globals wherever it is run. Anything else was compiled to be in what it was written in, and is compiled again to be here.
    FunctionExecutable* executable = executableOfCode(code);
    const FunctionInfo& info = infoOfExecutable(executable);
    bool isWhatCompileMakes = info.kind == CodeKind::Module || info.kind == CodeKind::Expression || info.kind == CodeKind::Interactive;
    JSScope* environment = environmentForGlobals(globalObject, globals);
    if (hasClosure)
        environment = environmentForCells(globalObject, environment, sortedFreeVariables(info), asTuple(closure));
    JSFunction* toRun = JSFunction::create(vm, globalObject, isWhatCompileMakes ? executable : executableTakingCells(globalObject, code), environment);
    JSValue result = info.usesNamespace ? call(globalObject, toRun, localsValue) : call(globalObject, toRun);
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
    // One that is written in JavaScript goes by the name of its function.
    if (!isWrittenInPython(asGenerator(self)))
        return jsString(vm, bodyOf(self)->jsExecutable()->ecmaName().string());
    const FunctionInfo& info = infoOfExecutable(bodyOf(self)->jsExecutable());
    return jsString(vm, qualified ? info.qualifiedName : info.name.string());
}

// <generator object f at 0x...>
PYTHON_NATIVE(generatorRepr)
{
    NATIVE_PROLOGUE();
    String name = asString(getGeneratorName<true>(globalObject, args[0]))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', typeName(globalObject, args[0]), " object "_s, name, " at "_s, addressOf(args[0].asCell()), '>'))));
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

// There is no code object for what is written in JavaScript, nor a frame object.
static JSValue getGeneratorCode(JSGlobalObject* globalObject, JSValue self)
{
    if (!isWrittenInPython(asGenerator(self)))
        return jsUndefined();
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
    if (stateOf(self) == static_cast<int32_t>(JSGenerator::State::Completed) || !isWrittenInPython(asGenerator(self)))
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

    initializeCodeType(globalObject);

    PyType* cell = realm->typeCell();
    addMethods(globalObject, cell, {
        { "__new__"_s, cellNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__repr__"_s, cellRepr },
    });
    addComparisons(globalObject, cell, cellCompare);
    cell->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    addGetSet(globalObject, cell, "cell_contents"_s, getCellContents, setCellContents);

    PyType* function = realm->typeFunction();
    addMethods(globalObject, function, { { "__new__"_s, functionNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass } });
    addGetSet(globalObject, function, "__code__"_s, getFunctionCode, setFunctionCode);
    addMember(globalObject, function, "__closure__"_s, getFunctionClosure);
    addMember(globalObject, function, "__globals__"_s, getFunctionGlobals);
    addMember(globalObject, function, "__builtins__"_s, getFunctionBuiltins);

    for (PyType* generator : { realm->typeGenerator(), realm->typeCoroutine(), realm->typeAsyncGenerator() }) {
        addGetSet(globalObject, generator, "__name__"_s, getGeneratorName<false>, setGeneratorName<false>);
        addGetSet(globalObject, generator, "__qualname__"_s, getGeneratorName<true>, setGeneratorName<true>);
        addMethods(globalObject, generator, { { "__repr__"_s, generatorRepr } });
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
    addMember(globalObject, coroutine, "cr_origin"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue origin = asGenerator(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_origin);
        return origin ? origin : jsUndefined();
    });
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
