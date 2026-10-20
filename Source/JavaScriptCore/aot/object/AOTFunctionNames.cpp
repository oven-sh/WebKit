/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionNames.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "JSCInlines.h"
#include <algorithm>
#include <optional>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static StringID writtenNameString(const ProgramTables& tables, FunctionID function)
{
    ASSERT(function.index() < tables.functionNames().size());
    return StringID(tables.functionNames()[function.index()]);
}

static ASCIILiteral initialNamePrefix(FunctionKindData kind)
{
    ASSERT(kind.hasInitialNamePrefix());
    if (kind.callable() == CallableKind::Bound)
        return "bound "_s;
    switch (kind.accessor()) {
    case MethodKind::Method:
        break;
    case MethodKind::Getter:
        return "get "_s;
    case MethodKind::Setter:
        return "set "_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasWrittenName(const Image& image, StringID name)
{
    return !image.isObfuscated() && image.text().hasText(name);
}

String writtenFunctionName(FunctionID function)
{
    const Image& image = *Image::current();
    StringID name = writtenNameString(image.programTables(), function);
    return hasWrittenName(image, name) ? image.text().createString(name) : emptyString();
}

String initialNameProperty(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    String written = writtenFunctionName(function);
    return kind.hasInitialNamePrefix() ? makeString(initialNamePrefix(kind), written) : written;
}

JSString* initialNamePropertyString(VM& vm, State& state, FunctionID function)
{
    if (functionKindData(function).hasInitialNamePrefix())
        return jsString(vm, initialNameProperty(function));
    StringID name = writtenNameString(state.tables, function);
    return hasWrittenName(*Image::current(), name) ? state.strings->string(vm, name) : jsEmptyString(vm);
}

static bool isBoundFunction(FunctionID function)
{
    return functionKindData(function).callable() == CallableKind::Bound;
}

static void recordNameSetAtRunTime(VM& vm, FunctionID closureFunction, const String& name, bool isOnlyClosure)
{
    if (isBoundFunction(closureFunction))
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    FunctionID sourceFunction = sourceFunctionOf(closureFunction);
    Locker locker { tables.namesLock };
    if (isOnlyClosure)
        tables.namesSetAtRunTime.set(sourceFunction, name.isolatedCopy());
    else if (auto result = tables.namesSetAtRunTime.add(sourceFunction, String()); result.isNewEntry)
        result.iterator->value = name.isolatedCopy();
    else if (result.iterator->value != name)
        result.iterator->value = String();
    tables.hasNamesSetAtRunTime.store(true, std::memory_order_release);
}

static String nameSetAtRunTime(VM& vm, FunctionID function)
{
    FunctionTables& tables = FunctionTables::of(vm);
    if (!tables.hasNamesSetAtRunTime.load(std::memory_order_acquire)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables.namesLock };
    return tables.namesSetAtRunTime.get(sourceFunction).isolatedCopy();
}

static bool usesOwnPropertiesForStackTraceName(JSCell* closure)
{
    DeclaredAs declared = declaredAs(closure);
    return declared == DeclaredAs::Function || declared == DeclaredAs::Class;
}

// A frame does not say which closure runs in it, and a function that captures something is created more than once as a rule.
static bool standsForEveryFrameOfItsFunction(JSObject* closure)
{
    return usesOwnPropertiesForStackTraceName(closure) && !captureCountsOf(*functionOf(closure)).captures;
}

void didStoreDisplayName(VM& vm, JSObject* closure)
{
    FunctionID closureFunction = *functionOf(closure);
    if (isBoundFunction(closureFunction))
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    FunctionID sourceFunction = sourceFunctionOf(closureFunction);
    Locker locker { tables.namesLock };
    auto& entry = tables.closuresWithDisplayName.add(sourceFunction, FunctionTables::ClosureWithDisplayName { }).iterator->value;
    JSObject* previous = entry.closure.get();
    if (previous != closure) {
        entry.isOneOfSeveral |= !!previous;
        entry.closure = Weak<JSObject>(closure);
    }
    tables.hasDisplayNames.store(true, std::memory_order_relaxed);
}

static JSValue ownDisplayNameValue(VM& vm, JSObject* closure)
{
    UniquedStringImpl* uid = vm.propertyNames->displayName.impl();
    if (auto* classObject = dynamicDowncast<Closure>(closure)) {
        const FieldData* field = StaticFields::findOwnField(*classObject, uid);
        if (field && StaticFields::isInVariable(*classObject, *field))
            return classObject->state().objects->classes.readStaticField(*field);
    }
    unsigned attributes;
    PropertyOffset offset = closure->structure()->getConcurrently(uid, attributes);
    if (offset == invalidOffset || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return JSValue();
    return closure->getDirect(offset);
}

static String singleClosureDisplayName(VM& vm, FunctionID function)
{
    FunctionTables& tables = FunctionTables::of(vm);
    if (!tables.hasDisplayNames.load(std::memory_order_relaxed)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables.namesLock };
    auto entry = tables.closuresWithDisplayName.find(sourceFunction);
    if (entry == tables.closuresWithDisplayName.end() || entry->value.isOneOfSeveral)
        return String();
    JSObject* closure = entry->value.closure.get();
    if (!closure || !standsForEveryFrameOfItsFunction(closure))
        return String();
    JSValue value = ownDisplayNameValue(vm, closure);
    if (!value || !value.isString())
        return String();
    String displayName = asString(value)->tryGetValueWithoutGC();
    return displayName.isEmpty() ? String() : displayName;
}

static String functionNameAsWritten(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    if (FunctionDescription(function).codeType() == ModuleCode || kind.isCommonJSModule)
        return emptyString();
    String written = writtenFunctionName(function);
    return kind.callable() == CallableKind::Bound ? makeString("bound "_s, written) : written;
}

String functionName(VM& vm, FunctionID function, NameForm form)
{
    switch (form) {
    case NameForm::AsWritten:
        return functionNameAsWritten(function);
    case NameForm::InFrames:
        if (String name = nameSetAtRunTime(vm, function); !name.isEmpty()) [[unlikely]]
            return name;
        if (String displayName = singleClosureDisplayName(vm, function); !displayName.isNull()) [[unlikely]]
            return displayName;
        return functionNameAsWritten(function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String computedFunctionName(JSGlobalObject* globalObject, const Identifier& key, MethodKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol()) {
        auto& symbol = static_cast<SymbolImpl&>(*uid);
        if (symbol.isPrivate())
            name = StringImpl::createSubstringSharingImpl(symbol, 0, symbol.length());
        else
            name = symbol.isNullSymbol() ? emptyString() : tryMakeString('[', StringView(uid), ']');
        if (!name) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope, "Function name is too long"_s);
            return { };
        }
    }
    if (kind == MethodKind::Method)
        return name;
    bool isGetter = kind == MethodKind::Getter;
    name = tryMakeString(isGetter ? "get "_s : "set "_s, name);
    if (!name) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope, isGetter ? "Getter name is too long"_s : "Setter name is too long"_s);
    return name;
}

static bool isOnlyClosureOf(FunctionID function, JSObject* closure)
{
    return Instance::of(closure->structure()->realm())->state->objects->closures.closureIfCreated(function) == closure;
}

void didDefineNameProperty(VM& vm, JSObject* closure, const String& name)
{
    FunctionID function = *functionOf(closure);
    bool isInRuntimeModule = functionKindData(function).isInRuntimeModule;
    if (!isInRuntimeModule && !usesOwnPropertiesForStackTraceName(closure))
        return;
    recordNameSetAtRunTime(vm, function, name, isInRuntimeModule || isOnlyClosureOf(function, closure));
}

void setComputedFunctionName(VM& vm, JSObject* closure, const Identifier& key, MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = computedFunctionName(closure->realm(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    CompiledCallee::reifyProperties(vm, closure);
    closure->putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    if (isCompiledClosure(closure)) {
        FunctionRareData* rareData = uncheckedDowncast<Closure>(closure)->ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    }
    didDefineNameProperty(vm, closure, name);
}

JSString* functionSourceText(JSGlobalObject* globalObject, FunctionID function)
{
    const ProgramTables& tables = programTables();
    auto texts = tables.functionTexts();
    if (texts.empty())
        return nullptr;
    std::span entries { tables.at<FunctionTextEntry>(texts[0].entriesOffset), texts[0].numberOfEntries };
    auto entry = std::ranges::lower_bound(entries, function.index(), { }, &FunctionTextEntry::function);
    if (entry == entries.end() || entry->function != function.index())
        return nullptr;
    VM& vm = globalObject->vm();
    std::span bytes { tables.at<Latin1Character>(texts[0].textOffset) + entry->start, entry->end - entry->start };
    if (charactersAreAllASCII(bytes)) [[likely]]
        return jsOwnedString(vm, StringImpl::createWithoutCopying(bytes));
    return jsString(vm, String::fromUTF8ReplacingInvalidSequences(bytes));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
