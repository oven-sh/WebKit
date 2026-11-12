/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTRepresentation.h"
#include "AOTSchemaEnums.h"
#include "AOTSupportTestCommands.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include "AOTTypeText.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LayoutSetInBrackets {
    TypeReader types;
    LayoutSetID set;

    void dump(PrintStream& out) const
    {
        if (!set) {
            out.print("[]"_s);
            return;
        }
        printLayoutSet(out, types, set);
    }
};

struct FieldOfLayoutText {
    FieldReference field;

    void dump(PrintStream& out) const { out.print('L', field.layout.index(), '.', field.field); }
};

struct LegalRepresentationsText {
    const Type& type;

    void dump(PrintStream& out) const
    {
        CommaPrinter comma;
        out.print('[');
        for (Representation representation : allMembersOf<Representation>) {
            if (isLegalRepresentation(type, representation))
                out.print(comma, nameOf(representation));
        }
        out.print(']');
    }
};

class TypeLawChecker {
    WTF_MAKE_NONCOPYABLE(TypeLawChecker);
public:
    explicit TypeLawChecker(PrintStream& out)
        : m_out(out)
    {
    }

    void check(bool holds, ASCIILiteral name, unsigned i, unsigned j, std::optional<unsigned> k = { })
    {
        if (holds)
            return;
        if (++m_numberOfViolations > maxPrintedViolations)
            return;
        m_out.print("law broken: "_s, name, " t"_s, i, " t"_s, j);
        if (k)
            m_out.print(" t"_s, *k);
        m_out.println();
    }

    void finish()
    {
        if (!m_numberOfViolations)
            m_out.println("laws hold"_s);
    }

private:
    static constexpr unsigned maxPrintedViolations = 20;

    PrintStream& m_out;
    unsigned m_numberOfViolations { 0 };
};

template<typename T>
class TypePairTable {
public:
    TypePairTable(size_t size, NOESCAPE const Invocable<T(unsigned, unsigned)> auto& compute)
        : m_size(size)
    {
        m_entries.reserveInitialCapacity(size * size);
        for (unsigned i = 0; i < size; ++i) {
            for (unsigned j = 0; j < size; ++j)
                m_entries.append(compute(i, j));
        }
    }

    const T& operator()(unsigned i, unsigned j) const { return m_entries[i * m_size + j]; }

private:
    size_t m_size;
    Vector<T> m_entries;
};

using OperationOnTypes = TypeID (*)(TypeArena&, TypeID, TypeID);

} // anonymous namespace

static unsigned numberOfLayoutsInTypeTest(const Program& program)
{
    auto firstAdded = std::ranges::find_if(program.layouts, [](const Layout& layout) {
        return layout.builtinLayout || layout.namespaceOf;
    });
    return static_cast<unsigned>(firstAdded - program.layouts.begin());
}

static std::optional<LayoutID> layoutNamedInTypeTest(StringView text, const Program& program)
{
    if (!text.startsWith('L'))
        return std::nullopt;
    auto index = parseTestNumber<uint32_t>(text.substring(1));
    if (!index || *index >= numberOfLayoutsInTypeTest(program))
        return std::nullopt;
    return LayoutID(*index);
}

static std::optional<Vector<LayoutID>> layoutsNamedInTypeTest(StringView text, const Program& program)
{
    Vector<LayoutID> layouts;
    if (text == "-"_s)
        return layouts;
    for (StringView entry : text.splitAllowingEmptyEntries(',')) {
        auto layout = layoutNamedInTypeTest(entry, program);
        if (!layout)
            return std::nullopt;
        layouts.append(*layout);
    }
    return layouts;
}

static PropertyKeyID findKeyForTypeTest(const Program& program, PropertyKeyKind kind, StringView text, LayoutID owner)
{
    StringID string = program.strings.find(encodeProgramString(text).span());
    if (!string)
        return { };
    return program.types.find(PropertyKey { .kind = kind, .text = string, .owner = owner }).value_or(PropertyKeyID());
}

static std::optional<PropertyKeyID> keyNamedInTypeTest(StringView word, const Program& program)
{
    if (word == "-"_s)
        return PropertyKeyID();
    if (!word.startsWith('#'))
        return findKeyForTypeTest(program, PropertyKeyKind::Text, word, { });
    size_t ownerMark = word.reverseFind('@');
    if (ownerMark == notFound)
        return std::nullopt;
    auto owner = layoutNamedInTypeTest(word.substring(ownerMark + 1), program);
    if (!owner)
        return std::nullopt;
    return findKeyForTypeTest(program, PropertyKeyKind::PrivateName, word.substring(1, ownerMark - 1), *owner);
}

static std::optional<Vector<TypeID>> typesOfTypeTest(TestCommandContext& context)
{
    Program& program = *context.program;
    if (context.functionOfTypesLine.value_or(FunctionID(0u)).index() >= program.functions.size()) {
        context.out.println("no function"_s);
        return std::nullopt;
    }
    Vector<TypeID> types = typesNamedBy(program, context.functionOfTypesLine);
    TypeArena arena(program.types, ArenaKeeps::InProgram);
    for (const String& argument : context.arguments) {
        unsigned offset = 0;
        String error;
        std::optional<TypeID> type = parseType(arena, argument, offset, error);
        if (!type || offset != argument.length()) {
            printBadArgument(context.out, argument);
            return std::nullopt;
        }
        types.append(*type);
    }
    return types;
}

static TypeID unionUnderTest(TypeArena& arena, TypeID a, TypeID b) { return arena.unionOf(a, b); }
static TypeID intersectionUnderTest(TypeArena& arena, TypeID a, TypeID b) { return arena.intersectionOf(a, b); }
static TypeID differenceUnderTest(TypeArena& arena, TypeID a, TypeID b) { return arena.differenceOf(a, b); }
static TypeID castUnderTest(TypeArena& arena, TypeID a, TypeID b) { return arena.castTo(a, b); }

static bool typesHaveUpperBound(const Type& a, const Type& b)
{
    if (a.isJavaScriptValue() && b.isJavaScriptValue())
        return true;
    if (a.opaque == b.opaque && a.environment == b.environment)
        return true;
    constexpr uint16_t markers = Primitive::Absent | Primitive::Unspecified;
    return a.hasOnlyPrimitives(markers) || b.hasOnlyPrimitives(markers);
}

static bool isCanonicalType(TypeArena& arena, TypeID type)
{
    return arena.intern(arena.at(type)) == type;
}

static bool typesHaveSameValues(TypeArena& arena, TypeID a, TypeID b)
{
    return arena.isSubset(a, b) && arena.isSubset(b, a);
}

static std::span<const ClassPart> unnamedClassPartsOfType(const TypeArena& arena, const Type& type)
{
    return type.unnamedClasses ? arena.at(type.unnamedClasses) : std::span<const ClassPart>();
}

static const ClassPart* findClassPartIn(std::span<const ClassPart> parts, BuiltinClassID builtinClass)
{
    auto part = std::ranges::find(parts, builtinClass, &ClassPart::builtinClass);
    return part == parts.end() ? nullptr : &*part;
}

static TypeID typeWithoutWrittenContents(TypeArena& arena, TypeID type)
{
    TypeParts parts = arena.at(type);
    if (!parts.unnamedClasses)
        return type;
    Vector<ClassPart> bareParts;
    for (const ClassPart& part : arena.at(parts.unnamedClasses))
        bareParts.append(ClassPart { part.builtinClass, { } });
    parts.unnamedClasses = arena.internClassParts(bareParts.span());
    return arena.intern(parts);
}

static bool operationIgnoresWrittenContents(OperationOnTypes operation, TypeID a, TypeID b, TypeID result, TypeArena& arena)
{
    TypeID resultWithoutThem = operation(arena, typeWithoutWrittenContents(arena, a), typeWithoutWrittenContents(arena, b));
    return typeWithoutWrittenContents(arena, result) == typeWithoutWrittenContents(arena, resultWithoutThem);
}

static bool agreeOnPartsDeterminedByValues(const Type& a, const Type& b)
{
    return a.primitives == b.primitives && a.integers.isSubsetOf(b.integers) && b.integers.isSubsetOf(a.integers) && a.boundsOfFractions == b.boundsOfFractions
        && a.stringLiterals == b.stringLiterals && a.symbolLiterals == b.symbolLiterals
        && a.opaque == b.opaque && a.environment == b.environment && a.anyObject == b.anyObject;
}

static bool partHoldsContentsOf(const ClassPart& holder, const ClassPart& part)
{
    for (unsigned position = 0; position < mostContentsOfClass; ++position) {
        if (holder.writtenContents[position] && holder.writtenContents[position] != part.writtenContents[position])
            return false;
    }
    return true;
}

static bool objectPartsAreContainedPartByPart(TypeArena& arena, const Type& type, const Type& holder)
{
    if (!arena.objectSets().isSubset(type.objects, holder.objects))
        return false;
    if (holder.anyObject)
        return true;
    if (type.anyObject || (type.anyFunction && !holder.anyFunction))
        return false;
    if (!arena.layoutSets().isSubset(type.unnamedLayouts, holder.unnamedLayouts))
        return false;
    std::span<const ClassPart> partsOfHolder = unnamedClassPartsOfType(arena, holder);
    return std::ranges::all_of(unnamedClassPartsOfType(arena, type), [&](const ClassPart& part) {
        const ClassPart* partOfHolder = findClassPartIn(partsOfHolder, part.builtinClass);
        return partOfHolder && partHoldsContentsOf(*partOfHolder, part);
    });
}

static bool mayHoldPartsOfType(TypeArena& arena, const Type& holder, const Type& type)
{
    const BuiltinTable& builtins = arena.builtins();
    std::span<const ClassPart> partsOfHolder = classPartsOf(arena, holder);
    bool holderHasFunctionClass = std::ranges::any_of(partsOfHolder, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass);
    });
    if (type.mayBeFunction() && holderHasFunctionClass)
        return true;

    SetView<ObjectID, ObjectSetID> namedByHolder;
    if (holder.objects)
        namedByHolder = arena.at(holder.objects);
    bool namesObjectThatOnlyMayBeHeld = type.objects && arena.at(type.objects).anyOf([&](ObjectID object) {
        if (namedByHolder.contains(object) || surelyFitsUnknownOf(arena, object, holder))
            return false;
        return overlap(arena, arena.at(arena.forObject(object)), holder);
    });
    if (namesObjectThatOnlyMayBeHeld)
        return true;

    bool hasLayoutThatOnlyMayBeHeld = type.unnamedLayouts && arena.at(type.unnamedLayouts).anyOf([&](LayoutID layout) {
        return mayBeInsideSomeClassPart(arena, layout, partsOfHolder) && !isInsideSomeClassPart(arena, layout, partsOfHolder);
    });
    if (hasLayoutThatOnlyMayBeHeld)
        return true;

    return std::ranges::any_of(classPartsOf(arena, type), [&](const ClassPart& part) {
        return overlapsAnotherClass(arena, part.builtinClass, holder);
    });
}

static bool typesHaveIncomparableParts(TypeArena& arena, const Type& a, const Type& b)
{
    if ((a.anyObject && !a.objects) || (b.anyObject && !b.objects))
        return false;
    return mayHoldPartsOfType(arena, a, b) || mayHoldPartsOfType(arena, b, a);
}

static bool namesOnlyObjectsOfOperands(TypeArena& arena, const Type& result, const Type& a, const Type& b)
{
    return arena.objectSets().isSubset(result.objects, arena.objectSets().unionOf(a.objects, b.objects));
}

static void freezeTypesOfTypeTest(Program& program)
{
    if (!program.types.isFrozen())
        program.types.freeze();
}

static Range rangeThatNoTypeTestWrites(unsigned index)
{
    int64_t bound = 100003 + index;
    return { -bound, bound };
}

static void checkTypeRelationsInThreadArena(TypeLawChecker& laws, Program& program, std::span<const TypeID> types, const TypePairTable<bool>& isInside, const TypePairTable<bool>& overlaps)
{
    freezeTypesOfTypeTest(program);
    TypeArena arena(program.types, ArenaKeeps::UntilReset);
    unsigned size = types.size();
    for (bool isAfterReset : { false, true }) {
        if (isAfterReset)
            arena.reset();
        for (unsigned i = 0; i < size; ++i) {
            for (unsigned j = 0; j < size; ++j)
                laws.check(arena.isSubset(types[i], types[j]) == isInside(i, j) && arena.overlap(types[i], types[j]) == overlaps(i, j), "same-in-both-arenas"_s, i, j);
        }
    }
}

static void checkTypeOperationInThreadArena(TypeLawChecker& laws, Program& program, std::span<const TypeID> types, OperationOnTypes operation, const TypePairTable<TypeID>& results)
{
    freezeTypesOfTypeTest(program);
    TypeArena arena(program.types, ArenaKeeps::UntilReset);
    unsigned size = types.size();
    Vector<String> textsBeforeReset;
    for (bool isAfterReset : { false, true }) {
        if (isAfterReset)
            arena.reset();
        for (unsigned i = 0; i < size; ++i) {
            TypeID newRange = arena.integer(rangeThatNoTypeTestWrites(i));
            laws.check(liesInThread(newRange), "new-type-lies-in-thread"_s, i, i);
            TypeID newType = arena.unionOf(types[i], newRange);
            for (unsigned j = 0; j < size; ++j) {
                laws.check(operation(arena, types[i], types[j]) == results(i, j), "same-id-in-both-arenas"_s, i, j);
                String text = WTF::toString(TypeInText { arena, operation(arena, newType, types[j]) });
                if (isAfterReset)
                    laws.check(text == textsBeforeReset[i * size + j], "same-text-after-reset"_s, i, j);
                else
                    textsBeforeReset.append(WTF::move(text));
            }
        }
    }
}

static void checkSubsetLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, const TypePairTable<bool>& isInside, const TypePairTable<bool>& overlaps)
{
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        Type a = arena.at(types[i]);
        laws.check(isInside(i, i), "reflexive"_s, i, i);
        laws.check(arena.isSubset(TypeTable::never, types[i]), "never"_s, i, i);
        if (a.isJavaScriptValue() && !(a.primitives & (Primitive::Absent | Primitive::Unspecified)))
            laws.check(arena.isSubset(types[i], TypeTable::anything), "anything"_s, i, i);
        for (unsigned j = 0; j < size; ++j) {
            Type b = arena.at(types[j]);
            laws.check(isInside(i, j) == isSubset(arena, a, b) && overlaps(i, j) == overlap(arena, a, b), "same-by-records"_s, i, j);
            laws.check(overlaps(i, j) == overlaps(j, i), "overlap-symmetric"_s, i, j);
            laws.check(!isInside(i, j) || overlaps(i, j) || a.isNever(), "subset-overlaps"_s, i, j);
            laws.check(isInside(i, j) == arena.isSubset(typeWithoutWrittenContents(arena, types[i]), typeWithoutWrittenContents(arena, types[j])), "ignores-written-contents"_s, i, j);
            if (!isInside(i, j))
                continue;
            if (isInside(j, i))
                laws.check(a.hasUnknown() || b.hasUnknown() ? agreeOnPartsDeterminedByValues(a, b) : types[i] == types[j], "antisymmetric"_s, i, j);
            for (unsigned k = 0; k < size; ++k)
                laws.check(!isInside(j, k) || isInside(i, k), "transitive"_s, i, j, k);
        }
    }
}

static void runSubset(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(program.types, ArenaKeeps::InProgram);
    TypePairTable<bool> isInside(size, [&](unsigned i, unsigned j) {
        return arena.isSubset(types->at(i), types->at(j));
    });
    TypePairTable<bool> overlaps(size, [&](unsigned i, unsigned j) {
        return arena.overlap(types->at(i), types->at(j));
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j)
            context.out.println("t"_s, i, " <= t"_s, j, ": "_s, yesOrNo(isInside(i, j)), " overlaps: "_s, yesOrNo(overlaps(i, j)));
    }

    TypeLawChecker laws(context.out);
    checkSubsetLaws(laws, arena, types->span(), isInside, overlaps);
    checkTypeRelationsInThreadArena(laws, program, types->span(), isInside, overlaps);
    laws.finish();
}

static void checkUnionLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, const TypePairTable<TypeID>& unions)
{
    unsigned size = types.size();
    TypePairTable<bool> isInside(size, [&](unsigned i, unsigned j) {
        return arena.isSubset(types[i], types[j]);
    });
    for (unsigned i = 0; i < size; ++i) {
        Type a = arena.at(types[i]);
        laws.check(unions(i, i) == types[i], "idempotent"_s, i, i);
        for (unsigned j = 0; j < size; ++j) {
            Type b = arena.at(types[j]);
            TypeID united = unions(i, j);
            laws.check(united == unions(j, i), "commutative"_s, i, j);
            laws.check(isCanonicalType(arena, united), "canonical"_s, i, j);
            laws.check(operationIgnoresWrittenContents(unionUnderTest, types[i], types[j], united, arena), "ignores-written-contents"_s, i, j);
            if (!typesHaveUpperBound(a, b))
                continue;
            laws.check(arena.isSubset(types[i], united) && arena.isSubset(types[j], united), "upper-bound"_s, i, j);

            bool bringsNoValueAndNoSeed = isInside(j, i) && a.seeds.containsAll(b.seeds);
            if (united == types[i])
                laws.check(bringsNoValueAndNoSeed, "unchanged-only-by-subset"_s, i, j);
            else if (bringsNoValueAndNoSeed)
                laws.check(!objectPartsAreContainedPartByPart(arena, b, a), "subset-leaves-unchanged"_s, i, j);

            for (unsigned k = 0; k < size; ++k) {
                Type c = arena.at(types[k]);
                if (isInside(i, k) && isInside(j, k))
                    laws.check(arena.isSubset(united, types[k]), "least"_s, i, j, k);
                if (typesHaveUpperBound(a, c) && typesHaveUpperBound(b, c))
                    laws.check(arena.unionOf(united, types[k]) == arena.unionOf(types[i], unions(j, k)), "associative"_s, i, j, k);
            }
        }
    }
}

static void runUnion(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(program.types, ArenaKeeps::InProgram);
    TypePairTable<TypeID> unions(size, [&](unsigned i, unsigned j) {
        return arena.unionOf(types->at(i), types->at(j));
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = i; j < size; ++j)
            context.out.println("t"_s, i, " | t"_s, j, " = "_s, TypeInText { arena, unions(i, j) });
    }

    TypeLawChecker laws(context.out);
    checkUnionLaws(laws, arena, types->span(), unions);
    checkTypeOperationInThreadArena(laws, program, types->span(), unionUnderTest, unions);
    laws.finish();
}

static void checkIntersectLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, const TypePairTable<TypeID>& intersections, const TypePairTable<TypeID>& differences)
{
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            Type a = arena.at(types[i]);
            Type b = arena.at(types[j]);
            TypeID intersection = intersections(i, j);
            TypeID difference = differences(i, j);
            if (!typesHaveIncomparableParts(arena, a, b)) {
                laws.check(arena.isSubset(intersection, types[i]) && arena.isSubset(intersection, types[j]), "lower-bound"_s, i, j);
                laws.check(!arena.isSubset(types[i], types[j]) || typesHaveSameValues(arena, intersection, types[i]), "subset-unchanged"_s, i, j);
            }
            laws.check(arena.overlap(types[i], types[j]) || isNeverType(intersection), "disjoint"_s, i, j);
            laws.check(namesOnlyObjectsOfOperands(arena, arena.at(intersection), a, b), "names-no-new-object"_s, i, j);
            laws.check(arena.isSubset(difference, types[i]), "difference-inside"_s, i, j);
            laws.check(arena.isSubset(types[i], arena.unionOf(difference, intersection)), "difference-covers"_s, i, j);
            if (typesHaveUpperBound(a, b))
                laws.check(arena.isSubset(types[i], arena.unionOf(difference, types[j])), "difference-complete"_s, i, j);
            laws.check(isCanonicalType(arena, intersection) && isCanonicalType(arena, difference), "canonical"_s, i, j);
            laws.check(operationIgnoresWrittenContents(intersectionUnderTest, types[i], types[j], intersection, arena), "ignores-written-contents"_s, i, j);
            for (unsigned k = 0; k < size; ++k) {
                if (arena.isSubset(types[k], types[i]) && arena.isSubset(types[k], types[j]))
                    laws.check(arena.isSubset(types[k], intersection), "greatest"_s, i, j, k);
            }
        }
    }
}

static void runIntersect(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(program.types, ArenaKeeps::InProgram);
    TypePairTable<TypeID> intersections(size, [&](unsigned i, unsigned j) {
        return arena.intersectionOf(types->at(i), types->at(j));
    });
    TypePairTable<TypeID> differences(size, [&](unsigned i, unsigned j) {
        return arena.differenceOf(types->at(i), types->at(j));
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            if (i == j)
                continue;
            context.out.println("t"_s, i, " & t"_s, j, " = "_s, TypeInText { arena, intersections(i, j) });
            context.out.println("t"_s, i, " - t"_s, j, " = "_s, TypeInText { arena, differences(i, j) });
        }
    }

    TypeLawChecker laws(context.out);
    checkIntersectLaws(laws, arena, types->span(), intersections, differences);
    checkTypeOperationInThreadArena(laws, program, types->span(), intersectionUnderTest, intersections);
    checkTypeOperationInThreadArena(laws, program, types->span(), differenceUnderTest, differences);
    laws.finish();
}

static bool keepsContentsOfCastType(TypeArena& arena, const Type& cast, const Type& source, const Type& castType)
{
    std::span<const ClassPart> partsOfSource = unnamedClassPartsOfType(arena, source);
    std::span<const ClassPart> partsOfCastType = unnamedClassPartsOfType(arena, castType);
    return std::ranges::all_of(unnamedClassPartsOfType(arena, cast), [&](const ClassPart& part) {
        const ClassPart* sourcePart = findClassPartIn(partsOfSource, part.builtinClass);
        const ClassPart* castPart = findClassPartIn(partsOfCastType, part.builtinClass);
        if (!sourcePart || !castPart)
            return true;
        const ClassPart& expected = castPart->hasWrittenContents() ? *castPart : *sourcePart;
        return part.writtenContents == expected.writtenContents;
    });
}

static bool namesOnlyClassesThatCastTypeMayHold(TypeArena& arena, const Type& cast, const Type& castType)
{
    const BuiltinTable& builtins = arena.builtins();
    std::span<const ClassPart> partsOfCastType = unnamedClassPartsOfType(arena, castType);
    return castType.anyObject || std::ranges::all_of(unnamedClassPartsOfType(arena, cast), [&](const ClassPart& part) {
        if (isFunctionClass(builtins, part.builtinClass))
            return true;
        return std::ranges::any_of(partsOfCastType, [&](const ClassPart& castPart) {
            return builtins.isSameOrSubclassOf(part.builtinClass, castPart.builtinClass)
                || relate(builtins, part.builtinClass, castPart.builtinClass) == ClassRelation::Unknown;
        });
    });
}

static void checkCastLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, const TypePairTable<TypeID>& casts)
{
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            Type a = arena.at(types[i]);
            Type b = arena.at(types[j]);
            Type cast = arena.at(casts(i, j));
            laws.check(typesHaveSameValues(arena, casts(i, j), arena.intersectionOf(types[i], types[j])), "same-values"_s, i, j);
            laws.check(namesOnlyClassesThatCastTypeMayHold(arena, cast, b), "names-only-classes-cast-type-may-hold"_s, i, j);
            laws.check(namesOnlyObjectsOfOperands(arena, cast, a, b), "names-no-new-object"_s, i, j);
            laws.check(isCanonicalType(arena, casts(i, j)), "canonical"_s, i, j);
            laws.check(operationIgnoresWrittenContents(castUnderTest, types[i], types[j], casts(i, j), arena), "ignores-written-contents"_s, i, j);
            if (!typesHaveIncomparableParts(arena, a, b))
                laws.check(keepsContentsOfCastType(arena, cast, a, b), "written-contents"_s, i, j);
        }
    }
}

static void runCast(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(program.types, ArenaKeeps::InProgram);
    TypePairTable<TypeID> casts(size, [&](unsigned i, unsigned j) {
        return arena.castTo(types->at(i), types->at(j));
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            if (i != j)
                context.out.println("t"_s, i, " as t"_s, j, " = "_s, TypeInText { arena, casts(i, j) });
        }
    }

    TypeLawChecker laws(context.out);
    checkCastLaws(laws, arena, types->span(), casts);
    checkTypeOperationInThreadArena(laws, program, types->span(), castUnderTest, casts);
    laws.finish();
}

static ClassPartsID unitedClassPartsOfTypes(TypeArena& arena, ClassPartsID a, ClassPartsID b)
{
    TypeParts first;
    first.unnamedClasses = a;
    TypeParts second;
    second.unnamedClasses = b;
    return arena.at(arena.unionOf(arena.intern(first), arena.intern(second))).unnamedClasses;
}

static TypeParts unitePartsWithoutCanonicalizing(TypeArena& arena, std::span<const TypeID> types)
{
    TypeParts raw;
    for (TypeID id : types | std::views::reverse) {
        Type type = arena.at(id);
        raw.primitives |= type.primitives;
        raw.seeds.add(type.seeds);
        raw.integers = raw.integers.unionWith(type.integers);
        raw.anyObject |= type.anyObject;
        raw.anyFunction |= type.anyFunction;
        raw.stringLiterals = arena.stringSets().unionOf(raw.stringLiterals, type.stringLiterals);
        raw.symbolLiterals = arena.keySets().unionOf(raw.symbolLiterals, type.symbolLiterals);
        raw.objects = arena.objectSets().unionOf(raw.objects, type.objects);
        raw.unnamedLayouts = arena.layoutSets().unionOf(raw.unnamedLayouts, type.unnamedLayouts);
        raw.unnamedClasses = unitedClassPartsOfTypes(arena, raw.unnamedClasses, type.unnamedClasses);
        if (type.environment)
            raw.environment = type.environment;
        if (type.opaque)
            raw.opaque = type.opaque;
    }
    return raw;
}

static void checkCanonicalLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, std::span<const TypeID> canonicalPrefixes)
{
    for (unsigned last = 0; last < types.size(); ++last) {
        laws.check(isCanonicalType(arena, types[last]), "canonical"_s, last, last);
        laws.check(isCanonicalType(arena, canonicalPrefixes[last]), "idempotent"_s, 0, last);
    }

    TypeID united = TypeTable::never;
    for (unsigned last = 0; last < types.size() && typesHaveUpperBound(arena.at(united), arena.at(types[last])); ++last) {
        TypeID canonical = canonicalPrefixes[last];
        united = arena.unionOf(united, types[last]);
        for (unsigned i = 0; i <= last; ++i)
            laws.check(arena.isSubset(types[i], canonical), "covers"_s, i, last);
        laws.check(arena.isSubset(united, canonical), "covers-union"_s, 0, last);
        laws.check(united == canonical, "equals-union"_s, 0, last);
    }
}

static void runCanonical(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;

    TypeArena arena(context.program->types, ArenaKeeps::InProgram);
    Vector<TypeID> canonicalPrefixes;
    for (unsigned last = 0; last < types->size(); ++last) {
        TypeID prefix = arena.intern(unitePartsWithoutCanonicalizing(arena, types->span().first(last + 1)));
        context.out.println("t0..t"_s, last, " = "_s, TypeInText { arena, prefix });
        canonicalPrefixes.append(prefix);
    }

    TypeLawChecker laws(context.out);
    checkCanonicalLaws(laws, arena, types->span(), canonicalPrefixes.span());
    laws.finish();
}

static void checkRepresentationLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types)
{
    for (unsigned i = 0; i < types.size(); ++i) {
        Type type = arena.at(types[i]);
        Representation representation = representationOf(type);
        laws.check(isLegalRepresentation(type, representation), "default-legal"_s, i, i);
        laws.check(representationOf(arena, types[i]) == representation, "same-by-id"_s, i, i);
        for (unsigned j = 0; j < types.size(); ++j) {
            Type other = arena.at(types[j]);
            if (type.isJavaScriptValue() == other.isJavaScriptValue() && arena.isSubset(types[i], types[j]))
                laws.check(isLegalRepresentation(type, representationOf(other)), "subset-legal"_s, i, j);
        }
        if (!type.isJavaScriptValue())
            continue;
        if (representation != Representation::Void)
            laws.check(arena.isSubset(arena.withoutPrimitives(types[i], Primitive::Unspecified), widestTypeOf(representation)), "widest"_s, i, i);
    }
}

static void runRepresentation(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;

    TypeArena arena(context.program->types, ArenaKeeps::InProgram);
    for (unsigned i = 0; i < types->size(); ++i) {
        Type type = arena.at(types->at(i));
        Representation representation = representationOf(type);
        context.out.println("t"_s, i, ": rep "_s, nameOf(representation),
            " cells "_s, yesOrNo(holdsCells(representation)), " legal "_s, LegalRepresentationsText { type });
    }
    for (Representation representation : allMembersOf<Representation>) {
        if (representation == Representation::Unchosen)
            continue;
        context.out.println(nameOf(representation), ": "_s, TypeInText { arena, widestTypeOf(representation) });
    }
    for (OpaqueKind kind : allMembersOf<OpaqueKind>) {
        TypeID id = arena.forOpaque(kind);
        Type type = arena.at(id);
        context.out.println(TypeInText { arena, id }, ": "_s, nameOf(representationOf(kind)), " legal "_s, LegalRepresentationsText { type });
    }

    TypeLawChecker laws(context.out);
    checkRepresentationLaws(laws, arena, types->span());
    laws.finish();
}

static void runMembers(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    PrintStream& out = context.out;

    for (unsigned index = 0; index < numberOfLayoutsInTypeTest(program); ++index)
        out.println('L', index, ": members "_s, LayoutSetInBrackets { program.types, program.declaredMembersOf(LayoutID(index)) });
    for (unsigned i = 0; i < types->size(); ++i) {
        Type type = program.at(types->at(i));
        out.println('t', i, ": members "_s, LayoutSetInBrackets { program.types, type.layouts }, " only-layouts: "_s, yesOrNo(type.hasOnlyLayouts()));
    }
}

static void runFindField(TestCommandContext& context)
{
    PrintStream& out = context.out;
    std::span<const String> words = context.arguments;
    if (words.size() < 2)
        return printMissingArgument(out);
    Program& program = *context.program;
    auto layout = layoutNamedInTypeTest(words[0], program);
    if (!layout)
        return printBadArgument(out, words[0]);
    auto key = keyNamedInTypeTest(words[1], program);
    if (!key)
        return printBadArgument(out, words[1]);

    std::span<const String> options = words.subspan(2);
    bool shouldAppendInheritedFields = !options.empty() && options[0] == "append-inherited"_s;
    if (shouldAppendInheritedFields)
        options = options.subspan(1);
    if (!options.empty())
        return printBadArgument(out, options[0]);

    if (shouldAppendInheritedFields)
        appendInheritedFields(program, *layout);
    auto field = findField(program, *layout, *key);
    if (!field)
        return out.println("field: none"_s);
    out.println("field: "_s, FieldOfLayoutText { *field }, " declaration: "_s, FieldOfLayoutText { declarationOf(program, *field) });
}

static Vector<LayoutID> layoutsSortedWithoutRepeats(Vector<LayoutID> layouts)
{
    std::ranges::sort(layouts);
    layouts.shrink(std::ranges::unique(layouts).begin() - layouts.begin());
    return layouts;
}

static void runLayoutSet(TestCommandContext& context)
{
    PrintStream& out = context.out;
    std::span<const String> words = context.arguments;
    if (words.empty())
        return printMissingArgument(out);
    Program& program = *context.program;

    Vector<Vector<LayoutID>> lists;
    for (const String& word : words) {
        auto list = layoutsNamedInTypeTest(word, program);
        if (!list)
            return printBadArgument(out, word);
        lists.append(WTF::move(*list));
    }

    TypeArena arena(program.types, ArenaKeeps::InProgram);
    Vector<LayoutSetID> sets = lists.map([&](const Vector<LayoutID>& list) {
        return arena.layoutSets().ofUnsorted(list.span());
    });
    for (unsigned i = 0; i < sets.size(); ++i)
        out.println(words[i], ": set "_s, sets[i].bits(), ' ', LayoutSetInBrackets { arena, sets[i] });

    Vector<Vector<LayoutID>> members = lists.map([](const Vector<LayoutID>& list) {
        return layoutsSortedWithoutRepeats(list);
    });
    TypeLawChecker laws(out);
    for (unsigned i = 0; i < sets.size(); ++i) {
        laws.check(sets[i] ? std::ranges::equal(arena.listOf(sets[i]), members[i]) : members[i].isEmpty(), "holds-its-members"_s, i, i);
        for (unsigned j = 0; j < sets.size(); ++j)
            laws.check((sets[i] == sets[j]) == (members[i] == members[j]), "one-id"_s, i, j);
    }

    freezeTypesOfTypeTest(program);
    TypeArena threadArena(program.types, ArenaKeeps::UntilReset);
    for (unsigned i = 0; i < sets.size(); ++i)
        laws.check(threadArena.layoutSets().ofUnsorted(lists[i].span()) == sets[i], "same-id-in-both-arenas"_s, i, i);
    laws.finish();
}

static uint16_t primitivesOnBothSidesOfBranch(const Type& type)
{
    uint16_t primitives = Primitive::WholeNumber | Primitive::AnyString | Primitive::BigInt | Primitive::Absent | Primitive::Unspecified;
    if (type.integers.minimum < 0 && type.integers.maximum > 0)
        primitives |= Primitive::Integer;
    return primitives;
}

static void checkTruthinessLaws(TypeLawChecker& laws, TypeArena& arena, std::span<const TypeID> types, std::span<const TypeID> truthyParts, std::span<const TypeID> falsyParts)
{
    for (unsigned i = 0; i < types.size(); ++i) {
        Type type = arena.at(types[i]);
        TypeID truthy = truthyParts[i];
        TypeID falsy = falsyParts[i];
        laws.check(arena.isSubset(truthy, types[i]) && arena.isSubset(falsy, types[i]), "inside"_s, i, i);
        laws.check(arena.unionOf(truthy, falsy) == types[i], "complete"_s, i, i);
        laws.check(isCanonicalType(arena, truthy) && isCanonicalType(arena, falsy), "canonical"_s, i, i);
        if (type.isJavaScriptValue())
            laws.check(arena.at(arena.intersectionOf(truthy, falsy)).hasOnlyPrimitives(primitivesOnBothSidesOfBranch(type)), "disjoint"_s, i, i);
    }
}

static void runTruthy(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;

    TypeArena arena(context.program->types, ArenaKeeps::InProgram);
    Vector<TypeID> truthyParts = types->map([&](TypeID type) {
        return truthyPartOf(arena, type);
    });
    Vector<TypeID> falsyParts = types->map([&](TypeID type) {
        return falsyPartOf(arena, type);
    });
    for (unsigned i = 0; i < types->size(); ++i) {
        context.out.println("t"_s, i, " truthy = "_s, TypeInText { arena, truthyParts[i] });
        context.out.println("t"_s, i, " falsy = "_s, TypeInText { arena, falsyParts[i] });
    }

    TypeLawChecker laws(context.out);
    checkTruthinessLaws(laws, arena, types->span(), truthyParts.span(), falsyParts.span());
    laws.finish();
}

static void runEdges(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    const Program& program = *context.program;

    TypeArena arena(context.program->types, ArenaKeeps::InProgram);
    TypingContext typing { program, arena, PositionTypes::knowingNothing() };
    TypeLawChecker laws(context.out);
    for (unsigned i = 0; i < types->size(); ++i) {
        for (unsigned j = 0; j < types->size(); ++j) {
            TypeID source = types->at(i);
            if (!arena.at(source).isJavaScriptValue() || !arena.at(types->at(j)).isJavaScriptValue())
                continue;
            TypeID tested = arena.testablePartOf(types->at(j));
            TypeID passing = narrowByTest(typing, source, tested, EdgeSide::Taken);
            TypeID failing = narrowByTest(typing, source, tested, EdgeSide::NotTaken);
            context.out.println("t"_s, i, " passing t"_s, j, " = "_s, TypeInText { arena, passing });
            context.out.println("t"_s, i, " failing t"_s, j, " = "_s, TypeInText { arena, failing });
            if (tested != types->at(j)) {
                context.out.println("t"_s, i, " passing t"_s, j, " as written = "_s, TypeInText { arena, narrowByTest(typing, source, types->at(j), EdgeSide::Taken) });
                context.out.println("t"_s, i, " failing t"_s, j, " as written = "_s, TypeInText { arena, narrowByTest(typing, source, types->at(j), EdgeSide::NotTaken) });
            }
            TypeID equal = narrowByComparison(typing, Opcode::StrictEq, true, source, types->at(j), EdgeSide::Taken);
            TypeID unequal = narrowByComparison(typing, Opcode::StrictEq, true, source, types->at(j), EdgeSide::NotTaken);
            context.out.println("t"_s, i, " equal to t"_s, j, " = "_s, TypeInText { arena, equal });
            context.out.println("t"_s, i, " unequal to t"_s, j, " = "_s, TypeInText { arena, unequal });

            if (!typesHaveIncomparableParts(arena, arena.at(types->at(j)), arena.at(tested)))
                laws.check(arena.isSubset(types->at(j), tested), "test-admits-its-type"_s, i, j);
            laws.check(arena.isSubset(source, arena.unionOf(passing, failing)), "complete"_s, i, j);
            laws.check(arena.isSubset(passing, source), "passing-inside"_s, i, j);
            laws.check(arena.isSubset(failing, source), "failing-inside"_s, i, j);
            laws.check(arena.isSubset(arena.differenceOf(source, tested), failing), "removes-only-what-surely-passes"_s, i, j);
            laws.check(arena.overlap(source, tested) || isNeverType(passing), "disjoint"_s, i, j);
            laws.check(isCanonicalType(arena, passing) && isCanonicalType(arena, failing), "canonical"_s, i, j);
            laws.check(arena.isSubset(equal, source) && arena.isSubset(unequal, source), "comparison-inside"_s, i, j);
            laws.check(isNeverType(types->at(j)) || arena.isSubset(source, arena.unionOf(equal, unequal)), "comparison-complete"_s, i, j);
        }
    }

    freezeTypesOfTypeTest(*context.program);
    TypeArena threadArena(context.program->types, ArenaKeeps::UntilReset);
    TypingContext typingOfCompile { program, threadArena, PositionTypes::knowingNothing() };
    for (unsigned i = 0; i < types->size(); ++i) {
        for (unsigned j = 0; j < types->size(); ++j) {
            TypeID source = types->at(i);
            if (!threadArena.at(source).isJavaScriptValue() || !threadArena.at(types->at(j)).isJavaScriptValue())
                continue;
            TypeID tested = threadArena.testablePartOf(types->at(j));
            TypeID passing = narrowByTest(typingOfCompile, source, tested, EdgeSide::Taken);
            if (threadArena.isSubset(passing, source))
                continue;
            context.out.println("t"_s, i, " passing t"_s, j, " in Compile = "_s, TypeInText { threadArena, passing });
            laws.check(threadArena.isSubset(passing, tested), "passing-inside-what-is-tested"_s, i, j);
        }
    }
    laws.finish();
}

#define FOR_EACH_LAW_OF_TYPES(v) \
    v(SubsetHoldsItself) \
    v(MeetLiesInFirst) \
    v(MeetLiesInSecond) \
    v(MeetIsTheSameBothWays) \
    v(MeetIsWrittenTheSameBothWays) \
    v(MeetWithItselfIsItself) \
    v(MeetHoldsWhatLiesInBoth) \
    v(CastLiesInFirst) \
    v(CastLiesInSecond) \
    v(CastWithItselfIsItself) \
    v(CastHoldsWhatLiesInBoth) \
    v(JoinHoldsFirst) \
    v(JoinHoldsSecond) \
    v(JoinIsTheSameBothWays) \
    v(JoinIsWrittenTheSameBothWays) \
    v(JoinWithItselfIsItself) \
    v(JoinLiesInWhatHoldsBoth) \
    v(DifferenceLiesInFirst) \
    v(DifferenceMeetsSecondNowhere) \
    v(DifferenceWithItselfIsNever) \
    v(DifferenceHoldsWhatSecondLacks) \
    v(DifferenceAndSecondHoldFirst) \
    v(OverlapIsTheSameBothWays) \
    v(WhatLiesInsideOverlaps) \
    v(WhatDoesNotOverlapMeetsNowhere) \
    v(WhatOverlapsMeetsSomewhere)

enum class LawOfTypes : uint8_t {
#define DECLARE_LAW_OF_TYPES(name) name,
    FOR_EACH_LAW_OF_TYPES(DECLARE_LAW_OF_TYPES)
#undef DECLARE_LAW_OF_TYPES
};

static ASCIILiteral nameOf(LawOfTypes law)
{
    switch (law) {
#define NAME_LAW_OF_TYPES(name) case LawOfTypes::name: return #name ## _s;
    FOR_EACH_LAW_OF_TYPES(NAME_LAW_OF_TYPES)
#undef NAME_LAW_OF_TYPES
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class LawsOfTypes {
    WTF_MAKE_NONCOPYABLE(LawsOfTypes);
public:
    LawsOfTypes(PrintStream& out, Program& program, Vector<TypeID>&& corpus)
        : m_out(out)
        , m_types(program.types, arenaKeepsFor(program.types))
        , m_corpus(WTF::move(corpus))
        , m_numberOfTypes(m_corpus.size())
    {
    }

    LawsOfTypes(PrintStream& out, Program& program, unsigned mostTypes)
        : m_out(out)
        , m_types(program.types, arenaKeepsFor(program.types))
    {
        std::span<const TypeID> typesOfProgram = program.types.typesOfWireIndexes();
        m_numberOfTypes = typesOfProgram.size();
        SetOfShapes shapes;
        for (bool acceptsRepeatedShapes : { false, true }) {
            for (TypeID type : typesOfProgram) {
                if (m_corpus.size() >= mostTypes)
                    break;
                if (!m_types.at(type).isJavaScriptValue())
                    continue;
                if (acceptsRepeatedShapes ? m_corpus.contains(type) : !shapes.add(shapeOf(type)).isNewEntry)
                    continue;
                m_corpus.append(type);
            }
        }
    }

    unsigned run()
    {
        size_t size = m_corpus.size();
        Vector<BitVector> inside(size);
        Vector<BitVector> overlaps(size);
        for (size_t a = 0; a < size; ++a) {
            for (size_t b = 0; b < size; ++b) {
                if (liesIn(m_corpus[a], m_corpus[b]))
                    inside[a].set(b);
                if (m_types.overlap(m_corpus[a], m_corpus[b]))
                    overlaps[a].set(b);
            }
        }
        for (size_t a = 0; a < size; ++a) {
            TypeID first = m_corpus[a];
            expect(LawOfTypes::SubsetHoldsItself, inside[a].get(a), first, first, { }, first);
            expect(LawOfTypes::MeetWithItselfIsItself, m_types.intersectionOf(first, first) == first, first, first, { }, m_types.intersectionOf(first, first));
            expect(LawOfTypes::CastWithItselfIsItself, m_types.castTo(first, first) == first, first, first, { }, m_types.castTo(first, first));
            expect(LawOfTypes::JoinWithItselfIsItself, m_types.unionOf(first, first) == first, first, first, { }, m_types.unionOf(first, first));
            expect(LawOfTypes::DifferenceWithItselfIsNever, isNeverType(m_types.differenceOf(first, first)), first, first, { }, m_types.differenceOf(first, first));
            for (size_t b = 0; b < size; ++b) {
                TypeID second = m_corpus[b];
                TypeID meet = m_types.intersectionOf(first, second);
                TypeID cast = m_types.castTo(first, second);
                TypeID join = m_types.unionOf(first, second);
                TypeID difference = m_types.differenceOf(first, second);

                expect(LawOfTypes::MeetLiesInFirst, liesIn(meet, first), first, second, { }, meet);
                expect(LawOfTypes::MeetLiesInSecond, liesIn(meet, second), first, second, { }, meet);
                expect(LawOfTypes::CastLiesInFirst, liesIn(cast, first), first, second, { }, cast);
                expect(LawOfTypes::CastLiesInSecond, liesIn(cast, second), first, second, { }, cast);
                expect(LawOfTypes::JoinHoldsFirst, liesIn(first, join), first, second, { }, join);
                expect(LawOfTypes::JoinHoldsSecond, liesIn(second, join), first, second, { }, join);
                expect(LawOfTypes::DifferenceLiesInFirst, liesIn(difference, first), first, second, { }, difference);
                expect(LawOfTypes::DifferenceMeetsSecondNowhere, isNeverType(m_types.intersectionOf(difference, second)), first, second, { }, difference);
                expect(LawOfTypes::DifferenceAndSecondHoldFirst, liesIn(first, m_types.unionOf(difference, second)), first, second, { }, difference);
                expect(LawOfTypes::OverlapIsTheSameBothWays, overlaps[a].get(b) == overlaps[b].get(a), first, second, { }, meet);
                expect(LawOfTypes::WhatLiesInsideOverlaps, !inside[a].get(b) || isNeverType(first) || overlaps[a].get(b), first, second, { }, meet);
                expect(LawOfTypes::WhatDoesNotOverlapMeetsNowhere, overlaps[a].get(b) || isNeverType(meet), first, second, { }, meet);
                expect(LawOfTypes::WhatOverlapsMeetsSomewhere, !overlaps[a].get(b) || !isNeverType(meet), first, second, { }, meet);
                if (a < b) {
                    TypeID meetTurned = m_types.intersectionOf(second, first);
                    bool isTheSame = liesIn(meet, meetTurned) && liesIn(meetTurned, meet);
                    expect(LawOfTypes::MeetIsTheSameBothWays, isTheSame, first, second, meetTurned, meet);
                    expect(LawOfTypes::MeetIsWrittenTheSameBothWays, !isTheSame || meet == meetTurned, first, second, meetTurned, meet);
                    TypeID joinTurned = m_types.unionOf(second, first);
                    isTheSame = liesIn(join, joinTurned) && liesIn(joinTurned, join);
                    expect(LawOfTypes::JoinIsTheSameBothWays, isTheSame, first, second, joinTurned, join);
                    expect(LawOfTypes::JoinIsWrittenTheSameBothWays, !isTheSame || join == joinTurned, first, second, joinTurned, join);
                }
                for (size_t c = 0; c < size; ++c) {
                    TypeID third = m_corpus[c];
                    if (inside[a].get(c) && inside[b].get(c))
                        expect(LawOfTypes::JoinLiesInWhatHoldsBoth, liesIn(join, third), first, second, third, join);
                    if (!inside[c].get(a))
                        continue;
                    if (inside[c].get(b)) {
                        expect(LawOfTypes::MeetHoldsWhatLiesInBoth, liesIn(third, meet), first, second, third, meet);
                        expect(LawOfTypes::CastHoldsWhatLiesInBoth, liesIn(third, cast), first, second, third, cast);
                    }
                    if (!overlaps[c].get(b) && !overlaps[b].get(c))
                        expect(LawOfTypes::DifferenceHoldsWhatSecondLacks, liesIn(third, difference), first, second, third, difference);
                }
            }
        }

        unsigned shapes = 0;
        unsigned numberThatHold = 0;
        m_out.println("laws of types: "_s, size, " of "_s, m_numberOfTypes, " types"_s);
        for (size_t law = 0; law < numberOfLaws; ++law) {
            if (m_broken[law])
                m_out.println("sum "_s, nameOf(static_cast<LawOfTypes>(law)), ": "_s, m_broken[law], " times in "_s, m_shapes[law], " shapes"_s);
            else
                ++numberThatHold;
            shapes += m_shapes[law];
        }
        m_out.println(numberThatHold, " of "_s, numberOfLaws, " laws hold"_s);
        return shapes;
    }

private:
    using SetOfShapes = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    static constexpr size_t numberOfLaws = static_cast<size_t>(LawOfTypes::WhatOverlapsMeetsSomewhere) + 1;

    static ArenaKeeps arenaKeepsFor(const TypeTable& table) { return table.isFrozen() ? ArenaKeeps::UntilReset : ArenaKeeps::InProgram; }

    bool liesIn(TypeID inner, TypeID outer) { return m_types.isSubset(inner, outer); }

    static uint64_t mix(uint64_t hash, uint64_t word) { return (hash ^ word) * 1099511628211ull; }

    uint64_t shapeOf(TypeID id) const
    {
        Type type = m_types.at(id);
        uint64_t hash = mix(14695981039346656037ull, type.primitives);
        hash = mix(hash, type.anyObject | type.anyFunction << 1 | !!type.unnamedLayouts << 2 | !!type.stringLiterals << 3 | !!type.symbolLiterals << 4 | type.integers.isSubsetOf(Range::int32()) << 5);
        hash = mix(hash, type.traitsOfNamed.toRaw());
        for (const ClassPart& part : classPartsOf(m_types, type)) {
            hash = mix(hash, part.builtinClass.index());
            hash = mix(hash, !!part.writtenContents[0] | !!part.writtenContents[1] << 1 | !!part.writtenContents[2] << 2);
        }
        return hash;
    }

    void expect(LawOfTypes law, bool holds, TypeID first, TypeID second, TypeID third, TypeID result)
    {
        if (holds)
            return;
        size_t index = static_cast<size_t>(law);
        ++m_broken[index];
        uint64_t shape = mix(mix(mix(mix(index, shapeOf(first)), shapeOf(second)), third ? shapeOf(third) : 0), shapeOf(result));
        if (!m_seen.add(shape).isNewEntry)
            return;
        ++m_shapes[index];
        m_out.print("broken "_s, nameOf(law), " | a: "_s, TypeInText { m_types, first }, " | b: "_s, TypeInText { m_types, second });
        if (third)
            m_out.print(" | c: "_s, TypeInText { m_types, third });
        m_out.println(" | gives: "_s, TypeInText { m_types, result });
    }

    PrintStream& m_out;
    TypeArena m_types;
    Vector<TypeID> m_corpus;
    size_t m_numberOfTypes { 0 };
    uint64_t m_broken[numberOfLaws] { };
    unsigned m_shapes[numberOfLaws] { };
    SetOfShapes m_seen;
};

static_assert(mostContentsOfClass == 3);

unsigned listBrokenLawsOfTypes(PrintStream& out, Program& program, unsigned mostTypes)
{
    return LawsOfTypes(out, program, mostTypes).run();
}

static void runLawsOfTypes(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    Vector<TypeID> corpus;
    for (TypeID type : *types) {
        if (program.at(type).isJavaScriptValue() && !corpus.contains(type))
            corpus.append(type);
    }
    LawsOfTypes(context.out, program, WTF::move(corpus)).run();
}

static bool someMemberMayBeObject(TypeReader reader, const SetView<ObjectID, ObjectSetID>& members, ObjectID object)
{
    return members.anyOf([&](ObjectID member) {
        return mayBeSameObject(reader, object, member);
    });
}

static void runAliases(TestCommandContext& context)
{
    Program& program = *context.program;
    auto types = typesOfTypeTest(context);
    if (!types)
        return;

    const ObjectTable& objects = program.types.objects();
    TypeArena arena(program.types, ArenaKeeps::InProgram);
    Vector<ObjectID> corpus;
    for (unsigned i = 0; i < types->size(); ++i) {
        ObjectSetID named = arena.at(types->at(i)).objects;
        arena.at(named).forEach([&](ObjectID object) {
            corpus.append(object);
        });
        for (unsigned j = 0; j < types->size(); ++j) {
            ObjectSetID namedByOther = arena.at(types->at(j)).objects;
            if (i == j || !named || !namedByOther)
                continue;
            ObjectSetAliasIndex index(arena, arena.at(namedByOther));
            Vector<ObjectID> withAlias;
            size_t numberOfPairs = 0;
            arena.objectSets().forEachAdded(named, namedByOther, [&](ObjectID object) {
                numberOfPairs += arena.at(namedByOther).size();
                if (index.namesAliasOf(object))
                    withAlias.append(object);
            });
            ObjectSetID listed = arena.objectSets().ofSorted(withAlias.span());
            context.out.print("t"_s, i, " in t"_s, j, " = "_s);
            printObjectSet(context.out, arena, listed);
            context.out.println(", "_s, index.numberOfPairsAsked(), " of "_s, numberOfPairs, " pairs asked"_s);
            if (arena.membersWithAliasIn(named, namedByOther) != listed || arena.membersWithAliasIn(named, namedByOther) != listed)
                context.out.println("the arena answers otherwise: t"_s, i, " t"_s, j);
        }
    }

    for (uint32_t function = 0; function < program.functions.size(); ++function) {
        FunctionID id(function);
        corpus.appendList({ objects.closureOf(id), objects.coroutineResultOf(id), objects.restArrayOf(id), objects.prototypeObjectOf(id), objects.constructedObjectOf(id) });
    }
    for (uint32_t module = 0; module < program.modules.size(); ++module)
        corpus.append(objects.namespaceOf(ModuleID(module)));
    for (unsigned layout = 0; layout < numberOfLayoutsInTypeTest(program); ++layout)
        corpus.appendList({ objects.instanceOf(LayoutID(layout)), objects.prototypeOf(LayoutID(layout)), objects.escapedOf(LayoutID(layout)) });
    const BuiltinTable& builtins = arena.builtins();
    for (uint32_t ofModule = objects.firstOf(ObjectKind::ModuleBuiltin); ofModule < objects.endOf(ObjectKind::ModuleBuiltin); ++ofModule)
        corpus.appendList({ ObjectID(ofModule), objects.objectOf(objects.builtinOf(ObjectID(ofModule))) });
    for (uint32_t index = 0; index < builtins.hostObjects().size(); ++index) {
        corpus.append(objects.hostObject(index));
        if (BuiltinID holder = builtins.hostObjects()[index].holder)
            corpus.append(objects.objectOf(holder));
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!builtins.contains(entry.id))
            continue;
        for (BuiltinID implemented : builtins.at(entry.id).implements)
            corpus.appendList({ objects.objectOf(implemented), objects.mergedFormOf(objects.objectOf(implemented)) });
    }
    std::ranges::sort(corpus);
    corpus.shrink(std::ranges::unique(corpus).begin() - corpus.begin());

    ObjectSetID everyObject = arena.objectSets().ofSorted(corpus.span());
    Vector<TypeID> typesOfObjects = corpus.map([&](ObjectID object) {
        return arena.forObjects(ObjectSets::of(object));
    });
    unsigned numberOfSets = 0;
    unsigned numberThatDiffer = 0;
    size_t numberOfPairsAsked = 0;

    auto holdTypesAgainstEachPair = [&](TypeID first, TypeID second) {
        Type a = arena.at(first);
        Type b = arena.at(second);
        bool somePairMayBeOneObject = arena.at(a.objects).anyOf([&](ObjectID ofFirst) {
            return arena.at(b.objects).anyOf([&](ObjectID ofSecond) {
                return mayBeSameObject(arena, ofFirst, ofSecond);
            });
        });
        bool saysYesToMore = mayContainAnyClosure(arena, a) || mayContainAnyClosure(arena, b);
        for (bool answer : { mayBeSameObject(arena, a, b), mayBeSameObject(arena, b, a) }) {
            if (answer == somePairMayBeOneObject || (answer && saysYesToMore))
                continue;
            if (++numberThatDiffer > 20)
                continue;
            context.out.print("two types differ from mayBeSameObject() of their members, which says "_s, somePairMayBeOneObject ? "yes"_s : "no"_s, ": "_s);
            printObjectSet(context.out, arena, a.objects);
            context.out.print(" "_s);
            printObjectSet(context.out, arena, b.objects);
            context.out.println();
        }
    };

    auto holdIndexAgainstEachMember = [&](std::span<const ObjectID> members, bool holdsTypesToo) {
        unsigned set = numberOfSets++;
        ObjectSetID setOfMembers = arena.objectSets().ofSorted(members);
        TypeID typeOfSet = arena.forObjects(setOfMembers);
        auto view = arena.at(setOfMembers);
        ObjectSetAliasIndex askedAscending(arena, view);
        ObjectSetAliasIndex askedDescending(arena, view);
        for (ObjectID object : corpus | std::views::reverse) {
            if (!view.contains(object))
                askedDescending.namesAliasOf(object);
        }
        Vector<ObjectID> withAlias;
        for (size_t index = 0; index < corpus.size(); ++index) {
            ObjectID object = corpus[index];
            if (view.contains(object))
                continue;
            if (holdsTypesToo)
                holdTypesAgainstEachPair(typesOfObjects[index], typeOfSet);
            bool hasAlias = someMemberMayBeObject(arena, view, object);
            if (hasAlias)
                withAlias.append(object);
            if (askedAscending.namesAliasOf(object) == hasAlias && askedDescending.namesAliasOf(object) == hasAlias)
                continue;
            if (++numberThatDiffer > 20)
                continue;
            context.out.print("the index differs from mayBeSameObject(), which says "_s, hasAlias ? "yes"_s : "no"_s, ", in set "_s, set, ": "_s);
            printObject(context.out, arena, object);
            view.forEach([&](ObjectID member) {
                if (!mayBeSameObject(arena, object, member))
                    return;
                context.out.print(" = "_s);
                printObject(context.out, arena, member);
            });
            context.out.println();
        }
        numberOfPairsAsked += askedAscending.numberOfPairsAsked() + askedDescending.numberOfPairsAsked();
        ObjectSetID expected = arena.objectSets().ofSorted(withAlias.span());
        if (arena.membersWithAliasIn(everyObject, setOfMembers) != expected || arena.membersWithAliasIn(everyObject, setOfMembers) != expected) {
            ++numberThatDiffer;
            context.out.println("the arena answers otherwise for set "_s, set);
        }
        return setOfMembers;
    };

    constexpr unsigned numberOfSeededSets = 24;
    WeakRandom random(20261102);
    Vector<ObjectSetID> seededSets;
    for (unsigned set = 0; set < numberOfSeededSets; ++set) {
        unsigned oneIn = 2 + set % 12;
        Vector<ObjectID> members;
        for (ObjectID object : corpus) {
            if (!random.getUint32(oneIn))
                members.append(object);
        }
        seededSets.append(holdIndexAgainstEachMember(members.span(), oneIn >= 8));
        if (!set)
            continue;
        if (ObjectSetID rest = arena.objectSets().differenceOf(seededSets[set - 1], seededSets[set]))
            holdTypesAgainstEachPair(arena.forObjects(rest), arena.forObjects(seededSets[set]));
    }

    for (size_t index = 0; index < corpus.size(); ++index)
        holdIndexAgainstEachMember(corpus.span().subspan(index, 1), true);

    Vector<Vector<ObjectID>> expectedInThread;
    for (unsigned set = 1; set < numberOfSeededSets; ++set) {
        auto view = arena.at(seededSets[set]);
        Vector<ObjectID> withAlias;
        arena.objectSets().forEachAdded(seededSets[set - 1], seededSets[set], [&](ObjectID object) {
            if (someMemberMayBeObject(arena, view, object))
                withAlias.append(object);
        });
        expectedInThread.append(WTF::move(withAlias));
    }
    freezeTypesOfTypeTest(program);
    TypeArena arenaOfThread(program.types, ArenaKeeps::UntilReset);
    unsigned numberOfAnswersInThread = 0;
    for (bool isAfterReset : { false, true }) {
        if (isAfterReset)
            arenaOfThread.reset();
        for (unsigned set = 1; set < numberOfSeededSets; ++set) {
            for (bool isRemembered : { false, true }) {
                ObjectSetID answer = arenaOfThread.membersWithAliasIn(seededSets[set - 1], seededSets[set]);
                numberOfAnswersInThread += liesInThread(answer);
                Vector<ObjectID> members;
                arenaOfThread.at(answer).forEach([&](ObjectID object) {
                    members.append(object);
                });
                if (members == expectedInThread[set - 1])
                    continue;
                ++numberThatDiffer;
                context.out.println("the arena of a thread answers otherwise for sets "_s, set - 1, " and "_s, set, isAfterReset ? " behind reset()"_s : ""_s, isRemembered ? ", asked again"_s : ""_s);
            }
        }
    }
    if (!numberOfAnswersInThread) {
        ++numberThatDiffer;
        context.out.println("no answer lay in the thread"_s);
    }

    context.out.println("in all "_s, numberOfPairsAsked, " pairs asked about "_s, corpus.size(), " objects and "_s, numberOfSets, " sets"_s);
    if (!numberThatDiffer)
        context.out.println("the index answers as mayBeSameObject() does"_s);
}

template<bool tellsTable>
static void runMemosAtRest(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    if (program.layouts.isEmpty())
        return printMissingArgument(context.out);

    auto answers = [&] {
        TypeArena arena(program.types, ArenaKeeps::InProgram);
        return types->map([&](TypeID type) {
            return std::pair(arena.testablePartOf(type), arena.describedByShape(type));
        });
    };
    program.types.beginKeepingMemosAtRest();
    auto before = answers();
    if constexpr (tellsTable) {
        BitVector layoutsOfDictionaries(program.layouts.size());
        layoutsOfDictionaries.quickSet(0);
        noteLayoutsOfDictionaries(program, layoutsOfDictionaries);
    } else
        program.layouts[0].holdsOrdinaryObjects = true;
    auto after = answers();
    for (unsigned i = 0; i < types->size(); ++i)
        context.out.println("t"_s, i, " is tested "_s, after[i].first == before[i].first ? "as before"_s : "otherwise"_s, " and described "_s, after[i].second == before[i].second ? "as before"_s : "otherwise"_s);
}

static void runJoin(TestCommandContext& context)
{
    auto types = typesOfTypeTest(context);
    if (!types)
        return;
    Program& program = *context.program;
    SetsOfLayoutsWritten everySetOfWire;
    for (TypeID type : program.types.typesOfWireIndexes()) {
        if (LayoutSetID set = type && !holdsOnlyPrimitives(type) ? program.at(type).unnamedLayouts : LayoutSetID(); set && !holdsOneMember(set))
            everySetOfWire.add(set.bits());
    }
    Families families(program, everySetOfWire, { }, context.build.parallelLoops());
    TypeArena arena(program.types, ArenaKeeps::InProgram);

    unsigned numberThatHangOnOrder = 0;
    auto joinedInOrder = [&](std::span<const unsigned> order, bool inPairs) {
        if (inPairs)
            return joinOf(arena, families, joinOf(arena, families, types->at(order[0]), types->at(order[1])), joinOf(arena, families, types->at(order[2]), types->at(order[3])));
        TypeID joined = types->at(order[0]);
        for (unsigned place : order.subspan(1))
            joined = joinOf(arena, families, joined, types->at(place));
        return joined;
    };
    auto forEachOrder = [&](std::span<const unsigned> chosen, const auto& functor) {
        Vector<unsigned, 4> order(chosen);
        do {
            functor(order.span(), false);
            if (order.size() == 4)
                functor(order.span(), true);
        } while (std::ranges::next_permutation(order).found);
    };
    auto printOrder = [&](std::span<const unsigned> order, bool inPairs) {
        for (size_t place = 0; place < order.size(); ++place)
            context.out.print(place ? (inPairs && place == 2 ? " | "_s : " "_s) : ""_s, "t"_s, order[place]);
    };
    auto judge = [&](std::span<const unsigned> chosen) {
        TypeID first = joinedInOrder(chosen, false);
        bool hangsOnOrder = false;
        forEachOrder(chosen, [&](std::span<const unsigned> order, bool inPairs) {
            hangsOnOrder |= joinedInOrder(order, inPairs) != first;
        });
        if (!hangsOnOrder) {
            printOrder(chosen, false);
            context.out.println(" = "_s, TypeInText { arena, first });
            return;
        }
        ++numberThatHangOnOrder;
        forEachOrder(chosen, [&](std::span<const unsigned> order, bool inPairs) {
            context.out.print("HANGS ON THE ORDER: "_s);
            printOrder(order, inPairs);
            context.out.println(" = "_s, TypeInText { arena, joinedInOrder(order, inPairs) });
        });
    };
    Vector<unsigned, 4> chosen;
    auto choose = [&](auto& self, unsigned from) -> void {
        if (chosen.size() >= 2)
            judge(chosen.span());
        if (chosen.size() == 4)
            return;
        for (unsigned next = from; next < types->size(); ++next) {
            chosen.append(next);
            self(self, next + 1);
            chosen.removeLast();
        }
    };
    choose(choose, 0);
    if (numberThatHangOnOrder)
        context.out.println(numberThatHangOnOrder, " joins hang on the order"_s);
    else
        context.out.println("every order ends alike"_s);
}

std::span<const TestCommand> typeTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "subset"_s, runSubset, ProgramOfCommand::AsWritten, ""_s },
        { "union"_s, runUnion, ProgramOfCommand::AsWritten, ""_s },
        { "intersect"_s, runIntersect, ProgramOfCommand::AsWritten, ""_s },
        { "join"_s, runJoin, ProgramOfCommand::AsWritten, ""_s },
        { "cast"_s, runCast, ProgramOfCommand::AsWritten, ""_s },
        { "canonical"_s, runCanonical, ProgramOfCommand::AsWritten, ""_s },
        { "rep"_s, runRepresentation, ProgramOfCommand::AsWritten, ""_s },
        { "members"_s, runMembers, ProgramOfCommand::AsWritten, ""_s },
        { "find-field"_s, runFindField, ProgramOfCommand::AsWritten, "L<n> <key>|#<key>@L<n>|- [append-inherited]"_s },
        { "layout-set"_s, runLayoutSet, ProgramOfCommand::AsWritten, "{ L<n>,L<n>.. | - }"_s },
        { "truthy"_s, runTruthy, ProgramOfCommand::AsWritten, ""_s },
        { "edges"_s, runEdges, ProgramOfCommand::AsWritten, ""_s },
        { "laws-of-types"_s, runLawsOfTypes, ProgramOfCommand::AsWritten, ""_s },
        { "aliases"_s, runAliases, ProgramOfCommand::AsWritten, ""_s },
        { "memos-at-rest"_s, runMemosAtRest<true>, ProgramOfCommand::AsWritten, ""_s },
        { "memos-at-rest-untold"_s, runMemosAtRest<false>, ProgramOfCommand::AsWritten, ""_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
