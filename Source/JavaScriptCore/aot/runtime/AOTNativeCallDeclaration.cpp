/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeCallDeclaration.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "FunctionPrototype.h"
#include "InternalFunction.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "NativeExecutable.h"
#include <algorithm>
#include <mutex>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

namespace JSC { namespace AOT {

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
            index->ensure(text, [] {
                return Vector<BuiltinID, 1>();
            }).iterator->value.append(id);
        }
    });
    return index.get();
}

String nameOfNativeFunction(JSObject* callee)
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

// Nothing leads from the realm to the constructor of a class that only a native binding hands out. The host keeps the Structure of the instances, and the prototype object names the constructor.
static BuiltinID findConstructorOfHostClass(const BuiltinTable& table, JSGlobalObject* realm, JSObject* callee)
{
    if (callee->inherits<JSFunction>())
        return { };
    VM& vm = realm->vm();
    for (const BuiltinClassBinding& binding : BuiltinBindings::classesOfHost()) {
        BuiltinID constructor = table.at(binding.builtinClass).constructor;
        Structure* structure = constructor && binding.findInstanceStructure ? binding.findInstanceStructure(realm) : nullptr;
        JSObject* prototype = structure ? structure->storedPrototypeObject() : nullptr;
        if (prototype && prototype->getDirect(vm, vm.propertyNames->constructor) == JSValue(callee))
            return constructor;
    }
    return { };
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
    const BuiltinClass& builtinClass = table.at(part.builtinClass);
    bool passes = false;
    switch (builtinClass.instanceTest) {
    case BuiltinInstanceTest::None:
        return Fit::Unknown;
    case BuiltinInstanceTest::JSTypeEquals:
        passes = object->type() == builtinClass.type;
        break;
    case BuiltinInstanceTest::ClassInfoInherits: {
        const ClassInfo* info = BuiltinBindings::instanceClassInfoOf(part.builtinClass);
        if (!info)
            return Fit::Unknown;
        passes = object->inherits(info);
        break;
    }
    case BuiltinInstanceTest::SharingModeEquals: {
        auto* buffer = dynamicDowncast<JSArrayBuffer>(object);
        passes = buffer && buffer->sharingMode() == builtinClass.sharingMode;
        break;
    }
    }
    if (!passes)
        return Fit::Outside;
    bool isNarrowed = !part.pristine.isEmpty() || !part.contents.empty() || part.isSettled;
    return isNarrowed ? Fit::Unknown : Fit::Inside;
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
    if (value.isUndefined())
        return has(BuiltinPrimitive::Undefined);
    if (value.isNull())
        return has(BuiltinPrimitive::Null);
    if (value.isBoolean())
        return has(BuiltinPrimitive::Boolean);
    if (value.isSymbol())
        return has(BuiltinPrimitive::Symbol);
    if (value.isBigInt())
        return has(BuiltinPrimitive::BigInt);
    if (value.isNumber()) {
        bool isNarrowed = type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || !type.integers.isEmpty();
        return isNarrowed ? Fit::Unknown : has(BuiltinPrimitive::Number);
    }
    return type.stringLiterals.empty() ? has(BuiltinPrimitive::String) : Fit::Unknown;
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

NativeCallDeclaration findDeclaration(const NativeCall& call, const NativeCallExpectation& expectation)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    NativeCallDeclaration declaration;
    if (expectation.overload && table.contains(expectation.overload)) {
        const BuiltinOverload& overload = table.at(expectation.overload);
        if (!expectation.matchesAnyOverload)
            return { NativeCallDeclarationSource::Compiler, overload.builtin, { &overload } };
        declaration.source = NativeCallDeclarationSource::Compiler;
        declaration.builtin = overload.builtin;
        appendOverloads(declaration, table.canonicalID(overload.builtin), overload.mode, call);
        return declaration;
    }

    auto declareByNativeCode = [&](BuiltinScope scope) {
        BuiltinID builtin = findBuiltinWithNativeCodeOf(table, call.callee, scope);
        if (!builtin)
            return false;
        declaration.source = NativeCallDeclarationSource::NativeCodeOfCallee;
        declaration.builtin = builtin;
        appendOverloads(declaration, table.canonicalID(builtin), modeOf(BuiltinCellKey::Kind::Object, call.kind), call);
        return true;
    };
    if (declareByNativeCode(BuiltinScope::Module))
        return declaration;

    // The program may have replaced a step of the path to the function, such as `console.log`. Compiled code that names the function has it in a cell.
    if (Instance* instance = Instance::of(call.realm); instance && instance->state) {
        if (std::optional<BuiltinCellKey> key = instance->state->builtinCells->keyOfCellHolding(call.callee)) {
            declaration.source = NativeCallDeclarationSource::CalleeInRealm;
            declaration.builtin = key->builtin();
            appendOverloads(declaration, table.canonicalID(key->builtin()), modeOf(key->kind(), call.kind), call);
            return declaration;
        }
    }

    String name = nameOfNativeFunction(call.callee);
    BuiltinCellKey::Kind kind = BuiltinCellKey::Kind::Object;
    if (name.startsWith("get "_s)) {
        kind = BuiltinCellKey::Kind::Getter;
        name = name.substring(4);
    } else if (name.startsWith("set "_s)) {
        kind = BuiltinCellKey::Kind::Setter;
        name = name.substring(4);
    }
    if (name.isEmpty()) {
        // Function.prototype is the one function of the realm that has no name.
        if (kind == BuiltinCellKey::Kind::Object && call.callee == call.realm->functionPrototype()) {
            declaration.source = NativeCallDeclarationSource::CalleeInRealm;
            declaration.builtin = Builtins::FunctionPrototype;
            appendOverloads(declaration, table.canonicalID(Builtins::FunctionPrototype), modeOf(kind, call.kind), call);
        }
        return declaration;
    }
    auto declareConstructorOfHostClass = [&] {
        BuiltinID constructor = kind == BuiltinCellKey::Kind::Object ? findConstructorOfHostClass(table, call.realm, call.callee) : BuiltinID();
        if (!constructor)
            return false;
        declaration.source = NativeCallDeclarationSource::CalleeInRealm;
        declaration.builtin = constructor;
        appendOverloads(declaration, table.canonicalID(constructor), modeOf(kind, call.kind), call);
        return true;
    };
    auto candidates = builtinsByFunctionName().find(name);
    if (candidates == builtinsByFunctionName().end()) {
        declareConstructorOfHostClass();
        return declaration;
    }

    // A spy of bun:test stands in the realm where the function stood that it spies on, under the name of that function, and runs what the program gave it.
    auto mayBe = [&](BuiltinID candidate) {
        return call.callee->inherits<JSFunction>() || table.at(candidate).kind == BuiltinKind::Constructor;
    };
    BuiltinCallMode mode = modeOf(kind, call.kind);
    for (BuiltinID candidate : candidates->value) {
        if (table.at(candidate).scope == BuiltinScope::Module || !mayBe(candidate))
            continue;
        BuiltinCellKey key = kind == BuiltinCellKey::Kind::Object ? BuiltinCellKey::object(candidate) : kind == BuiltinCellKey::Kind::Getter ? BuiltinCellKey::getter(candidate) : BuiltinCellKey::setter(candidate);
        // Compiled code has no cell for the function of an accessor that it never calls by itself. A generic read or store calls it all the same.
        bool mayBeInRealm = kind == BuiltinCellKey::Kind::Object ? key.hasCell(table) : table.at(candidate).kind == BuiltinKind::Accessor;
        if (!mayBeInRealm)
            continue;
        auto found = findInRealm(call.realm, key);
        if (!found || *found != call.callee)
            continue;
        declaration.source = NativeCallDeclarationSource::CalleeInRealm;
        declaration.builtin = candidate;
        appendOverloads(declaration, table.canonicalID(candidate), mode, call);
        return declaration;
    }

    // Nothing leads from the realm to what a native module exports either: a function of the realm that was not found there is known by its native code.
    if (declareByNativeCode(BuiltinScope::Realm) || declareConstructorOfHostClass())
        return declaration;

    // The program may have replaced a step of the path to the function, such as `console`. The call names the built-ins that flow lists as its callees.
    for (BuiltinID candidate : candidates->value) {
        if (table.at(candidate).scope == BuiltinScope::Module || !mayBe(candidate) || !expectation.isKnown || expectation.namesMoreBuiltins || !expectation.names(table.canonicalID(candidate)))
            continue;
        declaration.source = NativeCallDeclarationSource::CalleeInRealm;
        declaration.builtin = candidate;
        appendOverloads(declaration, table.canonicalID(candidate), mode, call);
        return declaration;
    }

    // Nothing leads from the realm to a member of a module, so every member of that name may be the callee.
    for (BuiltinID candidate : candidates->value) {
        if (table.at(candidate).scope != BuiltinScope::Module)
            continue;
        declaration.source = NativeCallDeclarationSource::NameOfModuleMember;
        declaration.builtin = candidate;
        appendOverloads(declaration, candidate, mode, call);
    }
    return declaration;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
