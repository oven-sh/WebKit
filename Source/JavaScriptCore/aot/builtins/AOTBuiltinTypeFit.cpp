/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTypes.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ClassTestedByRow {
    BuiltinClassID builtinClass;
    PristineAspectSet pristine;
    bool asksForMoreThanClass { false };

    unsigned demands() const
    {
        if (asksForMoreThanClass)
            return 2;
        return pristine.isEmpty() ? 0 : 1;
    }
};

struct FitWithoutPristineProofs {
    TypeFit fit { TypeFit::Unknown };
    bool needsPristineProof { false };
    bool fitsIfPristine { false };
};

struct PristineAspectsOfOperand {
    BuiltinClassID builtinClass;
    PristineAspectSet asked;
};

class BuiltinTypeFitWalk {
public:
    BuiltinTypeFitWalk(const BuiltinTypeContext& context, const Type& operand, const BuiltinType& tested)
        : m_context(context)
        , m_table(context.program.builtins())
        , m_reader(context.types)
        , m_operand(operand)
        , m_tested(tested)
    {
    }

    TypeFit run();
    bool needsPristineProof() const { return m_needsPristineProof; }
    bool fitsIfPristine() const { return m_fitsIfPristine; }

private:
    bool has(BuiltinTypeFlag flag) const { return m_tested.flags.contains(flag); }
    bool has(BuiltinPrimitive primitive) const { return m_tested.primitives.contains(primitive); }
    bool hasPart(BuiltinTypePartKind kind) const { return std::ranges::contains(m_tested.parts, kind, &BuiltinTypePart::kind); }
    bool hasNothingLeftToLearn() const { return !m_isOutside && !m_fitsIfPristine; }

    bool namesBuiltinIf(NOESCAPE const Invocable<bool(BuiltinID)> auto& matches) const
    {
        return std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Builtin && matches(m_table.canonicalID(part.builtin));
        });
    }

    void add(TypeFit fit)
    {
        m_isInside &= fit == TypeFit::Inside;
        m_isOutside &= fit == TypeFit::Outside;
        m_fitsIfPristine &= fit == TypeFit::Inside;
    }

    void addLayout(LayoutID layout)
    {
        TypeFit fit = fitOfLayout(layout);
        m_isInside &= fit == TypeFit::Inside;
        m_isOutside &= fit == TypeFit::Outside;
        m_fitsIfPristine &= fit == TypeFit::Inside || isWrittenBelowTestedClass(layout);
    }

    void addObjects();
    void addObjectsWithoutShape();
    void collectTestedObjectParts();

    TypeFit fitOfLiteralStrings() const;
    TypeFit fitOfAnyFunction() const;
    TypeFit fitOfClass(BuiltinClassID);
    TypeFit fitOfLayout(LayoutID) const;
    bool isWrittenBelowTestedClass(LayoutID) const;
    TypeFit fitOfObjectWithoutShape(ObjectID);
    const Type& testedShapes();

    const BuiltinTypeContext& m_context;
    const BuiltinTable& m_table;
    TypeReader m_reader;
    const Type& m_operand;
    const BuiltinType& m_tested;
    Vector<ClassTestedByRow, 4> m_classes;
    Vector<ClassPart, 4> m_classParts;
    std::optional<Type> m_testedShapes;
    bool m_namesLayoutOfOrdinaryObjects { false };
    bool m_takesSomeOrdinaryObjects { false };
    bool m_isInside { true };
    bool m_isOutside { true };
    bool m_needsPristineProof { false };
    bool m_fitsIfPristine { true };
};

} // anonymous namespace

static constexpr TypeFit fitOfTestedAtom(bool isCovered, bool isTouched)
{
    if (isCovered)
        return TypeFit::Inside;
    return isTouched ? TypeFit::Unknown : TypeFit::Outside;
}

static Range integersTestedBy(const BuiltinType& tested)
{
    if (!tested.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || tested.integers.isEmpty())
        return Range::full();
    return { tested.integers.minimum, tested.integers.maximum };
}

static bool isRelativeToOperandsOfCall(const BuiltinTypePart& part)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
    case BuiltinTypePartKind::Layout:
    case BuiltinTypePartKind::Builtin:
        return false;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE Vector<ClassTestedByRow, 4> classesTestedByRow(const BuiltinTable& table, const BuiltinType& tested)
{
    Vector<ClassTestedByRow, 4> classes;
    for (const BuiltinTypePart& part : tested.parts) {
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            classes.append(ClassTestedByRow { part.builtinClass, part.pristine, !part.contents.empty() || part.isOfClassItself });
            break;
        case BuiltinTypePartKind::Layout:
            if (table.at(part.layout).isTuple)
                classes.append(ClassTestedByRow { BuiltinClasses::Array, { }, true });
            break;
        case BuiltinTypePartKind::Builtin:
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        case BuiltinTypePartKind::Operand:
        case BuiltinTypePartKind::Slot:
        case BuiltinTypePartKind::IteratedValues:
        case BuiltinTypePartKind::CallbackResult:
        case BuiltinTypePartKind::Prototype:
            break;
        }
    }
    return classes;
}

static const ClassTestedByRow* findTestedClassAdmitting(const BuiltinTable& table, std::span<const ClassTestedByRow> classes, BuiltinClassID builtinClass)
{
    const ClassTestedByRow* found = nullptr;
    for (const ClassTestedByRow& tested : classes) {
        if (!table.isSameOrSubclassOf(builtinClass, tested.builtinClass))
            continue;
        if (!found || tested.demands() < found->demands())
            found = &tested;
    }
    return found;
}

TypeFit BuiltinTypeFitWalk::run()
{
    for (PrimitiveSet primitive : OptionSet<PrimitiveSet>::fromRaw(m_operand.primitives))
        add(primitive == PrimitiveSet::LiteralString ? fitOfLiteralStrings() : fitOfPrimitiveInBuiltinType(m_tested, primitive, m_operand.integers));
    if (m_operand.hasObjects())
        addObjects();

    if (m_isInside)
        return TypeFit::Inside;
    return m_isOutside ? TypeFit::Outside : TypeFit::Unknown;
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeFit fitOfPrimitiveInBuiltinType(const BuiltinType& tested, PrimitiveSet primitive, Range integers)
{
    auto has = [&](BuiltinPrimitive member) {
        return tested.primitives.contains(member);
    };
    auto fitOfMember = [&](BuiltinPrimitive member) {
        return has(member) ? TypeFit::Inside : TypeFit::Outside;
    };

    switch (primitive) {
    case PrimitiveSet::Undefined:
        return fitOfMember(BuiltinPrimitive::Undefined);
    case PrimitiveSet::Null:
        return fitOfMember(BuiltinPrimitive::Null);
    case PrimitiveSet::False:
    case PrimitiveSet::True:
        return fitOfMember(BuiltinPrimitive::Boolean);
    case PrimitiveSet::Integer: {
        if (!has(BuiltinPrimitive::Number))
            return TypeFit::Outside;
        Range accepted = integersTestedBy(tested);
        return fitOfTestedAtom(integers.isSubsetOf(accepted), !integers.intersectWith(accepted).isEmpty());
    }
    case PrimitiveSet::WholeNumber:
    case PrimitiveSet::FractionalNumber:
        return tested.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) ? TypeFit::Outside : fitOfMember(BuiltinPrimitive::Number);
    case PrimitiveSet::String:
        return fitOfTestedAtom(has(BuiltinPrimitive::String), !tested.stringLiterals.empty());
    case PrimitiveSet::LiteralString:
        break;
    case PrimitiveSet::Symbol:
    case PrimitiveSet::LiteralSymbol:
        return fitOfMember(BuiltinPrimitive::Symbol);
    case PrimitiveSet::BigInt:
        return fitOfMember(BuiltinPrimitive::BigInt);
    case PrimitiveSet::Absent:
    case PrimitiveSet::Unspecified:
        return TypeFit::Outside;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeFit fitOfOmittedOperandInBuiltinType(const BuiltinType& tested)
{
    return tested.primitives.containsAny({ BuiltinPrimitive::NotPassed, BuiltinPrimitive::Undefined }) ? TypeFit::Inside : TypeFit::Outside;
}

TypeFit BuiltinTypeFitWalk::fitOfLiteralStrings() const
{
    if (has(BuiltinPrimitive::String))
        return TypeFit::Inside;
    SetView<StringID, StringSetID> literals = m_reader.at(m_operand.stringLiterals);
    size_t numberOfListedLiterals = static_cast<size_t>(std::ranges::count_if(m_tested.stringLiterals, [&](BuiltinName name) {
        return literals.contains(m_context.program.stringOf(name));
    }));
    return fitOfTestedAtom(numberOfListedLiterals == literals.size(), !!numberOfListedLiterals);
}

void BuiltinTypeFitWalk::collectTestedObjectParts()
{
    m_classes = classesTestedByRow(m_table, m_tested);
    for (const ClassTestedByRow& tested : m_classes) {
        bool isListed = m_classParts.containsIf([&](const ClassPart& part) {
            return part.builtinClass == tested.builtinClass;
        });
        if (!isListed)
            m_classParts.append(ClassPart { tested.builtinClass, { } });
    }
    std::ranges::sort(m_classParts, { }, &ClassPart::builtinClass);

    m_namesLayoutOfOrdinaryObjects = std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Layout && !m_table.at(part.layout).isTuple;
    });
    m_takesSomeOrdinaryObjects = m_namesLayoutOfOrdinaryObjects || has(BuiltinTypeFlag::HasPlainData);
}

void BuiltinTypeFitWalk::addObjects()
{
    if (has(BuiltinTypeFlag::HasAnyObject)) {
        add(TypeFit::Inside);
        return;
    }
    if (!m_tested.mayBeObject()) {
        add(TypeFit::Outside);
        return;
    }
    if (m_operand.anyObject) {
        add(TypeFit::Unknown);
        return;
    }

    collectTestedObjectParts();
    if (m_operand.anyFunction)
        add(fitOfAnyFunction());
    if (m_operand.traitsOfNamed.contains(ObjectTrait::HasNoShape))
        addObjectsWithoutShape();
    for (const ClassPart& part : classPartsOf(m_reader, m_operand))
        add(fitOfClass(part.builtinClass));
    for (LayoutID layout : membersOf(m_reader, m_operand))
        addLayout(layout);
}

TypeFit BuiltinTypeFitWalk::fitOfAnyFunction() const
{
    bool mayBeInstance = std::ranges::any_of(m_classes, [&](const ClassTestedByRow& tested) {
        return isFunctionClass(m_table, tested.builtinClass);
    });
    return fitOfTestedAtom(has(BuiltinTypeFlag::HasAnyFunction), mayBeInstance || hasPart(BuiltinTypePartKind::Builtin));
}

TypeFit BuiltinTypeFitWalk::fitOfClass(BuiltinClassID builtinClass)
{
    bool takesFunctions = has(BuiltinTypeFlag::HasAnyFunction);
    if (takesFunctions && m_table.isSameOrSubclassOf(builtinClass, BuiltinClasses::Function))
        return TypeFit::Inside;

    if (const ClassTestedByRow* admitting = findTestedClassAdmitting(m_table, m_classes.span(), builtinClass)) {
        if (admitting->asksForMoreThanClass)
            return TypeFit::Unknown;
        m_needsPristineProof |= !admitting->pristine.isEmpty();
        return TypeFit::Inside;
    }

    bool mayBeFunction = takesFunctions && isFunctionClass(m_table, builtinClass);
    bool mayBePlainData = has(BuiltinTypeFlag::HasPlainData) && (m_table.isSameOrSubclassOf(BuiltinClasses::Array, builtinClass) || m_table.isSameOrSubclassOf(BuiltinClasses::Object, builtinClass));
    bool mayBeListedObject = m_namesLayoutOfOrdinaryObjects || hasPart(BuiltinTypePartKind::Builtin);
    bool mayBeInstance = std::ranges::any_of(m_classes, [&](const ClassTestedByRow& tested) {
        return m_table.isSameOrSubclassOf(tested.builtinClass, builtinClass) || relate(m_table, builtinClass, tested.builtinClass) == ClassRelation::Unknown;
    });
    return fitOfTestedAtom(false, mayBeFunction || mayBePlainData || mayBeListedObject || mayBeInstance);
}

TypeFit BuiltinTypeFitWalk::fitOfLayout(LayoutID layout) const
{
    bool isNamed = std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Layout && m_context.program.layoutOf(part.layout) == layout;
    });
    if (isNamed && !m_operand.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return TypeFit::Inside;
    if (isNamed || m_takesSomeOrdinaryObjects)
        return TypeFit::Unknown;

    BuiltinClassID writtenBase = m_context.program.at(layout).writtenBuiltinBase;
    if (!writtenBase)
        return fitOfTestedAtom(false, mayBeInsideSomeClassPart(m_reader, layout, m_classParts.span()));
    return fitOfTestedAtom(false, std::ranges::any_of(m_classes, [&](const ClassTestedByRow& tested) {
        return tested.pristine.isEmpty() || !m_table.isSameOrSubclassOf(writtenBase, tested.builtinClass);
    }));
}

bool BuiltinTypeFitWalk::isWrittenBelowTestedClass(LayoutID layout) const
{
    const Layout& record = m_context.program.at(layout);
    if (!record.writtenBuiltinBase)
        return false;
    return std::ranges::any_of(m_classes, [&](const ClassTestedByRow& tested) {
        if (tested.asksForMoreThanClass || !m_table.isSameOrSubclassOf(record.writtenBuiltinBase, tested.builtinClass))
            return false;
        return !tested.pristine.isEmpty() || !record.mayBeOrdinaryObject;
    });
}

const Type& BuiltinTypeFitWalk::testedShapes()
{
    if (!m_testedShapes) {
        TypeArena& types = m_context.types;
        TypeParts parts;
        parts.anyFunction = has(BuiltinTypeFlag::HasAnyFunction);
        if (!m_classParts.isEmpty())
            parts.unnamedClasses = types.internClassParts(m_classParts.span());
        m_testedShapes = types.at(types.intern(parts));
    }
    return *m_testedShapes;
}

void BuiltinTypeFitWalk::addObjectsWithoutShape()
{
    SetView<ObjectID, ObjectSetID> named = m_reader.at(m_operand.objects);
    bool namesOnlyClosures = (m_operand.traitsOfNamed & traitsOfIdentity).containsOnly({ ObjectTrait::IsClosure });
    bool tellsClosuresApart = hasPart(BuiltinTypePartKind::Builtin) && !has(BuiltinTypeFlag::HasAnyFunction);
    if (namesOnlyClosures && !tellsClosuresApart) {
        add(fitOfObjectWithoutShape(named.first()));
        return;
    }

    named.anyOf([&](ObjectID object) {
        if (m_reader.at(object).has(ObjectTrait::HasNoShape))
            add(fitOfObjectWithoutShape(object));
        return hasNothingLeftToLearn();
    });
}

TypeFit BuiltinTypeFitWalk::fitOfObjectWithoutShape(ObjectID object)
{
    const ObjectTable& objects = m_reader.objects();
    bool isCallable = m_reader.at(object).has(ObjectTrait::IsCallable);
    if (isCallable && has(BuiltinTypeFlag::HasAnyFunction))
        return TypeFit::Inside;

    if (BuiltinID builtin = objects.builtinOf(object)) {
        BuiltinID canonical = m_table.canonicalID(builtin);
        bool isNamed = namesBuiltinIf([&](BuiltinID named) {
            return named == canonical;
        });
        if (isNamed)
            return TypeFit::Inside;
    }

    if (!isCallable && m_takesSomeOrdinaryObjects)
        return TypeFit::Unknown;
    if (mayFitUnknownOf(m_reader, object, testedShapes()))
        return TypeFit::Unknown;
    return fitOfTestedAtom(false, namesBuiltinIf([&](BuiltinID named) {
        return mayBeSameObject(m_reader, object, objects.objectOf(named));
    }));
}

static FitWithoutPristineProofs fitWithoutPristineProofs(const BuiltinTypeContext& context, TypeID operandOrNotPassed, BuiltinTypeID testedID)
{
    const BuiltinTable& table = context.program.builtins();
    if (!testedID || table.at(testedID).isAnything())
        return { TypeFit::Inside, false, true };

    const BuiltinType& tested = table.at(testedID);
    if (std::ranges::any_of(tested.parts, isRelativeToOperandsOfCall))
        return { };
    if (!operandOrNotPassed) {
        TypeFit fit = fitOfOmittedOperandInBuiltinType(tested);
        return { fit, false, fit == TypeFit::Inside };
    }

    Type operand = context.types.at(operandOrNotPassed);
    if (operand.isNever())
        return { TypeFit::Inside, false, true };
    if (!operand.isJavaScriptValue())
        return { };

    BuiltinTypeFitWalk walk(context, operand, tested);
    TypeFit fit = walk.run();
    return { fit, fit == TypeFit::Inside && walk.needsPristineProof(), walk.fitsIfPristine() };
}

static std::optional<PristineAspectsOfOperand> pristineAspectsAskedOf(const BuiltinTypeContext& context, TypeID operandID, BuiltinTypeID testedID)
{
    if (!operandID || !testedID)
        return std::nullopt;
    const ClassPart* part = singleBuiltinClassOf(context.types, context.types.at(operandID));
    if (!part)
        return std::nullopt;
    BuiltinClassID builtinClass = part->builtinClass;

    const BuiltinTable& table = context.program.builtins();
    Vector<ClassTestedByRow, 4> classes = classesTestedByRow(table, table.at(testedID));
    const ClassTestedByRow* admitting = findTestedClassAdmitting(table, classes.span(), builtinClass);
    if (!admitting)
        return std::nullopt;

    return PristineAspectsOfOperand { builtinClass, admitting->pristine };
}

static PristineRequirement unprovenPristineRequirementOf(const PristineAspectsOfOperand& aspects)
{
    if (aspects.asked.isEmpty())
        return { };
    return { aspects.builtinClass, aspects.asked };
}

OperandFit fitOf(const BuiltinTypeContext& context, TypeID operandOrNotPassed, BuiltinTypeID tested)
{
    FitWithoutPristineProofs unproven = fitWithoutPristineProofs(context, operandOrNotPassed, tested);
    if (!unproven.needsPristineProof)
        return { unproven.fit, { } };

    std::optional<PristineAspectsOfOperand> aspects = pristineAspectsAskedOf(context, operandOrNotPassed, tested);
    if (!aspects)
        return { };
    return { TypeFit::Inside, unprovenPristineRequirementOf(*aspects) };
}

bool fitsIfPristine(const BuiltinTypeContext& context, TypeID operandOrNotPassed, BuiltinTypeID tested)
{
    return fitWithoutPristineProofs(context, operandOrNotPassed, tested).fitsIfPristine;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
