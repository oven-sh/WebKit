/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTErrorMessages.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTCompiledCallee.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTImageOfCellScope.h"
#include "AOTLayoutData.h"
#include "AOTModuleHost.h"
#include "AOTModuleTable.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPrimitives.h"
#include "AOTStackTrace.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeType.h"
#include "AOTState.h"
#include "AOTTraceFrame.h"
#include "AOTVector.h"
#include "ArgList.h"
#include "ConstructData.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "StackFrame.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <unicode/utf16.h>
#include <unicode/utf8.h>
#include <wtf/FileSystem.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

namespace Primitive = Typed::Primitive;

static bool isObfuscated()
{
    return Image::current()->isObfuscated();
}

static bool hasNonEmptyText(StringID string)
{
    const ImageText& text = Image::current()->text();
    return string && text.hasText(string) && !text.view(string).isEmpty();
}

static String functionName(JSObject* function)
{
    if (CompiledCallee::isCompiledCallee(function))
        return CompiledCallee::name(function);
    if (auto* jsFunction = dynamicDowncast<JSFunction>(function))
        return jsFunction->name(function->vm());
    if (auto* internalFunction = dynamicDowncast<InternalFunction>(function))
        return internalFunction->name();
    return { };
}

static String constructorName(JSObject* prototype)
{
    if (auto* lazyPrototype = dynamicDowncast<ClassPrototype>(prototype)) {
        ImageOfCellScope imageOfPrototype(prototype);
        const ClassTable& classes = (*Instance::owning(prototype)->state).objects->classes;
        return initialNameProperty(FunctionID(classes.classOf(*lazyPrototype)->classData().classConstructor));
    }
    if (CompiledCallee::isCompiledCallee(prototype))
        return { };
    VM& vm = prototype->vm();
    JSValue constructor = prototype->getDirect(vm, vm.propertyNames->constructor);
    return constructor && constructor.isObject() ? functionName(asObject(constructor)) : String();
}

static String className(JSObject* object)
{
    if (JSValue prototype = object->getPrototypeDirect(); prototype.isObject()) {
        if (String name = constructorName(asObject(prototype)); !name.isEmpty())
            return name;
    }
    return object->classInfo()->className;
}

static String describeNamed(ASCIILiteral kind, const String& name)
{
    return name.isEmpty() ? String(kind) : makeString(kind, ' ', name);
}

static String describeInstance(const ProgramTables& tables, const LayoutData& layout)
{
    if (!layout.isUntyped)
        return nameOf(tables, layout);
    if (isObfuscated() || !hasNonEmptyText(StringID(layout.name)))
        return "untyped object"_s;
    return makeString("untyped "_s, ProgramStrings::text(tables, StringID(layout.name)));
}

String describe(JSValue value)
{
    if (!value)
        return "nothing"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return "boolean"_s;
    if (value.isNumber())
        return "number"_s;
    if (value.isString())
        return "string"_s;
    if (value.isSymbol())
        return "symbol"_s;
    if (value.isBigInt())
        return "bigint"_s;
    if (Object* object = compiledObjectOrNull(value))
        return describeInstance(object->state().tables, object->layout());
    if (!value.isObject())
        return "internal cell"_s;
    JSObject* object = asObject(value);
    if (CompiledCallee::isCompiledCallee(object) && CompiledCallee::kind(object) == CompiledCallee::FunctionKind::Class)
        return describeNamed("class"_s, functionName(object));
    if (object->isCallable())
        return describeNamed("function"_s, functionName(object));
    if (dynamicDowncast<JSArray>(object))
        return "Array"_s;
    if (object->type() != FinalObjectType && !dynamicDowncast<ClassPrototype>(object))
        return makeString("built-in "_s, className(object));
    JSValue prototype = object->getPrototypeDirect();
    bool hasPlainPrototype = prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
    return hasPlainPrototype ? "untyped object"_str : makeString("untyped "_s, className(object));
}

static void appendUnionMember(StringBuilder& result, bool has, StringView name)
{
    if (has)
        result.append(result.isEmpty() ? ""_s : " | "_s, name);
}

static void appendPrimitives(StringBuilder& result, uint32_t primitives)
{
    // Both together go without saying beside another member.
    if ((primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull && (!result.isEmpty() || primitives & ~Primitive::UndefinedOrNull))
        primitives &= ~Primitive::UndefinedOrNull;
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::Boolean, "boolean"_s);
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::True, "true"_s);
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::False, "false"_s);
    appendUnionMember(result, primitives & Primitive::Double, "number"_s);
    appendUnionMember(result, (primitives & Primitive::Number) == Primitive::Integer, "integer"_s);
    appendUnionMember(result, primitives & Primitive::String, "string"_s);
    appendUnionMember(result, primitives & Primitive::Symbol, "symbol"_s);
    appendUnionMember(result, primitives & Primitive::BigInt, "bigint"_s);
    appendUnionMember(result, primitives & Primitive::Undefined, "undefined"_s);
    appendUnionMember(result, primitives & Primitive::Null, "null"_s);
    appendUnionMember(result, result.isEmpty(), "never"_s);
}

static String describeAnonymousType(std::span<const FieldData> fields)
{
    static constexpr size_t maxNamesShown = 4;
    const Image& image = *Image::current();
    StringBuilder result;
    result.append('{');
    size_t numberOfNames = 0;
    for (const FieldData& field : fields) {
        String name = ProgramIdentifiers::textOf(image, field.name);
        if (name.isNull())
            continue;
        if (numberOfNames == maxNamesShown) {
            result.append(", ..."_s);
            break;
        }
        result.append(numberOfNames++ ? ", "_s : " "_s, name);
    }
    result.append(" }"_s);
    return result.toString();
}

String nameOf(const ProgramTables& tables, const LayoutData& layout)
{
    if (isObfuscated())
        return "object"_s;
    if (hasNonEmptyText(StringID(layout.name)))
        return ProgramStrings::text(tables, StringID(layout.name));
    return describeAnonymousType(layout.fields());
}

static String describeMembers(const ProgramTables& tables, const ShapeTestData& members)
{
    static constexpr unsigned maxNamesShown = 4;
    Vector<String, maxNamesShown> names;
    StringBuilder result;
    for (uint32_t shape = members.first(); shape < members.first() + members.count; ++shape) {
        if (!tables.hasLayoutForShape(shape) || !tables.passes(members, shape))
            continue;
        String name = nameOf(tables, tables.layoutForShape(shape));
        if (names.contains(name))
            continue;
        if (names.size() == maxNamesShown) {
            appendUnionMember(result, true, "..."_s);
            break;
        }
        appendUnionMember(result, true, name);
        names.append(WTF::move(name));
    }
    appendUnionMember(result, result.isEmpty(), "object"_s);
    return result.toString();
}

String nameOf(const ProgramTables& tables, const LayoutSetData& set)
{
    if (isObfuscated())
        return "object"_s;
    if (hasNonEmptyText(StringID(set.name)))
        return ProgramStrings::text(tables, StringID(set.name));
    return describeMembers(tables, set.members);
}

static String describeArray(const String& elements)
{
    return elements.contains(" | "_s) ? makeString('(', elements, ")[]"_s) : makeString(elements, "[]"_s);
}

static String builtinClassName(const BuiltinClass& builtinClass, const String& elements)
{
    if (builtinClass.type == ArrayType)
        return describeArray(elements.isNull() ? "any"_str : elements);
    return builtinClass.constructor ? BuiltinTable::singleton().textOf(builtinClass.constructor) : "object"_str;
}

static String builtinClassName(JSType cellType, const String& elements)
{
    JSType describedType = cellType == DerivedArrayType ? ArrayType : cellType;
    auto& table = BuiltinTable::singleton();
    for (unsigned i = 0; table.contains(BuiltinClassID(i)); ++i) {
        const BuiltinClass& builtinClass = table.at(BuiltinClassID(i));
        if (builtinClass.instanceTest == BuiltinInstanceTest::JSTypeEquals && builtinClass.type == describedType && builtinClass.constructor)
            return builtinClassName(builtinClass, elements);
    }
    return "object"_s;
}

static String builtinCellName(const ProgramTables& tables, uint32_t number)
{
    auto& table = BuiltinTable::singleton();
    auto key = BuiltinCellKey::decode(tables.builtinCells()[number], table);
    return key ? makeString("typeof "_s, table.textOf(key->builtin())) : "object"_str;
}

static String namespaceName(const ProgramTables& tables, ModuleID module)
{
    String path = isObfuscated() ? String() : ModuleTable(tables).displayPathText(module);
    if (path.isEmpty())
        return "object"_s;
    return makeString("typeof import(\""_s, path, "\")"_s);
}

static void appendUnique(Vector<String, 2>& names, String&& name)
{
    if (!names.contains(name))
        names.append(WTF::move(name));
}

String describe(const ProgramTables& tables, const RuntimeType& type)
{
    static constexpr uint32_t allValues = Primitive::Boolean | Primitive::Number | Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull;
    static constexpr size_t maxLiteralsShown = 4;
    uint32_t primitives = type.primitiveSet().toRaw();
    bool allowsAnyObject = false;
    bool allowsAnyFunction = false;
    Vector<String, 2> layoutSetNames;
    Vector<String, 2> cellNames;
    String elements;
    if (auto offset = type.contents())
        elements = describe(tables, tables.typeAt(*offset));
    for (const RuntimeCellTest& test : type.cellTests()) {
        switch (test.kind) {
        case CellTestKind::IsObject:
        case CellTestKind::ShapeIn:
        case CellTestKind::CarriesBrandOfClass:
            if (auto set = type.layoutSet())
                appendUnique(layoutSetNames, nameOf(tables, tables.layoutSets()[*set]));
            else if (test.kind != CellTestKind::ShapeIn)
                allowsAnyObject = true;
            else
                appendUnique(layoutSetNames, describeMembers(tables, tables.shapeTestAt(test.shapeTestOffset())));
            continue;
        case CellTestKind::IsCallable:
            allowsAnyFunction = true;
            continue;
        case CellTestKind::CellTypeIs:
            for (unsigned cellType = test.cellTypes().first; cellType <= test.cellTypes().last; ++cellType) {
                if (cellType != ArrayBufferType) {
                    appendUnique(cellNames, builtinClassName(static_cast<JSType>(cellType), elements));
                    continue;
                }
                for (ArrayBufferSharingMode sharingMode : { ArrayBufferSharingMode::Default, ArrayBufferSharingMode::Shared })
                    appendUnique(cellNames, arrayBufferSharingModeName(sharingMode));
            }
            continue;
        case CellTestKind::InheritsClassInfo:
        case CellTestKind::IsPristine:
            appendUnique(cellNames, builtinClassName(BuiltinTable::singleton().at(test.builtinClass()), elements));
            continue;
        case CellTestKind::IsBuiltinCell:
            appendUnique(cellNames, builtinCellName(tables, test.builtinCellNumber()));
            continue;
        case CellTestKind::IsClosureOf:
            if (String name = initialNameProperty(closureFunctionWithNumber(test.closureNumber())); !name.isEmpty())
                appendUnique(cellNames, makeString("typeof "_s, name));
            else
                allowsAnyFunction = true;
            continue;
        case CellTestKind::IsSymbol:
            if (StringID description = ProgramStrings::identifierString(tables, test.symbolKeyNumber()); hasNonEmptyText(description))
                appendUnique(cellNames, makeString("typeof "_s, ProgramStrings::text(tables, description)));
            else
                primitives |= Primitive::Symbol;
            continue;
        case CellTestKind::SharingModeIs:
            appendUnique(cellNames, arrayBufferSharingModeName(test.sharingMode()));
            continue;
        case CellTestKind::IsNamespaceOf:
            appendUnique(cellNames, namespaceName(tables, test.module()));
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (allowsAnyObject && (primitives & allValues) == allValues)
        return "any"_s;
    StringBuilder result;
    for (auto& name : layoutSetNames)
        appendUnionMember(result, true, name);
    for (auto& name : cellNames)
        appendUnionMember(result, true, name);
    appendUnionMember(result, allowsAnyObject, "object"_s);
    appendUnionMember(result, allowsAnyFunction && !allowsAnyObject, "Function"_s);
    size_t numberOfLiterals = 0;
    for (uint32_t string : type.literalStrings()) {
        if (!Image::current()->text().hasText(StringID(string)))
            continue;
        if (numberOfLiterals++ < maxLiteralsShown)
            appendUnionMember(result, true, makeString('"', ProgramStrings::text(tables, StringID(string)), '"'));
    }
    appendUnionMember(result, numberOfLiterals > maxLiteralsShown, "..."_s);
    primitives &= numberOfLiterals ? Primitive::All & ~Primitive::String : Primitive::All;
    auto range = type.integerRange();
    if ((primitives & Primitive::Number) == Primitive::Integer && range && range->first >= -Typed::maxExactInteger && range->second <= Typed::maxExactInteger) {
        appendUnionMember(result, true, range->first == range->second ? makeString(range->first) : makeString("integer "_s, range->first, ".."_s, range->second));
        primitives &= ~Primitive::Number;
    }
    appendPrimitives(result, primitives);
    return result.toString();
}

static bool namesLayoutSet(const ProgramTables& tables, const RuntimeType& type)
{
    auto set = type.layoutSet();
    if (set && hasNonEmptyText(StringID(tables.layoutSets()[*set].name)))
        return true;
    auto contents = type.contents();
    return contents && namesLayoutSet(tables, tables.typeAt(*contents));
}

// A test that asks only for an object says less than the declared type, which is the text of the site.
static bool acceptsAnyObject(const ProgramTables& tables, const RuntimeType& type)
{
    bool hasObjectTest = std::ranges::any_of(type.cellTests(), [](const RuntimeCellTest& test) {
        return test.kind == CellTestKind::IsObject;
    });
    if (hasObjectTest && !type.layoutSet())
        return true;
    auto contents = type.contents();
    return contents && acceptsAnyObject(tables, tables.typeAt(*contents));
}

String describe(const ProgramTables& tables, const RuntimeType& type, Typed::TypeSiteID site)
{
    const TypeSiteData* data = site ? tables.findTypeSite(site.index()) : nullptr;
    // The test of a cast asks for a family, which lists every layout the program converts to the type behind `as`. The text of the site is that type.
    bool isAssertion = data && static_cast<TypeSiteKind>(data->kind) == TypeSiteKind::Assertion;
    if (!data || data->text == TypeSiteData::noText || (!isAssertion && !namesLayoutSet(tables, type) && !acceptsAnyObject(tables, type)))
        return describe(tables, type);
    return ProgramStrings::text(tables, StringID(data->text));
}

static thread_local const String* quotedSource;
// The run time has looked for what the program spells at the place, and has found nothing.
static thread_local bool hasNothingToQuote;
static String findCallOfNativeFunction(VM&);

String quotedSourceOfFailedExpression(VM& vm, bool messageEndsInQuote)
{
    if (quotedSource)
        return *quotedSource;
    if (!vm.hasCompiledCode() || hasNothingToQuote)
        return { };
    if (StringView constructor = nameOfConstructorThatRunsOnTop(vm); !constructor.isNull())
        return tryMakeString("new "_s, constructor);
    // `Function.prototype.call.call(o)`: what is no function is refused in `call`, which is JavaScript of the engine's own under `bun run`, and nothing is quoted out of that.
    return messageEndsInQuote ? findCallOfNativeFunction(vm) : String();
}

static JSObject* createQuoting(const String& source, JSObject* (*create)(JSGlobalObject*, JSValue), JSGlobalObject* globalObject, JSValue value)
{
    SetForScope quotedInScope(quotedSource, source.isNull() ? nullptr : &source);
    SetForScope nothingInScope(hasNothingToQuote, source.isNull());
    return create(globalObject, value);
}


// As the engine's message describes the value. Null where it cannot be described.
static String describe(JSGlobalObject* globalObject, JSValue value)
{
    DeferTerminationForAWhile deferTermination(globalObject->vm());
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    String description = errorDescriptionForValue(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return { };
    }
    return description;
}

JSObject* createUpdateOfNullishError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, "Cannot access property of undefined or null"_s);
}

static JSObject* createNotAnObjectErrorQuoting(const String& source, JSGlobalObject* globalObject, JSValue base)
{
    if (readOnTopTestsObjectOfUpdate(globalObject->vm()))
        return createUpdateOfNullishError(globalObject);
    return createQuoting(source, JSC::createNotAnObjectError, globalObject, base);
}

// `o.name`, `o.#name`. Any other key stands in brackets: `o["two words"]`.
static bool isWrittenBehindDot(StringView member)
{
    if (member.isEmpty() || isASCIIDigit(member[0]))
        return false;
    for (unsigned index = 0; index < member.length(); ++index) {
        char16_t character = member[index];
        if (isASCII(character) && !isASCIIAlphanumeric(character) && character != '_' && character != '$' && (character != '#' || index))
            return false;
    }
    return true;
}

static bool isPartOfName(Latin1Character character)
{
    return isASCIIAlphanumeric(character) || character == '_' || character == '$' || character >= 0x80;
}

static bool isQuote(Latin1Character character)
{
    return character == '"' || character == '\'' || character == '`';
}

// Where the line of `text[at]` starts.
static size_t startOfLine(std::span<const Latin1Character> text, size_t at)
{
    size_t lineFeed = at ? WTF::reverseFind(text, static_cast<Latin1Character>('\n'), at - 1) : notFound;
    return lineFeed == notFound ? 0 : lineFeed + 1;
}

// Where the code of the line in front of the line break `text[lineBreak]` ends: in front of a `//` comment and of the blanks at the end.
static size_t endOfCodeInFrontOf(std::span<const Latin1Character> text, size_t lineBreak)
{
    size_t start = startOfLine(text, lineBreak);
    size_t end = lineBreak;
    for (size_t at = start; at + 1 < lineBreak; ++at) {
        if (isQuote(text[at])) {
            Latin1Character quote = text[at];
            for (++at; at < lineBreak && text[at] != quote; ++at)
                at += text[at] == '\\';
        } else if (text[at] == '/' && text[at + 1] == '/') {
            end = at;
            break;
        }
    }
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        --end;
    return end;
}

// Where the code in front of `at` ends: in front of blanks, of line breaks and of lines that are comments.
static size_t endOfCodeBefore(std::span<const Latin1Character> text, size_t at)
{
    while (at && (text[at - 1] == ' ' || text[at - 1] == '\t'))
        --at;
    while (at && text[at - 1] == '\n')
        at = endOfCodeInFrontOf(text, at - 1);
    return at;
}

static bool isFirstInLine(std::span<const Latin1Character> text, size_t at)
{
    while (at && (text[at - 1] == ' ' || text[at - 1] == '\t'))
        --at;
    return at && text[at - 1] == '\n';
}

// Where the code goes on in a later line, if blanks and a `//` comment are all that the line has behind `at`. Lines that are comments may stand between. Otherwise `at`.
static size_t startOfCodeInLaterLine(std::span<const Latin1Character> text, size_t at)
{
    bool hasLineBreak = false;
    size_t next = at;
    while (next < text.size()) {
        Latin1Character character = text[next];
        if (character == '/' && next + 1 < text.size() && text[next + 1] == '/')
            next = std::min(WTF::find(text, static_cast<Latin1Character>('\n'), next), text.size());
        else if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
            hasLineBreak |= character == '\n';
            ++next;
        } else
            break;
    }
    return hasLineBreak && next < text.size() ? next : at;
}

// The place of the bracket that `text[close]` closes. Knows texts in quotes, `//` comments and nothing else of the language, much as the engine's functionCallBase():
// where that is not enough the brackets do not pair up, or the answer is quoted as it stands in the source.
static std::optional<size_t> findOpeningBracket(std::span<const Latin1Character> text, size_t close)
{
    unsigned depth = 0;
    for (size_t at = close + 1; at--;) {
        Latin1Character character = text[at];
        if (character == '\n') {
            at = endOfCodeInFrontOf(text, at);
            continue;
        }
        if (isQuote(character)) {
            size_t opening = at;
            do {
                if (!opening || (text[opening - 1] == '\n' && character != '`'))
                    return std::nullopt;
                --opening;
            } while (text[opening] != character || (opening && text[opening - 1] == '\\'));
            at = opening;
        } else if (character == ')' || character == ']')
            ++depth;
        else if ((character == '(' || character == '[') && !--depth)
            return at;
    }
    return std::nullopt;
}

static std::optional<size_t> findClosingBracket(std::span<const Latin1Character> text, size_t open)
{
    unsigned depth = 0;
    for (size_t at = open; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            for (++at; at < text.size() && text[at] != character; ++at) {
                if (text[at] == '\n' && character != '`')
                    return std::nullopt;
                if (text[at] == '\\')
                    ++at;
            }
        } else if (character == '/' && at + 1 < text.size() && text[at + 1] == '/')
            at = std::min(WTF::find(text, static_cast<Latin1Character>('\n'), at), text.size());
        else if (character == '/' && at + 1 < text.size() && text[at + 1] == '*') {
            for (at += 2; at + 1 < text.size() && (text[at] != '*' || text[at + 1] != '/'); ++at) { }
            ++at;
        } else if (character == '/') {
            // `= /[)\]]/`: behind a value it divides.
            size_t inFront = at;
            while (inFront > open && (text[inFront - 1] == ' ' || text[inFront - 1] == '\n'))
                --inFront;
            Latin1Character last = text[inFront - 1];
            if (isPartOfName(last) || last == ')' || last == ']' || last == '}' || isQuote(last))
                continue;
            bool isInClass = false;
            for (++at; at < text.size() && text[at] != '\n' && (text[at] != '/' || isInClass); ++at) {
                isInClass = text[at] == '[' || (isInClass && text[at] != ']');
                at += text[at] == '\\';
            }
        } else if (character == '(' || character == '[')
            ++depth;
        else if ((character == ')' || character == ']') && !--depth)
            return at;
    }
    return std::nullopt;
}

// The place of the backtick that closes the template which `text[open]` opens.
static std::optional<size_t> findEndOfTemplate(std::span<const Latin1Character> text, size_t open)
{
    for (size_t at = open + 1; at < text.size(); ++at) {
        if (text[at] == '\\')
            ++at;
        else if (text[at] == '`')
            return at;
        else if (text[at] == '$' && at + 1 < text.size() && text[at + 1] == '{') {
            unsigned depth = 0;
            for (++at; at < text.size(); ++at) {
                Latin1Character character = text[at];
                if (character == '`') {
                    std::optional<size_t> end = findEndOfTemplate(text, at);
                    if (!end)
                        return std::nullopt;
                    at = *end;
                } else if (isQuote(character)) {
                    for (++at; at < text.size() && text[at] != character; ++at)
                        at += text[at] == '\\';
                } else if (character == '{')
                    ++depth;
                else if (character == '}' && !--depth)
                    break;
            }
        }
    }
    return std::nullopt;
}

// `a.b` of `a.b.c` and `make("x")[0]` of `make("x")[0].c`, where `end` is the place of the last dot or of its `?`: names, dots, and what stands in brackets behind them.
// Round brackets may stand in front of it all: `(a as T)` of `(a as T).c`, `(a + b)` of `(a + b).c`. So may an array literal: `[a, b][1]` of `[a, b][1].c`.
static bool isHeadOfStatement(std::span<const Latin1Character> text, size_t open)
{
    if (text[open] != '(')
        return false;
    size_t end = open;
    while (end && text[end - 1] == ' ')
        --end;
    size_t word = end;
    while (word && isPartOfName(text[word - 1]))
        --word;
    if (word && (text[word - 1] == '.' || text[word - 1] == '#'))
        return false;
    for (ASCIILiteral keyword : { "if"_s, "while"_s, "for"_s, "with"_s }) {
        if (equalSpans(text.subspan(word, end - word), byteCast<Latin1Character>(keyword.span())))
            return true;
    }
    return false;
}

static std::optional<size_t> startOfPathInFrontOf(std::span<const Latin1Character> text, size_t end)
{
    size_t start = end;
    while (true) {
        // A chain: the dot or the `[` is the first thing in its line, and lines that are comments may stand between.
        size_t code = endOfCodeBefore(text, start);
        if (code != start && code && text[code - 1] != '\n' && start < text.size() && (text[start] == '.' || text[start] == '?' || (text[start] == '[' && isFirstInLine(text, start))))
            start = code;
        // `a!.b`, in a file that still has its types.
        size_t part = start - (start && text[start - 1] == '!');
        while (part && (text[part - 1] == ')' || text[part - 1] == ']')) {
            std::optional<size_t> opening = findOpeningBracket(text, part - 1);
            if (!opening)
                return std::nullopt;
            // `if(a)(b || c).d`: the head of a statement is no call of what stands behind it.
            if (part != start && text[part] == '(' && isHeadOfStatement(text, *opening))
                return part;
            part = *opening;
            if (part >= 2 && text[part - 1] == '.' && text[part - 2] == '?')
                part -= 2;
            part -= part && text[part - 1] == '!';
        }
        size_t name = part;
        while (name && isPartOfName(text[name - 1]))
            --name;
        if (name == part) {
            if (part != start && (text[part] == '(' || text[part] == '['))
                return part;
            return std::nullopt;
        }
        if (name && text[name - 1] == '#')
            --name;
        start = name;
        // The dot may be the last thing in the line in front.
        size_t behindDot = isFirstInLine(text, start) ? endOfCodeBefore(text, start) : start;
        if (!behindDot || text[behindDot - 1] != '.')
            return start;
        size_t before = behindDot - 1;
        // `...a.b`
        if (before && text[before - 1] == '.')
            return start;
        if (before && text[before - 1] == '?')
            --before;
        start = before;
    }
}

// `new K(a).b`: `new` takes the first call of the path, so it belongs to what stands in front of `.b`. In `new a.b.K()` it does not belong to `a.b`.
static size_t startWithNew(std::span<const Latin1Character> text, size_t start, size_t end)
{
    bool followsNew = start >= 4 && text[start - 4] == 'n' && text[start - 3] == 'e' && text[start - 2] == 'w' && text[start - 1] == ' ' && (start == 4 || !isPartOfName(text[start - 5]));
    return followsNew && WTF::find(text.subspan(start, end - start), static_cast<Latin1Character>('(')) != notFound ? start - 4 : start;
}

// Where the path that begins at `start` ends: names, dots, and what stands in brackets behind them. `start` where there is no path.
// A chain goes on in a later line that begins with a dot or a bracket, and behind a dot that ends a line.
static size_t endOfPathFrom(std::span<const Latin1Character> text, size_t start)
{
    size_t at = start;
    while (at < text.size()) {
        Latin1Character character = text[at];
        Latin1Character next = at + 1 < text.size() ? text[at + 1] : Latin1Character(0);
        if (isPartOfName(character) || character == '#' || character == '.')
            ++at;
        else if ((character == '?' && next == '.') || (character == '!' && (next == '.' || next == '[')))
            ++at;
        else if (character == '(' || character == '[') {
            std::optional<size_t> close = findClosingBracket(text, at);
            if (!close)
                return start;
            at = *close + 1;
        } else {
            // `a!` may end a line, in a file that still has its types.
            size_t endOfLine = at + (character == '!' && at > start);
            size_t later = startOfCodeInLaterLine(text, endOfLine);
            if (later == endOfLine)
                break;
            Latin1Character first = text[later];
            Latin1Character second = later + 1 < text.size() ? text[later + 1] : Latin1Character(0);
            bool goesOn = (first == '.' && second != '.') || (first == '?' && second == '.') || first == '[' || first == '(' || (endOfLine == at && at && text[at - 1] == '.' && isPartOfName(first));
            if (!goesOn)
                break;
            at = later;
        }
    }
    return at;
}

static size_t skipQuoted(std::span<const Latin1Character> text, size_t at, size_t to)
{
    Latin1Character quote = text[at];
    for (++at; at < to && text[at] != quote; ++at)
        at += text[at] == '\\';
    return at;
}

static bool isWordInFrontOfValue(std::span<const Latin1Character> word)
{
    for (ASCIILiteral keyword : { "new"_s, "return"_s, "typeof"_s, "await"_s, "void"_s, "delete"_s, "yield"_s, "throw"_s, "case"_s, "in"_s, "instanceof"_s, "of"_s }) {
        if (equalSpans(word, byteCast<Latin1Character>(keyword.span())))
            return true;
    }
    return false;
}

// Where `text[at]` opens a text in quotes or a regular expression: the place of what closes it. Otherwise nothing.
// A slash opens a regular expression where no value stands in front of it: `a = /[;'"]/.test(b)`, and not `a / b`.
static std::optional<size_t> endOfQuotedOrPattern(std::span<const Latin1Character> text, size_t from, size_t at, size_t to)
{
    if (isQuote(text[at]))
        return skipQuoted(text, at, to);
    if (text[at] != '/' || (at + 1 < to && (text[at + 1] == '/' || text[at + 1] == '*')))
        return std::nullopt;
    size_t inFront = at;
    while (inFront > from && text[inFront - 1] == ' ')
        --inFront;
    if (inFront > from && (isPartOfName(text[inFront - 1]) || text[inFront - 1] == ')' || text[inFront - 1] == ']' || isQuote(text[inFront - 1]))) {
        size_t word = inFront;
        while (word > from && isPartOfName(text[word - 1]))
            --word;
        if (!isWordInFrontOfValue(text.subspan(word, inFront - word)))
            return std::nullopt;
    }
    bool isInClass = false;
    for (size_t end = at + 1; end < to && text[end] != '\n'; ++end) {
        Latin1Character character = text[end];
        if (character == '\\')
            ++end;
        else if (character == '[')
            isInClass = true;
        else if (character == ']')
            isInClass = false;
        else if (character == '/' && !isInClass)
            return end;
    }
    return std::nullopt;
}

enum class WrittenReads : uint8_t { None, Same, Differ };

// Goes straight to where a text may spell a name: every place that spells it has these bytes.
class BytesOfName {
public:
    explicit BytesOfName(StringView name)
    {
        for (unsigned index = 0; index < name.length(); ++index)
            m_bytes.append(static_cast<Latin1Character>(name[index]));
    }

    // The first such place in `text[from, to)`.
    size_t findIn(std::span<const Latin1Character> text, size_t from, size_t to) const
    {
        size_t found = WTF::find(text.subspan(from, to - from), m_bytes.span());
        return found == notFound ? notFound : from + found;
    }

private:
    Vector<Latin1Character, 64> m_bytes;
};

// `a.b = c + 1` where `end` is behind `a.b`: a store is quoted with the value. Otherwise `end`.
static size_t endWithStoredValue(std::span<const Latin1Character> text, size_t end)
{
    size_t to = text.size();
    size_t at = end;
    while (at < to && text[at] == ' ')
        ++at;
    if (at + 1 >= to || text[at] != '=' || text[at + 1] == '=' || text[at + 1] == '>')
        return end;
    unsigned depth = 0;
    bool hasValue = false;
    for (++at; at < to; ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            for (++at; at < to && text[at] != character; ++at)
                at += text[at] == '\\';
        } else if (character == '(' || character == '[' || character == '{')
            ++depth;
        else if (character == ')' || character == ']' || character == '}') {
            if (!depth--)
                break;
        } else if ((character == ';' || character == ',') && !depth)
            break;
        else if (character == '\n' && !depth && hasValue)
            break;
        hasValue |= character != ' ' && character != '\n' && character != '\r' && character != '\t';
    }
    if (depth && at >= to)
        return end;
    at = std::min(at, to);
    while (text[at - 1] == ' ' || text[at - 1] == '\r')
        --at;
    return at;
}

// Each place in `text[from, to)` that reads `member` behind a dot. Same: all spell what they read it from alike, and `found` holds that with the member.
// The place of a read that failed lies in what it reads from. Of `a.x.b.x` both reads of `x` hold the place `a`, and the second reads from the first: the first is meant.
static WrittenReads findWrittenReads(std::span<const Latin1Character> text, size_t from, size_t to, StringView member, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    BytesOfName name(member);
    // The member stands behind what it is read from, which holds the place.
    for (size_t at = std::max(from, within.value_or(from)) + 1; at + member.length() <= to; ++at) {
        at = name.findIn(text, at, to);
        if (at == notFound)
            break;
        if (isPartOfName(text[at - 1]) || (at + member.length() < text.size() && isPartOfName(text[at + member.length()])))
            continue;
        if (!equal(member, text.subspan(at, member.length())))
            continue;
        // The dot may be the last thing in the line in front.
        size_t behindDot = text[at - 1] != '.' && isFirstInLine(text, at) ? endOfCodeBefore(text, at) : at;
        if (!behindDot || text[behindDot - 1] != '.')
            continue;
        size_t dot = behindDot - 1;
        if (dot && text[dot - 1] == '?')
            --dot;
        std::optional<size_t> start = startOfPathInFrontOf(text, dot);
        if (!start) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        start = startWithNew(text, *start, dot);
        if (within && (*within < *start || *within >= at))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, endWithStoredValue(text, at + member.length()) - *start);
        if (within) {
            found = read;
            return WrittenReads::Same;
        }
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// Each place in `text[from, to)` that reads an element: `a.b[k]`. The place of such a read is that of `a.b`, so of several that hold it the first is meant.
// In a file that runs as it is written it is that of the `[`.
// `name` is "[]" where the key is computed as the program runs. Otherwise the compiler knows the key, as it may where the source has `a[k]`: that is quoted only where
// the place is known. Whether `bun run` knows the key too, and prints the name, is what TextPrintedInPlace says.
static WrittenReads findWrittenElements(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    bool isComputed = name == "[]"_s;
    if (!isComputed && !within)
        return WrittenReads::None;
    for (size_t at = from + 1; at < to; ++at) {
        if (std::optional<size_t> end = endOfQuotedOrPattern(text, from, at, to)) {
            at = *end;
            continue;
        }
        if (text[at] != '[')
            continue;
        size_t inFront = isFirstInLine(text, at) ? endOfCodeBefore(text, at) : at;
        if (!inFront || !(isPartOfName(text[inFront - 1]) || text[inFront - 1] == ')' || text[inFront - 1] == ']' || text[inFront - 1] == '!'))
            continue;
        std::optional<size_t> start = startOfPathInFrontOf(text, at);
        std::optional<size_t> close = findClosingBracket(text, at);
        if (!start || !close || *close >= to) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        // An array literal is an object: in `[a, b][1][0]` it is not `[a, b][1]` that failed.
        if (text[*start] == '[' && findClosingBracket(text, *start) == std::optional<size_t>(at - 1))
            continue;
        start = startWithNew(text, *start, at);
        if (within && (*within < *start || *within > at))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, endWithStoredValue(text, *close + 1) - *start);
        if (within) {
            found = read;
            return WrittenReads::Same;
        }
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// Each place in `text[from, to)` that reads a private name: `a.b.#x`.
static WrittenReads findWrittenPrivateNames(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    for (size_t at = std::max(from, within.value_or(from)) + 1; at + 1 < to; ++at) {
        if (text[at - 1] != '.' || text[at] != '#')
            continue;
        size_t end = at + 1;
        while (end < to && isPartOfName(text[end]))
            ++end;
        size_t dot = at - 1;
        if (dot && text[dot - 1] == '?')
            --dot;
        std::optional<size_t> start = startOfPathInFrontOf(text, dot);
        if (!start) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        if (within && (*within < *start || *within >= at))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, endWithStoredValue(text, end) - *start);
        // As in findWrittenReads().
        if (within) {
            found = read;
            return WrittenReads::Same;
        }
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// Where the quote of a loop starts, in the head `text[from, to)`: at `x` in `const x`, at `const` in `const [a, b]` and `const { a }`, and at the start of what is assigned to: `o.x`, `[a, b]`.
static size_t startOfLoopTarget(std::span<const Latin1Character> text, size_t from, size_t to)
{
    auto skipBlanks = [&](size_t at) {
        while (at < to && (text[at] == ' ' || text[at] == '\n'))
            ++at;
        return at;
    };
    size_t start = skipBlanks(from);
    for (ASCIILiteral word : { "const"_s, "let"_s, "var"_s }) {
        size_t behind = start + word.length();
        if (behind >= to || isPartOfName(text[behind]) || !equal(StringView(word), text.subspan(start, word.length())))
            continue;
        size_t target = skipBlanks(behind);
        return target < to && isPartOfName(text[target]) ? target : start;
    }
    return start;
}

// `x of v` of `for (const x of v)`, and `[a, b]` of `const [a, b] = v`. The place of a loop lies in its head, or is the loop's own where the head has no name. That of a list is the list or stands in front of it.
// Only a file says where that is.
static WrittenReads findWrittenIterations(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    if (!within || *within >= to)
        return WrittenReads::None;
    auto isBlank = [](Latin1Character character) {
        return character == ' ' || character == '\n';
    };
    // `for ([a, b] of v)`: at the list it is the list that was given no list. A head may take several lines, so the `for` may stand on a line in front.
    for (size_t at = text[*within] == '[' ? 0 : *within + 1; at--;) {
        if (at + 4 >= text.size() || text[at] != 'f' || text[at + 1] != 'o' || text[at + 2] != 'r' || (text[at + 3] != ' ' && text[at + 3] != '(') || (at && isPartOfName(text[at - 1])))
            continue;
        size_t open = WTF::find(text, static_cast<Latin1Character>('('), at);
        std::optional<size_t> close = open != notFound && (open < *within || at == *within) ? findClosingBracket(text, open) : std::nullopt;
        if (!close || *close <= *within) {
            if (at < from)
                break;
            continue;
        }
        for (size_t word = std::max(*within, open); word + 4 < *close; ++word) {
            // `x of v`, and `[a,b]of v` as a minifier leaves it.
            bool closesPattern = text[word] == ']' || text[word] == '}';
            if ((isBlank(text[word]) || closesPattern) && text[word + 1] == 'o' && text[word + 2] == 'f' && isBlank(text[word + 3])) {
                size_t start = startOfLoopTarget(text, open + 1, word + closesPattern);
                found = text.subspan(start, *close - start);
                return WrittenReads::Same;
            }
        }
    }
    for (size_t at = *within; at < to; ++at) {
        Latin1Character character = text[at];
        if (character != '[') {
            // `const `, `(`, `{ p: `: what may stand in front of a list that takes values.
            if (isPartOfName(character) || character == ' ' || character == '(' || character == '{' || character == ':' || character == ',')
                continue;
            return WrittenReads::None;
        }
        if (at > from && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']'))
            return WrittenReads::None;
        std::optional<size_t> close = findClosingBracket(text, at);
        if (!close)
            return WrittenReads::None;
        // `const [[x]] = v`: the place in front of the lists is that of both, and either may be the one. The place of `[[x]] = v` is the list itself, and that of `[x]` in `const [a, [x]] = v` lies in the outer list.
        size_t first = at + 1;
        while (first < *close && isBlank(text[first]))
            ++first;
        if (at != *within && text[first] == '[')
            return WrittenReads::None;
        // The list may end on a later line.
        to = std::max(to, std::min(WTF::find(text, static_cast<Latin1Character>('\n'), *close), text.size()));
        size_t behind = *close + 1;
        while (behind < to && text[behind] == ' ')
            ++behind;
        bool takesValues = behind < to && ((text[behind] == '=' && (behind + 1 == to || (text[behind + 1] != '=' && text[behind + 1] != '>'))) || text[behind] == ':' || text[behind] == ',' || text[behind] == ')' || text[behind] == '}' || text[behind] == ']');
        takesValues |= behind + 2 < to && text[behind] == 'o' && text[behind + 1] == 'f' && text[behind + 2] == ' ';
        if (!takesValues)
            return WrittenReads::None;
        found = text.subspan(at, *close + 1 - at);
        return WrittenReads::Same;
    }
    return WrittenReads::None;
}

// The list that opens at `within`, where the compiler says that the place fills a list.
static WrittenReads findWrittenListAt(std::span<const Latin1Character> text, size_t, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    if (!within || *within >= to || text[*within] != '[')
        return WrittenReads::None;
    std::optional<size_t> close = findClosingBracket(text, *within);
    if (!close)
        return WrittenReads::None;
    found = text.subspan(*within, *close + 1 - *within);
    return WrittenReads::Same;
}

// A keyword that a whole expression follows: `export default k in a`.
static bool isFollowedByWholeExpression(std::span<const Latin1Character> word)
{
    for (std::span<const char> keyword : { "return"_span, "default"_span, "throw"_span, "case"_span, "yield"_span, "else"_span, "do"_span }) {
        if (equalSpans(word, byteCast<Latin1Character>(keyword)))
            return true;
    }
    return false;
}

static std::optional<size_t> startOfQuotedOrPathInFrontOf(std::span<const Latin1Character> text, size_t from, size_t end)
{
    if (end <= from || !isQuote(text[end - 1]))
        return startOfPathInFrontOf(text, end);
    for (size_t opening = end - 1; opening > from; --opening) {
        if (text[opening - 1] == text[end - 1] && (opening - 1 == from || text[opening - 2] != '\\'))
            return opening - 1;
    }
    return std::nullopt;
}

// Each place in `text[from, to)` that asks whether an object has a key: `k in a.b`. What `for (k in a)` walks may be anything, and `#k in a` is not quoted.
static WrittenReads findWrittenKeyTests(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    auto isBlank = [](Latin1Character character) {
        return character == ' ' || character == '\n' || character == '\r' || character == '\t';
    };
    for (size_t at = from + 1; at + 3 < to; ++at) {
        if (std::optional<size_t> end = endOfQuotedOrPattern(text, from, at, to)) {
            at = *end;
            if (at + 3 >= to)
                break;
        }
        // `k in a`, and `"k"in a` as a minifier leaves it: `at` is the last place in front of the word.
        if (text[at + 1] != 'i' || text[at + 2] != 'n' || !isBlank(text[at + 3]) || !(text[at] == ' ' || isQuote(text[at]) || text[at] == ')' || text[at] == ']'))
            continue;
        size_t endOfKey = at + (text[at] != ' ');
        while (endOfKey > from && text[endOfKey - 1] == ' ')
            --endOfKey;
        std::optional<size_t> start = startOfQuotedOrPathInFrontOf(text, from, endOfKey);
        size_t object = at + 3;
        while (object < text.size() && isBlank(text[object]))
            ++object;
        size_t end = endOfPathFrom(text, object);

        // Only where nothing around it binds more tightly than `in` is this all of the expression.
        size_t inFront = start.value_or(from);
        while (inFront > from && text[inFront - 1] == ' ')
            --inFront;
        Latin1Character before = inFront > from ? text[inFront - 1] : Latin1Character('(');
        size_t wordInFront = inFront;
        while (wordInFront > from && isPartOfName(text[wordInFront - 1]))
            --wordInFront;
        std::span<const Latin1Character> word = text.subspan(wordInFront, inFront - wordInFront);
        bool isWalked = equalSpans(word, byteCast<Latin1Character>("const"_span)) || equalSpans(word, byteCast<Latin1Character>("let"_span)) || equalSpans(word, byteCast<Latin1Character>("var"_span));
        if (before == '(') {
            size_t keyword = inFront - (inFront > from);
            while (keyword > from && text[keyword - 1] == ' ')
                --keyword;
            isWalked |= keyword >= from + 3 && text[keyword - 3] == 'f' && text[keyword - 2] == 'o' && text[keyword - 1] == 'r' && (keyword == from + 3 || !isPartOfName(text[keyword - 4]));
        }
        if (isWalked)
            continue;
        bool beginsHere = before == '(' || before == '[' || before == '{' || before == ',' || before == ';' || before == ':' || before == '?' || before == '&' || before == '|' || before == '=' || before == '\n'
            || (before == '>' && inFront >= from + 2 && text[inFront - 2] == '=') || isFollowedByWholeExpression(word);
        size_t behind = end;
        while (behind < text.size() && text[behind] == ' ')
            ++behind;
        Latin1Character after = behind < text.size() ? text[behind] : Latin1Character(')');
        bool endsHere = after == ')' || after == ']' || after == '}' || after == ',' || after == ';' || after == ':' || after == '?' || after == '&' || after == '|' || after == '=' || after == '!' || after == '\n' || after == '\r';
        if (!start || end == object || !beginsHere || !endsHere || text[*start] == '#') {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        // The place may be the one behind the last character.
        if (within && (*within < *start || *within > end))
            continue;
        std::span<const Latin1Character> test = text.subspan(*start, end - *start);
        if (found.data() && !equalSpans(found, test))
            return WrittenReads::Differ;
        found = test;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// `await g()`: what stands in front of a path of an operand.
static size_t startWithAwait(std::span<const Latin1Character> text, size_t from, size_t start)
{
    constexpr auto keyword = "await "_span;
    if (start < from + keyword.size() || !equalSpans(text.subspan(start - keyword.size(), keyword.size()), byteCast<Latin1Character>(keyword)))
        return start;
    size_t word = start - keyword.size();
    return word > from && isPartOfName(text[word - 1]) ? start : word;
}

// Each place in `text[from, to)` that tests `g() instanceof a.b`.
static WrittenReads findWrittenInstanceTests(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    constexpr auto keyword = " instanceof "_span;
    for (size_t at = from; at + keyword.size() < to; ++at) {
        if (std::optional<size_t> end = endOfQuotedOrPattern(text, from, at, to)) {
            at = *end;
            continue;
        }
        if (!equalSpans(text.subspan(at, keyword.size()), byteCast<Latin1Character>(keyword)))
            continue;
        std::optional<size_t> operandInFront = startOfQuotedOrPathInFrontOf(text, from, at);
        size_t object = at + keyword.size();
        object = object + 6 < to && equalSpans(text.subspan(object, 6), byteCast<Latin1Character>("await "_span)) ? object + 6 : object;
        size_t end = endOfPathFrom(text, object);
        std::optional<size_t> start = operandInFront ? std::optional(startWithAwait(text, from, *operandInFront)) : std::nullopt;
        while (start && *start > from && text[*start - 1] == '!')
            --*start;

        size_t inFront = start.value_or(from);
        while (inFront > from && text[inFront - 1] == ' ')
            --inFront;
        Latin1Character before = inFront > from ? text[inFront - 1] : Latin1Character('(');
        size_t wordInFront = inFront;
        while (wordInFront > from && isPartOfName(text[wordInFront - 1]))
            --wordInFront;
        bool beginsHere = before == '(' || before == '[' || before == '{' || before == ',' || before == ';' || before == ':' || before == '?' || before == '&' || before == '|' || before == '=' || before == '\n'
            || (before == '>' && inFront >= from + 2 && text[inFront - 2] == '=') || isFollowedByWholeExpression(text.subspan(wordInFront, inFront - wordInFront));
        size_t behind = end;
        while (behind < text.size() && text[behind] == ' ')
            ++behind;
        Latin1Character after = behind < text.size() ? text[behind] : Latin1Character(')');
        bool endsHere = after == ')' || after == ']' || after == '}' || after == ',' || after == ';' || after == ':' || after == '?' || after == '&' || after == '|' || after == '=' || after == '!' || after == '\n' || after == '\r';
        if (!start || end == object || !beginsHere || !endsHere) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        if (within && (*within < *start || *within > end))
            continue;
        std::span<const Latin1Character> test = text.subspan(*start, end - *start);
        if (found.data() && !equalSpans(found, test))
            return WrittenReads::Differ;
        found = test;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// Each place in `text[from, to)` that deletes a member: `delete a.b[k]`.
static WrittenReads findWrittenDeletions(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    constexpr auto keyword = "delete "_span;
    for (size_t at = from; at + keyword.size() < to; ++at) {
        if (std::optional<size_t> end = endOfQuotedOrPattern(text, from, at, to)) {
            at = *end;
            continue;
        }
        if (!equalSpans(text.subspan(at, keyword.size()), byteCast<Latin1Character>(keyword)) || (at && (isPartOfName(text[at - 1]) || text[at - 1] == '.')))
            continue;
        size_t path = at + keyword.size();
        while (path < to && text[path] == ' ')
            ++path;
        size_t end = endOfPathFrom(text, path);
        if (end == path) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        if (within && (*within < at || *within > end))
            continue;
        std::span<const Latin1Character> deletion = text.subspan(at, end - at);
        if (found.data() && !equalSpans(found, deletion))
            return WrittenReads::Differ;
        found = deletion;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// `a.b[k](c)`, a call of an element. There is no name to go by, so only a file says which it is. Of several that hold the place the first is meant.
static WrittenReads findWrittenCallsOfElements(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    if (!within)
        return WrittenReads::None;
    for (size_t at = from + 1; at < to; ++at) {
        if (std::optional<size_t> end = endOfQuotedOrPattern(text, from, at, to)) {
            at = *end;
            continue;
        }
        if (text[at] != '[' || !(isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']' || text[at - 1] == '!'))
            continue;
        std::optional<size_t> start = startOfPathInFrontOf(text, at);
        std::optional<size_t> closeOfKey = findClosingBracket(text, at);
        if (!start || !closeOfKey)
            continue;
        size_t open = *closeOfKey + 1 + (*closeOfKey + 1 < text.size() && text[*closeOfKey + 1] == '!');
        std::optional<size_t> close = open < text.size() && text[open] == '(' ? findClosingBracket(text, open) : std::nullopt;
        if (!close)
            continue;
        start = startWithNew(text, *start, at);
        // `new a[k]()`
        if (*start >= 4 && text[*start - 4] == 'n' && text[*start - 3] == 'e' && text[*start - 2] == 'w' && text[*start - 1] == ' ' && (*start == 4 || !isPartOfName(text[*start - 5])))
            *start -= 4;
        if (*within < *start || *within > *close)
            continue;
        found = text.subspan(*start, *close + 1 - *start);
        return WrittenReads::Same;
    }
    return WrittenReads::None;
}

// `within`: only what holds that place counts.
using FindInText = WrittenReads (*)(std::span<const Latin1Character>, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found);

// A run of `bun test` or `bun run` has the files of the program beside it, at the paths that a stack trace prints. An executable has not, and a module of Bun's is no file.
static size_t startOfNextLine(std::span<const Latin1Character> text, size_t from)
{
    size_t newline = WTF::find(text, static_cast<Latin1Character>('\n'), from);
    return newline == notFound ? text.size() : newline + 1;
}

struct SourceFile {
    // The number of the image and in it that of the module: a VM may run several images.
    uint64_t module { 0 };
    Vector<uint8_t> bytes;
    Vector<size_t> lineStarts;
    Vector<TextPrintedInPlace> textPrintedInPlace;

    // A column is found by following its line. A long line is followed once: these are places in it, a stretch apart.
    struct PlaceInLine {
        size_t offset;
        unsigned column;
    };
    size_t lineOfPlaces { notFound };
    Vector<PlaceInLine> placesInLine;

    // What is quoted is an expression, and no expression holds a `;` or a brace of the block it stands in. A long line is passed over once:
    // these are its `;`, `{` and `}` outside quoted text, each with the number of braces that are open behind it.
    struct MarkInLine {
        enum Kind : uint8_t { EndOfStatement, Open, Close };
        size_t offset;
        int32_t openBraces;
        Kind kind;
    };
    size_t lineOfMarks { notFound };
    Vector<MarkInLine> marksInLine;
};

// Kept with the VM and not in storage of the thread: every thread of the process would have room for that, whether or not it ever runs a program.
struct QuotesOfPlaces {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(QuotesOfPlaces);

    struct Answer {
        FindInText find { nullptr };
        String name;
        String quote;
        bool holdsForEveryName { false };
    };

    SourceFile lastFile;
    // An executable has no file for any of its modules: each is asked for once.
    UncheckedKeyHashSet<uint64_t> modulesWithoutFile;
    UncheckedKeyHashMap<const void*, Answer> answers;

    // A place is asked about in several ways, one after the other. Held only while it is.
    const void* placeInKeptTextIsOf { nullptr };
    std::optional<PlaceInKeptText> placeInKeptText;
    // Of the piece of kept text that was asked about last. A long statement or function is one piece: what is printed in place of its parts is read once, not once for each place in it.
    uint64_t moduleOfTextPrintedInPlace { 0 };
    std::pair<size_t, size_t> pieceOfTextPrintedInPlace;
    Vector<TextPrintedInPlace> textPrintedInPlace;
};

static QuotesOfPlaces& quotesOfPlaces(VM& vm)
{
    auto& quotes = vm.aotState().quotesOfPlaces;
    if (!quotes) {
        quotes = { new QuotesOfPlaces, [](QuotesOfPlaces* quotes) {
            delete quotes;
        } };
    }
    return *quotes;
}

// Never 0, which no file has and a set cannot hold.
static uint64_t numberAmongAllImages(ModuleID moduleOfImage)
{
    return (static_cast<uint64_t>(Image::current()->number()) + 1) << 32 | moduleOfImage.index();
}

static SourceFile* sourceFileOf(VM& vm, ModuleID moduleOfImage)
{
    QuotesOfPlaces& quotes = quotesOfPlaces(vm);
    SourceFile& lastFile = quotes.lastFile;
    uint64_t module = numberAmongAllImages(moduleOfImage);
    if (lastFile.module == module)
        return &lastFile;
    if (quotes.modulesWithoutFile.contains(module))
        return nullptr;
    String path = sourceURLOf(moduleOfImage);
    bool isAbsolute = path.startsWith('/') || (path.length() > 2 && isASCIIAlpha(path[0]) && path[1] == ':');
    std::optional<Vector<uint8_t>> bytes = isAbsolute ? FileSystem::readEntireFile(path) : std::nullopt;
    if (!bytes) {
        quotes.modulesWithoutFile.add(module);
        return nullptr;
    }
    lastFile = { module, WTF::move(*bytes), { }, textPrintedInPlaceOf(moduleOfImage, StartOfPrintedText::File), notFound, { } };
    countTextPrintedInPlaceDecoded(vm, lastFile.textPrintedInPlace.size());
    for (size_t start = 0; start < lastFile.bytes.size(); start = startOfNextLine(byteCast<Latin1Character>(lastFile.bytes.span()), start))
        lastFile.lineStarts.append(start);
    return &lastFile;
}

// Where the type behind `as` ends: at what closes the brackets around the cast, or at the comma behind it.
static size_t endOfType(std::span<const Latin1Character> text, size_t at)
{
    unsigned depth = 0;
    for (; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            for (++at; at < text.size() && text[at] != character; ++at)
                at += text[at] == '\\';
        } else if (character == '(' || character == '[' || character == '{' || character == '<')
            ++depth;
        else if (character == '>' && text[at - 1] == '=')
            continue;
        else if (character == ')' || character == ']' || character == '}' || character == '>') {
            if (!depth--)
                return at;
        } else if ((character == ',' || character == ';') && !depth)
            return at;
    }
    return at;
}

// `(a as T)!.b(c!)` is `(a).b(c)` once the types are gone. The text that the image keeps has none.
static Vector<Latin1Character> withoutTypes(std::span<const Latin1Character> text)
{
    auto startsWith = [&](size_t at, ASCIILiteral word) {
        return text.size() - at >= word.length() && equalSpans(text.subspan(at, word.length()), byteCast<Latin1Character>(word.span()));
    };
    Vector<Latin1Character> kept;
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        // `1 /* Green */ as T`: the comment is printed behind the value of a member of an enum.
        bool isBehindComment = at > 1 && text[at - 2] == '*' && text[at - 1] == '/';
        bool isBehindValue = at && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']' || isQuote(text[at - 1]) || isBehindComment);
        if (isQuote(character)) {
            size_t end = at + 1;
            while (end < text.size() && text[end] != character)
                end += 1 + (text[end] == '\\');
            end = std::min(end, text.size() - 1);
            kept.append(text.subspan(at, end - at));
            at = end;
        } else if (character == '!' && isBehindValue && !isQuote(text[at - 1]) && (at + 1 == text.size() || text[at + 1] != '='))
            continue;
        else if (character == ' ' && isBehindValue && (startsWith(at, " as "_s) || startsWith(at, " satisfies "_s))) {
            size_t end = endOfType(text, at);
            // `{ k: a as T }` is `{ k: a }`.
            while (end < text.size() && end > at + 1 && text[end - 1] == ' ')
                --end;
            at = end - 1;
            continue;
        }
        kept.append(text[at]);
    }
    return kept;
}

// Whether `text[open, close]` are brackets around a value: not those of a call, of parameters, or of `if` and its like.
static bool areAroundValue(std::span<const Latin1Character> text, size_t open, size_t close)
{
    size_t behind = close + 1;
    while (behind < text.size() && text[behind] == ' ')
        ++behind;
    if (behind < text.size() && (text[behind] == '{' || (text[behind] == '=' && behind + 1 < text.size() && text[behind + 1] == '>')))
        return false;
    size_t end = open;
    while (end && text[end - 1] == ' ')
        --end;
    if (!end)
        return true;
    // `f?.(a)`
    if (text[end - 1] == ')' || text[end - 1] == ']' || text[end - 1] == '.')
        return false;
    if (!isPartOfName(text[end - 1]))
        return true;
    if (end == open)
        return false;
    size_t start = end;
    while (start && isPartOfName(text[start - 1]))
        --start;
    return isWordInFrontOfValue(text.subspan(start, end - start));
}

static bool isObjectLiteral(std::span<const Latin1Character> text)
{
    if (text.empty() || text.front() != '{')
        return false;
    unsigned depth = 0;
    for (size_t at = 0; at < text.size(); ++at) {
        if (isQuote(text[at]))
            at = skipQuoted(text, at, text.size());
        else if (text[at] == '{')
            ++depth;
        else if (text[at] == '}' && !--depth)
            return at + 1 == text.size();
    }
    return false;
}

// `(a).b` is printed `a.b`, and `(a + b).c` as it is: Bun prints round brackets only where the meaning hangs on them.
// isTakenAtEnd: the text stood in brackets that are not printed, and what stands behind them reads from it or calls it.
static void appendWithoutIdleBrackets(Vector<Latin1Character>& printed, std::span<const Latin1Character> text, bool isTakenAtEnd = false)
{
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            size_t end = at + 1;
            while (end < text.size() && text[end] != character)
                end += 1 + (text[end] == '\\');
            if (character == '`')
                end = findEndOfTemplate(text, at).value_or(end);
            end = std::min(end, text.size() - 1);
            // Bun prints a text in double quotes where that leaves nothing to escape.
            // The last character may be a quote that nothing closes.
            std::span<const Latin1Character> inside = text.subspan(at + 1, end > at ? end - at - 1 : 0);
            bool hasNothingToEscape = end > at && text[end] == character && std::ranges::none_of(inside, [](Latin1Character character) {
                return isQuote(character) || character == '\\' || character == '$' || character == '\n';
            });
            // Behind a tag a template stays one.
            bool hasTag = character == '`' && at && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']');
            if (hasNothingToEscape && !hasTag) {
                printed.append('"');
                printed.append(inside);
                printed.append('"');
                at = end;
                continue;
            }
            printed.append(text.subspan(at, end - at));
            at = end;
        } else if (character == '[' && at && at + 3 < text.size() && isQuote(text[at + 1]) && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']')) {
            // `a["x"]` is printed `a.x`.
            size_t end = at + 2;
            while (end < text.size() && isPartOfName(text[end]) && isASCII(text[end]))
                ++end;
            if (end > at + 2 && !isASCIIDigit(text[at + 2]) && end + 1 < text.size() && text[end] == text[at + 1] && text[end + 1] == ']') {
                printed.append('.');
                printed.append(text.subspan(at + 2, end - at - 2));
                at = end + 1;
                continue;
            }
        } else if (std::optional<size_t> close = character == '(' ? findClosingBracket(text, at) : std::nullopt; close && areAroundValue(text, at, *close)) {
            std::span<const Latin1Character> inside = text.subspan(at + 1, *close - at - 1);
            while (!inside.empty() && inside.front() == ' ')
                inside = inside.subspan(1);
            while (!inside.empty() && inside.back() == ' ')
                inside = inside.first(inside.size() - 1);
            bool isConstructed = at >= 4 && text[at - 4] == 'n' && text[at - 3] == 'e' && text[at - 2] == 'w' && text[at - 1] == ' ';
            size_t behindBracket = *close + 1;
            while (behindBracket < text.size() && text[behindBracket] == ' ')
                ++behindBracket;
            auto standsBehindBracket = [&](ASCIILiteral token) {
                return spanHasPrefix(text.subspan(behindBracket), byteCast<Latin1Character>(token.span()));
            };
            bool isTaken = behindBracket == text.size() ? isTakenAtEnd : standsBehindBracket("."_s) || standsBehindBracket("["_s) || standsBehindBracket("("_s) || standsBehindBracket("`"_s) || standsBehindBracket("?."_s) || standsBehindBracket("**"_s);
            // `(true && b).c`, where `(d ? e : f)` is printed in place of all between the brackets.
            if (!inside.empty() && inside.front() == '(' && findClosingBracket(inside, 0) == std::optional<size_t>(inside.size() - 1)) {
                appendWithoutIdleBrackets(printed, inside, isTaken);
                at = *close;
                continue;
            }
            bool isPath = !inside.empty() && !isASCIIDigit(inside.front()) && startOfPathInFrontOf(inside, inside.size()) == std::optional<size_t>(0);
            // `(a?.b).c` stops elsewhere than `a?.b.c`, and `new (f()).K()` constructs something else than `new f().K()`.
            if (isPath && WTF::find(inside, static_cast<Latin1Character>('?')) == notFound && !(isConstructed && WTF::find(inside, static_cast<Latin1Character>('(')) != notFound)) {
                appendWithoutIdleBrackets(printed, inside, isTaken);
                at = *close;
                continue;
            }
            // `"a" in (await o)` is printed `"a" in await o`: the word binds its path more tightly than whatever takes two operands. `(await o).b`, `(await o)()` and `(await o) ** 2` stay as they are.
            bool isWordInFrontOfPath = equalSpans(inside, byteCast<Latin1Character>("void 0"_span)) || std::ranges::any_of(std::array { "await "_s, "typeof "_s, "void "_s, "delete "_s }, [&](ASCIILiteral word) {
                return inside.size() > word.length() && spanHasPrefix(inside, byteCast<Latin1Character>(word.span())) && !isASCIIDigit(inside[word.length()])
                    && startOfPathInFrontOf(inside, inside.size()) == std::optional<size_t>(word.length());
            });
            if (isWordInFrontOfPath && !isTaken && !isConstructed && WTF::find(inside, static_cast<Latin1Character>('?')) == notFound) {
                appendWithoutIdleBrackets(printed, inside);
                at = *close;
                continue;
            }
            // `({}) instanceof a` is quoted from where the object starts.
            if (isObjectLiteral(inside) && standsBehindBracket("instanceof "_s)) {
                printed.append(inside);
                at = *close;
                continue;
            }
            // `(Color.Green as any).x` is printed `1 /* Green */.x`.
            if (inside.size() > 4 && (isASCIIDigit(inside.front()) || isQuote(inside.front())) && spanHasSuffix(inside, byteCast<Latin1Character>(" */"_span))) {
                printed.append(inside);
                at = *close;
                continue;
            }
            // `(new K()).b` and `(new K).b` are printed `new K().b`.
            bool constructs = inside.size() > 4 && inside[0] == 'n' && inside[1] == 'e' && inside[2] == 'w' && inside[3] == ' ' && startOfPathInFrontOf(inside, inside.size()) == std::optional<size_t>(4);
            bool isReadFrom = *close + 1 < text.size() && (text[*close + 1] == '.' || text[*close + 1] == '[');
            if (constructs && isReadFrom && !isConstructed && WTF::find(inside, static_cast<Latin1Character>('?')) == notFound) {
                appendWithoutIdleBrackets(printed, inside);
                if (inside.back() != ')')
                    printed.append("()"_span);
                at = *close;
                continue;
            }
        }
        printed.append(text[at]);
    }
}

// `a.b` and `f()`, which a `[` in the next line reads an element of. Not `k in`.
static bool endsInValue(std::span<const Latin1Character> text)
{
    if (text.empty())
        return false;
    if (text.back() == ')' || text.back() == ']')
        return true;
    size_t start = text.size();
    while (start && isPartOfName(text[start - 1]))
        --start;
    return start != text.size() && !isWordInFrontOfValue(text.subspan(start));
}

// A chain and the arguments of a call are printed on one line, without the comma behind the last. What stands in curly brackets keeps its lines.
static Vector<Latin1Character> onOneLine(std::span<const Latin1Character> text)
{
    Vector<Latin1Character> kept;
    Vector<Latin1Character, 8> open;
    auto isBlank = [](Latin1Character character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            size_t end = at + 1;
            while (end < text.size() && text[end] != character)
                end += 1 + (text[end] == '\\');
            end = std::min(end, text.size() - 1);
            kept.append(text.subspan(at, end - at));
            at = end;
        } else if (character == '(' || character == '[' || character == '{')
            open.append(character);
        else if ((character == ')' || character == ']' || character == '}') && !open.isEmpty())
            open.removeLast();
        else if (character == '\n' || (character == '/' && at + 1 < text.size() && text[at + 1] == '/')) {
            size_t lineBreak = at;
            size_t next = at;
            while (next < text.size() && (isBlank(text[next]) || (text[next] == '/' && next + 1 < text.size() && text[next + 1] == '/'))) {
                if (text[next] == '/')
                    next = std::min(WTF::find(text, static_cast<Latin1Character>('\n'), next), text.size());
                else {
                    if (text[next] == '\n')
                        lineBreak = next;
                    ++next;
                }
            }
            while (!kept.isEmpty() && isBlank(kept.last()))
                kept.removeLast();
            Latin1Character behind = next < text.size() ? text[next] : Latin1Character(0);
            bool closes = behind == ')' || behind == ']' || behind == '}';
            if (closes && !kept.isEmpty() && kept.last() == ',')
                kept.removeLast();
            Latin1Character inFront = kept.isEmpty() ? Latin1Character(0) : kept.last();
            if (!open.isEmpty() && open.last() == '{')
                kept.append(text.subspan(lineBreak, next - lineBreak));
            else if (behind != '.' && !(behind == '?' && next + 1 < text.size() && text[next + 1] == '.') && !closes && inFront != '(' && inFront != '[' && inFront != '.' && !(behind == '[' && endsInValue(kept.span())))
                kept.append(' ');
            at = next - 1;
            continue;
        }
        kept.append(text[at]);
    }
    return kept;
}

// `true` is printed `!0`, `false` `!1` and `undefined` `void 0`.
static Vector<Latin1Character> withShortLiterals(std::span<const Latin1Character> text)
{
    Vector<Latin1Character> printed;
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            size_t end = at + 1;
            while (end < text.size() && text[end] != character)
                end += 1 + (text[end] == '\\');
            end = std::min(end, text.size() - 1);
            printed.append(text.subspan(at, end - at));
            at = end;
        } else if (isPartOfName(character) && (!at || (!isPartOfName(text[at - 1]) && text[at - 1] != '.' && text[at - 1] != '#'))) {
            size_t end = at;
            while (end < text.size() && isPartOfName(text[end]))
                ++end;
            std::span<const Latin1Character> word = text.subspan(at, end - at);
            // `{ true: 1 }` names a property.
            size_t behind = end;
            while (behind < text.size() && text[behind] == ' ')
                ++behind;
            size_t inFront = at;
            while (inFront && text[inFront - 1] == ' ')
                --inFront;
            bool isKey = behind < text.size() && text[behind] == ':' && inFront && (text[inFront - 1] == '{' || text[inFront - 1] == ',');
            auto is = [&](ASCIILiteral literal) {
                return !isKey && equalSpans(word, byteCast<Latin1Character>(literal.span()));
            };
            // What is read from or called stands in brackets: `undefined[0]` is printed `(void 0)[0]`.
            bool isTaken = end < text.size() && (text[end] == '.' || text[end] == '[' || text[end] == '(' || text[end] == '`' || (text[end] == '?' && end + 1 < text.size() && text[end + 1] == '.'));
            auto append = [&](ASCIILiteral literal) {
                if (isTaken)
                    printed.append('(');
                printed.append(byteCast<Latin1Character>(literal.span()));
                if (isTaken)
                    printed.append(')');
            };
            if (is("true"_s))
                append("!0"_s);
            else if (is("false"_s))
                append("!1"_s);
            else if (is("undefined"_s))
                append("void 0"_s);
            else
                printed.append(word);
            at = end - 1;
            continue;
        }
        printed.append(text[at]);
    }
    return printed;
}

static void appendEscape(Vector<Latin1Character>& printed, ASCIILiteral head, unsigned digits, uint32_t number, bool isLowercase = false)
{
    printed.append(byteCast<Latin1Character>(head.span()));
    for (unsigned digit = digits; digit--;) {
        uint8_t nibble = number >> (digit * 4) & 0xF;
        printed.append(isLowercase ? lowerNibbleToLowercaseASCIIHexDigit(nibble) : lowerNibbleToASCIIHexDigit(nibble));
    }
}

// The character of UTF-8 at `at`, which moves behind it. Nothing where there is none.
static std::optional<char32_t> readCharacter(std::span<const Latin1Character> text, size_t& at)
{
    int32_t character;
    U8_NEXT(text.data(), at, text.size(), character);
    return character < 0 ? std::nullopt : std::optional<char32_t>(character);
}

static void appendUnits(Vector<char16_t>& units, char32_t character)
{
    if (U_IS_BMP(character))
        return units.append(static_cast<char16_t>(character));
    units.append(U16_LEAD(character));
    units.append(U16_TRAIL(character));
}

// What the text between two quotes stands for. False where an escape is not one of the language.
static bool appendValueOfWrittenText(Vector<char16_t>& value, std::span<const Latin1Character> written)
{
    auto readHex = [&](size_t& at, size_t digits) -> std::optional<uint32_t> {
        if (written.size() - at < digits)
            return std::nullopt;
        uint32_t number = 0;
        for (; digits; --digits, ++at) {
            if (!isASCIIHexDigit(written[at]))
                return std::nullopt;
            number = number << 4 | toASCIIHexValue(written[at]);
        }
        return number;
    };
    for (size_t at = 0; at < written.size();) {
        if (written[at] != '\\') {
            std::optional<char32_t> character = readCharacter(written, at);
            if (!character)
                return false;
            // A line of a template ends with a line feed, however the file ends its lines.
            if (*character == '\r') {
                at += at < written.size() && written[at] == '\n';
                character = '\n';
            }
            appendUnits(value, *character);
            continue;
        }
        if (++at == written.size())
            return false;
        std::optional<char32_t> escaped = readCharacter(written, at);
        if (!escaped)
            return false;
        switch (*escaped) {
        case 'b':
            value.append('\b');
            break;
        case 'f':
            value.append('\f');
            break;
        case 'n':
            value.append('\n');
            break;
        case 'r':
            value.append('\r');
            break;
        case 't':
            value.append('\t');
            break;
        case 'v':
            value.append('\v');
            break;
        case 'x': {
            std::optional<uint32_t> number = readHex(at, 2);
            if (!number)
                return false;
            value.append(static_cast<char16_t>(*number));
            break;
        }
        case 'u': {
            if (at == written.size() || written[at] != '{') {
                std::optional<uint32_t> number = readHex(at, 4);
                if (!number)
                    return false;
                value.append(static_cast<char16_t>(*number));
                break;
            }
            size_t end = WTF::find(written, static_cast<Latin1Character>('}'), ++at);
            if (end == notFound || end == at || end - at > 8)
                return false;
            std::optional<uint32_t> number = readHex(at, end - at);
            if (!number || *number > UCHAR_MAX_VALUE)
                return false;
            appendUnits(value, *number);
            ++at;
            break;
        }
        // The text goes on in the next line.
        case '\r':
            at += at < written.size() && written[at] == '\n';
            break;
        case '\n':
        case 0x2028:
        case 0x2029:
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            uint32_t number = *escaped - '0';
            for (unsigned more = number < 4 ? 2 : 1; more && at < written.size() && isASCIIOctalDigit(written[at]); --more)
                number = number << 3 | (written[at++] - '0');
            value.append(static_cast<char16_t>(number));
            break;
        }
        default:
            appendUnits(value, *escaped);
        }
    }
    return true;
}

// best_quote_char_for_string() of Bun's printer.
static Latin1Character bestQuoteFor(std::span<const char16_t> value, bool allowsBacktick)
{
    size_t singleCost = 0;
    size_t doubleCost = 0;
    size_t backtickCost = 0;
    for (size_t at = 0; at < std::min<size_t>(value.size(), 1024); ++at) {
        switch (value[at]) {
        case '\'':
            ++singleCost;
            break;
        case '"':
            ++doubleCost;
            break;
        case '`':
            ++backtickCost;
            break;
        case '\n':
            ++singleCost;
            ++doubleCost;
            break;
        case '\\':
            ++at;
            break;
        case '$':
            backtickCost += at + 1 < value.size() && value[at + 1] == '{';
            break;
        default:
            break;
        }
    }
    if (allowsBacktick && backtickCost < std::min(singleCost, doubleCost))
        return '`';
    return singleCost < doubleCost ? '\'' : '"';
}

// write_pre_quoted_string() of Bun's printer, which prints nothing but ASCII for Bun to run.
static void appendTextBetweenQuotes(Vector<Latin1Character>& printed, std::span<const char16_t> value, Latin1Character quote)
{
    for (size_t at = 0; at < value.size(); ++at) {
        char16_t unit = value[at];
        switch (unit) {
        case '\b':
            printed.append("\\b"_span);
            break;
        case '\f':
            printed.append("\\f"_span);
            break;
        case '\r':
            printed.append("\\r"_span);
            break;
        case '\v':
            printed.append("\\v"_span);
            break;
        case '\\':
            printed.append("\\\\"_span);
            break;
        case '\n':
            if (quote == '`')
                printed.append('\n');
            else
                printed.append("\\n"_span);
            break;
        case '\t':
            if (quote == '`')
                printed.append('\t');
            else
                printed.append("\\t"_span);
            break;
        case '"':
        case '\'':
        case '`':
            if (unit == quote)
                printed.append('\\');
            printed.append(static_cast<Latin1Character>(unit));
            break;
        case '$':
            if (quote == '`' && at + 1 < value.size() && value[at + 1] == '{')
                printed.append('\\');
            printed.append('$');
            break;
        default:
            if (unit >= 0x20 && unit <= 0x7E)
                printed.append(static_cast<Latin1Character>(unit));
            else if (unit <= 0xFF)
                appendEscape(printed, "\\x"_s, 2, unit);
            else
                appendEscape(printed, "\\u"_s, 4, unit);
        }
    }
}

// `'it\'s'` is printed `"it's"`, a text with a line break in it between backticks, and what is not ASCII as an escape. `written` has its quotes.
static bool appendTextAsPrinted(Vector<Latin1Character>& printed, std::span<const Latin1Character> written, bool allowsBacktick)
{
    Vector<char16_t> value;
    if (written.size() < 2 || written.back() != written.front() || !appendValueOfWrittenText(value, written.subspan(1, written.size() - 2)))
        return false;
    Latin1Character quote = bestQuoteFor(value.span(), allowsBacktick);
    printed.append(quote);
    appendTextBetweenQuotes(printed, value.span(), quote);
    printed.append(quote);
    return true;
}

enum class WrittenAs : uint8_t { Name, RegularExpression, TemplateBehindTag };

// For what is not ASCII a name has `\u{5b57}`, and a regular expression and a template behind a tag have `\u5B57`. Above that the last has `\u{1f600}`, and the second two escapes.
static void appendWithEscapes(Vector<Latin1Character>& printed, std::span<const Latin1Character> written, WrittenAs kind)
{
    for (size_t at = 0; at < written.size();) {
        if (isASCII(written[at])) {
            printed.append(written[at++]);
            continue;
        }
        size_t start = at;
        std::optional<char32_t> character = readCharacter(written, at);
        if (!character) {
            printed.append(written.subspan(start, at - start));
            continue;
        }
        if (kind == WrittenAs::Name || (kind == WrittenAs::TemplateBehindTag && !U_IS_BMP(*character))) {
            unsigned digits = 1;
            while (*character >> (digits * 4))
                ++digits;
            appendEscape(printed, "\\u{"_s, digits, *character, true);
            printed.append('}');
            continue;
        }
        Vector<char16_t, 2> units;
        if (U_IS_BMP(*character))
            units.append(static_cast<char16_t>(*character));
        else
            units.appendList({ U16_LEAD(*character), U16_TRAIL(*character) });
        for (char16_t unit : units)
            appendEscape(printed, "\\u"_s, 4, unit);
    }
}

// A token of a quote, for PrintedBlanks.
struct QuotedToken {
    enum class Role : uint8_t { Operand, Keyword, Binary, Prefix, Postfix, Open, Close, Comma, Dot, Spread, KeyColon, Semicolon };

    bool endsOperand() const { return role == Role::Operand || role == Role::Close || role == Role::Postfix; }

    size_t start;
    size_t end;
    Role role;
    bool hasLineBreakInFront { false };
    bool holdsItems { false };
    bool hasLineBreakAtItems { false };
    unsigned marksOfIndentation { 0 };
    size_t partner { notFound };
};

// `text` with the blanks and the line breaks of Bun's printer, without comments and without the comma behind the last item.
// A list or an object is printed over several lines where a line breaks at one of its brackets or commas. How far those lines are indented is what the compiler says: markOfIndentation.
class PrintedBlanks {
public:
    static std::optional<Vector<Latin1Character>> print(std::span<const Latin1Character> text)
    {
        PrintedBlanks blanks(text);
        blanks.readTokens();
        return blanks.print();
    }

private:
    using Role = QuotedToken::Role;

    explicit PrintedBlanks(std::span<const Latin1Character> text)
        : m_text(text)
    {
    }

    bool is(const QuotedToken& token, ASCIILiteral spelled) const
    {
        // Marks may stand behind a bracket.
        return token.end - token.start >= spelled.length() && equalSpans(m_text.subspan(token.start, spelled.length()), byteCast<Latin1Character>(spelled.span()))
            && (token.end - token.start == spelled.length() || m_text[token.start + spelled.length()] <= markOfIndentation);
    }

    bool isAnyOf(const QuotedToken& token, std::initializer_list<ASCIILiteral> words) const
    {
        return std::ranges::any_of(words, [&](ASCIILiteral word) {
            return is(token, word);
        });
    }

    bool startsWith(size_t at, ASCIILiteral spelled) const
    {
        return m_text.size() - at >= spelled.length() && equalSpans(m_text.subspan(at, spelled.length()), byteCast<Latin1Character>(spelled.span()));
    }

    static bool isBlank(Latin1Character character)
    {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    }

    Latin1Character firstBehindBlanks(size_t at) const
    {
        while (at < m_text.size() && isBlank(m_text[at]))
            ++at;
        return at < m_text.size() ? m_text[at] : Latin1Character(0);
    }

    size_t endOfComment(size_t at) const
    {
        if (m_text[at + 1] == '/')
            return std::min(WTF::find(m_text, static_cast<Latin1Character>('\n'), at), m_text.size());
        for (at += 2; at + 1 < m_text.size(); ++at) {
            if (m_text[at] == '*' && m_text[at + 1] == '/')
                return at + 2;
        }
        return m_text.size();
    }

    bool startsComment(size_t at) const
    {
        return m_text[at] == '/' && at + 1 < m_text.size() && (m_text[at + 1] == '/' || m_text[at + 1] == '*');
    }

    // `1 /* Green */`: see markOfPrintedKey.
    bool isPrintedComment(size_t at) const
    {
        return startsComment(at) && at + 2 < m_text.size() && m_text[at + 2] == markOfPrintedKey;
    }

    size_t endOfQuoted(size_t at) const
    {
        if (m_text[at] != '`')
            return std::min(skipQuoted(m_text, at, m_text.size()) + 1, m_text.size());
        for (++at; at < m_text.size() && m_text[at] != '`'; ++at) {
            if (m_text[at] == '\\')
                ++at;
            else if (startsWith(at, "${"_s))
                at = endOfBody(at + 1) - 1;
        }
        return std::min(at + 1, m_text.size());
    }

    size_t endOfRegularExpression(size_t at) const
    {
        bool isInClass = false;
        for (++at; at < m_text.size() && m_text[at] != '\n'; ++at) {
            Latin1Character character = m_text[at];
            if (character == '\\')
                ++at;
            else if (character == '[')
                isInClass = true;
            else if (character == ']')
                isInClass = false;
            else if (character == '/' && !isInClass) {
                for (++at; at < m_text.size() && isPartOfName(m_text[at]); ++at) { }
                return at;
            }
        }
        return std::min(at, m_text.size());
    }

    // The body of a function or of a class stays as it is written.
    size_t endOfBody(size_t at) const
    {
        unsigned depth = 0;
        for (; at < m_text.size(); ++at) {
            Latin1Character character = m_text[at];
            if (isQuote(character))
                at = endOfQuoted(at) - 1;
            else if (startsComment(at))
                at = endOfComment(at) - 1;
            else if (character == '{')
                ++depth;
            else if (character == '}' && !--depth)
                return at + 1;
        }
        return m_text.size();
    }

    size_t endOfWord(size_t at) const
    {
        size_t start = at;
        for (++at; at < m_text.size() && isPartOfName(m_text[at]); ++at) { }
        // `1e-7`
        bool hasExponent = isASCIIDigit(m_text[start]) && !startsWith(start, "0x"_s) && (m_text[at - 1] == 'e' || m_text[at - 1] == 'E')
            && at + 1 < m_text.size() && (m_text[at] == '-' || m_text[at] == '+') && isASCIIDigit(m_text[at + 1]);
        if (hasExponent) {
            for (++at; at < m_text.size() && isASCIIDigit(m_text[at]); ++at) { }
        }
        return at;
    }

    size_t endOfPunctuator(size_t at) const
    {
        for (ASCIILiteral punctuator : { ">>>="_s, "..."_s, "==="_s, "!=="_s, "**="_s, "<<="_s, ">>="_s, ">>>"_s, "&&="_s, "||="_s, "??="_s, "=>"_s, "=="_s, "!="_s, "<="_s, ">="_s, "&&"_s, "||"_s, "??"_s, "?."_s, "++"_s, "--"_s, "+="_s, "-="_s, "*="_s, "/="_s, "%="_s, "&="_s, "|="_s, "^="_s, "<<"_s, ">>"_s, "**"_s }) {
            if (startsWith(at, punctuator))
                return at + punctuator.length();
        }
        return at + 1;
    }

    Role roleOfWord(const QuotedToken& word, const QuotedToken* last) const
    {
        if (!isAnyOf(word, { "const"_s, "let"_s, "var"_s, "typeof"_s, "void"_s, "delete"_s, "new"_s, "in"_s, "of"_s, "instanceof"_s, "await"_s, "yield"_s, "return"_s, "throw"_s, "function"_s, "class"_s, "extends"_s, "async"_s, "static"_s, "get"_s, "set"_s }))
            return Role::Operand;
        // `o.of`, `{ of: 1 }`, `[of]`, `of = 1`, `new.target`
        Latin1Character next = firstBehindBlanks(word.end);
        if ((last && last->role == Role::Dot) || !next || next == ':' || next == ',' || next == '}' || next == ']' || next == ')' || next == '=' || next == '.' || next == ';' || next == '?')
            return Role::Operand;
        if (isAnyOf(word, { "in"_s, "of"_s, "instanceof"_s }))
            return last && last->endsOperand() ? Role::Keyword : Role::Operand;
        // `{ get() { } }`
        if (isAnyOf(word, { "get"_s, "set"_s, "static"_s }) && next == '(')
            return Role::Operand;
        return Role::Keyword;
    }

    Role roleOfPunctuator(const QuotedToken& punctuator, const QuotedToken* last)
    {
        bool isBehindOperand = last && last->endsOperand();
        if (is(punctuator, ","_s))
            return Role::Comma;
        if (is(punctuator, ";"_s))
            return Role::Semicolon;
        if (is(punctuator, "."_s) || is(punctuator, "?."_s))
            return Role::Dot;
        if (is(punctuator, "..."_s))
            return Role::Spread;
        if (is(punctuator, "~"_s) || is(punctuator, "@"_s))
            return Role::Prefix;
        // `a!.b` has a type.
        if (is(punctuator, "!"_s))
            return isBehindOperand ? Role::Postfix : Role::Prefix;
        if (is(punctuator, "+"_s) || is(punctuator, "-"_s))
            return isBehindOperand ? Role::Binary : Role::Prefix;
        if (is(punctuator, "++"_s) || is(punctuator, "--"_s))
            return isBehindOperand && !punctuator.hasLineBreakInFront ? Role::Postfix : Role::Prefix;
        // `function* f`
        if (is(punctuator, "*"_s) && last && last->role == Role::Keyword)
            return Role::Postfix;
        if (is(punctuator, "?"_s)) {
            ++m_openConditions.last();
            return Role::Binary;
        }
        if (is(punctuator, ":"_s)) {
            if (!m_openConditions.last())
                return Role::KeyColon;
            --m_openConditions.last();
        }
        return Role::Binary;
    }

    bool opensBody(const QuotedToken* last) const
    {
        return last && (last->endsOperand() || is(*last, "=>"_s) || (last->role == Role::Keyword && isAnyOf(*last, { "class"_s, "static"_s })));
    }

    void readTokens()
    {
        bool hasLineBreakInFront = false;
        Vector<size_t, 8> open;
        m_openConditions.append(0);
        for (size_t at = 0; at < m_text.size();) {
            Latin1Character character = m_text[at];
            if (isBlank(character)) {
                hasLineBreakInFront |= character == '\n';
                ++at;
                continue;
            }
            if (startsComment(at)) {
                size_t end = endOfComment(at);
                if (isPrintedComment(at)) {
                    m_tokens.append({ at, end, Role::Postfix, hasLineBreakInFront });
                    hasLineBreakInFront = false;
                } else
                    hasLineBreakInFront |= WTF::find(m_text.subspan(at, end - at), static_cast<Latin1Character>('\n')) != notFound;
                at = end;
                continue;
            }
            if (character <= markOfIndentation) {
                ++at;
                continue;
            }
            const QuotedToken* last = m_tokens.isEmpty() ? nullptr : &m_tokens.last();
            QuotedToken token { at, at + 1, Role::Operand, hasLineBreakInFront };
            hasLineBreakInFront = false;
            std::optional<size_t> around = open.isEmpty() ? std::nullopt : std::optional<size_t>(open.last());
            bool standsAtItems = last && (last->role == Role::Comma || (around && last == &m_tokens[*around]));
            if (character == markOfTextFromPrinter) {
                // A function or a class: see withoutTextFromPrinter().
            } else if (isPartOfName(character) || character == '#') {
                token.end = endOfWord(at);
                token.role = roleOfWord(token, last);
            } else if (isQuote(character))
                token.end = endOfQuoted(at);
            else if (character == '/' && !(last && last->endsOperand()))
                token.end = endOfRegularExpression(at);
            else if (character == '{' && opensBody(last))
                token.end = endOfBody(at);
            else if (character == '(' || character == '[' || character == '{') {
                token.role = Role::Open;
                token.holdsItems = character == '{' || (character == '[' && !(last && last->endsOperand()));
                for (; token.end < m_text.size() && m_text[token.end] <= markOfIndentation; ++token.end)
                    token.marksOfIndentation += m_text[token.end] == markOfIndentation;
                open.append(m_tokens.size());
                m_openConditions.append(0);
            } else if (character == ')' || character == ']' || character == '}') {
                token.role = Role::Close;
                standsAtItems = true;
                if (around) {
                    token.partner = open.takeLast();
                    m_tokens[token.partner].partner = m_tokens.size();
                    m_openConditions.removeLast();
                }
            } else {
                token.end = endOfPunctuator(at);
                token.role = roleOfPunctuator(token, last);
                standsAtItems |= token.role == Role::Comma;
            }
            if (around && standsAtItems && token.hasLineBreakInFront)
                m_tokens[*around].hasLineBreakAtItems = true;
            at = token.end;
            m_tokens.append(token);
        }
    }

    // `${ a }` is printed `${a}`.
    // What stands between the values is printed like a text between backticks. Behind a tag it stays as it is written.
    bool appendTemplate(Vector<Latin1Character>& printed, const QuotedToken& token, bool hasTag) const
    {
        size_t startOfText = token.start + 1;
        auto appendText = [&](size_t end) {
            std::span<const Latin1Character> written = m_text.subspan(startOfText, end - startOfText);
            Vector<char16_t> value;
            if (hasTag)
                appendWithEscapes(printed, written, WrittenAs::TemplateBehindTag);
            else if (!appendValueOfWrittenText(value, written))
                printed.append(written);
            else
                appendTextBetweenQuotes(printed, value.span(), '`');
        };
        bool isClosed = token.end - token.start > 1 && m_text[token.end - 1] == '`';
        size_t endOfAll = token.end - isClosed;
        printed.append('`');
        for (size_t at = startOfText; at < endOfAll; ++at) {
            size_t end = startsWith(at, "${"_s) ? endOfBody(at + 1) : notFound;
            if (end > token.end || m_text[end - 1] != '}') {
                at += m_text[at] == '\\';
                continue;
            }
            std::optional<Vector<Latin1Character>> value = print(m_text.subspan(at + 2, end - at - 3));
            if (!value)
                return false;
            appendText(at);
            printed.append("${"_span);
            printed.appendVector(*value);
            printed.append('}');
            at = end - 1;
            startOfText = end;
        }
        appendText(std::max(startOfText, endOfAll));
        if (isClosed)
            printed.append('`');
        return true;
    }

    bool holdsValues(const QuotedToken& token) const
    {
        for (size_t at = token.start + 1; at + 1 < token.end; ++at) {
            if (m_text[at] == '\\')
                ++at;
            else if (startsWith(at, "${"_s))
                return true;
        }
        return false;
    }

    // `new K()` is printed `new K` where nothing is read from it.
    bool isIdleCallOfConstructor(const QuotedToken& open) const
    {
        if (!is(open, "("_s) || open.partner == notFound || &m_tokens[open.partner] != &open + 1)
            return false;
        const QuotedToken* behind = &open + 2;
        if (behind <= &m_tokens.last() && (behind->role == Role::Dot || behind->role == Role::Open || behind->role == Role::Postfix || m_text[behind->start] == '`'))
            return false;
        const QuotedToken* callee = &open;
        while (callee - 1 > m_tokens.begin() && (callee - 1)->role == Role::Operand && isPartOfName(m_text[(callee - 1)->start])) {
            callee -= 1;
            if ((callee - 1)->role != Role::Dot || is(*(callee - 1), "?."_s))
                return callee != &open && is(*(callee - 1), "new"_s) && (callee - 1)->role == Role::Keyword;
            callee -= 1;
        }
        return false;
    }

    bool isOverLines(const QuotedToken& open) const
    {
        return open.holdsItems && open.hasLineBreakAtItems && open.partner != notFound && &m_tokens[open.partner] != &open + 1;
    }

    // How often the line that holds the first token is indented.
    std::optional<unsigned> findIndentation() const
    {
        unsigned linesAround = 0;
        bool hasLines = false;
        for (const QuotedToken& token : m_tokens) {
            if (token.role == Role::Open) {
                if (token.marksOfIndentation)
                    return token.marksOfIndentation - 1 - std::min(linesAround, token.marksOfIndentation - 1);
                if (isOverLines(token)) {
                    hasLines = true;
                    ++linesAround;
                }
            } else if (token.role == Role::Close && token.partner != notFound && isOverLines(m_tokens[token.partner]))
                --linesAround;
        }
        return hasLines ? std::nullopt : std::optional<unsigned>(0);
    }

    bool needsBlank(const QuotedToken& last, const QuotedToken& token) const
    {
        bool isObject = m_text[token.role == Role::Close ? token.start : last.start] == (token.role == Role::Close ? '}' : '{');
        if (token.role == Role::Close)
            return isObject && last.role != Role::Open;
        if (last.role == Role::Open)
            return isObject;
        switch (token.role) {
        case Role::Comma:
            // `[, , a]` has holes.
            return last.role == Role::Comma;
        case Role::Postfix:
            return isPrintedComment(token.start);
        case Role::Semicolon:
        case Role::KeyColon:
            return false;
        case Role::Dot:
            // `1 .toString`
            return token.start > last.end && std::ranges::all_of(m_text.subspan(last.start, last.end - last.start), isASCIIDigit<Latin1Character>);
        default:
            break;
        }
        switch (last.role) {
        case Role::Dot:
        case Role::Spread:
            return false;
        case Role::Prefix:
            // `- -a`
            return token.role == Role::Prefix && m_text[last.start] == m_text[token.start] && m_text[token.start] != '!';
        case Role::Comma:
        case Role::Semicolon:
        case Role::KeyColon:
        case Role::Binary:
            return true;
        default:
            break;
        }
        if (token.role == Role::Binary)
            return true;
        if (last.role == Role::Keyword)
            return !(is(last, "function"_s) && is(token, "("_s));
        if (token.role == Role::Open)
            return false;
        // A tagged template.
        return m_text[token.start] != '`';
    }

    std::optional<Vector<Latin1Character>> print() const
    {
        std::optional<unsigned> indentation = findIndentation();
        if (!indentation)
            return std::nullopt;
        Vector<Latin1Character> printed;
        Vector<const QuotedToken*, 8> open;
        const QuotedToken* last = nullptr;
        for (const QuotedToken& token : m_tokens) {
            const QuotedToken* next = &token == &m_tokens.last() ? nullptr : &token + 1;
            if (isIdleCallOfConstructor(token) || (token.role == Role::Close && token.partner != notFound && isIdleCallOfConstructor(m_tokens[token.partner])))
                continue;
            if (token.role == Role::Comma && next && next->role == Role::Close) {
                // `[a, ,]` has a hole at its end.
                bool isBehindHole = m_text[next->start] == ']' && last && (last->role == Role::Comma || last->role == Role::Open);
                if (!isBehindHole)
                    continue;
            }
            bool closesLines = token.role == Role::Close && token.partner != notFound && isOverLines(m_tokens[token.partner]);
            if (closesLines)
                --*indentation;
            if (token.role == Role::Close && !open.isEmpty())
                open.removeLast();
            bool startsItem = last && !open.isEmpty() && isOverLines(*open.last()) && (last == open.last() || last->role == Role::Comma);
            if (closesLines || startsItem) {
                printed.append('\n');
                printed.appendUsingFunctor(*indentation * 2, [](size_t) {
                    return Latin1Character(' ');
                });
            } else if (last && needsBlank(*last, token))
                printed.append(' ');
            // `a => a` is printed `(a) => a`.
            bool isLoneParameter = token.role == Role::Operand && next && is(*next, "=>"_s);
            std::span<const Latin1Character> spelled = m_text.subspan(token.start, token.end - token.start);
            bool isEmptyBody = token.role == Role::Operand && spelled.front() == '{' && spelled.size() > 1 && last && is(*last, ")"_s) && std::ranges::all_of(spelled.subspan(1, spelled.size() - 2), isBlank);
            // `{ "a": 1 }` is printed `{ a: 1 }`.
            bool isKeyPrintedAsName = spelled.size() > 2 && isQuote(spelled.front()) && spelled.front() != '`' && spelled.back() == spelled.front() && next && next->role == Role::KeyColon
                && !isASCIIDigit(spelled[1]) && std::ranges::all_of(spelled.subspan(1, spelled.size() - 2), [](Latin1Character character) {
                    return isASCII(character) && isPartOfName(character);
                });
            if (isLoneParameter)
                printed.append('(');
            if (isEmptyBody)
                printed.append("{}"_span);
            else if (isKeyPrintedAsName)
                printed.append(spelled.subspan(1, spelled.size() - 2));
            else if (isQuote(spelled.front())) {
                bool hasTag = spelled.front() == '`' && last && last->endsOperand();
                bool isKey = next && next->role == Role::KeyColon;
                if (hasTag || (spelled.front() == '`' && holdsValues(token))) {
                    if (!appendTemplate(printed, token, hasTag))
                        return std::nullopt;
                } else if (!appendTextAsPrinted(printed, spelled, !isKey))
                    printed.append(spelled);
            } else if (std::ranges::all_of(spelled, isASCII<Latin1Character>))
                printed.append(spelled);
            else
                appendWithEscapes(printed, spelled, spelled.front() == '/' ? WrittenAs::RegularExpression : WrittenAs::Name);
            if (isLoneParameter)
                printed.append(')');
            if (token.role == Role::Open) {
                open.append(&token);
                *indentation += isOverLines(token);
            }
            last = &token;
        }
        return printed;
    }

    std::span<const Latin1Character> m_text;
    Vector<QuotedToken> m_tokens;
    Vector<unsigned, 8> m_openConditions;
};

enum class QuotedText : bool { Other, TakesValues };

// `text` with one mark for all that stands between two: see markOfTextFromPrinter.
static Vector<Latin1Character> withoutTextFromPrinter(std::span<const Latin1Character> text, Vector<std::span<const Latin1Character>, 4>& fromPrinter)
{
    constexpr auto mark = static_cast<Latin1Character>(markOfTextFromPrinter);
    Vector<Latin1Character> rest;
    for (size_t at = 0; at < text.size();) {
        size_t start = std::min(WTF::find(text, mark, at), text.size());
        rest.append(text.subspan(at, start - at));
        if (start == text.size())
            break;
        size_t end = std::min(WTF::find(text, mark, start + 1), text.size());
        fromPrinter.append(text.subspan(start + 1, end - start - 1));
        rest.append(mark);
        at = std::min(end + 1, text.size());
    }
    return rest;
}

// Nothing where a mark is lost.
static std::optional<Vector<Latin1Character>> withTextFromPrinter(std::span<const Latin1Character> text, std::span<const std::span<const Latin1Character>> fromPrinter)
{
    constexpr auto mark = static_cast<Latin1Character>(markOfTextFromPrinter);
    Vector<Latin1Character> whole;
    size_t at = 0;
    for (std::span<const Latin1Character> printed : fromPrinter) {
        size_t place = WTF::find(text, mark, at);
        if (place == notFound)
            return std::nullopt;
        whole.append(text.subspan(at, place - at));
        whole.append(printed);
        at = place + 1;
    }
    whole.append(text.subspan(at));
    return whole;
}

static String asPrinted(std::span<const Latin1Character> textWithAll, QuotedText quoted, bool runsAsWritten = false)
{
    if (quoted == QuotedText::TakesValues && runsAsWritten)
        return String::fromUTF8(textWithAll);
    Vector<std::span<const Latin1Character>, 4> fromPrinter;
    Vector<Latin1Character> rest = withoutTextFromPrinter(textWithAll, fromPrinter);
    std::span<const Latin1Character> text = rest.span();
    std::optional<Vector<Latin1Character>> withBlanks;
    if (!runsAsWritten)
        withBlanks = PrintedBlanks::print(text);
    if (!withBlanks && quoted == QuotedText::TakesValues)
        return { };
    if (!withBlanks)
        withBlanks = onOneLine(text);
    Vector<Latin1Character> printed;
    Vector<Latin1Character> withoutTypesAndBlanks = withoutTypes(withBlanks->span());
    appendWithoutIdleBrackets(printed, runsAsWritten ? withoutTypesAndBlanks.span() : withShortLiterals(withoutTypesAndBlanks.span()).span());
    printed.removeAllMatching([](Latin1Character character) {
        return character == markOfPrintedKey || character == markOfIndentation;
    });
    // `"a" in Color.Green` is printed `"a" in 1 /* Green */`, and the expression ends with its last token.
    if (spanHasSuffix(printed.span(), byteCast<Latin1Character>(" */"_span))) {
        if (size_t comment = reverseFind(printed.span(), byteCast<Latin1Character>(" /* "_span)); comment != notFound)
            printed.shrink(comment);
    }
    if (fromPrinter.isEmpty())
        return String::fromUTF8(printed.span());
    std::optional<Vector<Latin1Character>> whole = withTextFromPrinter(printed.span(), fromPrinter.span());
    return whole ? String::fromUTF8(whole->span()) : String();
}

// `found` lies in `text`, which is what the pieces count from.
static String asPrinted(std::span<const Latin1Character> text, std::span<const Latin1Character> found, std::span<const TextPrintedInPlace> pieces, FindInText find)
{
    QuotedText quoted = find == findWrittenListAt || find == findWrittenIterations ? QuotedText::TakesValues : QuotedText::Other;
    if (pieces.empty() || TextPrintedInPlace::sayFileRunsAsWritten(pieces))
        return asPrinted(found, quoted, !pieces.empty());
    Vector<Latin1Character> printed;
    size_t start = found.data() - text.data();
    appendWithTextPrintedInPlace(printed, text, start, start + found.size(), pieces);
    return asPrinted(printed.span(), quoted);
}

// A column counts units of UTF-16, and the file is UTF-8. Goes on from `text[at]`, which stands in `column`, to the wanted column or to the end of the line.
static size_t advanceToColumn(std::span<const Latin1Character> text, size_t at, size_t lineEnd, unsigned& column, unsigned wanted)
{
    while (column < wanted && at < lineEnd) {
        // Each character of ASCII is one unit.
        size_t size = std::min<size_t>({ lineEnd - at, wanted - column, 64 });
        uint8_t seen = 0;
        for (Latin1Character byte : text.subspan(at, size))
            seen |= byte;
        if (seen < 0x80) {
            column += static_cast<unsigned>(size);
            at += size;
            continue;
        }
        for (size_t end = at + size; column < wanted && at < end;) {
            Latin1Character lead = text[at];
            size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            column += length == 4 ? 2 : 1;
            at += length;
        }
    }
    return at;
}

static size_t offsetOfColumn(SourceFile& file, size_t line, unsigned column)
{
    constexpr unsigned columnsOfStretch = 4096;
    std::span<const Latin1Character> text = byteCast<Latin1Character>(file.bytes.span());
    size_t lineStart = file.lineStarts[line];
    size_t lineEnd = line + 1 < file.lineStarts.size() ? file.lineStarts[line + 1] - 1 : text.size();
    SourceFile::PlaceInLine place { lineStart, 1 };
    if (column > columnsOfStretch) {
        if (file.lineOfPlaces != line) {
            file.lineOfPlaces = line;
            file.placesInLine.shrink(0);
            while (place.offset < lineEnd) {
                file.placesInLine.append(place);
                place.offset = advanceToColumn(text, place.offset, lineEnd, place.column, place.column + columnsOfStretch);
            }
        }
        auto behind = std::ranges::upper_bound(file.placesInLine, column, { }, &SourceFile::PlaceInLine::column);
        place = behind == file.placesInLine.begin() ? SourceFile::PlaceInLine { lineStart, 1 } : *std::prev(behind);
    }
    return std::min(advanceToColumn(text, place.offset, lineEnd, place.column, column), lineEnd);
}

// The part of a file that holds every expression around a place in it, of those that belong to the function of the place. All of it, unless the line of the place is long.
static std::pair<size_t, size_t> findStatementsAround(SourceFile& file, size_t at)
{
    constexpr size_t lengthOfLongLine = 4096;
    using Mark = SourceFile::MarkInLine;
    std::span<const Latin1Character> text = byteCast<Latin1Character>(file.bytes.span());
    size_t line = std::ranges::upper_bound(file.lineStarts, at) - file.lineStarts.begin();
    if (!line)
        return { 0, text.size() };
    --line;
    size_t lineStart = file.lineStarts[line];
    size_t lineEnd = line + 1 < file.lineStarts.size() ? file.lineStarts[line + 1] - 1 : text.size();
    if (lineEnd - lineStart <= lengthOfLongLine)
        return { 0, text.size() };
    if (file.lineOfMarks != line) {
        file.lineOfMarks = line;
        file.marksInLine.shrink(0);
        int32_t openBraces = 0;
        for (size_t place = lineStart; place < lineEnd; ++place) {
            if (std::optional<size_t> end = endOfQuotedOrPattern(text, lineStart, place, lineEnd)) {
                place = *end;
                continue;
            }
            Latin1Character character = text[place];
            if (character == '/' && place + 1 < lineEnd && text[place + 1] == '*') {
                size_t end = place + 2;
                while (end + 1 < lineEnd && (text[end] != '*' || text[end + 1] != '/'))
                    ++end;
                place = end + 1;
            } else if (character == ';')
                file.marksInLine.append({ place, openBraces, Mark::EndOfStatement });
            else if (character == '{')
                file.marksInLine.append({ place, ++openBraces, Mark::Open });
            else if (character == '}')
                file.marksInLine.append({ place, --openBraces, Mark::Close });
        }
    }
    std::span<const Mark> marks = file.marksInLine.span();
    size_t behind = std::ranges::lower_bound(marks, at, { }, &Mark::offset) - marks.begin();
    int32_t openBraces = behind ? marks[behind - 1].openBraces : 0;
    size_t start = lineStart;
    for (size_t index = behind; index--;) {
        if (marks[index].openBraces == openBraces && marks[index].kind != Mark::Close) {
            start = marks[index].offset + 1;
            break;
        }
    }
    size_t end = lineEnd;
    for (size_t index = behind; index < marks.size(); ++index) {
        bool endsStatement = marks[index].kind == Mark::EndOfStatement && marks[index].openBraces == openBraces;
        if (endsStatement || (marks[index].kind == Mark::Close && marks[index].openBraces < openBraces)) {
            end = marks[index].offset;
            break;
        }
    }
    return { start, std::max(end, std::min(at + 1, lineEnd)) };
}

enum class SearchedText : bool { OnlyKnownPlace, WholeFunction };

// `text[from, to)` with one long name for each function and class that the compiler gives as Bun prints it, but for those around `place`.
// So nothing in them ends what is searched for, and `(() => null)().a` is a path. No text holds the byte that the name is made of.
static Vector<Latin1Character> withNamesForTextFromPrinter(std::span<const Latin1Character> text, size_t from, size_t to, size_t place, std::span<const TextPrintedInPlace> pieces)
{
    Vector<Latin1Character> named(text.subspan(from, to - from));
    size_t endOfNames = from;
    for (const TextPrintedInPlace& piece : pieces) {
        if (!isFromPrinter(piece) || piece.start < endOfNames || piece.end > to || (piece.start <= place && place < piece.end))
            continue;
        std::ranges::fill(named.mutableSpan().subspan(piece.start - from, piece.end - piece.start), Latin1Character(0xFF));
        endOfNames = piece.end;
    }
    return named;
}

// The lines of the path around `place`. A chain may take several, and the place of what failed in it is a name further up: that of `.c` in `a` / `.b` / `.c` is `b`.
static std::pair<size_t, size_t> linesOfPathAround(std::span<const Latin1Character> text, size_t place)
{
    size_t endOfName = place;
    while (endOfName < text.size() && isPartOfName(text[endOfName]))
        ++endOfName;
    size_t start = startOfLine(text, std::min(startOfPathInFrontOf(text, endOfName).value_or(place), place));
    // The place may be the last thing in brackets of the path: that of `.b` in `a[k]` / `.b` is `k`.
    size_t end = place;
    while (true) {
        end = std::max(endOfPathFrom(text, end), end);
        size_t close = end;
        while (close < text.size() && (text[close] == ' ' || text[close] == '\t'))
            ++close;
        close = startOfCodeInLaterLine(text, close);
        // `f(a, )`
        if (close < text.size() && text[close] == ',') {
            size_t behindComma = close + 1;
            while (behindComma < text.size() && (text[behindComma] == ' ' || text[behindComma] == '\t'))
                ++behindComma;
            close = startOfCodeInLaterLine(text, behindComma);
        }
        if (close >= text.size() || (text[close] != ')' && text[close] != ']'))
            break;
        end = close + 1;
    }
    return { start, std::min(WTF::find(text, static_cast<Latin1Character>('\n'), end), text.size()) };
}

static WrittenReads findWrittenCallOfReplacedNameAt(std::span<const Latin1Character>, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found);

// What `text[startOfSearched, endOfSearched)` spells around `place`, which counts from the start of `text` as the pieces do.
// The place of an error lies in what `bun run` has. Of `const r = f(x); return r.y` that is `return f(x).y`, so the place lies in the initializer, and what failed is written around the name.
// `findStatementsAround`: the part of the text that holds every expression around a place in it.
static String findSpelledAround(std::span<const Latin1Character> text, size_t startOfSearched, size_t endOfSearched, size_t place, std::span<const TextPrintedInPlace> pieces, StringView name, FindInText find, const Invocable<std::pair<size_t, size_t>(size_t)> auto& findStatementsAround)
{
    bool hasTextFromPrinter = std::ranges::any_of(pieces, isFromPrinter);
    Vector<size_t, 4> places { place };
    for (size_t index = 0; index < places.size(); ++index) {
        auto [startOfStatements, endOfStatements] = findStatementsAround(places[index]);
        std::span<const Latin1Character> searched = text.subspan(startOfStatements, endOfStatements - startOfStatements);
        size_t within = places[index] - startOfStatements;
        size_t lineStart = startOfLine(searched, within);
        size_t lineEnd = std::min(WTF::find(searched, static_cast<Latin1Character>('\n'), within), searched.size());
        Vector<Latin1Character> named;
        if (hasTextFromPrinter)
            named = withNamesForTextFromPrinter(text, startOfStatements, endOfStatements, places[index], pieces);
        std::span<const Latin1Character> read = hasTextFromPrinter ? named.span() : searched;
        std::span<const Latin1Character> found;
        auto isAsked = [&, startOfRead = startOfStatements] {
            if (find != findWrittenCallOfReplacedNameAt)
                return true;
            size_t callee = found.data() - read.data() + (spanHasPrefix(found, byteCast<Latin1Character>("new "_span)) ? 4 : 0);
            // `new (make())`: nor for what stands in brackets.
            if (callee < read.size() && read[callee] == '(')
                return true;
            // `undefined()`: the compiler has no name for a literal either.
            size_t endOfCallee = callee;
            while (endOfCallee < read.size() && isPartOfName(read[endOfCallee]))
                ++endOfCallee;
            std::span<const Latin1Character> word = read.subspan(callee, endOfCallee - callee);
            if (std::ranges::any_of(std::array { "undefined"_s, "null"_s, "true"_s, "false"_s }, [&](ASCIILiteral literal) { return equalSpans(word, byteCast<Latin1Character>(literal.span())); }))
                return true;
            // The name may be one that stands for a function as Bun prints it.
            return isASCII(read[callee]) && std::ranges::any_of(pieces, [&](const TextPrintedInPlace& piece) {
                return piece.start == startOfRead + callee && !isFromPrinter(piece);
            });
        };
        if (lineStart < lineEnd && find(read, lineStart, lineEnd, name, within, found) == WrittenReads::Same && isAsked())
            return asPrinted(text, searched.subspan(found.data() - read.data(), found.size()), pieces, find);
        auto [firstLineStart, lastLineEnd] = linesOfPathAround(read, within);
        found = { };
        if ((firstLineStart < lineStart || lastLineEnd > lineEnd) && find(read, firstLineStart, lastLineEnd, name, within, found) == WrittenReads::Same && isAsked())
            return asPrinted(text, searched.subspan(found.data() - read.data(), found.size()), pieces, find);
        for (const TextPrintedInPlace& piece : pieces) {
            bool printsPlace = piece.isCopy && piece.startOfCopied <= places[index] && places[index] <= piece.endOfCopied;
            if (printsPlace && piece.start >= startOfSearched && piece.start < endOfSearched && !places.contains(piece.start))
                places.append(piece.start);
        }
    }
    return { };
}

// What the function spells at the place.
static String findSpelled(VM& vm, const PlaceInFunctionText* place, StringView name, FindInText find, SearchedText searched = SearchedText::WholeFunction)
{
    // A file says which line and column it is, so only what holds that place counts.
    if (place->module && place->position.line && place->position.column) {
        SourceFile* file = sourceFileOf(vm, place->module);
        if (file && place->position.line <= file->lineStarts.size()) {
            std::span<const Latin1Character> text = byteCast<Latin1Character>(file->bytes.span());
            std::span<const TextPrintedInPlace> pieces = file->textPrintedInPlace.span();
            auto findStatementsAround = [&](size_t at) {
                return AOT::findStatementsAround(*file, at);
            };
            if (String quote = findSpelledAround(text, 0, text.size(), offsetOfColumn(*file, place->position.line - 1, place->position.column), pieces, name, find, findStatementsAround); !quote.isNull())
                return quote;
        }
    }

    // An executable has the text of the function alone, and knows where that stood in the file.
    std::span<const Latin1Character> text = functionSourceBytesOfSource(place->function);
    std::span<const Latin1Character> textOfAll = sourceBytesOfAllFunctions();
    if (place->module && place->position.line && place->position.column) {
        QuotesOfPlaces& quotes = quotesOfPlaces(vm);
        if (quotes.placeInKeptTextIsOf != place->address) {
            quotes.placeInKeptTextIsOf = place->address;
            quotes.placeInKeptText = findInKeptText(place->function, place->module, place->position);
            std::pair<size_t, size_t> piece;
            if (quotes.placeInKeptText)
                piece = { quotes.placeInKeptText->startOfPiece, quotes.placeInKeptText->endOfPiece };
            if (quotes.placeInKeptText && (quotes.moduleOfTextPrintedInPlace != numberAmongAllImages(place->module) || quotes.pieceOfTextPrintedInPlace != piece)) {
                quotes.moduleOfTextPrintedInPlace = numberAmongAllImages(place->module);
                quotes.pieceOfTextPrintedInPlace = piece;
                quotes.textPrintedInPlace = textPrintedInPlaceOf(place->module, StartOfPrintedText::SourceBytesOfAllFunctions, piece.first, piece.second);
                countTextPrintedInPlaceDecoded(vm, quotes.textPrintedInPlace.size());
            }
        }
        if (const std::optional<PlaceInKeptText>& found = quotes.placeInKeptText) {
            auto findStatementsAround = [&](size_t at) {
                return AOT::findStatementsAround(*found, at);
            };
            if (String quote = findSpelledAround(textOfAll, found->startOfSearched, found->endOfSearched, found->at, quotes.textPrintedInPlace.span(), name, find, findStatementsAround); !quote.isNull())
                return quote;
        }
    }

    if (searched == SearchedText::OnlyKnownPlace || !text.data())
        return { };
    Vector<TextPrintedInPlace> pieces;
    if (place->module)
        pieces = textPrintedInPlaceOf(place->module, StartOfPrintedText::SourceBytesOfAllFunctions);

    // Nothing says where in the text the place is. So this answers only where the line of the place, or else the whole function, has one such place, or spells each of them alike.
    std::span<const Latin1Character> found;
    WrittenReads reads = WrittenReads::None;
    if (place->line) {
        size_t lineStart = 0;
        for (unsigned line = 0; line < *place->line && lineStart < text.size(); ++line)
            lineStart = startOfNextLine(text, lineStart);
        size_t lineEnd = startOfNextLine(text, lineStart);
        lineEnd -= lineEnd > lineStart && text[lineEnd - 1] == '\n';
        if (lineStart < lineEnd)
            reads = find(text, lineStart, lineEnd, name, std::nullopt, found);
    }
    // Types that took lines of their own are not in the text, so the line may be another.
    if (reads == WrittenReads::None)
        reads = find(text, 0, text.size(), name, std::nullopt, found);
    return reads == WrittenReads::Same ? asPrinted(textOfAll, found, pieces.span(), find) : String();
}

// Each place in `text[from, to)` that calls `name`, which is the callee as it is written or the last of its names. `found` holds the call with its arguments, and `new` in front of it.
static WrittenReads findWrittenCalls(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    BytesOfName bytesOfName(name);
    for (size_t at = from; at + name.length() < to; ++at) {
        at = bytesOfName.findIn(text, at, to);
        if (at == notFound || at + name.length() >= to)
            break;
        size_t open = at + name.length() + (text[at + name.length()] == '!');
        if (open >= to || (text[open] != '(' && text[open] != '`') || (at && (isPartOfName(text[at - 1]) || text[at - 1] == '#')))
            continue;
        if (!equal(name, text.subspan(at, name.length())))
            continue;
        size_t start = at;
        if (at && text[at - 1] == '.') {
            size_t dot = at - 1;
            if (dot && text[dot - 1] == '?')
                --dot;
            std::optional<size_t> path = startOfPathInFrontOf(text, dot);
            if (!path)
                return WrittenReads::Differ;
            start = *path;
        }
        std::optional<size_t> close = text[open] == '`' ? findEndOfTemplate(text, open) : findClosingBracket(text, open);
        if (!close)
            return WrittenReads::Differ;
        if (start >= 4 && text[start - 4] == 'n' && text[start - 3] == 'e' && text[start - 2] == 'w' && text[start - 1] == ' ' && (start == 4 || !isPartOfName(text[start - 5])))
            start -= 4;
        if (within && (*within < start || *within > *close))
            continue;
        std::span<const Latin1Character> call = text.subspan(start, *close + 1 - start);
        // `f(f(n))`: both calls hold the place of the inner one. The place of a call is the last name of its callee, and that of a construction its `new`.
        if (within && (*within == at || *within == start)) {
            found = call;
            return WrittenReads::Same;
        }
        if (found.data() && !equalSpans(found, call))
            return WrittenReads::Differ;
        found = call;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// The call whose callee ends in the name that begins at `within`: that is the column which a stack trace gives for a call.
enum class WrittenCallee : bool { Name, ReplacedName };
static WrittenReads findWrittenCallAt(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found, WrittenCallee callee)
{
    if (!within || *within >= to)
        return WrittenReads::None;
    bool isReplaced = callee == WrittenCallee::ReplacedName;
    size_t at = *within;
    // `a[k](b)`: the column is that of the key. In `[f()]` and `a[f()]` it is that of `f`.
    if (at > from + 1 && text[at - 1] == '[') {
        if (WrittenReads reads = findWrittenCallsOfElements(text, at - 2, to, name, within, found); reads != WrittenReads::None)
            return reads;
    }
    // `new (make())(b)`: with operands the column is that of the last token in the brackets.
    if (isReplaced) {
        size_t end = at;
        while (end < text.size() && isPartOfName(text[end]))
            ++end;
        end += end == at;
        std::optional<size_t> open = end < text.size() && text[end] == ')' ? findOpeningBracket(text, end) : std::nullopt;
        if (open && *open >= from + 4 && spanHasPrefix(text.subspan(*open - 4), byteCast<Latin1Character>("new ("_span)))
            at = *open - 4;
    }
    if (!isPartOfName(text[at]) || isASCIIDigit(text[at]) || (at && (isPartOfName(text[at - 1]) || text[at - 1] == '#')))
        return WrittenReads::None;
    // `new (make())(b)`, `new (make())`: what is constructed stands in brackets.
    if (isReplaced && spanHasPrefix(text.subspan(at), byteCast<Latin1Character>("new ("_span))) {
        std::optional<size_t> close = findClosingBracket(text, at + 4);
        if (close && *close + 1 < text.size() && text[*close + 1] == '(')
            close = findClosingBracket(text, *close + 1);
        if (!close)
            return WrittenReads::Differ;
        found = text.subspan(at, *close + 1 - at);
        return WrittenReads::Same;
    }
    // `new K`: the column is that of `new`.
    if (isReplaced && spanHasPrefix(text.subspan(at), byteCast<Latin1Character>("new "_span)) && at + 4 < to && isPartOfName(text[at + 4]))
        at += 4;
    size_t open = at;
    while (open < text.size() && isPartOfName(text[open]))
        ++open;
    bool followsNew = isReplaced && at >= 4 && spanHasPrefix(text.subspan(at - 4), byteCast<Latin1Character>("new "_span)) && (at == 4 || !isPartOfName(text[at - 5]));
    if (followsNew && (open >= text.size() || (text[open] != '(' && text[open] != '.' && text[open] != '[' && text[open] != '!' && text[open] != '?' && text[open] != '`'))) {
        found = text.subspan(at - 4, open - at + 4);
        return WrittenReads::Same;
    }
    if (open < text.size() && text[open] == '!')
        ++open;
    if (open + 1 < text.size() && text[open] == '?' && text[open + 1] == '.')
        open += 2;
    if (open >= text.size() || (text[open] != '(' && (text[open] != '`' || !isReplaced)))
        return WrittenReads::None;
    size_t start = at;
    if (at && text[at - 1] == '.') {
        size_t dot = at - 1;
        if (dot && text[dot - 1] == '?')
            --dot;
        std::optional<size_t> path = startOfPathInFrontOf(text, dot);
        if (!path)
            return WrittenReads::Differ;
        start = *path;
    }
    std::optional<size_t> close = text[open] == '`' ? findEndOfTemplate(text, open) : findClosingBracket(text, open);
    if (!close)
        return WrittenReads::Differ;
    if (followsNew && start == at)
        start -= 4;
    found = text.subspan(start, *close + 1 - start);
    return WrittenReads::Same;
}

static WrittenReads findWrittenCallAt(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    return findWrittenCallAt(text, from, to, name, within, found, WrittenCallee::Name);
}

// The same, where something else is printed in place of the name: findSpelledAround() sees to that.
static WrittenReads findWrittenCallOfReplacedNameAt(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    return findWrittenCallAt(text, from, to, name, within, found, WrittenCallee::ReplacedName);
}

// The engine's own C++ refuses what is not an object in a function that the program calls, or where it asks a value for its iterator.
static WrittenReads findWrittenCallOrIterationAt(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    WrittenReads reads = findWrittenCallAt(text, from, to, name, within, found);
    return reads == WrittenReads::None ? findWrittenIterations(text, from, to, name, within, found) : reads;
}

static String findAt(VM& vm, const PlaceInFunctionText* place, StringView name, FindInText find, bool& holdsForEveryName)
{
    // Asked one after the other, so that each is printed in its own way.
    if (find == findWrittenCallOrIterationAt) {
        String quote = findSpelled(vm, place, name, findWrittenCallAt);
        return quote.isNull() ? findSpelled(vm, place, name, findWrittenIterations) : quote;
    }
    // `a[k]`, where the compiler knows what `k` holds. The function may read that name behind a dot elsewhere, and that is not what failed:
    // where an element stands at the place and no read of the name, the rest of the function is not looked through for one.
    if (find == findWrittenReads && findSpelled(vm, place, name, find, SearchedText::OnlyKnownPlace).isNull()
        && !findSpelled(vm, place, "[]"_s, findWrittenElements, SearchedText::OnlyKnownPlace).isNull()) {
        holdsForEveryName = true;
        return findSpelled(vm, place, name, findWrittenElements);
    }
    String quote = findSpelled(vm, place, name, find);
    // `a[k]`, where the name is what `k` held this time.
    if (quote.isNull() && find == findWrittenReads) {
        holdsForEveryName = true;
        return findSpelled(vm, place, name, findWrittenElements);
    }
    // The compiler gave no name: the key is computed, or private, or it is the values of a list that are read.
    if (quote.isNull() && find == findWrittenElements && name == "[]"_s) {
        quote = findSpelled(vm, place, name, findWrittenPrivateNames);
        return quote.isNull() ? findSpelled(vm, place, name, findWrittenIterations) : quote;
    }
    if (!quote.isNull() || (find != findWrittenCalls && find != findWrittenCallsOfElements))
        return quote;
    if (size_t dot = name.reverseFind('.'); find == findWrittenCalls && dot != notFound) {
        // `(api as T).run()` calls `api.run`. Another `.run()` of the line does not.
        quote = findSpelled(vm, place, name.substring(dot + 1), find);
        StringView callee = quote;
        if (callee.startsWith("new "_s))
            callee = callee.substring(4);
        return callee.length() > name.length() && callee.startsWith(name) && callee[name.length()] == '(' ? quote : String();
    }
    // `const b = null; b(1)`: the compiler's name for the callee is `null`, or it has none. What is written there is printed with `null` in place of `b`.
    return findSpelled(vm, place, name, findWrittenCallOfReplacedNameAt, SearchedText::OnlyKnownPlace);
}

// A place asks for one name in one way, so its text is looked through once, however often it throws.
static String findOnce(VM& vm, const PlaceInFunctionText& place, StringView name, FindInText find)
{
    if (!place.address)
        return { };
    if (place.isWhereListIsWritten && (find == findWrittenIterations || find == findWrittenCallOrIterationAt || find == findWrittenElements))
        find = findWrittenListAt;
    QuotesOfPlaces::Answer& answer = quotesOfPlaces(vm).answers.add(place.address, QuotesOfPlaces::Answer { }).iterator->value;
    if (answer.find != find || (!answer.holdsForEveryName && StringView(answer.name) != name)) {
        bool holdsForEveryName = find == findWrittenElements;
        String quote = findAt(vm, &place, name, find, holdsForEveryName);
        answer = { find, name.toString(), WTF::move(quote), holdsForEveryName };
        QuotesOfPlaces& quotes = quotesOfPlaces(vm);
        quotes.placeInKeptTextIsOf = nullptr;
    }
    return answer.quote;
}

// `Object.keys(null)`: the function throws from the engine's own C++, which knows neither its name nor who called it.
static String findCallOfNativeFunction(VM& vm)
{
    String quote;
    withPlaceOfCallOfNativeFunction(vm, [&](const PlaceInFunctionText& place) {
        quote = findOnce(vm, place, "()"_s, findWrittenCallOrIterationAt);
    });
    return quote;
}

// What the function on top spells at the place that failed.
static String findOnTop(VM& vm, StringView name, FindInText find)
{
    if (name.isEmpty() || !name.containsOnlyASCII())
        return { };
    auto findOnce = [&](const PlaceInFunctionText& place) {
        return AOT::findOnce(vm, place, name, find);
    };
    std::optional<PlaceInFunctionText> place = placeOnTopInFunctionText(vm);
    String quote;
    if (place) [[likely]] {
        quote = findOnce(*place);
        if (!quote.isNull()) [[likely]]
            return quote;
    }
    if (place ? find == findWrittenCalls && (name.endsWith(".call"_s) || name.endsWith(".apply"_s)) : g_imagesShareRealms) {
        withPlaceThatCalledLibraryOnTop(vm, [&](const PlaceInFunctionText& placeThatCalled) {
            quote = findOnce(placeThatCalled);
        });
    }
    return quote;
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, StringView member)
{
    // No name: the compiler does not know the key.
    if (String written = findOnTop(globalObject->vm(), member.isNull() ? StringView("[]"_s) : member, isWrittenBehindDot(member) ? findWrittenReads : findWrittenElements); !written.isNull())
        return createNotAnObjectErrorQuoting(written, globalObject, base);
    String source;
    if (String description = describe(globalObject, base); !member.isNull() && !description.isNull())
        source = isWrittenBehindDot(member) ? tryMakeString(description, '.', member) : tryMakeString(description, "[\""_s, member, "\"]"_s);
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier& member)
{
    if (member.isPrivateName())
        return createNotAnObjectErrorQuoting(findOnTop(globalObject->vm(), "#"_s, findWrittenPrivateNames), globalObject, base);
    if (!member.isSymbol())
        return createNotAnObjectError(globalObject, base, StringView(member.impl()));
    if (String written = findOnTop(globalObject->vm(), "[]"_s, findWrittenElements); !written.isNull())
        return createNotAnObjectErrorQuoting(written, globalObject, base);
    String source;
    if (String description = describe(globalObject, base); !description.isNull())
        source = tryMakeString(description, "[Symbol("_s, StringView(member.impl()), ")]"_s);
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    if (String written = findOnTop(globalObject->vm(), "[]"_s, findWrittenElements); !written.isNull())
        return createNotAnObjectErrorQuoting(written, globalObject, base);
    String source;
    // To describe an object would name its class, which says nothing of the key it would become.
    if (key && !key.isObject()) {
        // A key made by concatenation is a rope. This is a throw, so flatten it; running out of memory just loses the quote.
        if (key.isString() && asString(key)->isRope()) {
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
            asString(key)->value(globalObject);
            if (scope.exception()) [[unlikely]] {
                scope.clearException();
                return createNotAnObjectErrorQuoting(String(), globalObject, base);
            }
        }
        String description = describe(globalObject, base);
        String descriptionOfKey = describe(globalObject, key);
        if (!description.isNull() && !descriptionOfKey.isNull())
            source = tryMakeString(description, '[', descriptionOfKey, ']');
    }
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createKeyTestOfNonObjectError(JSGlobalObject* globalObject, JSValue base)
{
    return createQuoting(findOnTop(globalObject->vm(), "in"_s, findWrittenKeyTests), createInvalidInParameterError, globalObject, base);
}

JSObject* createDeletionFromNullishError(JSGlobalObject* globalObject, JSValue base)
{
    return createNotAnObjectErrorQuoting(findOnTop(globalObject->vm(), "delete"_s, findWrittenDeletions), globalObject, base);
}

JSObject* createIterationOfNullishError(JSGlobalObject* globalObject, JSValue iterated)
{
    return createNotAnObjectErrorQuoting(findOnTop(globalObject->vm(), "of"_s, findWrittenIterations), globalObject, iterated);
}

JSObject* createPrivateNameOfNullishError(JSGlobalObject* globalObject, JSValue base)
{
    return createNotAnObjectErrorQuoting(findOnTop(globalObject->vm(), "#"_s, findWrittenPrivateNames), globalObject, base);
}

JSObject* createAbsentPrivateNameError(JSGlobalObject* globalObject, JSObject* (*create)(JSGlobalObject*))
{
    String source = findOnTop(globalObject->vm(), "#"_s, findWrittenPrivateNames);
    SetForScope quotedInScope(quotedSource, source.isNull() ? nullptr : &source);
    return create(globalObject);
}

JSObject* createSuperBaseNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier* member)
{
    String source = !member || member->isSymbol() || member->isPrivateName() ? String() : tryMakeString("super."_s, StringView(member->impl()));
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

// `new a.K(b)` and `new K`, but not `new K(b).c()`, which calls.
static bool isConstruction(const String& source)
{
    if (!source.startsWith("new "_s))
        return false;
    CString bytes = source.utf8();
    std::span<const Latin1Character> text = byteCast<Latin1Character>(bytes.span());
    std::optional<size_t> open = text.back() == ')' ? findOpeningBracket(text, text.size() - 1) : std::nullopt;
    if (!open)
        return true;
    // `new (void 0)(b)` and `new (void 0)`: the first brackets hold what is constructed.
    if (text[4] == '(' && *open <= findClosingBracket(text, 4).value_or(0) + 1)
        return true;
    return WTF::find(text.first(*open), static_cast<Latin1Character>('(')) == notFound;
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = name.isEmpty() ? findOnTop(globalObject->vm(), "[]"_s, findWrittenCallsOfElements) : findOnTop(globalObject->vm(), name, findWrittenCalls);
    // `o["a b"]()`, and a key that is printed in brackets though it could stand behind a dot: one with a letter that is not ASCII.
    if (source.isNull() && !name.isEmpty())
        source = findOnTop(globalObject->vm(), "[]"_s, findWrittenCallsOfElements);
    if (source.isNull() || isConstruction(source)) {
        source = name.isNull() ? String() : tryMakeString(name, "()"_s);
        if (!source.isNull() && !source.containsOnlyASCII()) {
            Vector<Latin1Character> printed;
            appendWithEscapes(printed, byteCast<Latin1Character>(source.utf8().span()), WrittenAs::Name);
            source = String::fromUTF8(printed.span());
        }
    }
    return createQuoting(source, JSC::createNotAFunctionError, globalObject, callee);
}

static String quoteConstruction(VM& vm, StringView name)
{
    String source = name.isEmpty() ? findOnTop(vm, "()"_s, findWrittenCallOfReplacedNameAt) : findOnTop(vm, name, findWrittenCalls);
    // As in createNotAFunctionError().
    if (source.isNull() && !name.isEmpty())
        source = findOnTop(vm, "[]"_s, findWrittenCallsOfElements);
    if (source.isNull() || !isConstruction(source))
        return name.isNull() ? String() : tryMakeString("new "_s, name);
    // Bun's transpiler prints `new K()` as `new K`.
    return source.endsWith("()"_s) ? source.left(source.length() - 2) : source;
}

// A constructor that nobody wrote passes on what it is given.
static String quoteCallOfParent(VM& vm)
{
    String source = findOnTop(vm, "super"_s, findWrittenCalls);
    return source.isNull() ? String("super(...args)"_s) : source;
}

JSObject* createParentNotAConstructorError(JSGlobalObject* globalObject, JSValue parent)
{
    return createQuoting(quoteCallOfParent(globalObject->vm()), JSC::createNotAConstructorError, globalObject, parent);
}

JSObject* createNotAConstructorError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = quoteConstruction(globalObject->vm(), name);
    return createQuoting(source, JSC::createNotAConstructorError, globalObject, callee);
}

void throwNotAConstructorErrorInFrameOf(JSGlobalObject* globalObject, JSFunction* function, StringView name, bool isParent)
{
    String source = isParent ? quoteCallOfParent(globalObject->vm()) : quoteConstruction(globalObject->vm(), name);
    SetForScope quotedInScope(quotedSource, source.isNull() ? nullptr : &source);
    CallData constructData;
    constructData.type = CallData::Type::Native;
    constructData.native.function = function->nativeConstructor();
    constructData.native.isBoundFunction = false;
    constructData.native.isWasm = false;
    construct(globalObject, function, constructData, ArgList());
}

// The operation holds its text where that is short. Otherwise it stands in the text of the function, if the program has that.
static String quoteOfInstanceTest(JSGlobalObject* globalObject, StringView expression)
{
    return expression.isNull() ? findOnTop(globalObject->vm(), "instanceof"_s, findWrittenInstanceTests) : expression.toString();
}

JSObject* createInstanceofRightHandSideNotAFunctionError(JSGlobalObject* globalObject, JSValue constructor, StringView expression)
{
    return createQuoting(quoteOfInstanceTest(globalObject, expression), createInvalidInstanceofParameterErrorNotFunction, globalObject, constructor);
}

JSObject* createHasInstanceNotAFunctionError(JSGlobalObject* globalObject, JSValue constructor, StringView expression)
{
    return createQuoting(quoteOfInstanceTest(globalObject, expression), createInvalidInstanceofParameterErrorHasInstanceValueNotFunction, globalObject, constructor);
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, const Identifier& name)
{
    return createNotAFunctionError(globalObject, callee, name.isNull() || name.isSymbol() || name.isPrivateName() ? StringView() : StringView(name.impl()));
}

JSObject* createBuiltinModulesUnavailableError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("Built-in modules aren't available in this realm"_s));
}

JSObject* createNativeBindingsUnavailableError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("Native bindings aren't available in this realm"_s));
}

JSObject* createBuiltinWithoutPathError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, "Built-in has no path from the global object"_s);
}

JSObject* createBuiltinNotCompiledError(JSGlobalObject* globalObject, const String& builtin)
{
    return createError(globalObject, withAOTErrorNote(makeString(builtin, " was not compiled into this executable because your code never uses it. To keep it, use it somewhere in your code."_s)));
}

JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject* globalObject, const String& functionName)
{
    return createBuiltinNotCompiledError(globalObject, functionName.isEmpty() ? "A built-in function"_str : makeString("The built-in function '"_s, functionName, '\''));
}

JSObject* createBuiltinsNotCompiledError(JSGlobalObject* globalObject, std::span<const String> builtins)
{
    StringBuilder alternatives;
    for (size_t index = 0; index < builtins.size(); ++index) {
        if (index)
            alternatives.append(index + 1 == builtins.size() ? " or "_s : ", "_s);
        alternatives.append(builtins[index]);
    }
    return createError(globalObject, withAOTErrorNote(makeString("None of "_s, alternatives.toString(), " was compiled into this executable because your code never uses them. To keep them, use one of them somewhere in your code."_s)));
}

JSObject* createExternalModuleNotLoadedError(JSGlobalObject* globalObject, const String& path)
{
    return createTypeError(globalObject, withAOTErrorNote(makeString("The module \""_s, path, "\" is not loaded"_s)));
}

static JSObject* createErrorWithCode(JSGlobalObject* globalObject, const String& message, ASCIILiteral code)
{
    VM& vm = globalObject->vm();
    JSObject* error = createError(globalObject, message);
    error->putDirect(vm, Identifier::fromString(vm, "code"_s), jsNontrivialString(vm, String(code)));
    return error;
}

JSObject* createLoadByNameRefusedError(JSGlobalObject* globalObject, StringView name, ASCIILiteral code, StringView asker)
{
    // A module of Bun's is no file that could be another entry point. The call went through a value that the compiler did not follow to the loader.
    bool isModuleOfBun = name.startsWith("node:"_s) || name.startsWith("bun:"_s);
    const Image* image = Image::current();
    if (!isModuleOfBun && image && hasFlag(image->header().options.admitted(), AdmittedCode::FilesRunByHost))
        return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, '\'', asker.isEmpty() ? ""_s : " from '"_s, asker, asker.isEmpty() ? ""_s : "'"_s, ": it's compiled into this test run, but no test file imports it by a constant path, so only compiled code reaches it. Import it by a constant path in a test file."_s)), code);
    ASCIILiteral advice = isModuleOfBun ? "The compiler couldn't tell at build time that this call reaches a module loader. Call require(), import() or process.getBuiltinModule() directly."_s : "Pass the file to bun compile as another entry point."_s;
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. "_s, advice)), code);
}

JSObject* createExportClosedToOutsideError(JSGlobalObject* globalObject, StringView exportName, StringView module, StringView asker)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("Cannot read the export '"_s, exportName, "' of '"_s, module, '\'', asker.isEmpty() ? ""_s : " from '"_s, asker, asker.isEmpty() ? ""_s : "'"_s, ": the file is compiled into this test run, but no test file imports that name from it by a constant path, so only compiled code reaches it. Import the name in a test file."_s)));
}

JSObject* createModuleInOtherRealmError(JSGlobalObject* globalObject, StringView path)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time and can't be imported in another realm"_s)));
}

JSObject* createLoaderChangeRefusedError(JSGlobalObject* globalObject, LoaderChange change, StringView changed)
{
    // Only a test run takes files out, and only those that its build saw taken out.
    if (change == LoaderChange::TakeOutOfRequireCache)
        return createError(globalObject, makeString("Cannot take '"_s, changed, "' out of require.cache: the build found nothing that does, so the code that imports it was compiled for a module that loads once. Spell the path where it is deleted, in a file that the tests import: delete require.cache[require.resolve(\"./module\")]"_s));
    if (change == LoaderChange::StoreUnderNameOfModuleInRequireCache)
        return createError(globalObject, withAOTErrorNote(makeString("Cannot store an entry under '"_s, changed, "' in require.cache: the build read no store under that key, so require(\""_s, changed, "\") was compiled to answer with the module. Spell the key where it is stored: require.cache[\""_s, changed, "\"] = entry. In a test, mock.module(\""_s, changed, "\", () => ({ .. })) replaces the module too."_s)));
    bool isInRequireCache = change == LoaderChange::ReplaceInRequireCache;
    return createError(globalObject, withAOTErrorNote(makeString("Cannot replace "_s, isInRequireCache ? "'"_s : ""_s, changed, isInRequireCache ? "' in require.cache"_s : ""_s, ": imports in this executable were resolved at build time, so the module loader can't be patched."_s)));
}

JSObject* createModuleMissingAtBuildError(JSGlobalObject* globalObject, const String& specifier, const String& importer, ASCIILiteral code)
{
    return createErrorWithCode(globalObject, makeString("Cannot find module '"_s, specifier, "' from '"_s, importer, "'"_s), code);
}

JSObject* createExportStatementFindsNothingError(JSGlobalObject* globalObject, const String& exportName, const String& specifier)
{
    return createSyntaxError(globalObject, makeString("export '"_s, exportName, "' not found in '"_s, specifier, "'"_s));
}

JSObject* createExportStatementFindsSeveralError(JSGlobalObject* globalObject, const String& exportName, const String& specifier)
{
    return createSyntaxError(globalObject, makeString("Cannot export '"_s, exportName, "' multiple times in '"_s, specifier, "'"_s));
}

JSObject* createSuperCallInDirectEvalError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("super() cannot be called from direct eval in a compiled executable"_s));
}

static String messageOfFunctionNotCompiled(const String& description, const String& advice = "call it directly somewhere in your code"_s)
{
    return withAOTErrorNote(makeString(description, " was not compiled into this executable because nothing that the compiler can follow calls it. To keep it, "_s, advice, '.'));
}

String functionNotCompiledMessage(const String& functionName)
{
    return messageOfFunctionNotCompiled(makeString('\'', functionName, '\''));
}

String exportNotCompiledMessage(const String& functionName, const String& module)
{
    return messageOfFunctionNotCompiled(makeString('\'', functionName, '\''), makeString("import it by name: import { "_s, functionName, " } from \""_s, module, '"'));
}

String functionNotCompiledMessage(const String& path, unsigned line)
{
    return messageOfFunctionNotCompiled(makeString("The function at "_s, path, ':', line));
}

String functionNotCompiledMessage()
{
    return messageOfFunctionNotCompiled("This function"_s);
}

ASCIILiteral evaluationRefusedMessage()
{
    return "Code generation from strings disallowed for this context: the compiler couldn't tell at build time that this call reaches eval() or the Function constructor. Call eval() or new Function() directly."_s;
}

String memberOfLockedBuiltin(BuiltinID holder, UniquedStringImpl* key)
{
    String text = BuiltinTable::singleton().textOf(holder);
    if (key->isSymbol())
        return makeString(text, "[Symbol("_s, StringView(key), ")]"_s);
    if (parseIndex(*key))
        return makeString(text, '[', StringView(key), ']');
    return holder == Builtins::GlobalThis ? String(key) : makeString(text, '.', StringView(key));
}

// Fuzzilli's loop in Bun compares a compiled program with `bun run`, where these errors are never thrown, also when the program catches them.
static String printForFuzzilli(String&& message)
{
#if ENABLE(FUZZILLI)
    dataLogLn(message);
#endif
    return WTF::move(message);
}

String deletionFromLockedBuiltinMessage(const String& property)
{
    return printForFuzzilli(withAOTErrorNote(makeString("Cannot delete "_s, property, " because built-in objects are read-only in a compiled executable"_s)));
}

String assignmentToLockedBuiltinMessage(const String& property)
{
    return printForFuzzilli(withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable"_s)));
}

String assignmentThatBuildDidNotSeeMessage(const String& property)
{
    return printForFuzzilli(withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable, and the build didn't see your code assign to this one. Spell out the object and the name where it does, as in "_s, property, " = value"_s)));
}

static constexpr ASCIILiteral pageOnCompiledTests = " (https://bun.com/docs/compiler/debugging#run-your-tests-compiled)"_s;

static String refusalThatNoTestRunLifts(ASCIILiteral change, const String& member, ASCIILiteral does)
{
    return makeString(change, member, " because built-in objects are read-only in compiled code, and a test can't "_s, does, " this one: it is read without being looked up. Replace a function of your own that uses it"_s, pageOnCompiledTests);
}

static String refusalInTestRun(VM& vm, ASCIILiteral change, BuiltinID holder, UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    String member = memberOfLockedBuiltin(holder, key);
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    bool isText = !key->isSymbol() && !parseIndex(*key);
    // A member that is constant unless written is locked only where the build has followed no write to it.
    bool canBeOpened = found ? table.canBeReplacedInTestRuns(found) || table.at(found).mutability == BuiltinMutability::ConstantUnlessWritten : isText && table.canTakeNameInTestRuns(holder, name);
    ASCIILiteral does = found ? "replace"_s : "add"_s;
    if (!canBeOpened)
        return refusalThatNoTestRunLifts(change, member, does);
    String object = table.textOf(holder);
    String spelled = isText ? makeString(object, '.', StringView(key)) : makeString(object, '[', StringView(key), ']');
    String example = found
        ? makeString(spelled, " = f or spyOn("_s, object, ", "_s, isText ? "\""_s : ""_s, StringView(key), isText ? "\""_s : ""_s, ')')
        : makeString(spelled, " = value"_s);
    return makeString(change, member, " because built-in objects are read-only in compiled code, and the build didn't see a test "_s, does, " this one. Spell out the object and the name where the test does it, as in "_s, example, pageOnCompiledTests);
}

bool isWrittenByFileOfProduct(VM& vm)
{
    // Compiled code writes itself, or calls a built-in function that does. Code from disk writes for itself: no compiled frame is on top then.
    std::optional<bool> answer;
    forEachShownFrameOfRunAtTop(vm, [&](const TraceFrame& frame) {
        ImageOfTraceFrameScope imageOfFrame(frame);
        std::optional<SourcePosition> position = positionOf(frame);
        // Bun's own code writes for whoever has called it.
        if (position && position->source)
            answer = findsBuiltinsReadOnly(position->source);
        return answer ? IterationStatus::Done : IterationStatus::Continue;
    });
    return answer.value_or(false);
}

String assignmentToLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot assign to "_s, holder, key);
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    bool isNewName = !key->isSymbol() && !parseIndex(*key) && !found;
    bool isOpenWhereBuildSeesWrite = found ? table.at(found).mutability == BuiltinMutability::ConstantUnlessWritten : isNewName && table.canonicalID(holder) != Builtins::ObjectPrototype && table.canTakeNameInTestRuns(holder, name);
    if (isOpenWhereBuildSeesWrite)
        return assignmentThatBuildDidNotSeeMessage(memberOfLockedBuiltin(holder, key));
    return assignmentToLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
}

String deletionFromLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot delete "_s, holder, key);
    return deletionFromLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
}

String assignmentToFoldedMemberMessage(const String& property, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalThatNoTestRunLifts("Cannot assign to "_s, property, "replace"_s);
    return assignmentToLockedBuiltinMessage(property);
}

String deletionOfFoldedMemberMessage(const String& property, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalThatNoTestRunLifts("Cannot delete "_s, property, "replace"_s);
    return deletionFromLockedBuiltinMessage(property);
}

String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl* uid)
{
    return assignmentToLockedBuiltinMessage(makeString(uid->isSymbol() ? '[' : '\'', StringView(uid), uid->isSymbol() ? ']' : '\'', " of a built-in function"_s));
}

String prototypeChangeOfLockedBuiltinMessage(const String& holder, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in compiled code, and a test can't change this"_s, pageOnCompiledTests);
    return printForFuzzilli(withAOTErrorNote(makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in a compiled executable"_s)));
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
