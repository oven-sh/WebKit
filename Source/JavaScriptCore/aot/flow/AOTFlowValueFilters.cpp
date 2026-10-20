/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowValue.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowObjects.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

BitVector findCallableClasses(const Program&);

static constexpr uint16_t plainPrimitives = Primitive::Undefined | Primitive::Null | Primitive::False | Primitive::True | Primitive::WholeNumber | Primitive::FractionalNumber | Primitive::BigInt | Primitive::Absent;
static constexpr uint16_t keyPrimitives = Primitive::String | Primitive::Symbol;
static constexpr uint16_t literalPrimitives = Primitive::LiteralString | Primitive::LiteralSymbol;

static_assert((plainPrimitives | keyPrimitives | literalPrimitives | Primitive::Integer) == Primitive::Defined);

static bool hasLiteral(const Type& type, StringID literal)
{
    return type.stringLiterals.contains(literal);
}

static bool hasLiteral(const Type& type, PropertyKeyID literal)
{
    return type.symbolLiterals.contains(literal);
}

template<typename Literal>
static Vector<Literal, 32> literalsInside(std::span<const Literal> literals, const Type& type)
{
    Vector<Literal, 32> retained;
    for (Literal literal : literals) {
        if (hasLiteral(type, literal))
            retained.append(literal);
    }
    return retained;
}

template<typename Literal>
static Vector<Literal, 32> literalsOutside(std::span<const Literal> literals, const Type& type)
{
    Vector<Literal, 32> retained;
    for (Literal literal : literals) {
        if (!hasLiteral(type, literal))
            retained.append(literal);
    }
    return retained;
}

static bool hasInstanceTest(const BuiltinClass& builtinClass)
{
    switch (builtinClass.instanceTest) {
    case BuiltinInstanceTest::None:
        return false;
    case BuiltinInstanceTest::JSTypeEquals:
    case BuiltinInstanceTest::ClassInfoInherits:
    case BuiltinInstanceTest::SharingModeEquals:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool haveSameCellTypeTest(const BuiltinClass& a, const BuiltinClass& b)
{
    return a.instanceTest == BuiltinInstanceTest::JSTypeEquals && b.instanceTest == BuiltinInstanceTest::JSTypeEquals && a.type == b.type;
}

static bool mayExtendBuiltinClass(const Layout& layout)
{
    return layout.has(LayoutFlags::HasHeritage);
}

static bool containsEveryObject(const Type& type)
{
    return type.anyObject && type.anyFunction;
}

static bool mayContainObjects(const Type& type)
{
    return type.hasObjects() || type.environment;
}

BitVector findCallableClasses(const Program& program)
{
    unsigned count = program.builtins().classCount();
    BitVector callableClasses(count);
    for (unsigned index = 0; index < count; ++index) {
        if (Type::builtinClass(BuiltinClassID(index)).overlaps(Type::function(), program))
            callableClasses.quickSet(index);
    }
    return callableClasses;
}

namespace {

class TypeMembership {
public:
    TypeMembership(const Program& program, const FlowObjectTable& objects, const BitVector& callableClasses, const Type& type)
        : m_program(program)
        , m_objects(objects)
        , m_callableClasses(callableClasses)
        , m_type(type)
        , m_hasUntestableClass(std::ranges::any_of(type.classes, [&](const ClassPart& part) {
            return !hasInstanceTest(program.builtins().at(part.builtinClass));
        }))
        , m_hasCallableClass(std::ranges::any_of(type.classes, [&](const ClassPart& part) {
            return callableClasses.quickGet(part.builtinClass.index());
        }))
    {
        ASSERT(!type.anyObject);
    }

    bool mayContain(FlowObjectID) const;
    bool alwaysContains(FlowObjectID) const;
    bool hasLayoutOf(FlowObjectID) const;

private:
    bool hasLayoutMember(LayoutID) const;
    bool mayContainLayout(LayoutID) const;
    bool mayContainClosureAsBuiltin(FunctionID) const;
    bool mayContainInstance(BuiltinClassID instanceClass) const;
    bool alwaysContainsInstance(BuiltinClassID instanceClass) const;
    bool hasPartAccepting(BuiltinClassID instanceClass) const;
    bool hasSubclassPart(BuiltinClassID instanceClass) const;
    bool hasPlainPart(BuiltinClassID) const;
    bool anyOfClassChain(BuiltinClassID, NOESCAPE const Invocable<bool(BuiltinClassID)> auto& predicate) const;

    const Program& m_program;
    const FlowObjectTable& m_objects;
    const BitVector& m_callableClasses;
    const Type& m_type;
    bool m_hasUntestableClass;
    bool m_hasCallableClass;
};

bool TypeMembership::hasLayoutMember(LayoutID layout) const
{
    return std::ranges::binary_search(membersOf(m_program, m_type), layout);
}

bool TypeMembership::hasLayoutOf(FlowObjectID object) const
{
    const FlowObject& record = m_objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Allocation:
    case FlowObjectKind::ModuleNamespace:
        return record.layout && hasLayoutMember(record.layout);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool TypeMembership::hasPartAccepting(BuiltinClassID instanceClass) const
{
    const BuiltinTable& table = m_program.builtins();
    const BuiltinClass& instance = table.at(instanceClass);
    return std::ranges::any_of(m_type.classes, [&](const ClassPart& part) {
        return part.builtinClass == instanceClass || haveSameCellTypeTest(instance, table.at(part.builtinClass));
    });
}

bool TypeMembership::hasSubclassPart(BuiltinClassID instanceClass) const
{
    const BuiltinTable& table = m_program.builtins();
    return std::ranges::any_of(m_type.classes, [&](const ClassPart& part) {
        return table.isSameOrSubclassOf(part.builtinClass, instanceClass);
    });
}

bool TypeMembership::hasPlainPart(BuiltinClassID builtinClass) const
{
    return hasInstanceTest(m_program.builtins().at(builtinClass)) && Type::builtinClass(builtinClass).isSubsetOf(m_type, m_program);
}

bool TypeMembership::anyOfClassChain(BuiltinClassID builtinClass, NOESCAPE const Invocable<bool(BuiltinClassID)> auto& predicate) const
{
    const BuiltinTable& table = m_program.builtins();
    for (BuiltinClassID current = builtinClass; current; current = table.at(current).parent) {
        if (predicate(current))
            return true;
    }
    return false;
}

bool TypeMembership::mayContain(FlowObjectID object) const
{
    const FlowObject& record = m_objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        return mayContainObjects(m_type);
    case FlowObjectKind::Allocation:
        return mayContainLayout(record.layout);
    case FlowObjectKind::Closure:
        return m_type.anyFunction || m_hasUntestableClass || m_hasCallableClass || m_type.functions.contains(record.function) || mayContainClosureAsBuiltin(record.function);
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return mayContainBuiltin(m_program.types, m_type, record.builtin);
    case FlowObjectKind::Instance:
        return mayContainInstance(record.builtinClass);
    case FlowObjectKind::Opaque:
        return record.builtinClass ? mayContainInstance(record.builtinClass) : m_type.hasObjects();
    case FlowObjectKind::ModuleNamespace:
        return record.layout ? mayContainLayout(record.layout) : m_hasUntestableClass;
    case FlowObjectKind::Prototype:
        return m_hasUntestableClass;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool TypeMembership::mayContainClosureAsBuiltin(FunctionID function) const
{
    return std::ranges::any_of(m_type.builtins, [&](BuiltinID builtin) {
        return mayBeClosureOf(m_program.types, builtin, function);
    });
}

bool TypeMembership::mayContainLayout(LayoutID layout) const
{
    const Layout& record = m_program.at(layout);
    if (record.isEnvironment())
        return m_type.environment == layout;
    if (m_hasUntestableClass || hasLayoutMember(layout))
        return true;
    return mayExtendBuiltinClass(record) && !m_type.classes.isEmpty();
}

bool TypeMembership::mayContainInstance(BuiltinClassID instanceClass) const
{
    if (m_hasUntestableClass || hasSubclassPart(instanceClass))
        return true;
    return anyOfClassChain(instanceClass, [&](BuiltinClassID candidate) {
        return hasPartAccepting(candidate) || (m_type.anyFunction && m_callableClasses.quickGet(candidate.index()));
    });
}

bool TypeMembership::alwaysContains(FlowObjectID object) const
{
    const FlowObject& record = m_objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Closure:
        return m_type.anyFunction || m_type.functions.contains(record.function);
    case FlowObjectKind::Builtin:
        return Type::forBuiltin(record.builtin).isSubsetOf(m_type, m_program);
    case FlowObjectKind::Instance:
        return alwaysContainsInstance(record.builtinClass);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool TypeMembership::alwaysContainsInstance(BuiltinClassID instanceClass) const
{
    const BuiltinTable& table = m_program.builtins();
    BuiltinClassID nearestTestedClass;
    for (BuiltinClassID current = instanceClass; current; current = table.at(current).parent) {
        if (current == BuiltinClasses::Function && m_type.anyFunction)
            return true;
        if (!hasInstanceTest(table.at(current)))
            continue;
        if (!nearestTestedClass)
            nearestTestedClass = current;
        if (hasPlainPart(current) && (current == nearestTestedClass || haveSameCellTypeTest(table.at(current), table.at(nearestTestedClass))))
            return true;
    }
    return false;
}

} // anonymous namespace

FlowValue FlowValueTable::filter(const FlowValue& value, const Type& target)
{
    bool removesAbsent = value.primitives & ~target.primitives & Primitive::Absent;
    if (target.isAnything() && !removesAbsent)
        return value;

    FlowValue result;
    result.primitives = value.primitives & target.primitives & plainPrimitives;
    setIntegers(result, rangeOf(value.integers).intersectWith(target.integers));
    filterLiterals(value, target, result);

    if (value.objects.isEmpty() || !mayContainObjects(target))
        return result;
    if (containsEveryObject(target)) {
        result.objects = value.objects;
        return result;
    }

    TypeMembership membership(m_program, m_objects, m_callableClasses, target);
    value.objects.forEach([&](FlowObjectID object) {
        if (membership.mayContain(object))
            result.objects.add(object);
    });
    return result;
}

void FlowValueTable::filterLiterals(const FlowValue& value, const Type& target, FlowValue& result)
{
    if (!(value.primitives & (keyPrimitives | literalPrimitives)))
        return;
    result.primitives |= value.primitives & target.primitives & keyPrimitives;

    if (target.primitives & Primitive::String)
        setStrings(result, value.strings, Subject { });
    else if (value.primitives & Primitive::String)
        setStrings(result, m_stringSets.intern(target.stringLiterals.span()), Subject { });
    else
        setStrings(result, m_stringSets.intern(literalsInside(stringsOf(value.strings), target).span()), Subject { });

    if (target.primitives & Primitive::Symbol)
        setSymbols(result, value.symbols, Subject { });
    else if (value.primitives & Primitive::Symbol)
        setSymbols(result, m_symbolSets.intern(target.symbolLiterals.span()), Subject { });
    else
        setSymbols(result, m_symbolSets.intern(literalsInside(symbolsOf(value.symbols), target).span()), Subject { });
}

FlowValue FlowValueTable::filterOut(const FlowValue& value, const Type& tested)
{
    FlowValue result;
    result.primitives = value.primitives & (plainPrimitives | keyPrimitives) & ~tested.primitives;
    if (!rangeOf(value.integers).isSubsetOf(tested.integers)) {
        result.integers = value.integers;
        result.primitives |= Primitive::Integer;
    }
    filterOutLiterals(value, tested, result);

    if (containsEveryObject(tested))
        return result;
    if (!tested.hasObjects()) {
        result.objects = value.objects;
        return result;
    }

    TypeMembership membership(m_program, m_objects, m_callableClasses, tested);
    value.objects.forEach([&](FlowObjectID object) {
        if (!membership.alwaysContains(object))
            result.objects.add(object);
    });
    return result;
}

void FlowValueTable::filterOutLiterals(const FlowValue& value, const Type& tested, FlowValue& result)
{
    if (!(tested.primitives & Primitive::String))
        setStrings(result, m_stringSets.intern(literalsOutside(stringsOf(value.strings), tested).span()), Subject { });
    if (!(tested.primitives & Primitive::Symbol))
        setSymbols(result, m_symbolSets.intern(literalsOutside(symbolsOf(value.symbols), tested).span()), Subject { });
}

bool FlowValueTable::isInside(const FlowValue& value, const Type& type) const
{
    if (value.primitives & (plainPrimitives | keyPrimitives) & ~type.primitives)
        return false;
    if (!rangeOf(value.integers).isSubsetOf(type.integers))
        return false;

    auto isLiteralOfType = [&](auto literal) {
        return hasLiteral(type, literal);
    };
    if (!(type.primitives & Primitive::String) && !std::ranges::all_of(stringsOf(value.strings), isLiteralOfType))
        return false;
    if (!(type.primitives & Primitive::Symbol) && !std::ranges::all_of(symbolsOf(value.symbols), isLiteralOfType))
        return false;

    if (value.objects.isEmpty() || containsEveryObject(type))
        return true;
    if (!type.hasObjects())
        return false;

    TypeMembership membership(m_program, m_objects, m_callableClasses, type);
    return value.objects.allOf([&](FlowObjectID object) {
        return membership.alwaysContains(object) || membership.hasLayoutOf(object);
    });
}

bool FlowValueTable::isObjectInside(FlowObjectID object, const Type& type) const
{
    if (object == unknownFlowObject)
        return false;
    if (containsEveryObject(type))
        return true;

    TypeMembership membership(m_program, m_objects, m_callableClasses, type);
    return membership.alwaysContains(object) || membership.hasLayoutOf(object);
}

bool FlowValueTable::isObjectOutside(FlowObjectID object, const Type& type) const
{
    if (object == unknownFlowObject || containsEveryObject(type))
        return false;
    return !TypeMembership(m_program, m_objects, m_callableClasses, type).mayContain(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
