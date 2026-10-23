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
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeType.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <wtf/NeverDestroyed.h>
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

static bool hasPlainPrototype(JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
}

static bool isNativeObject(JSObject* object)
{
    return object->type() != FinalObjectType && !object->isCallable() && !dynamicDowncast<JSArray>(object) && !isCompiledObject(object) && !dynamicDowncast<ClassPrototype>(object);
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

String describe(JSValue value, ValueDetail detail)
{
    bool describesKindOnly = detail == ValueDetail::KindOnly;
    if (!value)
        return "nothing"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean() && describesKindOnly)
        return "boolean"_s;
    if (value.isBoolean())
        return value.asBoolean() ? "boolean true"_s : "boolean false"_s;
    if (value.isNumber() && describesKindOnly)
        return "number"_s;
    if (value.isNumber())
        return makeString("number "_s, value.asNumber());
    if (value.isString()) {
        static constexpr unsigned maxLength = 40;
        StringImpl* text = describesKindOnly ? nullptr : asString(value)->tryGetValueImpl();
        if (!text)
            return "string"_s;
        return makeString("string \""_s, StringView { *text }.left(maxLength), text->length() > maxLength ? "...\""_s : "\""_s);
    }
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
    if (isNativeObject(object))
        return makeString("built-in "_s, className(object));
    return hasPlainPrototype(object) ? "untyped object"_str : makeString("untyped "_s, className(object));
}

static void appendUnionMember(StringBuilder& result, bool has, StringView name)
{
    if (has)
        result.append(result.isEmpty() ? ""_s : " | "_s, name);
}

static uint32_t withoutImplicitNullish(uint32_t primitives, bool hasOtherMembers)
{
    bool hasBoth = (primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull;
    return hasBoth && (hasOtherMembers || primitives & ~Primitive::UndefinedOrNull) ? primitives & ~Primitive::UndefinedOrNull : primitives;
}

static void appendPrimitives(StringBuilder& result, uint32_t primitives)
{
    primitives = withoutImplicitNullish(primitives, !result.isEmpty());
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

static const LayoutData* memberLayoutForShape(const ProgramTables& tables, const ShapeTestData& members, uint32_t shape)
{
    if (!tables.hasLayoutForShape(shape) || !tables.passes(members, shape))
        return nullptr;
    return &tables.layoutForShape(shape);
}

static String describeMembers(const ProgramTables& tables, const ShapeTestData& members)
{
    static constexpr unsigned maxNamesShown = 4;
    Vector<String, maxNamesShown> names;
    StringBuilder result;
    for (uint32_t shape = members.first(); shape < members.first() + members.count; ++shape) {
        const LayoutData* layout = memberLayoutForShape(tables, members, shape);
        if (!layout)
            continue;
        String name = nameOf(tables, *layout);
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

namespace {

struct DescribedType {
    uint32_t primitives { 0 };
    bool allowsAnyObject { false };
    bool allowsAnyFunction { false };
    std::optional<std::pair<int64_t, int64_t>> integerRange;
    Vector<String, 2> layoutSetNames;
    Vector<String, 2> cellNames;
    String elements;
    Vector<String, 2> literalStrings;
};

} // namespace

static String describe(const DescribedType& type)
{
    static constexpr uint32_t allValues = Primitive::Boolean | Primitive::Number | Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull;
    static constexpr size_t maxLiteralsShown = 4;
    if (type.allowsAnyObject && (type.primitives & allValues) == allValues)
        return "any"_s;
    StringBuilder result;
    for (auto& name : type.layoutSetNames)
        appendUnionMember(result, true, name);
    for (auto& name : type.cellNames)
        appendUnionMember(result, true, name);
    appendUnionMember(result, type.allowsAnyObject, "object"_s);
    appendUnionMember(result, type.allowsAnyFunction && !type.allowsAnyObject, "Function"_s);
    for (size_t i = 0; i < std::min(type.literalStrings.size(), maxLiteralsShown); ++i)
        appendUnionMember(result, true, makeString('"', type.literalStrings[i], '"'));
    appendUnionMember(result, type.literalStrings.size() > maxLiteralsShown, "..."_s);
    uint32_t primitives = type.primitives & Primitive::All;
    if (!type.literalStrings.isEmpty())
        primitives &= ~Primitive::String;
    auto range = type.integerRange;
    if ((primitives & Primitive::Number) == Primitive::Integer && range && range->first >= -Typed::maxExactInteger && range->second <= Typed::maxExactInteger) {
        appendUnionMember(result, true, range->first == range->second ? makeString(range->first) : makeString("integer "_s, range->first, ".."_s, range->second));
        appendPrimitives(result, primitives & ~Primitive::Number);
    } else
        appendPrimitives(result, primitives);
    return result.toString();
}

static void appendUnique(Vector<String, 2>& names, String&& name)
{
    if (!names.contains(name))
        names.append(WTF::move(name));
}

static void appendClassesWithCellType(DescribedType& described, JSType cellType)
{
    if (cellType != ArrayBufferType) {
        appendUnique(described.cellNames, builtinClassName(cellType, described.elements));
        return;
    }
    for (ArrayBufferSharingMode sharingMode : { ArrayBufferSharingMode::Default, ArrayBufferSharingMode::Shared })
        appendUnique(described.cellNames, arrayBufferSharingModeName(sharingMode));
}

static void appendCellTest(DescribedType& described, const ProgramTables& tables, const RuntimeType& type, const RuntimeCellTest& test)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        if (auto set = type.layoutSet())
            appendUnique(described.layoutSetNames, nameOf(tables, tables.layoutSets()[*set]));
        else
            described.allowsAnyObject = true;
        return;
    case CellTestKind::IsCallable:
        described.allowsAnyFunction = true;
        return;
    case CellTestKind::CellTypeIs: {
        JSTypeRange cellTypes = test.cellTypes();
        for (unsigned cellType = cellTypes.first; cellType <= cellTypes.last; ++cellType)
            appendClassesWithCellType(described, static_cast<JSType>(cellType));
        return;
    }
    case CellTestKind::ShapeIn:
        if (auto set = type.layoutSet())
            appendUnique(described.layoutSetNames, nameOf(tables, tables.layoutSets()[*set]));
        else
            appendUnique(described.layoutSetNames, describeMembers(tables, tables.shapeTestAt(test.shapeTestOffset())));
        return;
    case CellTestKind::InheritsClassInfo:
    case CellTestKind::IsPristine:
        appendUnique(described.cellNames, builtinClassName(BuiltinTable::singleton().at(test.builtinClass()), described.elements));
        return;
    case CellTestKind::IsBuiltinCell:
        appendUnique(described.cellNames, builtinCellName(tables, test.builtinCellNumber()));
        return;
    case CellTestKind::IsClosureOf:
        if (String name = initialNameProperty(test.function()); !name.isEmpty())
            appendUnique(described.cellNames, makeString("typeof "_s, name));
        else
            described.allowsAnyFunction = true;
        return;
    case CellTestKind::IsSymbol:
        if (StringID description = ProgramStrings::identifierString(tables, test.symbolKeyNumber()); hasNonEmptyText(description))
            appendUnique(described.cellNames, makeString("typeof "_s, ProgramStrings::text(tables, description)));
        else
            described.primitives |= Primitive::Symbol;
        return;
    case CellTestKind::SharingModeIs:
        appendUnique(described.cellNames, arrayBufferSharingModeName(test.sharingMode()));
        return;
    case CellTestKind::IsNamespaceOf:
        appendUnique(described.cellNames, namespaceName(tables, test.module()));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DescribedType describedTypeOf(const ProgramTables& tables, const RuntimeType& type)
{
    DescribedType described;
    described.primitives = type.primitiveSet().toRaw();
    described.integerRange = type.integerRange();
    if (auto offset = type.contents())
        described.elements = describe(tables, tables.typeAt(*offset));
    for (const RuntimeCellTest& test : type.cellTests())
        appendCellTest(described, tables, type, test);
    for (uint32_t string : type.literalStrings())
        described.literalStrings.append(ProgramStrings::text(tables, StringID(string)));
    return described;
}

String describe(const ProgramTables& tables, const RuntimeType& type)
{
    return describe(describedTypeOf(tables, type));
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

// The test of a cast asks for a family, which lists every layout the program converts to the type behind `as`. The text of the site is that type.
static bool isAssertion(const TypeSiteData& site)
{
    return static_cast<TypeSiteKind>(site.kind) == TypeSiteKind::Assertion;
}

String describe(const ProgramTables& tables, const RuntimeType& type, Typed::TypeSiteID site)
{
    const TypeSiteData* data = site ? tables.findTypeSite(site.index()) : nullptr;
    if (!data || data->text == TypeSiteData::noText || (!isAssertion(*data) && !namesLayoutSet(tables, type) && !acceptsAnyObject(tables, type)))
        return describe(tables, type);
    return ProgramStrings::text(tables, StringID(data->text));
}

static ASCIILiteral adviceToKeepFunctionSource()
{
    return Image::current()->header().options.adviceToKeepFunctionSource();
}

static thread_local const String* quotedSource;

StringView quotedSourceOfFailedExpression()
{
    return quotedSource ? StringView(*quotedSource) : StringView();
}

static JSObject* createQuoting(const String& source, NOESCAPE const Invocable<JSObject*()> auto& create)
{
    SetForScope quotedInScope(quotedSource, source.isNull() ? nullptr : &source);
    return create();
}

// As the engine's message describes the value. Null where it cannot be described.
static String describe(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    String description = errorDescriptionForValue(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return { };
    }
    return description;
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, StringView member)
{
    String source;
    if (String description = describe(globalObject, base); !member.isNull() && !description.isNull())
        source = tryMakeString(description, '.', member);
    return createQuoting(source, [&] {
        return JSC::createNotAnObjectError(globalObject, base);
    });
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier& member)
{
    return createNotAnObjectError(globalObject, base, member.isSymbol() || member.isPrivateName() ? StringView() : StringView(member.impl()));
}

JSObject* createNotAnObjectError(JSGlobalObject* globalObject, JSValue base, JSValue key)
{
    String source;
    if (key.isInt32() || (key.isString() && !asString(key)->isRope())) {
        String description = describe(globalObject, base);
        String descriptionOfKey = describe(globalObject, key);
        if (!description.isNull() && !descriptionOfKey.isNull())
            source = tryMakeString(description, '[', descriptionOfKey, ']');
    }
    return createQuoting(source, [&] {
        return JSC::createNotAnObjectError(globalObject, base);
    });
}

JSObject* createSuperBaseNotAnObjectError(JSGlobalObject* globalObject, JSValue base, const Identifier& member)
{
    String source = member.isSymbol() || member.isPrivateName() ? String() : tryMakeString("super."_s, StringView(member.impl()));
    return createQuoting(source, [&] {
        return JSC::createNotAnObjectError(globalObject, base);
    });
}

JSObject* createNotAFunctionError(JSGlobalObject* globalObject, JSValue callee, StringView name)
{
    String source = name.isNull() ? String() : tryMakeString(name, "()"_s);
    return createQuoting(source, [&] {
        return JSC::createNotAFunctionError(globalObject, callee);
    });
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

static JSObject* createNotCompiledError(JSGlobalObject* globalObject, const String& message)
{
    return createError(globalObject, withAOTErrorNote(makeString(message, adviceToKeepFunctionSource())));
}

JSObject* createBuiltinNotCompiledError(JSGlobalObject* globalObject, const String& builtin)
{
    return createNotCompiledError(globalObject, makeString(builtin, " was not compiled into this executable because your code never uses it. To keep it, use it somewhere in your code."_s));
}

JSObject* createBuiltinFunctionNotCompiledError(JSGlobalObject* globalObject, const String& functionName)
{
    return createNotCompiledError(globalObject, makeString("The built-in function '"_s, functionName, "' was not compiled into this executable because your code never uses it. To keep it, use it somewhere in your code."_s));
}

JSObject* createBuiltinsNotCompiledError(JSGlobalObject* globalObject, std::span<const String> builtins)
{
    StringBuilder alternatives;
    for (size_t index = 0; index < builtins.size(); ++index) {
        if (index)
            alternatives.append(index + 1 == builtins.size() ? " or "_s : ", "_s);
        alternatives.append(builtins[index]);
    }
    return createNotCompiledError(globalObject, makeString("None of "_s, alternatives.toString(), " was compiled into this executable because your code never uses them. To keep them, use one of them somewhere in your code."_s));
}

JSObject* createExternalModuleNotLoadedError(JSGlobalObject* globalObject, const String& path)
{
    return createTypeError(globalObject, withAOTErrorNote(makeString("The module \""_s, path, "\" is not loaded"_s)));
}

static JSObject* createLoadByNameRefusedErrorWithoutCode(JSGlobalObject* globalObject, StringView name)
{
    // A module of Bun's is no file that could be another entry point. The call went through a value that the compiler did not follow to the loader.
    if (name.startsWith("node:"_s) || name.startsWith("bun:"_s))
        return createError(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. The compiler couldn't tell at build time that this call reaches a module loader. Call require(), import() or process.getBuiltinModule() directly."_s)));
    return createError(globalObject, withAOTErrorNote(makeString("Cannot load '"_s, name, "': this executable can't load a module from a dynamic path. Pass the file to bun compile as another entry point."_s, adviceToKeepFunctionSource())));
}

JSObject* createLoadByNameRefusedError(JSGlobalObject* globalObject, StringView name, ASCIILiteral code)
{
    VM& vm = globalObject->vm();
    JSObject* error = createLoadByNameRefusedErrorWithoutCode(globalObject, name);
    error->putDirect(vm, Identifier::fromString(vm, "code"_s), jsNontrivialString(vm, String(code)));
    return error;
}

JSObject* createModuleInShadowRealmError(JSGlobalObject* globalObject, StringView path)
{
    return createSyntaxError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time and can't be imported inside a ShadowRealm"_s)));
}

JSObject* createForeignModuleObjectsError(JSGlobalObject* globalObject, StringView path)
{
    return createTypeError(globalObject, withAOTErrorNote(makeString("The module "_s, path, " was compiled ahead of time, so it only runs with the module and exports objects the module loader created for it"_s)));
}

static String describe(LoaderChange change, StringView changed)
{
    switch (change) {
    case LoaderChange::ReplaceInRequireCache:
        return makeString("replace '"_s, changed, "' in require.cache"_s);
    case LoaderChange::ReplaceLoaderFunction:
        return makeString("replace "_s, changed);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSObject* createLoaderChangeRefusedError(JSGlobalObject* globalObject, LoaderChange change, StringView changed)
{
    return createError(globalObject, withAOTErrorNote(makeString("Cannot "_s, describe(change, changed), ": imports in this executable were resolved at build time, so the module loader can't be patched."_s)));
}

JSObject* createModuleMissingAtBuildError(JSGlobalObject* globalObject, const String& specifier, const String& importer)
{
    return createError(globalObject, makeString("Cannot find module '"_s, specifier, "' from '"_s, importer, "'"_s));
}

JSObject* createSuperCallInDirectEvalError(JSGlobalObject* globalObject)
{
    return createTypeError(globalObject, withAOTErrorNote("super() cannot be called from direct eval in a compiled executable"_s));
}

static String functionNotCompiledMessage(const String& description, ASCIILiteral advice, int)
{
    return withAOTErrorNote(makeString(description, " was not compiled into this executable because your code never calls it. To keep it, call it somewhere in your code."_s, advice));
}

String functionNotCompiledMessage(const String& functionName, ASCIILiteral advice)
{
    return functionNotCompiledMessage(makeString('\'', functionName, '\''), advice, 0);
}

String functionNotCompiledMessage(const String& path, unsigned line, ASCIILiteral advice)
{
    return functionNotCompiledMessage(makeString("The function at "_s, path, ':', line), advice, 0);
}

String functionNotCompiledMessage(ASCIILiteral advice)
{
    return functionNotCompiledMessage("This function"_s, advice, 0);
}

ASCIILiteral evaluationRefusedMessage()
{
    static constexpr ASCIILiteral message = "Code generation from strings disallowed for this context: the compiler couldn't tell at build time that this call reaches eval() or the Function constructor. Call eval() or new Function() directly."_s;
    const Image* image = Image::current();
    if (!image || !image->header().options.omitsFunctionSource)
        return message;

    static LazyNeverDestroyed<ASCIICString> messageWithAdvice;
    static std::once_flag once;
    std::call_once(once, [&] {
        messageWithAdvice.construct(makeString(message, image->header().options.adviceToKeepFunctionSource()).ascii());
    });
    return ASCIILiteral::fromLiteralUnsafe(messageWithAdvice->data());
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

String deletionFromLockedBuiltinMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot delete "_s, property, " because built-in objects are read-only in a compiled executable"_s));
}

String assignmentToLockedBuiltinMessage(const String& property)
{
    return withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable"_s));
}

String assignmentToNameThatBuiltinFunctionInheritsMessage(UniquedStringImpl* uid)
{
    if (uid->isSymbol())
        return assignmentToLockedBuiltinMessage(makeString('[', StringView(uid), "] of a built-in function"_s));
    return assignmentToLockedBuiltinMessage(makeString('\'', StringView(uid), "' of a built-in function"_s));
}

String prototypeChangeOfLockedBuiltinMessage(const String& holder)
{
    return withAOTErrorNote(makeString("Cannot change the prototype of "_s, holder, " because built-in objects are read-only in a compiled executable"_s));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
