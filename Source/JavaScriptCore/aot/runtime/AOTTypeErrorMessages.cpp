/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeErrorMessages.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTCompiledCallee.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeLayoutTable.h"
#include "AOTRuntimeType.h"
#include "AOTState.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>
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
        return makeString("native "_s, className(object));
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

static const LayoutData* singleMemberLayout(const ProgramTables& tables, const LayoutSetData& set)
{
    const LayoutData* single = nullptr;
    for (uint32_t shape = set.members.first; shape < set.members.first + set.members.count; ++shape) {
        const LayoutData* layout = memberLayoutForShape(tables, set.members, shape);
        if (!layout)
            continue;
        if (single && single->index != layout->index)
            return nullptr;
        if (!single)
            single = layout;
    }
    return single;
}

static String describeMembers(const ProgramTables& tables, const ShapeTestData& members)
{
    static constexpr unsigned maxNamesShown = 4;
    Vector<String, maxNamesShown> names;
    StringBuilder result;
    for (uint32_t shape = members.first; shape < members.first + members.count; ++shape) {
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
    StringID path(tables.modules()[module.index()].displayPath);
    if (isObfuscated() || !hasNonEmptyText(path))
        return "object"_s;
    return makeString("typeof import(\""_s, ProgramStrings::text(tables, path), "\")"_s);
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

String describe(const ProgramTables& tables, const RuntimeType& type)
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
    return describe(described);
}

static bool namesLayoutSet(const ProgramTables& tables, const RuntimeType& type)
{
    auto set = type.layoutSet();
    if (set && hasNonEmptyText(StringID(tables.layoutSets()[*set].name)))
        return true;
    auto contents = type.contents();
    return contents && namesLayoutSet(tables, tables.typeAt(*contents));
}

// The test of a written object type asks only for an object. Which type was written is in the text of the site.
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

// The test of a cast lets through the objects that its operand may be. Nobody wrote those behind `as`.
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

String describeKinds(State& state, uint32_t kinds, uint32_t name)
{
    StringID expectedClass(name);
    bool printsClass = !isObfuscated() && hasNonEmptyText(expectedClass);
    StringBuilder result;
    appendUnionMember(result, expectedClass || kinds & typeAllowsObjects, printsClass ? ProgramStrings::text(state.tables, expectedClass) : "object"_str);
    appendUnionMember(result, kinds & typeAllowsFunctions, "Function"_s);
    appendPrimitives(result, kinds & Primitive::All);
    return result.toString();
}

namespace {

struct DataProperty {
    enum class Status : uint8_t { Found, Absent, IsNotData };

    Status status;
    JSValue value;
};

} // namespace

static DataProperty readOwnDataProperty(VM& vm, JSObject* holder, PropertyName name)
{
    if (isCompiledObject(holder)) {
        if (JSValue value = Object::tryGetOwnDataProperty(vm, holder, name))
            return { DataProperty::Status::Found, value };
        switch (PropertyWalk::hasOwnProperty(holder, name.uid())) {
        case PropertyWalk::Presence::Absent:
            return { DataProperty::Status::Absent, { } };
        case PropertyWalk::Presence::Present:
        case PropertyWalk::Presence::Unknown:
            return { DataProperty::Status::IsNotData, { } };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    unsigned attributes;
    JSValue value = holder->getDirect(vm, name, attributes);
    if (!value)
        return { DataProperty::Status::Absent, { } };
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return { DataProperty::Status::IsNotData, { } };
    return { DataProperty::Status::Found, value };
}

static DataProperty readDataProperty(VM& vm, JSObject* object, PropertyName name)
{
    for (JSObject* holder = object; holder;) {
        PropertyWalk::materializeLazyProperties(vm, holder);
        DataProperty property = readOwnDataProperty(vm, holder, name);
        if (property.status != DataProperty::Status::Absent)
            return property;
        JSValue prototype = holder->getPrototypeDirect();
        holder = prototype.isObject() ? asObject(prototype) : nullptr;
    }
    return { DataProperty::Status::Absent, { } };
}

static const LayoutSetData* singleLayoutSet(const ProgramTables& tables, const RuntimeType& type)
{
    if (type.cellTests().size() != 1)
        return nullptr;
    auto set = type.layoutSet();
    return set ? &tables.layoutSets()[*set] : nullptr;
}

static String describeDeclaredType(const ProgramTables& tables, const LayoutData& layout, uint32_t fieldIndex, const RuntimeType& declared)
{
    uint32_t typeSource = layout.typeStringsOffset() ? tables.at<uint32_t>(layout.typeStringsOffset())[fieldIndex] : UINT32_MAX;
    return typeSource == UINT32_MAX ? describe(tables, declared) : ProgramStrings::text(tables, StringID(typeSource));
}

static String explainFieldMismatch(State& state, VM& vm, JSObject* object, const LayoutData& layout, const String& expectedName)
{
    const RuntimeLayoutTable& layouts = state.objects->layouts;
    ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate())
            continue;
        const Identifier& name = identifiers.identifier(field.name);
        DataProperty found = readDataProperty(vm, object, name);
        switch (found.status) {
        case DataProperty::Status::IsNotData:
            continue;
        case DataProperty::Status::Absent:
            if (field.flags & fieldIsOptional)
                continue;
            return makeString("property '"_s, name.string(), "' is missing in "_s, describe(object), " but required in "_s, expectedName);
        case DataProperty::Status::Found:
            break;
        }
        const RuntimeType& declared = layouts.typeOf(field);
        if (!layouts.valueHasType(declared, found.value))
            return makeString("property '"_s, name.string(), "': expected "_s, describeDeclaredType(state.tables, layout, layout.indexOf(field), declared), ", got "_s, describe(found.value));
    }
    return { };
}

String explainMismatch(State& state, VM& vm, JSValue value, const RuntimeType& type)
{
    if (isObfuscated())
        return { };
    const ProgramTables& tables = state.tables;
    const LayoutSetData* set = value.isObject() ? singleLayoutSet(tables, type) : nullptr;
    if (!set)
        return { };
    JSObject* object = asObject(value);
    Object* compiled = state.objects->layouts.compiledObject(object);
    String expectedName = nameOf(tables, *set);
    if (isNativeObject(object))
        return makeString("a native object cannot be used as "_s, expectedName);
    if (!compiled && object->type() != FinalObjectType)
        return { };
    const LayoutData* layout = singleMemberLayout(tables, *set);
    if (String reason = layout ? explainFieldMismatch(state, vm, object, *layout, expectedName) : String(); !reason.isNull())
        return reason;
    if (compiled)
        return makeString(describe(value), " is not assignable to "_s, expectedName);
    if (!hasPlainPrototype(object))
        return makeString("an instance of an untyped class cannot be used as "_s, expectedName);
    if (layout && layout->kind() == LayoutKind::Instance)
        return makeString("an untyped object cannot be used as an instance of class "_s, expectedName);
    return { };
}

String mismatchMessage(State& state, VM& vm, JSValue value, const RuntimeType& expected, Typed::TypeSiteID site)
{
    const ProgramTables& tables = state.tables;
    if (isObfuscated())
        return makeString("expected "_s, describe(tables, expected, site), ", got "_s, describe(value, ValueDetail::KindOnly));
    String reason = explainMismatch(state, vm, value, expected);
    String expectedDescription = describe(tables, expected, site);
    String actualDescription = describe(value);
    const LayoutSetData* set = expectedDescription == actualDescription ? singleLayoutSet(tables, expected) : nullptr;
    if (Object* object = set ? state.objects->layouts.compiledObject(value) : nullptr) {
        auto withLocation = [&](const String& name, uint32_t path, uint32_t line) {
            return path == noDeclaredPath ? name : makeString(name, " (declared at "_s, ProgramStrings::text(tables, StringID(path)), ':', line, ')');
        };
        const LayoutData* memberLayout = set->declaredPath == noDeclaredPath ? singleMemberLayout(tables, *set) : nullptr;
        expectedDescription = withLocation(expectedDescription, memberLayout ? memberLayout->declaredPath() : set->declaredPath, memberLayout ? memberLayout->declaredLine() : set->declaredLine);
        actualDescription = withLocation(actualDescription, object->layout().declaredPath(), object->layout().declaredLine());
    }
    return makeString("expected "_s, expectedDescription, ", got "_s, actualDescription, reason.isEmpty() ? ""_s : "\n  "_s, reason);
}

String nullishMessage(JSValue value, const String& property, bool isWrite)
{
    ASCIILiteral valueName = value.isNull() ? "null"_s : "undefined"_s;
    if (property.isEmpty() || property.impl()->isSymbol())
        return makeString(isWrite ? "Cannot set properties of "_s : "Cannot read properties of "_s, valueName);
    return makeString(isWrite ? "Cannot set properties of "_s : "Cannot read properties of "_s, valueName, isWrite ? " (setting '"_s : " (reading '"_s, property, "')"_s);
}

ASCIILiteral nullishIterationMessage(JSValue value)
{
    return value.isNull() ? "null is not iterable"_s : "undefined is not iterable"_s;
}

String nullishMessage(JSGlobalObject* globalObject, JSValue value, JSValue key, bool isWrite)
{
    String property;
    if (key.isInt32())
        property = String::number(key.asInt32());
    else if (key.isString() && !asString(key)->isRope())
        property = asString(key)->value(globalObject);
    return nullishMessage(value, property, isWrite);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
