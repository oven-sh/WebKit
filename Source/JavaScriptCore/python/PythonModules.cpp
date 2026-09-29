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

#include "GlobalObjectMethodTable.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonCompiler.h"
#include "PythonConfiguration.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonLifecycle.h"
#include "SourceProvider.h"
#include <wtf/MonotonicTime.h>
#include <wtf/WallTime.h>

// Modules, and some of those that are written in C++.

namespace JSC { namespace Python {

static PyDict* modulesOf(JSGlobalObject* globalObject) { return uncheckedDowncast<PyDict>(globalObject->pyRealm()->modules()); }

JSObject* newModule(JSGlobalObject* globalObject, const String& name, PyType* type)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    JSObject* module = PyInstance::create(vm, (type ? type : globalObject->pyRealm()->typeModule())->instanceStructure());
    module->putDirect(vm, names.dunder_name, jsString(vm, name));
    module->putDirect(vm, names.dunder_doc, jsUndefined());
    module->putDirect(vm, names.dunder_package, jsUndefined());
    module->putDirect(vm, names.dunder_loader, jsUndefined());
    module->putDirect(vm, names.dunder_spec, jsUndefined());
    return module;
}

JSObject* newBuiltinModule(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    JSObject* module = newModule(globalObject, String(name));
    if (auto* description = findModuleDescription(name); description && !description->doc.isNull())
        module->putDirect(vm, vm.pythonNames().dunder_doc, jsString(vm, String(description->doc)));
    return module;
}

JSObject* tryModule(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != PyInstanceType)
        return nullptr;
    return typeOf(globalObject, value)->isSubtypeOf(globalObject->pyRealm()->typeModule()) ? asObject(value) : nullptr;
}

// PyMapping_GetOptionalItem(), of the dict that an object is the properties of. Empty if it is not there, or if it raised.
static JSValue getOptionalItem(JSGlobalObject* globalObject, JSObject* namespaceObject, PropertyName name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = getItem(globalObject, PyDict::backedBy(globalObject, namespaceObject), jsString(vm, String(name.uid())));
    if (scope.exception()) [[unlikely]] {
        catchException(globalObject, BuiltinType::KeyError);
        return { };
    }
    return value;
}

JSValue loadGlobal(JSGlobalObject* globalObject, JSObject* globals, JSObject* builtins, PropertyName name, GlobalLocation& location, GlobalsAre globalsAre)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isOfDerivedClass = [&] (JSObject* object) { return !!object->getDirect(vm, vm.pythonNames().private_isDictOfDerivedClass); };
    // _PyEval_LoadGlobalStackRef(): a dict of a class that a program has derived is asked as anything is asked for an item, so that its __getitem__() and __missing__() have their say. Nothing is remembered of that.
    // annotationlib depends on it: it runs what works out annotations again with globals that make something up for whatever is not there.
    if (globalsAre == GlobalsAre::AskedAsAMapping && isOfDerivedClass(globals)) [[unlikely]] {
        JSValue value = getOptionalItem(globalObject, globals, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value) {
            value = getOptionalItem(globalObject, builtins, name);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!value)
            return raiseNameError(globalObject, scope, String(name.uid()));
        return value;
    }
    // What is not enumerable is no variable: see getStoredAttribute().
    auto find = [&] (JSObject* object, PropertyOffset& offset) -> JSValue {
        unsigned attributes;
        offset = object->structure()->get(vm, name, attributes);
        if (!isValidOffset(offset) || (attributes & PropertyAttribute::DontEnum))
            return { };
        return object->getDirect(offset);
    };

    PropertyOffset offset;
    Structure* globalsStructure = globals->structure();
    if (JSValue value = find(globals, offset)) {
        if (globalsStructure->propertyAccessesAreCacheable())
            location = { globalsStructure, nullptr, offset };
        return value;
    }
    if (isOfDerivedClass(builtins)) [[unlikely]] {
        JSValue value = getOptionalItem(globalObject, builtins, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            return raiseNameError(globalObject, scope, String(name.uid()));
        return value;
    }
    if (JSValue value = find(builtins, offset)) {
        // That it is not among the globals goes with their structure, unless that is a dictionary, which can be added to and stay the same. One that has
        // been added to a great deal is likely to have settled down, so it is given one chance to be made an ordinary structure again.
        if (globalsStructure->isDictionary() && !globalsStructure->hasBeenFlattenedBefore())
            globalsStructure = globalsStructure->flattenDictionaryStructure(vm, globals);
        Structure* builtinsStructure = builtins->structure();
        if (!globalsStructure->isDictionary() && builtinsStructure->propertyAccessesAreCacheable())
            location = { globalsStructure, builtinsStructure, offset };
        return value;
    }
    return raiseNameError(globalObject, scope, String(name.uid()));
}

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
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*x < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("expected a nonnegative input, got "_s, reprOfDouble(*x))));
    return JSValue::encode(floatFromDouble(std::sqrt(*x)));
}

PYTHON_NATIVE(mathFloorOrCeil)
{
    auto isFloor = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue self;
    const Identifier& name = isFloor ? names.dunder_floor : names.dunder_ceil;
    JSValue method = lookupSpecial(globalObject, args[0], name, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method) {
        // Whatever can be made a float of is one, for this.
        auto value = toDouble(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        method = lookupSpecial(globalObject, floatFromDouble(*value), name, self);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
}

// log(x, base=e)
PYTHON_NATIVE(mathLog)
{
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*x <= 0)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("expected a positive input, got "_s, reprOfDouble(*x))));
    double result = std::log(*x);
    if (args.size() > 1) {
        auto base = toDouble(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (*base <= 0)
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("expected a positive input, got "_s, reprOfDouble(*base))));
        result /= std::log(*base);
    }
    return JSValue::encode(floatFromDouble(result));
}

PYTHON_NATIVE(mathClassify)
{
    auto test = unpack<int>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto x = toDouble(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(!test ? std::isnan(*x) : test == 1 ? std::isinf(*x) : std::isfinite(*x)));
}

PYTHON_NATIVE(mathGcd)
{
    NATIVE_PROLOGUE();
    JSValue result = jsNumber(0);
    for (unsigned i = 0; i < args.size(); ++i) {
        JSValue a = result;
        JSValue b = toInt(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
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
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[0], names.dunder_trunc, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!method)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("type "_s, typeName(globalObject, args[0]), " doesn't define __trunc__ method"_s)));
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

JSObject* createMathModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "math"_s);
    JSObject* ns = module;
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
    addFunction(globalObject, ns, "log"_s, mathLog, 0, "($module, x, base=None, /)"_s);
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

JSObject* createTimeModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "time"_s);
    JSObject* ns = module;
    addFunction(globalObject, ns, "time"_s, timeTime);
    addFunction(globalObject, ns, "monotonic"_s, timeMonotonic);
    addFunction(globalObject, ns, "perf_counter"_s, timeMonotonic);
    addFunction(globalObject, ns, "perf_counter_ns"_s, timeMonotonicNanoseconds);
    addFunction(globalObject, ns, "monotonic_ns"_s, timeMonotonicNanoseconds);
    return module;
}

JSValue sysAttribute(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    return getStoredAttribute(vm, globalObject->pyRealm()->sysModule(), Identifier::fromString(vm, name));
}

// The source in a file, for showing a line of it. Null if there is no reading it, whatever the matter is: whoever asks has something else to say.
SourceCode readSourceIfPresent(JSGlobalObject* globalObject, const String& path)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    const FileOperations* files = fileOperations(globalObject);
    if (!files)
        return { };
    int descriptor = files->open(path.utf8(), O_RDONLY | O_CLOEXEC, 0);
    if (descriptor < 0)
        return { };
    Vector<uint8_t> bytes;
    std::array<uint8_t, 64 * KB> chunk;
    int64_t count;
    while ((count = files->read(descriptor, chunk)) > 0 || count == -EINTR) {
        if (count > 0)
            bytes.append(std::span(chunk).first(static_cast<size_t>(count)));
    }
    files->close(descriptor);
    if (count < 0)
        return { };
    SourceCode source = makeSource(globalObject, bytes.span(), SourceOrigin(), path);
    if (scope.exception()) [[unlikely]] {
        scope.tryClearException();
        return { };
    }
    return source;
}

// ---- A module that is asked for by where it is, and not by name: `import ... from "./module.py"` in JavaScript

// To Python a module is known by its name, and there is one of each name. So a file is given the name that `import` would find it by: where it
// is from the first directory of sys.path that it is in. If it is in none, `import` cannot find it, and it goes by the name of the file.
static String moduleNameForPath(JSGlobalObject* globalObject, const String& path, bool& isOnSearchPath)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    isOnSearchPath = false;
    String stem = path.endsWith(".py"_s) ? path.left(path.length() - 3) : path;
    if (stem.endsWith("/__init__"_s))
        stem = stem.left(stem.length() - 9);

    MarkedArgumentBuffer directories;
    collect(globalObject, sysAttribute(globalObject, "path"_s), directories);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < directories.size(); ++i) {
        if (!directories.at(i).isString())
            continue;
        String directory = asString(directories.at(i))->value(globalObject);
        if (directory.isEmpty() || !stem.startsWith(directory))
            continue;
        StringView rest = StringView(stem).substring(directory.length());
        if (!directory.endsWith('/')) {
            if (!rest.startsWith('/'))
                continue;
            rest = rest.substring(1);
        }
        if (!rest.isEmpty() && !rest.contains('.')) {
            isOnSearchPath = true;
            return makeStringByReplacingAll(rest.toString(), '/', '.');
        }
    }
    size_t slash = stem.reverseFind('/');
    return slash == notFound ? stem : stem.substring(slash + 1);
}

JSValue importModuleFromSource(JSGlobalObject* globalObject, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    startPython(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    const String& path = source.provider()->sourceURL();
    bool isOnSearchPath;
    String fullName = moduleNameForPath(globalObject, path, isOnSearchPath);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = jsString(vm, fullName);
    JSValue module = getImportedModule(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (module)
        return module;
    if (isOnSearchPath)
        RELEASE_AND_RETURN(scope, importModule(globalObject, fullName));
    // importlib.util.spec_from_file_location(), and then what `import` does with a spec.
    JSValue importlib = globalObject->pyRealm()->importState().importlib.get();
    JSValue external = getAttribute(globalObject, importlib, Identifier::fromString(vm, "_bootstrap_external"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue spec = callMethodNamed(globalObject, external, Identifier::fromString(vm, "spec_from_file_location"_s), name, jsString(vm, path));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethodNamed(globalObject, importlib, Identifier::fromString(vm, "_load"_s), spec));
}

void exportModule(JSGlobalObject* globalObject, const SourceCode& source, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModuleFromSource(globalObject, source);
    RETURN_IF_EXCEPTION(scope, void());
    exportNames.append(vm.propertyNames->defaultKeyword);
    exportValues.append(module);
    if (!module.isObject())
        return;
    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    asObject(module)->methodTable()->getOwnPropertyNames(asObject(module), globalObject, properties, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, void());
    for (auto& name : properties) {
        // A variable that is called `default` is to be had from the module.
        if (name == vm.propertyNames->defaultKeyword)
            continue;
        JSValue value = asObject(module)->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, void());
        exportNames.append(name);
        exportValues.append(value);
    }
}

void initializeLibrary(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSValue frameLocals = importModule(globalObject, "_framelocals"_s);
    RELEASE_ASSERT(frameLocals);
    auto* proxy = asType(asObject(frameLocals)->getDirect(vm, Identifier::fromString(vm, "FrameLocalsProxy"_s)));
    proxy->setFlag(PyType::IsMapping);
    realm->setFrameLocalsProxyType(vm, proxy);
}

} } // namespace JSC::Python
