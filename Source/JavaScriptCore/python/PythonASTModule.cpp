/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonASTModule.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PyType.h"
#include "PythonASTBuilder.h"
#include "PythonASTValidator.h"
#include "PythonASTWalker.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonSymbolTable.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

// The module _ast: the syntax tree as objects. This is what of CPython's Python/Python-ast.c is written by hand, in Parser/asdl_c.py. The rest of that file is generated from Parser/Python.asdl, and here it
// is done by going through the table in PythonASDL.h.

namespace JSC { namespace Python {

// ---- The class AST

// PyType_GenericNew(): whatever it is given is for __init__().
PYTHON_NATIVE(astNew)
{
    NATIVE_PROLOGUE();
    if (!args.size() || !isClass(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "ast.AST.__new__(): not enough arguments"_s));
    return JSValue::encode(PyInstance::create(vm, asType(args[0])->instanceStructure()));
}

// ast_type_init()
PYTHON_NATIVE(astInit)
{
    NATIVE_PROLOGUE();
    ASTState* state = astState(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue self = args[0];
    PyType* type = typeOf(globalObject, self);
    unsigned given = args.size() - 1;

    JSValue fields = getAttribute(globalObject, type, Identifier::fromString(vm, "_fields"_s));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t fieldCount = length(globalObject, fields);
    RETURN_IF_EXCEPTION(scope, { });
    PySet* remaining = setFromIterable(globalObject, realm->typeSet()->instanceStructure(), fields);
    RETURN_IF_EXCEPTION(scope, { });

    if (fieldCount < static_cast<int64_t>(given))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameWithoutModule(globalObject), " constructor takes at most "_s, fieldCount, " positional argument"_s, fieldCount == 1 ? ""_s : "s"_s)));

    auto set = [&] (JSValue name, JSValue value) {
        auto property = attributeName(globalObject, scope, name);
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        setAttribute(globalObject, self, *property, value);
    };
    for (unsigned i = 0; i < given; ++i) {
        JSValue name = getItem(globalObject, fields, intFromUInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        set(name, args[i + 1]);
        RETURN_IF_EXCEPTION(scope, { });
        remaining->remove(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
    }

    JSValue attributes;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        JSString* key = args.keywordName(i);
        bool isField = contains(globalObject, fields, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (isField) {
            bool wasRemaining = !!remaining->remove(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            if (!wasRemaining)
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), " got multiple values for argument "_s, repr(globalObject, key))));
        } else {
            if (!attributes) {
                attributes = getAttribute(globalObject, type, Identifier::fromString(vm, "_attributes"_s));
                RETURN_IF_EXCEPTION(scope, { });
            }
            bool isAttribute = contains(globalObject, attributes, key);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isAttribute) {
                if (!warn(globalObject, BuiltinType::DeprecationWarning, concatenate(type->nameString(globalObject), ".__init__ got an unexpected keyword argument "_s, repr(globalObject, key),
                    ". Support for arbitrary keyword arguments is deprecated and will be removed in Python 3.15."_s)))
                    return { };
            }
        }
        set(key, args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }

    if (!remaining->size())
        RETURN_NONE();
    JSValue fieldTypes = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_field_types"_s));
    RETURN_IF_EXCEPTION(scope, { });
    // Probably a class that a program has derived from AST, and has not said. What is not given is then not there, as it used to be.
    if (!fieldTypes)
        RETURN_NONE();
    MarkedArgumentBuffer remainingNames;
    for (unsigned i = 0; i < remaining->entryCount(); ++i) {
        if (JSValue name = remaining->keyAt(i))
            remainingNames.append(name);
    }
    for (unsigned i = 0; i < remainingNames.size(); ++i) {
        JSValue name = remainingNames.at(i);
        JSValue fieldType = isDict(fieldTypes) ? asDict(fieldTypes)->get(globalObject, name) : JSValue();
        RETURN_IF_EXCEPTION(scope, { });
        if (!fieldType) {
            if (!warn(globalObject, BuiltinType::DeprecationWarning, concatenate("Field "_s, repr(globalObject, name), " is missing from "_s, type->nameString(globalObject), "._field_types. This will become an error in Python 3.15."_s)))
                return { };
        } else if (isUnion(globalObject, fieldType)) {
            // It may be left out, and the class has None for it.
        } else if (isExactly(globalObject, fieldType, BuiltinType::GenericAlias)) {
            JSArray* empty = newList(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            set(name, empty);
            RETURN_IF_EXCEPTION(scope, { });
        } else if (fieldType == state->classFor(ASTClass::expr_context)) {
            set(name, state->singletonFor(ASTClass::Load));
            RETURN_IF_EXCEPTION(scope, { });
        } else {
            if (!warn(globalObject, BuiltinType::DeprecationWarning, concatenate(type->nameString(globalObject), ".__init__ missing 1 required positional argument: "_s, repr(globalObject, name), ". This will become an error in Python 3.15."_s)))
                return { };
        }
    }
    RETURN_NONE();
}

static JSValue dictOfNode(JSGlobalObject* globalObject, JSValue self)
{
    return getAttributeIfPresent(globalObject, self, globalObject->vm().pythonNames().dunder_dict);
}

// ast_type_reduce(). It is made again with as many arguments as it has fields, so that nothing is warned of, and they are all None, so that copying it does not go down through the whole tree at once. Then it
// is given what is in its __dict__.
PYTHON_NATIVE(astReduce)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    PyType* type = typeOf(globalObject, self);
    JSValue dict = dictOfNode(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::create(globalObject, 0) }));
    JSValue fields = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_fields"_s));
    RETURN_IF_EXCEPTION(scope, { });
    unsigned present = 0;
    if (fields) {
        int64_t fieldCount = length(globalObject, fields);
        RETURN_IF_EXCEPTION(scope, { });
        for (int64_t i = 0; i < fieldCount; ++i) {
            JSValue name = getItem(globalObject, fields, intFromInt64(globalObject, i));
            RETURN_IF_EXCEPTION(scope, { });
            JSValue value = asDict(dict)->get(globalObject, name);
            RETURN_IF_EXCEPTION(scope, { });
            if (!value)
                break;
            ++present;
        }
    }
    PyTuple* arguments = PyTuple::create(globalObject, present);
    for (unsigned i = 0; i < present; ++i)
        arguments->initializeAt(vm, i, jsUndefined());
    return JSValue::encode(PyTuple::create(globalObject, { type, arguments, dict }));
}

// ast_type_replace_check()
static bool checkReplacement(JSGlobalObject* globalObject, JSValue self, JSValue dict, JSValue fields, JSValue attributes, const NativeArguments& args)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, self);
    PySet* expecting = fields ? setFromIterable(globalObject, realm->typeSet()->instanceStructure(), fields) : PySet::create(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    auto forEachAttribute = [&] (const auto& function) {
        if (!attributes)
            return;
        forEach(globalObject, attributes, [&] (JSValue name) {
            function(name);
            return !scope.exception();
        });
    };
    forEachAttribute([&] (JSValue name) { expecting->add(globalObject, name); });
    RETURN_IF_EXCEPTION(scope, false);

    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        bool wasExpected = !!expecting->remove(globalObject, args.keywordName(i));
        RETURN_IF_EXCEPTION(scope, false);
        if (!wasExpected) {
            raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), ".__replace__ got an unexpected keyword argument "_s, repr(globalObject, args.keywordName(i)), '.'));
            return false;
        }
    }
    if (dict) {
        asDict(dict)->forEach(globalObject, [&] (JSValue key, JSValue) {
            expecting->remove(globalObject, key);
            return !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, false);
        forEachAttribute([&] (JSValue name) { expecting->remove(globalObject, name); });
        RETURN_IF_EXCEPTION(scope, false);
    }
    JSValue fieldTypes = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_field_types"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (fieldTypes && isDict(fieldTypes)) {
        asDict(fieldTypes)->forEach(globalObject, [&] (JSValue name, JSValue fieldType) {
            if (isUnion(globalObject, fieldType))
                expecting->remove(globalObject, name);
            return !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, false);
    }

    if (!expecting->size())
        return true;
    Vector<String> names;
    for (unsigned i = 0; i < expecting->entryCount(); ++i) {
        if (JSValue name = expecting->keyAt(i)) {
            names.append(repr(globalObject, name));
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    std::ranges::sort(names, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
    StringBuilder joined;
    for (auto& name : names)
        joined.append(joined.isEmpty() ? ""_s : ", "_s, name);
    raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), ".__replace__ missing "_s, names.size(), " keyword argument"_s, names.size() == 1 ? ""_s : "s"_s, ": "_s, joined.toString(), '.'));
    return false;
}

// ast_type_replace(): copy.replace()
PYTHON_NATIVE(astReplace)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s));
    JSValue self = args[0];
    PyType* type = typeOf(globalObject, self);
    JSValue fields = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_fields"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attributes = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_attributes"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue dict = dictOfNode(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkReplacement(globalObject, self, dict, fields, attributes, args))
        return { };

    PyDict* payload = PyDict::create(globalObject);
    if (dict) {
        for (JSValue keys : { fields, attributes }) {
            if (!keys)
                continue;
            forEach(globalObject, keys, [&] (JSValue key) {
                JSValue value = asDict(dict)->get(globalObject, key);
                if (value && !scope.exception())
                    payload->set(globalObject, key, value);
                return !scope.exception();
            });
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        payload->set(globalObject, args.keywordNameAsGiven(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    MarkedArgumentBuffer none;
    RELEASE_AND_RETURN(scope, JSValue::encode(callWithKeywordDict(globalObject, type, none, payload)));
}

static String reprOfNode(JSGlobalObject*, JSValue self, int depth);

static bool isNode(JSGlobalObject* globalObject, JSValue value)
{
    ASTState* state = astState(globalObject);
    return state && typeOf(globalObject, value)->isSubtypeOf(state->classFor(ASTClass::AST));
}

// ast_repr_list(). Only the first and the last are shown.
static String reprOfNodes(JSGlobalObject* globalObject, JSValue list, int depth)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = length(globalObject, list);
    RETURN_IF_EXCEPTION(scope, { });
    if (!size)
        RELEASE_AND_RETURN(scope, repr(globalObject, list));
    bool isAList = isList(list);
    StringBuilder result;
    result.append(isAList ? '[' : '(');
    for (int64_t i = 0; i < std::min<int64_t>(size, 2); ++i) {
        JSValue item = getItem(globalObject, list, intFromInt64(globalObject, i ? size - 1 : 0));
        RETURN_IF_EXCEPTION(scope, { });
        if (i)
            result.append(", "_s);
        result.append(isNode(globalObject, item) ? reprOfNode(globalObject, item, depth - 1) : repr(globalObject, item));
        RETURN_IF_EXCEPTION(scope, { });
        if (!i && size > 2)
            result.append(", ..."_s);
    }
    result.append(isAList ? ']' : ')');
    return result.toString();
}

// ast_repr_max_depth()
static String reprOfNode(JSGlobalObject* globalObject, JSValue self, int depth)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, self);
    String name = type->nameString(globalObject);
    if (depth <= 0)
        return concatenate(name, "(...)"_s);
    ReprGuard guard(globalObject, self.asCell());
    if (guard.isRecursive())
        return concatenate(name, "(...)"_s);

    JSValue fields = getAttributeIfPresent(globalObject, type, Identifier::fromString(vm, "_fields"_s));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t fieldCount = fields ? length(globalObject, fields) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder result;
    result.append(name, '(');
    for (int64_t i = 0; i < fieldCount; ++i) {
        JSValue fieldName = getItem(globalObject, fields, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        auto property = attributeName(globalObject, scope, fieldName);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue value = getAttribute(globalObject, self, *property);
        RETURN_IF_EXCEPTION(scope, { });
        String text = isList(value) || isTuple(value) ? reprOfNodes(globalObject, value, depth) : isNode(globalObject, value) ? reprOfNode(globalObject, value, depth - 1) : repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        result.append(i ? ", "_s : ""_s, property->string(), '=', text);
    }
    result.append(')');
    return result.toString();
}

PYTHON_NATIVE(astRepr)
{
    NATIVE_PROLOGUE();
    String result = reprOfNode(globalObject, args[0], 3);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, result));
}

// ---- The classes

// asdl_of() of Parser/asdl_c.py: what a class says of itself in __doc__
static void appendFields(StringBuilder& out, const ASDLClass& description)
{
    if (description.fields.empty())
        return;
    out.append('(');
    bool isFirst = true;
    for (auto& field : description.fields) {
        static constexpr ASCIILiteral quantifiers[] = { ""_s, "?"_s, "*"_s, "?*"_s };
        ASCIILiteral type = field.type == ASTClass::identifier ? "identifier"_s : field.type == ASTClass::int_ ? "int"_s : field.type == ASTClass::string ? "string"_s : field.type == ASTClass::constant ? "constant"_s : descriptionOf(field.type).name;
        out.append(isFirst ? ""_s : ", "_s, type, quantifiers[static_cast<unsigned>(field.quantifier)], ' ', field.name);
        isFirst = false;
    }
    out.append(')');
}

template<typename Function>
static void forEachConstructorOf(ASTClass sum, const Function& function)
{
    for (unsigned i = 0; i < numberOfASTClasses; ++i) {
        if (ASDL::classes[i].kind == ASDLClass::Kind::Constructor && ASDL::classes[i].base == sum)
            function(static_cast<ASTClass>(i));
    }
}

static bool isSimpleSum(ASTClass sum)
{
    bool result = true;
    forEachConstructorOf(sum, [&] (ASTClass constructor) { result &= descriptionOf(constructor).fields.empty(); });
    return result;
}

static String docOf(ASTClass astClass)
{
    const ASDLClass& description = descriptionOf(astClass);
    StringBuilder out;
    out.append(description.name);
    if (description.kind != ASDLClass::Kind::Sum) {
        appendFields(out, description);
        return out.toString();
    }
    out.append(" = "_s);
    bool isSimple = isSimpleSum(astClass);
    bool isFirst = true;
    forEachConstructorOf(astClass, [&] (ASTClass constructor) {
        if (!isFirst) {
            if (isSimple)
                out.append(" | "_s);
            else {
                out.append('\n');
                for (size_t i = 0; i <= description.name.length(); ++i)
                    out.append(' ');
                out.append("| "_s);
            }
        }
        isFirst = false;
        out.append(descriptionOf(constructor).name);
        appendFields(out, descriptionOf(constructor));
    });
    return out.toString();
}

static PyTuple* namesOf(JSGlobalObject* globalObject, std::span<const ASDLField> fields)
{
    VM& vm = globalObject->vm();
    PyTuple* result = PyTuple::create(globalObject, fields.size());
    for (size_t i = 0; i < fields.size(); ++i)
        result->initializeAt(vm, i, jsString(vm, String(fields[i].name)));
    return result;
}

// init_types() and add_ast_annotations()
static void initializeASTState(JSGlobalObject* globalObject, ASTState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    auto identifier = [&] (ASCIILiteral name) { return Identifier::fromString(vm, name); };
    JSString* moduleName = jsNontrivialString(vm, "ast"_s);

    // make_type()
    auto makeType = [&] (ASCIILiteral name, PyType* base, PyDict* contents) -> PyType* {
        JSValue result = newType(globalObject, realm->typeType(), jsString(vm, String(name)), PyTuple::create(globalObject, { base }), contents, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return asType(result);
    };

    {
        // Its instances have a __dict__, and there are no weak references to them. Those of what is derived from it can have both.
        PyDict* contents = PyDict::create(globalObject);
        contents->setString(globalObject, "__slots__"_s, PyTuple::create(globalObject, { jsNontrivialString(vm, "__dict__"_s) }));
        contents->setString(globalObject, "__module__"_s, moduleName);
        PyType* type = makeType("AST"_s, realm->typeObject(), contents);
        RETURN_IF_EXCEPTION(scope, void());
        deleteAttribute(globalObject, type, identifier("__slots__"_s));
        RETURN_IF_EXCEPTION(scope, void());
        state.classes[0].set(vm, realm, type);
        // It is only AST itself that says what module it is in.
        type->setDottedName("ast.AST"_s);
        using Kind = PyNativeFunction::Kind;
        addGenericGetAttribute(globalObject, type);
        addGenericSetAttribute(globalObject, type);
        addMethods(globalObject, type, {
            { "__new__"_s, astNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
            { "__init__"_s, astInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
            { "__repr__"_s, astRepr },
            { "__reduce__"_s, astReduce },
            { "__replace__"_s, astReplace, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        });
        PyTuple* none = PyTuple::create(globalObject, 0);
        for (ASCIILiteral name : { "_fields"_s, "__match_args__"_s, "_attributes"_s })
            type->putDirect(vm, identifier(name), none);
    }

    for (unsigned i = 1; i < numberOfASTClasses; ++i) {
        auto astClass = static_cast<ASTClass>(i);
        const ASDLClass& description = ASDL::classes[i];
        PyTuple* fields = namesOf(globalObject, description.fields);
        PyDict* contents = PyDict::create(globalObject);
        contents->setString(globalObject, "_fields"_s, fields);
        contents->setString(globalObject, "__match_args__"_s, fields);
        contents->setString(globalObject, "__module__"_s, moduleName);
        contents->setString(globalObject, "__doc__"_s, jsString(vm, docOf(astClass)));
        PyType* type = makeType(description.name, state.classFor(description.base), contents);
        RETURN_IF_EXCEPTION(scope, void());
        state.classes[i].set(vm, realm, type);

        // add_attributes()
        if (description.kind != ASDLClass::Kind::Constructor)
            type->putDirect(vm, identifier("_attributes"_s), namesOf(globalObject, description.attributes));
        // What may be left out is None if it is.
        for (auto& list : { description.fields, description.attributes }) {
            for (auto& field : list) {
                if (field.quantifier == ASDLField::Quantifier::Optional)
                    type->putDirect(vm, identifier(field.name), jsUndefined());
            }
        }
        if (description.kind == ASDLClass::Kind::Constructor && isSimpleSum(description.base))
            state.singletons[i].set(vm, realm, PyInstance::create(vm, type->instanceStructure()));
    }

    for (unsigned i = 1; i < numberOfASTClasses; ++i) {
        const ASDLClass& description = ASDL::classes[i];
        if (description.kind == ASDLClass::Kind::Sum)
            continue;
        PyDict* annotations = PyDict::create(globalObject);
        for (auto& field : description.fields) {
            JSValue type;
            switch (field.type) {
            case ASTClass::identifier:
            case ASTClass::string:
                type = realm->typeStr();
                break;
            case ASTClass::int_:
                type = realm->typeInt();
                break;
            case ASTClass::constant:
                type = realm->typeObject();
                break;
            default:
                type = state.classFor(field.type);
                break;
            }
            switch (field.quantifier) {
            case ASDLField::Quantifier::One:
                break;
            case ASDLField::Quantifier::Optional:
                type = unionOf(globalObject, type, jsUndefined());
                break;
            case ASDLField::Quantifier::Sequence:
            case ASDLField::Quantifier::SequenceOfOptional:
                type = newGenericAlias(globalObject, realm->typeList(), type);
                break;
            }
            RETURN_IF_EXCEPTION(scope, void());
            annotations->setString(globalObject, String(field.name), type);
        }
        PyType* type = state.classes[i].get();
        type->putDirect(vm, identifier("_field_types"_s), annotations);
        setAttribute(globalObject, type, names.dunder_annotations, annotations);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

ASTState* astState(JSGlobalObject* globalObject)
{
    ASTState& state = globalObject->pyRealm()->ast();
    if (state.classes.back()) [[likely]]
        return &state;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // While it is being made, what is asked of it is asked of as much as there is.
    if (state.classes[0])
        return &state;
    initializeASTState(globalObject, state);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return &state;
}

bool isAST(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASTState* state = astState(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, isInstanceOf(globalObject, value, state->classFor(ASTClass::AST)));
}

// ---- From what the parser makes

namespace {

// The ast2obj_ functions.
class ObjectMaker : public ASTWalker<ObjectMaker> {
public:
    ObjectMaker(JSGlobalObject* globalObject, ASTState& state)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_state(state)
    {
    }

    JSValue make(Module& module)
    {
        walk(module);
        return m_hasFailed ? JSValue() : m_result;
    }

    void open(ASTClass astClass, const Node&) { open(astClass); }

    void open(ASTClass astClass)
    {
        m_containers.append(PyInstance::create(m_vm, m_state.classFor(astClass)->instanceStructure()));
        m_positions.append({ });
    }

    void name(ASCIILiteral name) { m_positions.last().name = name; }

    void close(const Node& node)
    {
        JSObject* object = asObject(m_containers.last());
        // A program may have said that it is less than nowhere.
        object->putDirect(m_vm, Identifier::fromString(m_vm, "lineno"_s), jsNumber(static_cast<int>(node.line)));
        object->putDirect(m_vm, Identifier::fromString(m_vm, "col_offset"_s), jsNumber(static_cast<int>(node.column)));
        object->putDirect(m_vm, Identifier::fromString(m_vm, "end_lineno"_s), jsNumber(static_cast<int>(node.endLine)));
        object->putDirect(m_vm, Identifier::fromString(m_vm, "end_col_offset"_s), jsNumber(static_cast<int>(node.endColumn)));
        close();
    }

    void close()
    {
        JSValue object = m_containers.last();
        m_containers.removeLast();
        m_positions.removeLast();
        emit(object);
    }

    void null() { emit(jsUndefined()); }
    void identifier(const Identifier& identifier) { emit(jsString(m_vm, identifier.string())); }
    void unicodePrefix() { emit(jsSingleCharacterString(m_vm, static_cast<Latin1Character>('u'))); }

    void text(const Text& text)
    {
        if (!text.isBytes)
            return emit(jsString(m_vm, text.text->string()));
        emit(bytesOf(text.text->string()));
    }
    void integer(int number) { emit(jsNumber(number)); }
    void singleton(ASTClass astClass) { emit(m_state.singletonFor(astClass)); }

    void constant(Constant::Type type)
    {
        switch (type) {
        case Constant::Type::None:
            return emit(jsUndefined());
        case Constant::Type::True:
            return emit(jsBoolean(true));
        case Constant::Type::False:
            return emit(jsBoolean(false));
        case Constant::Type::Ellipsis:
            return emit(m_globalObject->pyRealm()->ellipsis());
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void constant(Constant& node) { emit(valueOf(node)); }

    JSValue valueOf(Constant& node)
    {
        auto emit = [] (JSValue value) { return value; };
        switch (node.type) {
        case Constant::Type::None:
            return jsUndefined();
        case Constant::Type::True:
            return jsBoolean(true);
        case Constant::Type::False:
            return jsBoolean(false);
        case Constant::Type::Ellipsis:
            return m_globalObject->pyRealm()->ellipsis();
        case Constant::Type::Integer:
            if (node.integer <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return emit(intFromInt64(m_globalObject, node.isNegative ? -static_cast<int64_t>(node.integer) : static_cast<int64_t>(node.integer)));
            return emit(parseInt(m_globalObject, concatenate(node.isNegative ? "-"_s : ""_s, node.integer), 10));
        case Constant::Type::BigInteger:
            return emit(parseInt(m_globalObject, concatenate(node.isNegative ? "-"_s : ""_s, node.text->string()), node.radix));
        case Constant::Type::Float:
            return emit(floatFromDouble(node.real));
        case Constant::Type::Imaginary:
            return emit(PyComplex::create(m_globalObject, 0, node.real));
        case Constant::Type::String:
            return emit(jsString(m_vm, node.text->string()));
        case Constant::Type::Bytes:
            return bytesOf(node.text->string());
        case Constant::Type::Complex:
            return PyComplex::create(m_globalObject, node.real, node.imaginary);
        case Constant::Type::Tuple:
        case Constant::Type::FrozenSet: {
            if (!canGoDeeper())
                return { };
            MarkedArgumentBuffer elements;
            for (Constant* element : node.elements)
                elements.append(valueOf(*element));
            PyTuple* tuple = PyTuple::createFromArguments(m_globalObject, elements);
            if (node.type == Constant::Type::Tuple)
                return tuple;
            return setFromIterable(m_globalObject, m_globalObject->pyRealm()->typeFrozenSet()->instanceStructure(), tuple);
        }
        case Constant::Type::Invalid:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void openList(size_t size)
    {
        JSArray* list = newList(m_globalObject, size);
        if (!list) {
            m_hasFailed = true;
            list = newList(m_globalObject);
        }
        m_containers.append(list);
        m_positions.append({ });
    }

    void element() { }

    void closeList() { close(); }

    bool canGoDeeper()
    {
        if (m_hasFailed)
            return false;
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        raiseRecursionError(m_globalObject);
        m_hasFailed = true;
        return false;
    }

private:
    struct Position {
        ASCIILiteral name; // Of a node: the field that comes next.
        unsigned index { 0 }; // Of a list: the element that comes next.
    };

    // Of a character for each.
    JSValue bytesOf(StringView text)
    {
        Vector<uint8_t, 64> bytes;
        bytes.reserveInitialCapacity(text.length());
        for (unsigned i = 0; i < text.length(); ++i)
            bytes.append(static_cast<uint8_t>(text[i]));
        return newBytes(m_globalObject, bytes.span());
    }

    // It is the value of a field, or an element of a list, or the whole.
    void emit(JSValue value)
    {
        if (!value) {
            m_hasFailed = true;
            return;
        }
        if (m_containers.isEmpty()) {
            m_result = value;
            return;
        }
        JSObject* container = asObject(m_containers.last());
        Position& position = m_positions.last();
        if (position.name.isNull())
            listInitializeAt(m_globalObject, uncheckedDowncast<JSArray>(container), position.index++, value);
        else
            container->putDirect(m_vm, Identifier::fromString(m_vm, position.name), value);
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    ASTState& m_state;
    // Those that have begun and not ended.
    MarkedArgumentBuffer m_containers;
    Vector<Position, 32> m_positions;
    JSValue m_result;
    bool m_hasFailed { false };
};

} // anonymous namespace

JSValue objectFromAST(JSGlobalObject* globalObject, Module& module)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASTState* state = astState(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = ObjectMaker(globalObject, *state).make(module);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// ---- To what the parser makes

namespace {

// What the obj2ast_ functions ask of an object, and what they say if it will not do.
class ObjectSource {
public:
    using Value = JSValue;
    static constexpr bool attributesComeFirst = false;

    ObjectSource(JSGlobalObject* globalObject, ASTState& state, Arena& arena)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_state(state)
        , m_arena(arena)
    {
    }

    bool hasFailed() const { return m_hasFailed; }

    void fail(ASTError::Kind kind, String&& message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        m_hasFailed = true;
        raiseASTError(m_globalObject, { kind, WTF::move(message) });
        scope.release();
    }

    bool isNone(JSValue value) { return Python::isNone(value); }
    void leave(JSValue, Node&) { }

    bool canGoDeeper(ASTClass owner)
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        fail(ASTError::Kind::RecursionError, concatenate("maximum recursion depth exceeded while traversing '"_s, descriptionOf(owner).name, "' node"_s));
        return false;
    }

    ASTClass constructorOf(JSValue value, ASTClass sum)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        for (unsigned i = static_cast<unsigned>(sum) + 1; i < numberOfASTClasses && ASDL::classes[i].kind == ASDLClass::Kind::Constructor && ASDL::classes[i].base == sum; ++i) {
            bool isOne = isInstanceOf(m_globalObject, value, m_state.classes[i].get());
            if (scope.exception()) [[unlikely]] {
                m_hasFailed = true;
                return ASTClass::AST;
            }
            if (isOne)
                return static_cast<ASTClass>(i);
        }
        String text = repr(m_globalObject, value);
        if (scope.exception()) [[unlikely]] {
            m_hasFailed = true;
            return ASTClass::AST;
        }
        scope.release();
        fail(ASTError::Kind::TypeError, concatenate("expected some sort of "_s, descriptionOf(sum).name, ", but got "_s, text));
        return ASTClass::AST;
    }

    std::optional<JSValue> field(JSValue node, ASTClass owner, const ASDLField& field)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue value = getAttributeIfPresent(m_globalObject, node, Identifier::fromString(m_vm, field.name));
        if (scope.exception()) [[unlikely]] {
            m_hasFailed = true;
            return std::nullopt;
        }
        if (value)
            return value;
        scope.release();
        if (field.quantifier == ASDLField::Quantifier::One)
            fail(ASTError::Kind::TypeError, concatenate("required field \""_s, field.name, "\" missing from "_s, descriptionOf(owner).name));
        return std::nullopt;
    }

    template<typename Function>
    bool forEachElement(JSValue value, ASTClass owner, const ASDLField& field, const Function& function)
    {
        if (!isList(value)) {
            fail(ASTError::Kind::TypeError, concatenate(descriptionOf(owner).name, " field \""_s, field.name, "\" must be a list, not a "_s, typeOf(m_globalObject, value)->nameWithoutModule(m_globalObject)));
            return false;
        }
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* list = asList(value);
        unsigned size = list->length();
        for (unsigned i = 0; i < size; ++i) {
            JSValue element = listGet(m_globalObject, list, i);
            if (scope.exception()) [[unlikely]] {
                m_hasFailed = true;
                return false;
            }
            if (!function(element))
                return false;
            if (list->length() != size) {
                scope.release();
                fail(ASTError::Kind::RuntimeError, concatenate(descriptionOf(owner).name, " field \""_s, field.name, "\" changed size during iteration"_s));
                return false;
            }
        }
        return true;
    }

    const Identifier* identifier(JSValue value)
    {
        if (Python::isNone(value))
            return nullptr;
        if (!value.isString()) {
            fail(ASTError::Kind::TypeError, "AST identifier must be of type str"_s);
            return nullptr;
        }
        return textOf(asString(value));
    }

    // bytes will do as well, whatever it says.
    Text string(JSValue value)
    {
        if (value.isString())
            return { textOf(asString(value)), false };
        if (isExactly(m_globalObject, value, BuiltinType::Bytes)) {
            Buffer buffer { value };
            return { &m_arena.identifiers().makeIdentifier(m_vm, byteCast<Latin1Character>(buffer.span())), true };
        }
        fail(ASTError::Kind::TypeError, "AST string must be of type str"_s);
        return { };
    }

    std::optional<int> integer(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!classify(value).isInt()) {
            String text = repr(m_globalObject, value);
            if (scope.exception()) [[unlikely]] {
                m_hasFailed = true;
                return std::nullopt;
            }
            scope.release();
            fail(ASTError::Kind::ValueError, concatenate("invalid integer value: "_s, text));
            return std::nullopt;
        }
        auto result = toCInt(m_globalObject, value);
        if (scope.exception()) [[unlikely]]
            m_hasFailed = true;
        return result;
    }

    // Anything will do for now. Whether it can be a constant is looked into with the rest, by validate().
    Constant* constant(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyRealm* realm = m_globalObject->pyRealm();
        auto* result = m_arena.create<Constant>();
        PyType* type = typeOf(m_globalObject, value);
        if (Python::isNone(value))
            return result;
        if (value.isBoolean()) {
            result->type = value.asBoolean() ? Constant::Type::True : Constant::Type::False;
            return result;
        }
        if (value == realm->ellipsis()) {
            result->type = Constant::Type::Ellipsis;
            return result;
        }
        if (value.isString()) {
            result->type = Constant::Type::String;
            result->text = textOf(asString(value));
            return result;
        }
        if (type == realm->typeInt()) {
            Number number = classify(value);
            if (number.kind == Number::Kind::Small) {
                result->type = Constant::Type::Integer;
                result->isNegative = number.small < 0;
                result->integer = std::abs(static_cast<int64_t>(number.small));
                return result;
            }
            result->type = Constant::Type::BigInteger;
            result->isNegative = number.big->sign();
            result->radix = 16;
            String digits = number.big->toString(m_globalObject, 16);
            if (scope.exception()) [[unlikely]] {
                m_hasFailed = true;
                return nullptr;
            }
            result->text = textOf(result->isNegative ? StringView(digits).substring(1) : StringView(digits));
            return result;
        }
        if (type == realm->typeFloat()) {
            result->type = Constant::Type::Float;
            result->real = classify(value).real;
            return result;
        }
        if (type == realm->typeComplex()) {
            auto* complex = uncheckedDowncast<PyComplex>(value.asCell());
            result->type = Constant::Type::Complex;
            result->real = complex->real();
            result->imaginary = complex->imaginary();
            return result;
        }
        if (type == realm->typeBytes()) {
            result->type = Constant::Type::Bytes;
            Buffer buffer { value };
            result->text = &m_arena.identifiers().makeIdentifier(m_vm, byteCast<Latin1Character>(buffer.span()));
            return result;
        }
        if (type == realm->typeTuple() || type == realm->typeFrozenSet()) {
            result->type = type == realm->typeTuple() ? Constant::Type::Tuple : Constant::Type::FrozenSet;
            if (!m_vm.isSafeToRecurse()) [[unlikely]] {
                scope.release();
                fail(ASTError::Kind::RecursionError, "maximum recursion depth exceeded during compilation"_s);
                return nullptr;
            }
            Vector<Constant*, 8> elements;
            forEach(m_globalObject, value, [&] (JSValue element) {
                Constant* converted = constant(element);
                if (converted)
                    elements.append(converted);
                return !!converted;
            });
            if (scope.exception() || m_hasFailed) [[unlikely]] {
                m_hasFailed = true;
                return nullptr;
            }
            result->elements = m_arena.copy(elements);
            return result;
        }
        result->type = Constant::Type::Invalid;
        result->text = textOf(type->nameWithoutModule(m_globalObject));
        return result;
    }

private:
    const Identifier* textOf(JSString* string)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto view = string->view(m_globalObject);
        if (scope.exception()) [[unlikely]] {
            m_hasFailed = true;
            return nullptr;
        }
        return textOf(view);
    }

    const Identifier* textOf(StringView view)
    {
        if (view.is8Bit())
            return &m_arena.identifiers().makeIdentifier(m_vm, view.span8());
        return &m_arena.identifiers().makeIdentifier(m_vm, view.span16());
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    ASTState& m_state;
    Arena& m_arena;
    bool m_hasFailed { false };
};

} // anonymous namespace

void raiseASTError(JSGlobalObject* globalObject, const ASTError& error)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    BuiltinType type = BuiltinType::ValueError;
    switch (error.kind) {
    case ASTError::Kind::ValueError:
        break;
    case ASTError::Kind::TypeError:
        type = BuiltinType::TypeError;
        break;
    case ASTError::Kind::SystemError:
        type = BuiltinType::SystemError;
        break;
    case ASTError::Kind::RecursionError:
        type = BuiltinType::RecursionError;
        break;
    case ASTError::Kind::RuntimeError:
        type = BuiltinType::RuntimeError;
        break;
    }
    raise(globalObject, scope, type, error.message);
}

// PyAST_obj2mod(), and then _PyAST_Validate()
Module* astFromObject(JSGlobalObject* globalObject, Arena& arena, JSValue object, Module::Kind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!audit(globalObject, "compile"_s, object, jsUndefined()))
        return nullptr;
    ASTState* state = astState(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    // PyAst_CheckMode()
    ASTClass required = kind == Module::Kind::Module ? ASTClass::Module : kind == Module::Kind::Expression ? ASTClass::Expression : kind == Module::Kind::Interactive ? ASTClass::Interactive : ASTClass::FunctionType;
    bool isRequired = isInstanceOf(globalObject, object, state->classFor(required));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!isRequired) {
        raiseTypeError(globalObject, scope, concatenate("expected "_s, descriptionOf(required).name, " node, got "_s, typeOf(globalObject, object)->nameWithoutModule(globalObject)));
        return nullptr;
    }

    ObjectSource source(globalObject, *state, arena);
    Module* module = ASTBuilder<ObjectSource>(arena, source).buildModule(object);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT(module && !source.hasFailed());
    if (ASTError error = validate(vm, *module)) {
        scope.release();
        raiseASTError(globalObject, error);
        return nullptr;
    }
    return module;
}

// ---- The module

JSObject* createASTModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASTState* state = astState(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* module = newBuiltinModule(globalObject, "_ast"_s);
    module->putDirect(vm, Identifier::fromString(vm, "AST"_s), state->classFor(ASTClass::AST));
    module->putDirect(vm, Identifier::fromString(vm, "PyCF_ALLOW_TOP_LEVEL_AWAIT"_s), jsNumber(static_cast<int>(AllowTopLevelAwait)));
    module->putDirect(vm, Identifier::fromString(vm, "PyCF_ONLY_AST"_s), jsNumber(0x400));
    module->putDirect(vm, Identifier::fromString(vm, "PyCF_TYPE_COMMENTS"_s), jsNumber(0x1000));
    module->putDirect(vm, Identifier::fromString(vm, "PyCF_OPTIMIZED_AST"_s), jsNumber(0x8400));
    for (unsigned i = 1; i < numberOfASTClasses; ++i)
        module->putDirect(vm, Identifier::fromString(vm, ASDL::classes[i].name), state->classes[i].get());
    return module;
}

} } // namespace JSC::Python
