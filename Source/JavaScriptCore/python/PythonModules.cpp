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

#include "PythonCompiler.h"
#include "SourceProvider.h"
#include <wtf/MonotonicTime.h>
#include <wtf/WallTime.h>

// import, and the modules that are written in C++.

namespace JSC { namespace Python {

static SourceReader s_sourceReader;

void setSourceReader(SourceReader reader)
{
    s_sourceReader = reader;
}

static PyDict* modulesOf(JSGlobalObject* globalObject) { return uncheckedDowncast<PyDict>(globalObject->pyRealm()->modules()); }

void registerModule(JSGlobalObject* globalObject, const String& name, JSValue module)
{
    modulesOf(globalObject)->setString(globalObject, name, module);
}

// ---- math

static double toDegrees(double x) { return x * (180.0 / std::numbers::pi); }
static double toRadians(double x) { return x * (std::numbers::pi / 180.0); }

#define FOR_EACH_MATH_FUNCTION(v) \
    v(log2, std::log2) v(log10, std::log10) v(log1p, std::log1p) v(exp, std::exp) v(expm1, std::expm1) v(sin, std::sin) v(cos, std::cos) v(tan, std::tan) \
    v(asin, std::asin) v(acos, std::acos) v(atan, std::atan) v(sinh, std::sinh) v(cosh, std::cosh) v(tanh, std::tanh) v(fabs, std::fabs) \
    v(degrees, toDegrees) v(radians, toRadians)

#define FOR_EACH_MATH_FUNCTION_OF_TWO(v) \
    v(atan2, std::atan2) v(pow, std::pow) v(fmod, std::fmod) v(copysign, std::copysign)

// A function of one float that gives a float.
PYTHON_NATIVE(mathFunction)
{
    static constexpr double (*functions[])(double) = {
#define ENTRY(name, function) function,
        FOR_EACH_MATH_FUNCTION(ENTRY)
#undef ENTRY
    };
    auto function = functions[unpack<unsigned>(callFrame, 0)];
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math function"_s, 1, 1))
        return { };
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    double result = function(*x);
    if (std::isnan(result) && !std::isnan(*x))
        return JSValue::encode(raiseValueError(globalObject, scope, "math domain error"_s));
    if (std::isinf(result) && std::isfinite(*x))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "math range error"_s));
    return JSValue::encode(floatFromDouble(result));
}

PYTHON_NATIVE(mathFunctionOfTwo)
{
    static constexpr double (*functions[])(double, double) = {
#define ENTRY(name, function) function,
        FOR_EACH_MATH_FUNCTION_OF_TWO(ENTRY)
#undef ENTRY
    };
    auto function = functions[unpack<unsigned>(callFrame, 0)];
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math function"_s, 2, 2))
        return { };
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto y = toDouble(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    double result = function(*x, *y);
    if (std::isnan(result) && !std::isnan(*x) && !std::isnan(*y))
        return JSValue::encode(raiseValueError(globalObject, scope, "math domain error"_s));
    return JSValue::encode(floatFromDouble(result));
}

PYTHON_NATIVE(mathSqrt)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math.sqrt"_s, 1, 1))
        return { };
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*x < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, makeString("expected a nonnegative input, got "_s, reprOfDouble(*x))));
    return JSValue::encode(floatFromDouble(std::sqrt(*x)));
}

PYTHON_NATIVE(mathFloorOrCeil)
{
    auto isFloor = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, isFloor ? "math.floor"_s : "math.ceil"_s, 1, 1))
        return { };
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], isFloor ? names.dunder_floor : names.dunder_ceil, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("must be real number, not "_s, typeName(globalObject, args[0]))));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// log(x, base=e)
PYTHON_NATIVE(mathLog)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "log"_s, 1, 2))
        return { };
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*x <= 0)
        return JSValue::encode(raiseValueError(globalObject, scope, makeString("expected a positive input, got "_s, reprOfDouble(*x))));
    double result = std::log(*x);
    if (args.size() > 1) {
        auto base = toDouble(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (*base <= 0)
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("expected a positive input, got "_s, reprOfDouble(*base))));
        result /= std::log(*base);
    }
    return JSValue::encode(floatFromDouble(result));
}

PYTHON_NATIVE(mathClassify)
{
    auto test = unpack<int>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math function"_s, 1, 1))
        return { };
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(!test ? std::isnan(*x) : test == 1 ? std::isinf(*x) : std::isfinite(*x)));
}

PYTHON_NATIVE(mathGcd)
{
    NATIVE_PROLOGUE();
    JSValue result = jsNumber(0);
    for (unsigned i = 0; i < args.size(); ++i) {
        if (!classify(args[i]).isInt())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', typeName(globalObject, args[i]), "' object cannot be interpreted as an integer"_s)));
        JSValue a = result;
        JSValue b = args[i].isBoolean() ? jsNumber(args[i].asBoolean()) : args[i];
        while (true) {
            bool isZero = !isTrue(globalObject, b);
            RETURN_IF_EXCEPTION(scope, { });
            if (isZero)
                break;
            JSValue remainder = binaryOperation(globalObject, BinaryOperator::Mod, false, a, b);
            RETURN_IF_EXCEPTION(scope, { });
            a = b;
            b = remainder;
        }
        Number number = classify(a);
        bool isNegative = number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign();
        result = isNegative ? numberUnaryOperation(globalObject, UnaryOperator::USub, a) : a;
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(mathFactorial)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math.factorial"_s, 1, 1))
        return { };
    auto n = toIndex(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*n < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "factorial() not defined for negative values"_s));
    JSValue result = jsNumber(1);
    for (int64_t i = 2; i <= *n; ++i) {
        result = binaryOperation(globalObject, BinaryOperator::Mult, false, result, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(mathTrunc)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math.trunc"_s, 1, 1))
        return { };
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_trunc, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("type "_s, typeName(globalObject, args[0]), " doesn't define __trunc__ method"_s)));
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

PYTHON_NATIVE(mathHypot)
{
    NATIVE_PROLOGUE();
    double sum = 0;
    for (unsigned i = 0; i < args.size(); ++i) {
        auto x = toDouble(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        sum = std::hypot(sum, *x);
    }
    return JSValue::encode(floatFromDouble(sum));
}

PYTHON_NATIVE(mathIsqrt)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "math.isqrt"_s, 1, 1))
        return { };
    auto n = toIndex(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*n < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "isqrt() argument must be nonnegative"_s));
    int64_t root = static_cast<int64_t>(std::sqrt(static_cast<double>(*n)));
    while (root * root > *n)
        --root;
    while ((root + 1) * (root + 1) <= *n)
        ++root;
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, root)));
}

static PyModule* createMathModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyModule* module = PyModule::create(globalObject, "math"_s);
    JSObject* ns = module->namespaceObject();
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, ns, name, function, data); };
    auto constant = [&] (ASCIILiteral name, double value) { ns->putDirect(vm, Identifier::fromString(vm, name), floatFromDouble(value)); };
    constant("pi"_s, std::numbers::pi);
    constant("e"_s, 2.718281828459045);
    constant("tau"_s, 2 * std::numbers::pi);
    constant("inf"_s, std::numeric_limits<double>::infinity());
    constant("nan"_s, std::numeric_limits<double>::quiet_NaN());
    add("sqrt"_s, mathSqrt);
    add("floor"_s, mathFloorOrCeil, pack(true));
    add("ceil"_s, mathFloorOrCeil, pack(false));
    add("trunc"_s, mathTrunc);
    add("log"_s, mathLog);
    unsigned index = 0;
#define ADD(name, function) add(#name ""_s, mathFunction, index++);
    FOR_EACH_MATH_FUNCTION(ADD)
#undef ADD
    index = 0;
#define ADD(name, function) add(#name ""_s, mathFunctionOfTwo, index++);
    FOR_EACH_MATH_FUNCTION_OF_TWO(ADD)
#undef ADD
    add("hypot"_s, mathHypot);
    add("isnan"_s, mathClassify, pack(0));
    add("isinf"_s, mathClassify, pack(1));
    add("isfinite"_s, mathClassify, pack(2));
    add("gcd"_s, mathGcd);
    add("factorial"_s, mathFactorial);
    add("isqrt"_s, mathIsqrt);
    return module;
}

// ---- time

PYTHON_NATIVE(timeTime)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(floatFromDouble(WallTime::now().secondsSinceEpoch().seconds()));
}

PYTHON_NATIVE(timeMonotonic)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(floatFromDouble(MonotonicTime::now().secondsSinceEpoch().seconds()));
}

PYTHON_NATIVE(timeMonotonicNanoseconds)
{
    UNUSED_PARAM(callFrame);
    return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(MonotonicTime::now().secondsSinceEpoch().nanoseconds())));
}

static PyModule* createTimeModule(JSGlobalObject* globalObject)
{
    PyModule* module = PyModule::create(globalObject, "time"_s);
    JSObject* ns = module->namespaceObject();
    addFunction(globalObject, ns, "time"_s, timeTime);
    addFunction(globalObject, ns, "monotonic"_s, timeMonotonic);
    addFunction(globalObject, ns, "perf_counter"_s, timeMonotonic);
    addFunction(globalObject, ns, "perf_counter_ns"_s, timeMonotonicNanoseconds);
    addFunction(globalObject, ns, "monotonic_ns"_s, timeMonotonicNanoseconds);
    return module;
}

// ---- sys

PYTHON_NATIVE(sysExit)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemExit, args.at(0)));
}

PYTHON_NATIVE(sysException)
{
    UNUSED_PARAM(callFrame);
    JSValue handled = globalObject->pyRealm()->handledException();
    return JSValue::encode(handled ? handled : jsUndefined());
}

PYTHON_NATIVE(sysExcInfo)
{
    UNUSED_PARAM(callFrame);
    JSValue handled = globalObject->pyRealm()->handledException();
    if (!handled)
        return JSValue::encode(PyTuple::create(globalObject, { jsUndefined(), jsUndefined(), jsUndefined() }));
    JSValue traceback = handled.isObject() ? asObject(handled)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_traceback) : JSValue();
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, handled), handled, traceback ? traceback : jsUndefined() }));
}

PYTHON_NATIVE(sysGetRecursionLimit)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(1000));
}

PYTHON_NATIVE(returnNone)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

PYTHON_NATIVE(stdoutWrite)
{
    NATIVE_PROLOGUE();
    if (!args.at(0) || !args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("write() argument must be str, not "_s, typeName(globalObject, args.at(0)))));
    auto view = asString(args[0])->view(globalObject);
    writeToStandardOutput(view);
    return JSValue::encode(jsNumber(stringLength(globalObject, asString(args[0]))));
}

PYTHON_NATIVE(stderrWrite)
{
    NATIVE_PROLOGUE();
    if (!args.at(0) || !args[0].isString())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("write() argument must be str, not "_s, typeName(globalObject, args.at(0)))));
    auto utf8 = asString(args[0])->view(globalObject)->utf8();
    fwrite(utf8.data(), 1, utf8.length(), stderr);
    return JSValue::encode(jsNumber(stringLength(globalObject, asString(args[0]))));
}

PYTHON_NATIVE(stdoutFlush)
{
    UNUSED_PARAM(callFrame);
    flushStandardOutput(globalObject);
    RETURN_NONE();
}

// FIXME: These should be io.TextIOWrapper objects.
static JSObject* createStream(JSGlobalObject* globalObject, NativeFunction write, NativeFunction flush)
{
    PyRealm* realm = globalObject->pyRealm();
    JSObject* stream = PyInstance::create(globalObject->vm(), realm->typeObject()->instanceStructure());
    addFunction(globalObject, stream, "write"_s, write);
    addFunction(globalObject, stream, "flush"_s, flush);
    return stream;
}

static PyModule* createSysModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyModule* module = PyModule::create(globalObject, "sys"_s);
    JSObject* ns = module->namespaceObject();
    auto set = [&] (ASCIILiteral name, JSValue value) { ns->putDirect(vm, Identifier::fromString(vm, name), value); };
    set("modules"_s, realm->modules());
    set("path"_s, newList(globalObject));
    set("argv"_s, newList(globalObject));
    set("version"_s, jsNontrivialString(vm, "3.14.0 (JavaScriptCore)"_s));
    set("version_info"_s, PyTuple::create(globalObject, { jsNumber(3), jsNumber(14), jsNumber(0), jsNontrivialString(vm, "final"_s), jsNumber(0) }));
    set("hexversion"_s, jsNumber(0x030E00F0));
    set("maxsize"_s, intFromInt64(globalObject, std::numeric_limits<int64_t>::max()));
    set("maxunicode"_s, jsNumber(0x10FFFF));
    set("byteorder"_s, jsNontrivialString(vm, "little"_s));
#if OS(DARWIN)
    set("platform"_s, jsNontrivialString(vm, "darwin"_s));
#elif OS(WINDOWS)
    set("platform"_s, jsNontrivialString(vm, "win32"_s));
#else
    set("platform"_s, jsNontrivialString(vm, "linux"_s));
#endif
    set("stdout"_s, createStream(globalObject, stdoutWrite, stdoutFlush));
    set("stderr"_s, createStream(globalObject, stderrWrite, returnNone));
    addFrameFunctions(globalObject, ns);
    addFunction(globalObject, ns, "exit"_s, sysExit);
    addFunction(globalObject, ns, "exception"_s, sysException);
    addFunction(globalObject, ns, "exc_info"_s, sysExcInfo);
    addFunction(globalObject, ns, "getrecursionlimit"_s, sysGetRecursionLimit);
    addFunction(globalObject, ns, "setrecursionlimit"_s, returnNone);
    return module;
}

static PyModule* createBuiltinsModule(JSGlobalObject* globalObject)
{
    // FIXME: It should be the namespace itself, so that what is set here is seen everywhere.
    VM& vm = globalObject->vm();
    PyModule* module = PyModule::create(globalObject, "builtins"_s);
    JSObject* builtins = globalObject->pyRealm()->builtinsNamespace();
    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    builtins->getOwnNonIndexPropertyNames(globalObject, properties, DontEnumPropertiesMode::Exclude);
    for (auto& name : properties)
        module->namespaceObject()->putDirect(vm, name, builtins->getDirect(vm, name));
    return module;
}

static PyModule* createNativeModule(JSGlobalObject* globalObject, const String& name)
{
    if (name == "sys"_s)
        return createSysModule(globalObject);
    if (name == "math"_s)
        return createMathModule(globalObject);
    if (name == "time"_s)
        return createTimeModule(globalObject);
    if (name == "builtins"_s)
        return createBuiltinsModule(globalObject);
    return nullptr;
}

// import js: JavaScript's global object, as it is.
static JSValue createJavaScriptModule(JSGlobalObject* globalObject, const String& name)
{
    return name == "js"_s ? JSValue(globalObject->globalThis()) : JSValue();
}

// ---- The modules that are written in Python and come with the engine

struct LibrarySource {
    ASCIILiteral name;
    std::span<const unsigned char> source;
};

#include "PythonLibrarySources.h"

static std::optional<String> librarySourceFor(const String& name)
{
    for (auto& library : s_librarySources) {
        if (name == library.name)
            return String::fromUTF8(byteCast<char8_t>(library.source));
    }
    return std::nullopt;
}

// ---- import

static JSValue sysAttribute(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    JSValue sys = modulesOf(globalObject)->getString(globalObject, "sys"_s);
    if (!sys) {
        sys = createSysModule(globalObject);
        registerModule(globalObject, "sys"_s, sys);
    }
    return uncheckedDowncast<PyModule>(sys.asCell())->namespaceObject()->getDirect(vm, Identifier::fromString(vm, name));
}

// Runs the source of a module. Empty if it raised.
static JSValue loadSourceModule(JSGlobalObject* globalObject, const String& fullName, const String& path, const String& source, bool isPackage, ImplementationVisibility visibility = ImplementationVisibility::Public)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    PyModule* module = PyModule::create(globalObject, fullName);
    JSObject* ns = module->namespaceObject();
    ns->putDirect(vm, names.dunder_file, jsString(vm, path));
    size_t lastDot = fullName.reverseFind('.');
    if (isPackage) {
        MarkedArgumentBuffer directories;
        directories.append(jsString(vm, path.left(path.reverseFind('/'))));
        ns->putDirect(vm, names.dunder_path, newList(globalObject, directories));
        ns->putDirect(vm, names.dunder_package, jsString(vm, fullName));
    } else
        ns->putDirect(vm, names.dunder_package, lastDot == notFound ? jsEmptyString(vm) : jsString(vm, fullName.left(lastDot)));

    // It is there to be found while it runs, in case what it imports imports it.
    registerModule(globalObject, fullName, module);

    auto provider = StringSourceProvider::create(source, SourceOrigin(), String(path), SourceTaintedOrigin::Untainted);
    provider->setLanguage(SourceLanguage::Python);
    JSFunction* function = compileModule(globalObject, SourceCode(WTF::move(provider)), ns, visibility);
    if (function)
        call(globalObject, function);
    if (scope.exception()) {
        // Comparing strings cannot raise, so this is safe with an exception pending.
        return { };
    }
    return module;
}

// The module with the full name, from sys.modules or by loading it. Empty, with nothing raised, if there is no such module.
static JSValue findOrLoad(JSGlobalObject* globalObject, const String& fullName, JSValue parent)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue module = modulesOf(globalObject)->getString(globalObject, fullName))
        return module;

    if (!parent) {
        if (PyModule* module = createNativeModule(globalObject, fullName)) {
            registerModule(globalObject, fullName, module);
            return module;
        }
        if (JSValue module = createJavaScriptModule(globalObject, fullName)) {
            registerModule(globalObject, fullName, module);
            return module;
        }
        if (auto source = librarySourceFor(fullName))
            RELEASE_AND_RETURN(scope, loadSourceModule(globalObject, fullName, makeString("<frozen "_s, fullName, '>'), *source, false, ImplementationVisibility::Private));
    }
    if (!s_sourceReader)
        return { };

    JSValue searchPath;
    if (parent) {
        searchPath = getAttributeIfPresent(globalObject, parent, vm.pythonNames().dunder_path);
        RETURN_IF_EXCEPTION(scope, { });
        if (!searchPath)
            return { };
    } else
        searchPath = sysAttribute(globalObject, "path"_s);

    size_t lastDot = fullName.reverseFind('.');
    String leaf = lastDot == notFound ? fullName : fullName.substring(lastDot + 1);
    MarkedArgumentBuffer directories;
    collect(globalObject, searchPath, directories);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < directories.size(); ++i) {
        if (!directories.at(i).isString())
            continue;
        String directory = asString(directories.at(i))->value(globalObject);
        if (directory.isEmpty())
            directory = "."_s;
        String source;
        String path = makeString(directory, '/', leaf, "/__init__.py"_s);
        bool isPackage = s_sourceReader(path, source);
        if (!isPackage) {
            path = makeString(directory, '/', leaf, ".py"_s);
            if (!s_sourceReader(path, source))
                continue;
        }
        JSValue module = loadSourceModule(globalObject, fullName, path, source, isPackage);
        if (!module) {
            // A module that failed to load is as if it had never been.
            Exception* exception = scope.exception();
            if (exception && scope.tryClearException()) {
                modulesOf(globalObject)->remove(globalObject, jsString(vm, fullName));
                throwException(globalObject, scope, exception);
            }
            return { };
        }
        // What is in sys.modules now is the module, whatever that is.
        if (JSValue registered = modulesOf(globalObject)->getString(globalObject, fullName))
            module = registered;
        if (parent) {
            setAttribute(globalObject, parent, Identifier::fromString(vm, leaf), module);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return module;
    }
    return { };
}

void initializeLibrary(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto attribute = [&] (JSValue module, ASCIILiteral name) {
        return uncheckedDowncast<PyModule>(module.asCell())->namespaceObject()->getDirect(vm, Identifier::fromString(vm, name));
    };
    JSValue groups = findOrLoad(globalObject, "_exceptiongroup"_s, JSValue());
    RELEASE_ASSERT(groups);
    for (ASCIILiteral name : { "BaseExceptionGroup"_s, "ExceptionGroup"_s })
        realm->builtinsNamespace()->putDirect(vm, Identifier::fromString(vm, name), attribute(groups, name));
    // Compiled code calls these as it calls what is written in C++.
    realm->runtimeFunctions()->putDirect(vm, Identifier::fromString(vm, "matchExceptionGroup"_s), attribute(groups, "match_exception_group"_s));
    realm->runtimeFunctions()->putDirect(vm, Identifier::fromString(vm, "prepareReraiseStar"_s), attribute(groups, "prepare_reraise_star"_s));
}

JSValue importModule(JSGlobalObject* globalObject, JSObject* globals, const String& givenName, JSValue fromList, unsigned level, bool wantsLeaf)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();

    String name = givenName;
    if (level) {
        // from . import x, from ..package import y: relative to the package that this module is in.
        JSValue packageValue = globals ? globals->getDirect(vm, names.dunder_package) : JSValue();
        String package;
        if (packageValue && packageValue.isString())
            package = asString(packageValue)->value(globalObject);
        else if (JSValue moduleName = globals ? globals->getDirect(vm, names.dunder_name) : JSValue(); moduleName && moduleName.isString()) {
            package = asString(moduleName)->value(globalObject);
            if (!globals->getDirect(vm, names.dunder_path)) {
                size_t dot = package.reverseFind('.');
                package = dot == notFound ? emptyString() : package.left(dot);
            }
        }
        if (package.isEmpty())
            return raise(globalObject, scope, BuiltinType::ImportError, "attempted relative import with no known parent package"_s);
        for (unsigned i = 1; i < level; ++i) {
            size_t dot = package.reverseFind('.');
            if (dot == notFound)
                return raise(globalObject, scope, BuiltinType::ImportError, "attempted relative import beyond top-level package"_s);
            package = package.left(dot);
        }
        name = name.isEmpty() ? package : makeString(package, '.', name);
    } else if (name.isEmpty())
        return raiseValueError(globalObject, scope, "Empty module name"_s);

    // a, then a.b, then a.b.c.
    JSValue first;
    JSValue module;
    size_t position = 0;
    while (true) {
        size_t dot = name.find('.', position);
        String fullName = dot == notFound ? name : name.left(dot);
        JSValue next = findOrLoad(globalObject, fullName, module);
        RETURN_IF_EXCEPTION(scope, { });
        if (!next) {
            JSObject* error = createException(globalObject, globalObject->pyRealm()->typeModuleNotFoundError(), module && !getAttributeIfPresent(globalObject, module, names.dunder_path)
                ? makeString("No module named '"_s, fullName, "'; '"_s, name.left(position - 1), "' is not a package"_s)
                : makeString("No module named '"_s, fullName, '\''));
            error->putDirect(vm, Identifier::fromString(vm, "name"_s), jsString(vm, fullName));
            throwException(globalObject, scope, error);
            return { };
        }
        module = next;
        if (!first)
            first = module;
        if (dot == notFound)
            break;
        position = dot + 1;
    }

    bool hasFromList = !isNone(fromList) && isTrue(globalObject, fromList);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasFromList) {
        if (wantsLeaf)
            return module;
        if (!level)
            return first;
        // import of a relative name cannot be written, but __import__ can be asked for it.
        return module;
    }

    // from package import module: what is named may be a module of the package that has not been loaded.
    JSValue path = getAttributeIfPresent(globalObject, module, names.dunder_path);
    RETURN_IF_EXCEPTION(scope, { });
    if (path) {
        MarkedArgumentBuffer wanted;
        collect(globalObject, fromList, wanted);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 0; i < wanted.size(); ++i) {
            if (!wanted.at(i).isString())
                continue;
            String item = asString(wanted.at(i))->value(globalObject);
            if (item == "*"_s)
                continue;
            JSValue present = getAttributeIfPresent(globalObject, module, Identifier::fromString(vm, item));
            RETURN_IF_EXCEPTION(scope, { });
            if (present)
                continue;
            findOrLoad(globalObject, makeString(name, '.', item), module);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return module;
}

} } // namespace JSC::Python
