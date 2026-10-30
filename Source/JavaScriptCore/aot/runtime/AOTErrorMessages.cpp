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
#include "AOTVector.h"
#include "ArgList.h"
#include "ConstructData.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <wtf/FileSystem.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

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
        return initialNameProperty(FunctionID(classes.classWithIndex(lazyPrototype->classIndex())->classData().classConstructor));
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

String quotedSourceOfFailedExpression(VM& vm)
{
    if (quotedSource)
        return *quotedSource;
    if (!vm.hasCompiledCode())
        return { };
    StringView constructor = nameOfConstructorThatRunsOnTop(vm);
    return constructor.isNull() ? String() : tryMakeString("new "_s, constructor);
}

static JSObject* createQuoting(const String& source, JSObject* (*create)(JSGlobalObject*, JSValue), JSGlobalObject* globalObject, JSValue value)
{
    SetForScope quotedInScope(quotedSource, source.isNull() ? nullptr : &source);
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

// Where the code of the line in front of the line break `text[lineBreak]` ends: in front of a `//` comment and of the blanks at the end.
static size_t endOfCodeInFrontOf(std::span<const Latin1Character> text, size_t lineBreak)
{
    size_t start = lineBreak;
    while (start && text[start - 1] != '\n')
        --start;
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
        else if (character == '(' || character == '[')
            ++depth;
        else if ((character == ')' || character == ']') && !--depth)
            return at;
    }
    return std::nullopt;
}

// `a.b` of `a.b.c` and `make("x")[0]` of `make("x")[0].c`, where `end` is the place of the last dot or of its `?`: names, dots, and what stands in brackets behind them.
// Round brackets may stand in front of it all: `(a as T)` of `(a as T).c`, `(a + b)` of `(a + b).c`.
static std::optional<size_t> startOfPathInFrontOf(std::span<const Latin1Character> text, size_t end)
{
    size_t start = end;
    while (true) {
        // A chain: the dot is the first thing in its line, and lines that are comments may stand between.
        size_t code = start;
        while (code && (text[code - 1] == ' ' || text[code - 1] == '\t'))
            --code;
        while (code && text[code - 1] == '\n')
            code = endOfCodeInFrontOf(text, code - 1);
        if (code != start && code && text[code - 1] != '\n' && start < text.size() && (text[start] == '.' || text[start] == '?'))
            start = code;
        // `a!.b`, in a file that still has its types.
        size_t part = start - (start && text[start - 1] == '!');
        while (part && (text[part - 1] == ')' || text[part - 1] == ']')) {
            std::optional<size_t> opening = findOpeningBracket(text, part - 1);
            if (!opening)
                return std::nullopt;
            part = *opening;
            if (part >= 2 && text[part - 1] == '.' && text[part - 2] == '?')
                part -= 2;
            part -= part && text[part - 1] == '!';
        }
        size_t name = part;
        while (name && isPartOfName(text[name - 1]))
            --name;
        if (name == part) {
            if (part != start && text[part] == '(')
                return part;
            return std::nullopt;
        }
        if (name && text[name - 1] == '#')
            --name;
        start = name;
        if (!start || text[start - 1] != '.')
            return start;
        size_t before = start - 1;
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
        } else
            break;
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

enum class WrittenReads : uint8_t { None, Same, Differ };

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
static WrittenReads findWrittenReads(std::span<const Latin1Character> text, size_t from, size_t to, StringView member, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    for (size_t at = from + 1; at + member.length() <= to; ++at) {
        if (text[at - 1] != '.' || (at + member.length() < text.size() && isPartOfName(text[at + member.length()])))
            continue;
        if (!equal(member, text.subspan(at, member.length())))
            continue;
        size_t dot = at - 1;
        if (dot && text[dot - 1] == '?')
            --dot;
        std::optional<size_t> start = startOfPathInFrontOf(text, dot);
        if (!start)
            return WrittenReads::Differ;
        start = startWithNew(text, *start, dot);
        if (within && (*within < *start || *within >= at + member.length()))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, endWithStoredValue(text, at + member.length()) - *start);
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// Each place in `text[from, to)` that reads an element: `a.b[k]`. The place of such a read is that of `a.b`, so of several that hold it the first is meant.
// `name` is "[]" where the key is computed as the program runs. Otherwise the compiler knows the key, as it may where the source has `a[k]`. Then `bun run` may know
// it too and print the name, which `bun test` does not: so that is quoted only where a file says which place it is, as in a test run.
static WrittenReads findWrittenElements(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    bool isComputed = name == "[]"_s;
    if (!isComputed && !within)
        return WrittenReads::None;
    for (size_t at = from + 1; at < to; ++at) {
        if (isQuote(text[at])) {
            Latin1Character quote = text[at];
            for (++at; at < to && text[at] != quote; ++at)
                at += text[at] == '\\';
            continue;
        }
        if (text[at] != '[' || !(isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']' || text[at - 1] == '!'))
            continue;
        std::optional<size_t> start = startOfPathInFrontOf(text, at);
        std::optional<size_t> close = findClosingBracket(text, at);
        if (!start || !close || *close >= to) {
            if (within)
                continue;
            return WrittenReads::Differ;
        }
        start = startWithNew(text, *start, at);
        if (within && (*within < *start || *within >= at))
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
    for (size_t at = from + 1; at + 1 < to; ++at) {
        if (text[at - 1] != '.' || text[at] != '#')
            continue;
        size_t end = at + 1;
        while (end < to && isPartOfName(text[end]))
            ++end;
        size_t dot = at - 1;
        if (dot && text[dot - 1] == '?')
            --dot;
        std::optional<size_t> start = startOfPathInFrontOf(text, dot);
        if (!start)
            return WrittenReads::Differ;
        if (within && (*within < *start || *within >= end))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, endWithStoredValue(text, end) - *start);
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

// `x of v` of `for (const x of v)`, and `[a, b]` of `const [a, b] = v`. The place is that of `x`, or stands in front of the list. Only a file says where that is.
static WrittenReads findWrittenIterations(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    if (!within)
        return WrittenReads::None;
    for (size_t at = from; at + 4 < *within; ++at) {
        if (text[at] != 'f' || text[at + 1] != 'o' || text[at + 2] != 'r' || text[at + 3] != ' ' || (at > from && isPartOfName(text[at - 1])))
            continue;
        size_t open = WTF::find(text, static_cast<Latin1Character>('('), at);
        std::optional<size_t> close = open != notFound && open < *within ? findClosingBracket(text, open) : std::nullopt;
        if (!close || *close <= *within || *close >= to)
            continue;
        for (size_t word = *within; word + 4 < *close; ++word) {
            if (text[word] == ' ' && text[word + 1] == 'o' && text[word + 2] == 'f' && text[word + 3] == ' ') {
                found = text.subspan(*within, *close - *within);
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
        // Of a list in a list either may be the one.
        if (!close || *close >= to || WTF::find(text.subspan(at + 1, *close - at - 1), static_cast<Latin1Character>('[')) != notFound)
            return WrittenReads::None;
        size_t behind = *close + 1;
        while (behind < to && text[behind] == ' ')
            ++behind;
        bool takesValues = behind < to && ((text[behind] == '=' && (behind + 1 == to || (text[behind + 1] != '=' && text[behind + 1] != '>'))) || text[behind] == ':' || text[behind] == ',' || text[behind] == ')' || text[behind] == '}');
        if (!takesValues)
            return WrittenReads::None;
        found = text.subspan(at, *close + 1 - at);
        return WrittenReads::Same;
    }
    return WrittenReads::None;
}

// Each place in `text[from, to)` that asks whether an object has a key: `k in a.b`. What `for (k in a)` walks may be anything, and `#k in a` is not quoted.
static WrittenReads findWrittenKeyTests(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    auto isBlank = [](Latin1Character character) {
        return character == ' ' || character == '\n' || character == '\r' || character == '\t';
    };
    for (size_t at = from + 1; at + 3 < to; ++at) {
        if (isQuote(text[at])) {
            at = skipQuoted(text, at, to);
            continue;
        }
        if (text[at] != ' ' || text[at + 1] != 'i' || text[at + 2] != 'n' || !isBlank(text[at + 3]))
            continue;
        size_t endOfKey = at;
        while (endOfKey > from && text[endOfKey - 1] == ' ')
            --endOfKey;
        std::optional<size_t> start;
        if (endOfKey > from && isQuote(text[endOfKey - 1])) {
            for (size_t opening = endOfKey - 1; opening > from && !start; --opening) {
                if (text[opening - 1] == text[endOfKey - 1] && (opening - 1 == from || text[opening - 2] != '\\'))
                    start = opening - 1;
            }
        } else
            start = startOfPathInFrontOf(text, endOfKey);
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
            || (before == '>' && inFront >= from + 2 && text[inFront - 2] == '=') || equalSpans(word, byteCast<Latin1Character>("return"_span));
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

// Each place in `text[from, to)` that deletes a member: `delete a.b[k]`.
static WrittenReads findWrittenDeletions(std::span<const Latin1Character> text, size_t from, size_t to, StringView, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    constexpr auto keyword = "delete "_span;
    for (size_t at = from; at + keyword.size() < to; ++at) {
        if (isQuote(text[at])) {
            at = skipQuoted(text, at, to);
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
        if (isQuote(text[at])) {
            at = skipQuoted(text, at, to);
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

static const SourceFile* sourceFileOf(VM& vm, ModuleID moduleOfImage)
{
    QuotesOfPlaces& quotes = quotesOfPlaces(vm);
    SourceFile& lastFile = quotes.lastFile;
    // Never 0, which no file has and the set cannot hold.
    uint64_t module = (static_cast<uint64_t>(Image::current()->number()) + 1) << 32 | moduleOfImage.index();
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
    lastFile = { module, WTF::move(*bytes), { } };
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
        bool isBehindValue = at && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']' || isQuote(text[at - 1]));
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
    if (text[end - 1] == ')' || text[end - 1] == ']')
        return false;
    if (!isPartOfName(text[end - 1]))
        return true;
    if (end == open)
        return false;
    size_t start = end;
    while (start && isPartOfName(text[start - 1]))
        --start;
    std::span word = text.subspan(start, end - start);
    for (ASCIILiteral keyword : { "new"_s, "return"_s, "typeof"_s, "await"_s, "void"_s, "delete"_s, "yield"_s, "throw"_s, "case"_s, "in"_s, "of"_s }) {
        if (equalSpans(word, byteCast<Latin1Character>(keyword.span())))
            return true;
    }
    return false;
}

// `(a).b` is printed `a.b`, and `(a + b).c` as it is: Bun prints round brackets only where the meaning hangs on them.
static void appendWithoutIdleBrackets(Vector<Latin1Character>& printed, std::span<const Latin1Character> text)
{
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isQuote(character)) {
            size_t end = at + 1;
            while (end < text.size() && text[end] != character)
                end += 1 + (text[end] == '\\');
            end = std::min(end, text.size() - 1);
            // Bun prints a text in double quotes where that leaves nothing to escape.
            // The last character may be a quote that nothing closes.
            std::span<const Latin1Character> inside = text.subspan(at + 1, end > at ? end - at - 1 : 0);
            bool hasNothingToEscape = end > at && text[end] == character && std::ranges::none_of(inside, [](Latin1Character character) {
                return isQuote(character) || character == '\\' || character == '$';
            });
            if (hasNothingToEscape) {
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
            bool isPath = !inside.empty() && !isASCIIDigit(inside.front()) && startOfPathInFrontOf(inside, inside.size()) == std::optional<size_t>(0);
            // `(a?.b).c` stops elsewhere than `a?.b.c`, and `new (f()).K()` constructs something else than `new f().K()`.
            if (isPath && WTF::find(inside, static_cast<Latin1Character>('?')) == notFound && !(isConstructed && WTF::find(inside, static_cast<Latin1Character>('(')) != notFound)) {
                appendWithoutIdleBrackets(printed, inside);
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
            else if (behind != '.' && !(behind == '?' && next + 1 < text.size() && text[next + 1] == '.') && !closes && inFront != '(' && inFront != '[')
                kept.append(' ');
            at = next - 1;
            continue;
        }
        kept.append(text[at]);
    }
    return kept;
}

static String asPrinted(std::span<const Latin1Character> text)
{
    Vector<Latin1Character> printed;
    appendWithoutIdleBrackets(printed, withoutTypes(onOneLine(text).span()).span());
    return String::fromUTF8(printed.span());
}

// A column counts units of UTF-16, and the file is UTF-8.
static size_t offsetOfColumn(std::span<const Latin1Character> text, size_t lineStart, size_t lineEnd, unsigned column)
{
    size_t at = lineStart;
    for (unsigned units = 1; units < column && at < lineEnd;) {
        Latin1Character lead = text[at];
        size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        units += length == 4 ? 2 : 1;
        at += length;
    }
    return std::min(at, lineEnd);
}

// What the function spells at the place.
static String findSpelled(VM& vm, const PlaceInFunctionText* place, StringView name, FindInText find)
{
    std::span<const Latin1Character> found;
    // A file says which line and column it is, so only what holds that place counts.
    if (place->module && place->position.line && place->position.column) {
        const SourceFile* file = sourceFileOf(vm, place->module);
        if (file && place->position.line <= file->lineStarts.size()) {
            std::span<const Latin1Character> text = byteCast<Latin1Character>(file->bytes.span());
            size_t lineStart = file->lineStarts[place->position.line - 1];
            size_t lineEnd = place->position.line < file->lineStarts.size() ? file->lineStarts[place->position.line] - 1 : text.size();
            if (lineStart < lineEnd && find(text, lineStart, lineEnd, name, offsetOfColumn(text, lineStart, lineEnd, place->position.column), found) == WrittenReads::Same)
                return asPrinted(found);
            found = { };
        }
    }

    // The text of the function is all there is to go by. So this answers only where the line of the place, or else the whole function, has one such place, or spells each of them alike.
    std::span<const Latin1Character> text = functionSourceBytesOfSource(place->function);
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
    return reads == WrittenReads::Same ? asPrinted(found) : String();
}

// Each place in `text[from, to)` that calls `name`, which is the callee as it is written or the last of its names. `found` holds the call with its arguments, and `new` in front of it.
static WrittenReads findWrittenCalls(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    for (size_t at = from; at + name.length() < to; ++at) {
        size_t open = at + name.length() + (text[at + name.length()] == '!');
        if (open >= to || text[open] != '(' || (at && (isPartOfName(text[at - 1]) || text[at - 1] == '#')))
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
        std::optional<size_t> close = findClosingBracket(text, open);
        if (!close)
            return WrittenReads::Differ;
        if (start >= 4 && text[start - 4] == 'n' && text[start - 3] == 'e' && text[start - 2] == 'w' && text[start - 1] == ' ' && (start == 4 || !isPartOfName(text[start - 5])))
            start -= 4;
        if (within && (*within < start || *within > *close))
            continue;
        std::span<const Latin1Character> call = text.subspan(start, *close + 1 - start);
        if (found.data() && !equalSpans(found, call))
            return WrittenReads::Differ;
        found = call;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
}

static String findAt(VM& vm, const PlaceInFunctionText* place, StringView name, FindInText find, bool& holdsForEveryName)
{
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
    size_t dot = name.reverseFind('.');
    if (!quote.isNull() || find != findWrittenCalls || dot == notFound)
        return quote;
    // `(api as T).run()` calls `api.run`. Another `.run()` of the line does not.
    quote = findSpelled(vm, place, name.substring(dot + 1), find);
    StringView callee = quote;
    if (callee.startsWith("new "_s))
        callee = callee.substring(4);
    return callee.length() > name.length() && callee.startsWith(name) && callee[name.length()] == '(' ? quote : String();
}

// What the function on top spells at the place that failed. A place asks for one name in one way, so its text is looked through once, however often it throws.
static String findOnTop(VM& vm, StringView name, FindInText find)
{
    if (name.isEmpty() || !name.containsOnlyASCII())
        return { };
    std::optional<PlaceInFunctionText> place = placeOnTopInFunctionText(vm);
    if (!place || !place->address)
        return { };
    QuotesOfPlaces::Answer& answer = quotesOfPlaces(vm).answers.add(place->address, QuotesOfPlaces::Answer { }).iterator->value;
    if (answer.find != find || (!answer.holdsForEveryName && StringView(answer.name) != name)) {
        bool holdsForEveryName = find == findWrittenElements;
        String quote = findAt(vm, &*place, name, find, holdsForEveryName);
        answer = { find, name.toString(), WTF::move(quote), holdsForEveryName };
    }
    return answer.quote;
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
    std::span<const Latin1Character> text = source.span8();
    std::optional<size_t> open = text.back() == ')' ? findOpeningBracket(text, text.size() - 1) : std::nullopt;
    return !open || WTF::find(text.first(*open), static_cast<Latin1Character>('(')) == notFound;
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = name.isEmpty() ? findOnTop(globalObject->vm(), "[]"_s, findWrittenCallsOfElements) : findOnTop(globalObject->vm(), name, findWrittenCalls);
    if (source.isNull() || isConstruction(source))
        source = name.isNull() ? String() : tryMakeString(name, "()"_s);
    return createQuoting(source, JSC::createNotAFunctionError, globalObject, callee);
}

static String quoteConstruction(VM& vm, StringView name)
{
    String source = findOnTop(vm, name, findWrittenCalls);
    // Bun's transpiler prints `new K()` as `new K`.
    if (source.isNull() || !isConstruction(source) || source.endsWith("()"_s))
        source = name.isNull() ? String() : tryMakeString("new "_s, name);
    return source;
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

JSObject* createInstanceofRightHandSideNotAFunctionError(JSGlobalObject* globalObject, JSValue constructor, StringView expression)
{
    return createQuoting(expression.isNull() ? String() : expression.toString(), createInvalidInstanceofParameterErrorNotFunction, globalObject, constructor);
}

JSObject* createHasInstanceNotAFunctionError(JSGlobalObject* globalObject, JSValue constructor, StringView expression)
{
    return createQuoting(expression.isNull() ? String() : expression.toString(), createInvalidInstanceofParameterErrorHasInstanceValueNotFunction, globalObject, constructor);
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

static String messageOfFunctionNotCompiled(const String& description)
{
    return withAOTErrorNote(makeString(description, " was not compiled into this executable because your code never calls it. To keep it, call it somewhere in your code."_s));
}

String functionNotCompiledMessage(const String& functionName)
{
    return messageOfFunctionNotCompiled(makeString('\'', functionName, '\''));
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

static constexpr ASCIILiteral pageOnCompiledTests = " (https://bun.com/docs/compiler/debugging#run-your-tests-compiled)"_s;

static String refusalInTestRun(VM& vm, ASCIILiteral change, BuiltinID holder, UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    String member = memberOfLockedBuiltin(holder, key);
    BuiltinName name = table.nameOf(vm, key);
    BuiltinID found = name ? table.findOwnMember(holder, name) : BuiltinID();
    bool isText = !key->isSymbol() && !parseIndex(*key);
    bool canBeOpened = found ? table.canBeReplacedInTestRuns(found) : isText && table.canTakeNameInTestRuns(holder, name);
    ASCIILiteral does = found ? "replace"_s : "add"_s;
    if (!canBeOpened)
        return makeString(change, member, " because built-in objects are read-only in compiled code, and a test can't "_s, does, " this one: it is read without being looked up. Replace a function of your own that uses it"_s, pageOnCompiledTests);
    String object = table.textOf(holder);
    String spelled = isText ? makeString(object, '.', StringView(key)) : makeString(object, '[', StringView(key), ']');
    String example = found
        ? makeString(spelled, " = f or spyOn("_s, object, ", "_s, isText ? "\""_s : ""_s, StringView(key), isText ? "\""_s : ""_s, ')')
        : makeString(spelled, " = value"_s);
    return makeString(change, member, " because built-in objects are read-only in compiled code, and the build didn't see a test "_s, does, " this one. Spell out the object and the name where the test does it, as in "_s, example, pageOnCompiledTests);
}

String assignmentToLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot assign to "_s, holder, key);
    return assignmentToLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
}

String deletionFromLockedBuiltinMessage(VM& vm, BuiltinID holder, UniquedStringImpl* key, LockedIn lockedIn)
{
    if (lockedIn == LockedIn::TestRun)
        return refusalInTestRun(vm, "Cannot delete "_s, holder, key);
    return deletionFromLockedBuiltinMessage(memberOfLockedBuiltin(holder, key));
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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
