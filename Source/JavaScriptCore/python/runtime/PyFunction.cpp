/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyFunction.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "JSCInlines.h"
#include "PreciseJumpTargetsInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyCodeCache.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// Functions, bound methods, cells and code objects; and what functions written in C++ look like to Python.
// (Objects/funcobject.c, classobject.c, cellobject.c, codeobject.c, methodobject.c and descrobject.c in CPython are the reference.)

PY_DEFINE_CELL(PyCell);
PY_DEFINE_CELL(PyMethod);
PY_DEFINE_CELL(PyCode);
const ClassInfo PyFunction::s_info = { "PyFunction"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyFunction) };

// ---- Cells ----

PyCell::PyCell(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCell* PyCell::create(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* cell = new (NotNull, allocateCell<PyCell>(vm)) PyCell(vm, rt.structure<PyCell>(), rt.cellType());
    cell->finishCreation(vm);
    cell->m_value.set(vm, cell, value);
    return cell;
}

template<typename Visitor>
void PyCell::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCell>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_value);
}

DEFINE_VISIT_CHILDREN(PyCell);

static JSValue cellNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "cell"_s, args)) || failed(scope, checkPositional(globalObject, "cell"_s, args, 0, 1)))
        return { };
    RELEASE_AND_RETURN(scope, PyCell::create(globalObject, args.get(0)));
}

static JSValue cellRepr(JSGlobalObject* globalObject, JSValue self)
{
    JSValue value = PyCell::from(self)->value();
    if (!value)
        return makeStr(globalObject, makeString("<cell at "_s, addressForRepr(globalObject, self), ": empty>"_s));
    return makeStr(globalObject, makeString("<cell at "_s, addressForRepr(globalObject, self), ": "_s, typeName(globalObject, value), " object at "_s, addressForRepr(globalObject, value), '>'));
}

template<CompareOperator op>
static JSValue cellCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!PyCell::is(other))
        return runtime(globalObject).notImplemented();
    JSValue a = PyCell::from(self)->value();
    JSValue b = PyCell::from(other)->value();
    if (a && b)
        return richCompare(globalObject, a, b, op);
    // An empty cell comes before any other.
    int x = !!a;
    int y = !!b;
    switch (op) {
    case CompareOperator::Eq:
        return jsBoolean(x == y);
    case CompareOperator::NotEq:
        return jsBoolean(x != y);
    case CompareOperator::Lt:
        return jsBoolean(x < y);
    case CompareOperator::LtE:
        return jsBoolean(x <= y);
    case CompareOperator::Gt:
        return jsBoolean(x > y);
    default:
        return jsBoolean(x >= y);
    }
}

static JSValue cellGetContents(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue value = PyCell::from(self)->value())
        return value;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "Cell is empty"_s);
}

static void cellSetContents(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyCell::from(self)->setValue(getVM(globalObject), value);
}

static constexpr NativeDef cellMethods[] = {
    PY_NEW(cellNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", cellRepr, "($self, /)"),
    PY_SLOT("__eq__", cellCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", cellCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", cellCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", cellCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", cellCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", cellCompare<CompareOperator::GtE>, "($self, value, /)"),
};

static constexpr NativeGetSet cellGetSets[] = {
    { "cell_contents"_s, cellGetContents, cellSetContents },
};

// ---- Functions ----

PyFunction::PyFunction(VM& vm, FunctionExecutable* executable, JSGlobalObject* globalObject, Structure* structure)
    : Base(vm, executable, globalObject, structure)
    , m_info(executable->unlinkedExecutable()->pythonCodeInfo())
{
    ASSERT(m_info);
}

PY_DEFINE_JS_VIEW(PyFunction)

Structure* PyFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags), info());
}

JSValue currentBuiltins(JSGlobalObject* globalObject)
{
    if (PyFunction* function = currentFunction(globalObject))
        return function->builtinsObject();
    return runtime(globalObject).builtins();
}

static JSValue builtinsFromGlobals(JSGlobalObject* globalObject, PyDict* globals)
{
    JSValue builtins = globals->get(runtime(globalObject).names.dBuiltins());
    if (!builtins)
        return currentBuiltins(globalObject);
    if (auto* module = PyModule::tryFrom(builtins))
        return module->dict();
    return builtins;
}

static JSValue getOptionalItem(JSGlobalObject* globalObject, JSValue mapping, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    if (auto* dict = PyDict::tryFrom(mapping); dict && isExactDict(rt, dict)) [[likely]]
        return dict->get(name);
    JSValue value = getItem(globalObject, mapping, name);
    if (failed(scope, value))
        clearPendingExceptionIfMatches(globalObject, rt.KeyError());
    return value;
}

JSValue PyFunction::lookupBuiltin(JSGlobalObject* globalObject, JSString* name)
{
    return getOptionalItem(globalObject, builtinsObject(), name);
}

JSValue currentBuiltin(JSGlobalObject* globalObject, JSString* name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A key need not be a str to equal one.
    JSValue builtins = currentBuiltins(globalObject);
    auto* dict = PyDict::tryFrom(builtins);
    JSValue value = dict && isExactDict(runtime(globalObject), dict) ? dict->get(globalObject, name) : getOptionalItem(globalObject, builtins, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return raise(globalObject, scope, runtime(globalObject).AttributeError(), stringOf(name));
    return value;
}

PyFunction* PyFunction::create(JSGlobalObject* globalObject, FunctionExecutable* executable, PyDict* globals)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // The optimizing JITs know nothing of Python's instructions: JS code that calls the function must not try to inline it.
    executable->setNeverInline(true);
    executable->setNeverOptimize(true);
    auto* function = new (NotNull, allocateCell<PyFunction>(vm)) PyFunction(vm, executable, globalObject, rt.functionStructure());
    function->finishCreation(vm);
    CodeInfo& info = function->codeInfo();
    function->m_globals.set(vm, function, globals);
    JSValue builtins = builtinsFromGlobals(globalObject, globals);
    if (auto* dict = PyDict::tryFrom(builtins)) [[likely]] {
        function->m_builtins.set(vm, function, dict);
        function->m_hasUnusualNamespaces = !isExactDict(rt, dict) || !isExactDict(rt, globals);
    } else {
        function->m_builtins.set(vm, function, PyDict::create(globalObject));
        function->m_builtinsObject.set(vm, function, builtins);
        function->m_hasUnusualNamespaces = true;
    }
    function->m_name.set(vm, function, jsString(vm, info.name.string().string()));
    function->m_qualname.set(vm, function, makeStr(vm, info.qualname));
    function->m_doc.set(vm, function, info.docstring.isNull() ? jsUndefined() : JSValue(makeStr(vm, info.docstring)));
    if (JSValue module = globals->get(rt.names.dName()))
        function->m_module.set(vm, function, module);
    return function;
}

template<typename Visitor>
void PyFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyFunction>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_globals);
    visitor.append(thisObject->m_builtins);
    visitor.append(thisObject->m_builtinsObject);
    visitor.append(thisObject->m_closure);
    visitor.append(thisObject->m_defaults);
    visitor.append(thisObject->m_kwDefaults);
    visitor.append(thisObject->m_dict);
    visitor.append(thisObject->m_name);
    visitor.append(thisObject->m_qualname);
    visitor.append(thisObject->m_doc);
    visitor.append(thisObject->m_module);
    visitor.append(thisObject->m_annotations);
    visitor.append(thisObject->m_annotate);
    visitor.append(thisObject->m_typeParams);
    visitor.append(thisObject->m_code);
    visitor.append(thisObject->m_generatorBody);
    visitor.append(thisObject->m_generatorOwner);
}

DEFINE_VISIT_CHILDREN(PyFunction);

// JSC takes a function to keep the executable it was made with, and there is no setter. Who relies on it:
// - call sites that are linked to the function jump to its code without looking: they are unlinked;
// - code from the optimizing compilers that has the function for a constant may call the executable it had then: it is thrown away.
//   (They never look into code written in Python, so that is all they can know about it.)
// A call in progress goes on in the old code, which its frame keeps.
void PyFunction::setCode(VM& vm, PyCode* code)
{
    FunctionExecutable* previous = functionExecutable();
    FunctionExecutable* next = code->executable();
    m_code.set(vm, this, code);
    if (next == previous)
        return;

    next->setNeverInline(true);
    next->setNeverOptimize(true);
    if (FunctionRareData* rareData = this->rareData())
        reinterpret_cast<WriteBarrier<ExecutableBase>*>(reinterpret_cast<char*>(rareData) + FunctionRareData::offsetOfExecutable())->set(vm, rareData, next);
    else {
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<char*>(this) + offsetOfExecutableOrRareData()) = std::bit_cast<uintptr_t>(next);
        vm.writeBarrier(this, next);
    }
    ASSERT(functionExecutable() == next);
    m_info = next->unlinkedExecutable()->pythonCodeInfo();
    m_generatorBody.clear();

    if (CodeBlock* codeBlock = previous->codeBlockForCall())
        codeBlock->unlinkOrUpgradeIncomingCalls(vm, nullptr);
#if ENABLE(DFG_JIT)
    vm.heap.completeAllJITPlans();
    Vector<CodeBlock*> optimized;
    vm.heap.forEachCodeBlock([&](CodeBlock* codeBlock) {
        if (JITCode::isOptimizingJIT(codeBlock->jitType()))
            optimized.append(codeBlock);
    });
    for (CodeBlock* codeBlock : optimized)
        codeBlock->jettison(Profiler::JettisonDueToUnprofiledWatchpoint);
#endif
}

PyFunction* PyFunction::generatorBody(JSGlobalObject* globalObject, CodeBlock* ownCodeBlock)
{
    if (PyFunction* body = m_generatorBody.get())
        return body;
    VM& vm = getVM(globalObject);
    PyFunction* body = create(globalObject, ownCodeBlock->functionExpr(CodeInfo::generatorBodyIndex), globals());
    body->setClosure(vm, closure());
    body->m_generatorOwner.set(vm, body, this);
    m_generatorBody.set(vm, this, body);
    return body;
}

// ---- Binding the arguments of a call ----

static String qualnameForMessage(JSGlobalObject* globalObject, PyFunction* function)
{
    JSValue qualname = function->qualname();
    if (qualname.isString()) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
        String text = asString(qualname)->value(globalObject);
        if (!scope.exception()) [[likely]]
            return text;
        scope.clearException();
    }
    return function->codeInfo().qualname;
}

// The name among the candidates that is closest to the one given, if any is close.
String calculateSuggestion(const Vector<String>& candidates, const String& name)
{
    static constexpr size_t maxCandidates = 750;
    static constexpr size_t maxStringSize = 40;
    static constexpr size_t moveCost = 2;
    static constexpr size_t caseCost = 1;
    if (candidates.size() >= maxCandidates)
        return { };

    auto substitutionCost = [](char a, char b) -> size_t {
        if ((a & 31) != (b & 31))
            return moveCost;
        if (a == b)
            return 0;
        return toASCIILower(a) == toASCIILower(b) ? caseCost : moveCost;
    };
    size_t buffer[maxStringSize];
    auto distanceBetween = [&](std::span<const char> a, std::span<const char> b, size_t maxCost) -> size_t {
        while (!a.empty() && !b.empty() && a.front() == b.front()) {
            a = a.subspan(1);
            b = b.subspan(1);
        }
        while (!a.empty() && !b.empty() && a.back() == b.back()) {
            a = a.first(a.size() - 1);
            b = b.first(b.size() - 1);
        }
        if (a.empty() || b.empty())
            return (a.size() + b.size()) * moveCost;
        if (a.size() > maxStringSize || b.size() > maxStringSize)
            return maxCost + 1;
        if (b.size() < a.size())
            std::swap(a, b);
        if ((b.size() - a.size()) * moveCost > maxCost)
            return maxCost + 1;
        for (size_t i = 0; i < a.size(); ++i)
            buffer[i] = (i + 1) * moveCost;
        size_t result = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            size_t distance = result = j * moveCost;
            size_t minimum = std::numeric_limits<size_t>::max();
            for (size_t i = 0; i < a.size(); ++i) {
                size_t substitute = distance + substitutionCost(b[j], a[i]);
                distance = buffer[i];
                result = std::min(std::min(result, distance) + moveCost, substitute);
                buffer[i] = result;
                minimum = std::min(minimum, result);
            }
            if (minimum > maxCost)
                return maxCost + 1;
        }
        return result;
    };

    CString nameUTF8 = name.utf8();
    size_t bestDistance = std::numeric_limits<size_t>::max();
    String best;
    for (auto& candidate : candidates) {
        if (candidate == name)
            continue;
        CString candidateUTF8 = candidate.utf8();
        size_t maxDistance = (nameUTF8.length() + candidateUTF8.length() + 3) * moveCost / 6;
        maxDistance = std::min(maxDistance, bestDistance - 1);
        size_t distance = distanceBetween(byteCast<char>(nameUTF8.span()), byteCast<char>(candidateUTF8.span()), maxDistance);
        if (distance > maxDistance)
            continue;
        if (best.isNull() || distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

static NEVER_INLINE void raiseMissingArguments(JSGlobalObject* globalObject, PyFunction* function, const JSValue* slots, unsigned start, unsigned end, ASCIILiteral kind)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodeInfo& info = function->codeInfo();
    Vector<String, 4> names;
    for (unsigned i = start; i < end; ++i) {
        if (!slots[i])
            names.append(strRepr(info.parameterNames[i].string()));
    }
    StrBuilder list;
    size_t count = names.size();
    if (count == 1)
        list.append(names[0]);
    else if (count == 2)
        list.append(names[0], " and "_s, names[1]);
    else {
        for (size_t i = 0; i + 2 < count; ++i) {
            if (i)
                list.append(", "_s);
            list.append(names[i]);
        }
        list.append(", "_s, names[count - 2], ", and "_s, names[count - 1]);
    }
    raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() missing "_s, count, " required "_s, kind, " argument"_s, count == 1 ? ""_s : "s"_s, ": "_s, list.toString());
}

static NEVER_INLINE void raiseTooManyPositional(JSGlobalObject* globalObject, PyFunction* function, const JSValue* slots, unsigned given)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodeInfo& info = function->codeInfo();
    unsigned keywordOnlyGiven = 0;
    for (unsigned i = info.argCount; i < info.argCount + info.kwOnlyCount; ++i)
        keywordOnlyGiven += !!slots[i];
    unsigned defaultCount = function->defaults() ? function->defaults()->length() : 0;
    bool plural;
    String signature;
    if (defaultCount) {
        plural = true;
        signature = makeString("from "_s, static_cast<int>(info.argCount) - static_cast<int>(defaultCount), " to "_s, info.argCount);
    } else {
        plural = info.argCount != 1;
        signature = String::number(info.argCount);
    }
    String keywordOnly = emptyString();
    if (keywordOnlyGiven)
        keywordOnly = makeString(" positional argument"_s, given != 1 ? "s"_s : ""_s, " (and "_s, keywordOnlyGiven, " keyword-only argument"_s, keywordOnlyGiven != 1 ? "s"_s : ""_s, ')');
    raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() takes "_s, signature, " positional argument"_s, plural ? "s"_s : ""_s, " but "_s, given, keywordOnly, given == 1 && !keywordOnlyGiven ? " was"_s : " were"_s, " given"_s);
}

// `keywordValues`: the values that go with the names.
static NEVER_INLINE bool bindArgumentsSlow(JSGlobalObject* globalObject, CallFrame* callFrame, PyFunction* function, unsigned positionalCount, PyTuple* keywordNames, const JSValue* keywordValues)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodeInfo& info = function->codeInfo();
    unsigned argCount = info.argCount;
    unsigned namedCount = argCount + info.kwOnlyCount;
    bool hasVarArgs = info.flags.contains(CodeFlag::VarArgs);
    const JSValue* args = reinterpret_cast<const JSValue*>(callFrame->addressOfArgumentsStart());

    // A call in JS may pass any function more than it takes, and those who call callbacks do: array.map(f) is f(item, index, array).
    if (positionalCount > argCount && !hasVarArgs && !isCalledByPython(globalObject, callFrame))
        positionalCount = argCount;

    // The parameters are worked out on the side: they go where the arguments are.
    MarkedArgumentBuffer buffer;
    buffer.fill(vm, info.parameterCount(), [](JSValue*) { });
    JSValue* slots = valuesOf(buffer);

    PyDict* keywordDict = nullptr;
    if (info.flags.contains(CodeFlag::VarKeywords)) {
        keywordDict = PyDict::create(globalObject);
        slots[namedCount + hasVarArgs] = keywordDict;
    }
    unsigned copied = std::min(positionalCount, argCount);
    for (unsigned i = 0; i < copied; ++i)
        slots[i] = args[i];
    if (hasVarArgs)
        slots[namedCount] = positionalCount > copied ? PyTuple::create(globalObject, std::span<const JSValue> { args + copied, positionalCount - copied }) : rt.emptyTuple();

    unsigned keywordCount = keywordNames ? keywordNames->length() : 0;
    // The keys of the dict that the names came from, if some of them are instances of classes derived from str.
    JSValue keysValue = keywordCount ? hiddenField(globalObject, keywordNames, keywordKeysField) : JSValue();
    PyTuple* keys = keysValue ? PyTuple::from(keysValue) : nullptr;
    for (unsigned k = 0; k < keywordCount; ++k) {
        JSString* keyword = asString(keywordNames->at(k));
        JSValue value = keywordValues[k];
        const StringImpl* impl = keyword->tryGetValueImpl();
        unsigned index = info.posOnlyCount;
        if (keys && !keys->at(k).isString()) [[unlikely]] {
            // Such a class says itself what its instances are equal to.
            for (; index < namedCount; ++index) {
                bool isEqual = richCompareBool(globalObject, keys->at(k), internString(vm, info.parameterNames[index].string()), CompareOperator::Eq);
                RETURN_IF_EXCEPTION(scope, false);
                if (isEqual)
                    break;
            }
        } else {
            for (; index < namedCount; ++index) {
                if (info.parameterNames[index].impl() == impl)
                    break;
            }
        }
        if (index == namedCount && !impl->isAtom()) {
            for (index = info.posOnlyCount; index < namedCount; ++index) {
                if (WTF::equal(info.parameterNames[index].impl(), impl))
                    break;
            }
        }
        if (index < namedCount) {
            if (slots[index]) {
                raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() got multiple values for argument '"_s, stringOf(keyword), '\'');
                return false;
            }
            slots[index] = value;
            continue;
        }
        if (keywordDict) {
            if (keys) [[unlikely]] {
                keywordDict->set(globalObject, keys->at(k), value);
                RETURN_IF_EXCEPTION(scope, false);
            } else
                keywordDict->set(vm, keyword, value);
            continue;
        }

        // Nothing takes it. It may be the name of a parameter that is positional-only.
        if (info.posOnlyCount) {
            StrBuilder names;
            for (unsigned i = 0; i < info.posOnlyCount; ++i) {
                for (unsigned j = 0; j < keywordCount; ++j) {
                    if (WTF::equal(info.parameterNames[i].impl(), asString(keywordNames->at(j))->tryGetValueImpl())) {
                        if (!names.isEmpty())
                            names.append(", "_s);
                        names.append(info.parameterNames[i].string());
                    }
                }
            }
            if (!names.isEmpty()) {
                raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() got some positional-only arguments passed as keyword arguments: '"_s, names.toString(), '\'');
                return false;
            }
        }
        Vector<String> candidates;
        for (unsigned i = info.posOnlyCount; i < namedCount; ++i)
            candidates.append(info.parameterNames[i].string());
        String suggestion = calculateSuggestion(candidates, stringOf(keyword));
        if (!suggestion.isNull())
            raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() got an unexpected keyword argument '"_s, stringOf(keyword), "'. Did you mean '"_s, suggestion, "'?"_s);
        else
            raiseTypeError(globalObject, scope, qualnameForMessage(globalObject, function), "() got an unexpected keyword argument '"_s, stringOf(keyword), '\'');
        return false;
    }

    if (positionalCount > argCount && !hasVarArgs) {
        scope.release();
        raiseTooManyPositional(globalObject, function, slots, positionalCount);
        return false;
    }

    if (positionalCount < argCount) {
        PyTuple* defaults = function->defaults();
        unsigned defaultCount = defaults ? defaults->length() : 0;
        unsigned required = argCount - std::min(defaultCount, argCount);
        for (unsigned i = positionalCount; i < required; ++i) {
            if (!slots[i]) {
                scope.release();
                raiseMissingArguments(globalObject, function, slots, 0, required, "positional"_s);
                return false;
            }
        }
        for (unsigned i = std::max(copied, required); i < argCount; ++i) {
            if (!slots[i])
                slots[i] = defaults->at(i + defaultCount - argCount); // The last default is that of the last parameter.
        }
    }

    if (info.kwOnlyCount) {
        PyDict* defaults = function->kwDefaults();
        bool isMissing = false;
        for (unsigned i = argCount; i < namedCount; ++i) {
            if (slots[i])
                continue;
            if (defaults) {
                JSValue value = defaults->get(globalObject, jsString(vm, info.parameterNames[i].string().string()));
                RETURN_IF_EXCEPTION(scope, false);
                if (value) {
                    slots[i] = value;
                    continue;
                }
            }
            isMissing = true;
        }
        if (isMissing) {
            scope.release();
            raiseMissingArguments(globalObject, function, slots, argCount, namedCount, "keyword-only"_s);
            return false;
        }
    }

    for (unsigned i = 0; i < info.parameterCount(); ++i)
        callFrame->setArgument(i, slots[i]);
    return true;
}

bool bindArguments(JSGlobalObject* globalObject, CallFrame* callFrame, PyFunction* function)
{
    CodeInfo& info = function->codeInfo();
    unsigned count = callFrame->argumentCount();
    JSValue thisValue = callFrame->thisValue();
    PyTuple* keywordNames = nullptr;
    if (thisValue.isCell() && thisValue.asCell()->type() == PyKwNamesType) [[unlikely]] {
        keywordNames = uncheckedDowncast<PyTuple>(thisValue.asCell());
        count -= keywordNames->length();
    } else if (hasKeywordMarker(globalObject, callFrame)) [[unlikely]] {
        // From JS: f(1, kwargs({ a: 2 })).
        MarkedArgumentBuffer values;
        if (!keywordsOfMarker(globalObject, callFrame->uncheckedArgument(count - 1), keywordNames, values))
            return false;
        return bindArgumentsSlow(globalObject, callFrame, function, count - 1, keywordNames, valuesOf(values));
    } else if (count == info.argCount && info.parameterCount() == count) [[likely]]
        return true; // Every argument is where its parameter is.
    else if (count < info.argCount && info.parameterCount() == info.argCount) {
        // So it is, and what is left out has defaults. The last default is that of the last parameter.
        PyTuple* defaults = function->defaults();
        if (unsigned defaultCount = defaults ? defaults->length() : 0; info.argCount - count <= defaultCount) {
            for (unsigned i = count; i < info.argCount; ++i)
                callFrame->setArgument(i, defaults->at(i + defaultCount - info.argCount));
            return true;
        }
    }
    return bindArgumentsSlow(globalObject, callFrame, function, count, keywordNames, reinterpret_cast<const JSValue*>(callFrame->addressOfArgumentsStart()) + count);
}

// ---- The type of functions ----

static JSValue functionRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(globalObject, makeString("<function "_s, qualnameForMessage(globalObject, PyFunction::from(self)), " at "_s, addressForRepr(globalObject, self), '>'));
}

static JSValue functionCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    return call(globalObject, self, args);
}

static JSValue functionGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue)
{
    if (isNone(instance))
        return self;
    return PyMethod::create(globalObject, self, instance);
}

static JSValue functionNew(JSGlobalObject* globalObject, JSValue, JSValue code, JSValue globals, JSValue name, JSValue defaults, JSValue closure, JSValue kwDefaults)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* codeObject = PyCode::tryFrom(code);
    if (!codeObject)
        return raiseTypeError(globalObject, scope, "function() argument 'code' must be code, not "_s, typeNameOfArgument(globalObject, code));
    if (!isDict(globals))
        return raiseTypeError(globalObject, scope, "function() argument 'globals' must be dict, not "_s, typeName(globalObject, globals));
    if (name && !isNone(name) && !isStr(name))
        return raiseTypeError(globalObject, scope, "arg 3 (name) must be None or string"_s);
    if (defaults && !isNone(defaults) && !PyTuple::is(defaults))
        return raiseTypeError(globalObject, scope, "arg 4 (defaults) must be None or tuple"_s);
    if (kwDefaults && !isNone(kwDefaults) && !isDict(kwDefaults))
        return raiseTypeError(globalObject, scope, "arg 6 (kwdefaults) must be None or dict"_s);
    size_t freeCount = codeObject->codeInfo().freeVariables.size();
    PyTuple* closureTuple = closure ? PyTuple::tryFrom(closure) : nullptr;
    if (!closureTuple) {
        if (freeCount && (!closure || isNone(closure)))
            return raiseTypeError(globalObject, scope, "arg 5 (closure) must be tuple"_s);
        if (closure && !isNone(closure))
            return raiseTypeError(globalObject, scope, "arg 5 (closure) must be None or tuple"_s);
    }
    size_t closureCount = closureTuple ? closureTuple->length() : 0;
    if (closureCount != freeCount)
        return raiseValueError(globalObject, scope, codeObject->codeInfo().name.string(), " requires closure of length "_s, freeCount, ", not "_s, closureCount);
    for (size_t i = 0; i < closureCount; ++i) {
        if (!PyCell::is(closureTuple->at(i)))
            return raiseTypeError(globalObject, scope, "arg 5 (closure) expected cell, found "_s, typeName(globalObject, closureTuple->at(i)));
    }

    PyFunction* function = PyFunction::create(globalObject, codeObject->executable(), PyDict::from(globals));
    function->setCode(vm, codeObject);
    if (name && !isNone(name))
        function->setName(vm, strValue(name));
    if (defaults && !isNone(defaults))
        function->setDefaults(vm, PyTuple::from(defaults));
    if (kwDefaults && !isNone(kwDefaults))
        function->setKwDefaults(vm, PyDict::from(kwDefaults));
    if (closureCount)
        function->setClosure(vm, closureTuple);
    return function;
}

static JSValue orNone(JSValue value) { return value ? value : jsUndefined(); }
template<typename T> static JSValue orNone(T* cell) { return cell ? JSValue(cell) : jsUndefined(); }

static JSValue functionGetName(JSGlobalObject*, JSValue self) { return PyFunction::from(self)->name(); }
static JSValue functionGetQualname(JSGlobalObject*, JSValue self) { return PyFunction::from(self)->qualname(); }
static JSValue functionGetDoc(JSGlobalObject*, JSValue self) { return orNone(PyFunction::from(self)->doc()); }
static JSValue functionGetModule(JSGlobalObject*, JSValue self) { return orNone(PyFunction::from(self)->module()); }
static JSValue functionGetGlobals(JSGlobalObject*, JSValue self) { return PyFunction::from(self)->globals(); }
static JSValue functionGetBuiltins(JSGlobalObject*, JSValue self) { return PyFunction::from(self)->builtinsObject(); }
static JSValue functionGetClosure(JSGlobalObject*, JSValue self) { return orNone(PyFunction::from(self)->closure()); }
// What of a function is audited: reading and changing its code and the defaults of its parameters.
static bool auditFunctionAttribute(JSGlobalObject* globalObject, ASCIILiteral event, JSValue self, ASCIILiteral name, JSValue value = { })
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isAudited(globalObject)) [[likely]]
        return true;
    JSString* nameString = makeStr(getVM(globalObject), name);
    RELEASE_AND_RETURN(scope, value ? audit(globalObject, event, self, nameString, value) : audit(globalObject, event, self, nameString));
}

static JSValue functionGetDefaults(JSGlobalObject* globalObject, JSValue self)
{
    if (!auditFunctionAttribute(globalObject, "object.__getattr__"_s, self, "__defaults__"_s))
        return { };
    return orNone(PyFunction::from(self)->defaults());
}
static JSValue functionGetKwDefaults(JSGlobalObject* globalObject, JSValue self)
{
    if (!auditFunctionAttribute(globalObject, "object.__getattr__"_s, self, "__kwdefaults__"_s))
        return { };
    return orNone(PyFunction::from(self)->kwDefaults());
}
static JSValue functionGetAnnotate(JSGlobalObject*, JSValue self) { return orNone(PyFunction::from(self)->annotate()); }
static JSValue functionGetCode(JSGlobalObject* globalObject, JSValue self)
{
    if (!auditFunctionAttribute(globalObject, "object.__getattr__"_s, self, "__code__"_s))
        return { };
    return codeOf(globalObject, PyFunction::from(self));
}

static JSValue functionGetTypeParams(JSGlobalObject* globalObject, JSValue self)
{
    JSValue params = PyFunction::from(self)->typeParams();
    return params ? params : JSValue(runtime(globalObject).emptyTuple());
}

static void functionSetName(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isStr(value)) {
        raiseTypeError(globalObject, scope, "__name__ must be set to a string object"_s);
        return;
    }
    PyFunction::from(self)->setName(vm, value);
}

static void functionSetQualname(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !isStr(value)) {
        raiseTypeError(globalObject, scope, "__qualname__ must be set to a string object"_s);
        return;
    }
    PyFunction::from(self)->setQualname(vm, value);
}

static void functionSetDoc(JSGlobalObject* globalObject, JSValue self, JSValue value) { PyFunction::from(self)->setDoc(getVM(globalObject), orNone(value)); }
static void functionSetModule(JSGlobalObject* globalObject, JSValue self, JSValue value) { PyFunction::from(self)->setModule(getVM(globalObject), value); }

static void functionSetDefaults(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value && !isNone(value) && !PyTuple::is(value)) {
        raiseTypeError(globalObject, scope, "__defaults__ must be set to a tuple object"_s);
        return;
    }
    if (failed(scope, auditFunctionAttribute(globalObject, value ? "object.__setattr__"_s : "object.__delattr__"_s, self, "__defaults__"_s, value)))
        return;
    if (!value || isNone(value))
        return PyFunction::from(self)->setDefaults(vm, nullptr);
    PyFunction::from(self)->setDefaults(vm, PyTuple::from(value));
}

static void functionSetKwDefaults(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value && !isNone(value) && !isDict(value)) {
        raiseTypeError(globalObject, scope, "__kwdefaults__ must be set to a dict object"_s);
        return;
    }
    if (failed(scope, auditFunctionAttribute(globalObject, value ? "object.__setattr__"_s : "object.__delattr__"_s, self, "__kwdefaults__"_s, value)))
        return;
    if (!value || isNone(value))
        return PyFunction::from(self)->setKwDefaults(vm, nullptr);
    PyFunction::from(self)->setKwDefaults(vm, PyDict::from(value));
}

static void functionSetCode(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* function = PyFunction::from(self);
    auto* code = value ? PyCode::tryFrom(value) : nullptr;
    if (!code) {
        raiseTypeError(globalObject, scope, "__code__ must be set to a code object"_s);
        return;
    }
    if (failed(scope, auditFunctionAttribute(globalObject, "object.__setattr__"_s, self, "__code__"_s, value)))
        return;
    size_t freeCount = code->codeInfo().freeVariables.size();
    size_t closureCount = function->closure() ? function->closure()->length() : 0;
    if (freeCount != closureCount) {
        String name = function->name().isString() ? String(asString(function->name())->value(globalObject)) : String();
        RETURN_IF_EXCEPTION(scope, void());
        raiseValueError(globalObject, scope, name, "() requires a code object with "_s, closureCount, " free vars, not "_s, freeCount);
        return;
    }
    constexpr OptionSet<CodeFlag> kinds { CodeFlag::Generator, CodeFlag::Coroutine, CodeFlag::AsyncGenerator };
    if ((codeOf(globalObject, function)->flags() & kinds) != (code->flags() & kinds)) {
        if (failed(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "Assigning a code object of non-matching type is deprecated (e.g., from a generator to a plain function)"_s)))
            return;
    }
    function->setCode(vm, code);
}

static JSValue functionGetAnnotations(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* function = PyFunction::from(self);
    if (JSValue annotations = function->annotations())
        return annotations;
    JSValue annotations;
    JSValue annotate = function->annotate();
    if (annotate && isCallable(globalObject, annotate)) {
        annotations = call(globalObject, annotate, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isDict(annotations))
            return raiseTypeError(globalObject, scope, "__annotate__() must return a dict, not "_s, typeName(globalObject, annotations));
    } else
        annotations = PyDict::create(globalObject);
    function->setAnnotations(vm, annotations);
    return annotations;
}

static void functionSetAnnotations(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* function = PyFunction::from(self);
    if (value && isNone(value))
        value = JSValue();
    if (value && !isDict(value)) {
        raiseTypeError(globalObject, scope, "__annotations__ must be set to a dict object"_s);
        return;
    }
    function->setAnnotations(vm, value);
    function->setAnnotate(vm, { });
}

static void functionSetAnnotate(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* function = PyFunction::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "__annotate__ cannot be deleted"_s);
        return;
    }
    if (isNone(value))
        return function->setAnnotate(vm, { });
    if (!isCallable(globalObject, value)) {
        raiseTypeError(globalObject, scope, "__annotate__ must be callable or None"_s);
        return;
    }
    function->setAnnotate(vm, value);
    function->setAnnotations(vm, { });
}

static void functionSetTypeParams(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value || !PyTuple::is(value)) {
        raiseTypeError(globalObject, scope, "__type_params__ must be set to a tuple"_s);
        return;
    }
    PyFunction::from(self)->setTypeParams(vm, value);
}

static constexpr NativeDef functionMethods[] = {
    PY_NEW(functionNew, "($type, /, code, globals, name=None, argdefs=None, closure=None, kwdefaults=None)"),
    PY_SLOT("__repr__", functionRepr, "($self, /)"),
    PY_SLOT("__call__", functionCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", functionGet, "($self, instance, owner=None, /)"),
};

static constexpr NativeGetSet functionGetSets[] = {
    { "__name__"_s, functionGetName, functionSetName },
    { "__qualname__"_s, functionGetQualname, functionSetQualname },
    { "__doc__"_s, functionGetDoc, functionSetDoc },
    { "__module__"_s, functionGetModule, functionSetModule },
    PY_MEMBER("__globals__", functionGetGlobals),
    PY_MEMBER("__builtins__", functionGetBuiltins),
    PY_MEMBER("__closure__", functionGetClosure),
    { "__code__"_s, functionGetCode, functionSetCode },
    { "__defaults__"_s, functionGetDefaults, functionSetDefaults },
    { "__kwdefaults__"_s, functionGetKwDefaults, functionSetKwDefaults },
    { "__annotations__"_s, functionGetAnnotations, functionSetAnnotations },
    { "__annotate__"_s, functionGetAnnotate, functionSetAnnotate },
    { "__type_params__"_s, functionGetTypeParams, functionSetTypeParams },
};

// ---- Bound methods ----

PyMethod::PyMethod(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyMethod* PyMethod::create(JSGlobalObject* globalObject, JSValue function, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* method = new (NotNull, allocateCell<PyMethod>(vm)) PyMethod(vm, rt.structure<PyMethod>(), rt.methodType());
    method->finishCreation(vm);
    method->m_function.set(vm, method, function);
    method->m_self.set(vm, method, self);
    return method;
}

template<typename Visitor>
void PyMethod::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyMethod>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_function);
    visitor.append(thisObject->m_self);
}

DEFINE_VISIT_CHILDREN(PyMethod);

// What calls any Python object that JS, or compiled Python code, calls as it would a function.
JSC_DEFINE_HOST_FUNCTION(callPythonObject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    CallArgs args(callFrame);
    if (!args.hasKeywords() && hasKeywordMarker(globalObject, callFrame)) [[unlikely]]
        return JSValue::encode(resultForCaller(globalObject, callFrame, callWithKeywordMarker(globalObject, callFrame)));
    // As in bindArgumentsSlow(): what JS passes a bound method beyond what it takes is dropped.
    if (auto* method = dynamicDowncast<PyMethod>(callFrame->jsCallee()); method && !args.hasKeywords()) {
        if (auto* function = PyFunction::tryFrom(method->function())) {
            CodeInfo& info = function->codeInfo();
            if (args.size() + 1 > info.argCount && info.argCount && !info.flags.contains(CodeFlag::VarArgs) && !isCalledByPython(globalObject, callFrame))
                args = CallArgs(args.positional().first(info.argCount - 1));
        }
    }
    return JSValue::encode(resultForCaller(globalObject, callFrame, call(globalObject, callFrame->jsCallee(), args)));
}

CallData PyMethod::getCallData(JSCell*)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callPythonObject;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

static JSValue methodNew(JSGlobalObject* globalObject, JSValue, JSValue function, JSValue instance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isCallable(globalObject, function))
        return raiseTypeError(globalObject, scope, "first argument must be callable"_s);
    if (isNone(instance))
        return raiseTypeError(globalObject, scope, "instance must not be None"_s);
    RELEASE_AND_RETURN(scope, PyMethod::create(globalObject, function, instance));
}

static JSValue methodCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return call(globalObject, self, args); }
static JSValue methodGet(JSGlobalObject*, JSValue self, JSValue, JSValue) { return self; }

static JSValue methodRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* method = PyMethod::from(self);
    JSValue name = getAttrIfPresent(globalObject, method->function(), rt.names.dQualname());
    RETURN_IF_EXCEPTION(scope, { });
    if (!name) {
        name = getAttrIfPresent(globalObject, method->function(), rt.names.dName());
        RETURN_IF_EXCEPTION(scope, { });
    }
    String nameText = "?"_s;
    if (name && isStr(name)) {
        nameText = strValue(name)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSString* selfRepr = repr(globalObject, method->self());
    RETURN_IF_EXCEPTION(scope, { });
    String selfText = selfRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<bound method "_s, nameText, " of "_s, selfText, '>'));
}

template<bool wantEqual>
static JSValue methodEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyMethod::is(other))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    auto* a = PyMethod::from(self);
    auto* b = PyMethod::from(other);
    bool equal = isEqual(globalObject, a->function(), b->function());
    RETURN_IF_EXCEPTION(scope, { });
    if (equal)
        equal = isIdentical(a->self(), b->self());
    return jsBoolean(equal == wantEqual);
}

static JSValue methodHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* method = PyMethod::from(self);
    JSValue selfHash = objectHash(globalObject, method->self());
    int64_t functionHash = hash(globalObject, method->function());
    RETURN_IF_EXCEPTION(scope, { });
    int64_t result = tryInt64(selfHash).value_or(0) ^ functionHash;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result == -1 ? static_cast<int64_t>(-2) : result));
}

// What the method itself does not have is looked for on the function.
static JSValue methodGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    auto* method = PyMethod::from(self);
    if (JSValue descriptor = method->type()->lookup(name))
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, descriptor, self, method->type()));
    RELEASE_AND_RETURN(scope, getAttr(globalObject, method->function(), name));
}

static JSValue methodReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* method = PyMethod::from(self);
    JSValue name = getAttr(globalObject, method->function(), rt.names.dName());
    RETURN_IF_EXCEPTION(scope, { });
    if (isStr(name)) {
        // A private name is to be looked up from outside the class.
        String text = strValue(name)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (text.length() >= 3 && text.startsWith("__"_s) && !text.endsWith("__"_s) && !text.contains('.')) {
            String className = (PyType::is(method->self()) ? PyType::from(method->self()) : typeOf(globalObject, method->self()))->name();
            unsigned underscores = 0;
            while (underscores < className.length() && className[underscores] == '_')
                ++underscores;
            if (underscores < className.length())
                name = makeStr(vm, makeString('_', StringView(className).substring(underscores), text));
        }
    }
    JSValue getattr = currentBuiltin(globalObject, internString(vm, "getattr"_s));
    if (!getattr)
        return { };
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { method->self(), name }) }));
}

static JSValue methodGetFunc(JSGlobalObject*, JSValue self) { return PyMethod::from(self)->function(); }
static JSValue methodGetSelf(JSGlobalObject*, JSValue self) { return PyMethod::from(self)->self(); }
static JSValue methodGetDoc(JSGlobalObject* globalObject, JSValue self) { return getAttr(globalObject, PyMethod::from(self)->function(), runtime(globalObject).names.dDoc()); }

static constexpr NativeDef methodMethods[] = {
    PY_NEW(methodNew, "($type, function, instance, /)"),
    PY_SLOT("__call__", methodCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", methodGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", methodRepr, "($self, /)"),
    PY_SLOT("__eq__", methodEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", methodEquality<false>, "($self, value, /)"),
    PY_SLOT("__hash__", methodHash, "($self, /)"),
    PY_SLOT("__getattribute__", methodGetAttribute, "($self, name, /)"),
    PY_METHOD("__reduce__", methodReduce, "($self, /)"),
};

static constexpr NativeGetSet methodGetSets[] = {
    PY_MEMBER("__func__", methodGetFunc),
    PY_MEMBER("__self__", methodGetSelf),
    { "__doc__"_s, methodGetDoc },
};

// ---- Code objects ----

PyCode::PyCode(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyCode* PyCode::of(JSGlobalObject* globalObject, FunctionExecutable* executable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (JSCell* existing = executable->pythonCodeObject())
        return uncheckedDowncast<PyCode>(existing);
    auto* code = new (NotNull, allocateCell<PyCode>(vm)) PyCode(vm, rt.structure<PyCode>(), rt.codeType());
    code->finishCreation(vm);
    code->m_executable.set(vm, code, executable);
    code->m_codeFlags = code->codeInfo().flags;
    executable->setPythonCodeObject(vm, code);
    return code;
}

PyCode* PyCode::createWithFlags(JSGlobalObject* globalObject, FunctionExecutable* executable, OptionSet<CodeFlag> flags)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* code = new (NotNull, allocateCell<PyCode>(vm)) PyCode(vm, rt.structure<PyCode>(), rt.codeType());
    code->finishCreation(vm);
    code->m_executable.set(vm, code, executable);
    code->m_codeFlags = flags;
    return code;
}

PyCode* PyCode::ofWithFlags(JSGlobalObject* globalObject, FunctionExecutable* executable, OptionSet<CodeFlag> flags)
{
    PyCode* code = of(globalObject, executable);
    code->m_codeFlags = flags;
    return code;
}

PyCode* codeOf(JSGlobalObject* globalObject, PyFunction* function)
{
    function = function->codeOwner();
    if (PyCode* code = function->assignedCode())
        return code;
    return PyCode::of(globalObject, function->functionExecutable());
}

template<typename Visitor>
void PyCode::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCode>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_executable);
}

DEFINE_VISIT_CHILDREN(PyCode);

// What replace() gave another value, if no copy of the code could be made (see codeReplace()): a dict, the hidden field of the
// result. These only describe the code then. What runs goes by the CodeInfo: a traceback shows the names that has.
static constexpr unsigned codeReplacedField = 0;
static constexpr unsigned codePositionsField = 1; // See codePositions().
static constexpr unsigned codeBytesField = 2; // co_code, once it has been asked for: tracers look at it for every line.

static JSValue codeReplaced(JSGlobalObject* globalObject, JSValue self, ASCIILiteral name)
{
    JSValue replaced = hiddenField(globalObject, self, codeReplacedField);
    return replaced ? PyDict::from(replaced)->get(internString(getVM(globalObject), name)) : JSValue();
}

static JSValue codeGetName(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue replaced = codeReplaced(globalObject, self, "co_name"_s))
        return replaced;
    return jsString(getVM(globalObject), PyCode::from(self)->codeInfo().name.string().string());
}

static JSValue codeGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue replaced = codeReplaced(globalObject, self, "co_qualname"_s))
        return replaced;
    return makeStr(globalObject, PyCode::from(self)->codeInfo().qualname);
}

static JSValue codeGetFilename(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue replaced = codeReplaced(globalObject, self, "co_filename"_s))
        return replaced;
    return makeStr(globalObject, PyCode::from(self)->codeInfo().filename);
}

static JSValue codeGetFirstLineNo(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue replaced = codeReplaced(globalObject, self, "co_firstlineno"_s))
        return replaced;
    return jsNumber(PyCode::from(self)->codeInfo().firstLineNumber);
}

static JSValue codeRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = strValue(codeGetName(globalObject, self))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String filename = strValue(codeGetFilename(globalObject, self))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString("<code object "_s, name, " at "_s, addressForRepr(globalObject, self), ", file \""_s, filename, "\", line "_s, codeGetFirstLineNo(globalObject, self).asInt32(), '>')));
}

// The code block that has the instructions: for a generator function, that of the body. Null: an exception.
CodeBlock* codeBlockOf(JSGlobalObject* globalObject, PyCode* code)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto link = [&](FunctionExecutable* executable) -> CodeBlock* {
        CodeBlock* codeBlock = executable->codeBlockForCall();
        if (!codeBlock) {
            // It has never been called.
            PyFunction* function = PyFunction::create(globalObject, executable, PyDict::create(globalObject));
            executable->prepareForExecution<FunctionExecutable>(vm, function, function->scope(), CodeSpecializationKind::CodeForCall, codeBlock);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        return codeBlock;
    };
    CodeBlock* codeBlock = link(code->executable());
    if (codeBlock && code->codeInfo().isGeneratorLike())
        codeBlock = link(codeBlock->functionExpr(CodeInfo::generatorBodyIndex));
    return codeBlock;
}

CodeBlock* codeBlockIfAny(PyCode* code)
{
    CodeBlock* codeBlock = code->executable()->codeBlockForCall();
    if (codeBlock && code->codeInfo().isGeneratorLike())
        codeBlock = codeBlock->functionExpr(CodeInfo::generatorBodyIndex)->codeBlockForCall();
    return codeBlock;
}

template<typename Filter>
static JSValue namesOfVariables(JSGlobalObject* globalObject, CodeInfo& info, const Filter& filter)
{
    VM& vm = getVM(globalObject);
    MarkedArgumentBuffer buffer;
    for (auto& variable : info.variables) {
        if (!variable.isHidden && filter(variable))
            buffer.append(jsString(vm, variable.name.string().string()));
    }
    return PyTuple::create(globalObject, spanOf(buffer));
}

static JSValue tupleOfNames(JSGlobalObject* globalObject, const Vector<Identifier>& identifiers)
{
    VM& vm = getVM(globalObject);
    PyTuple* result = PyTuple::create(globalObject, identifiers.size());
    for (unsigned i = 0; i < identifiers.size(); ++i)
        result->initializeAt(vm, i, jsString(vm, identifiers[i].string().string()));
    return result;
}

static JSValue codeGetArgCount(JSGlobalObject*, JSValue self) { return jsNumber(PyCode::from(self)->codeInfo().argCount); }
static JSValue codeGetPosOnlyArgCount(JSGlobalObject*, JSValue self) { return jsNumber(PyCode::from(self)->codeInfo().posOnlyCount); }
static JSValue codeGetKwOnlyArgCount(JSGlobalObject*, JSValue self) { return jsNumber(PyCode::from(self)->codeInfo().kwOnlyCount); }
static JSValue codeGetFlags(JSGlobalObject*, JSValue self) { return jsNumber(PyCode::from(self)->flags().toRaw()); }
static JSValue codeGetStackSize(JSGlobalObject*, JSValue) { return jsNumber(0); }
static JSValue codeGetNames(JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, PyCode::from(self)->codeInfo().names); }
static JSValue codeGetFreeVars(JSGlobalObject* globalObject, JSValue self) { return tupleOfNames(globalObject, PyCode::from(self)->codeInfo().freeVariables); }
static JSValue codeGetEmptyBytes(JSGlobalObject* globalObject, JSValue) { return runtime(globalObject).emptyBytes(); }

// Nothing reads a co_linetable but the interpreter it is for (programs have co_lines()). What takes a code object apart and puts it
// together again with code(...), as cloudpickle does, passes it along. So here it is what code(...) makes the code from: all of
// it, as marshal writes it.
static JSValue codeGetLineTable(JSGlobalObject* globalObject, JSValue self)
{
    Vector<uint8_t> bytes;
    if (!writeMarshalledCode(globalObject, PyCode::from(self), bytes))
        return runtime(globalObject).emptyBytes();
    return PyBytes::create(globalObject, bytes.span());
}

// The parameters, then the other variables that are not only cells.
static JSValue codeGetVarNames(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    CodeInfo& info = PyCode::from(self)->codeInfo();
    MarkedArgumentBuffer names;
    for (auto& parameter : info.parameterNames)
        names.append(jsString(vm, parameter.string().string()));
    // Those of inlined comprehensions come last, and share the place of a variable of the same name.
    Vector<Identifier, 16> seen;
    for (bool hidden : { false, true }) {
        for (auto& variable : info.variables) {
            // (A parameter may be known to the code by another name than the one it shows: the `format` of __annotate__.)
            if (variable.isHidden != hidden || variable.kind != CodeInfo::VariableKind::Local || variable.reg.isArgument() || info.parameterNames.contains(variable.name) || seen.contains(variable.name))
                continue;
            seen.append(variable.name);
            names.append(jsString(vm, variable.name.string().string()));
        }
    }
    return PyTuple::create(globalObject, spanOf(names));
}

static JSValue codeGetNLocals(JSGlobalObject* globalObject, JSValue self)
{
    return jsNumber(PyTuple::from(codeGetVarNames(globalObject, self))->length());
}

static JSValue codeGetCellVars(JSGlobalObject* globalObject, JSValue self)
{
    return namesOfVariables(globalObject, PyCode::from(self)->codeInfo(), [](auto& variable) { return variable.kind == CodeInfo::VariableKind::Cell; });
}

// The constants, as CPython would have them, or nearly: it works out more from constants than is done here. It leaves out
// those its instructions do not need: what went into making another constant, and what there are instructions to load
// without one. But never the first, where the docstring would be.
// The items of co_consts.
template<typename Function>
static void forEachConstantShown(JSGlobalObject* globalObject, PyCode* code, CodeBlock* codeBlock, const Function& function)
{
    bool isFirst = true;
    for (auto& constant : code->codeInfo().constants) {
        JSValue value = constant.reg.isValid() ? codeBlock->getConstant(constant.reg) : JSValue(PyCode::of(globalObject, codeBlock->functionExpr(constant.functionIndex)));
        bool needsNoConstant = isNone(value) || value.isBoolean() || (isSmallInt(value) && smallIntValue(value) >= -1 && smallIntValue(value) <= 255) || (value.isString() && !asString(value)->length());
        if (isFirst || (constant.isUsed && !needsNoConstant))
            function(constant, value);
        isFirst = false;
    }
}

static JSValue codeGetConsts(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* code = PyCode::from(self);
    CodeBlock* codeBlock = codeBlockOf(globalObject, code);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer constants;
    forEachConstantShown(globalObject, code, codeBlock, [&](const CodeInfo::Constant&, JSValue value) {
        constants.append(value);
    });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(constants)));
}

// Where the text of the code starts in its line.
static unsigned codeStartColumn(const SourceCode& source)
{
    StringView text = source.provider()->source();
    unsigned start = source.startOffset();
    unsigned column = 0;
    while (column < start && text[start - column - 1] != '\n')
        ++column;
    return column;
}

static bool codesAreEqual(JSGlobalObject*, PyCode*, PyCode*);
static JSValue codePositions(JSGlobalObject*, JSValue self);

// Equal, of the same type, and so for what they contain: 0, 0.0, -0.0 and False are four constants.
static bool constantsAreTheSame(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (a == b)
        return true;
    if (typeOf(globalObject, a) != typeOf(globalObject, b))
        return false;
    auto sameBits = [](double x, double y) { return std::bit_cast<uint64_t>(x) == std::bit_cast<uint64_t>(y); };
    if (auto* code = PyCode::tryFrom(a))
        RELEASE_AND_RETURN(scope, codesAreEqual(globalObject, code, PyCode::from(b)));
    if (isFloat(a))
        return sameBits(floatValue(a), floatValue(b));
    if (auto* complex = PyComplex::tryFrom(a))
        return sameBits(complex->real(), PyComplex::from(b)->real()) && sameBits(complex->imaginary(), PyComplex::from(b)->imaginary());
    if (auto* tuple = PyTuple::tryFrom(a)) {
        auto* other = PyTuple::from(b);
        if (tuple->length() != other->length())
            return false;
        for (unsigned i = 0; i < tuple->length(); ++i) {
            bool same = constantsAreTheSame(globalObject, tuple->at(i), other->at(i));
            RETURN_IF_EXCEPTION(scope, false);
            if (!same)
                return false;
        }
        return true;
    }
    if (auto* set = PySet::tryFrom(a)) {
        auto* other = PySet::from(b);
        if (set->size() != other->size())
            return false;
        JSValue item;
        for (size_t position = 0; set->next(position, item);) {
            bool found = false;
            JSValue candidate;
            for (size_t otherPosition = 0; !found && other->next(otherPosition, candidate);) {
                found = constantsAreTheSame(globalObject, item, candidate);
                RETURN_IF_EXCEPTION(scope, false);
            }
            if (!found)
                return false;
        }
        return true;
    }
    RELEASE_AND_RETURN(scope, isEqual(globalObject, a, b));
}

// Codes are equal if they do the same and say the same of where they come from, but for the name of the file. That the text is the
// same stands for the constants, the code within and the positions being the same. Code made from a tree has no text.
static bool codesAreEqual(JSGlobalObject* globalObject, PyCode* a, PyCode* b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (a == b)
        return true;
    bool sameName = isEqual(globalObject, codeGetName(globalObject, a), codeGetName(globalObject, b));
    RETURN_IF_EXCEPTION(scope, false);
    if (!sameName || a->flags() != b->flags() || codeGetFirstLineNo(globalObject, a) != codeGetFirstLineNo(globalObject, b))
        return false;
    if (a->executable() == b->executable())
        return true;

    CodeInfo& first = a->codeInfo();
    CodeInfo& second = b->codeInfo();
    if (first.argCount != second.argCount || first.posOnlyCount != second.posOnlyCount || first.kwOnlyCount != second.kwOnlyCount || first.parameterNames != second.parameterNames || first.names != second.names || first.variables.size() != second.variables.size())
        return false;
    for (unsigned i = 0; i < first.variables.size(); ++i) {
        if (first.variables[i].name != second.variables[i].name || first.variables[i].kind != second.variables[i].kind)
            return false;
    }
    CodeBlock* firstBlock = codeBlockOf(globalObject, a);
    RETURN_IF_EXCEPTION(scope, false);
    CodeBlock* secondBlock = codeBlockOf(globalObject, b);
    RETURN_IF_EXCEPTION(scope, false);
    auto& firstInstructions = firstBlock->instructions();
    auto& secondInstructions = secondBlock->instructions();
    if (firstInstructions.sizeInBytes() != secondInstructions.sizeInBytes())
        return false;
    {
        Monitoring::OriginalInstructions originalInstructionsOfFirst(runtime(globalObject).monitoring(), a);
        Monitoring::OriginalInstructions originalInstructionsOfSecond(runtime(globalObject).monitoring(), b);
        if (memcmp(firstInstructions.rawPointer(), secondInstructions.rawPointer(), firstInstructions.sizeInBytes()))
            return false;
    }
    const SourceCode& firstSource = a->executable()->source();
    const SourceCode& secondSource = b->executable()->source();
    bool sameColumn = codeStartColumn(firstSource) == codeStartColumn(secondSource);
    if (firstSource.view() == secondSource.view() && sameColumn && !first.hasReplacedConstants && !second.hasReplacedConstants)
        return true;
    // Not every instruction has its place recorded. Where the code starts tells two lambdas of a line apart. (The text of code made from a tree is blank.)
    auto hasText = [](const SourceCode& source) {
        return source.view().find([](char16_t character) { return character != ' ' && character != '\n'; }) != notFound;
    };
    if (!sameColumn && hasText(firstSource) && hasText(secondSource))
        return false;

    if (first.constants.size() != second.constants.size())
        return false;
    for (unsigned i = 0; i < first.constants.size(); ++i) {
        auto valueOf = [&](CodeBlock* block, auto& constant) {
            return constant.reg.isValid() ? block->getConstant(constant.reg) : JSValue(PyCode::of(globalObject, block->functionExpr(constant.functionIndex)));
        };
        bool same = constantsAreTheSame(globalObject, valueOf(firstBlock, first.constants[i]), valueOf(secondBlock, second.constants[i]));
        RETURN_IF_EXCEPTION(scope, false);
        if (!same)
            return false;
    }
    codePositions(globalObject, a);
    RETURN_IF_EXCEPTION(scope, false);
    codePositions(globalObject, b);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isEqual(globalObject, hiddenField(globalObject, a, codePositionsField), hiddenField(globalObject, b, codePositionsField)));
}

static JSValue codeEq(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* code = PyCode::tryFrom(other);
    if (!code)
        return runtime(globalObject).notImplemented();
    return jsBoolean(codesAreEqual(globalObject, PyCode::from(self), code));
}

static JSValue codeNe(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto* code = PyCode::tryFrom(other);
    if (!code)
        return runtime(globalObject).notImplemented();
    return jsBoolean(!codesAreEqual(globalObject, PyCode::from(self), code));
}

static JSValue codeHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* code = PyCode::from(self);
    int64_t nameHash = hash(globalObject, codeGetName(globalObject, self));
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t result = static_cast<uint64_t>(nameHash) * 1000003 ^ code->flags().toRaw();
    result = result * 1000003 ^ static_cast<uint64_t>(codeGetFirstLineNo(globalObject, self).asInt32());
    // Not the instructions: tools change them where they stand, and debuggers keep code objects in dictionaries.
    CodeInfo& info = code->codeInfo();
    for (uint64_t count : { static_cast<uint64_t>(info.argCount), static_cast<uint64_t>(info.kwOnlyCount), static_cast<uint64_t>(info.names.size()), static_cast<uint64_t>(info.variables.size()), static_cast<uint64_t>(info.constants.size()) })
        result = result * 1000003 ^ count;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(result >> 2)));
}

// `bytes`: what codeGetLineTable() gives. Null without an exception: they are not that, or not this build's.
static PyCode* codeFromLineTable(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    if (bytes.size() < MarshalledCodeHeader::size)
        return nullptr;
    auto header = MarshalledCodeHeader::read(bytes.first<MarshalledCodeHeader::size>());
    auto payload = bytes.subspan(MarshalledCodeHeader::size);
    if (header.payloadSize != payload.size())
        return nullptr;
    return readMarshalledCode(globalObject, header, payload);
}

// How code describes itself is in its CodeInfo, which goes with its instructions. `fresh` is code that nobody has seen yet.
// A null string, no line: as it is.
static PyCode* describeAnew(JSGlobalObject* globalObject, PyCode* fresh, const String& name, const String& qualname, const String& filename, std::optional<int> firstLine, int lineDelta, OptionSet<CodeFlag> flags)
{
    VM& vm = getVM(globalObject);
    auto change = [&](CodeInfo& info) {
        info.lineDelta = lineDelta; // marshal does not write it.
        if (!name.isNull())
            info.name = Identifier::fromString(vm, name);
        if (!qualname.isNull())
            info.qualname = qualname;
        if (!filename.isNull())
            info.filename = filename;
        if (firstLine) {
            info.lineDelta += *firstLine - info.firstLineNumber;
            info.firstLineNumber = *firstLine;
        }
    };
    change(fresh->codeInfo());
    if (fresh->codeInfo().isGeneratorLike()) {
        UnlinkedFunctionCodeBlock* codeBlock = fresh->executable()->unlinkedExecutable()->codeBlocksDecodingCached(vm).first;
        change(*codeBlock->functionExpr(CodeInfo::generatorBodyIndex)->pythonCodeInfo());
    }
    return PyCode::ofWithFlags(globalObject, fresh->executable(), flags);
}

// replace(co_consts=...). The instructions stay, so a constant can only make way for one that they can do the same with: a number,
// a str or a bytes for another of its type, the code of a function for code that wants as many cells of the enclosing function.
struct ReplacedConstant {
    CodeInfo::Constant constant;
    JSValue value; // Kept alive by the tuple it is from.
};

// False: an exception.
static bool codeFindReplacedConstants(JSGlobalObject* globalObject, PyCode* code, PyTuple* wanted, Vector<ReplacedConstant>& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodeBlock* codeBlock = codeBlockOf(globalObject, code);
    RETURN_IF_EXCEPTION(scope, false);
    Vector<ReplacedConstant> current;
    forEachConstantShown(globalObject, code, codeBlock, [&](const CodeInfo::Constant& constant, JSValue value) {
        current.append({ constant, value });
    });
    if (current.size() != wanted->length()) {
        raiseNotImplementedError(globalObject, scope, "code.replace() cannot change the number of constants"_s);
        return false;
    }
    for (unsigned i = 0; i < current.size(); ++i) {
        JSValue before = current[i].value;
        JSValue after = wanted->at(i);
        if (before == after)
            continue;
        if (auto* codeBefore = PyCode::tryFrom(before)) {
            auto* codeAfter = PyCode::tryFrom(after);
            if (!codeAfter || codeAfter->codeInfo().freeVariables.size() != codeBefore->codeInfo().freeVariables.size()) {
                raiseNotImplementedError(globalObject, scope, "code.replace() can only put code with as many free variables in the place of code in co_consts"_s);
                return false;
            }
        } else {
            bool same = constantsAreTheSame(globalObject, before, after);
            RETURN_IF_EXCEPTION(scope, false);
            if (same)
                continue;
            PyType* type = typeOf(globalObject, before);
            Runtime& rt = runtime(globalObject);
            bool isSimple = type == rt.intType() || type == rt.floatType() || type == rt.complexType() || type == rt.strType() || type == rt.bytesType();
            if (!isSimple || typeOf(globalObject, after) != type) {
                raiseNotImplementedError(globalObject, scope, "code.replace() can only put a number, a str or a bytes in the place of another of the same type in co_consts"_s);
                return false;
            }
        }
        result.append({ current[i].constant, after });
    }
    return true;
}

// `fresh` is code that nobody has seen yet, and that has not run.
static void codeChangeConstants(JSGlobalObject* globalObject, PyCode* fresh, const Vector<ReplacedConstant>& changes)
{
    VM& vm = getVM(globalObject);
    FunctionExecutable* owner = fresh->executable();
    UnlinkedFunctionCodeBlock* codeBlock = owner->unlinkedExecutable()->codeBlocksDecodingCached(vm).first;
    if (fresh->codeInfo().isGeneratorLike()) {
        UnlinkedFunctionExecutable* body = codeBlock->functionExpr(CodeInfo::generatorBodyIndex);
        owner = owner->pythonFunctionExpression(vm, CodeInfo::generatorBodyIndex, codeBlock->numberOfFunctionExprs(), body);
        codeBlock = body->codeBlocksDecodingCached(vm).first;
    }
    fresh->codeInfo().hasReplacedConstants |= !changes.isEmpty();
    for (auto& change : changes) {
        if (change.constant.reg.isValid()) {
            codeBlock->constantRegister(change.constant.reg).set(vm, codeBlock, change.value);
            continue;
        }
        owner->setPythonFunctionExpression(vm, change.constant.functionIndex, codeBlock->numberOfFunctionExprs(), PyCode::from(change.value)->executable());
        fresh->codeInfo().hasFunctionsFromElsewhere = true;
    }
}

static JSValue codeReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size())
        return raiseTypeError(globalObject, scope, "replace() takes no positional arguments"_s);

    enum class Kind : uint8_t { Int, Str, Bytes, Tuple };
    static constexpr struct {
        ASCIILiteral name;
        Kind kind;
        bool describesOnly; // Nothing that runs depends on it.
    } fields[] = {
        { "co_argcount"_s, Kind::Int, false }, { "co_posonlyargcount"_s, Kind::Int, false }, { "co_kwonlyargcount"_s, Kind::Int, false },
        { "co_nlocals"_s, Kind::Int, false }, { "co_stacksize"_s, Kind::Int, false }, { "co_flags"_s, Kind::Int, true },
        { "co_firstlineno"_s, Kind::Int, true }, { "co_code"_s, Kind::Bytes, false }, { "co_consts"_s, Kind::Tuple, false },
        { "co_names"_s, Kind::Tuple, false }, { "co_varnames"_s, Kind::Tuple, false }, { "co_freevars"_s, Kind::Tuple, false },
        { "co_cellvars"_s, Kind::Tuple, false }, { "co_filename"_s, Kind::Str, true }, { "co_name"_s, Kind::Str, true },
        { "co_qualname"_s, Kind::Str, true }, { "co_linetable"_s, Kind::Bytes, false }, { "co_exceptiontable"_s, Kind::Bytes, false },
    };
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String name = stringOf(args.keywordName(i));
        if (!std::ranges::any_of(fields, [&](auto& field) { return name == field.name; }))
            return raiseTypeError(globalObject, scope, "replace() got an unexpected keyword argument '"_s, name, '\'');
    }

    auto* code = PyCode::from(self);
    auto flags = code->flags();
    JSValue before = hiddenField(globalObject, self, codeReplacedField);
    PyDict* replaced = before ? PyDict::from(before)->copy(globalObject) : PyDict::create(globalObject);
    Vector<ReplacedConstant> replacedConstants;
    // In the order of the parameters, not of the arguments.
    for (auto& field : fields) {
        JSValue value;
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            if (stringOf(args.keywordName(i)) == field.name)
                value = args.keywordValue(i);
        }
        if (!value)
            continue;
        switch (field.kind) {
        case Kind::Int: {
            int number = toCInt(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            if (number < 0)
                return raiseValueError(globalObject, scope, field.name, " must be a positive integer"_s);
            value = jsNumber(number);
            break;
        }
        case Kind::Str:
            if (!isStr(value))
                return raiseTypeError(globalObject, scope, "replace() argument '"_s, field.name, "' must be str, not "_s, typeNameOfArgument(globalObject, value));
            break;
        case Kind::Bytes:
            if (!PyBytes::is(value))
                return raiseTypeError(globalObject, scope, "replace() argument '"_s, field.name, "' must be bytes, not "_s, typeNameOfArgument(globalObject, value));
            break;
        case Kind::Tuple:
            if (!PyTuple::is(value))
                return raiseTypeError(globalObject, scope, "replace() argument '"_s, field.name, "' must be tuple, not "_s, typeNameOfArgument(globalObject, value));
            break;
        }
        JSString* name = internString(vm, field.name);
        if (field.name == "co_flags"_s)
            flags = OptionSet<CodeFlag>::fromRaw(static_cast<uint32_t>(value.asInt32()));
        else if (field.describesOnly)
            replaced->set(vm, name, value);
        else if (field.name == "co_consts"_s) {
            if (failed(scope, codeFindReplacedConstants(globalObject, code, PyTuple::from(value), replacedConstants)))
                return { };
        } else {
            // FIXME: The instructions are not CPython's, and cannot be made from these.
            JSValue current = getAttr(globalObject, self, name);
            RETURN_IF_EXCEPTION(scope, { });
            bool same = isEqual(globalObject, current, value);
            RETURN_IF_EXCEPTION(scope, { });
            if (!same && field.name == "co_nlocals"_s)
                return raiseValueError(globalObject, scope, "code: co_nlocals != len(co_varnames)"_s);
            if (!same)
                return raiseNotImplementedError(globalObject, scope, "code.replace() cannot change "_s, field.name);
        }
    }
    if (replaced->size() || !replacedConstants.isEmpty()) {
        // The result has instructions of its own (see describeAnew()): what marshal would write and read back.
        Vector<uint8_t> bytes;
        PyCode* copy = !before && writeMarshalledCode(globalObject, code, bytes) ? codeFromLineTable(globalObject, bytes.span()) : nullptr;
        if (!copy && !replacedConstants.isEmpty())
            return raiseNotImplementedError(globalObject, scope, "code.replace() cannot change co_consts of code that is itself the result of replace()"_s);
        if (copy) {
            codeChangeConstants(globalObject, copy, replacedConstants);
            auto text = [&](ASCIILiteral name) -> String {
                JSValue value = replaced->get(internString(vm, name));
                return value ? String(strValue(value)->value(globalObject)) : String();
            };
            String name = text("co_name"_s);
            RETURN_IF_EXCEPTION(scope, { });
            String qualname = text("co_qualname"_s);
            RETURN_IF_EXCEPTION(scope, { });
            String filename = text("co_filename"_s);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue firstLine = replaced->get(internString(vm, "co_firstlineno"_s));
            RELEASE_AND_RETURN(scope, describeAnew(globalObject, copy, name, qualname, filename, firstLine ? std::optional<int>(firstLine.asInt32()) : std::nullopt, code->codeInfo().lineDelta, flags));
        }
    }
    PyCode* result = PyCode::createWithFlags(globalObject, code->executable(), flags);
    if (replaced->size())
        setHiddenField(globalObject, result, codeReplacedField, replaced);
    return result;
}

static JSValue codeNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "code"_s, args)) || failed(scope, checkPositional(globalObject, "code"_s, args, 16, 18)))
        return { };
    // code(argcount, posonlyargcount, kwonlyargcount, nlocals, stacksize, flags, codestring, constants, names, varnames, filename,
    // name, qualname, firstlineno, linetable, exceptiontable, freevars=(), cellvars=()). The instructions are not CPython's and cannot
    // be made from a codestring. What can be done is to put together what another code object was taken apart into.
    for (unsigned index : { 0, 1, 2, 3, 4, 5, 13 }) {
        toCInt(globalObject, args[index]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (auto* variableNames = PyTuple::tryFrom(args[9]); variableNames && toCInt(globalObject, args[3]) != static_cast<int>(variableNames->length()))
        return raiseValueError(globalObject, scope, "code: co_nlocals != len(co_varnames)"_s);
    auto* lineTable = PyBytes::tryFrom(args[14]);
    PyCode* code = lineTable ? codeFromLineTable(globalObject, lineTable->span()) : nullptr;
    if (!code)
        return raiseNotImplementedError(globalObject, scope, "code objects cannot be made from bytecode, only from the attributes of a code object of the same build"_s);
    static constexpr std::pair<unsigned, ASCIILiteral> unchangeable[] = {
        { 0, "co_argcount"_s }, { 1, "co_posonlyargcount"_s }, { 2, "co_kwonlyargcount"_s }, { 7, "co_consts"_s }, { 8, "co_names"_s },
        { 9, "co_varnames"_s }, { 16, "co_freevars"_s }, { 17, "co_cellvars"_s },
    };
    for (auto [index, name] : unchangeable) {
        if (index >= args.size())
            continue;
        JSValue own = getAttr(globalObject, code, internString(vm, name));
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, own, args[index]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!same)
            return raiseNotImplementedError(globalObject, scope, "code(): "_s, name, " is not that of the code the linetable is from"_s);
    }
    int flags = toCInt(globalObject, args[5]);
    RETURN_IF_EXCEPTION(scope, { });
    int firstLine = toCInt(globalObject, args[13]);
    RETURN_IF_EXCEPTION(scope, { });
    String texts[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isStr(args[10 + i]))
            return raiseTypeError(globalObject, scope, "code() argument "_s, 11 + i, " must be str, not "_s, typeNameOfArgument(globalObject, args[10 + i]));
        texts[i] = strValue(args[10 + i])->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, describeAnew(globalObject, code, texts[1], texts[2], texts[0], firstLine, 0, OptionSet<CodeFlag>::fromRaw(static_cast<uint32_t>(flags))));
}

// co_lines(), co_positions() and co_branches() return iterators of types of their own. Fields: the iterator of a list.
static JSValue codeIteratorIter(JSGlobalObject*, JSValue self) { return self; }
static JSValue codeIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSValue item = iterNext(globalObject, PyRecord::from(self)->get(0));
    RETURN_IF_EXCEPTION(scope, { });
    return item ? item : raiseStopIteration(globalObject, scope);
}

static constexpr NativeDef codeIteratorMethods[] = {
    PY_SLOT("__iter__", codeIteratorIter, "($self, /)"),
    PY_SLOT("__next__", codeIteratorNext, "($self, /)"),
};
static constexpr TypeDef lineIteratorTypeDef { "line_iterator"_s, { }, codeIteratorMethods, { }, { } };
static constexpr TypeDef positionsIteratorTypeDef { "positions_iterator"_s, { }, codeIteratorMethods, { }, { } };

static JSValue makeCodeIterator(JSGlobalObject* globalObject, const TypeDef& def, JSArray* items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* result = PyRecord::create(globalObject, runtime(globalObject).typeFor(def), 1);
    RETURN_IF_EXCEPTION(scope, { });
    result->set(vm, 0, iterator);
    return result;
}

// The runs of bytes of the instructions that come from one line. -1: from none. Where several lines begin at an instruction, see
// visibleOffset().
struct LineRun {
    unsigned start;
    unsigned end;
    int line;
};
static Vector<LineRun> lineRunsOf(const CodeInfo& info, CodeBlock* codeBlock)
{
    Vector<LineRun> runs;
    // What sets up the frame is of the line the code begins on.
    unsigned start = 0;
    int line = info.name.string() == "<module>"_s ? 0 : info.firstLineNumber;
    auto finishRun = [&](unsigned end) {
        if (start < end) {
            if (!runs.isEmpty() && runs.last().line == line)
                runs.last().end = end;
            else
                runs.append({ start, end, line });
        }
        start = end;
    };
    unsigned linesAtOffset = 0;
    unsigned lastOffset = std::numeric_limits<unsigned>::max();
    for (auto& mark : codeBlock->unlinkedCodeBlock()->pythonMarks()) {
        if (mark.offset != std::exchange(lastOffset, mark.offset))
            linesAtOffset = 0;
        unsigned offset = mark.offset + linesAtOffset;
        int next;
        if (mark.kind() == PythonMark::Line) {
            next = lineOfMarkValue(mark.value()) + info.lineDelta;
            ++linesAtOffset;
        } else if (mark.kind() == PythonMark::Start || (mark.kind() == PythonMark::NoLine && !mark.value()))
            next = -1;
        else
            continue;
        if (next == line)
            continue;
        finishRun(offset);
        line = next;
    }
    finishRun(codeBlock->instructions().size());
    return runs;
}

// (start, end, line) for each of them.
static JSValue codeLines(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodeBlock* codeBlock = codeBlockOf(globalObject, PyCode::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* lines = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (auto& run : lineRunsOf(PyCode::from(self)->codeInfo(), codeBlock)) {
        listAppend(globalObject, lines, PyTuple::create(globalObject, { jsNumber(shownOffset(run.start)), jsNumber(shownOffset(run.end)), run.line >= 0 ? jsNumber(run.line) : jsUndefined() }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, makeCodeIterator(globalObject, lineIteratorTypeDef, lines));
}

// (line, end line, column, end column) for each unit of co_code, which is to say for each byte of the instructions.
static JSValue codePositions(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // traceback.py asks for all of them for each frame of each traceback it formats. They are worked out once.
    if (JSValue known = hiddenField(globalObject, self, codePositionsField))
        RELEASE_AND_RETURN(scope, makeCodeIterator(globalObject, positionsIteratorTypeDef, asArray(known)));
    CodeBlock* codeBlock = codeBlockOf(globalObject, PyCode::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* positions = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    StringView text = codeBlock->source().provider()->source();
    Vector<unsigned> lineStarts { 0 };
    for (unsigned i = 0; i < text.length(); ++i) {
        if (text[i] == '\n')
            lineStarts.append(i + 1);
    }
    // Columns count the bytes of UTF-8, which is what the characters of the text are (compileTree()).
    auto locate = [&](unsigned offset, int& line, int& column) {
        offset = std::min(offset, text.length());
        unsigned index = std::ranges::upper_bound(lineStarts, offset) - lineStarts.begin() - 1;
        line = index + 1;
        column = offset - lineStarts[index];
    };
    auto& codeInfo = PyCode::from(self)->codeInfo();
    auto runs = lineRunsOf(codeInfo, codeBlock);
    int lineDelta = codeInfo.lineDelta;
    bool hasColumns = configuration().codeDebugRanges;
    // What sets up the frame is at the very start of the line the code begins on, as RESUME is in CPython.
    unsigned endOfPrologue = 0;
    for (auto& mark : codeBlock->unlinkedCodeBlock()->pythonMarks()) {
        if (mark.kind() == PythonMark::Start) {
            endOfPrologue = mark.offset;
            break;
        }
    }
    JSValue prologue;
    if (hasColumns && endOfPrologue)
        prologue = PyTuple::create(globalObject, { jsNumber(runs[0].line), jsNumber(std::max(runs[0].line, codeInfo.firstLineNumber)), jsNumber(0), jsNumber(0) });
    unsigned runIndex = 0;
    int lineOfLast = -2;
    JSValue last; // For a byte of which only the line is known.
    for (const auto& instruction : codeBlock->instructions()) {
        if (prologue && instruction.offset() < endOfPrologue) {
            for (unsigned i = 0; i < instruction->size(); ++i)
                listAppend(globalObject, positions, prologue);
            RETURN_IF_EXCEPTION(scope, { });
            continue;
        }
        // Only what may raise an exception has a place in the source recorded. What has none is given that of whatever comes before.
        auto info = codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(instruction.offset()));
        // With nothing before it either, it goes with what sets up the frame, as the first instructions of a class body do in CPython.
        bool hasPlace = info.startOffset || info.endOffset;
        int line, column, endLine, endColumn;
        locate(info.divot - info.startOffset, line, column);
        locate(info.divot + info.endOffset, endLine, endColumn);
        line += lineDelta;
        endLine += lineDelta;
        JSValue exact;
        for (unsigned offset = instruction.offset(); offset < instruction.offset() + instruction->size(); ++offset) {
            while (runIndex + 1 < runs.size() && runs[runIndex].end <= offset)
                ++runIndex;
            int lineOfByte = runs[runIndex].line;
            if (lineOfByte == line && hasColumns && hasPlace) {
                if (!exact)
                    exact = PyTuple::create(globalObject, { jsNumber(line), jsNumber(endLine), jsNumber(column), jsNumber(endColumn) });
                listAppend(globalObject, positions, exact);
            } else if (!hasPlace && prologue && lineOfByte == runs[0].line)
                listAppend(globalObject, positions, prologue);
            else {
                if (lineOfByte != std::exchange(lineOfLast, lineOfByte)) {
                    JSValue number = lineOfByte >= 0 ? jsNumber(lineOfByte) : jsUndefined();
                    last = PyTuple::create(globalObject, { number, number, jsUndefined(), jsUndefined() });
                }
                listAppend(globalObject, positions, last);
            }
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    setHiddenField(globalObject, self, codePositionsField, positions);
    RELEASE_AND_RETURN(scope, makeCodeIterator(globalObject, positionsIteratorTypeDef, positions));
}

// The instructions are JSC's, which nothing written for CPython can read. This is an outline of them in CPython's terms, for what goes
// through the code to find where things are: a unit for each byte (see shownOffset()), all NOP but for the RESUME that comes before the
// program's own code, which is how pdb finds the first line of a function, and what decides where the code goes on: jumps, branches,
// returns and the like. coverage.py follows those from where a branch went to the next line.
static JSValue codeGetCode(JSGlobalObject* globalObject, JSValue self)
{
    // Lib/_opcode_metadata.py. RESUME, JUMP_BACKWARD, POP_JUMP_IF_* and FOR_ITER have a unit of cache.
    static constexpr uint8_t cache = 0;
    static constexpr uint8_t nop = 25;
    static constexpr uint8_t resume = 128;
    static constexpr uint8_t extendedArg = 67;
    static constexpr uint8_t jumpForward = 76, jumpBackward = 74, popJumpIfTrue = 102, popJumpIfFalse = 99, forIter = 68;
    static constexpr uint8_t returnValue = 33, yieldValue = 119, raiseVarargs = 103, reraise = 104;
    static constexpr uint8_t loadAttr = 79, loadGlobal = 91, loadName = 92, storeAttr = 109, storeGlobal = 114, storeName = 115;
    static constexpr uint8_t importFrom = 71, importName = 72, loadCommonConstant = 80, loadConst = 81, loadSmallInt = 93;
    static constexpr unsigned cachesOfLoadAttr = 9, cachesOfLoadGlobal = 4, cachesOfStoreAttr = 4;
    static constexpr unsigned commonConstantNone = 7;

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue known = hiddenField(globalObject, self, codeBytesField))
        return known;
    CodeBlock* codeBlock = codeBlockOf(globalObject, PyCode::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    Vector<uint8_t> code;
    code.fill(0, shownOffset(codeBlock->instructions().size()));
    Monitoring::OriginalInstructions originalInstructions(runtime(globalObject).monitoring(), PyCode::from(self));
    {
        // The argument of a NOP means nothing. It is the byte the unit stands for, because programs take functions whose co_code is
        // the same to do the same (hypothesis drops a map(f) if f has the co_code of `lambda x: x`).
        auto* bytes = static_cast<const uint8_t*>(codeBlock->instructions().rawPointer());
        for (unsigned i = 0; i < code.size(); i += 2) {
            code[i] = nop;
            code[i + 1] = bytes[i / 2];
        }
    }
    unsigned start = codeBlock->instructions().size();
    for (auto& mark : codeBlock->unlinkedCodeBlock()->pythonMarks()) {
        if (mark.kind() != PythonMark::Start)
            continue;
        start = mark.offset;
        if (mark.offset >= 2) {
            for (unsigned offset : { mark.offset - 2, mark.offset - 1 })
                code[shownOffset(offset) + 1] = 0;
            code[shownOffset(mark.offset - 2)] = resume;
            code[shownOffset(mark.offset - 1)] = cache;
        }
        break;
    }
    auto putUnit = [&](unsigned offset, uint8_t opcode, uint8_t argument) {
        code[shownOffset(offset)] = opcode;
        code[shownOffset(offset) + 1] = argument;
    };
    // In the first `room` units of the instruction at `offset`. The distance is counted from after the caches.
    auto putJump = [&](unsigned offset, unsigned room, uint8_t forward, unsigned cachesOfForward, uint8_t backward, unsigned cachesOfBackward, unsigned target) {
        for (unsigned prefixes = 0; prefixes < 4; ++prefixes) {
            bool isForward = target >= offset + prefixes + 1 + cachesOfForward;
            if (!isForward && !backward)
                return;
            unsigned caches = isForward ? cachesOfForward : cachesOfBackward;
            unsigned from = offset + prefixes + 1 + caches;
            if (from - offset > room)
                return;
            uint64_t distance = isForward ? target - from : from - target;
            if (distance >> (8 * (prefixes + 1)))
                continue;
            for (unsigned i = 0; i < prefixes; ++i)
                putUnit(offset + i, extendedArg, distance >> (8 * (prefixes - i)));
            putUnit(offset + prefixes, isForward ? forward : backward, distance);
            for (unsigned i = 0; i < caches; ++i)
                putUnit(offset + prefixes + 1 + i, cache, 0);
            return;
        }
    };
    OpcodeID previous = op_nop;
    for (const auto& instruction : codeBlock->instructions()) {
        unsigned offset = instruction.offset();
        // BytecodeGenerator::emitOutOfLineCatchHandler()
        bool isPartOfCatch = std::exchange(previous, instruction->opcodeID()) == op_catch;
        if (offset < start || isPartOfCatch)
            continue;
        unsigned size = instruction->size();
        auto target = [&] {
            unsigned result = offset;
            extractStoredJumpTargetsForInstruction(codeBlock->unlinkedCodeBlock(), instruction, [&](int32_t relative) {
                result = offset + relative;
            });
            return result;
        };
        switch (instruction->opcodeID()) {
        case op_jmp:
        case op_py_jmp_back:
            putJump(offset, size, jumpForward, 0, jumpBackward, 1, target());
            break;
        case op_py_jtrue:
        case op_py_jeq:
        case op_py_jne:
        case op_py_jlt:
        case op_py_jle:
        case op_py_jgt:
        case op_py_jge:
            putJump(offset, size, popJumpIfTrue, 1, 0, 0, target());
            break;
        case op_py_jfalse:
        case op_py_jneq:
        case op_py_jnne:
        case op_py_jnlt:
        case op_py_jnle:
        case op_py_jngt:
        case op_py_jnge:
            putJump(offset, size, popJumpIfFalse, 1, 0, 0, target());
            break;
        case op_py_for_iter:
            // Its last unit is where it is said to go on.
            putJump(offset, size - 1, forIter, 1, 0, 0, target());
            break;
        case op_ret:
        case op_py_ret:
        case op_py_generator_return:
            putUnit(offset, returnValue, 0);
            break;
        case op_py_generator_suspend:
            putUnit(offset, yieldValue, 0);
            break;
        case op_throw:
            putUnit(offset, reraise, 0);
            break;
        case op_py_runtime:
            switch (static_cast<RuntimeFunction>(instruction->as<OpPyRuntime>().m_function)) {
            case RuntimeFunction::Raise:
                putUnit(offset, raiseVarargs, 0);
                break;
            case RuntimeFunction::Reraise:
                putUnit(offset, reraise, 0);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    // After all that, where no offset of the program's points: which names the code reads and writes and what it imports, in order,
    // for what goes through code to find that out (cloudpickle for the globals a function needs, inspect.getclosurevars(),
    // modulefinder). They do not fit in the units of their instructions: CPython's have caches behind them.
    auto& names = PyCode::from(self)->codeInfo().names;
    auto appendUnit = [&](uint8_t opcode, unsigned argument, unsigned caches = 0) {
        for (unsigned bits : { 24, 16, 8 }) {
            if (argument >> bits)
                code.appendList({ extendedArg, static_cast<uint8_t>(argument >> bits) });
        }
        code.appendList({ opcode, static_cast<uint8_t>(argument) });
        for (unsigned i = 0; i < caches; ++i)
            code.appendList({ cache, static_cast<uint8_t>(0) });
    };
    auto indexOfName = [&](JSValue name) -> size_t {
        if (!name || !name.isString())
            return notFound;
        const StringImpl* impl = asString(name)->tryGetValueImpl();
        return names.findIf([&](const Identifier& other) { return WTF::equal(other.impl(), impl); });
    };
    auto append = [&](uint8_t opcode, VirtualRegister nameRegister, unsigned shift, unsigned caches) {
        if (size_t index = indexOfName(codeBlock->getConstant(nameRegister)); index != notFound)
            appendUnit(opcode, static_cast<unsigned>(index) << shift, caches);
    };
    // The operands of an import are constants, which are moved to where the runtime takes them from right before it.
    struct Move {
        VirtualRegister destination;
        VirtualRegister constant;
    };
    Vector<Move, 4> moves;
    PyTuple* constants = nullptr;
    for (const auto& instruction : codeBlock->instructions()) {
        if (instruction->opcodeID() == op_mov) {
            auto move = instruction->as<OpMov>();
            if (move.m_src.isConstant())
                moves.append({ move.m_dst, move.m_src });
            else
                moves.clear();
            continue;
        }
        if (instruction->opcodeID() == op_py_runtime) {
            auto call = instruction->as<OpPyRuntime>();
            auto operand = [&](unsigned index) -> JSValue {
                for (auto& move : moves) {
                    if (move.destination.offset() == call.m_argv.offset() - static_cast<int>(index))
                        return codeBlock->getConstant(move.constant);
                }
                return { };
            };
            if (static_cast<RuntimeFunction>(call.m_function) == RuntimeFunction::ImportFrom) {
                if (size_t index = indexOfName(operand(1)); index != notFound)
                    appendUnit(importFrom, index);
            } else if (static_cast<RuntimeFunction>(call.m_function) == RuntimeFunction::ImportName) {
                size_t name = indexOfName(operand(0));
                JSValue fromList = operand(1);
                JSValue level = operand(2);
                JSValue mode = operand(3);
                size_t indexOfFromList = notFound;
                if (fromList && !isNone(fromList)) {
                    if (!constants) {
                        constants = PyTuple::from(codeGetConsts(globalObject, self));
                        RETURN_IF_EXCEPTION(scope, { });
                    }
                    indexOfFromList = std::ranges::find(constants->span(), fromList) - constants->span().begin();
                    if (indexOfFromList == constants->span().size())
                        fromList = { };
                }
                if (name != notFound && fromList && level && level.isInt32() && level.asInt32() >= 0 && level.asInt32() < 256 && mode && mode.isInt32()) {
                    appendUnit(loadSmallInt, level.asInt32());
                    if (isNone(fromList))
                        appendUnit(loadCommonConstant, commonConstantNone);
                    else
                        appendUnit(loadConst, indexOfFromList);
                    appendUnit(importName, static_cast<unsigned>(name) << 2 | mode.asInt32());
                }
            }
        }
        moves.clear();
        switch (instruction->opcodeID()) {
        case op_py_load_global:
            append(loadGlobal, instruction->as<OpPyLoadGlobal>().m_name, 1, cachesOfLoadGlobal);
            break;
        case op_py_store_global:
            append(storeGlobal, instruction->as<OpPyStoreGlobal>().m_name, 0, 0);
            break;
        case op_py_load_name:
            append(loadName, instruction->as<OpPyLoadName>().m_name, 0, 0);
            break;
        case op_py_store_name:
            append(storeName, instruction->as<OpPyStoreName>().m_name, 0, 0);
            break;
        case op_py_get_attr:
            append(loadAttr, instruction->as<OpPyGetAttr>().m_name, 1, cachesOfLoadAttr);
            break;
        case op_py_get_method:
            append(loadAttr, instruction->as<OpPyGetMethod>().m_name, 1, cachesOfLoadAttr);
            break;
        case op_py_set_attr:
            append(storeAttr, instruction->as<OpPySetAttr>().m_name, 0, cachesOfStoreAttr);
            break;
        default:
            break;
        }
    }
    JSValue result = PyBytes::create(globalObject, code.span());
    RETURN_IF_EXCEPTION(scope, { });
    setHiddenField(globalObject, self, codeBytesField, result);
    return result;
}

// (In CPython too it is a "line_iterator".)
// (where, where it goes on if it does not jump, where it jumps to) for each branch.
static JSValue codeBranches(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* branches = runtime(globalObject).monitoring().branches(globalObject, PyCode::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeCodeIterator(globalObject, lineIteratorTypeDef, branches));
}

// The name of a variable by its number among all of them. dis asks for the method whatever the code is like.
static JSValue codeVarNameFromOparg(JSGlobalObject* globalObject, JSValue self, JSValue opargValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int oparg = toCInt(globalObject, opargValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* locals = PyTuple::from(codeGetVarNames(globalObject, self));
    for (PyTuple* names : { locals, PyTuple::from(codeGetCellVars(globalObject, self)), PyTuple::from(codeGetFreeVars(globalObject, self)) }) {
        for (JSValue name : names->span()) {
            // A parameter that inner functions use is counted once.
            if (names != locals && locals->span().end() != std::ranges::find_if(locals->span(), [&](JSValue other) { return stringOf(strValue(other)) == stringOf(strValue(name)); }))
                continue;
            if (!oparg--)
                return name;
        }
    }
    return raise(globalObject, scope, runtime(globalObject).IndexError(), "tuple index out of range"_s);
}

static constexpr NativeDef codeMethods[] = {
    PY_NEW(codeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", codeRepr, "($self, /)"),
    PY_SLOT("__eq__", codeEq, "($self, value, /)"),
    PY_SLOT("__ne__", codeNe, "($self, value, /)"),
    PY_SLOT("__hash__", codeHash, "($self, /)"),
    PY_METHOD("replace", codeReplace, "($self, /, **changes)"),
    PY_METHOD("__replace__", codeReplace, "($self, /, **changes)"),
    PY_METHOD("co_lines", codeLines, "($self, /)"),
    PY_METHOD("co_positions", codePositions, "($self, /)"),
    PY_METHOD("co_branches", codeBranches, "($self, /)"),
    PY_METHOD("_varname_from_oparg", codeVarNameFromOparg, "($self, /, oparg)"),
};

static constexpr NativeGetSet codeGetSets[] = {
    PY_MEMBER("co_name", codeGetName),
    PY_MEMBER("co_qualname", codeGetQualname),
    PY_MEMBER("co_filename", codeGetFilename),
    PY_MEMBER("co_firstlineno", codeGetFirstLineNo),
    PY_MEMBER("co_argcount", codeGetArgCount),
    PY_MEMBER("co_posonlyargcount", codeGetPosOnlyArgCount),
    PY_MEMBER("co_kwonlyargcount", codeGetKwOnlyArgCount),
    PY_MEMBER("co_nlocals", codeGetNLocals),
    PY_MEMBER("co_stacksize", codeGetStackSize),
    PY_MEMBER("co_flags", codeGetFlags),
    PY_MEMBER("co_names", codeGetNames),
    { "co_varnames"_s, codeGetVarNames },
    { "co_cellvars"_s, codeGetCellVars },
    { "co_freevars"_s, codeGetFreeVars },
    PY_MEMBER("co_consts", codeGetConsts),
    { "co_code"_s, codeGetCode },
    { "_co_code_adaptive"_s, codeGetCode },
    PY_MEMBER("co_linetable", codeGetLineTable),
    PY_MEMBER("co_exceptiontable", codeGetEmptyBytes),
};

// ---- Functions written in C++, as Python sees them ----

static PyNativeFunction* asNative(JSValue value) { return uncheckedDowncast<PyNativeFunction>(value.asCell()); }

static JSValue nativeCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return asNative(self)->invoke(globalObject, args); }
static JSValue nativeGetName(JSGlobalObject* globalObject, JSValue self) { return internString(getVM(globalObject), asNative(self)->def().name); }

static JSValue nativeGetDoc(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    JSValue owner = native->owner();
    if (!owner)
        return jsUndefined();
    if (auto* type = PyType::tryFrom(owner))
        return docstringFor(globalObject, makeString(type->nameInMessages(), '.', native->def().name));
    if (owner.isString())
        return docstringFor(globalObject, makeString(stringOf(asString(owner)), '.', native->def().name));
    return jsUndefined();
}

static JSValue nativeGetTextSignature(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    auto& def = native->def();
    // Whatever the type: its own signature says what it takes.
    if (def.kind == NativeKind::New)
        return makeStr(getVM(globalObject), "($type, *args, **kwargs)"_s);
    StringView signatureText = def.signatureText;
    if (signatureText.startsWith('!'))
        signatureText = signatureText.substring(1);
    else if (def.signature.isVariadic) {
        // "(*args, **kwargs)" in a table only says that the function looks at its arguments itself. CPython has no signature for most
        // of those. Anything more than that is meant.
        StringView text = signatureText;
        for (ASCIILiteral part : { "("_s, "$self"_s, "$type"_s, ", "_s, "*args"_s, ", "_s, "**kwargs"_s, ")"_s }) {
            if (text.startsWith(part))
                text = text.substring(part.length());
        }
        if (text.isEmpty()) {
            // Those that stand for an operation have one all the same: __call__, __init__.
            if (def.kind == NativeKind::SlotWrapper)
                return makeStr(getVM(globalObject), "($self, /, *args, **kwargs)"_s);
            return jsUndefined();
        }
    }
    // An __init__ is shown as what stands for the operation, whatever it takes.
    if (def.kind == NativeKind::SlotWrapper && def.name == "__init__"_s)
        return makeStr(getVM(globalObject), "($self, /, *args, **kwargs)"_s);
    if (def.kind != NativeKind::Function || native->ownerType() || def.signature.hasSelf)
        return makeStr(globalObject, signatureText.toString());
    // A function of a module is bound to it, which the tables leave out: "(obj, /)" is "($module, obj, /)".
    StringView parameters = signatureText.substring(1, signatureText.length() - 2);
    if (parameters.isEmpty())
        return makeStr(getVM(globalObject), "($module, /)"_s);
    bool hasPositionalOnly = parameters == "/"_s || parameters.endsWith(", /"_s) || parameters.contains(", /, "_s);
    return makeStr(globalObject, makeString("($module, "_s, hasPositionalOnly ? ""_s : "/, "_s, parameters, ')'));
}

static JSValue nativeGetObjClass(JSGlobalObject*, JSValue self) { return asNative(self)->owner(); }

static JSValue nativeGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    // "list.append" for a method, bound or not; "len" for a function of a module.
    if (PyType* owner = native->ownerType())
        return makeStr(globalObject, makeString(stringOf(owner->qualname()), '.', native->def().name));
    return nativeGetName(globalObject, self);
}

// "Sub.append" for a method of list bound to an instance of Sub, "Sub.fromkeys" for one bound to the class.
static JSValue nativeFunctionGetQualname(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue bound = asNative(self)->self();
    if (!bound || PyModule::is(bound))
        RELEASE_AND_RETURN(scope, nativeGetQualname(globalObject, self));
    JSValue typeQualname = getAttr(globalObject, PyType::is(bound) ? bound : JSValue(typeOf(globalObject, bound)), runtime(globalObject).names.dQualname());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(typeQualname))
        return raiseTypeError(globalObject, scope, "<method>.__class__.__qualname__ is not a unicode object"_s);
    String prefix = strValue(typeQualname)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(prefix, '.', asNative(self)->def().name));
}

// What a built-in function is bound to: an object, or the module it belongs to. A static method says None, though it is shown and
// pickled as a method of its type.
static JSValue nativeGetSelf(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    if (JSValue bound = native->self())
        return bound;
    if (native->def().kind == NativeKind::StaticMethod && native->ownerType())
        return jsUndefined();
    JSValue owner = native->owner();
    if (owner && owner.isString()) {
        if (JSValue module = runtime(globalObject).modules()->get(asString(owner)))
            return module;
    }
    return jsUndefined();
}

// What a program assigns to the __module__ of a built-in function is kept as a property of the object, behind Python's back.
static JSValue nativeGetModule(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    VM& vm = getVM(globalObject);
    if (JSValue assigned = native->getDirect(vm, Identifier::fromString(vm, "__module__"_s)))
        return assigned;
    JSValue owner = native->owner();
    // A static method of a built-in type is of the module of the type.
    if (owner && PyType::is(owner) && native->kind() == NativeKind::Function && !native->self())
        return getAttr(globalObject, owner, runtime(globalObject).names.dModule());
    return owner && owner.isString() ? owner : jsUndefined();
}

static void nativeSetModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    asNative(self)->putDirect(vm, Identifier::fromString(vm, "__module__"_s), value ? value : jsUndefined());
}

static JSValue nativeFunctionRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    JSValue bound = native->self();
    if (!bound && native->def().kind == NativeKind::StaticMethod)
        bound = native->ownerType();
    if (!bound || PyModule::is(bound))
        return makeStr(globalObject, makeString("<built-in function "_s, native->def().name, '>'));
    return makeStr(globalObject, makeString("<built-in method "_s, native->def().name, " of "_s, typeName(globalObject, bound), " object at "_s, addressForRepr(globalObject, bound), '>'));
}

static JSValue methodWrapperRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    return makeStr(globalObject, makeString("<method-wrapper '"_s, native->def().name, "' of "_s, typeName(globalObject, native->self()), " object at "_s, addressForRepr(globalObject, native->self()), '>'));
}

static JSValue methodDescriptorRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    return makeStr(globalObject, descriptorRepr(globalObject, "method"_s, native->def().name, native->ownerType()));
}

static JSValue wrapperDescriptorRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    return makeStr(globalObject, descriptorRepr(globalObject, "slot wrapper"_s, native->def().name, native->ownerType()));
}

template<bool wantEqual>
static JSValue nativeEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    // A function and a method-wrapper, say, are the same kind of object here and not to Python.
    auto* b = PyNativeFunction::tryFrom(other);
    if (!b || typeOf(globalObject, other) != typeOf(globalObject, self))
        return runtime(globalObject).notImplemented();
    auto* a = asNative(self);
    bool equal = &a->def() == &b->def() && a->kind() == b->kind() && (a->self() ? b->self() && isIdentical(a->self(), b->self()) : !b->self());
    return jsBoolean(equal == wantEqual);
}

static JSValue nativeHash(JSGlobalObject* globalObject, JSValue self)
{
    auto* native = asNative(self);
    uint64_t bits = std::bit_cast<uintptr_t>(&native->def());
    if (JSValue bound = native->self())
        bits ^= static_cast<uint64_t>(tryInt64(objectHash(globalObject, bound)).value_or(0));
    int64_t result = static_cast<int64_t>(bits >> 4);
    return makeInt(globalObject, result == -1 ? static_cast<int64_t>(-2) : result);
}

static JSValue nativeDescriptorGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* native = asNative(self);
    PyType* type = native->ownerType();
    if (native->kind() == NativeKind::ClassMethod) {
        JSValue cls = owner && !isNone(owner) ? owner : JSValue();
        if (!cls) {
            if (isNone(instance))
                return raiseTypeError(globalObject, scope, "descriptor '"_s, native->def().name, "' for type '"_s, type->nameInMessages(), "' needs either an object or a type"_s);
            cls = typeOf(globalObject, instance);
        }
        auto* given = PyType::tryFrom(cls);
        if (!given)
            return raiseTypeError(globalObject, scope, "descriptor '"_s, native->def().name, "' for type '"_s, type->nameInMessages(), "' needs a type, not a '"_s, typeName(globalObject, cls), "' as arg 2"_s);
        if (!given->isSubtypeOf(type))
            return raiseTypeError(globalObject, scope, "descriptor '"_s, native->def().name, "' requires a subtype of '"_s, type->nameInMessages(), "' but received '"_s, given->nameInMessages(), '\'');
        RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, self, { }, given));
    }
    if (isNone(instance))
        return self;
    if (!typeOf(globalObject, instance)->isSubtypeOf(type))
        return raiseTypeError(globalObject, scope, "descriptor '"_s, native->def().name, "' for '"_s, type->nameInMessages(), "' objects doesn't apply to a '"_s, typeName(globalObject, instance), "' object"_s);
    RELEASE_AND_RETURN(scope, bindDescriptor(globalObject, self, instance, type));
}

static JSValue nativeReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* native = asNative(self);
    JSValue bound = native->self();
    if (!bound && native->def().kind == NativeKind::StaticMethod)
        bound = native->ownerType();
    if (!bound || PyModule::is(bound))
        return nativeGetName(globalObject, self);
    JSValue getattr = currentBuiltin(globalObject, internString(vm, "getattr"_s));
    if (!getattr)
        return { };
    return PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { bound, nativeGetName(globalObject, self) }) });
}

static JSValue nativeDescriptorReduce(JSGlobalObject* globalObject, JSValue self)
{
    JSValue getattr = currentBuiltin(globalObject, internString(getVM(globalObject), "getattr"_s));
    if (!getattr)
        return { };
    return PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { nativeGetObjClass(globalObject, self), nativeGetName(globalObject, self) }) });
}

static constexpr NativeDef nativeFunctionMethods[] = {
    PY_SLOT("__call__", nativeCall, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", nativeFunctionRepr, "($self, /)"),
    PY_SLOT("__eq__", nativeEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", nativeEquality<false>, "($self, value, /)"),
    PY_SLOT("__hash__", nativeHash, "($self, /)"),
    PY_METHOD("__reduce__", nativeReduce, "($self, /)"),
};

static constexpr NativeGetSet nativeFunctionGetSets[] = {
    { "__name__"_s, nativeGetName },
    { "__qualname__"_s, nativeFunctionGetQualname },
    { "__self__"_s, nativeGetSelf },
    PY_WRITABLE_MEMBER("__module__", nativeGetModule, nativeSetModule),
    { "__doc__"_s, nativeGetDoc },
    { "__text_signature__"_s, nativeGetTextSignature },
};

static constexpr NativeDef methodWrapperMethods[] = {
    PY_SLOT("__call__", nativeCall, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", methodWrapperRepr, "($self, /)"),
    PY_SLOT("__eq__", nativeEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", nativeEquality<false>, "($self, value, /)"),
    PY_SLOT("__hash__", nativeHash, "($self, /)"),
    PY_METHOD("__reduce__", nativeReduce, "($self, /)"),
};

static constexpr NativeGetSet methodWrapperGetSets[] = {
    { "__name__"_s, nativeGetName },
    { "__qualname__"_s, nativeGetQualname },
    PY_MEMBER("__self__", nativeGetSelf),
    { "__objclass__"_s, nativeGetObjClass },
    { "__doc__"_s, nativeGetDoc },
    { "__text_signature__"_s, nativeGetTextSignature },
};

static constexpr NativeDef methodDescriptorMethods[] = {
    PY_SLOT("__call__", nativeCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", nativeDescriptorGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", methodDescriptorRepr, "($self, /)"),
    PY_METHOD("__reduce__", nativeDescriptorReduce, "($self, /)"),
};

static constexpr NativeDef classMethodDescriptorMethods[] = {
    PY_SLOT("__call__", nativeCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", nativeDescriptorGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", methodDescriptorRepr, "($self, /)"),
};

static constexpr NativeDef wrapperDescriptorMethods[] = {
    PY_SLOT("__call__", nativeCall, "($self, *args, **kwargs)"),
    PY_SLOT("__get__", nativeDescriptorGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__repr__", wrapperDescriptorRepr, "($self, /)"),
    PY_METHOD("__reduce__", nativeDescriptorReduce, "($self, /)"),
};

static constexpr NativeGetSet descriptorGetSets[] = {
    PY_MEMBER("__name__", nativeGetName),
    { "__qualname__"_s, nativeGetQualname },
    PY_MEMBER("__objclass__", nativeGetObjClass),
    { "__doc__"_s, nativeGetDoc },
    { "__text_signature__"_s, nativeGetTextSignature },
};

void initializeFunctionTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto define = [&](PyType* type, ASCIILiteral name, OptionSet<TypeFlag> flags, std::span<const NativeDef> methods, std::span<const NativeGetSet> getsets) {
        type->initializeBuiltin(globalObject, { name, flags, methods, getsets, { } }, rt.typeType(), rt.objectType());
    };
    define(rt.functionType(), "function"_s, { TypeFlag::HasDict, TypeFlag::HasWeakref }, functionMethods, functionGetSets);
    rt.functionType()->addGetSets(globalObject, dictGetSet());
    define(rt.methodType(), "method"_s, { TypeFlag::HasWeakref }, methodMethods, methodGetSets);
    define(rt.cellType(), "cell"_s, { }, cellMethods, cellGetSets);
    rt.cellType()->dict()->set(rt.vm(), rt.name(Special::Hash), jsUndefined());
    rt.cellType()->didChange(rt.vm());
    define(rt.codeType(), "code"_s, { TypeFlag::HasWeakref }, codeMethods, codeGetSets);
    define(rt.nativeFunctionType(), "builtin_function_or_method"_s, { TypeFlag::HasWeakref }, nativeFunctionMethods, nativeFunctionGetSets);
    define(rt.methodWrapperType(), "method-wrapper"_s, { }, methodWrapperMethods, methodWrapperGetSets);
    define(rt.methodDescriptorType(), "method_descriptor"_s, { }, methodDescriptorMethods, descriptorGetSets);
    define(rt.classMethodDescriptorType(), "classmethod_descriptor"_s, { }, classMethodDescriptorMethods, descriptorGetSets);
    define(rt.wrapperDescriptorType(), "wrapper_descriptor"_s, { }, wrapperDescriptorMethods, descriptorGetSets);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
