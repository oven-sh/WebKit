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
#include "PyFrame.h"
#include "PythonGenerators.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/StringBuilder.h>

// Tracebacks, and what Python sees of frames. PyFrame.h says what a frame object is.

namespace JSC { namespace Python {

// ---- Tracebacks

// A traceback is a PyNativeObject: the next one, the frame, and where the frame had got to, in the bytecode and in the source.
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
    unsigned sourceOffset = callFrame->codeBlock()->expressionInfoForBytecodeIndex(bytecodeIndex).divot;
    JSValue entry = PyNativeObject::create(globalObject, BuiltinType::Traceback, head, PyFrame::forCallFrame(vm, callFrame), jsNumber(bytecodeIndex.offset()), jsNumber(sourceOffset));
    asObject(exception)->putDirect(vm, vm.pythonNames().private_traceback, entry);
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

static unsigned lineOf(JSValue traceback)
{
    PyNativeObject* entry = asNative(traceback);
    return asFrame(entry->field(TracebackField::Frame))->executable()->source().provider()->documentLineColumnForOffset(entry->field(TracebackField::SourceOffset).asInt32()).line;
}

template<unsigned field>
static JSValue getField(JSGlobalObject*, JSValue self)
{
    JSValue value = asNative(self)->field(field);
    return value ? value : jsUndefined();
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
        raiseTypeError(globalObject, scope, makeString("expected traceback object, got '"_s, typeName(globalObject, value), '\''));
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
    return jsNumber(asFrame(self)->line(globalObject->vm()));
}

static void setFrameLine(JSGlobalObject* globalObject, JSValue, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
    else
        raiseValueError(globalObject, scope, "f_lineno can only be set in a trace function"_s);
}

static JSValue getFrameLastInstruction(JSGlobalObject* globalObject, JSValue self)
{
    // A generator that has not started has been made, which is the beginning of its code.
    auto index = asFrame(self)->bytecodeIndex(globalObject->vm());
    return jsNumber(index ? index->offset() : 0);
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
    asFrame(self)->setTrace(globalObject->vm(), value && !isNone(value) ? value : JSValue());
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
    (asFrame(self)->*setter)(value.asBoolean());
}

PYTHON_NATIVE(frameClear)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "clear"_s, 1, 1))
        return { };
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
    return JSValue::encode(jsString(vm, makeString("<frame at 0x"_s, hex(std::bit_cast<uintptr_t>(frame), Lowercase), ", file "_s, filename, ", line "_s, frame->line(vm), ", code "_s, frame->functionInfo().name.string(), '>')));
}

PYTHON_NATIVE(sysGetFrame)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "_getframe"_s, 0, 1))
        return { };
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
    return JSValue::encode(PyFrame::forCallFrame(vm, frame));
}

// ---- _frame: what FrameLocalsProxy, which is written in Python, is written in terms of

static PyFrame* frameArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (auto* frame = dynamicDowncast<PyFrame>(value))
        return frame;
    raiseTypeError(globalObject, scope, makeString("expect frame, not "_s, typeName(globalObject, value ? value : jsUndefined())));
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
    if (!args.check(globalObject, scope, "get_variable"_s, 3, 3))
        return { };
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
    if (!args.check(globalObject, scope, "set_variable"_s, 3, 3))
        return { };
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
    if (!args.check(globalObject, scope, "extra_locals"_s, 2, 2))
        return { };
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
    JSObject* module = newModule(globalObject, "_frame"_s);
    JSObject* ns = module;
    ns->putDirect(vm, Identifier::fromString(vm, "frame"_s), globalObject->pyRealm()->typeFrame());
    addFunction(globalObject, ns, "variable_names"_s, frameVariableNames);
    addFunction(globalObject, ns, "get_variable"_s, frameGetVariable);
    addFunction(globalObject, ns, "set_variable"_s, frameSetVariable);
    addFunction(globalObject, ns, "extra_locals"_s, frameExtraLocals);
    return module;
}

// ---- What is printed when an exception gets away

static void appendTraceback(JSGlobalObject* globalObject, StringBuilder& builder, JSValue traceback)
{
    if (!traceback || !isTraceback(globalObject, traceback))
        return;
    builder.append("Traceback (most recent call last):\n"_s);
    for (JSValue cursor = traceback; cursor && !isNone(cursor); cursor = asNative(cursor)->field(TracebackField::Next)) {
        PyFrame* frame = asFrame(asNative(cursor)->field(TracebackField::Frame));
        SourceProvider* provider = frame->executable()->source().provider();
        builder.append("  File \""_s, provider->sourceURL(), "\", line "_s, lineOf(cursor), ", in "_s, frame->functionInfo().name.string(), '\n');
        // The line itself, without its indentation.
        StringView text = provider->source();
        unsigned start = std::min<unsigned>(asNative(cursor)->field(TracebackField::SourceOffset).asInt32(), text.length());
        while (start && text[start - 1] != '\n')
            --start;
        unsigned end = start;
        while (end < text.length() && text[end] != '\n')
            ++end;
        StringView content = text.substring(start, end - start).trim([] (char16_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\r'; });
        if (!content.isEmpty())
            builder.append("    "_s, content, '\n');
    }
}

static void appendException(JSGlobalObject* globalObject, StringBuilder& builder, JSValue exception, Vector<JSCell*, 8>& seen)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& names = vm.pythonNames();
    JSObject* object = exception.isObject() ? asObject(exception) : nullptr;
    if (object) {
        seen.append(object);
        // What led to it comes first.
        JSValue cause = object->getDirect(vm, names.private_cause);
        JSValue context = object->getDirect(vm, names.private_context);
        JSValue suppress = object->getDirect(vm, names.private_suppressContext);
        if (cause && cause.isObject() && !seen.contains(cause.asCell())) {
            appendException(globalObject, builder, cause, seen);
            builder.append("\nThe above exception was the direct cause of the following exception:\n\n"_s);
        } else if (context && context.isObject() && !(suppress && suppress.isTrue()) && !seen.contains(context.asCell())) {
            appendException(globalObject, builder, context, seen);
            builder.append("\nDuring handling of the above exception, another exception occurred:\n\n"_s);
        }
        appendTraceback(globalObject, builder, object->getDirect(vm, names.private_traceback));
    }

    PyType* type = typeOf(globalObject, exception);
    String name = qualifiedNameOfType(globalObject, type);
    String message = str(globalObject, exception);
    if (scope.exception()) {
        scope.clearException();
        message = "<exception str() failed>"_s;
    }
    builder.append(name);
    if (!message.isEmpty())
        builder.append(": "_s, message);
    builder.append('\n');

    JSValue notes = object ? getAttributeIfPresent(globalObject, exception, names.dunder_notes) : JSValue();
    scope.clearException();
    if (notes && isList(notes)) {
        for (unsigned i = 0; i < asList(notes)->length(); ++i) {
            String note = str(globalObject, listGet(asList(notes), i));
            if (scope.exception()) {
                scope.clearException();
                continue;
            }
            builder.append(note, '\n');
        }
    }
}

String formatException(JSGlobalObject* globalObject, JSValue exception)
{
    StringBuilder builder;
    Vector<JSCell*, 8> seen;
    appendException(globalObject, builder, exception, seen);
    return builder.toString();
}

// ---- Setting them up

void initializeTracebackTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyType* traceback = realm->typeTraceback();
    traceback->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, traceback));
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
}

} } // namespace JSC::Python
