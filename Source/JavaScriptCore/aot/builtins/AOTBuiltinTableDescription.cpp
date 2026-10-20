/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableWords.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

extern "C" const char* Bun__removableFeatureName(uint16_t featureIndex);

namespace JSC { namespace AOT {

using WordList = std::span<const ASCIILiteral>;

struct TableDescription {
    JSGlobalObject* realm;
    VM& vm;
    const BuiltinTable& table;
};

static void put(const TableDescription& description, JSObject* object, ASCIILiteral name, JSValue value)
{
    object->putDirect(description.vm, Identifier::fromString(description.vm, name), value);
}

static JSValue describeText(const TableDescription& description, const String& text)
{
    return jsString(description.vm, text);
}

template<typename Elements, typename Functor>
static JSArray* describeList(const TableDescription& description, const Elements& elements, const Functor& describeElement)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSArray* array = constructEmptyArray(description.realm, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned index = 0;
    for (auto&& element : elements) {
        JSValue value = describeElement(description, element);
        RETURN_IF_EXCEPTION(scope, nullptr);
        array->putDirectIndex(description.realm, index++, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return array;
}

template<typename Enum>
static JSValue describeWord(const TableDescription& description, WordList words, Enum value)
{
    return describeText(description, words[static_cast<unsigned>(value)]);
}

template<typename Enum>
static JSValue describeOptionalWord(const TableDescription& description, WordList words, std::optional<Enum> value)
{
    return value ? describeWord(description, words, *value) : jsNull();
}

template<typename Enum>
static JSArray* describeWords(const TableDescription& description, WordList words, OptionSet<Enum> set)
{
    Vector<ASCIILiteral, 32> wordsInSet;
    for (size_t position = 0; position < words.size(); ++position) {
        if ((set.toRaw() >> position) & 1)
            wordsInSet.append(words[position]);
    }
    return describeList(description, wordsInSet, describeText);
}

static JSValue describeHeap(const TableDescription& description, AbstractHeap heap)
{
    return describeWord(description, abstractHeapWords, heap);
}

template<typename ID>
static JSValue describeToken(const TableDescription& description, ID id)
{
    return id ? describeText(description, description.table.tokenOf(id).toString()) : jsNull();
}

static JSValue describeFunction(RuntimeFunctionID id)
{
    return id ? jsNumber(id.index()) : jsNull();
}

static JSValue describeNameText(const TableDescription& description, BuiltinName name)
{
    return describeText(description, String::fromUTF8(description.table.textOf(name)));
}

static JSValue describeName(const TableDescription& description, BuiltinName name)
{
    const BuiltinTable& table = description.table;
    if (table.isRegisteredSymbolName(name))
        return describeText(description, makeString("@@@"_s, String::fromUTF8(table.textOf(name))));
    if (table.isSymbolName(name))
        return describeText(description, makeString("@@"_s, String::fromUTF8(table.textOf(name))));
    return describeNameText(description, name);
}

static JSValue describeOperand(const TableDescription& description, BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return describeText(description, "this"_s);
    case BuiltinOperand::Kind::Argument: {
        JSObject* object = constructEmptyObject(description.realm);
        put(description, object, "argument"_s, jsNumber(operand.argumentIndex()));
        return object;
    }
    case BuiltinOperand::Kind::ArgumentsFrom: {
        JSObject* object = constructEmptyObject(description.realm);
        put(description, object, "argumentsFrom"_s, jsNumber(operand.argumentIndex()));
        return object;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue describeOptionalOperand(const TableDescription& description, std::optional<BuiltinOperand> operand)
{
    return operand ? describeOperand(description, *operand) : jsNull();
}

static JSObject* describeType(const TableDescription&, BuiltinTypeID);

static JSObject* describeSlotContents(const TableDescription& description, const BuiltinSlotContents& contents)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* type = describeType(description, contents.type);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "slot"_s, describeToken(description, contents.slot));
    put(description, object, "type"_s, type);
    return object;
}

static JSObject* describeTypePart(const TableDescription& description, const BuiltinTypePart& part)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "part"_s, describeWord(description, builtinTypePartKindWords, part.kind));
    switch (part.kind) {
    case BuiltinTypePartKind::Instance: {
        JSArray* pristine = describeWords(description, pristineAspectsWords, part.pristine);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSArray* contents = describeList(description, part.contents, describeSlotContents);
        RETURN_IF_EXCEPTION(scope, nullptr);
        put(description, object, "class"_s, describeToken(description, part.builtinClass));
        put(description, object, "pristine"_s, pristine);
        put(description, object, "contents"_s, contents);
        return object;
    }
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        return object;
    case BuiltinTypePartKind::Layout:
        put(description, object, "layout"_s, describeToken(description, part.layout));
        return object;
    case BuiltinTypePartKind::Builtin:
        put(description, object, "builtin"_s, describeToken(description, part.builtin));
        return object;
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        put(description, object, "operand"_s, describeOptionalOperand(description, part.operand));
        return object;
    case BuiltinTypePartKind::IteratedValues:
        put(description, object, "operand"_s, describeOptionalOperand(description, part.operand));
        put(description, object, "isSettled"_s, jsBoolean(part.isSettled));
        return object;
    case BuiltinTypePartKind::Slot:
        put(description, object, "operand"_s, describeOptionalOperand(description, part.operand));
        put(description, object, "slot"_s, describeToken(description, part.slot));
        return object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue describeIntegers(const TableDescription& description, const BuiltinType& type)
{
    if (!type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers))
        return jsNull();
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "minimum"_s, jsNumber(type.integers.minimum));
    put(description, object, "maximum"_s, jsNumber(type.integers.maximum));
    return object;
}

static JSObject* describeType(const TableDescription& description, BuiltinTypeID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinType& type = description.table.at(id);
    JSArray* primitives = describeWords(description, builtinPrimitiveWords, type.primitives);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* stringLiterals = describeList(description, type.stringLiterals, describeName);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* parts = describeList(description, type.parts, describeTypePart);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "isAnything"_s, jsBoolean(type.flags.contains(BuiltinTypeFlag::IsAnything)));
    put(description, object, "hasAnyObject"_s, jsBoolean(type.flags.contains(BuiltinTypeFlag::HasAnyObject)));
    put(description, object, "hasAnyFunction"_s, jsBoolean(type.flags.contains(BuiltinTypeFlag::HasAnyFunction)));
    put(description, object, "hasPlainData"_s, jsBoolean(type.flags.contains(BuiltinTypeFlag::HasPlainData)));
    put(description, object, "primitives"_s, primitives);
    put(description, object, "integers"_s, describeIntegers(description, type));
    put(description, object, "stringLiterals"_s, stringLiterals);
    put(description, object, "freshness"_s, describeWord(description, builtinFreshnessWords, type.freshness));
    put(description, object, "parts"_s, parts);
    return object;
}

static JSValue describeMemberStep(const TableDescription& description, BuiltinName name)
{
    return name ? describeName(description, name) : jsNull();
}

static JSObject* describePlace(const TableDescription& description, const BuiltinPlace& place)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "place"_s, describeWord(description, builtinPlaceKindWords, place.kind));
    switch (place.kind) {
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::NativeMemory:
        return object;
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Prototype:
        put(description, object, "operand"_s, describeOptionalOperand(description, place.operand));
        return object;
    case BuiltinPlaceKind::IteratedValues:
        put(description, object, "operand"_s, describeOptionalOperand(description, place.operand));
        put(description, object, "isSettled"_s, jsBoolean(place.isSettled));
        return object;
    case BuiltinPlaceKind::Slot:
        put(description, object, "operand"_s, describeOptionalOperand(description, place.operand));
        put(description, object, "slot"_s, describeToken(description, place.slot));
        return object;
    case BuiltinPlaceKind::ResultSlot:
        put(description, object, "slot"_s, describeToken(description, place.slot));
        return object;
    case BuiltinPlaceKind::ResultField:
        put(description, object, "index"_s, jsNumber(place.fieldIndex));
        return object;
    case BuiltinPlaceKind::NewValue: {
        JSObject* type = describeType(description, place.newValueType);
        RETURN_IF_EXCEPTION(scope, nullptr);
        put(description, object, "type"_s, type);
        return object;
    }
    case BuiltinPlaceKind::Member: {
        JSArray* path = describeList(description, place.memberPath, describeMemberStep);
        RETURN_IF_EXCEPTION(scope, nullptr);
        put(description, object, "operand"_s, describeOptionalOperand(description, place.operand));
        put(description, object, "path"_s, path);
        put(description, object, "firstStep"_s, jsNumber(place.firstMemberStep));
        return object;
    }
    case BuiltinPlaceKind::NativeCell:
        put(description, object, "cell"_s, describeNameText(description, place.nativeCell));
        return object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* describeKey(const TableDescription& description, const BuiltinKey& key)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "key"_s, describeWord(description, builtinKeyKindWords, key.kind));
    switch (key.kind) {
    case BuiltinKeyKind::None:
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::Indexes:
        return object;
    case BuiltinKeyKind::Names: {
        JSArray* names = describeList(description, key.names, describeName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        put(description, object, "names"_s, names);
        return object;
    }
    case BuiltinKeyKind::Operand:
    case BuiltinKeyKind::ElementsOf:
        put(description, object, "operand"_s, describeOptionalOperand(description, key.operand));
        return object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* describeTransfer(const TableDescription& description, const BuiltinTransfer& transfer)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* from = describePlace(description, transfer.from);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* to = describePlace(description, transfer.to);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "from"_s, from);
    put(description, object, "to"_s, to);
    return object;
}

static JSObject* describeCallback(const TableDescription& description, const BuiltinCallback& callback)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* newTarget = describePlace(description, callback.newTarget);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* thisValue = describePlace(description, callback.thisValue);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* arguments = describeList(description, callback.arguments, describePlace);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* spreadArguments = describePlace(description, callback.spreadArguments);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* resultTo = describePlace(description, callback.resultTo);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "callee"_s, describeOperand(description, callback.callee));
    put(description, object, "times"_s, describeWord(description, builtinCallbackTimesWords, callback.times));
    put(description, object, "constructs"_s, jsBoolean(callback.constructs));
    put(description, object, "newTarget"_s, newTarget);
    put(description, object, "thisValue"_s, thisValue);
    put(description, object, "arguments"_s, arguments);
    put(description, object, "spreadArguments"_s, spreadArguments);
    put(description, object, "resultUse"_s, describeWord(description, builtinCallbackResultUseWords, callback.resultUse));
    put(description, object, "resultTo"_s, resultTo);
    return object;
}

static JSObject* describeHandlerCall(const TableDescription& description, const BuiltinHandlerCall& call)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* callee = describePlace(description, call.callee);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* thisValue = describePlace(description, call.thisValue);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* arguments = describeList(description, call.arguments, describePlace);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "callee"_s, callee);
    put(description, object, "times"_s, describeWord(description, builtinCallbackTimesWords, call.times));
    put(description, object, "thisValue"_s, thisValue);
    put(description, object, "arguments"_s, arguments);
    put(description, object, "resultUse"_s, describeWord(description, builtinCallbackResultUseWords, call.resultUse));
    return object;
}

static JSObject* describePropertyAction(const TableDescription& description, const BuiltinPropertyAction& action)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* key = describeKey(description, action.key);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* value = describePlace(description, action.value);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool hasIntegrityLevel = action.kind == BuiltinPropertyActionKind::ChangesIntegrity;

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "action"_s, describeWord(description, builtinPropertyActionKindWords, action.kind));
    put(description, object, "object"_s, describeOperand(description, action.object));
    put(description, object, "key"_s, key);
    put(description, object, "value"_s, value);
    put(description, object, "integrityLevel"_s, hasIntegrityLevel ? describeWord(description, builtinIntegrityLevelWords, action.integrityLevel) : jsNull());
    return object;
}

static JSObject* describeParameter(const TableDescription& description, const BuiltinParameter& parameter)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* accepted = describeType(description, parameter.accepted);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* converted = describeType(description, parameter.converted);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* refused = describeType(description, parameter.refused);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* uses = describeWords(description, operandUseWords, parameter.uses);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* roles = describeWords(description, builtinOperandRoleWords, parameter.roles);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* protocols = describeWords(description, protocolWords, parameter.protocols);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* namedMembers = describeList(description, parameter.namedMembers, describeName);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* calledMembers = describeList(description, parameter.calledMembers, describeName);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "operand"_s, describeOperand(description, parameter.operand));
    put(description, object, "mode"_s, describeWord(description, builtinParameterModeWords, parameter.mode));
    put(description, object, "accepted"_s, accepted);
    put(description, object, "conversion"_s, describeWord(description, builtinConversionWords, parameter.conversion));
    put(description, object, "converted"_s, converted);
    put(description, object, "refused"_s, refused);
    put(description, object, "uses"_s, uses);
    put(description, object, "roles"_s, roles);
    put(description, object, "protocols"_s, protocols);
    put(description, object, "namedMembers"_s, namedMembers);
    put(description, object, "calledMembers"_s, calledMembers);
    return object;
}

static JSObject* describeResult(const TableDescription& description, const BuiltinResult& result)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* type = describeType(description, result.type);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "type"_s, type);
    put(description, object, "rangeRule"_s, describeWord(description, builtinRangeRuleWords, result.rangeRule));
    put(description, object, "rangeOperand"_s, describeOptionalOperand(description, result.rangeOperand));
    put(description, object, "substringOf"_s, describeOptionalOperand(description, result.substringOf));
    put(description, object, "typedResultForm"_s, describeWord(description, typedResultFormWords, result.typedResultForm));
    put(description, object, "typedResultSource"_s, describeOptionalOperand(description, result.typedResultSource));
    put(description, object, "iteratedOperand"_s, describeOptionalOperand(description, result.iteratedOperand));
    put(description, object, "iterationKind"_s, result.iteratedOperand ? describeWord(description, iterationKindWords, result.iterationKind) : jsNull());
    put(description, object, "prototypeSource"_s, describeOptionalOperand(description, result.prototypeSource));
    return object;
}

static JSObject* describeImplementation(const TableDescription& description, const BuiltinImplementation& implementation)
{
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "how"_s, describeWord(description, builtinImplementationKindWords, implementation.kind));
    put(description, object, "otherwise"_s, describeOptionalWord(description, builtinImplementationKindWords, implementation.otherwise));
    put(description, object, "callee"_s, describeWord(description, builtinCalleeWords, implementation.callee));
    put(description, object, "inlineBuiltin"_s, describeOptionalWord(description, inlineBuiltinWords, implementation.inlineBuiltin));
    put(description, object, "maximumArgumentCount"_s, implementation.maximumArgumentCount ? jsNumber(*implementation.maximumArgumentCount) : jsNull());
    put(description, object, "function"_s, describeFunction(implementation.function));
    return object;
}

static JSObject* describeOverload(const TableDescription& description, BuiltinOverloadID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinTable& table = description.table;
    const BuiltinOverload& overload = table.at(id);

    JSValue receiver = jsNull();
    if (overload.receiver) {
        receiver = describeParameter(description, *overload.receiver);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSArray* parameters = describeList(description, overload.parameters, describeParameter);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* result = describeResult(description, overload.result);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* reads = describeList(description, overload.effects.reads, describeHeap);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* writes = describeList(description, overload.effects.writes, describeHeap);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* transfers = describeList(description, overload.flow.transfers, describeTransfer);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* callbacks = describeList(description, overload.flow.callbacks, describeCallback);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* propertyActions = describeList(description, overload.flow.propertyActions, describePropertyAction);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* handlerCalls = describeList(description, overload.flow.handlerCalls, describeHandlerCall);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "token"_s, describeText(description, table.tokenOf(id)));
    put(description, object, "path"_s, describeText(description, table.textOf(overload.builtin)));
    put(description, object, "mode"_s, describeWord(description, builtinCallModeWords, overload.mode));
    put(description, object, "isProvenByConstruction"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::IsProvenByConstruction)));
    put(description, object, "isUndeclared"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::IsUndeclared)));
    put(description, object, "textHash"_s, describeText(description, makeString(hex(overload.textHash, 16, Lowercase))));
    put(description, object, "receiver"_s, receiver);
    put(description, object, "parameters"_s, parameters);
    put(description, object, "result"_s, result);
    put(description, object, "capability"_s, describeWord(description, builtinCapabilityWords, overload.effects.capability));
    put(description, object, "throws"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::Throws)));
    put(description, object, "runsOnlyOperandCode"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode)));
    put(description, object, "readsStateBeforeConversions"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::ReadsStateBeforeConversions)));
    put(description, object, "isSpecialCase"_s, jsBoolean(overload.flags.contains(BuiltinOverloadFlag::IsSpecialCase)));
    put(description, object, "reads"_s, reads);
    put(description, object, "writes"_s, writes);
    put(description, object, "transfers"_s, transfers);
    put(description, object, "callbacks"_s, callbacks);
    put(description, object, "propertyActions"_s, propertyActions);
    put(description, object, "handlerCalls"_s, handlerCalls);
    put(description, object, "implementation"_s, describeImplementation(description, overload.code.implementation));
    put(description, object, "model"_s, describeFunction(overload.code.model));
    return object;
}

static JSObject* describeEntry(const TableDescription& description, BuiltinID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinTable& table = description.table;
    const Builtin& builtin = table.at(id);
    JSArray* platforms = describeWords(description, builtinPlatformWords, builtin.platforms);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* flags = describeWords(description, builtinFlagWords, builtin.flags);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* overloads = describeList(description, table.overloadIDsOf(id), describeOverload);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "id"_s, jsNumber(id.index()));
    put(description, object, "token"_s, describeToken(description, id));
    put(description, object, "path"_s, describeText(description, table.textOf(id)));
    put(description, object, "kind"_s, describeWord(description, builtinKindWords, builtin.kind));
    put(description, object, "origin"_s, describeWord(description, builtinOriginWords, builtin.origin));
    put(description, object, "scope"_s, describeWord(description, builtinScopeWords, builtin.scope));
    put(description, object, "mutability"_s, describeWord(description, builtinMutabilityWords, builtin.mutability));
    put(description, object, "platforms"_s, platforms);
    put(description, object, "flags"_s, flags);
    put(description, object, "holder"_s, describeToken(description, builtin.holder));
    put(description, object, "name"_s, describeNameText(description, builtin.name));
    put(description, object, "isSymbolName"_s, jsBoolean(table.isSymbolName(builtin.name)));
    put(description, object, "prototypeLink"_s, describeWord(description, builtinPrototypeLinkWords, builtin.prototypeLink));
    put(description, object, "inheritsFrom"_s, describeToken(description, builtin.inheritsFrom));
    put(description, object, "instanceClass"_s, describeToken(description, builtin.instanceClass));
    put(description, object, "sameObjectAs"_s, describeToken(description, builtin.sameObjectAs));
    put(description, object, "overloads"_s, overloads);
    return object;
}

static JSObject* describeSlot(const TableDescription& description, BuiltinSlotID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinSlot& slot = description.table.at(id);
    JSValue holds = jsNull();
    if (slot.holds) {
        holds = describeType(description, slot.holds);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "token"_s, describeToken(description, id));
    put(description, object, "name"_s, describeNameText(description, slot.name));
    put(description, object, "owner"_s, describeToken(description, slot.owner));
    put(description, object, "heap"_s, describeHeap(description, slot.heap));
    put(description, object, "contentsSlot"_s, describeOptionalWord(description, contentsSlotWords, slot.contentsSlot));
    put(description, object, "holds"_s, holds);
    return object;
}

static JSObject* describeClass(const TableDescription& description, BuiltinClassID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinClass& builtinClass = description.table.at(id);
    JSArray* slots = describeList(description, builtinClass.slots, describeSlot);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "token"_s, describeToken(description, id));
    put(description, object, "origin"_s, describeWord(description, builtinOriginWords, builtinClass.origin));
    put(description, object, "parent"_s, describeToken(description, builtinClass.parent));
    put(description, object, "constructor"_s, describeToken(description, builtinClass.constructor));
    put(description, object, "prototype"_s, describeToken(description, builtinClass.prototype));
    put(description, object, "instanceTest"_s, describeWord(description, builtinInstanceTestWords, builtinClass.instanceTest));
    put(description, object, "isPrimitiveWrapper"_s, jsBoolean(builtinClass.isPrimitiveWrapper));
    put(description, object, "isNeverPristine"_s, jsBoolean(builtinClass.isNeverPristine));
    put(description, object, "hasClosureInstances"_s, jsBoolean(builtinClass.hasClosureInstances));
    put(description, object, "definesCharactersOnNewInstances"_s, jsBoolean(builtinClass.definesCharactersOnNewInstances));
    JSValue namesDefinedOnNewInstances = jsNull();
    if (builtinClass.listsNamesDefinedOnNewInstances) {
        namesDefinedOnNewInstances = describeList(description, builtinClass.namesDefinedOnNewInstances, describeName);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    put(description, object, "namesDefinedOnNewInstances"_s, namesDefinedOnNewInstances);
    put(description, object, "slots"_s, slots);
    put(description, object, "stateHeap"_s, describeOptionalWord(description, abstractHeapWords, builtinClass.stateHeap));
    return object;
}

static JSObject* describeLayoutField(const TableDescription& description, const BuiltinLayoutField& field)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    JSObject* type = describeType(description, field.type);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "name"_s, field.name ? describeName(description, field.name) : jsNull());
    put(description, object, "isOptional"_s, jsBoolean(field.isOptional));
    put(description, object, "type"_s, type);
    return object;
}

static JSObject* describeLayout(const TableDescription& description, BuiltinLayoutID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinLayout& layout = description.table.at(id);
    JSArray* fields = describeList(description, layout.fields, describeLayoutField);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "token"_s, describeToken(description, id));
    put(description, object, "isTuple"_s, jsBoolean(layout.isTuple));
    put(description, object, "fields"_s, fields);
    return object;
}

template<typename ID>
static BuiltinTableIDRange<ID> idsBelow(unsigned count)
{
    return { ID(0), count };
}

JSValue BuiltinTable::describeForTesting(JSGlobalObject* realm) const
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    TableDescription description { realm, vm, *this };

    JSArray* entries = describeList(description, idsBelow<BuiltinID>(builtinCount()), describeEntry);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* classes = describeList(description, idsBelow<BuiltinClassID>(classCount()), describeClass);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* layouts = describeList(description, idsBelow<BuiltinLayoutID>(layoutCount()), describeLayout);
    RETURN_IF_EXCEPTION(scope, { });

    JSObject* object = constructEmptyObject(realm);
    put(description, object, "entries"_s, entries);
    put(description, object, "classes"_s, classes);
    put(description, object, "layouts"_s, layouts);
    return object;
}

struct FeatureUseSummary {
    RemovableFeature feature;
    bool isConditional;
};

static void addFeatureUse(Vector<FeatureUseSummary>& summaries, const BuiltinFeatureUse& use)
{
    bool isConditional = use.overload || use.condition;
    size_t position = summaries.findIf([&](const FeatureUseSummary& summary) {
        return summary.feature == use.feature;
    });
    if (position == notFound) {
        summaries.append({ use.feature, isConditional });
        return;
    }
    summaries[position].isConditional = summaries[position].isConditional && isConditional;
}

static Vector<FeatureUseSummary> summarizeFeatureUses(std::span<const BuiltinFeatureUse> uses)
{
    Vector<FeatureUseSummary> summaries;
    for (const BuiltinFeatureUse& use : uses) {
        if (!use.overload)
            addFeatureUse(summaries, use);
    }
    for (const BuiltinFeatureUse& use : uses) {
        if (use.overload)
            addFeatureUse(summaries, use);
    }
    return summaries;
}

static JSObject* describeFeatureUse(const TableDescription& description, const FeatureUseSummary& summary)
{
    const char* name = Bun__removableFeatureName(static_cast<uint16_t>(summary.feature));
    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "feature"_s, name ? describeText(description, String::fromUTF8(name)) : jsNull());
    put(description, object, "isConditional"_s, jsBoolean(summary.isConditional));
    return object;
}

static Vector<RuntimeModuleID> findModulesByRoot(const BuiltinTable& table)
{
    Vector<RuntimeModuleID> modules(FillWith { }, table.builtinCount(), RuntimeModuleID());
    for (RuntimeModuleID module : idsBelow<RuntimeModuleID>(table.moduleCount())) {
        BuiltinID namespaceObject = table.namespaceOf(module);
        if (namespaceObject && !table.at(namespaceObject).holder)
            modules[namespaceObject.index()] = module;
    }
    return modules;
}

static JSValue describeFeatureUserKind(const TableDescription& description, BuiltinID id, RuntimeModuleID module)
{
    if (!module)
        return describeWord(description, builtinKindWords, description.table.at(id).kind);
    bool isPublic = description.table.at(module).flags.contains(RuntimeModuleFlag::IsPublic);
    return describeText(description, isPublic ? "Module"_s : "InternalModule"_s);
}

static bool hasHiddenRoot(const BuiltinTable& table, const Vector<RuntimeModuleID>& modulesByRoot, BuiltinID id)
{
    BuiltinID root = table.rootOf(id);
    const Builtin& builtin = table.at(root);
    if (builtin.holder || root == Builtins::GlobalThis)
        return false;
    return builtin.kind != BuiltinKind::NativeBinding && !modulesByRoot[root.index()];
}

static JSObject* describeFeatureUser(const TableDescription& description, const Vector<RuntimeModuleID>& modulesByRoot, BuiltinID id)
{
    auto scope = DECLARE_THROW_SCOPE(description.vm);
    const BuiltinTable& table = description.table;
    JSArray* uses = describeList(description, summarizeFeatureUses(table.featureUsesOf(id)), describeFeatureUse);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // The table holds no C++ names: only Bun's generators look an entry up by one, and they read builtin-uses.json.
    JSArray* nativeSymbols = constructEmptyArray(description.realm, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSObject* object = constructEmptyObject(description.realm);
    put(description, object, "id"_s, jsNumber(id.index()));
    put(description, object, "path"_s, describeText(description, table.textOf(id)));
    put(description, object, "kind"_s, describeFeatureUserKind(description, id, modulesByRoot[id.index()]));
    put(description, object, "uses"_s, uses);
    put(description, object, "nativeSymbols"_s, nativeSymbols);
    put(description, object, "hasHiddenRoot"_s, jsBoolean(hasHiddenRoot(table, modulesByRoot, id)));
    return object;
}

JSValue BuiltinTable::describeFeatureUsesForTesting(JSGlobalObject* realm, BuiltinPlatform platform) const
{
    VM& vm = realm->vm();
    TableDescription description { realm, vm, *this };
    Vector<RuntimeModuleID> modulesByRoot = findModulesByRoot(*this);

    Vector<BuiltinID> users;
    for (BuiltinID id : idsBelow<BuiltinID>(builtinCount())) {
        if (existsOn(id, platform) && !featureUsesOf(id).empty())
            users.append(id);
    }
    return describeList(description, users, [&](const TableDescription&, BuiltinID id) -> JSValue {
        return describeFeatureUser(description, modulesByRoot, id);
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
