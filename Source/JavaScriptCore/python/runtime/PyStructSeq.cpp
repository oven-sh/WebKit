/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyStructSeq.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyStr.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// An instance is a tuple of the fields that are items. The others are in a second tuple, a hidden field of the first.
static constexpr unsigned otherValuesField = 0;
// What a type knows of its fields is in hidden fields of the type. Its dict has the counts too, where programs expect them, but most of
// these types can be written to. A class that is no such type can still come here: the mro() of a metaclass may put one among its bases.
static constexpr unsigned namesField = 0; // A tuple with an item for each field: the name, if it has one.
static constexpr unsigned itemCountField = 1;

struct StructSeqFields {
    explicit operator bool() const { return names; }
    unsigned count() const { return names->length(); }
    JSString* name(unsigned index) const { return names->at(index).isString() ? asString(names->at(index)) : nullptr; }

    PyTuple* names { nullptr };
    unsigned itemCount { 0 };
};

static StructSeqFields fieldsOf(JSGlobalObject* globalObject, PyType* type)
{
    PyTuple* names = PyTuple::tryFrom(hiddenField(globalObject, type, namesField));
    JSValue itemCount = hiddenField(globalObject, type, itemCountField);
    if (!names || !itemCount || !itemCount.isUInt32() || itemCount.asUInt32() > names->length())
        return { };
    return { names, itemCount.asUInt32() };
}

static JSValue raiseIsNoStructSeq(JSGlobalObject* globalObject, PyType* type, ASCIILiteral attribute)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseTypeError(globalObject, scope, "Missed attribute '"_s, attribute, "' of type "_s, type->nameInMessages());
}

JSValue structSeqField(JSGlobalObject* globalObject, PyTuple* tuple, unsigned index)
{
    if (index < tuple->length())
        return tuple->at(index);
    index -= tuple->length();
    if (auto* rest = PyTuple::tryFrom(hiddenField(globalObject, tuple, otherValuesField)); rest && index < rest->length())
        return rest->at(index);
    return jsUndefined();
}

template<unsigned index>
static JSValue structSeqGetter(JSGlobalObject* globalObject, JSValue self) { return structSeqField(globalObject, PyTuple::from(self), index); }

template<size_t... indices>
static constexpr std::array<NativeGetSet::Getter, sizeof...(indices)> structSeqGetters(std::index_sequence<indices...>) { return { structSeqGetter<indices>... }; }

PyTuple* makeStructSeq(JSGlobalObject* globalObject, PyType* type, std::span<const JSValue> values)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // (The __new__ of one of these types can be written into another.)
    StructSeqFields fields = fieldsOf(globalObject, type);
    if (!fields || values.size() != fields.count()) [[unlikely]] {
        raiseIsNoStructSeq(globalObject, type, "n_fields"_s);
        return nullptr;
    }
    unsigned visible = fields.itemCount;
    PyTuple* tuple = PyTuple::createUninitialized(vm, rt.structure<PyTuple>(), type, visible);
    for (unsigned i = 0; i < visible; ++i)
        tuple->initializeAt(vm, i, values[i]);
    if (values.size() > visible)
        setHiddenField(globalObject, tuple, otherValuesField, PyTuple::create(globalObject, values.subspan(visible)));
    return tuple;
}

JSValue structSeqNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);
    StructSeqFields fields = fieldsOf(globalObject, type);
    // A class that only names one of these types. It is made by what it inherits, as in CPython.
    if (!fields) [[unlikely]] {
        if (!isLaidOutAs(type, runtime(globalObject).tupleType()))
            return raiseTypeError(globalObject, scope, "cannot create '"_s, type->nameInMessages(), "' instances"_s);
        RELEASE_AND_RETURN(scope, tupleNew(globalObject, cls, args));
    }
    // Whatever the type, the messages about the arguments call it "structseq".
    static constexpr ASCIILiteral keywords[] = { "sequence"_s, "dict"_s };
    JSValue arguments[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "structseq"_s, keywords, 1, 2, false, args, arguments)))
        return { };
    auto [sequence, dictValue] = arguments;
    unsigned minimum = fields.itemCount;
    unsigned maximum = fields.count();
    PyTuple* given = tupleFromIterable(globalObject, sequence);
    if (failed(scope, given)) [[unlikely]] {
        if (clearPendingExceptionIfMatches(globalObject, runtime(globalObject).TypeError()))
            raiseTypeError(globalObject, scope, "constructor requires a sequence"_s);
        return { };
    }
    String name = qualifiedTypeName(globalObject, type);
    if (dictValue && !isDict(dictValue))
        return raiseTypeError(globalObject, scope, name, "() takes a dict as second arg, if any"_s);
    unsigned length = given->length();
    if (minimum != maximum) {
        if (length < minimum)
            return raiseTypeError(globalObject, scope, name, "() takes an at least "_s, minimum, "-sequence ("_s, length, "-sequence given)"_s);
        if (length > maximum)
            return raiseTypeError(globalObject, scope, name, "() takes an at most "_s, maximum, "-sequence ("_s, length, "-sequence given)"_s);
    } else if (length != minimum)
        return raiseTypeError(globalObject, scope, name, "() takes a "_s, minimum, "-sequence ("_s, length, "-sequence given)"_s);

    auto* dict = dictValue ? PyDict::from(dictValue) : nullptr;
    MarkedArgumentBuffer values;
    unsigned found = 0;
    for (unsigned i = 0; i < maximum; ++i) {
        JSValue value = i < length ? given->at(i) : JSValue();
        if (!value && dict && fields.name(i)) {
            value = dict->get(fields.name(i));
            found += !!value;
        }
        values.append(value ? value : jsUndefined());
    }
    if (dict && dict->size() > found)
        return raiseTypeError(globalObject, scope, name, "() got duplicate or unexpected field name(s)"_s);
    RELEASE_AND_RETURN(scope, makeStructSeq(globalObject, type, spanOf(values)));
}

static JSValue structSeqRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = PyTuple::from(self);
    PyType* type = tuple->type();
    StructSeqFields fields = fieldsOf(globalObject, type);
    if (!fields) [[unlikely]] {
        if (!tuple->length())
            RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString(type->nameInMessages(), "()"_s)));
        return raise(globalObject, scope, runtime(globalObject).SystemError(), makeString("In structseq_repr(), member 0 name is NULL for type "_s, type->nameInMessages()));
    }
    StrBuilder builder;
    builder.append(qualifiedTypeName(globalObject, type), '(');
    // An item goes by the name of the field that is as far among those with names: where some have none, as in
    // os.stat_result, that is not the field it is the value of.
    unsigned nameIndex = 0;
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (i)
            builder.append(", "_s);
        while (nameIndex < fields.count() && !fields.name(nameIndex))
            ++nameIndex;
        if (nameIndex < fields.count())
            builder.append(stringOf(fields.name(nameIndex++)), '=');
        JSString* text = repr(globalObject, tuple->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        String string = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(string);
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue structSeqReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyTuple* tuple = PyTuple::from(self);
    PyType* type = tuple->type();
    StructSeqFields fields = fieldsOf(globalObject, type);
    if (!fields) [[unlikely]]
        return raiseIsNoStructSeq(globalObject, type, "n_unnamed_fields"_s);
    PyDict* hidden = PyDict::create(globalObject);
    for (unsigned i = tuple->length(); i < fields.count(); ++i) {
        if (JSString* name = fields.name(i))
            hidden->set(vm, name, structSeqField(globalObject, tuple, i));
    }
    return PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { PyTuple::create(globalObject, tuple->span()), hidden }) });
}

static JSValue structSeqReplace(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size())
        return raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s);
    PyTuple* tuple = PyTuple::from(self);
    PyType* type = tuple->type();
    StructSeqFields fields = fieldsOf(globalObject, type);
    if (!fields) [[unlikely]]
        RELEASE_AND_RETURN(scope, raiseIsNoStructSeq(globalObject, type, "n_unnamed_fields"_s));
    MarkedArgumentBuffer values;
    unsigned used = 0;
    for (unsigned i = 0; i < fields.count(); ++i) {
        JSString* name = fields.name(i);
        if (!name)
            return raiseTypeError(globalObject, scope, "__replace__() is not supported for "_s, qualifiedTypeName(globalObject, type), " because it has unnamed field(s)"_s);
        JSValue value = args.keyword(name);
        used += !!value;
        values.append(value ? value : structSeqField(globalObject, tuple, i));
    }
    if (used != args.keywordCount()) {
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            bool isKnown = false;
            for (unsigned j = 0; j < fields.count() && !isKnown; ++j)
                isKnown = WTF::equal(args.keywordName(i)->tryGetValueImpl(), fields.name(j)->tryGetValueImpl());
            if (!isKnown)
                return raiseTypeError(globalObject, scope, "Got unexpected field name(s): ['"_s, stringOf(args.keywordName(i)), "']"_s);
        }
    }
    RELEASE_AND_RETURN(scope, makeStructSeq(globalObject, type, spanOf(values)));
}

static constexpr NativeDef structSeqMethods[] = {
    PY_SLOT("__repr__", structSeqRepr, "($self, /)"),
    PY_METHOD("__reduce__", structSeqReduce, "($self, /)"),
    PY_METHOD("__replace__", structSeqReplace, "($self, /, **changes)"),
};

static constexpr NativeDef structSeqConstructor[] = {
    PY_NEW(structSeqNew, "($type, /, sequence, dict=<unrepresentable>, *args)"),
};

PyType* structSeqTypeFor(JSGlobalObject* globalObject, const StructSeqDef& def)
{
    Runtime& rt = runtime(globalObject);
    if (PyType* type = rt.typeIfMadeFor(def))
        return type;
    PyType* type = makeStructSeqType(globalObject, def);
    ASSERT(!rt.typeIfMadeFor(def));
    rt.keepTypeMadeFor(def, type);
    return type;
}

PyType* makeStructSeqType(JSGlobalObject* globalObject, const StructSeqDef& def)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    static constexpr auto getters = structSeqGetters(std::make_index_sequence<maxStructSeqFields>());
    RELEASE_ASSERT(def.fields.size() <= maxStructSeqFields);

    // The descriptors point at these for as long as the type lives, which is as long as the process may.
    auto* getsets = static_cast<NativeGetSet*>(fastMalloc(def.fields.size() * sizeof(NativeGetSet)));
    unsigned named = 0;
    for (unsigned i = 0; i < def.fields.size(); ++i) {
        if (!def.fields[i].isNull())
            new (NotNull, &getsets[named++]) NativeGetSet(def.fields[i], getters[i], KnownOfName(def.fields[i], KnownOfName::HashOnly));
    }

    PyType* type = PyType::createBare(vm, rt.typeStructure());
    OptionSet<TypeFlag> flags;
    if (!def.canBeInstantiated)
        flags.add(TypeFlag::DisallowInstantiation);
    if (!def.isImmutable)
        flags.add(TypeFlag::Mutable);
    type->initializeBuiltin(globalObject, { def.name, flags, structSeqMethods, std::span<const NativeGetSet> { getsets, named }, { } }, rt.typeType(), rt.tupleType());
    // As in CPython, where one with fields that are no items is larger than a tuple.
    if (def.fields.size() == def.sequenceFieldCount)
        type->shareLayoutOfBase(vm);
    if (def.canBeInstantiated)
        type->addMethods(globalObject, structSeqConstructor);

    PyTuple* names = PyTuple::create(globalObject, static_cast<unsigned>(def.fields.size()));
    MarkedArgumentBuffer matchArgs;
    for (unsigned i = 0; i < def.fields.size(); ++i) {
        if (def.fields[i].isNull())
            continue;
        JSString* name = internString(vm, def.fields[i]);
        names->initializeAt(vm, i, name);
        if (i < def.sequenceFieldCount)
            matchArgs.append(name);
    }
    PyDict* dict = type->dict();
    setHiddenField(globalObject, type, namesField, names);
    setHiddenField(globalObject, type, itemCountField, jsNumber(def.sequenceFieldCount));
    dict->set(vm, internString(vm, "n_sequence_fields"_s), jsNumber(def.sequenceFieldCount));
    dict->set(vm, internString(vm, "n_fields"_s), jsNumber(static_cast<unsigned>(def.fields.size())));
    dict->set(vm, internString(vm, "n_unnamed_fields"_s), jsNumber(static_cast<unsigned>(def.fields.size()) - named));
    dict->set(vm, rt.names.dMatchArgs(), PyTuple::create(globalObject, spanOf(matchArgs)));
    type->didChange(vm);
    return type;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
