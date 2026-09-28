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
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/StringBuilder.h>

// Tracebacks and frames.

namespace JSC { namespace Python {

// A traceback is a PyNativeObject: the next one, the function whose frame it was, and where in the source.
enum TracebackField : unsigned { Next, Function, Offset };
// A frame is one too: the code, the globals, the line, and the frame that called it.
enum FrameField : unsigned { Code, Globals, Line, Back };

static PyNativeObject* asNative(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }

static bool isTraceback(JSGlobalObject* globalObject, JSValue value)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeTraceback();
}

void addTracebackEntry(JSGlobalObject* globalObject, JSValue exception, CallFrame* frame, BytecodeIndex bytecodeIndex)
{
    if (!exception.isObject())
        return;
    VM& vm = globalObject->vm();
    CodeBlock* codeBlock = frame->codeBlock();
    if (codeBlock->ownerExecutable()->implementationVisibility() != ImplementationVisibility::Public)
        return;
    DeferGCForAWhile deferGC(vm);
    JSValue head = asObject(exception)->getDirect(vm, vm.pythonNames().private_traceback);
    if (!head || !isTraceback(globalObject, head))
        head = jsUndefined();
    unsigned offset = codeBlock->expressionInfoForBytecodeIndex(bytecodeIndex).divot;
    JSValue entry = PyNativeObject::create(globalObject, BuiltinType::Traceback, head, frame->jsCallee(), jsNumber(offset));
    asObject(exception)->putDirect(vm, vm.pythonNames().private_traceback, entry);
}

static FunctionExecutable* executableOf(JSValue function)
{
    return uncheckedDowncast<JSFunction>(function.asCell())->jsExecutable();
}

static unsigned lineOf(JSValue traceback)
{
    PyNativeObject* entry = asNative(traceback);
    return executableOf(entry->field(TracebackField::Function))->source().provider()->documentLineColumnForOffset(entry->field(TracebackField::Offset).asInt32()).line;
}

static JSValue getTracebackFrame(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    JSValue function = asNative(self)->field(TracebackField::Function);
    JSValue code = getAttribute(globalObject, function, Identifier::fromString(vm, "__code__"_s));
    JSValue globals = getAttribute(globalObject, function, Identifier::fromString(vm, "__globals__"_s));
    return PyNativeObject::create(globalObject, BuiltinType::Frame, code, globals, jsNumber(lineOf(self)), jsUndefined());
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

// ---- sys._getframe()

static JSValue frameFor(JSGlobalObject* globalObject, CallFrame* frame)
{
    VM& vm = globalObject->vm();
    if (!frame)
        return jsUndefined();
    JSValue function = frame->jsCallee();
    JSValue code = getAttribute(globalObject, function, Identifier::fromString(vm, "__code__"_s));
    JSValue globals = getAttribute(globalObject, function, Identifier::fromString(vm, "__globals__"_s));
    CodeBlock* codeBlock = frame->codeBlock();
    unsigned line = codeBlock->source().provider()->documentLineColumnForOffset(codeBlock->expressionInfoForBytecodeIndex(frame->bytecodeIndex()).divot).line;
    return PyNativeObject::create(globalObject, BuiltinType::Frame, code, globals, jsNumber(line), frameFor(globalObject, callerOf(frame)));
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
    return JSValue::encode(frameFor(globalObject, frame));
}

// ---- What is printed when an exception gets away

static void appendTraceback(JSGlobalObject* globalObject, StringBuilder& builder, JSValue traceback)
{
    if (!traceback || !isTraceback(globalObject, traceback))
        return;
    builder.append("Traceback (most recent call last):\n"_s);
    for (JSValue cursor = traceback; cursor && !isNone(cursor); cursor = asNative(cursor)->field(TracebackField::Next)) {
        FunctionExecutable* executable = executableOf(asNative(cursor)->field(TracebackField::Function));
        SourceProvider* provider = executable->source().provider();
        unsigned line = lineOf(cursor);
        builder.append("  File \""_s, provider->sourceURL(), "\", line "_s, line, ", in "_s, executable->unlinkedExecutable()->pythonInfo()->name.string(), '\n');
        // The line itself, without its indentation.
        StringView text = provider->source();
        unsigned start = asNative(cursor)->field(TracebackField::Offset).asInt32();
        start = std::min(start, text.length());
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

void initializeTracebackTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyType* traceback = realm->typeTraceback();
    traceback->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, traceback));
    addGetSet(globalObject, traceback, "tb_next"_s, getField<TracebackField::Next>, setTracebackNext);
    addMember(globalObject, traceback, "tb_frame"_s, getTracebackFrame);
    addGetSet(globalObject, traceback, "tb_lineno"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(lineOf(self)); });
    addMember(globalObject, traceback, "tb_lasti"_s, getField<TracebackField::Offset>);

    PyType* frame = realm->typeFrame();
    addGetSet(globalObject, frame, "f_lineno"_s, getField<FrameField::Line>);
    addGetSet(globalObject, frame, "f_back"_s, getField<FrameField::Back>);
}

void addFrameFunctions(JSGlobalObject* globalObject, JSObject* sysNamespace)
{
    addFunction(globalObject, sysNamespace, "_getframe"_s, sysGetFrame);
}

} } // namespace JSC::Python
