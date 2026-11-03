/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAstModule.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// The module _ast: the classes of the nodes. (Python/Python-ast.c in CPython, which Parser/asdl_c.py generates, is the reference.)
//
// AST is written here. The others are classes like any a program makes, derived from it, with what the grammar says of them
// as class attributes.

namespace {

struct AstClassDef {
    ASCIILiteral name;
    AstClass base;
    AstClassKind kind;
    AstAttributes attributes;
    ASCIILiteral fields;
};

struct AstFieldDef {
    StringView type;
    StringView name;
    bool isOptional { false };
    bool isSequence { false };
};

}

static constexpr AstClassDef astClassDefs[] = {
    { "AST"_s, AstClass::AST, AstClassKind::Sum, AstAttributes::None, ""_s },
#define PY_DEFINE_CLASS(id, name, base, kind, attributes, fields) { name ""_s, AstClass::base, AstClassKind::kind, AstAttributes::attributes, fields ""_s },
    FOR_EACH_PY_AST_CLASS(PY_DEFINE_CLASS)
#undef PY_DEFINE_CLASS
};
static_assert(std::size(astClassDefs) == numberOfAstClasses);

static constexpr ASCIILiteral astNameTexts[] = { "lineno"_s, "col_offset"_s, "end_lineno"_s, "end_col_offset"_s, "_fields"_s, "_attributes"_s, "_field_types"_s };
static_assert(std::size(astNameTexts) == static_cast<unsigned>(AstName::Count));

static constexpr unsigned astStateField = 0; // Of the type AST: the table of an AstState.

ASCIILiteral nameOfAstClass(AstClass id)
{
    return astClassDefs[static_cast<unsigned>(id)].name;
}

// "expr?* keys, expr* values"
static Vector<AstFieldDef, 8> astParseFields(ASCIILiteral text)
{
    Vector<AstFieldDef, 8> result;
    StringView rest { text };
    while (!rest.isEmpty()) {
        size_t comma = rest.find(", "_s);
        StringView item = comma == notFound ? rest : rest.left(comma);
        rest = comma == notFound ? StringView() : rest.substring(comma + 2);
        size_t space = item.find(' ');
        ASSERT(space != notFound);
        AstFieldDef field;
        field.name = item.substring(space + 1);
        field.type = item.left(space);
        while (field.type.endsWith('*') || field.type.endsWith('?')) {
            if (field.type.endsWith('*'))
                field.isSequence = true;
            else
                field.isOptional = true;
            field.type = field.type.left(field.type.length() - 1);
        }
        // `expr?*` is a sequence some items of which may be None.
        if (field.isSequence)
            field.isOptional = false;
        result.append(field);
    }
    return result;
}

const AstClassInfo& infoOfAstClass(AstClass id)
{
    static LazyNeverDestroyed<Vector<AstClassInfo>> table;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        table.construct();
        for (auto& def : astClassDefs) {
            AstClassInfo info { def.name, def.base, def.kind, def.kind == AstClassKind::Constructor ? astClassDefs[static_cast<unsigned>(def.base)].attributes : def.attributes, false, AstClass::AST, { } };
            for (auto& parsed : astParseFields(def.fields)) {
                AstField field { parsed.name, AstField::Type::Node, AstClass::AST, parsed.isOptional, parsed.isSequence };
                if (parsed.type == "identifier"_s)
                    field.type = AstField::Type::Identifier;
                else if (parsed.type == "int"_s)
                    field.type = AstField::Type::Int;
                else if (parsed.type == "string"_s)
                    field.type = AstField::Type::String;
                else if (parsed.type == "constant"_s)
                    field.type = AstField::Type::Constant;
                else {
                    for (unsigned i = 0; i < numberOfAstClasses; ++i) {
                        if (parsed.type == astClassDefs[i].name)
                            field.node = static_cast<AstClass>(i);
                    }
                    ASSERT(field.node != AstClass::AST);
                }
                info.fields.append(WTF::move(field));
            }
            table->append(WTF::move(info));
        }
        for (unsigned i = 1; i < numberOfAstClasses; ++i) {
            auto& info = table.get()[i];
            if (info.kind == AstClassKind::Sum)
                info.isSimpleSum = info.attributes == AstAttributes::None;
            if (info.kind != AstClassKind::Constructor)
                continue;
            auto& sum = table.get()[static_cast<unsigned>(info.base)];
            sum.lastConstructor = static_cast<AstClass>(i);
            if (!info.fields.isEmpty())
                sum.isSimpleSum = false;
        }
    });
    return table.get()[static_cast<unsigned>(id)];
}

// What the grammar says of the class, which is its docstring.
static String astSignature(const AstClassDef& def)
{
    if (def.kind != AstClassKind::Sum)
        return !def.fields.length() ? String(def.name) : makeString(def.name, '(', def.fields, ')');

    AstClass id = static_cast<AstClass>(&def - astClassDefs);
    bool isSimple = def.attributes == AstAttributes::None;
    for (auto& other : astClassDefs) {
        if (other.base == id && other.fields.length())
            isSimple = false;
    }
    StrBuilder builder;
    builder.append(def.name, " = "_s);
    bool first = true;
    for (auto& other : astClassDefs) {
        if (other.base != id || &other == &def)
            continue;
        if (!first) {
            if (isSimple)
                builder.append(" | "_s);
            else {
                builder.append('\n');
                for (size_t i = 0; i <= def.name.length(); ++i)
                    builder.append(' ');
                builder.append("| "_s);
            }
        }
        first = false;
        builder.append(astSignature(other));
    }
    return builder.toString();
}

// ---- AST ----

extern const TypeDef astTypeDef;

static JSValue astNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return allocateInstance(globalObject, PyType::from(cls), runtime(globalObject).typeFor(astTypeDef));
}

// 'a', 'b', and 'c'
static String astFormatMissing(const Vector<String, 8>& names)
{
    StrBuilder builder;
    size_t total = names.size();
    for (size_t i = 0; i < total; ++i) {
        size_t left = total - i;
        builder.append('\'', names[i], '\'');
        if (left == 1)
            break;
        if (total == 2)
            builder.append(" and "_s);
        else if (left == 2)
            builder.append(", and "_s);
        else
            builder.append(", "_s);
    }
    return builder.toString();
}

static JSValue astInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    AstState state(globalObject);
    if (!state)
        return { };

    PyType* type = typeOf(globalObject, self);
    if (state.isAbstract(type)) {
        bool goOn = warn(globalObject, rt.DeprecationWarning(), makeString("Instantiating abstract AST node class "_s, typeNameForMessage(globalObject, type), " is deprecated. This will become an error in Python 3.20"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!goOn)
            return { };
    }

    JSValue fieldsValue = getAttr(globalObject, type, state.name(AstName::Fields));
    RETURN_IF_EXCEPTION(scope, { });
    length(globalObject, fieldsValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* fields = tupleFromIterable(globalObject, fieldsValue);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned fieldCount = fields->length();
    Vector<bool, 8> isGiven(FillWith { }, fieldCount, false);

    if (fieldCount < args.size())
        return raiseTypeError(globalObject, scope, type->name(), " constructor takes at most "_s, fieldCount, " positional argument"_s, fieldCount == 1 ? ""_s : "s"_s);

    for (unsigned i = 0; i < args.size(); ++i) {
        JSString* name = attributeName(globalObject, fields->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, self, name, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        isGiven[i] = true;
    }

    JSValue attributes;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        JSString* key = args.keywordName(i);
        bool isField = false;
        for (unsigned j = 0; j < fieldCount; ++j) {
            bool same = isEqual(globalObject, fields->at(j), key);
            RETURN_IF_EXCEPTION(scope, { });
            if (!same)
                continue;
            isField = true;
            if (isGiven[j]) {
                String keyText = reprForMessage(globalObject, key);
                return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), " got multiple values for argument "_s, keyText);
            }
            isGiven[j] = true;
            break;
        }
        if (!isField) {
            if (!attributes) {
                attributes = getAttr(globalObject, type, state.name(AstName::Attributes));
                RETURN_IF_EXCEPTION(scope, { });
            }
            bool isAttribute = contains(globalObject, attributes, key);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isAttribute) {
                String keyText = reprForMessage(globalObject, key);
                return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__init__ got an unexpected keyword argument "_s, keyText);
            }
        }
        setAttr(globalObject, self, key, args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }

    if (!isGiven.contains(false))
        return jsUndefined();

    // A class of the program's own that says nothing of the types of its fields: those not given do not exist.
    JSValue fieldTypes = getAttrIfPresent(globalObject, type, state.name(AstName::FieldTypes));
    RETURN_IF_EXCEPTION(scope, { });
    if (!fieldTypes)
        return jsUndefined();
    if (!isDict(fieldTypes))
        return raiseBadInternalCall(globalObject, scope);

    Vector<String, 8> missing;
    for (unsigned i = 0; i < fieldCount; ++i) {
        if (isGiven[i])
            continue;
        JSValue name = fields->at(i);
        JSValue fieldType = PyDict::from(fieldTypes)->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!fieldType) {
            String nameText = reprForMessage(globalObject, name);
            return raiseTypeError(globalObject, scope, "Field "_s, nameText, " is missing from "_s, typeNameForMessage(globalObject, type), "._field_types"_s);
        }
        PyType* kind = typeOf(globalObject, fieldType);
        if (kind == rt.unionType())
            continue; // Optional: the class has None.
        JSValue defaultValue;
        if (kind == rt.genericAliasType()) {
            defaultValue = newList(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        } else if (fieldType == JSValue(state.type(AstClass::AnyExprContext)))
            defaultValue = state.singleton(AstClass::Load);
        else {
            JSString* text = str(globalObject, name);
            RETURN_IF_EXCEPTION(scope, { });
            String string = text->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            missing.append(string);
            continue;
        }
        JSString* attribute = attributeName(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, self, attribute, defaultValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!missing.isEmpty())
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__init__ missing "_s, missing.size(), " required positional argument"_s, missing.size() == 1 ? ""_s : "s"_s, ": "_s, astFormatMissing(missing));
    return jsUndefined();
}

// The object is made again with None for each field, so that none is missing and no node is an argument, which could
// recurse without end; then the fields are set from the dict.
static JSValue astReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    AstState state(globalObject);
    if (!state)
        return { };

    PyType* type = typeOf(globalObject, self);
    JSValue dict = getAttrIfPresent(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { type, rt.emptyTuple() }));
    JSValue fields = getAttrIfPresent(globalObject, type, state.name(AstName::Fields));
    RETURN_IF_EXCEPTION(scope, { });
    if (!fields)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { type, rt.emptyTuple(), dict }));
    if (!isDict(dict))
        return raiseBadInternalCall(globalObject, scope);

    int64_t fieldCount = length(globalObject, fields);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned present = 0;
    for (int64_t i = 0; i < fieldCount; ++i) {
        JSValue name = getItem(globalObject, fields, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue value = PyDict::from(dict)->get(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            break;
        ++present;
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { type, PyTuple::create(globalObject, present), dict }));
}

// payload[key] = dict[key], for each of the keys that is there.
static bool astCopyPresent(JSGlobalObject* globalObject, PyDict* payload, JSValue keys, PyDict* dict)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!keys)
        return true;
    int64_t count = length(globalObject, keys);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue key = getItem(globalObject, keys, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        JSValue value = dict->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            continue;
        payload->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static JSValue astReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size())
        return raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s);
    AstState state(globalObject);
    if (!state)
        return { };

    PyType* type = typeOf(globalObject, self);
    JSValue fields = getAttrIfPresent(globalObject, type, state.name(AstName::Fields));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attributes = getAttrIfPresent(globalObject, type, state.name(AstName::Attributes));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue dict = getAttrIfPresent(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });

    PyDict* payload = PyDict::create(globalObject);
    if (dict) {
        if (!isDict(dict))
            return raiseBadInternalCall(globalObject, scope);
        if (failed(scope, astCopyPresent(globalObject, payload, fields, PyDict::from(dict))))
            return { };
        if (failed(scope, astCopyPresent(globalObject, payload, attributes, PyDict::from(dict))))
            return { };
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        payload->set(globalObject, args.keywordName(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, callWithTupleAndDict(globalObject, type, rt.emptyTuple(), payload));
}

static bool astAppendRepr(JSGlobalObject*, const AstState&, StringBuilder&, JSValue node, int depth);

static bool astAppendReprOfAny(JSGlobalObject* globalObject, const AstState& state, StringBuilder& builder, JSValue value, int depth)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isInstanceOf(globalObject, value, state.type(AstClass::AST)))
        RELEASE_AND_RETURN(scope, astAppendRepr(globalObject, state, builder, value, depth - 1));
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    builder.append(string);
    return true;
}

// Only the first item and the last are shown: [Constant(value=1), ..., Constant(value=3)]
static bool astAppendReprOfSequence(JSGlobalObject* globalObject, const AstState& state, StringBuilder& builder, JSValue sequence, int depth)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = length(globalObject, sequence);
    RETURN_IF_EXCEPTION(scope, false);
    if (!count) {
        JSString* text = repr(globalObject, sequence);
        RETURN_IF_EXCEPTION(scope, false);
        String string = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        builder.append(string);
        return true;
    }

    JSValue first = getItem(globalObject, sequence, jsNumber(0));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue last;
    if (count > 1) {
        last = getItem(globalObject, sequence, makeInt(globalObject, count - 1));
        RETURN_IF_EXCEPTION(scope, false);
    }

    bool isAList = isList(sequence);
    builder.append(isAList ? '[' : '(');
    if (!astAppendReprOfAny(globalObject, state, builder, first, depth))
        return false;
    if (count > 2)
        builder.append(", ..."_s);
    if (last) {
        builder.append(", "_s);
        if (!astAppendReprOfAny(globalObject, state, builder, last, depth))
            return false;
    }
    builder.append(isAList ? ']' : ')');
    return true;
}

static bool astAppendRepr(JSGlobalObject* globalObject, const AstState& state, StringBuilder& builder, JSValue node, int depth)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, node);
    String typeName = type->nameInMessages();
    if (depth <= 0) {
        builder.append(typeName, "(...)"_s);
        return true;
    }
    ReprGuard guard(globalObject, node);
    if (guard.isRecursive()) {
        builder.append(typeName, "(...)"_s);
        return true;
    }

    JSValue fields = getAttr(globalObject, type, state.name(AstName::Fields));
    RETURN_IF_EXCEPTION(scope, false);
    int64_t fieldCount = length(globalObject, fields);
    RETURN_IF_EXCEPTION(scope, false);

    builder.append(typeName, '(');
    for (int64_t i = 0; i < fieldCount; ++i) {
        JSValue nameValue = getItem(globalObject, fields, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        JSString* name = attributeName(globalObject, nameValue);
        RETURN_IF_EXCEPTION(scope, false);
        JSValue value = getAttr(globalObject, node, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (i)
            builder.append(", "_s);
        builder.append(stringOf(name), '=');
        bool ok = isList(value) || PyTuple::is(value) ? astAppendReprOfSequence(globalObject, state, builder, value, depth) : astAppendReprOfAny(globalObject, state, builder, value, depth);
        RETURN_IF_EXCEPTION(scope, false);
        if (!ok)
            return false;
    }
    builder.append(')');
    return true;
}

static JSValue astRepr(JSGlobalObject* globalObject, JSValue self)
{
    AstState state(globalObject);
    if (!state)
        return { };
    StrBuilder builder;
    if (!astAppendRepr(globalObject, state, builder, self, 3))
        return { };
    return makeStr(globalObject, builder);
}

static constexpr NativeDef astMethods[] = {
    PY_NEW(astNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", astInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", astRepr, "($self, /)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", objectSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", objectDelAttr, "($self, name, /)"),
    PY_METHOD("__reduce__", astReduce, "($self, /)"),
    PY_METHOD("__replace__", astReplace, "($self, /, **fields)"),
};

constexpr TypeDef astTypeDef { "ast.AST"_s, { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::Mutable, TypeFlag::OwnLayout }, astMethods, { }, { } };

// ---- The classes ----

static PyType* astTypeNamed(JSGlobalObject* globalObject, PyTuple* table, StringView name)
{
    Runtime& rt = runtime(globalObject);
    if (name == "identifier"_s || name == "string"_s)
        return rt.strType();
    if (name == "int"_s)
        return rt.intType();
    if (name == "constant"_s)
        return rt.objectType();
    for (unsigned i = 0; i < numberOfAstClasses; ++i) {
        if (name == astClassDefs[i].name)
            return PyType::from(table->at(i));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PyTuple* astMakeClasses(JSGlobalObject* globalObject, PyType* base)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyTuple* table = PyTuple::create(globalObject, 3 * numberOfAstClasses + static_cast<unsigned>(AstName::Count));
    auto nameAt = [&](AstName name) { return asString(table->at(3 * numberOfAstClasses + static_cast<unsigned>(name))); };
    for (unsigned i = 0; i < static_cast<unsigned>(AstName::Count); ++i)
        table->initializeAt(vm, 3 * numberOfAstClasses + i, internString(vm, astNameTexts[i]));

    base->addGetSets(globalObject, dictGetSet());
    base->dict()->set(vm, nameAt(AstName::Fields), rt.emptyTuple());
    base->dict()->set(vm, rt.names.dMatchArgs(), rt.emptyTuple());
    base->dict()->set(vm, nameAt(AstName::Attributes), rt.emptyTuple());
    base->dict()->set(vm, rt.names.dModule(), internString(vm, "ast"_s));
    base->didChange(vm);
    table->initializeAt(vm, 0, base);
    table->initializeAt(vm, numberOfAstClasses, rt.emptyTuple());

    JSString* moduleName = internString(vm, "ast"_s);
    PyTuple* positions = PyTuple::create(globalObject, { nameAt(AstName::Lineno), nameAt(AstName::ColOffset), nameAt(AstName::EndLineno), nameAt(AstName::EndColOffset) });

    for (unsigned i = 1; i < numberOfAstClasses; ++i) {
        auto& def = astClassDefs[i];
        auto fields = astParseFields(def.fields);
        PyTuple* fieldNames = PyTuple::create(globalObject, fields.size());
        for (unsigned j = 0; j < fields.size(); ++j)
            fieldNames->initializeAt(vm, j, internString(vm, fields[j].name.toString()));

        PyDict* dict = PyDict::create(globalObject);
        dict->set(vm, nameAt(AstName::Fields), fieldNames);
        dict->set(vm, rt.names.dMatchArgs(), fieldNames);
        dict->set(vm, rt.names.dModule(), moduleName);
        dict->set(vm, rt.names.dDoc(), makeStr(vm, astSignature(def)));
        JSValue made = call(globalObject, rt.typeType(), makeStr(vm, def.name), PyTuple::create(globalObject, { table->at(static_cast<unsigned>(def.base)) }), dict);
        RETURN_IF_EXCEPTION(scope, nullptr);
        PyType* type = PyType::from(made);
        table->initializeAt(vm, i, type);
        table->initializeAt(vm, numberOfAstClasses + i, fieldNames);

        if (def.kind != AstClassKind::Constructor) {
            setAttr(globalObject, type, nameAt(AstName::Attributes), def.attributes == AstAttributes::None ? rt.emptyTuple() : positions);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (def.attributes == AstAttributes::OptionalEnd) {
                setAttr(globalObject, type, nameAt(AstName::EndLineno), jsUndefined());
                RETURN_IF_EXCEPTION(scope, nullptr);
                setAttr(globalObject, type, nameAt(AstName::EndColOffset), jsUndefined());
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
        for (unsigned j = 0; j < fields.size(); ++j) {
            if (!fields[j].isOptional)
                continue;
            setAttr(globalObject, type, asString(fieldNames->at(j)), jsUndefined());
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (def.kind == AstClassKind::Constructor && fields.isEmpty() && astClassDefs[static_cast<unsigned>(def.base)].attributes == AstAttributes::None)
            table->initializeAt(vm, 2 * numberOfAstClasses + i, allocateInstance(globalObject, type, base));
    }

    // The types of the fields, now that all the classes are there.
    for (unsigned i = 1; i < numberOfAstClasses; ++i) {
        auto& def = astClassDefs[i];
        if (def.kind == AstClassKind::Sum)
            continue;
        PyType* type = PyType::from(table->at(i));
        PyDict* fieldTypes = PyDict::create(globalObject);
        for (auto& field : astParseFields(def.fields)) {
            JSValue fieldType = astTypeNamed(globalObject, table, field.type);
            if (field.isSequence)
                fieldType = makeGenericAlias(globalObject, rt.listType(), fieldType);
            else if (field.isOptional)
                fieldType = makeUnion(globalObject, fieldType, jsUndefined());
            RETURN_IF_EXCEPTION(scope, nullptr);
            fieldTypes->set(vm, internString(vm, field.name.toString()), fieldType);
        }
        setAttr(globalObject, type, nameAt(AstName::FieldTypes), fieldTypes);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setAttr(globalObject, type, rt.names.dAnnotations(), fieldTypes);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return table;
}

AstState::AstState(JSGlobalObject* globalObject)
{
    PyType* base = runtime(globalObject).typeFor(astTypeDef);
    JSValue table = hiddenField(globalObject, base, astStateField);
    if (!table) [[unlikely]] {
        m_table = astMakeClasses(globalObject, base);
        if (m_table)
            setHiddenField(globalObject, base, astStateField, m_table);
        return;
    }
    m_table = uncheckedDowncast<PyTuple>(table.asCell());
}

bool AstState::isAbstract(PyType* type) const
{
    for (unsigned i = 0; i < numberOfAstClasses; ++i) {
        if (astClassDefs[i].kind == AstClassKind::Sum && m_table->at(i) == JSValue(type))
            return true;
    }
    return false;
}

bool isAST(JSGlobalObject* globalObject, JSValue value)
{
    // Nothing is a node before the classes are made.
    PyType* base = runtime(globalObject).typeFor(astTypeDef);
    return isInstance(globalObject, value, base);
}

// ---- The module ----

static JSValue astIsAbstract(JSGlobalObject* globalObject, JSValue cls)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    AstState state(globalObject);
    if (!state)
        return { };
    hashForContainer(globalObject, cls, "a set element"_s);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = PyType::tryFrom(cls);
    return jsBoolean(type && state.isAbstract(type));
}

static constexpr NativeDef astFunctions[] = {
    PY_FUNCTION("_is_abstract", astIsAbstract, "(cls, /)"),
};

void initializeAstModule(JSGlobalObject* globalObject, PyModule* module)
{
    AstState state(globalObject);
    if (!state)
        return;
    module->addFunctions(globalObject, astFunctions);
    module->add(globalObject, "AST"_s, state.type(AstClass::AST));
    module->add(globalObject, "PyCF_ALLOW_TOP_LEVEL_AWAIT"_s, jsNumber(CompileAllowTopLevelAwait));
    module->add(globalObject, "PyCF_ONLY_AST"_s, jsNumber(CompileOnlyAST));
    module->add(globalObject, "PyCF_TYPE_COMMENTS"_s, jsNumber(CompileTypeComments));
    module->add(globalObject, "PyCF_OPTIMIZED_AST"_s, jsNumber(CompileOptimizedAST));
    for (unsigned i = 1; i < numberOfAstClasses; ++i)
        module->add(globalObject, astClassDefs[i].name, state.type(static_cast<AstClass>(i)));
}

} // namespace Python
} // namespace JSC
