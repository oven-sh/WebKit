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
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "PyFrame.h"
#include "PythonGenerators.h"
#include "PythonImport.h"
#include "PythonUnicodeType.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/StringBuilder.h>

// Tracebacks, and what Python sees of frames. PyFrame.h says what a frame object is.

namespace JSC { namespace Python {

// ---- Tracebacks

// A traceback is a PyNativeObject: the next one, the frame, and where the frame had got to, in the bytecode and in the source.
// SourceOffset is the UnlinkedCodeBlock to find it in until it is asked for, which for most it never is. That is kept in any case by what the function of the frame is an instance of.
enum TracebackField : unsigned { Next, Frame, BytecodeOffset, SourceOffset };

static PyNativeObject* asNative(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }
static PyFrame* asFrame(JSValue value) { return uncheckedDowncast<PyFrame>(value.asCell()); }

static bool isTraceback(JSGlobalObject* globalObject, JSValue value)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeTraceback();
}

void addTracebackEntry(JSGlobalObject* globalObject, JSValue exception, CallFrame* callFrame, BytecodeIndex bytecodeIndex)
{
    if (!exception.isObject() || !isFrameToPython(callFrame, bytecodeIndex))
        return;
    VM& vm = globalObject->vm();
    DeferGCForAWhile deferGC(vm);
    JSValue head = asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback);
    if (!head || !isTraceback(globalObject, head))
        head = jsUndefined();
    JSValue entry = PyNativeObject::create(globalObject, BuiltinType::Traceback, head, PyFrame::forCallFrame(vm, callFrame), JSC::jsNumber(bytecodeIndex.offset()), callFrame->codeBlock()->unlinkedCodeBlock());
    asObject(exception)->putDirect(vm, vm.pythonNames().private_traceback, entry);
}

JSValue tracebackOf(VM& vm, JSValue exception)
{
    return exception.isObject() ? asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback) : JSValue();
}

Vector<StackFrame> stackOfTraceback(VM& vm, JSCell* owner, JSValue traceback)
{
    Vector<StackFrame> stack;
    for (JSValue cursor = traceback; cursor && cursor.isObject() && isTraceback(asObject(cursor)->realm(), cursor); cursor = asNative(cursor)->field(TracebackField::Next)) {
        PyNativeObject* entry = asNative(cursor);
        JSFunction* function = asFrame(entry->field(TracebackField::Frame))->function();
        if (!function)
            continue;
        // What it was compiled into may have been thrown away since, and then all that there is to say is what it was called.
        CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
        if (codeBlock)
            stack.append(StackFrame(vm, owner, function, codeBlock->baselineAlternative(), BytecodeIndex(entry->field(TracebackField::BytecodeOffset).asInt32())));
        else
            stack.append(StackFrame(vm, owner, function));
    }
    stack.reverse();
    return stack;
}

void leaveFrame(VM& vm, CallFrame* callFrame, BytecodeIndex bytecodeIndex)
{
    if (!isFrameToPython(callFrame, bytecodeIndex))
        return;
    PyFrame* frame = PyFrame::forCallFrameIfExists(vm, callFrame);
    if (!frame || frame->state() == PyFrame::State::Over)
        return;
    DeferGCForAWhile deferGC(vm);
    frame->leave(vm, callFrame, bytecodeIndex);
}

void unwindFrame(VM& vm, CallFrame* callFrame, BytecodeIndex bytecodeIndex)
{
    leaveFrame(vm, callFrame, bytecodeIndex);
    // If it got as far as being counted.
    if (bytecodeIndex.offset() >= pythonInfoOfFrame(callFrame)->details->enterOffset)
        vm.leavePythonFrame();
}

static int lineOf(JSValue traceback)
{
    PyNativeObject* entry = asNative(traceback);
    // One that a program made is where the program said.
    if (JSValue line = entry->getDirect(entry->vm(), entry->vm().pythonNames().private_line))
        return line.asInt32();
    PyFrame* frame = asFrame(entry->field(TracebackField::Frame));
    if (JSValue code = entry->field(TracebackField::SourceOffset); code.isCell()) {
        // CodeBlock::expressionInfoForBytecodeIndex()
        unsigned divot = uncheckedDowncast<UnlinkedCodeBlock>(code.asCell())->expressionInfoForBytecodeIndex(BytecodeIndex(entry->field(TracebackField::BytecodeOffset).asInt32())).divot;
        entry->setField(entry->vm(), TracebackField::SourceOffset, JSC::jsNumber(divot + frame->executable()->source().startOffset()));
    }
    return frame->executable()->source().provider()->documentLineColumnForOffset(entry->field(TracebackField::SourceOffset).asInt32()).line + frame->functionInfo().lineDelta;
}

int lineOfTracebackFor(JSGlobalObject* globalObject, JSValue exception, PyFrame* frame)
{
    VM& vm = globalObject->vm();
    JSValue traceback = getAttributeIfPresent(globalObject, exception, vm.pythonNames().dunder_traceback);
    for (JSValue cursor = traceback; cursor && !isNone(cursor); cursor = asNative(cursor)->field(TracebackField::Next)) {
        if (asNative(cursor)->field(TracebackField::Frame) == JSValue(frame))
            return lineOf(cursor);
    }
    return -1;
}

void removeImportlibFrames(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* raised = scope.exception();
    if (!raised || vm.isTerminationException(raised) || !raised->value().isObject())
        return;
    JSObject* exception = asObject(raised->value());
    // If it is an ImportError, every run of importlib's frames goes. Otherwise, those that end in a call to _call_with_frames_removed().
    bool alwaysTrims = typeOf(globalObject, exception)->isSubtypeOf(globalObject->pyRealm()->typeImportError());
    JSValue base = exception->getDirect(vm, vm.pythonNames().private_traceback);
    if (!base || !isTraceback(globalObject, base))
        return;

    // What has the link to a traceback: the one before it, or nothing if it is the exception.
    auto setLink = [&] (PyNativeObject* holder, JSValue next) {
        if (holder)
            holder->setField(vm, TracebackField::Next, next);
        else
            base = next;
    };
    bool isInImportlib = false;
    PyNativeObject* previous = nullptr;
    PyNativeObject* outer = nullptr;
    for (JSValue cursor = base; !isNone(cursor);) {
        PyNativeObject* entry = asNative(cursor);
        JSValue next = entry->field(TracebackField::Next);
        PyFrame* frame = asFrame(entry->field(TracebackField::Frame));
        const String& filename = frame->executable()->source().provider()->sourceURL();
        bool isNowInImportlib = filename == "<frozen importlib._bootstrap>"_s || filename == "<frozen importlib._bootstrap_external>"_s;
        // This is where the run begins.
        if (isNowInImportlib && !isInImportlib)
            outer = previous;
        isInImportlib = isNowInImportlib;
        if (isInImportlib && (alwaysTrims || frame->functionInfo().name.string() == "_call_with_frames_removed"_s)) {
            setLink(outer, next);
            previous = outer;
        } else
            previous = entry;
        cursor = next;
    }
    exception->putDirect(vm, vm.pythonNames().private_traceback, base);
}

template<unsigned field>
static JSValue getField(JSGlobalObject*, JSValue self)
{
    JSValue value = asNative(self)->field(field);
    return value ? value : jsUndefined();
}

// traceback(tb_next, tb_frame, tb_lasti, tb_lineno)
PYTHON_NATIVE(tracebackNew)
{
    NATIVE_PROLOGUE();
    JSValue next = args.at(1);
    JSValue frame = args.at(2);
    if (!dynamicDowncast<PyFrame>(frame))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("traceback() argument 'tb_frame' must be frame, not "_s, isNone(frame) ? "None"_s : typeName(globalObject, frame))));
    int numbers[2];
    for (unsigned i = 0; i < 2; ++i) {
        auto number = toCInt(globalObject, args.at(3 + i));
        RETURN_IF_EXCEPTION(scope, { });
        numbers[i] = *number;
    }
    if (!isNone(next) && !isTraceback(globalObject, next))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("expected traceback object or None, got '"_s, typeName(globalObject, next), '\'')));
    auto* entry = PyNativeObject::create(globalObject, BuiltinType::Traceback, next, frame, jsNumber(numbers[0]));
    // -1 is for it to be worked out, which here is where the frame is.
    entry->putDirect(vm, names.private_line, jsNumber(numbers[1] == -1 ? static_cast<int>(asFrame(frame)->line(vm)) : numbers[1]));
    return JSValue::encode(entry);
}

PYTHON_NATIVE(tracebackDir)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MarkedArgumentBuffer attributes;
    for (ASCIILiteral name : { "tb_frame"_s, "tb_next"_s, "tb_lasti"_s, "tb_lineno"_s })
        attributes.append(jsString(vm, String(name)));
    return JSValue::encode(newList(globalObject, attributes));
}

static void setTracebackNext(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete tb_next attribute"_s);
        return;
    }
    if (!isNone(value) && !isTraceback(globalObject, value)) {
        raiseTypeError(globalObject, scope, concatenate("expected traceback object, got '"_s, typeName(globalObject, value), '\''));
        return;
    }
    for (JSValue cursor = value; !isNone(cursor); cursor = asNative(cursor)->field(TracebackField::Next)) {
        if (cursor == self) {
            raiseValueError(globalObject, scope, "traceback loop detected"_s);
            return;
        }
    }
    asNative(self)->setField(vm, TracebackField::Next, value);
}

// ---- Frames

static JSValue getFrameBack(JSGlobalObject* globalObject, JSValue self)
{
    PyFrame* back = asFrame(self)->back(globalObject->vm());
    return back ? JSValue(back) : jsUndefined();
}

static JSValue getFrameCode(JSGlobalObject* globalObject, JSValue self)
{
    return codeObjectFor(globalObject, asFrame(self)->executable());
}

static JSValue getFrameGlobals(JSGlobalObject* globalObject, JSValue self)
{
    return PyDict::backedBy(globalObject, asFrame(self)->globals(globalObject->vm()));
}

static JSValue getFrameBuiltins(JSGlobalObject* globalObject, JSValue self)
{
    return PyDict::backedBy(globalObject, builtinsOfScope(globalObject->vm(), asFrame(self)->function()->scope()));
}

// The variables of a function are seen through a proxy, which reads and writes them where they are. Other code keeps its names in a mapping,
// and that is what this is.
static JSValue getFrameLocals(JSGlobalObject* globalObject, JSValue self)
{
    PyFrame* frame = asFrame(self);
    switch (frame->functionInfo().kind) {
    case CodeKind::Function:
    case CodeKind::Lambda:
    case CodeKind::GeneratorExpression:
        return call(globalObject, globalObject->pyRealm()->frameLocalsProxyType(), frame);
    default:
        return localsOfFrame(globalObject, frame);
    }
}

static JSValue getFrameLine(JSGlobalObject* globalObject, JSValue self)
{
    return intFromUInt64(globalObject, asFrame(self)->line(globalObject->vm()));
}

static void setFrameLine(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
    else if (typeOf(globalObject, value) != globalObject->pyRealm()->typeInt())
        raiseValueError(globalObject, scope, "lineno must be an integer"_s);
    else
        RELEASE_AND_RETURN(scope, asFrame(self)->goOnFromLine(globalObject, value));
}

static JSValue getFrameLastInstruction(JSGlobalObject* globalObject, JSValue self)
{
    // A generator that has not started has been made, which is the beginning of its code.
    auto index = asFrame(self)->bytecodeIndex(globalObject->vm());
    return intFromUInt64(globalObject, index ? index->offset() : 0);
}

static JSValue getFrameGenerator(JSGlobalObject*, JSValue self)
{
    JSGenerator* generator = asFrame(self)->generator();
    return generator ? JSValue(generator) : jsUndefined();
}

static JSValue getFrameTrace(JSGlobalObject*, JSValue self)
{
    JSValue trace = asFrame(self)->trace();
    return trace ? trace : jsUndefined();
}

static void setFrameTrace(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyFrame* frame = asFrame(self);
    frame->setTrace(globalObject->vm(), value && !isNone(value) ? value : JSValue());
    if (frame->trace() && frame->tracesOpcodes())
        setTracesOpcodes(globalObject, frame, true);
}

template<bool (PyFrame::*getter)() const>
static JSValue getFrameFlag(JSGlobalObject*, JSValue self)
{
    return jsBoolean((asFrame(self)->*getter)());
}

template<void (PyFrame::*setter)(bool)>
static void setFrameFlag(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !value.isBoolean()) {
        raiseTypeError(globalObject, scope, value ? "attribute value type must be bool"_s : "can't delete numeric/char attribute"_s);
        return;
    }
    PyFrame* frame = asFrame(self);
    (frame->*setter)(value.asBoolean());
    if (setter == &PyFrame::setTracesOpcodes && frame->tracesOpcodes() && frame->trace())
        setTracesOpcodes(globalObject, frame, true);
}

PYTHON_NATIVE(frameClear)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = asFrame(args[0]);
    switch (frame->state()) {
    case PyFrame::State::Running:
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot clear an executing frame"_s));
    case PyFrame::State::Suspended:
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot clear a suspended frame"_s));
    case PyFrame::State::NotStarted:
        // It never will be. It keeps what it was called with.
        generatorClose(globalObject, frame->generator());
        RETURN_IF_EXCEPTION(scope, { });
        break;
    case PyFrame::State::Over:
        frame->clear();
        break;
    }
    RETURN_NONE();
}

PYTHON_NATIVE(frameRepr)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = asFrame(args[0]);
    String filename = repr(globalObject, jsString(vm, frame->executable()->source().provider()->sourceURL()));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<frame at 0x"_s, hex(std::bit_cast<uintptr_t>(frame), Lowercase), ", file "_s, filename, ", line "_s, frame->line(vm), ", code "_s, frame->functionInfo().name.string(), '>'))));
}

PYTHON_NATIVE(sysGetFrame)
{
    NATIVE_PROLOGUE();
    int64_t depth = 0;
    if (args.size()) {
        auto index = toIndex(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        depth = *index;
    }
    CallFrame* frame = callerOf(callFrame);
    for (int64_t i = 0; i < depth && frame; ++i)
        frame = callerOf(frame);
    if (!frame)
        return JSValue::encode(raiseValueError(globalObject, scope, "call stack is not deep enough"_s));
    PyFrame* result = PyFrame::forCallFrame(vm, frame);
    if (!audit(globalObject, "sys._getframe"_s, result))
        return { };
    return JSValue::encode(result);
}

// _getframemodulename(depth=0)
PYTHON_NATIVE(sysGetFrameModuleName)
{
    NATIVE_PROLOGUE();
    int depth = 0;
    if (JSValue value = args.at(0)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        depth = *given;
    }
    if (!audit(globalObject, "sys._getframemodulename"_s, jsNumber(depth)))
        return { };
    CallFrame* frame = callerOf(callFrame);
    while (frame && depth-- > 0)
        frame = callerOf(frame);
    if (!frame)
        RETURN_NONE();
    // PyFunction_GetModule()
    JSValue module = getAttributeIfPresent(globalObject, frame->jsCallee(), names.dunder_module);
    if (scope.exception() && !scope.tryClearException())
        return { };
    return JSValue::encode(module ? module : jsUndefined());
}

// ---- _frame: what FrameLocalsProxy, which is written in Python, is written in terms of

static PyFrame* frameArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (auto* frame = dynamicDowncast<PyFrame>(value))
        return frame;
    raiseTypeError(globalObject, scope, concatenate("expect frame, not "_s, typeName(globalObject, value ? value : jsUndefined())));
    return nullptr;
}

// variable_names(frame): the names of its variables, bound or not.
PYTHON_NATIVE(frameVariableNames)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = frameArgument(globalObject, scope, args.at(0));
    if (!frame)
        return { };
    PyTuple* names_ = PyTuple::create(globalObject, frame->variableCount());
    for (unsigned i = 0; i < frame->variableCount(); ++i)
        names_->initializeAt(vm, i, jsString(vm, frame->variableName(i).string()));
    return JSValue::encode(names_);
}

static std::optional<unsigned> variableIndexArgument(JSGlobalObject* globalObject, ThrowScope& scope, PyFrame* frame, JSValue value)
{
    if (value && value.isInt32() && static_cast<uint32_t>(value.asInt32()) < frame->variableCount())
        return value.asInt32();
    raise(globalObject, scope, BuiltinType::IndexError, "no such variable"_s);
    return std::nullopt;
}

// get_variable(frame, index, default): the default if it is unbound.
PYTHON_NATIVE(frameGetVariable)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = frameArgument(globalObject, scope, args[0]);
    if (!frame)
        return { };
    auto index = variableIndexArgument(globalObject, scope, frame, args[1]);
    if (!index)
        return { };
    JSValue value = frame->variable(vm, *index);
    return JSValue::encode(value ? value : args[2]);
}

// set_variable(frame, index, value)
PYTHON_NATIVE(frameSetVariable)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = frameArgument(globalObject, scope, args[0]);
    if (!frame)
        return { };
    auto index = variableIndexArgument(globalObject, scope, frame, args[1]);
    if (!index)
        return { };
    frame->setVariable(vm, *index, args[2]);
    RETURN_NONE();
}

// extra_locals(frame, create): the dict of what has been added to its locals that is not a variable. None if there is none and none is to be made.
PYTHON_NATIVE(frameExtraLocals)
{
    NATIVE_PROLOGUE();
    PyFrame* frame = frameArgument(globalObject, scope, args[0]);
    if (!frame)
        return { };
    if (!frame->extraLocals() && args[1].isTrue())
        frame->setExtraLocals(vm, PyDict::create(globalObject));
    return JSValue::encode(frame->extraLocals() ? JSValue(frame->extraLocals()) : jsUndefined());
}

JSObject* createFrameModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_frame"_s);
    JSObject* ns = module;
    ns->putDirect(vm, Identifier::fromString(vm, "frame"_s), globalObject->pyRealm()->typeFrame());
    addFunction(globalObject, ns, "variable_names"_s, frameVariableNames, 0, "($module, frame, /)"_s);
    addFunction(globalObject, ns, "get_variable"_s, frameGetVariable, 0, "($module, frame, index, default, /)"_s);
    addFunction(globalObject, ns, "set_variable"_s, frameSetVariable, 0, "($module, frame, index, value, /)"_s);
    addFunction(globalObject, ns, "extra_locals"_s, frameExtraLocals, 0, "($module, frame, create, /)"_s);
    return module;
}

// ---- What is printed when an exception gets away

void forEachTracebackEntry(JSGlobalObject* globalObject, JSValue traceback, const ScopedLambda<void(PyFrame*, unsigned bytecodeOffset, unsigned line)>& function)
{
    if (!traceback || !isTraceback(globalObject, traceback))
        return;
    for (JSValue cursor = traceback; cursor && !isNone(cursor); cursor = asNative(cursor)->field(TracebackField::Next))
        function(asFrame(asNative(cursor)->field(TracebackField::Frame)), asNative(cursor)->field(TracebackField::BytecodeOffset).asInt32(), lineOf(cursor));
}

// _Py_DisplaySourceLine(): a line of a file, without what it is indented by. Null if there is no such line, or no such file.
String sourceLineForDisplay(JSGlobalObject* globalObject, const String& filename, int64_t line)
{
    if (line <= 0 || filename.isEmpty())
        return { };
    // <string>, <stdin> and the like are the names of no files.
    if (filename.startsWith('<') && filename.endsWith('>'))
        return { };
    SourceCode source = readSourceIfPresent(globalObject, filename);
    if (source.isNull()) {
        // _Py_FindSourceFile(): a file of that name in one of the directories that modules are looked for in.
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        size_t slash = filename.reverseFind('/');
        String tail = slash == notFound ? filename : filename.substring(slash + 1);
        JSValue path = sysAttribute(globalObject, "path"_s);
        if (!path || !isList(path))
            return { };
        for (unsigned i = 0; i < asList(path)->length() && source.isNull(); ++i) {
            JSValue directory = listGet(globalObject, asList(path), i);
            if (scope.exception()) {
                scope.clearException();
                return { };
            }
            if (!directory.isString())
                continue;
            String prefix = asString(directory)->value(globalObject);
            source = readSourceIfPresent(globalObject, concatenate(prefix, prefix.isEmpty() || prefix.endsWith('/') ? ""_s : "/"_s, tail));
        }
        if (source.isNull())
            return { };
    }
    StringView text = source.view();
    size_t start = 0;
    for (int64_t i = 1; i < line; ++i) {
        start = text.find('\n', start);
        if (start == notFound)
            return { };
        ++start;
    }
    if (start >= text.length())
        return { };
    size_t end = text.find('\n', start);
    StringView result = text.substring(start, end == notFound ? text.length() - start : end - start);
    while (!result.isEmpty() && (result[0] == ' ' || result[0] == '\t' || result[0] == '\f'))
        result = result.substring(1);
    while (!result.isEmpty() && result[result.length() - 1] == '\r')
        result = result.left(result.length() - 1);
    return result.toString();
}

// _PyTraceBack_Print() and tb_printinternal() of CPython's Python/traceback.c. It is not what an exception that gets away is shown by, which is written in Python and says a good deal more.
String formatTraceback(JSGlobalObject* globalObject, JSValue traceback)
{
    if (!traceback || !isTraceback(globalObject, traceback))
        return emptyString();
    // PyTraceBack_LIMIT
    int64_t limit = 1000;
    if (JSValue given = sysAttribute(globalObject, "tracebacklimit"_s); given && isInstance(globalObject, given, globalObject->pyRealm()->typeInt())) {
        // PyLong_AsLongAndOverflow(). It is an int, so that asking raises nothing.
        limit = *toIndex(globalObject, given, true);
        if (limit <= 0)
            return emptyString();
    }
    int64_t depth = 0;
    forEachTracebackEntry(globalObject, traceback, [&] (PyFrame*, unsigned, unsigned) { ++depth; });

    // TB_RECURSIVE_CUTOFF
    static constexpr unsigned cutoff = 3;
    TextBuilder builder;
    builder.append("Traceback (most recent call last):\n"_s);
    String lastFile;
    String lastName;
    unsigned lastLine = 0;
    unsigned count = 0;
    auto finishRun = [&] {
        if (count > cutoff)
            builder.append("  [Previous line repeated "_s, count - cutoff, " more time"_s, count - cutoff > 1 ? "s"_s : ""_s, "]\n"_s);
    };
    forEachTracebackEntry(globalObject, traceback, [&] (PyFrame* frame, unsigned, unsigned line) {
        // It is the innermost that are shown.
        if (depth-- > limit)
            return;
        String file = frame->executable()->source().provider()->sourceURL();
        String name = frame->functionInfo().name.string();
        if (lastFile.isNull() || file != lastFile || line != lastLine || name != lastName) {
            finishRun();
            lastFile = file;
            lastLine = line;
            lastName = name;
            count = 0;
        }
        if (++count > cutoff)
            return;
        // tb_displayline()
        builder.append("  File \""_s, file, "\", line "_s, line, ", in "_s, name, '\n');
        if (String text = sourceLineForDisplay(globalObject, file, line); !text.isNull())
            builder.append("    "_s, text, '\n');
    });
    finishRun();
    return builder.tryFinish();
}

// For a SyntaxError, where in the source it is, which is not where it was raised: the file and the line, the text of the line, and under that what part
// of it. This is TracebackException._format_syntax_error() of CPython's Lib/traceback.py. What is left to say is then its message alone, with the name
// of the file if there was no line to give with it, which this returns. Null if it is not a SyntaxError.
String appendSyntaxErrorLocation(JSGlobalObject* globalObject, StringBuilder& builder, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!isInstance(globalObject, exception, globalObject->pyRealm()->typeSyntaxError()))
        return { };
    auto attribute = [&] (ASCIILiteral name) -> JSValue {
        JSValue value = getAttributeIfPresent(globalObject, exception, Identifier::fromString(vm, name));
        if (scope.exception()) {
            scope.clearException();
            return jsUndefined();
        }
        return value ? value : jsUndefined();
    };
    auto text = [&] (JSValue value) -> String {
        String result = str(globalObject, value);
        if (scope.exception()) {
            scope.clearException();
            return "<exception str() failed>"_s;
        }
        return result;
    };
    auto isTruthy = [&] (JSValue value) {
        bool result = isTrue(globalObject, value);
        scope.clearException();
        return result;
    };

    JSValue filename = attribute("filename"_s);
    JSValue line = attribute("lineno"_s);
    String suffix = emptyString();
    if (!isNone(line))
        builder.append("  File \""_s, isTruthy(filename) ? text(filename) : String("<string>"_s), "\", line "_s, text(line), '\n');
    else if (!isNone(filename))
        suffix = concatenate(" ("_s, text(filename), ')');

    JSValue textValue = attribute("text"_s);
    if (textValue.isString()) {
        // Lengths and offsets are in characters, so this goes by those.
        Vector<char32_t> whole;
        for (char32_t c : StringView(asString(textValue)->value(globalObject).data).codePoints())
            whole.append(c);
        size_t end = whole.size();
        while (end && whole[end - 1] == '\n')
            --end;
        size_t start = 0;
        while (start < end && (whole[start] == ' ' || whole[start] == '\n' || whole[start] == '\f'))
            ++start;
        auto stripped = whole.span().subspan(start, end - start);
        int64_t spaces = start;
        int64_t rightStrippedLength = end;
        auto appendLine = [&] {
            builder.append("    "_s);
            for (char32_t c : stripped)
                builder.append(c);
            builder.append('\n');
        };

        JSValue offsetValue = attribute("offset"_s);
        if (isNone(offsetValue))
            appendLine();
        else if (auto given = isInt(offsetValue) ? tryInt64(offsetValue) : std::nullopt) {
            int64_t offset = *given;
            int64_t endOffset = rightStrippedLength + 1;
            JSValue endLine = attribute("end_lineno"_s);
            bool isOnOneLine = isEqual(globalObject, line, endLine);
            scope.clearException();
            if (isOnOneLine) {
                JSValue endOffsetValue = attribute("end_offset"_s);
                auto givenEnd = isInt(endOffsetValue) ? tryInt64(endOffsetValue) : std::nullopt;
                endOffset = givenEnd && *givenEnd ? *givenEnd : offset;
            }
            int64_t wholeLength = whole.size();
            if (wholeLength && offset > wholeLength)
                offset = rightStrippedLength + 1;
            if (wholeLength && endOffset > wholeLength)
                endOffset = rightStrippedLength + 1;
            if (offset >= endOffset || endOffset < 0)
                endOffset = offset + 1;
            // From counting from one in the whole line to counting from zero in what is left of it.
            int64_t column = offset - 1 - spaces;
            int64_t endColumn = endOffset - 1 - spaces;
            appendLine();
            if (column >= 0) {
                builder.append("    "_s);
                // A tab is kept, so that what is under it lines up.
                for (int64_t i = 0; i < column && i < static_cast<int64_t>(stripped.size()); ++i)
                    builder.append(Unicode::isWhitespace(stripped[i]) ? stripped[i] : U' ');
                for (int64_t i = column; i < endColumn; ++i)
                    builder.append('^');
                builder.append('\n');
            }
        }
    }
    JSValue message = attribute("msg"_s);
    return concatenate(isTruthy(message) ? text(message) : String("<no detail available>"_s), suffix);
}

// ---- Setting them up

void initializeTracebackTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyType* traceback = realm->typeTraceback();
    traceback->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, traceback));
    addMethods(globalObject, traceback, {
        { "__new__"_s, tracebackNew, PyNativeFunction::Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__dir__"_s, tracebackDir },
    });
    addGetSet(globalObject, traceback, "tb_next"_s, getField<TracebackField::Next>, setTracebackNext);
    addMember(globalObject, traceback, "tb_frame"_s, getField<TracebackField::Frame>);
    addGetSet(globalObject, traceback, "tb_lineno"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(lineOf(self)); });
    addMember(globalObject, traceback, "tb_lasti"_s, getField<TracebackField::BytecodeOffset>);

    PyType* frame = realm->typeFrame();
    frame->setInstanceStructure(vm, PyFrame::createStructure(vm, globalObject, frame));
    addMethods(globalObject, frame, {
        { "clear"_s, frameClear },
        { "__repr__"_s, frameRepr },
    });
    addGetSet(globalObject, frame, "f_back"_s, getFrameBack);
    addGetSet(globalObject, frame, "f_locals"_s, getFrameLocals);
    addGetSet(globalObject, frame, "f_lineno"_s, getFrameLine, setFrameLine);
    addGetSet(globalObject, frame, "f_trace"_s, getFrameTrace, setFrameTrace);
    addGetSet(globalObject, frame, "f_lasti"_s, getFrameLastInstruction);
    addGetSet(globalObject, frame, "f_globals"_s, getFrameGlobals);
    addGetSet(globalObject, frame, "f_builtins"_s, getFrameBuiltins);
    addGetSet(globalObject, frame, "f_code"_s, getFrameCode);
    addGetSet(globalObject, frame, "f_trace_opcodes"_s, getFrameFlag<&PyFrame::tracesOpcodes>, setFrameFlag<&PyFrame::setTracesOpcodes>);
    addGetSet(globalObject, frame, "f_trace_lines"_s, getFrameFlag<&PyFrame::tracesLines>, setFrameFlag<&PyFrame::setTracesLines>);
    addGetSet(globalObject, frame, "f_generator"_s, getFrameGenerator);
}

void addFrameFunctions(JSGlobalObject* globalObject, JSObject* sysNamespace)
{
    addFunction(globalObject, sysNamespace, "_getframe"_s, sysGetFrame);
    addFunction(globalObject, sysNamespace, "_getframemodulename"_s, sysGetFrameModuleName);
}

} } // namespace JSC::Python
