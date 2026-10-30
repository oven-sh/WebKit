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
        const ClassTable& classes = State::of(prototype->structure()->realm()).objects->classes;
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

// The place of the bracket that `text[close]` closes. Knows texts in quotes and nothing else of the language, as the engine's functionCallBase(): where that
// is not enough the brackets do not pair up, or the answer is quoted as it stands in the source.
static std::optional<size_t> findOpeningBracket(std::span<const Latin1Character> text, size_t close)
{
    unsigned depth = 0;
    for (size_t at = close + 1; at--;) {
        Latin1Character character = text[at];
        if (character == '\n')
            return std::nullopt;
        if (isQuote(character)) {
            size_t opening = at;
            do {
                if (!opening || text[opening - 1] == '\n')
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
        if (character == '\n')
            return std::nullopt;
        if (isQuote(character)) {
            for (++at; at < text.size() && text[at] != character; ++at) {
                if (text[at] == '\n')
                    return std::nullopt;
                if (text[at] == '\\')
                    ++at;
            }
        } else if (character == '(' || character == '[')
            ++depth;
        else if ((character == ')' || character == ']') && !--depth)
            return at;
    }
    return std::nullopt;
}

// `a.b` of `a.b.c` and `make("x")[0]` of `make("x")[0].c`, where `end` is the place of the last dot or of its `?`: names, dots, and what stands in brackets behind them.
static std::optional<size_t> startOfPathInFrontOf(std::span<const Latin1Character> text, size_t end)
{
    size_t start = end;
    while (true) {
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
        // `(null).c`: Bun's transpiler prints what stands in brackets of its own in its own way.
        if (name == part)
            return std::nullopt;
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

enum class WrittenReads : uint8_t { None, Same, Differ };

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
        if (within && (*within < *start || *within >= at + member.length()))
            continue;
        std::span<const Latin1Character> read = text.subspan(*start, at + member.length() - *start);
        if (found.data() && !equalSpans(found, read))
            return WrittenReads::Differ;
        found = read;
    }
    return found.data() ? WrittenReads::Same : WrittenReads::None;
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
    ModuleID module;
    Vector<uint8_t> bytes;
    Vector<size_t> lineStarts;
};

static const SourceFile& sourceFileOf(ModuleID module)
{
    static thread_local SourceFile lastFile;
    if (lastFile.module == module)
        return lastFile;
    String path = sourceURLOf(module);
    bool isAbsolute = path.startsWith('/') || (path.length() > 2 && isASCIIAlpha(path[0]) && path[1] == ':');
    std::optional<Vector<uint8_t>> bytes = isAbsolute ? FileSystem::readEntireFile(path) : std::nullopt;
    lastFile = { module, bytes ? WTF::move(*bytes) : Vector<uint8_t>(), { } };
    for (size_t start = 0; start < lastFile.bytes.size(); start = startOfNextLine(byteCast<Latin1Character>(lastFile.bytes.span()), start))
        lastFile.lineStarts.append(start);
    return lastFile;
}

// `a!.b(c!)` is `a.b(c)` once the types are gone.
static String withoutNonNullMarks(std::span<const Latin1Character> text)
{
    Vector<Latin1Character> kept;
    Latin1Character quote = 0;
    for (size_t at = 0; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (quote) {
            if (character == '\\' && at + 1 < text.size())
                kept.append(text[at++]);
            else if (character == quote)
                quote = 0;
        } else if (isQuote(character))
            quote = character;
        else if (character == '!' && at && (isPartOfName(text[at - 1]) || text[at - 1] == ')' || text[at - 1] == ']') && (at + 1 == text.size() || text[at + 1] != '='))
            continue;
        kept.append(text[at]);
    }
    return String::fromUTF8(kept.span());
}

// What the function spells at the place. The text of the function is all there is to go by, so this answers only where the line of the place,
// or else the whole function, has one such place, or spells each of them alike.
static String findAt(const PlaceInFunctionText* place, StringView name, FindInText find)
{
    std::span<const Latin1Character> text = functionSourceBytesOfSource(place->function);
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
    if (reads == WrittenReads::Same)
        return String::fromUTF8(found);

    // The file says which line and column it is.
    if (!place->module || !place->position.line || !place->position.column)
        return { };
    const SourceFile& file = sourceFileOf(place->module);
    if (place->position.line > file.lineStarts.size())
        return { };
    text = byteCast<Latin1Character>(file.bytes.span());
    size_t lineStart = file.lineStarts[place->position.line - 1];
    size_t lineEnd = place->position.line < file.lineStarts.size() ? file.lineStarts[place->position.line] - 1 : text.size();
    if (lineStart >= lineEnd)
        return { };
    found = { };
    reads = find(text, lineStart, lineEnd, name, lineStart + place->position.column - 1, found);
    if (reads == WrittenReads::None)
        reads = find(text, lineStart, lineEnd, name, std::nullopt, found);
    return reads == WrittenReads::Same ? withoutNonNullMarks(found) : String();
}

// What the function on top spells at the place that failed. A place asks for one name in one way, so its text is looked through once, however often it throws.
static String findOnTop(VM& vm, StringView name, FindInText find)
{
    struct Answer {
        FindInText find { nullptr };
        String name;
        String quote;
    };
    static thread_local UncheckedKeyHashMap<const void*, Answer> answers;
    if (name.isEmpty() || !name.containsOnlyASCII())
        return { };
    std::optional<PlaceInFunctionText> place = placeOnTopInFunctionText(vm);
    if (!place || !place->address)
        return { };
    Answer& answer = answers.add(place->address, Answer { }).iterator->value;
    if (answer.find != find || StringView(answer.name) != name)
        answer = { find, name.toString(), findAt(&*place, name, find) };
    return answer.quote;
}

// Each place in `text[from, to)` that calls `name`, which is the callee as it is written or the last of its names. `found` holds the call with its arguments, and `new` in front of it.
static WrittenReads findWrittenCalls(std::span<const Latin1Character> text, size_t from, size_t to, StringView name, std::optional<size_t> within, std::span<const Latin1Character>& found)
{
    for (size_t at = from; at + name.length() + 1 < to; ++at) {
        size_t open = at + name.length() + (text[at + name.length()] == '!');
        if (text[open] != '(' || (at && (isPartOfName(text[at - 1]) || text[at - 1] == '#')))
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

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, StringView member)
{
    if (isWrittenBehindDot(member)) {
        if (String written = findOnTop(globalObject->vm(), member, findWrittenReads); !written.isNull())
            return createNotAnObjectErrorQuoting(written, globalObject, base);
    }
    String source;
    if (String description = describe(globalObject, base); !member.isNull() && !description.isNull())
        source = isWrittenBehindDot(member) ? tryMakeString(description, '.', member) : tryMakeString(description, "[\""_s, member, "\"]"_s);
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier& member)
{
    if (member.isPrivateName())
        return createNotAnObjectError(globalObject, base, StringView());
    if (!member.isSymbol())
        return createNotAnObjectError(globalObject, base, StringView(member.impl()));
    String source;
    if (String description = describe(globalObject, base); !description.isNull())
        source = tryMakeString(description, "[Symbol("_s, StringView(member.impl()), ")]"_s);
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
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

JSObject* createSuperBaseNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier* member)
{
    String source = !member || member->isSymbol() || member->isPrivateName() ? String() : tryMakeString("super."_s, StringView(member->impl()));
    return createNotAnObjectErrorQuoting(source, globalObject, base);
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = findOnTop(globalObject->vm(), name, findWrittenCalls);
    if (source.isNull() || source.startsWith("new "_s))
        source = name.isNull() ? String() : tryMakeString(name, "()"_s);
    return createQuoting(source, JSC::createNotAFunctionError, globalObject, callee);
}

static String quoteConstruction(VM& vm, StringView name)
{
    String source = findOnTop(vm, name, findWrittenCalls);
    // Bun's transpiler prints `new K()` as `new K`.
    if (source.isNull() || !source.startsWith("new "_s) || source.endsWith("()"_s))
        source = name.isNull() ? String() : tryMakeString("new "_s, name);
    return source;
}

JSObject* createNotAConstructorError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = quoteConstruction(globalObject->vm(), name);
    return createQuoting(source, JSC::createNotAConstructorError, globalObject, callee);
}

void throwNotAConstructorErrorInFrameOf(JSGlobalObject* globalObject, JSFunction* function, StringView name)
{
    String source = quoteConstruction(globalObject->vm(), name);
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

JSObject* createLoadByNameRefusedError(JSGlobalObject* globalObject, StringView name, ASCIILiteral code)
{
    // A module of Bun's is no file that could be another entry point. The call went through a value that the compiler did not follow to the loader.
    bool isModuleOfBun = name.startsWith("node:"_s) || name.startsWith("bun:"_s);
    ASCIILiteral advice = isModuleOfBun ? "The compiler couldn't tell at build time that this call reaches a module loader. Call require(), import() or process.getBuiltinModule() directly."_s : "Pass the file to bun compile as another entry point."_s;
    return createErrorWithCode(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. "_s, advice)), code);
}

JSObject* createModuleInOtherRealmError(JSGlobalObject* globalObject, StringView path)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time and can't be imported in another realm"_s)));
}

JSObject* createLoaderChangeRefusedError(JSGlobalObject* globalObject, LoaderChange change, StringView changed)
{
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

String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl* uid)
{
    return assignmentToLockedBuiltinMessage(makeString(uid->isSymbol() ? '[' : '\'', StringView(uid), uid->isSymbol() ? ']' : '\'', " of a built-in function"_s));
}

String prototypeChangeOfLockedBuiltinMessage(const String& holder)
{
    return printForFuzzilli(withAOTErrorNote(makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in a compiled executable"_s)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
