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

#include "JSLexicalEnvironment.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonGenerators.h"
#include <wtf/SafeStrerror.h>
#include <wtf/text/StringBuilder.h>

// The functions of the builtins module.

namespace JSC { namespace Python {


// ---- print()

// file.write(str(value))
static void writeTo(JSGlobalObject* globalObject, JSValue file, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue write = getAttribute(globalObject, file, Identifier::fromString(vm, "write"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (!value.isString()) {
        String text = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        value = jsString(vm, text);
    }
    scope.release();
    call(globalObject, write, value);
}

// print(*objects, sep=' ', end='\n', file=None, flush=False)
PYTHON_NATIVE(builtinPrint)
{
    NATIVE_PROLOGUE();
    JSValue separator;
    JSValue end;
    JSValue file;
    bool flush = false;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String name = args.keywordName(i)->value(globalObject);
        JSValue value = args.keywordValue(i);
        if (name == "sep"_s || name == "end"_s) {
            if (isNone(value))
                continue;
            if (!value.isString())
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, " must be None or a string, not "_s, typeName(globalObject, value))));
            (name == "sep"_s ? separator : end) = value;
        } else if (name == "file"_s)
            file = isNone(value) ? JSValue() : value;
        else if (name == "flush"_s) {
            flush = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("print() got an unexpected keyword argument '"_s, name, '\'')));
    }

    if (!file) {
        file = sysAttribute(globalObject, "stdout"_s);
        if (!file)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "lost sys.stdout"_s));
        // There may be none, as in a program with no console.
        if (isNone(file))
            RETURN_NONE();
    }

    // Each thing is written by itself, which whatever the file is can tell.
    for (unsigned i = 0; i < args.size(); ++i) {
        if (i) {
            writeTo(globalObject, file, separator ? separator : JSValue(vm.smallStrings.singleCharacterString(' ')));
            RETURN_IF_EXCEPTION(scope, { });
        }
        writeTo(globalObject, file, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    writeTo(globalObject, file, end ? end : JSValue(vm.smallStrings.singleCharacterString('\n')));
    RETURN_IF_EXCEPTION(scope, { });

    if (flush) {
        JSValue flushMethod = getAttribute(globalObject, file, Identifier::fromString(vm, "flush"_s));
        RETURN_IF_EXCEPTION(scope, { });
        call(globalObject, flushMethod);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- What every object can be asked

PYTHON_NATIVE(builtinRepr_)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(builtinAscii)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder builder;
    for (char32_t c : StringView(text).codePoints()) {
        if (c < 0x80)
            builder.append(static_cast<Latin1Character>(c));
        else if (c <= 0xFF)
            builder.append("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
        else if (c <= 0xFFFF)
            builder.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
        else
            builder.append("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
    }
    return JSValue::encode(jsString(vm, builder.toString()));
}

PYTHON_NATIVE(builtinLen)
{
    NATIVE_PROLOGUE();
    int64_t size = length(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, size)));
}

PYTHON_NATIVE(builtinHash_)
{
    NATIVE_PROLOGUE();
    int64_t result = hash(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, result)));
}

PYTHON_NATIVE(builtinId)
{
    NATIVE_PROLOGUE();
    // What is not a cell has no address, so its bits will have to do.
    int64_t identity = args[0].isCell() ? static_cast<int64_t>(std::bit_cast<uintptr_t>(args[0].asCell())) : static_cast<int64_t>(JSValue::encode(args[0]) & 0x7FFFFFFFFFFFFFFFLL);
    JSValue result = intFromInt64(globalObject, identity);
    RETURN_IF_EXCEPTION(scope, { });
    if (!audit(globalObject, "builtins.id"_s, result))
        return { };
    return JSValue::encode(result);
}

PYTHON_NATIVE(builtinCallable)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(jsBoolean(isCallable(globalObject, args[0])));
}

PYTHON_NATIVE(builtinFormat_)
{
    NATIVE_PROLOGUE();
    String specification = emptyString();
    if (args.size() > 1) {
        if (!args[1].isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("format() argument 2 must be str, not "_s, isNone(args[1]) ? "None"_str : typeName(globalObject, args[1]))));
        specification = asString(args[1])->value(globalObject);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(format(globalObject, args[0], specification)));
}

// ---- Attributes

static std::optional<Identifier> attributeNameArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue name)
{
    if (!name.isString()) {
        raiseTypeError(globalObject, scope, makeString("attribute name must be string, not '"_s, typeName(globalObject, name), '\''));
        return std::nullopt;
    }
    return asString(name)->toIdentifier(globalObject);
}

PYTHON_NATIVE(builtinGetAttr)
{
    NATIVE_PROLOGUE();
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 2)
        RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, args[0], *name)));
    JSValue value = getAttributeIfPresent(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value ? value : args[2]);
}

PYTHON_NATIVE(builtinHasAttr)
{
    NATIVE_PROLOGUE();
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = getAttributeIfPresent(globalObject, args[0], *name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(!!value));
}

PYTHON_NATIVE(builtinSetAttr)
{
    NATIVE_PROLOGUE();
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    setAttribute(globalObject, args[0], *name, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(builtinDelAttr)
{
    NATIVE_PROLOGUE();
    auto name = attributeNameArgument(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    deleteAttribute(globalObject, args[0], *name);
    RETURN_NONE();
}

PYTHON_NATIVE(builtinIsInstance)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isInstanceOf(globalObject, args[0], args[1]))));
}

PYTHON_NATIVE(builtinIsSubclass)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(isSubclassOf(globalObject, args[0], args[1]))));
}

PYTHON_NATIVE(builtinDir)
{
    NATIVE_PROLOGUE();
    if (!args.size()) {
        JSValue locals = localsOfFrame(globalObject, callerOf(callFrame));
        MarkedArgumentBuffer keys;
        collect(globalObject, locals, keys);
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer sortedKeys;
        sortValues(globalObject, keys, JSValue(), false, sortedKeys);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, sortedKeys)));
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_dir, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue found = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer values;
    collect(globalObject, found, values);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer sorted;
    sortValues(globalObject, values, JSValue(), false, sorted);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, sorted)));
}

PYTHON_NATIVE(builtinVars)
{
    NATIVE_PROLOGUE();
    if (!args.size())
        return JSValue::encode(localsOfFrame(globalObject, callerOf(callFrame)));
    JSValue dict = getAttributeIfPresent(globalObject, args[0], names.dunder_dict);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        return JSValue::encode(raiseTypeError(globalObject, scope, "vars() argument must have __dict__ attribute"_s));
    return JSValue::encode(dict);
}

// ---- Iteration

PYTHON_NATIVE(builtinIter)
{
    NATIVE_PROLOGUE();
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, JSValue::encode(getIterator(globalObject, args[0])));
    if (!isCallable(globalObject, args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "iter(v, w): v must be callable"_s));
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::Callable, args[0], args[1]));
}

PYTHON_NATIVE(builtinNext)
{
    NATIVE_PROLOGUE();
    // What a generator returns is carried by the StopIteration.
    if (args[0].isCell() && args[0].asCell()->type() == JSGeneratorType && args.size() == 1 && generatorKindOf(globalObject, asGenerator(args[0])) == GeneratorKind::Generator)
        RELEASE_AND_RETURN(scope, JSValue::encode(generatorSend(globalObject, uncheckedDowncast<JSGenerator>(args[0].asCell()), jsUndefined())));
    JSValue value = iteratorNext(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    if (args.size() > 1)
        return JSValue::encode(args[1]);
    return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
}

PYTHON_NATIVE(builtinAnyOrAll)
{
    auto isAny = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    bool result = !isAny;
    forEach(globalObject, args[0], [&] (JSValue value) {
        if (isTrue(globalObject, value) != isAny)
            return true;
        result = isAny;
        return false;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result));
}

// sum(iterable, /, start=0)
PYTHON_NATIVE(builtinSum)
{
    NATIVE_PROLOGUE();
    if (!args.size() || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sum() takes at most 2 arguments ("_s, args.size(), " given)"_s)));
    JSValue total = args.at(1);
    if (!total)
        total = jsNumber(0);
    if (total.isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "sum() can't sum strings [use ''.join(seq) instead]"_s));
    forEach(globalObject, args[0], [&] (JSValue value) {
        total = binaryOperation(globalObject, BinaryOperator::Add, false, total, value);
        return !!total;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(total);
}

// min(iterable, *, key=None, default=...) and min(a, b, ..., key=None)
PYTHON_NATIVE(builtinMinOrMax)
{
    auto isMax = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    ASCIILiteral name = isMax ? "max"_s : "min"_s;
    if (!args.size())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, " expected at least 1 argument, got 0"_s)));
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    JSValue keyFunction = args.keyword(globalObject, "key"_s);
    if (keyFunction && isNone(keyFunction))
        keyFunction = { };
    JSValue defaultValue = args.keyword(globalObject, "default"_s);
    if (defaultValue && args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Cannot specify a default for "_s, name, "() with multiple positional arguments"_s)));

    JSValue best;
    JSValue bestKey;
    auto consider = [&] (JSValue value) -> bool {
        JSValue key = value;
        if (keyFunction) {
            key = call(globalObject, keyFunction, value);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (best) {
            JSValue isBetter = compare(globalObject, isMax ? ComparisonOperator::Gt : ComparisonOperator::Lt, key, bestKey);
            RETURN_IF_EXCEPTION(scope, false);
            bool replace = isTrue(globalObject, isBetter);
            RETURN_IF_EXCEPTION(scope, false);
            if (!replace)
                return true;
        }
        best = value;
        bestKey = key;
        return true;
    };
    if (args.size() == 1)
        forEach(globalObject, args[0], consider);
    else {
        for (unsigned i = 0; i < args.size() && consider(args[i]); ++i) { }
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (best)
        return JSValue::encode(best);
    if (defaultValue)
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, makeString(name, "() iterable argument is empty"_s)));
}

// sorted(iterable, /, *, key=None, reverse=False)
// sorted(iterable, /, *, key=None, reverse=False): a list of what is in it, sorted as list.sort() does it, which is given the rest.
PYTHON_NATIVE(builtinSorted)
{
    NATIVE_PROLOGUE();
    if (args.size() != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sorted expected 1 argument, got "_s, args.size())));
    MarkedArgumentBuffer values;
    collect(globalObject, args[0], values);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = newList(globalObject, values);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    arguments.append(list);
    for (unsigned i = 0; i < args.keywordCount(); ++i)
        arguments.append(args.keywordValue(i));
    callWithKeywords(globalObject, realm->typeList()->lookup(vm, Identifier::fromString(vm, "sort"_s)), arguments, args.keywordNames());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(list);
}

// ---- Numbers and characters

PYTHON_NATIVE(builtinAbs)
{
    NATIVE_PROLOGUE();
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_abs, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("bad operand type for abs(): '"_s, typeName(globalObject, args[0]), '\'')));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// round(number, ndigits=None)
PYTHON_NATIVE(builtinRound)
{
    NATIVE_PROLOGUE();
    JSValue number = args.at(0);
    JSValue digits = args.at(1);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, number, names.dunder_round, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type "_s, typeName(globalObject, number), " doesn't define __round__ method"_s)));
    if (!digits || isNone(digits))
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self, digits)));
}

PYTHON_NATIVE(builtinDivmod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(divmod(globalObject, args[0], args[1])));
}

// pow(base, exp, mod=None)
PYTHON_NATIVE(builtinPow)
{
    NATIVE_PROLOGUE();
    JSValue modulus = args.at(2);
    RELEASE_AND_RETURN(scope, JSValue::encode(power(globalObject, args.at(0), args.at(1), modulus ? modulus : jsUndefined())));
}

PYTHON_NATIVE(builtinChr)
{
    NATIVE_PROLOGUE();
    auto code = toIndex(globalObject, args[0], true);
    RETURN_IF_EXCEPTION(scope, { });
    if (*code < 0 || *code > 0x10FFFF)
        return JSValue::encode(raiseValueError(globalObject, scope, "chr() arg not in range(0x110000)"_s));
    StringBuilder builder;
    builder.append(static_cast<char32_t>(*code));
    return JSValue::encode(jsString(vm, builder.toString()));
}

PYTHON_NATIVE(builtinOrd)
{
    NATIVE_PROLOGUE();
    if (!args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("ord() expected string of length 1, but "_s, typeName(globalObject, args[0]), " found"_s)));
    unsigned count = stringLength(globalObject, asString(args[0]));
    if (count != 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("ord() expected a character, but string of length "_s, count, " found"_s)));
    auto view = asString(args[0])->view(globalObject);
    return JSValue::encode(jsNumber(static_cast<int32_t>(*view->codePoints().begin())));
}

// bin(), oct() and hex()
PYTHON_NATIVE(builtinInRadix)
{
    auto radix = unpack<unsigned>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue index = toInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    String digits = reprOfInt(globalObject, classify(index), radix);
    RETURN_IF_EXCEPTION(scope, { });
    ASCIILiteral prefix = radix == 2 ? "0b"_s : radix == 8 ? "0o"_s : "0x"_s;
    if (digits.startsWith('-'))
        return JSValue::encode(jsString(vm, makeString('-', prefix, StringView(digits).substring(1))));
    return JSValue::encode(jsString(vm, makeString(prefix, digits)));
}

// ---- Classes and modules

// __build_class__(function, name, *bases, **keywords)
PYTHON_NATIVE(builtinBuildClass)
{
    NATIVE_PROLOGUE();
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__build_class__: not enough arguments"_s));
    if (!args[1].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "__build_class__: name is not a string"_s));
    PyTuple* bases = PyTuple::create(globalObject, args.size() - 2);
    for (unsigned i = 2; i < args.size(); ++i)
        bases->initializeAt(vm, i - 2, args[i]);
    PyDict* keywords = nullptr;
    if (args.keywordCount()) {
        keywords = PyDict::create(globalObject);
        for (unsigned i = 0; i < args.keywordCount(); ++i)
            keywords->set(globalObject, args.keywordName(i), args.keywordValue(i));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(buildClass(globalObject, args[0], asString(args[1]), bases, keywords)));
}

// __import__(name, globals=None, locals=None, fromlist=(), level=0)
PYTHON_NATIVE(builtinImport)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, "__import__() argument 1 must be str"_s));
    JSValue fromList = args.at(3);
    JSValue levelValue = args.at(4);
    unsigned level = levelValue && levelValue.isInt32() ? levelValue.asInt32() : 0;
    JSObject* globals = globalsOfFrame(globalObject, callerOf(callFrame));
    RELEASE_AND_RETURN(scope, JSValue::encode(importModule(globalObject, globals, asString(args[0])->value(globalObject), fromList ? fromList : jsUndefined(), level, false)));
}

void initializeBuiltinFunctions(JSGlobalObject* globalObject, JSObject* namespaceObject)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, namespaceObject, name, function, data); };
    add("print"_s, builtinPrint);
    add("repr"_s, builtinRepr_);
    add("ascii"_s, builtinAscii);
    add("len"_s, builtinLen);
    add("hash"_s, builtinHash_);
    add("id"_s, builtinId);
    add("callable"_s, builtinCallable);
    add("format"_s, builtinFormat_);
    addFunction(globalObject, namespaceObject, "getattr"_s, builtinGetAttr, 0, "(object, name, default=None, /)"_s);
    add("hasattr"_s, builtinHasAttr);
    add("setattr"_s, builtinSetAttr);
    add("delattr"_s, builtinDelAttr);
    add("isinstance"_s, builtinIsInstance);
    add("issubclass"_s, builtinIsSubclass);
    addFunction(globalObject, namespaceObject, "dir"_s, builtinDir, 0, "(object=None, /)"_s);
    addFunction(globalObject, namespaceObject, "vars"_s, builtinVars, 0, "(object=None, /)"_s);
    addFunction(globalObject, namespaceObject, "iter"_s, builtinIter, 0, "(object, sentinel=None, /)"_s);
    addFunction(globalObject, namespaceObject, "next"_s, builtinNext, 0, "(iterator, default=None, /)"_s);
    add("any"_s, builtinAnyOrAll, pack(true));
    add("all"_s, builtinAnyOrAll, pack(false));
    add("sum"_s, builtinSum);
    // These have something of their own to say before their keywords are looked at.
    addFunction(globalObject, namespaceObject, "min"_s, builtinMinOrMax, pack(false), "(*args, key=None, default=None)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, namespaceObject, "max"_s, builtinMinOrMax, pack(true), "(*args, key=None, default=None)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, namespaceObject, "sorted"_s, builtinSorted, 0, { }, PyNativeFunction::Arguments::AreNotChecked);
    add("abs"_s, builtinAbs);
    add("round"_s, builtinRound);
    add("divmod"_s, builtinDivmod);
    add("pow"_s, builtinPow);
    add("chr"_s, builtinChr);
    add("ord"_s, builtinOrd);
    add("bin"_s, builtinInRadix, pack(2));
    add("oct"_s, builtinInRadix, pack(8));
    add("hex"_s, builtinInRadix, pack(16));
    addFunction(globalObject, namespaceObject, "__build_class__"_s, builtinBuildClass, 0, "(*args, **kwargs)"_s);
    add("__import__"_s, builtinImport);
}

} } // namespace JSC::Python
