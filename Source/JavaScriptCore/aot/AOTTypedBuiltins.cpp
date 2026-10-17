/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedBuiltins.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTTypedType.h"
#include "BuiltinNames.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "PageCount.h"
#include "PureNaN.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
#include <numbers>
#include <string_view>
#include <wtf/MathExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

unsigned Builtin::numberOfRequiredParameters() const
{
    unsigned result = 0;
    for (auto& parameter : parameters)
        result += parameter.mode == ParameterMode::Required;
    return result;
}

uint16_t Builtin::argumentUses(unsigned index) const
{
    if (index < parameters.size())
        return parameters[index].uses;
    if (!parameters.isEmpty() && parameters.last().mode == ParameterMode::Rest)
        return parameters.last().uses;
    return OperandUse::Everything;
}

bool Builtin::takes(unsigned numberOfArguments) const
{
    if (numberOfArguments < numberOfRequiredParameters())
        return false;
    return numberOfArguments <= parameters.size() || (!parameters.isEmpty() && parameters.last().mode == ParameterMode::Rest);
}

static constexpr uint64_t keyFor(BuiltinKind kind, BuiltinName holder, BuiltinName name)
{
    return static_cast<uint64_t>(kind) << 32 | static_cast<uint64_t>(holder.index()) << 16 | name.index();
}

struct FirstBuiltin {
    uint64_t key;
    uint16_t index;
};

namespace {

using Result = BuiltinResult;
using Name = BuiltinName;

constexpr BuiltinParameter asIs { ArgumentConversion::None };
constexpr BuiltinParameter isNumber { ArgumentConversion::RequiresNumber };
constexpr BuiltinParameter isInteger { ArgumentConversion::RequiresInteger };
constexpr BuiltinParameter isTypedArray { ArgumentConversion::RequiresTypedArray };
constexpr BuiltinParameter toNumber { ArgumentConversion::ToNumber };
constexpr BuiltinParameter toString { ArgumentConversion::ToString };
constexpr BuiltinParameter toPropertyKey { ArgumentConversion::ToPropertyKey };
constexpr BuiltinParameter optional(BuiltinParameter parameter) { return { parameter.conversion, ParameterMode::Optional }; }
constexpr BuiltinParameter rest(BuiltinParameter parameter) { return { parameter.conversion, ParameterMode::Rest }; }
constexpr BuiltinParameter uses(BuiltinParameter parameter, uint16_t uses) { return { parameter.conversion, parameter.mode, uses }; }

constexpr BuiltinImplementation generic { };
constexpr BuiltinImplementation cell { BuiltinImplementation::Cell, BuiltinCallee::None };
constexpr BuiltinImplementation number { BuiltinImplementation::Number, BuiltinCallee::None };
constexpr BuiltinImplementation calls(BuiltinCallee callee) { return { BuiltinImplementation::Call, callee }; }
constexpr BuiltinImplementation inlined(InlineBuiltin which, BuiltinCallee callee) { return { BuiltinImplementation::Inline, callee, which }; }
constexpr BuiltinImplementation does(LanguageOperation which) { return { BuiltinImplementation::Operation, BuiltinCallee::None, InlineBuiltin::This, which }; }
constexpr BuiltinImplementation prelude(PreludeFunction which) { return { BuiltinImplementation::Prelude, BuiltinCallee::Function, InlineBuiltin::This, LanguageOperation::CopyDataProperties, which }; }

constexpr uint16_t noClass = BuiltinNameData::noClass;
constexpr uint16_t multipleClasses = BuiltinNameData::multipleClasses;
constexpr uint16_t noOperation = BuiltinNameData::noOperation;

} // anonymous namespace

#include "AOTTypedBuiltinEntries.inc"

static constexpr std::string_view toStringView(ASCIILiteral text) { return { text.characters(), text.length() }; }

static_assert([] {
    for (unsigned i = 0; i + 1 < numberOfBuiltinNames; ++i) {
        auto a = toStringView(builtinNames[i].text);
        auto b = toStringView(builtinNames[i + 1].text);
        if (!std::ranges::lexicographical_compare(a, b, [](char x, char y) { return static_cast<uint8_t>(x) < static_cast<uint8_t>(y); }))
            return false;
    }
    for (unsigned i = 0; i + 1 < std::size(firstBuiltins); ++i) {
        if (firstBuiltins[i].key >= firstBuiltins[i + 1].key)
            return false;
    }
    return true;
}());

static_assert([] {
    for (auto& entry : builtinEntries) {
        if (entry.nameName.index() >= numberOfBuiltinNames || toStringView(entry.name) != toStringView(builtinNames[entry.nameName.index()].text))
            return false;
        if (entry.holderName ? entry.holderName.index() >= numberOfBuiltinNames || toStringView(entry.holder) != toStringView(builtinNames[entry.holderName.index()].text) : !entry.holder.isEmpty())
            return false;
        if (entry.parameters.first + entry.parameters.count > std::size(builtinParameterPool))
            return false;
        if (entry.result.kinds & BuiltinResult::Instance && entry.result.engineClass >= numberOfEngineClasses)
            return false;
    }
    for (auto& first : firstBuiltins) {
        auto& entry = builtinEntries[first.index];
        if (first.key != keyFor(entry.kind, entry.holderName, entry.nameName))
            return false;
        if (first.index && first.key == keyFor(builtinEntries[first.index - 1].kind, builtinEntries[first.index - 1].holderName, builtinEntries[first.index - 1].nameName))
            return false;
    }
    for (unsigned i = 0; i < numberOfEngineClasses; ++i) {
        auto& engineClass = engineClasses[i];
        if (engineClass.index != i || builtinNames[engineClass.name.index()].engineClass != i || toStringView(engineClass.text) != toStringView(builtinNames[engineClass.name.index()].text))
            return false;
        if (engineClass.firstOriginalStructure + engineClass.numberOfOriginalStructures > numberOfOriginalStructures)
            return false;
    }
    for (uint16_t index : cellsOfReplaceableGlobalsData) {
        if (!builtinNames[builtinEntries[index].holderName.index()].isReplaceable)
            return false;
    }
    return true;
}());

#define AOT_TYPED_CHECK_OPERATION(spelled, entry, nameOfEntry) static_assert(builtinEntries[entry].kind == BuiltinKind::Operation && toStringView(builtinEntries[entry].name) == #spelled && builtinEntries[entry].nameName == BuiltinName(nameOfEntry) && builtinNames[nameOfEntry].operation == entry);
FOR_EACH_AOT_TYPED_IR_OPERATION(AOT_TYPED_CHECK_OPERATION)
#undef AOT_TYPED_CHECK_OPERATION

static constinit const Builtins s_builtins;

const Builtins& Builtins::singleton()
{
    return s_builtins;
}

double Builtins::numberOf(BuiltinID id) const
{
    return numberOfBuiltin(id.index());
}

std::span<const uint16_t> Builtins::replaceableGlobalCells() const
{
    return cellsOfReplaceableGlobalsData;
}

static bool isSymbolSpelling(std::span<const uint8_t> bytes)
{
    return bytes.size() > 2 && bytes[0] == '@' && bytes[1] == '@';
}

BuiltinName Builtins::nameOf(std::span<const uint8_t> bytes) const
{
    if (bytes.empty() || isSymbolSpelling(bytes))
        return { };
    auto* found = std::lower_bound(std::begin(builtinNames), std::end(builtinNames), bytes, [](const BuiltinNameData& name, std::span<const uint8_t> bytes) {
        return std::ranges::lexicographical_compare(byteCast<uint8_t>(name.text.span8()), bytes);
    });
    if (found == std::end(builtinNames) || !equalSpans(byteCast<uint8_t>(found->text.span8()), bytes))
        return { };
    return BuiltinName(found - builtinNames);
}

BuiltinID Builtins::find(BuiltinKind kind, BuiltinName holder, BuiltinName name) const
{
    if (!name)
        return { };
    uint64_t key = keyFor(kind, holder, name);
    auto* found = std::lower_bound(std::begin(firstBuiltins), std::end(firstBuiltins), key, [](const FirstBuiltin& first, uint64_t key) {
        return first.key < key;
    });
    if (found == std::end(firstBuiltins) || found->key != key)
        return { };
    return BuiltinID(found->index);
}

BuiltinID Builtins::next(BuiltinID id) const
{
    unsigned index = id.index() + 1;
    if (index >= numberOfBuiltins)
        return { };
    auto& previous = builtinEntries[id.index()];
    auto& entry = builtinEntries[index];
    if (entry.kind != previous.kind || entry.holderName != previous.holderName || entry.nameName != previous.nameName)
        return { };
    return BuiltinID(index);
}

BuiltinID Builtins::findForInstance(BuiltinKind kind, const EngineClass& engineClass, BuiltinName name) const
{
    if (BuiltinID found = find(kind, engineClass.name, name))
        return found;
    if (engineClass.listedAs != engineClass.name) {
        if (BuiltinID found = find(kind, engineClass.listedAs, name))
            return found;
    }
    return find(kind, objectClass().name, name);
}

BuiltinID Builtins::findStatic(BuiltinKind kind, BuiltinName global, BuiltinName name) const
{
    if (!global)
        return { };
    if (BuiltinID found = find(kind, global, name))
        return found;
    const EngineClass* engineClass = classNamed(global);
    if (!engineClass || engineClass->listedAs == engineClass->name)
        return { };
    return find(kind, engineClass->listedAs, name);
}

BuiltinID Builtins::findConstructor(BuiltinName global) const
{
    const EngineClass* engineClass = classNamed(global);
    return find(BuiltinKind::Constructor, { }, engineClass ? engineClass->listedAs : global);
}

const EngineClass* Builtins::classNamed(BuiltinName name) const
{
    if (!name || builtinNames[name.index()].engineClass == noClass)
        return nullptr;
    return &engineClasses[builtinNames[name.index()].engineClass];
}

const EngineClass* Builtins::classSpeculatedFor(BuiltinName method) const
{
    if (!method || builtinNames[method.index()].classSpeculatedFor >= multipleClasses)
        return nullptr;
    return &engineClasses[builtinNames[method.index()].classSpeculatedFor];
}

std::optional<PreludeFunction> Builtins::preludeFunctionNamed(std::span<const uint8_t> bytes) const
{
    UNUSED_PARAM(bytes);
#define AOT_TYPED_MATCH_PRELUDE_FUNCTION(name, text) \
    if (equalSpans(bytes, text ""_s.span8())) \
        return PreludeFunction::name;
    FOR_EACH_AOT_TYPED_PRELUDE_FUNCTION(AOT_TYPED_MATCH_PRELUDE_FUNCTION)
#undef AOT_TYPED_MATCH_PRELUDE_FUNCTION
    return std::nullopt;
}

GlobalInterface Builtins::interfaceNamed(BuiltinName name) const
{
    if (!name)
        return GlobalInterface::Unknown;
    if (name == functionClass().name)
        return GlobalInterface::Function;
    if (name == stringClass().name)
        return GlobalInterface::String;
    if (name == numberClass().name)
        return GlobalInterface::Number;
    if (name == booleanClass().name)
        return GlobalInterface::Boolean;
    if (name == symbolClass().name)
        return GlobalInterface::Symbol;
    if (name == bigIntClass().name)
        return GlobalInterface::BigInt;
    return classNamed(name) ? GlobalInterface::Class : GlobalInterface::Unknown;
}

static Structure* originalStructure(JSGlobalObject* globalObject, unsigned index)
{
    switch (index) {
#define AOT_TYPED_ORIGINAL_STRUCTURE_CASE(number, getter) \
    case number: \
        return globalObject->getter;
    FOR_EACH_AOT_TYPED_ORIGINAL_STRUCTURE(AOT_TYPED_ORIGINAL_STRUCTURE_CASE)
#undef AOT_TYPED_ORIGINAL_STRUCTURE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class PropertyKind : uint8_t { Data, Getter, Setter };
enum class Mutability : bool { Fixed, Replaceable };

static std::expected<JSValue, ASCIILiteral> findOwnProperty(JSGlobalObject* globalObject, JSObject* holder, const Identifier& key, PropertyKind kind, Mutability mutability = Mutability::Fixed)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(holder, PropertySlot::InternalMethodType::GetOwnProperty);
    bool isFound = holder->methodTable()->getOwnPropertySlot(holder, globalObject, key, slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return makeUnexpected("cannot be looked up"_s);
    }
    if (!isFound)
        return makeUnexpected("is missing"_s);
    if (slot.isAccessor() != (kind != PropertyKind::Data))
        return makeUnexpected(slot.isAccessor() ? "is an accessor"_s : "is not an accessor"_s);
    unsigned fixed = kind == PropertyKind::Data ? PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete : static_cast<unsigned>(PropertyAttribute::DontDelete);
    if (globalObject->hasImmutableIntrinsics() && mutability == Mutability::Fixed && (slot.attributes() & fixed) != fixed)
        return makeUnexpected("can be replaced"_s);
    switch (kind) {
    case PropertyKind::Getter:
        if (slot.getterSetter()->isGetterNull())
            return makeUnexpected("has no getter"_s);
        return JSValue(slot.getterSetter()->getter());
    case PropertyKind::Setter:
        if (slot.getterSetter()->isSetterNull())
            return makeUnexpected("has no setter"_s);
        return JSValue(slot.getterSetter()->setter());
    case PropertyKind::Data:
        break;
    }
    JSValue value = slot.getValue(globalObject, key);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return makeUnexpected("cannot be read"_s);
    }
    return value;
}

static std::expected<JSObject*, String> findObject(JSGlobalObject* globalObject, JSObject* holder, const Identifier& key, Mutability mutability = Mutability::Fixed)
{
    auto found = findOwnProperty(globalObject, holder, key, PropertyKind::Data, mutability);
    if (!found)
        return makeUnexpected(makeString(key.string(), ' ', found.error()));
    if (!found->isObject())
        return makeUnexpected(makeString(key.string(), " is not an object"_s));
    return asObject(*found);
}

static std::expected<JSObject*, String> findGlobal(JSGlobalObject* globalObject, ASCIILiteral name, Mutability mutability = Mutability::Fixed)
{
    return findObject(globalObject, globalObject, Identifier::fromString(globalObject->vm(), name), mutability);
}

static BuiltinCallee calleeKindOf(JSValue value)
{
    if (!value.isCell())
        return BuiltinCallee::None;
    JSCell* cell = value.asCell();
    if (auto* function = dynamicDowncast<JSFunction>(cell))
        return function->isHostFunction() ? BuiltinCallee::HostFunction : BuiltinCallee::Function;
    if (cell->type() == InternalFunctionType)
        return BuiltinCallee::InternalFunction;
    return cell->isCallable() ? BuiltinCallee::Function : BuiltinCallee::None;
}

static ASCIILiteral textOf(BuiltinCallee callee)
{
    switch (callee) {
    case BuiltinCallee::None:
        return "no function"_s;
    case BuiltinCallee::Function:
        return "a function that is not native"_s;
    case BuiltinCallee::HostFunction:
        return "a host function"_s;
    case BuiltinCallee::InternalFunction:
        return "an InternalFunction"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String calleeMismatch(BuiltinCallee expected, JSValue value)
{
    BuiltinCallee found = calleeKindOf(value);
    if (found == expected || (expected == BuiltinCallee::Function && found != BuiltinCallee::None))
        return { };
    return makeString("is "_s, textOf(found), ", the table says "_s, expected == BuiltinCallee::Function ? "a function"_s : textOf(expected));
}

static String textOf(const Builtin& builtin)
{
    switch (builtin.kind) {
    case BuiltinKind::Function:
        return makeString("call "_s, builtin.name);
    case BuiltinKind::Constructor:
        return makeString("construct "_s, builtin.name);
    case BuiltinKind::StaticFunction:
        return makeString("static "_s, builtin.holder, '.', builtin.name);
    case BuiltinKind::Constant:
        return makeString("constant "_s, builtin.holder, '.', builtin.name);
    case BuiltinKind::Method:
        return makeString("method "_s, builtin.holder, ".prototype."_s, builtin.name);
    case BuiltinKind::Getter:
        return makeString("getter "_s, builtin.holder, ".prototype."_s, builtin.name);
    case BuiltinKind::Setter:
        return makeString("setter "_s, builtin.holder, ".prototype."_s, builtin.name);
    case BuiltinKind::Operation:
        return makeString("operation "_s, builtin.name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::expected<JSValue, String> Builtins::findProperty(JSGlobalObject* globalObject, const Builtin& builtin, JSObject* given, bool underPrivateName) const
{
    VM& vm = globalObject->vm();
    bool isSharedByTypedArrays = builtin.holderName == BuiltinName(AOT_TYPED_BUILTIN_NAME_TypedArray);
    Mutability mutability = isReplaceable(builtin.holderName) ? Mutability::Replaceable : Mutability::Fixed;
    std::expected<JSObject*, String> holder = given;
    if (!given)
        holder = findGlobal(globalObject, isSharedByTypedArrays ? "Int8Array"_s : builtin.holder, mutability);
    if (holder && isSharedByTypedArrays) {
        if (globalObject->hasImmutableIntrinsics() && !(*holder)->structure()->inheritorsMayOverrideReadOnlyProperties())
            return makeUnexpected(String("Int8Array can be given another prototype"_s));
        JSObject* shared = (*holder)->getPrototypeDirect().getObject();
        if (!shared)
            return makeUnexpected(String("Int8Array inherits from no object"_s));
        holder = shared;
    }
    if (holder && builtin.hasReceiver())
        holder = findObject(globalObject, *holder, vm.propertyNames->prototype);
    if (!holder)
        return makeUnexpected(holder.error());
    Identifier key = Identifier::fromString(vm, builtin.name);
    if (isSymbolSpelling(asBytes(builtin.name.span8()))) {
        auto symbols = findGlobal(globalObject, "Symbol"_s);
        if (!symbols)
            return makeUnexpected(symbols.error());
        auto symbol = findOwnProperty(globalObject, *symbols, Identifier::fromString(vm, builtin.name.span8().subspan(2)), PropertyKind::Data);
        if (!symbol || !symbol->isSymbol())
            return makeUnexpected(String("is named by no symbol"_s));
        key = Identifier::fromUid(asSymbol(*symbol)->privateName());
    }
    if (underPrivateName) {
        PrivateSymbolImpl* name = vm.propertyNames->builtinNames().lookUpPrivateName(builtin.name.span8());
        if (!name)
            return makeUnexpected(String("is no private name"_s));
        key = Identifier::fromUid(vm, name);
        mutability = Mutability::Replaceable;
    }
    PropertyKind kind = PropertyKind::Data;
    if (builtin.implementation.kind != BuiltinImplementation::Number) {
        if (builtin.kind == BuiltinKind::Getter)
            kind = PropertyKind::Getter;
        else if (builtin.kind == BuiltinKind::Setter)
            kind = PropertyKind::Setter;
    }
    auto found = findOwnProperty(globalObject, *holder, key, kind, mutability);
    if (!found)
        return makeUnexpected(String(found.error()));
    return *found;
}

std::expected<JSCell*, String> Builtins::findOriginalStructure(JSGlobalObject* globalObject, unsigned index) const
{
    const EngineClass* engineClass = nullptr;
    for (auto& candidate : engineClasses) {
        if (index - candidate.firstOriginalStructure < candidate.numberOfOriginalStructures)
            engineClass = &candidate;
    }
    RELEASE_ASSERT(engineClass);
    auto mismatch = [&](auto what) {
        return makeUnexpected(makeString("class "_s, engineClass->text, ": structure "_s, index - engineClass->firstOriginalStructure, ": "_s, what));
    };
    Structure* structure = originalStructure(globalObject, index);
    if (!structure)
        return mismatch("is missing"_s);
    if (structure->typeInfo().type() != engineClass->type)
        return mismatch("has another JSType"_s);
    if (structure->maxOffset() != invalidOffset)
        return mismatch("has properties"_s);
    auto prototype = findGlobal(globalObject, engineClass->text);
    if (prototype)
        prototype = findObject(globalObject, *prototype, globalObject->vm().propertyNames->prototype);
    if (!prototype)
        return mismatch(prototype.error());
    if (structure->hasPolyProto() || structure->storedPrototype() != JSValue(*prototype))
        return mismatch("has another prototype"_s);
    return static_cast<JSCell*>(structure);
}

std::expected<JSCell*, String> Builtins::lookUp(JSGlobalObject* globalObject, unsigned index, JSObject* holder) const
{
    if (index >= firstPrototypeCell()) {
        auto prototype = findGlobal(globalObject, engineClasses[index - firstPrototypeCell()].text);
        if (prototype)
            prototype = findObject(globalObject, *prototype, globalObject->vm().propertyNames->prototype);
        if (!prototype)
            return makeUnexpected(String(prototype.error()));
        return static_cast<JSCell*>(*prototype);
    }
    if (index >= originalStructureCell(0))
        return findOriginalStructure(globalObject, index - originalStructureCell(0));
    if (index >= numberOfBuiltins) {
        BuiltinName name(index - numberOfBuiltins);
        ASCIILiteral text = builtinNames[name.index()].text;
        auto object = findGlobal(globalObject, text);
        if (!object)
            return makeUnexpected(object.error());
        const EngineClass* engineClass = classNamed(name);
        BuiltinName listedAs = engineClass ? engineClass->listedAs : name;
        for (BuiltinKind kind : { BuiltinKind::Function, BuiltinKind::Constructor }) {
            for (BuiltinID id = find(kind, { }, listedAs); id; id = next(id)) {
                if (String mismatch = calleeMismatch(at(id).implementation.callee, *object); !mismatch.isNull())
                    return makeUnexpected(makeString(kind == BuiltinKind::Function ? "call "_s : "construct "_s, text, ": "_s, mismatch));
            }
        }
        return static_cast<JSCell*>(*object);
    }

    auto& builtin = builtinEntries[index];
    auto found = findProperty(globalObject, builtin, holder);
    if (!found)
        return makeUnexpected(makeString(textOf(builtin), ": "_s, found.error()));
    if (builtin.implementation.kind == BuiltinImplementation::Cell) {
        if (!found->isSymbol())
            return makeUnexpected(makeString(textOf(builtin), ": is not a symbol"_s));
        return found->asCell();
    }
    if (String mismatch = calleeMismatch(builtin.implementation.callee, *found); !mismatch.isNull())
        return makeUnexpected(makeString(textOf(builtin), ": "_s, mismatch));
    return found->asCell();
}

BuiltinID Builtins::entryImplementedBy(PreludeFunction which) const
{
    for (unsigned index = 0; index < numberOfBuiltins; ++index) {
        auto& implementation = builtinEntries[index].implementation;
        if (implementation.kind == BuiltinImplementation::Prelude && implementation.prelude == which)
            return BuiltinID(index);
    }
    return { };
}

bool Builtins::isHostFunctionInEngine(BuiltinID id) const
{
    auto& entry = at(id);
    for (auto& sibling : builtinEntries) {
        if (sibling.kind == entry.kind && sibling.holderName == entry.holderName && sibling.nameName == entry.nameName && sibling.implementation.kind == BuiltinImplementation::Call && sibling.implementation.callee != BuiltinCallee::Function)
            return true;
    }
    return false;
}

JSCell* Builtins::findCell(JSGlobalObject* globalObject, unsigned index) const
{
    if (isReplaceableGlobalCell(index))
        return globalObject->vm().structureStructure.get();
    if (globalObject->vm().useImmutableIntrinsics && !globalObject->hasImmutableIntrinsics()) [[unlikely]] {
        Instance* instance = globalObject->aotInstance();
        JSCell* captured = instance && instance->negativeOffsetFields().typedBuiltins ? instance->negativeOffsetFields().typedBuiltins[index] : nullptr;
        return captured ? captured : globalObject->vm().structureStructure.get();
    }
    DeferTerminationForAWhile deferTermination(globalObject->vm());
    auto found = lookUp(globalObject, index);
    if (!found) [[unlikely]] {
        dataLogLn("AOT: the built-ins table does not describe this realm: ", found.error());
        RELEASE_ASSERT_NOT_REACHED();
    }
    return *found;
}

JSCell* Builtins::captureCell(JSGlobalObject* globalObject, JSObject* holder, unsigned index) const
{
    DeferTerminationForAWhile deferTermination(globalObject->vm());
    auto found = lookUp(globalObject, index, holder);
    return found ? *found : globalObject->vm().structureStructure.get();
}

unsigned Builtins::globalCell(ASCIILiteral global) const
{
    BuiltinName name = nameOf(byteCast<uint8_t>(global.span8()));
    RELEASE_ASSERT(isGlobal(name));
    return cellOf(name);
}

unsigned Builtins::staticFunctionCell(ASCIILiteral global, ASCIILiteral name) const
{
    BuiltinID entry = findStatic(BuiltinKind::StaticFunction, nameOf(byteCast<uint8_t>(global.span8())), nameOf(byteCast<uint8_t>(name.span8())));
    RELEASE_ASSERT(entry && hasCell(entry));
    return cellOf(entry);
}

bool Builtins::hasCell(unsigned index) const
{
    if (index >= firstPrototypeCell())
        return isGlobal(engineClasses[index - firstPrototypeCell()].name);
    if (index >= originalStructureCell(0))
        return true;
    if (index >= numberOfBuiltins)
        return builtinNames[index - numberOfBuiltins].isGlobal;
    auto& builtin = builtinEntries[index];
    switch (builtin.kind) {
    case BuiltinKind::Function:
    case BuiltinKind::Constructor:
    case BuiltinKind::Operation:
        return false;
    default:
        break;
    }
    switch (builtin.implementation.kind) {
    case BuiltinImplementation::Generic:
        return callOrApply(builtin.nameName) == BuiltinID(index);
    case BuiltinImplementation::Cell:
        return true;
    default:
        return builtin.implementation.callee != BuiltinCallee::None;
    }
}

Vector<String> Builtins::mismatches(JSGlobalObject* globalObject) const
{
    Vector<String> result;
    auto add = [&](String&& mismatch) {
        if (!result.contains(mismatch))
            result.append(WTF::move(mismatch));
    };
    for (unsigned index = 0; index < numberOfCells(); ++index) {
        if (!hasCell(index))
            continue;
        if (index < numberOfBuiltins && isReplaceable(builtinEntries[index].holderName) && !findGlobal(globalObject, builtinEntries[index].holder, Mutability::Replaceable))
            continue;
        if (auto found = lookUp(globalObject, index); !found)
            add(WTF::move(found.error()));
    }
    for (unsigned index = 0; index < numberOfBuiltins; ++index) {
        auto& builtin = builtinEntries[index];
        if (builtin.implementation.kind != BuiltinImplementation::Number)
            continue;
        double number = numberOf(BuiltinID(index));
        auto found = findProperty(globalObject, builtin);
        if (!found)
            add(makeString(textOf(builtin), ": "_s, found.error()));
        else if (!found->isNumber() || std::bit_cast<uint64_t>(purifyNaN(found->asNumber())) != std::bit_cast<uint64_t>(purifyNaN(number)))
            add(makeString(textOf(builtin), ": is not "_s, number));
    }
    auto isSameFunction = [](JSValue a, JSValue b) {
        auto* first = dynamicDowncast<JSFunction>(a);
        auto* second = dynamicDowncast<JSFunction>(b);
        if (a == b || !first || !second)
            return a == b;
        return first->isHostFunction() ? second->isHostFunction() && first->nativeFunction() == second->nativeFunction() : first->executable() == second->executable();
    };
    for (unsigned index = 0; index < numberOfBuiltins; ++index) {
        auto& builtin = builtinEntries[index];
        auto found = builtin.hasPrivateAlias ? findProperty(globalObject, builtin) : makeUnexpected(String());
        if (!found)
            continue;
        auto twin = findProperty(globalObject, builtin, nullptr, true);
        if (!twin)
            add(makeString(textOf(builtin), ": under the private name: "_s, twin.error()));
        else if (!isSameFunction(*found, *twin))
            add(makeString(textOf(builtin), ": under the private name is another function"_s));
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
