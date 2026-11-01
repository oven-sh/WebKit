/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeCallValidation.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTNameClass.h"
#include "AOTObject.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "GlobalObjectMethodTable.h"
#include "InternalFunction.h"
#include "IntlDateTimeFormat.h"
#include "IntlNumberFormat.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSCustomGetterFunction.h"
#include "JSCustomSetterFunction.h"
#include "JSFunctionWithFields.h"
#include "NativeExecutable.h"
#include "NullGetterFunction.h"
#include "NullSetterFunction.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <mutex>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

enum class NativeCallDeclarationSource : uint8_t {
    Compiler,
    CalleeInRealm,
    NativeCodeOfCallee,
    NameOfModuleMember,
    NotInTable,
};

struct NativeCallDeclaration {
    NativeCallDeclarationSource source { NativeCallDeclarationSource::NotInTable };
    BuiltinID builtin;
    Vector<const BuiltinOverload*, 4> overloads;
};

using BuiltinsByName = UncheckedKeyHashMap<String, Vector<BuiltinID, 1>>;

static const BuiltinsByName& builtinsByFunctionName()
{
    static LazyNeverDestroyed<BuiltinsByName> index;
    static std::once_flag once;
    std::call_once(once, [] {
        index.construct();
        const BuiltinTable& table = BuiltinTable::singleton();
        for (unsigned number = 0; number < table.builtinCount(); ++number) {
            BuiltinID id(number);
            const Builtin& builtin = table.at(id);
            if (!builtin.overloadCount || !builtin.name)
                continue;
            String text = String::fromUTF8(table.textOf(builtin.name));
            if (table.isSymbolName(builtin.name))
                text = makeString("[Symbol."_s, text, ']');
            index->add(text, Vector<BuiltinID, 1>()).iterator->value.append(id);
        }
    });
    return index.get();
}

static String nameOfNativeFunction(JSObject* callee)
{
    if (auto* function = dynamicDowncast<JSFunction>(callee))
        return function->isHostFunction() ? uncheckedDowncast<NativeExecutable>(function->executable())->name() : String();
    if (auto* function = dynamicDowncast<InternalFunction>(callee))
        return function->name();
    return { };
}

// The host makes a function object of a module's built-in for whoever asks, so nothing leads from the realm to one. All of them have the native code that the host has bound to the built-in.
static BuiltinID findBuiltinWithNativeCodeOf(const BuiltinTable& table, JSObject* callee, BuiltinScope scope)
{
    auto* function = dynamicDowncast<JSFunction>(callee);
    if (!function || !function->isHostFunction())
        return { };
    TaggedNativeFunction code = uncheckedDowncast<NativeExecutable>(function->executable())->function();
    for (const BuiltinNativeFunctionBinding& binding : BuiltinBindings::nativeFunctionsOfHost()) {
        if (table.at(binding.builtin).scope == scope && code == TaggedNativeFunction(binding.function))
            return binding.builtin;
    }
    return { };
}

// `formatter.format` answers a function that the accessor has bound to the formatter. Whoever bound it has made its target too, and nothing leads from the realm to that: the formatter keeps the bound function.
// Flow takes the accessor for the target, and what a call does stands among the overloads of the accessor.
struct FunctionBoundByAccessor {
    JSBoundFunction* function { nullptr };
    BuiltinID accessor;
};

static FunctionBoundByAccessor findFunctionBoundByAccessorWithTarget(const NativeCall& call)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto ifCalleeIsTarget = [&](JSBoundFunction* bound, BuiltinID accessor) -> FunctionBoundByAccessor {
        if (!bound || bound->targetFunction() != call.callee || bound->boundArgsLength() > call.arguments.size() || !table.answersFunctionBoundToReceiver(table.canonicalID(accessor)))
            return { };
        return { bound, accessor };
    };
    if (call.kind != CodeSpecializationKind::CodeForCall)
        return { };
    if (auto* formatter = dynamicDowncast<IntlNumberFormat>(call.thisValue))
        return ifCalleeIsTarget(formatter->boundFormat(), Builtins::IntlNumberFormatPrototypeFormat);
    if (auto* formatter = dynamicDowncast<IntlDateTimeFormat>(call.thisValue))
        return ifCalleeIsTarget(formatter->boundFormat(), Builtins::IntlDateTimeFormatPrototypeFormat);
    return { };
}

// A formatter with a pattern has the pattern bound in front of what the caller passes. The overloads speak of what the caller passes.
static NativeCall withoutArgumentsBoundByAccessor(const NativeCall& call)
{
    FunctionBoundByAccessor bound = findFunctionBoundByAccessorWithTarget(call);
    if (!bound.function)
        return call;
    NativeCall callOfBoundFunction = call;
    callOfBoundFunction.arguments = ArgList(call.arguments.data() + bound.function->boundArgsLength(), call.arguments.size() - bound.function->boundArgsLength());
    return callOfBoundFunction;
}

// Nothing leads from the realm to the constructor of a class that only a native binding hands out. The host keeps the Structure of the instances, and the prototype object names the constructor.
static BuiltinID findConstructorOfHostClass(const BuiltinTable& table, JSGlobalObject* realm, JSObject* callee)
{
    if (callee->inherits<JSFunction>())
        return { };
    VM& vm = realm->vm();
    BuiltinID found;
    BuiltinBindings::forEachInstanceStructureOfHost(realm, [&](const BuiltinClassBinding& binding, Structure* structure) {
        BuiltinID constructor = table.at(binding.builtinClass).constructor;
        JSObject* prototype = constructor && !found ? structure->storedPrototypeObject() : nullptr;
        if (prototype && prototype->getDirect(vm, vm.propertyNames->constructor) == JSValue(callee))
            found = constructor;
    });
    return found;
}

// Whoever asks for the description of an accessor of native code gets new function objects for its halves.
static bool isSameHalfOfNativeAccessor(JSCell* found, JSObject* callee)
{
    if (auto* getter = dynamicDowncast<JSCustomGetterFunction>(callee)) {
        auto* other = dynamicDowncast<JSCustomGetterFunction>(found);
        return other && other->getter() == getter->getter() && other->propertyName() == getter->propertyName();
    }
    if (auto* setter = dynamicDowncast<JSCustomSetterFunction>(callee)) {
        auto* other = dynamicDowncast<JSCustomSetterFunction>(found);
        return other && other->setter() == setter->setter() && other->propertyName() == setter->propertyName();
    }
    return false;
}

static bool isValueOfProperty(JSGlobalObject* realm, BuiltinID property, JSObject* callee)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto holder = findInRealm(realm, BuiltinCellKey::object(table.at(property).holder));
    if (!holder || !(*holder)->isObject())
        return false;
    auto value = findValueInHolder(realm, asObject(*holder), table.identifierOf(realm->vm(), table.at(property).name));
    return value && *value == JSValue(callee);
}

static BuiltinCallMode modeOf(BuiltinCellKey::Kind kind, CodeSpecializationKind specialization)
{
    switch (kind) {
    case BuiltinCellKey::Kind::Object:
        return specialization == CodeSpecializationKind::CodeForConstruct ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    case BuiltinCellKey::Kind::Getter:
    case BuiltinCellKey::Kind::Value:
        return BuiltinCallMode::Get;
    case BuiltinCellKey::Kind::Setter:
        return BuiltinCallMode::Set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class Fit : uint8_t { Outside, Unknown, Inside };

static Fit fitOfInstance(const BuiltinTable& table, const BuiltinTypePart& part, JSObject* object)
{
    std::optional<bool> passes = passesInstanceTest(table, part.builtinClass, object);
    if (passes && !*passes)
        return Fit::Outside;
    bool isNarrowed = !part.pristine.isEmpty() || !part.contents.empty() || part.isSettled;
    return passes && !isNarrowed ? Fit::Inside : Fit::Unknown;
}

static Fit fitOfObject(const BuiltinTable& table, const BuiltinType& type, JSObject* object)
{
    if (type.flags.contains(BuiltinTypeFlag::HasAnyObject))
        return Fit::Inside;
    if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction) && object->isCallable())
        return Fit::Inside;
    Fit fit = type.flags.contains(BuiltinTypeFlag::HasPlainData) ? Fit::Unknown : Fit::Outside;
    for (const BuiltinTypePart& part : type.parts)
        fit = std::max(fit, part.kind == BuiltinTypePartKind::Instance ? fitOfInstance(table, part, object) : Fit::Unknown);
    return fit;
}

static Fit fitOf(const BuiltinTable& table, BuiltinTypeID id, std::optional<JSValue> passed)
{
    const BuiltinType& type = table.at(id);
    if (type.isAnything())
        return Fit::Inside;
    auto has = [&](BuiltinPrimitive primitive) {
        return type.primitives.contains(primitive) ? Fit::Inside : Fit::Outside;
    };
    if (!passed)
        return std::max(has(BuiltinPrimitive::NotPassed), has(BuiltinPrimitive::Undefined));
    JSValue value = *passed;
    if (value.isObject())
        return fitOfObject(table, type, asObject(value));
    using enum BuiltinPrimitive;
    BuiltinPrimitive primitive = value.isUndefined() ? Undefined : value.isNull() ? Null : value.isBoolean() ? Boolean : value.isSymbol() ? Symbol : value.isBigInt() ? BigInt : value.isNumber() ? Number : String;
    bool isNarrowed = primitive == Number ? type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || !type.integers.isEmpty() : primitive == String && !type.stringLiterals.empty();
    return isNarrowed ? Fit::Unknown : has(primitive);
}

static Fit fitOf(const BuiltinTable& table, const BuiltinParameter& parameter, std::optional<JSValue> passed)
{
    if (!passed && parameter.mode != BuiltinParameterMode::Required)
        return Fit::Inside;
    Fit fit = fitOf(table, parameter.accepted, passed);
    if (fit != Fit::Outside || parameter.conversion == BuiltinConversion::None)
        return fit;
    return fitOf(table, parameter.refused, passed) == Fit::Outside ? Fit::Inside : Fit::Unknown;
}

static Fit fitOf(const BuiltinTable& table, const BuiltinOverload& overload, const NativeCall& call)
{
    Fit fit = Fit::Inside;
    if (overload.receiver && overload.mode != BuiltinCallMode::Construct)
        fit = fitOf(table, *overload.receiver, call.thisValue);
    for (const BuiltinParameter& parameter : overload.parameters) {
        unsigned first = parameter.operand.argumentIndex();
        if (first >= call.arguments.size()) {
            fit = std::min(fit, fitOf(table, parameter, std::nullopt));
            continue;
        }
        unsigned end = parameter.operand.kind() == BuiltinOperand::Kind::ArgumentsFrom ? call.arguments.size() : first + 1;
        for (unsigned index = first; index < end; ++index)
            fit = std::min(fit, fitOf(table, parameter, call.arguments.at(index)));
    }
    return fit;
}

// The first overload that takes the operands is the one the call is. Where that cannot be told, a later one may be it as well.
static void appendOverloads(NativeCallDeclaration& declaration, BuiltinID builtin, BuiltinCallMode mode, const NativeCall& call)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    size_t sizeBefore = declaration.overloads.size();
    for (const BuiltinOverload& overload : table.overloadsOf(builtin)) {
        if (overload.mode != mode)
            continue;
        Fit fit = overload.isUndeclared() ? Fit::Unknown : fitOf(table, overload, call);
        if (fit != Fit::Outside)
            declaration.overloads.append(&overload);
        if (fit == Fit::Inside)
            return;
    }
    if (declaration.overloads.size() != sizeBefore)
        return;
    for (const BuiltinOverload& overload : table.overloadsOf(builtin)) {
        if (overload.mode == mode)
            declaration.overloads.append(&overload);
    }
}

// May create objects of the realm that are created on demand.
static NativeCallDeclaration findDeclaration(const NativeCall& call, const NativeCallExpectation& expectation, BuiltinID accessorThatBoundCallee)
{
    using enum NativeCallDeclarationSource;
    using Kind = BuiltinCellKey::Kind;
    const BuiltinTable& table = BuiltinTable::singleton();
    NativeCallDeclaration declaration;
    auto declare = [&](NativeCallDeclarationSource source, BuiltinID builtin, BuiltinCallMode mode) {
        if (!builtin)
            return false;
        declaration.source = source;
        declaration.builtin = builtin;
        appendOverloads(declaration, source == NameOfModuleMember ? builtin : table.canonicalID(builtin), mode, call);
        return true;
    };
    if (expectation.overload && table.contains(expectation.overload)) {
        const BuiltinOverload& overload = table.at(expectation.overload);
        if (!expectation.matchesAnyOverload)
            return { Compiler, overload.builtin, { &overload } };
        declare(Compiler, overload.builtin, overload.mode);
        return declaration;
    }

    if (declare(NativeCodeOfCallee, findBuiltinWithNativeCodeOf(table, call.callee, BuiltinScope::Module), modeOf(Kind::Object, call.kind)))
        return declaration;

    if (declare(CalleeInRealm, accessorThatBoundCallee, BuiltinCallMode::Call))
        return declaration;

    // The program may have replaced a step of the path to the function, such as `console.log`. Compiled code that names the function has it in a cell.
    if (Instance* instance = Instance::of(call.realm); instance && instance->state) {
        if (std::optional<BuiltinCellKey> key = instance->state->builtinCells->keyOfCellHolding(call.callee)) {
            declare(CalleeInRealm, key->builtin(), modeOf(key->kind(), call.kind));
            return declaration;
        }
    }

    String name = nameOfNativeFunction(call.callee);
    Kind kind = name.startsWith("get "_s) ? Kind::Getter : name.startsWith("set "_s) ? Kind::Setter : Kind::Object;
    if (kind != Kind::Object)
        name = name.substring(4);
    // The native code of such a half has the name of the property.
    else if (call.callee->inherits<JSCustomGetterFunction>())
        kind = Kind::Getter;
    else if (call.callee->inherits<JSCustomSetterFunction>())
        kind = Kind::Setter;
    BuiltinCallMode mode = modeOf(kind, call.kind);
    if (name.isEmpty()) {
        // Function.prototype is the one function of the realm that has no name.
        if (kind == Kind::Object && call.callee == call.realm->functionPrototype())
            declare(CalleeInRealm, Builtins::FunctionPrototype, mode);
        return declaration;
    }
    auto declareConstructorOfHostClass = [&] {
        return kind == Kind::Object && declare(CalleeInRealm, findConstructorOfHostClass(table, call.realm, call.callee), mode);
    };
    auto candidates = builtinsByFunctionName().find(name);
    if (candidates == builtinsByFunctionName().end()) {
        declareConstructorOfHostClass();
        return declaration;
    }

    // A spy of bun:test stands in the realm where the function stood that it spies on, under the name of that function, and runs what the program gave it.
    auto mayBeInRealm = [&](BuiltinID candidate) {
        return table.at(candidate).scope != BuiltinScope::Module && (call.callee->inherits<JSFunction>() || table.at(candidate).kind == BuiltinKind::Constructor);
    };
    for (BuiltinID candidate : candidates->value) {
        BuiltinCellKey key = kind == Kind::Object ? BuiltinCellKey::object(candidate) : kind == Kind::Getter ? BuiltinCellKey::getter(candidate) : BuiltinCellKey::setter(candidate);
        // Compiled code has no cell for the function of an accessor that it never calls by itself. A generic read or store calls it all the same.
        bool canBeFound = mayBeInRealm(candidate) && (kind == Kind::Object ? key.hasCell(table) : table.at(candidate).kind == BuiltinKind::Accessor);
        auto found = canBeFound ? findInRealm(call.realm, key) : makeUnexpected(BuiltinLookupFailure::IsMissing);
        if (found && (*found == call.callee || isSameHalfOfNativeAccessor(*found, call.callee))) {
            declare(CalleeInRealm, candidate, mode);
            return declaration;
        }
        // A property that the program may replace initially holds a native function, and its entry describes what a call to that function does.
        if (kind == Kind::Object && table.at(candidate).kind == BuiltinKind::DataProperty && mayBeInRealm(candidate) && isValueOfProperty(call.realm, candidate, call.callee)) {
            declare(CalleeInRealm, candidate, mode);
            return declaration;
        }
    }

    // Nothing leads from the realm to what a native module exports either: a function of the realm that was not found there is known by its native code.
    if (declare(NativeCodeOfCallee, findBuiltinWithNativeCodeOf(table, call.callee, BuiltinScope::Realm), modeOf(Kind::Object, call.kind)) || declareConstructorOfHostClass())
        return declaration;

    // The program may have replaced a step of the path to the function, such as `console`. The call names the built-ins that flow lists as its callees.
    for (BuiltinID candidate : candidates->value) {
        if (mayBeInRealm(candidate) && expectation.isKnown && !expectation.namesMoreBuiltins && expectation.names(table.canonicalID(candidate))) {
            declare(CalleeInRealm, candidate, mode);
            return declaration;
        }
    }

    // Nothing leads from the realm to a member of a module, so every member of that name may be the callee. One whose native code the host has bound was looked for by that code.
    for (BuiltinID candidate : candidates->value) {
        if (table.at(candidate).scope == BuiltinScope::Module && !std::ranges::contains(BuiltinBindings::nativeFunctionsOfHost(), candidate, &BuiltinNativeFunctionBinding::builtin))
            declare(NameOfModuleMember, candidate, mode);
    }
    return declaration;
}

static constexpr unsigned maxCheckedArguments = 16;

#if !defined(BUN_HARDENED)
struct CheckedNativeCalls {
    static CheckedNativeCalls* ifAskedFor()
    {
        static CheckedNativeCalls* calls;
        static std::once_flag once;
        std::call_once(once, [] {
            const char* path = getenv("BUN_AOT_CHECKED_NATIVE_CALLS");
            if (!path || !*path)
                return;
            calls = new CheckedNativeCalls { .path = Vector<char>(unsafeMakeSpan(path, strlen(path) + 1)) };
            RuntimeReporter::callAtExit(writeCheckedNativeCalls);
        });
        return calls;
    }

    Vector<char> path;
    Lock lock;
    // How many calls of an entry returned, threw, changed an operand, and came with what the compiler believed about the call.
    HashMap<String, std::array<uint64_t, 4>> counts;
};
#endif

void writeCheckedNativeCalls()
{
#if !defined(BUN_HARDENED)
    CheckedNativeCalls* calls = CheckedNativeCalls::ifAskedFor();
    if (!calls)
        return;
    Locker locker { calls->lock };
    Vector<String> entries = copyToVector(calls->counts.keys());
    AOT::sort(entries, codePointCompareLessThan);
    StringPrintStream out;
    for (const String& entry : entries) {
        auto [returned, threw, changedOperand, hadExpectation] = calls->counts.get(entry);
        out.print(entry, "\t", returned, "\t", threw, "\t", changedOperand, "\t", hadExpectation, "\n");
    }
    calls->counts.clear();
    auto text = out.toString().utf8();
    FILE* file = text.length() ? fopen(calls->path.span().data(), "ab") : nullptr;
    if (!file)
        return;
    // One write, so that the lines of two processes do not mix.
    setvbuf(file, nullptr, _IOFBF, text.length() + 1);
    fwrite(text.data(), 1, text.length(), file);
    fclose(file);
#endif
}

NativeCallExpectation NativeCallExpectation::take(JSGlobalObject* realm, JSObject* callee)
{
    Instance* instance = Instance::of(realm);
    if (!instance || instance->scratch.expectedCall.callee != callee)
        return { };
    ExpectedCallWords& words = instance->scratch.expectedCall;
    words.callee = nullptr;
    return words.site == overwrittenSite ? NativeCallExpectation() : decode(words);
}

static bool isBuiltinIterator(JSObject* object)
{
    static constexpr JSType types[] { JSArrayIteratorType, JSMapIteratorType, JSSetIteratorType, JSStringIteratorType, JSRegExpStringIteratorType, JSIteratorHelperType, JSWrapForValidIteratorType, JSGeneratorType };
    return std::ranges::contains(types, object->type());
}

static bool declares(BuiltinPropertyActionKind action, OperandChange change)
{
    switch (action) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
        return change == OperandChange::PropertyAdded || change == OperandChange::PropertyStored;
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::DefinesFrom:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        return change == OperandChange::PropertyAdded || change == OperandChange::PropertyStored || change == OperandChange::AttributesChanged;
    case BuiltinPropertyActionKind::DeletesByKey:
        return change == OperandChange::PropertyRemoved;
    case BuiltinPropertyActionKind::ChangesIntegrity:
        return change == OperandChange::AttributesChanged;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String describe(BuiltinOperand position)
{
    return position.isReceiver() ? "this"_str : makeString("argument "_s, position.argumentIndex());
}

// The analysis opens the operands of a function that the host's modules reach under a private name of the global object.
static bool isPrivateGlobal(JSGlobalObject* realm, JSObject* callee)
{
    auto isPrivateName = [](UniquedStringImpl* key) {
        return key->isSymbol() && static_cast<SymbolImpl*>(key)->isPrivate();
    };
    SymbolTable* variables = realm->symbolTable();
    ConcurrentJSLocker locker(variables->m_lock);
    for (auto entry = variables->begin(locker); entry != variables->end(locker); ++entry) {
        if (isPrivateName(entry->key.get()) && realm->variableAt(entry->value.scopeOffset()).get() == JSValue(callee))
            return true;
    }
    bool isFound = false;
    realm->structure()->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        isFound = isPrivateName(entry.key()) && realm->getDirect(entry.offset()) == JSValue(callee);
        return !isFound;
    });
    return isFound;
}

struct CodeReturnedByNativeBindings {
    Lock lock;
    WTF::Vector<std::pair<uint32_t, TaggedNativeFunction>> entries WTF_GUARDED_BY_LOCK(lock);
};

static CodeReturnedByNativeBindings& codeReturnedByNativeBindings()
{
    static NeverDestroyed<CodeReturnedByNativeBindings> code;
    return code;
}

void noteNativeBindingResult(uint32_t hostNumber, JSValue answer)
{
    CodeReturnedByNativeBindings& code = codeReturnedByNativeBindings();
    Locker locker { code.lock };
    auto note = [&](JSValue value) WTF_REQUIRES_LOCK(code.lock) {
        auto* function = dynamicDowncast<JSFunction>(value);
        if (!function || !function->isHostFunction())
            return;
        std::pair<uint32_t, TaggedNativeFunction> entry { hostNumber, function->nativeFunction() };
        if (!code.entries.contains(entry))
            code.entries.append(entry);
    };
    note(answer);
    if (!answer.isObject())
        return;
    JSObject* holder = asObject(answer);
    // A binding may answer its functions in an array.
    if (auto* array = dynamicDowncast<JSArray>(holder)) {
        for (unsigned index = 0; index < array->length(); ++index) {
            if (array->canGetIndexQuickly(index))
                note(array->getIndexQuickly(index));
        }
    }
    holder->structure()->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) WTF_REQUIRES_LOCK(code.lock) {
        note(holder->getDirect(entry.offset()));
        return true;
    });
}

// If unseen code may run, an object held by native code may change in any way, and no proof relies on the rule.
static bool isAnsweredBy(BuiltinID binding, JSObject* callee)
{
    auto* function = dynamicDowncast<JSFunction>(callee);
    std::optional<uint16_t> hostNumber = BuiltinBindings::hostNumberOf(binding);
    if (!function || !function->isHostFunction() || !hostNumber)
        return false;
    CodeReturnedByNativeBindings& code = codeReturnedByNativeBindings();
    Locker locker { code.lock };
    return code.entries.contains(std::pair<uint32_t, TaggedNativeFunction> { *hostNumber, function->nativeFunction() });
}

// Nothing leads from the realm to what a native binding answers, and the compiler does not name the binding at the call.
static bool isDeclaredByNativeBinding(JSObject* callee, OperandUses declaring)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        const Builtin& builtin = table.at(BuiltinID(index));
        if (builtin.kind != BuiltinKind::NativeBinding)
            continue;
        bool declares = std::ranges::any_of(table.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& overload) {
            return !overload.isUndeclared() && std::ranges::any_of(overload.parameters, [&](const BuiltinParameter& parameter) {
                return parameter.uses.containsAny(declaring);
            });
        });
        if (declares && isAnsweredBy(BuiltinID(index), callee))
            return true;
    }
    return false;
}

class NativeCallValidationScope::Checker {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Checker);
    WTF_MAKE_NONCOPYABLE(Checker);
public:
    Checker(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
        : m_vm(vm)
        , m_call(withoutArgumentsBoundByAccessor(call))
        , m_expectation(expectation)
    {
        DeferGCForAWhile deferGC(vm);
        m_declaration = findDeclaration(m_call, m_expectation, findFunctionBoundByAccessorWithTarget(call).accessor);

        if (call.kind == CodeSpecializationKind::CodeForCall)
            addOperand(BuiltinOperand::receiver(), call.thisValue);
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), maxCheckedArguments); ++index)
            addOperand(BuiltinOperand::argument(index), m_call.arguments.at(index));
        checkOperandsOfUnknownFunction();

        m_collectionVersions = collectionVersionsOf(m_vm);
        m_didEnterVMBefore = std::exchange(vm.didEnterVM, false);
        if (Instance* instance = Instance::of(call.realm))
            m_didEnterCompiledCodeBefore = std::exchange(instance->scratch.didEnterCompiledCode, 0);
        m_outer = std::exchange(s_innermost, this);
    }

    bool finish(JSValue result)
    {
        s_innermost = m_outer;
        // What throws allocates the error that it throws: compiled code expects a collection on the way to a handler, whatever the call is said to do.
        bool didCollect = collectionVersionsOf(m_vm) != m_collectionVersions && !m_vm.exceptionForInspection();
        bool didEnterCompiledCode = false;
        if (Instance* instance = Instance::of(m_call.realm)) {
            didEnterCompiledCode = instance->scratch.didEnterCompiledCode;
            instance->scratch.didEnterCompiledCode |= m_didEnterCompiledCodeBefore;
        }
        // The stub through which native code enters compiled code sets it as an entry scope of the engine does.
        bool didRunCode = m_vm.didEnterVM;
        m_vm.didEnterVM |= m_didEnterVMBefore;

        DeferGCForAWhile deferGC(m_vm);
        if (didEnterCompiledCode && m_expectation.isKnown && !m_expectation.mayEnterCompiledCode)
            fail(ValidationFailureKind::NativeCallRanCode, m_call.callee, "the call enters no compiled code"_s);
        if (didRunCode && !mayRunCode())
            fail(ValidationFailureKind::NativeCallRanCode, m_call.callee, "the entry runs no code with these operands"_s);
        if (didCollect && m_expectation.isKnown && !m_expectation.mayCollect)
            fail(ValidationFailureKind::NativeCallCollected, m_call.callee, "the call does not collect"_s);
        // The conversion of an object operand allocates whether or not it enters JavaScript, and the capability of the entry says nothing of it.
        else if (didCollect && !didRunCode && !mayCollect() && !mayRunCode())
            fail(ValidationFailureKind::NativeCallCollected, m_call.callee, "the entry does not allocate"_s);
        // An entry that may run code through an operand may as well get to a function of the table that way: Reflect.set(o, "__proto__", p) calls the native setter. What that writes is its own doing.
        bool othersMayHaveWritten = didRunCode || (m_didCallFunctionThatWrites && mayRunCode());
        checkException(othersMayHaveWritten);
        checkChanges(othersMayHaveWritten);
        if (!othersMayHaveWritten)
            checkResult(result);
        if (m_outer && result && result.isObject())
            m_outer->m_resultsOfCalledFunctions.append(asObject(result));
        bool countsAsCode = didRunCode || m_declaration.overloads.isEmpty();
        if (m_outer && !countsAsCode && (m_didCallFunctionThatWrites || !declaredWrites().isEmpty()))
            m_outer->m_didCallFunctionThatWrites = true;
#if !defined(BUN_HARDENED)
        if (CheckedNativeCalls* calls = CheckedNativeCalls::ifAskedFor(); calls && m_declaration.source != NativeCallDeclarationSource::NotInTable) {
            Locker locker { calls->lock };
            auto& [returned, threw, changedOperand, hadExpectation] = calls->counts.add(describeCallee().isolatedCopy(), std::array<uint64_t, 4> { }).iterator->value;
            ++(m_vm.exceptionForInspection() ? threw : returned);
            changedOperand += m_didChangeOperand;
            hadExpectation += m_expectation.isKnown;
        }
#endif
        return countsAsCode;
    }

private:
    struct Operand {
        BuiltinOperand position;
        JSObject* object;
        OperandSnapshot before;
    };

    void addOperand(BuiltinOperand position, JSValue value)
    {
        if (!canCapture(value))
            return;
        JSObject* object = asObject(value);
        // What is declared for either position may happen to an object that is passed twice.
        if (!std::ranges::contains(m_operands, object, &Operand::object))
            m_operands.append({ position, object, capture(object) });
    }

    // The analysis hands the operands of a callee it does not know to native code, whatever the callee turns out to be.
    bool isTakenForUnknownFunction() const
    {
        if (m_declaration.source == NativeCallDeclarationSource::Compiler || m_declaration.source == NativeCallDeclarationSource::NotInTable)
            return m_declaration.source == NativeCallDeclarationSource::NotInTable;
        return m_expectation.isKnown && !m_expectation.names(m_table.canonicalID(m_declaration.builtin));
    }

    // { transfer: [buffer] }: the member itself, or an element of the array that it holds.
    bool holdsUnderNamedMember(const BuiltinParameter& parameter, JSObject* holder, JSObject* held) const
    {
        for (BuiltinName name : parameter.namedMembers) {
            Identifier key = m_table.identifierOf(m_vm, name);
            PropertySlot slot(holder, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
            if (!holder->methodTable()->getOwnPropertySlot(holder, m_call.realm, key, slot) || !slot.isValue())
                continue;
            JSValue member = slot.getValue(m_call.realm, key);
            if (member == JSValue(held))
                return true;
            if (!parameter.protocols.contains(Protocol::Iterate) || !isJSArray(member))
                continue;
            JSArray* array = asArray(member);
            for (unsigned index = 0; index < array->length(); ++index) {
                if (array->canGetIndexQuickly(index) && array->getIndexQuickly(index) == JSValue(held))
                    return true;
            }
        }
        return false;
    }

    // Of one of the operands.
    OperandUses usesOf(JSObject* object) const
    {
        OperandUses uses;
        for (const BuiltinOverload* overload : m_declaration.overloads) {
            if (overload->isUndeclared())
                return OperandUses::all();
            auto addUsesAt = [&](BuiltinOperand position, JSValue operand) {
                const BuiltinParameter* parameter = operand.isObject() ? m_table.parameterFor(*overload, position) : nullptr;
                // What is declared for an operand and for what it reaches may happen to another operand that it holds.
                bool holdsObject = parameter && !position.isReceiver() && parameter->uses.contains(OperandUse::AppliesTransitively) && holdsUnderNamedMember(*parameter, asObject(operand), object);
                if (parameter && (operand == JSValue(object) || holdsObject))
                    uses.add(parameter->uses);
                // A function that iterates steps a built-in iterator without a call of `next`, whose entry has the write.
                if (parameter && operand == JSValue(object) && parameter->protocols.contains(Protocol::Iterate) && isBuiltinIterator(object))
                    uses.add(OperandUse::WritesContents);
            };
            if (m_call.kind == CodeSpecializationKind::CodeForCall)
                addUsesAt(BuiltinOperand::receiver(), m_call.thisValue);
            for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index)
                addUsesAt(BuiltinOperand::argument(index), m_call.arguments.at(index));
        }
        return uses;
    }

    bool isAt(BuiltinOperand position, JSObject* object) const
    {
        if (position.isReceiver())
            return m_call.kind == CodeSpecializationKind::CodeForCall && m_call.thisValue == JSValue(object);
        for (unsigned index = position.argumentIndex(); index < m_call.arguments.size(); ++index) {
            if (position.coversArgument(index) && m_call.arguments.at(index) == JSValue(object))
                return true;
        }
        return false;
    }

    bool covers(const BuiltinKey& key, UniquedStringImpl* name) const
    {
        switch (key.kind) {
        case BuiltinKeyKind::None:
        case BuiltinKeyKind::Any:
        case BuiltinKeyKind::ElementsOf:
            return true;
        case BuiltinKeyKind::Indexes: {
            // As flow has it (Program::isIndexLike()): an index, or another canonical numeric string: an array-like whose length is past the limit of arrays takes its elements under such names.
            if (name->isSymbol())
                return false;
            CString text = String(name).utf8();
            return Typed::mayBeElementKey(Typed::classifyName(byteCast<uint8_t>(text.span())));
        }
        case BuiltinKeyKind::Names:
            return std::ranges::any_of(key.names, [&](BuiltinName listed) {
                return m_table.identifierOf(m_vm, listed).impl() == name;
            });
        case BuiltinKeyKind::Operand: {
            JSValue operand = !key.operand ? JSValue() : key.operand->isReceiver() ? m_call.thisValue : m_call.arguments.at(key.operand->argumentIndex());
            if (!operand || operand.isObject())
                return true;
            if (operand.isSymbol())
                return &asSymbol(operand)->uid() == name;
            return !name->isSymbol() && operand.toWTFString(m_call.realm) == StringView(name);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool hasActionOn(JSObject* object, OperandChange change, UniquedStringImpl* key) const
    {
        return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            return overload->isUndeclared() || std::ranges::any_of(overload->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return declares(action.kind, change) && isAt(action.object, object) && covers(action.key, key);
            });
        });
    }

    bool anyParameterWithOperand(const BuiltinOverload& overload, const Invocable<bool(const BuiltinParameter&, JSValue)> auto& predicate) const
    {
        if (overload.receiver && predicate(*overload.receiver, m_call.thisValue))
            return true;
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
            const BuiltinParameter* parameter = m_table.parameterFor(overload, BuiltinOperand::argument(index));
            if (parameter && predicate(*parameter, m_call.arguments.at(index)))
                return true;
        }
        return false;
    }

    AbstractHeaps declaredWrites() const
    {
        AbstractHeaps writes;
        for (const BuiltinOverload* overload : m_declaration.overloads) {
            bool mayRunAnyCode = overload->effects.capability == BuiltinCapability::JS && !overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
            if (overload->isUndeclared() || mayRunAnyCode)
                return AbstractHeaps::fromRaw((1ull << numberOfEnumerators<AbstractHeap>) - 1);
            writes.add(overload->effects.writes);
        }
        return writes;
    }

    bool mayRunCode() const
    {
        if (m_declaration.overloads.isEmpty())
            return true;
        // A constructor reads `prototype` from another new.target, which may be a proxy or have an accessor there. The frame has it in the place of `this`.
        if (m_call.kind == CodeSpecializationKind::CodeForConstruct && m_call.thisValue.isObject() && m_call.thisValue != JSValue(m_call.callee))
            return true;

        const Builtin& builtin = m_table.at(m_declaration.builtin);
        if (builtin.flags.containsAny({ BuiltinFlag::LoadsModuleByName, BuiltinFlag::EvaluatesText, BuiltinFlag::RunsUnseenCode }))
            return true;
        if (!m_table.calledFunctionsOf(m_declaration.builtin).empty() || !m_table.moduleLoadsOf(m_declaration.builtin).empty() || m_table.unseenCodeOf(m_declaration.builtin))
            return true;

        for (const BuiltinOverload* overload : m_declaration.overloads) {
            if (overload->isUndeclared())
                return true;
            // The capability is that of the function behind the conversions of its operands.
            bool runsOnlyConversions = overload->effects.capability != BuiltinCapability::JS;
            constexpr OperandUses usesRunningSettersAndTraps { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DeletesByKey, OperandUse::TakesDescriptor };
            auto mayRunCodeThrough = [&](const BuiltinParameter& parameter, JSValue operand) {
                if (operand.isObject())
                    return !parameter.protocols.isEmpty() || parameter.conversion != BuiltinConversion::None || parameter.uses.containsAny(usesRunningSettersAndTraps) || parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused);
                return !runsOnlyConversions && !operand.isUndefinedOrNull() && !parameter.protocols.isEmpty();
            };
            if (anyParameterWithOperand(*overload, mayRunCodeThrough))
                return true;
            if (runsOnlyConversions)
                continue;
            if (!overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
                return true;
            if (!std::ranges::all_of(overload->flow.callbacks, isCalledAfterReturn, &BuiltinCallback::times) || !std::ranges::all_of(overload->flow.handlerCalls, isCalledAfterReturn, &BuiltinHandlerCall::times))
                return true;
        }
        return false;
    }

    // A conversion throws for an operand outside of what the parameter accepts. Only the overload that the compiler matched is known to fit the operands.
    bool mayThrow() const
    {
        if (m_declaration.source != NativeCallDeclarationSource::Compiler || m_expectation.matchesAnyOverload)
            return true;
        return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            bool mayRunAnyCode = overload->effects.capability == BuiltinCapability::JS && !overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
            return overload->throws() || mayRunAnyCode || anyParameterWithOperand(*overload, [](const BuiltinParameter& parameter, JSValue operand) {
                return !parameter.protocols.isEmpty() && operand.isObject() && asObject(operand)->type() == ProxyObjectType;
            });
        });
    }

    bool mayCollect() const
    {
        return m_declaration.overloads.isEmpty() || std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            if (overload->isUndeclared() || overload->effects.capability != BuiltinCapability::None)
                return true;
            // The capability is that of the function behind the conversions of its operands.
            return anyParameterWithOperand(*overload, [](const BuiltinParameter& parameter, JSValue operand) {
                return operand.isString() && readsCharactersOfString(parameter.conversion);
            });
        });
    }

    String describeCallee() const
    {
        switch (m_declaration.source) {
        case NativeCallDeclarationSource::Compiler:
            if (m_expectation.matchesAnyOverload)
                return makeString(m_table.tokenOf(m_declaration.builtin), "/any"_s);
            return m_table.tokenOf(m_expectation.overload);
        case NativeCallDeclarationSource::CalleeInRealm:
        case NativeCallDeclarationSource::NativeCodeOfCallee:
            return makeString(m_table.tokenOf(m_declaration.builtin), isTakenForUnknownFunction() ? "/not-named-by-the-call"_s : "/any"_s);
        case NativeCallDeclarationSource::NameOfModuleMember:
            return makeString(m_table.tokenOf(m_declaration.builtin), "/by-name"_s);
        case NativeCallDeclarationSource::NotInTable:
            return makeString("function-not-in-table '"_s, nameOfNativeFunction(m_call.callee), "' ("_s, m_call.callee->classInfo()->className, ')');
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void fail(ValidationFailureKind kind, JSValue found, const String& message)
    {
        reportOrCrash(m_vm, { kind, m_expectation.function, m_expectation.value, found }, "native call"_s, describeCallee(), message);
    }

    void failForUnknownFunction(JSValue found, OperandUses declaring, const String& message)
    {
        bool restsOnNativeCode = m_expectation.isKnown && m_expectation.trustsNativeCode && !isPrivateGlobal(m_call.realm, m_call.callee) && !isHelperOfLibraryFunctions();
        if (!restsOnNativeCode)
            noteChange(makeString(message, " (no proof of this program rests on it)"_s));
        else if (!isDeclaredByNativeBinding(m_call.callee, declaring))
            fail(ValidationFailureKind::NativeCallChangedOperand, found, message);
    }

    void noteChange(const String& message)
    {
        if (RuntimeReporter* reporter = RuntimeReporter::ifExplicitlyEnabled(m_vm))
            reporter->reportNativeCall(ValidationFailureKind::NativeCallChangedOperand, describeCallee(), message);
    }

    // A library function calls these, and where flow goes by the entry or the model of that function it does not see the call.
    bool isHelperOfLibraryFunctions() const
    {
        Instance* instance = Instance::of(m_call.realm);
        return instance && std::ranges::contains(instance->realm.linkTimeConstants, JSValue::encode(m_call.callee));
    }

    // Flow hands the operands of a function it does not know to native code.
    void checkOperandsOfUnknownFunction()
    {
        // A bound function hands its operands on to its target, and a proxy to its target or its trap, which flow calls in its place.
        if (m_declaration.source != NativeCallDeclarationSource::NotInTable || isHelperOfLibraryFunctions() || m_call.callee->inherits<JSBoundFunction>() || m_call.callee->type() == ProxyObjectType)
            return;
        for (const Operand& operand : m_operands) {
            Object* compiled = compiledObjectOrNull(operand.object);
            if (compiled && compiled->layout().impossibleMutations().contains(ObjectMutation::ReachNativeCode))
                fail(ValidationFailureKind::ImpossibleMutation, compiled, makeString(describe(operand.position), ": layout "_s, compiled->layout().index, " never reaches native code"_s));
        }
    }

    // Two views of one buffer, and the buffer itself, hold the same bytes: what the call writes through one of them shows in the others.
    OperandUses usesOfBytesOf(const Operand& operand) const
    {
        OperandUses uses = usesOf(operand.object);
        auto endOf = [](const Operand& each) {
            return static_cast<const uint8_t*>(each.before.bytes) + each.before.byteLength;
        };
        for (const Operand& other : m_operands) {
            if (other.object != operand.object && operand.before.bytes < endOf(other) && other.before.bytes < endOf(operand))
                uses.add(usesOf(other.object));
        }
        return uses;
    }

    // A class of the host may give its objects a [[Set]], a [[Delete]] or a [[DefineOwnProperty]] of its own, and a property may have a setter of native code. To an entry that stores, deletes or defines by key they are what
    // the traps of a proxy and the setters of the program are: the passes take the call for one that runs code nobody knows, and flow goes by what the table says of the object or of the accessor, as it does for a statement.
    // Neither has a function object, so no call of it is checked, and none counts as code that ran. What it does to the properties of its own object is its own doing.
    bool isDoneByNativeCodeOfObject(const Operand& operand, const OperandDifference& difference, OperandUses uses) const
    {
        constexpr OperandUses usesStoring { OperandUse::StoresByKey, OperandUse::AssignsFrom };
        constexpr OperandUses usesDefining { OperandUse::DefinesName, OperandUse::TakesDescriptor };
        if (difference.change != OperandChange::PropertyAdded && difference.change != OperandChange::PropertyRemoved && difference.change != OperandChange::PropertyStored && difference.change != OperandChange::AttributesChanged)
            return false;
        // The classes of the engine have types of their own, and what their methods do is the language's.
        if (operand.object->type() == ObjectType) {
            const MethodTable* methods = operand.object->methodTable();
            if (uses.containsAny(usesStoring) && methods->put != JSObject::put)
                return true;
            MethodTable::DeletePropertyFunctionPtr ordinaryDelete = JSObject::deleteProperty;
            if (uses.contains(OperandUse::DeletesByKey) && methods->deleteProperty != ordinaryDelete)
                return true;
            if (uses.containsAny(usesDefining) && methods->defineOwnProperty != JSObject::defineOwnProperty)
                return true;
        }
        return difference.key && uses.containsAny(usesStoring) && operand.before.properties.containsIf([&](const CapturedProperty& property) {
            return property.key == difference.key && property.attributes & PropertyAttribute::CustomAccessorOrValue;
        });
    }

    // An entry that reads a member by name enters an accessor of native code above the object, as a statement would. That has no function object either. What it lays on the object that it is entered with stands in its own entry, and is what it answers.
    bool isDoneByNativeGetterAbove(const Operand& operand, const OperandDifference& difference) const
    {
        if (difference.change != OperandChange::PropertyAdded || !difference.key)
            return false;
        UniquedStringImpl* key = difference.key.get();
        JSObject* holder = nullptr;
        for (JSValue above = operand.object->getPrototypeDirect(); above.isObject() && !holder; above = asObject(above)->getPrototypeDirect()) {
            PropertySlot slot(asObject(above), PropertySlot::InternalMethodType::VMInquiry, &m_vm);
            if (!asObject(above)->methodTable()->getOwnPropertySlot(asObject(above), m_call.realm, Identifier::fromUid(m_vm, key), slot))
                continue;
            if (!(slot.attributes() & PropertyAttribute::CustomAccessor))
                return false;
            holder = asObject(above);
        }
        if (!holder)
            return false;
        JSValue laid = operand.object->getDirect(m_vm, Identifier::fromUid(m_vm, key));
        auto laysOnReceiver = [&](BuiltinName name) {
            for (uint32_t index = 0; index < m_table.builtinCount(); ++index) {
                const Builtin& entry = m_table.at(BuiltinID(index));
                if (entry.kind != BuiltinKind::Accessor || entry.name != name)
                    continue;
                auto inRealm = findInRealm(m_call.realm, BuiltinCellKey::object(entry.holder));
                if (!inRealm || *inRealm != holder)
                    continue;
                return std::ranges::any_of(m_table.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& getter) {
                    return getter.mode == BuiltinCallMode::Get && fitOf(m_table, getter.result.type, laid) != Fit::Outside && std::ranges::any_of(getter.flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                        return declares(action.kind, difference.change) && action.object.isReceiver() && covers(action.key, key);
                    });
                });
            }
            return false;
        };
        return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            auto readsAt = [&](BuiltinOperand position, JSValue passed) {
                const BuiltinParameter* parameter = passed == JSValue(operand.object) ? m_table.parameterFor(*overload, position) : nullptr;
                return parameter && parameter->protocols.contains(Protocol::Named) && std::ranges::any_of(parameter->namedMembers, [&](BuiltinName name) {
                    return m_table.identifierOf(m_vm, name).impl() == key && laysOnReceiver(name);
                });
            };
            if (m_call.kind == CodeSpecializationKind::CodeForCall && readsAt(BuiltinOperand::receiver(), m_call.thisValue))
                return true;
            for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
                if (readsAt(BuiltinOperand::argument(index), m_call.arguments.at(index)))
                    return true;
            }
            return false;
        });
    }

    // What the operand's own `next` would do. The call site's writes, which hold those of that code, are checked above.
    bool isSteppedAsDeclared(const Operand& operand, const OperandDifference& difference) const
    {
        return difference.change == OperandChange::InternalStateChanged && isBuiltinIterator(operand.object) && std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            const BuiltinParameter* parameter = m_table.parameterFor(*overload, operand.position);
            return parameter && parameter->protocols.contains(Protocol::Iterate);
        });
    }

    void checkChanges(bool didRunCode)
    {
        AbstractHeaps declared = declaredWrites();
        bool hasNoDeclaration = !m_declaration.overloads.isEmpty() && std::ranges::all_of(m_declaration.overloads, &BuiltinOverload::isUndeclared);
        for (const Operand& operand : m_operands) {
            Vector<OperandDifference> differences = differencesBetween(m_vm, operand.object, operand.before, capture(operand.object));
            if (differences.isEmpty())
                continue;
            m_didChangeOperand = true;
            for (const OperandDifference& difference : differences) {
                // As compiled code takes it.
                bool isOfProperty = difference.change == OperandChange::PropertyAdded || difference.change == OperandChange::PropertyStored || difference.change == OperandChange::PropertyRemoved;
                OperandChange change = isOfProperty && difference.key && isJSArray(operand.object) && isCanonicalNumericIndexString(difference.key.get()) ? OperandChange::ElementsChanged : difference.change;
                OperandUses uses = difference.change == OperandChange::BytesChanged ? usesOfBytesOf(operand) : usesOf(operand.object);
                // A resolving function settles the promise that it was made for. Handed that very promise, it rejects it: no change of what it is handed as such.
                if (difference.change == OperandChange::InternalStateChanged && operand.object->type() == JSPromiseType && m_call.callee->inherits<JSFunctionWithFields>())
                    continue;
                String what = makeString(describe(operand.position), ": "_s, describe(difference));
                AbstractHeaps heaps = difference.change == OperandChange::InternalStateChanged ? heapsOfClassOf(operand.object) : rowOf(change).heaps;

                if (m_expectation.isKnown && !heaps.isEmpty() && !m_expectation.writes.containsAny(heaps))
                    fail(ValidationFailureKind::NativeCallWroteHeap, operand.object, makeString(what, ", the call writes "_s, describeMembers(m_expectation.writes), ", not "_s, describeMembers(heaps)));
                if (didRunCode || isDoneByNativeCodeOfObject(operand, difference, uses) || isDoneByNativeGetterAbove(operand, difference))
                    continue;
                OperandUses declaring = difference.isRepresentationOnly ? OperandUses() : rowOf(change).uses;
                if (!declaring.isEmpty() && isTakenForUnknownFunction()) {
                    // Flow analysis opens the contents of a container passed to native code, no type describes the length of a buffer or view, and the call writes every heap: Worker.prototype.postMessage detaches the buffers in its transfer list.
                    bool isOfBytes = difference.change == OperandChange::InternalStateChanged || difference.change == OperandChange::BytesChanged;
                    if (isOfBytes && (operand.object->type() == ArrayBufferType || isTypedArrayTypeIncludingDataView(operand.object->type()))) {
                        noteChange(makeString(what, " (no proof rests on the bytes of what native code gets)"_s));
                        continue;
                    }
                    failForUnknownFunction(operand.object, declaring, makeString(what, ", and native code changes nothing it is handed"_s));
                    continue;
                }
                // Flow opens the operands it knows of an overload without a declaration, so no proof about this operand is wrong. What the unknown object stands for there keeps its proofs.
                if (!declaring.isEmpty() && hasNoDeclaration) {
                    noteChange(makeString(what, ", and the overload has no declaration"_s));
                    continue;
                }
                checkArrivedValue(operand, difference);
                if (!declaring.isEmpty() && !uses.containsAny(declaring))
                    fail(ValidationFailureKind::NativeCallChangedOperand, operand.object, makeString(what, ", uses "_s, describeMembers(uses), ", needs "_s, describeMembers(declaring)));
                else if (change == difference.change && difference.key && uses != OperandUses::all() && !hasActionOn(operand.object, difference.change, difference.key.get()))
                    fail(ValidationFailureKind::NativeCallChangedOperand, operand.object, makeString(what, ", no property action covers the key"_s));
                else if (!m_declaration.overloads.isEmpty() && !heaps.isEmpty() && !declared.containsAny(heaps) && !isSteppedAsDeclared(operand, difference))
                    fail(ValidationFailureKind::NativeCallWroteHeap, operand.object, makeString(what, ", the entry writes "_s, describeMembers(declared), ", not "_s, describeMembers(heaps)));
            }
        }
    }

    // The value that the object itself has where the other object changed. Empty where it has none.
    JSValue ownValueAt(JSObject* object, const OperandDifference& difference) const
    {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
        if (!difference.key) {
            if (!object->methodTable()->getOwnPropertySlotByIndex(object, m_call.realm, *difference.index, slot) || !slot.isValue())
                return { };
            return slot.getValue(m_call.realm, static_cast<uint64_t>(*difference.index));
        }
        PropertyName key(difference.key.get());
        if (!object->methodTable()->getOwnPropertySlot(object, m_call.realm, key, slot) || !slot.isValue())
            return { };
        return slot.getValue(m_call.realm, key);
    }

    // A definition reads `value`, `get` and `set` as a statement would: the description has them, or an object above it.
    bool describes(const BuiltinParameter& parameter, JSValue description, JSObject* described) const
    {
        if (!description || !description.isObject())
            return false;
        return std::ranges::any_of(parameter.namedMembers, [&](BuiltinName name) {
            Identifier key = m_table.identifierOf(m_vm, name);
            PropertySlot slot(description, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
            return asObject(description)->getPropertySlot(m_call.realm, key, slot) && slot.isValue() && slot.getValue(m_call.realm, key) == JSValue(described);
        });
    }

    // The description is an operand, or what a map of descriptions has under the key.
    bool isDescribedFor(JSObject* holder, JSObject* arrived, const OperandDifference& difference) const
    {
        return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            return std::ranges::any_of(overload->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                bool takesMap = action.kind == BuiltinPropertyActionKind::TakesDescriptorMap;
                if ((!takesMap && action.kind != BuiltinPropertyActionKind::TakesDescriptor) || action.value.kind != BuiltinPlaceKind::Operand || !isAt(action.object, holder))
                    return false;
                BuiltinOperand position = *action.value.operand;
                const BuiltinParameter* parameter = m_table.parameterFor(*overload, position);
                JSValue operand = position.isReceiver() ? m_call.thisValue : m_call.arguments.at(position.argumentIndex());
                if (!parameter || !parameter->uses.contains(OperandUse::RetainsContents) || !operand.isObject())
                    return false;
                return describes(*parameter, takesMap ? ownValueAt(asObject(operand), difference) : operand, arrived);
            });
        });
    }

    // What an operand holds may be another operand, or the operand itself. A copy of its properties hands that on under its key, and a definition hands on what the description holds.
    bool isAmongRetainedContents(JSObject* holder, JSObject* arrived, const OperandDifference& difference) const
    {
        if (!difference.key && !difference.index)
            return false;
        bool isCopied = std::ranges::any_of(m_operands, [&](const Operand& source) {
            return usesOf(source.object).contains(OperandUse::RetainsContents) && ownValueAt(source.object, difference) == JSValue(arrived);
        });
        return isCopied || isDescribedFor(holder, arrived, difference);
    }

    // An operand that another operand holds after the call is retained.
    void checkArrivedValue(const Operand& holder, const OperandDifference& difference)
    {
        // The bits may be those of an unboxed number, so they are only compared.
        for (const Operand& operand : m_operands) {
            if (JSValue::encode(operand.object) != difference.arrived || operand.object == holder.object)
                continue;
            OperandUses uses = usesOf(operand.object);
            if (!uses.contains(OperandUse::Retains) && !isAmongRetainedContents(holder.object, operand.object, difference))
                fail(ValidationFailureKind::NativeCallKeptOperand, operand.object, makeString(describe(operand.position), ": "_s, describe(holder.position), " holds it, uses "_s, describeMembers(uses)));
        }
    }

    // What an operand holds may be another operand, or the operand itself.
    bool mayReturnHeldValue() const
    {
        return std::ranges::any_of(m_declaration.overloads, [](const BuiltinOverload* overload) {
            return overload->isUndeclared()
                || std::ranges::contains(overload->flow.callbacks, BuiltinCallbackResultUse::Returned, &BuiltinCallback::resultUse)
                || std::ranges::any_of(overload->flow.propertyActions, [](const BuiltinPropertyAction& action) {
                    return action.value.kind == BuiltinPlaceKind::Result;
                })
                || std::ranges::any_of(overload->flow.transfers, [](const BuiltinTransfer& transfer) {
                    return transfer.to.kind == BuiltinPlaceKind::Result && transfer.from.kind != BuiltinPlaceKind::Operand;
                });
        });
    }

    void checkResult(JSValue result)
    {
        if (!result || !result.isObject() || isTakenForUnknownFunction() || mayReturnHeldValue())
            return;
        // An entry that forwards the result of a member of an operand may find a table function there: Date.prototype.toJSON calls whatever `toISOString` resolves to. Flow analysis calls that function, and its result is its own responsibility.
        if (m_resultsOfCalledFunctions.contains(asObject(result)) && mayRunCode())
            return;
        // A result declared as anything stands for every object that native code holds, and it holds an operand that it retains.
        bool mayReturnRetainedOperand = std::ranges::all_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
            return m_table.at(overload->result.type).isAnything();
        });
        for (const Operand& operand : m_operands) {
            if (operand.object != asObject(result))
                continue;
            OperandUses uses = usesOf(operand.object);
            if (mayReturnRetainedOperand && uses.contains(OperandUse::Retains))
                continue;
            if (!uses.contains(OperandUse::Returns))
                fail(ValidationFailureKind::NativeCallKeptOperand, operand.object, makeString(describe(operand.position), ": it is the result, uses "_s, describeMembers(uses)));
        }
    }

    void checkException(bool didRunCode)
    {
        Exception* exception = m_vm.exceptionForInspection();
        if (!exception || m_vm.isTerminationException(exception))
            return;
        if (auto* error = dynamicDowncast<ErrorInstance>(exception->value()); error && (error->isStackOverflowError() || error->isOutOfMemoryError()))
            return;
        if (m_expectation.isKnown && !m_expectation.mayThrow)
            fail(ValidationFailureKind::NativeCallThrew, exception->value(), "the call does not throw"_s);
        else if (!didRunCode && !mayThrow())
            fail(ValidationFailureKind::NativeCallThrew, exception->value(), "the entry does not throw"_s);
    }

    VM& m_vm;
    const BuiltinTable& m_table { BuiltinTable::singleton() };
    NativeCall m_call;
    NativeCallExpectation m_expectation;
    NativeCallDeclaration m_declaration;
    Vector<Operand, 4> m_operands;
    CollectionVersions m_collectionVersions;
    bool m_didEnterVMBefore { false };
    bool m_didEnterCompiledCodeBefore { false };
    // The call in the course of which this one is made.
    Checker* m_outer { nullptr };
    bool m_didCallFunctionThatWrites { false };
    // They are only compared.
    Vector<JSObject*, 2> m_resultsOfCalledFunctions;
    bool m_didChangeOperand { false };
    static thread_local Checker* s_innermost;
};

thread_local NativeCallValidationScope::Checker* NativeCallValidationScope::Checker::s_innermost;

NEVER_INLINE void NativeCallValidationScope::open(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
{
    JSC_REMOVABLE_DEFINITION(ImageCompiler, &NativeCallValidationScope::open);
    // The engine fills the missing half of an accessor pair with a function of its own. No program can name it, and a call of it is the read or the store that finds no accessor.
    // The function object of a compiled closure has native code that enters the compiled code, or that throws where a class is called without `new`. What it is handed goes to compiled code or nowhere.
    if (is<NullGetterFunction>(call.callee) || is<NullSetterFunction>(call.callee) || CompiledCallee::isCompiledCallee(call.callee))
        return;
    m_checker = new Checker(vm, call, expectation);
}

NEVER_INLINE bool NativeCallValidationScope::close()
{
    JSC_REMOVABLE_DEFINITION(ImageCompiler, &NativeCallValidationScope::close);
    bool didRunCode = m_checker->finish(m_result);
    delete std::exchange(m_checker, nullptr);
    return didRunCode;
}

static TaggedNativeFunction nativeFunctionOf(JSObject* callee, CodeSpecializationKind kind)
{
    if (auto* function = dynamicDowncast<JSFunction>(callee))
        return kind == CodeSpecializationKind::CodeForCall ? function->nativeFunction() : function->nativeConstructor();
    return uncheckedDowncast<InternalFunction>(callee)->nativeFunctionFor(kind);
}

static EncodedJSValue callUnderValidation(JSGlobalObject* realm, CallFrame* callFrame, CodeSpecializationKind kind)
{
    JSObject* callee = callFrame->jsCallee();
    NativeCallValidationScope validation(realm->vm(), { realm, callee, kind, callFrame->thisValue(), ArgList(callFrame) });
    EncodedJSValue result = nativeFunctionOf(callee, kind)(realm, callFrame);
    validation.didReturn(JSValue::decode(result));
    return result;
}

JSC_DEFINE_HOST_FUNCTION(callNativeFunctionUnderValidation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return callUnderValidation(realm, callFrame, CodeSpecializationKind::CodeForCall);
}

JSC_DEFINE_HOST_FUNCTION(constructNativeFunctionUnderValidation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return callUnderValidation(realm, callFrame, CodeSpecializationKind::CodeForConstruct);
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
