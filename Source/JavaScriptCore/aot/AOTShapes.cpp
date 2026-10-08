/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "BuiltinNames.h"
#include "JSCInlines.h"
#include "UnlinkedFunctionExecutable.h"

namespace JSC { namespace AOT {

static Type typeOfNative(StringView name)
{
    struct Native {
        ASCIILiteral name;
        Type type;
    };
    static const Native natives[] = {
        { "Map"_s, TMap }, { "ReadonlyMap"_s, TMap }, { "Set"_s, TSet }, { "ReadonlySet"_s, TSet }, { "WeakMap"_s, TWeakMap }, { "WeakSet"_s, TWeakSet },
        { "RegExp"_s, TRegExp }, { "Promise"_s, TPromise }, { "Date"_s, TDate }, { "ArrayBuffer"_s, TArrayBuffer }, { "DataView"_s, TDataView },
        { "Error"_s, TError }, { "TypeError"_s, TError }, { "RangeError"_s, TError }, { "SyntaxError"_s, TError }, { "EvalError"_s, TError },
        { "ReferenceError"_s, TError }, { "URIError"_s, TError }, { "AggregateError"_s, TError },
        { "Int8Array"_s, typeForTypedArray(Int8ArrayType) }, { "Uint8Array"_s, typeForTypedArray(Uint8ArrayType) },
        { "Uint8ClampedArray"_s, typeForTypedArray(Uint8ClampedArrayType) }, { "Int16Array"_s, typeForTypedArray(Int16ArrayType) },
        { "Uint16Array"_s, typeForTypedArray(Uint16ArrayType) }, { "Int32Array"_s, typeForTypedArray(Int32ArrayType) },
        { "Uint32Array"_s, typeForTypedArray(Uint32ArrayType) }, { "Float32Array"_s, typeForTypedArray(Float32ArrayType) },
        { "Float64Array"_s, typeForTypedArray(Float64ArrayType) }, { "BigInt64Array"_s, typeForTypedArray(BigInt64ArrayType) },
        { "BigUint64Array"_s, typeForTypedArray(BigUint64ArrayType) },
    };
    for (auto& native : natives) {
        if (name == native.name)
            return native.type;
    }
    return TAnyObject;
}

static bool isArray(const Facts& facts, const Facts::Shape& shape)
{
    if (shape.kind != Facts::ShapeKind::Native)
        return false;
    UniquedStringImpl* name = facts.name(shape.name);
    return name && (equal(name, "Array"_s) || equal(name, "ReadonlyArray"_s));
}

static bool isMadeByClassesOfProgramOnly(const Facts& facts, uint32_t constructor)
{
    for (unsigned depth = 0; depth < 64; ++depth) {
        auto shape = facts.shape(constructor);
        if (shape.kind == Facts::ShapeKind::Function)
            return true;
        if (shape.kind != Facts::ShapeKind::Class)
            return false;
        auto base = facts.type(shape.prototype);
        if (base.isEmpty())
            return true;
        if (base.bits || !base.strings.empty() || base.shapes.size() != 1)
            return false;
        constructor = base.shapes[0];
    }
    return false;
}

static Type bitFor(Vector<uint32_t, 8>& listed, uint32_t shape, unsigned firstBit, unsigned numberOfBits)
{
    size_t index = listed.find(shape);
    if (index == notFound) {
        if (listed.size() + 1 >= numberOfBits)
            return Type(1) << (firstBit + numberOfBits - 1);
        index = listed.size();
        listed.append(shape);
    }
    return Type(1) << (firstBit + index);
}

Type Graph::typeForShape(uint32_t number)
{
    if (!isOutermost())
        return outermost().typeForShape(number);
    const Facts& facts = *Facts::shared();
    auto shape = facts.shape(number);
    switch (shape.kind) {
    case Facts::ShapeKind::Record:
    case Facts::ShapeKind::Prototype:
        return TFinalObjectTag | bitFor(m_shapesListed, number, firstShapeBit, numberOfShapeBits);
    case Facts::ShapeKind::Instance:
        if (!isMadeByClassesOfProgramOnly(facts, shape.of))
            return TAnyObject;
        return TFinalObjectTag | bitFor(m_shapesListed, number, firstShapeBit, numberOfShapeBits);
    case Facts::ShapeKind::Function:
    case Facts::ShapeKind::Class:
        if (shape.of && programFunctions()) {
            for (UnlinkedFunctionExecutable* executable : facts.executablesOf(shape.of - 1)) {
                if (uint32_t function = programFunctions()->numberOf(executable))
                    return functionType(function);
            }
        }
        return TFunction;
    case Facts::ShapeKind::Bound:
        return TFunction;
    case Facts::ShapeKind::Native:
        if (isArray(facts, shape))
            return TArrayTag | bitFor(m_arrayShapesListed, number, firstArrayShapeBit, numberOfArrayShapeBits);
        if (UniquedStringImpl* name = facts.name(shape.name))
            return typeOfNative(StringView { name });
        return TAnyObject;
    case Facts::ShapeKind::None:
    case Facts::ShapeKind::Namespace:
    case Facts::ShapeKind::Declared:
    case Facts::ShapeKind::Foreign:
        return TAnyObject;
    }
    return TAnyObject;
}

Type Graph::typeForFact(uint32_t number)
{
    if (!isOutermost())
        return outermost().typeForFact(number);
    if (auto it = m_typesForFacts.find(number); it != m_typesForFacts.end())
        return it->value;
    auto fact = Facts::shared()->type(number);
    Type type = TNone;
    if (fact.isUnknown())
        type = TTop;
    else {
        if (fact.bits & Facts::Undefined)
            type |= TUndefined;
        if (fact.bits & Facts::Null)
            type |= TNull;
        if (fact.bits & (Facts::True | Facts::False))
            type |= TBoolean;
        if (fact.bits & Facts::Int32)
            type |= TInt32;
        if (fact.bits & Facts::Double)
            type |= TNumber;
        if ((fact.bits & Facts::String) || !fact.strings.empty())
            type |= TString;
        if (fact.bits & Facts::Symbol)
            type |= TSymbol;
        if (fact.bits & Facts::BigInt)
            type |= TBigInt;
        for (uint32_t shape : fact.shapes)
            type |= typeForShape(shape);
    }
    m_typesForFacts.add(number, type);
    return type;
}

std::optional<Facts::Function> Graph::factsAboutFunction() const
{
    const Facts* facts = Facts::shared();
    if (!facts)
        return std::nullopt;
    auto number = facts->numberOfFunctionWithCode(m_codeBlock);
    if (!number)
        return std::nullopt;
    return facts->function(*number);
}

Facts::Site Graph::factsAbout(const Node* node)
{
    const Facts* facts = Facts::shared();
    if (!facts)
        return { };
    return facts->site(typeTagOf(node));
}

std::optional<Vector<uint32_t, 8>> Graph::shapesIn(Type type)
{
    Graph& graph = outermost();
    if ((type & TFinalObjectTag) && (type & TShapesNotListed))
        return std::nullopt;
    if ((type & TArrayTag) && (type & TArrayShapesNotListed))
        return std::nullopt;
    Vector<uint32_t, 8> shapes;
    if (type & TFinalObjectTag) {
        for (unsigned i = 0; i < graph.m_shapesListed.size(); ++i) {
            if (type & (Type(1) << (firstShapeBit + i)))
                shapes.append(graph.m_shapesListed[i]);
        }
    }
    if (type & TArrayTag) {
        for (unsigned i = 0; i < graph.m_arrayShapesListed.size(); ++i) {
            if (type & (Type(1) << (firstArrayShapeBit + i)))
                shapes.append(graph.m_arrayShapesListed[i]);
        }
    }
    return shapes;
}

Type Graph::typeOfPropertyInShape(uint32_t number, UniquedStringImpl* name, unsigned depth)
{
    const Facts& facts = *Facts::shared();
    if (depth > 32)
        return TTop;
    Type result = TNone;
    if (const Facts::Field* field = facts.field(number, name)) {
        if (field->flags & Facts::MayBeAccessor)
            return TTop;
        result |= typeForFact(field->type);
        if (field->flags & Facts::IsPresent)
            return result;
    }
    auto shape = facts.shape(number);
    switch (shape.kind) {
    case Facts::ShapeKind::Record:
    case Facts::ShapeKind::Instance:
    case Facts::ShapeKind::Prototype:
        break;
    default:
        return TTop;
    }
    if (shape.flags & (Facts::IsLost | Facts::IsKeyed | Facts::IsReshaped))
        return TTop;
    auto prototype = facts.type(shape.prototype);
    if (prototype.isUnknown())
        return TTop;
    if (prototype.isEmpty())
        return isAbsentFromObjectPrototype(name) ? result | TUndefined : TTop;
    if (prototype.bits & ~static_cast<uint32_t>(Facts::Null))
        return TTop;
    if (prototype.bits & Facts::Null)
        result |= TUndefined;
    for (uint32_t parent : prototype.shapes)
        result |= typeOfPropertyInShape(parent, name, depth + 1);
    return result;
}

std::optional<Type> Graph::typeOfProperty(Type base, UniquedStringImpl* name)
{
    if (!Facts::shared())
        return std::nullopt;
    base &= ~(TOther | TEmpty);
    if (!base || !isSubtype(base, TFinalObject))
        return std::nullopt;
    auto shapes = shapesIn(base);
    if (!shapes)
        return std::nullopt;
    Type result = TNone;
    for (uint32_t shape : *shapes)
        result |= typeOfPropertyInShape(shape, name, 0);
    return result;
}

std::optional<unsigned> Graph::slotOfPlacedField(Type base, UniquedStringImpl* name)
{
    const Facts* facts = Facts::shared();
    if (!facts)
        return std::nullopt;
    base &= ~(TOther | TEmpty);
    if (!base || !isSubtype(base, TFinalObject))
        return std::nullopt;
    auto shapes = shapesIn(base);
    if (!shapes || shapes->isEmpty())
        return std::nullopt;
    std::optional<unsigned> slot;
    for (uint32_t number : *shapes) {
        const Facts::Field* field = facts->field(number, name);
        if (!field || !(field->flags & Facts::IsPlaced))
            return std::nullopt;
        unsigned index = field - facts->shape(number).fields.data();
        if ((slot && *slot != index) || index >= JSFinalObject::maxInlineCapacity)
            return std::nullopt;
        slot = index;
    }
    return slot;
}

Vector<UniquedStringImpl*, 8> Graph::namesPlacedIn(uint32_t number)
{
    const Facts& facts = *Facts::shared();
    Vector<UniquedStringImpl*, 8> names;
    for (auto& field : facts.shape(number).fields) {
        if (!(field.flags & Facts::IsPlaced) || names.size() >= JSFinalObject::maxInlineCapacity)
            break;
        UniquedStringImpl* name = facts.name(field.name);
        if (!name)
            break;
        names.append(name);
    }
    return names;
}

std::optional<Type> Graph::typeOfElements(Type base)
{
    const Facts* facts = Facts::shared();
    if (!facts)
        return std::nullopt;
    base &= ~(TOther | TEmpty);
    if (!base || !isSubtype(base, TArray))
        return std::nullopt;
    auto shapes = shapesIn(base);
    if (!shapes)
        return std::nullopt;
    Type result = TNone;
    for (uint32_t number : *shapes) {
        auto shape = facts->shape(number);
        if (shape.slots.empty() || (shape.flags & Facts::IsLost))
            return std::nullopt;
        result |= typeForFact(shape.slots[0]);
    }
    return result;
}

const KnownFunction* Graph::calleeProvenByFacts(const Node* node) const
{
    if (node->graph != this)
        return node->graph->calleeProvenByFacts(node);
    const Facts* facts = Facts::shared();
    if (!facts || !programFunctions() || node->kind != NodeKind::Bytecode)
        return nullptr;
    bool isConstruct = node->opcode == op_construct;
    if (node->opcode != op_call && node->opcode != op_call_ignore_result && node->opcode != op_tail_call && !isConstruct)
        return nullptr;
    if (!isConstruct && node->opcode != op_tail_call) {
        const Node* callee = node->use(callOperands(node->instruction).callee);
        if (callee->isBytecode(op_get_by_id_direct) && callee->graph == this && m_codeBlock->identifier(callee->as<OpGetByIdDirect>().m_property) == m_vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName()) {
            const KnownFunction* initializer = programFunctions()->function(facts->fieldInitializerCalledBy(m_codeBlock));
            return initializer && initializer->forCall ? initializer : nullptr;
        }
    }
    auto site = facts->site(typeTagOf(node));
    if (site.kind != (isConstruct ? Facts::SiteKind::New : Facts::SiteKind::Call))
        return nullptr;
    auto callee = facts->type(site.callee);
    if (callee.bits || callee.shapes.size() != 1)
        return nullptr;
    auto shape = facts->shape(callee.shapes[0]);
    if ((shape.kind != Facts::ShapeKind::Function && shape.kind != Facts::ShapeKind::Class) || !shape.of)
        return nullptr;
    for (UnlinkedFunctionExecutable* executable : facts->executablesOf(shape.of - 1)) {
        const KnownFunction* function = programFunctions()->function(programFunctions()->numberOf(executable));
        if (!function)
            continue;
        if (isConstruct ? !function->forConstruct || function->executable->isBuiltinDefaultClassConstructor() : !function->forCall)
            return nullptr;
        if (node->opcode == op_tail_call && function->conventionForCall.signature == Signature::List)
            return nullptr;
        return function;
    }
    return nullptr;
}

Type Graph::refinedByFacts(const Node* node, Type computed)
{
    if (!computed || node->kind != NodeKind::Bytecode)
        return computed;
    switch (node->opcode) {
    case op_new_object:
        if (auto site = factsAbout(node); site.kind == Facts::SiteKind::Object)
            return computed & typeForShape(site.number);
        return computed;
    case op_new_array:
    case op_new_array_buffer:
    case op_new_array_with_spread:
        if (auto site = factsAbout(node); site.kind == Facts::SiteKind::Array)
            return computed & typeForShape(site.number);
        return computed;
    case op_call:
    case op_tail_call:
        if (auto site = factsAbout(node); site.kind == Facts::SiteKind::Call)
            return computed & typeForFact(site.number);
        return computed;
    case op_construct:
        if (auto site = factsAbout(node); site.kind == Facts::SiteKind::New)
            return computed & typeForFact(site.number);
        return computed;
    case op_get_from_scope:
        if (auto site = factsAbout(node); site.kind == Facts::SiteKind::Read)
            return computed & (typeForFact(site.number) | TEmpty);
        return computed;
    case op_create_this:
        if (auto function = node->graph->factsAboutFunction())
            return computed & typeForFact(function->thisType);
        return computed;
    case op_get_by_id:
        if (auto type = typeOfProperty(node->use(node->as<OpGetById>().m_base)->type, node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl()))
            return computed & *type;
        return computed;
    case op_get_by_val:
        if (auto type = typeOfElements(node->use(node->as<OpGetByVal>().m_base)->type); type && isSubtype(node->use(node->as<OpGetByVal>().m_property)->type, TNumber))
            return computed & (*type | TUndefined);
        return computed;
    default:
        return computed;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
