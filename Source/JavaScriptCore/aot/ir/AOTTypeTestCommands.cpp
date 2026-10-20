/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTIRPrinter.h"
#include "AOTRepresentation.h"
#include "AOTSchemaEnums.h"
#include "AOTSortedSet.h"
#include "AOTSupportTestCommands.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <ranges>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using NumberSet = SortedSet<uint32_t>;

struct TypeText {
    const Program& program;
    const Type& type;

    void dump(PrintStream& out) const { printType(out, program, type); }
};

struct LayoutText {
    const Program& program;
    LayoutID layout;

    void dump(PrintStream& out) const { IRPrinter(out, program).print(layout); }
};

struct LayoutSetText {
    const Program& program;
    LayoutSetID set;

    void dump(PrintStream& out) const
    {
        if (!set) {
            out.print("[]"_s);
            return;
        }
        IRPrinter(out, program).print(set);
    }
};

struct FieldText {
    const Program& program;
    FieldReference field;

    void dump(PrintStream& out) const { out.print(LayoutText { program, field.layout }, '.', field.field); }
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

struct NumberSetText {
    const NumberSet& set;

    void dump(PrintStream& out) const
    {
        if (set.isEmpty()) {
            out.print('-');
            return;
        }
        CommaPrinter comma(","_s);
        for (uint32_t number : set)
            out.print(comma, number);
    }
};

class LawChecker {
    WTF_MAKE_NONCOPYABLE(LawChecker);
public:
    explicit LawChecker(PrintStream& out)
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
class PairTable {
public:
    PairTable(size_t size, NOESCAPE const Invocable<T(unsigned, unsigned)> auto& compute)
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

struct ElementOperation {
    ASCIILiteral name;
    void (*run)(PrintStream&, NumberSet&, uint32_t);
};

struct SetOperation {
    ASCIILiteral name;
    void (*run)(PrintStream&, NumberSet&, const NumberSet&);
};

} // anonymous namespace

static std::optional<LayoutID> parseLayout(StringView text, const Program& program)
{
    if (!text.startsWith('L'))
        return std::nullopt;
    auto index = parseTestNumber<uint32_t>(text.substring(1));
    if (!index || *index >= program.numberOfLayoutsAsRead())
        return std::nullopt;
    return LayoutID(*index);
}

static std::optional<Vector<LayoutID>> parseLayoutList(StringView text, const Program& program)
{
    Vector<LayoutID> layouts;
    if (text == "-"_s)
        return layouts;
    for (StringView entry : text.splitAllowingEmptyEntries(',')) {
        auto layout = parseLayout(entry, program);
        if (!layout)
            return std::nullopt;
        layouts.append(*layout);
    }
    return layouts;
}

static PropertyKeyID findKey(const Program& program, PropertyKeyKind kind, StringView text, LayoutID owner)
{
    StringID string = program.strings.find(encodeProgramString(text).span());
    if (!string)
        return { };
    return program.types.find(PropertyKey { .kind = kind, .text = string, .owner = owner }).value_or(PropertyKeyID());
}

static std::optional<PropertyKeyID> parseKey(StringView word, const Program& program)
{
    if (word == "-"_s)
        return PropertyKeyID();
    if (!word.startsWith('#'))
        return findKey(program, PropertyKeyKind::Text, word, { });
    size_t ownerMark = word.reverseFind('@');
    if (ownerMark == notFound)
        return std::nullopt;
    auto owner = parseLayout(word.substring(ownerMark + 1), program);
    if (!owner)
        return std::nullopt;
    return findKey(program, PropertyKeyKind::PrivateName, word.substring(1, ownerMark - 1), *owner);
}

static std::optional<BaseChain> parseBaseChain(StringView word)
{
    if (word == "proven"_s)
        return BaseChain::Proven;
    if (word == "predicted"_s)
        return BaseChain::Predicted;
    return std::nullopt;
}

static std::optional<Vector<Type>> typesUnderTest(TestCommandContext& context)
{
    if (!context.arguments.empty()) {
        printBadArgument(context.out, context.arguments[0]);
        return std::nullopt;
    }
    const Program& program = *context.program;
    if (program.functions.isEmpty()) {
        context.out.println("no function"_s);
        return std::nullopt;
    }
    return program.functions[0].parameters.map([&](const DeclaredType& parameter) {
        return program.at(parameter.type);
    });
}

static bool hasUpperBound(const Type& a, const Type& b)
{
    if (a.isJavaScriptValue() && b.isJavaScriptValue())
        return true;
    if (a.opaque == b.opaque && a.environment == b.environment)
        return true;
    return a.hasOnlyPrimitives(Primitive::Absent) || b.hasOnlyPrimitives(Primitive::Absent);
}

static bool isCanonical(const Type& type)
{
    Type copy = type;
    copy.canonicalize();
    return copy == type;
}

static bool hasSameValues(const Type& a, const Type& b, const Program& program)
{
    return a.isSubsetOf(b, program) && b.isSubsetOf(a, program);
}

static Type withoutPredictedContents(const Type& type)
{
    Type result = type;
    for (ClassPart& part : result.classes)
        part.predictedContents.clear();
    return result;
}

using TypeOperation = Type (Type::*)(const Type&, TypeArena&) const;

static bool ignoresPredictedContents(TypeOperation operation, const Type& a, const Type& b, const Type& result, TypeArena& arena)
{
    Type expected = (withoutPredictedContents(a).*operation)(withoutPredictedContents(b), arena);
    return withoutPredictedContents(result) == withoutPredictedContents(expected);
}

// Types with the same values may differ in declared parts, in the types their contents name, and in a callable built-in listed beside `function`.
static Type topLevelPartsOf(const Type& type, const BuiltinTable& builtins)
{
    Type result = withoutPredictedContents(type);
    for (ClassPart& part : result.classes) {
        part.contents.clear();
        if (!part.contentsAreProven)
            part.arrayStorage = ArrayStorage::Other;
    }
    if (result.anyFunction) {
        removeAllMatching(result.builtins, [&](BuiltinID builtin) {
            return isCallableBuiltin(builtins, builtin);
        });
    }
    return result;
}

static bool hasFunctionClass(const BuiltinTable& builtins, const Type& type)
{
    return std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass);
    });
}

// No type says that a function or a built-in is an instance of a class, that a built-in is a closure of a function, or that an object is an instance of two classes.
static bool mayContainPartsOf(const Type& container, const Type& type, const TypeTable& types)
{
    const BuiltinTable& builtins = types.builtins();
    if (hasCallableParts(type) && hasFunctionClass(builtins, container))
        return true;
    bool mayContainUnlistedBuiltin = std::ranges::any_of(type.builtins, [&](BuiltinID builtin) {
        return !includesBuiltin(container, builtin, builtins) && mayContainBuiltin(types, container, builtin);
    });
    if (mayContainUnlistedBuiltin)
        return true;
    return std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return overlapsAnotherClass(builtins, part.builtinClass, container);
    });
}

static bool hasIncomparableParts(const Type&, const Type&, const TypeTable&);

static bool hasIncomparableContents(const ClassPart& a, const ClassPart& b, const TypeTable& types)
{
    if (!a.contentsAreProven || !b.contentsAreProven)
        return false;
    if (a.arrayStorage != b.arrayStorage && a.arrayStorage != ArrayStorage::Other && b.arrayStorage != ArrayStorage::Other)
        return true;
    for (unsigned index = 0; index < std::max(a.contents.size(), b.contents.size()); ++index) {
        if (hasIncomparableParts(types.at(a.contentsAt(index)), types.at(b.contentsAt(index)), types))
            return true;
    }
    return false;
}

static bool hasIncomparableParts(const Type& a, const Type& b, const TypeTable& types)
{
    if (a.anyObject || b.anyObject)
        return false;
    if (mayContainPartsOf(a, b, types) || mayContainPartsOf(b, a, types))
        return true;
    return std::ranges::any_of(a.classes, [&](const ClassPart& part) {
        const ClassPart* partner = b.findClassPart(part.builtinClass);
        return partner && hasIncomparableContents(part, *partner, types);
    });
}

// The first type is a copy: the relation answers early when the two types are one object.
static bool computeIsSubset(Type a, const Type& b, const Program& program)
{
    return a.isSubsetOf(b, program);
}

static void checkSubsetLaws(LawChecker& laws, const Program& program, std::span<const Type> types, const PairTable<bool>& isSubset, const PairTable<bool>& overlaps)
{
    const BuiltinTable& builtins = program.builtins();
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        laws.check(isSubset(i, i), "reflexive"_s, i, i);
        laws.check(computeIsSubset(Type::never(), types[i], program), "never"_s, i, i);
        if (types[i].isJavaScriptValue() && !(types[i].primitives & Primitive::Absent))
            laws.check(types[i].isSubsetOf(Type::anything(), program), "anything"_s, i, i);
        for (unsigned j = 0; j < size; ++j) {
            laws.check(overlaps(i, j) == overlaps(j, i), "overlap-symmetric"_s, i, j);
            laws.check(!isSubset(i, j) || overlaps(i, j) || types[i].isNever(), "subset-overlaps"_s, i, j);
            laws.check(isSubset(i, j) == withoutPredictedContents(types[i]).isSubsetOf(withoutPredictedContents(types[j]), program), "ignores-predictions"_s, i, j);
            if (!isSubset(i, j))
                continue;
            if (isSubset(j, i))
                laws.check(topLevelPartsOf(types[i], builtins) == topLevelPartsOf(types[j], builtins), "antisymmetric"_s, i, j);
            for (unsigned k = 0; k < size; ++k)
                laws.check(!isSubset(j, k) || isSubset(i, k), "transitive"_s, i, j, k);
        }
    }
}

static void runSubset(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    unsigned size = types->size();

    PairTable<bool> isSubset(size, [&](unsigned i, unsigned j) {
        return computeIsSubset(types->at(i), types->at(j), program);
    });
    PairTable<bool> overlaps(size, [&](unsigned i, unsigned j) {
        return types->at(i).overlaps(types->at(j), program);
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j)
            context.out.println("t"_s, i, " <= t"_s, j, ": "_s, yesOrNo(isSubset(i, j)), " overlaps: "_s, yesOrNo(overlaps(i, j)));
    }

    LawChecker laws(context.out);
    checkSubsetLaws(laws, program, types->span(), isSubset, overlaps);
    laws.finish();
}

static void printUnions(PrintStream& out, const Program& program, unsigned size, const PairTable<Type>& unions)
{
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = i; j < size; ++j)
            out.println("t"_s, i, " | t"_s, j, " = "_s, TypeText { program, unions(i, j) });
    }
}

static void checkUnionLaws(LawChecker& laws, const Program& program, std::span<const Type> types, const PairTable<Type>& unions, TypeArena& arena)
{
    unsigned size = types.size();
    PairTable<bool> isSubset(size, [&](unsigned i, unsigned j) {
        return types[i].isSubsetOf(types[j], program);
    });
    for (unsigned i = 0; i < size; ++i) {
        const Type& a = types[i];
        laws.check(unions(i, i) == a, "idempotent"_s, i, i);
        for (unsigned j = 0; j < size; ++j) {
            const Type& b = types[j];
            const Type& united = unions(i, j);
            laws.check(united == unions(j, i), "commutative"_s, i, j);
            laws.check(isCanonical(united), "canonical"_s, i, j);
            laws.check(ignoresPredictedContents(&Type::unionWith, a, b, united, arena), "ignores-predictions"_s, i, j);
            if (!hasUpperBound(a, b))
                continue;
            laws.check(a.isSubsetOf(united, program) && b.isSubsetOf(united, program), "upper-bound"_s, i, j);
            for (unsigned k = 0; k < size; ++k) {
                const Type& c = types[k];
                if (isSubset(i, k) && isSubset(j, k))
                    laws.check(united.isSubsetOf(c, program), "least"_s, i, j, k);
                if (hasUpperBound(a, c) && hasUpperBound(b, c))
                    laws.check(united.unionWith(c, arena) == a.unionWith(unions(j, k), arena), "associative"_s, i, j, k);
            }
        }
    }
}

static void runUnion(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(context.program->types);
    PairTable<Type> unions(size, [&](unsigned i, unsigned j) {
        return types->at(i).unionWith(types->at(j), arena);
    });
    printUnions(context.out, program, size, unions);

    LawChecker laws(context.out);
    checkUnionLaws(laws, program, types->span(), unions, arena);
    laws.finish();
}

static void checkWideningLaws(LawChecker& laws, const Program& program, std::span<const Type> types, const PairTable<Type>& unions, TypeArena& arena)
{
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            const Type& a = types[i];
            const Type& b = types[j];
            const Type& widened = unions(i, j);
            laws.check(widened == unions(j, i), "commutative"_s, i, j);
            laws.check(isCanonical(widened), "canonical"_s, i, j);
            laws.check(a.unionWith(b, arena).isSubsetOf(widened, program), "covers-union"_s, i, j);
            laws.check(i != j || widened == a, "idempotent"_s, i, j);
            if (hasUpperBound(a, b))
                laws.check(a.isSubsetOf(widened, program) && b.isSubsetOf(widened, program), "upper-bound"_s, i, j);
        }
    }
}

static void runUnionWidening(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(context.program->types);
    WideningContext widening { program, arena, context.build, Subject { .function = FunctionID(0u) } };
    PairTable<Type> unions(size, [&](unsigned i, unsigned j) {
        return types->at(i).unionWideningWith(types->at(j), widening);
    });
    printUnions(context.out, program, size, unions);

    context.losses = context.build.takeLosses(context.program.get());
    for (const Wire::LossRecord& loss : context.losses)
        context.out.println("loss "_s, nameOf(loss.limit), " x"_s, loss.count);

    LawChecker laws(context.out);
    checkWideningLaws(laws, program, types->span(), unions, arena);
    laws.finish();
}

static void checkIntersectLaws(LawChecker& laws, const Program& program, std::span<const Type> types, const PairTable<Type>& intersections, const PairTable<Type>& differences, TypeArena& arena)
{
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            const Type& a = types[i];
            const Type& b = types[j];
            const Type& intersection = intersections(i, j);
            const Type& difference = differences(i, j);
            if (!hasIncomparableParts(a, b, program.types)) {
                laws.check(intersection.isSubsetOf(a, program) && intersection.isSubsetOf(b, program), "lower-bound"_s, i, j);
                laws.check(!a.isSubsetOf(b, program) || hasSameValues(intersection, a, program), "subset-unchanged"_s, i, j);
            }
            laws.check(a.overlaps(b, program) || intersection.isNever(), "disjoint"_s, i, j);
            laws.check(difference.isSubsetOf(a, program), "difference-inside"_s, i, j);
            laws.check(a.isSubsetOf(difference.unionWith(intersection, arena), program), "difference-covers"_s, i, j);
            if (hasUpperBound(a, b))
                laws.check(a.isSubsetOf(difference.unionWith(b, arena), program), "difference-complete"_s, i, j);
            laws.check(isCanonical(intersection) && isCanonical(difference), "canonical"_s, i, j);
            laws.check(ignoresPredictedContents(&Type::intersectWith, a, b, intersection, arena), "ignores-predictions"_s, i, j);
            for (unsigned k = 0; k < size; ++k) {
                const Type& c = types[k];
                if (c.isSubsetOf(a, program) && c.isSubsetOf(b, program))
                    laws.check(c.isSubsetOf(intersection, program), "greatest"_s, i, j, k);
            }
        }
    }
}

static void runIntersect(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(context.program->types);
    PairTable<Type> intersections(size, [&](unsigned i, unsigned j) {
        return types->at(i).intersectWith(types->at(j), arena);
    });
    PairTable<Type> differences(size, [&](unsigned i, unsigned j) {
        return types->at(i).without(types->at(j), arena);
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            if (i == j)
                continue;
            context.out.println("t"_s, i, " & t"_s, j, " = "_s, TypeText { program, intersections(i, j) });
            context.out.println("t"_s, i, " - t"_s, j, " = "_s, TypeText { program, differences(i, j) });
        }
    }

    LawChecker laws(context.out);
    checkIntersectLaws(laws, program, types->span(), intersections, differences, arena);
    laws.finish();
}

static bool hasExpectedDeclaredParts(const Type& cast, const Type& source, const Type& castType)
{
    return std::ranges::all_of(cast.classes, [&](const ClassPart& part) {
        const ClassPart* sourcePart = source.findClassPart(part.builtinClass);
        const ClassPart* castPart = castType.findClassPart(part.builtinClass);
        if (!sourcePart || !castPart || (sourcePart->contentsAreProven && castPart->contentsAreProven))
            return true;
        bool usesSource = sourcePart->contentsAreProven || (!castPart->contentsAreProven && castPart->contents.isEmpty());
        const ClassPart& expected = usesSource ? *sourcePart : *castPart;
        return part.contentsAreProven == expected.contentsAreProven && part.arrayStorage == expected.arrayStorage && part.contents == expected.contents;
    });
}

static bool namesClassesAsCastType(const BuiltinTable& builtins, const Type& cast, const Type& castType)
{
    return castType.anyObject || std::ranges::all_of(cast.classes, [&](const ClassPart& part) {
        if (isFunctionClass(builtins, part.builtinClass))
            return true;
        return std::ranges::any_of(castType.classes, [&](const ClassPart& castPart) {
            return builtins.isSameOrSubclassOf(part.builtinClass, castPart.builtinClass);
        });
    });
}

static bool namesBuiltinsAsCastType(const BuiltinTable& builtins, const Type& cast, const Type& castType)
{
    return castType.anyObject || std::ranges::all_of(cast.builtins, [&](BuiltinID builtin) {
        return includesBuiltin(castType, builtin, builtins);
    });
}

static void checkCastLaws(LawChecker& laws, const Program& program, std::span<const Type> types, const PairTable<Type>& casts, TypeArena& arena)
{
    const BuiltinTable& builtins = program.types.builtins();
    unsigned size = types.size();
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            const Type& a = types[i];
            const Type& b = types[j];
            const Type& cast = casts(i, j);
            laws.check(hasSameValues(cast, a.intersectWith(b, arena), program), "same-values"_s, i, j);
            laws.check(namesClassesAsCastType(builtins, cast, b), "names-classes-as-cast-type"_s, i, j);
            laws.check(namesBuiltinsAsCastType(builtins, cast, b), "names-builtins-as-cast-type"_s, i, j);
            laws.check(isCanonical(cast), "canonical"_s, i, j);
            laws.check(ignoresPredictedContents(&Type::castTo, a, b, cast, arena), "ignores-predictions"_s, i, j);
            if (!hasIncomparableParts(a, b, program.types))
                laws.check(hasExpectedDeclaredParts(cast, a, b), "declared-parts"_s, i, j);
        }
    }
}

static void runCast(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    unsigned size = types->size();

    TypeArena arena(context.program->types);
    PairTable<Type> casts(size, [&](unsigned i, unsigned j) {
        return types->at(i).castTo(types->at(j), arena);
    });
    for (unsigned i = 0; i < size; ++i) {
        for (unsigned j = 0; j < size; ++j) {
            if (i != j)
                context.out.println("t"_s, i, " as t"_s, j, " = "_s, TypeText { program, casts(i, j) });
        }
    }

    LawChecker laws(context.out);
    checkCastLaws(laws, program, types->span(), casts, arena);
    laws.finish();
}

static Type uniteWithoutCanonicalizing(std::span<const Type> types, TypeArena& arena)
{
    Type raw;
    Vector<LayoutID> members;
    bool hasUnrestrictedRegExp = false;
    for (const Type& type : types | std::views::reverse) {
        raw.primitives |= type.primitives;
        raw.integers = raw.integers.unionWith(type.integers);
        raw.anyObject |= type.anyObject;
        raw.anyFunction |= type.anyFunction;
        raw.stringLiterals.unite(type.stringLiterals);
        raw.symbolLiterals.unite(type.symbolLiterals);
        raw.functions.unite(type.functions);
        raw.builtins.unite(type.builtins);
        raw.regExpLiteralSet.unite(type.regExpLiteralSet);
        raw.classes.appendVector(type.classes);
        members.append(membersOf(arena.table(), type));
        hasUnrestrictedRegExp |= type.regExpLiteralSet.isEmpty() && type.findClassPart(BuiltinClasses::RegExp);
        if (type.environment)
            raw.environment = type.environment;
        if (type.opaque)
            raw.opaque = type.opaque;
    }
    if (hasUnrestrictedRegExp)
        raw.regExpLiteralSet.clear();
    raw.layouts = Type::forLayouts(members.span(), arena).layouts;
    return raw;
}

static void checkCanonicalLaws(LawChecker& laws, const Program& program, std::span<const Type> types, std::span<const Type> canonicalPrefixes, TypeArena& arena)
{
    const BuiltinTable& builtins = program.builtins();
    for (unsigned last = 0; last < types.size(); ++last)
        laws.check(isCanonical(canonicalPrefixes[last]), "idempotent"_s, 0, last);

    Type united;
    for (unsigned last = 0; last < types.size() && hasUpperBound(united, types[last]); ++last) {
        const Type& canonical = canonicalPrefixes[last];
        united = united.unionWith(types[last], arena);
        for (unsigned i = 0; i <= last; ++i)
            laws.check(types[i].isSubsetOf(canonical, program), "covers"_s, i, last);
        laws.check(united.isSubsetOf(canonical, program), "covers-union"_s, 0, last);
        laws.check(topLevelPartsOf(united, builtins) == topLevelPartsOf(canonical, builtins), "equals-union"_s, 0, last);
    }
}

static void runCanonical(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;

    TypeArena arena(context.program->types);
    Vector<Type> canonicalPrefixes;
    for (unsigned last = 0; last < types->size(); ++last) {
        Type prefix = uniteWithoutCanonicalizing(types->span().first(last + 1), arena);
        prefix.canonicalize();
        context.out.println("t0..t"_s, last, " = "_s, TypeText { program, prefix });
        canonicalPrefixes.append(WTF::move(prefix));
    }

    LawChecker laws(context.out);
    checkCanonicalLaws(laws, program, types->span(), canonicalPrefixes.span(), arena);
    laws.finish();
}

static void runWithoutProofs(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;

    Vector<Type> testableTypes = types->map([](const Type& type) {
        return type.withoutProofs();
    });
    for (unsigned i = 0; i < types->size(); ++i)
        context.out.println("t"_s, i, " = "_s, TypeText { program, testableTypes[i] });

    LawChecker laws(context.out);
    for (unsigned i = 0; i < types->size(); ++i) {
        const Type& testable = testableTypes[i];
        laws.check(testable.withoutProofs() == testable, "idempotent"_s, i, i);
        laws.check(types->at(i).isSubsetOf(testable, program), "superset"_s, i, i);
        laws.check(isCanonical(testable), "canonical"_s, i, i);
    }
    laws.finish();
}

static void checkRepresentationLaws(LawChecker& laws, const Program& program, std::span<const Type> types)
{
    for (unsigned i = 0; i < types.size(); ++i) {
        const Type& type = types[i];
        Representation representation = defaultRepresentationOf(type);
        laws.check(isLegalRepresentation(type, representation), "default-legal"_s, i, i);
        for (unsigned j = 0; j < types.size(); ++j) {
            if (type.isJavaScriptValue() == types[j].isJavaScriptValue() && type.isSubsetOf(types[j], program))
                laws.check(isLegalRepresentation(type, defaultRepresentationOf(types[j])), "subset-legal"_s, i, j);
        }
        if (!type.isJavaScriptValue())
            continue;
        if (representation != Representation::Void)
            laws.check(type.isSubsetOf(widestTypeOf(representation), program), "widest"_s, i, i);
        laws.check(isLegalRepresentation(type, representationOf(defaultSlotRepresentationOf(type))), "slot-legal"_s, i, i);
    }
}

static void runRepresentation(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;

    for (unsigned i = 0; i < types->size(); ++i) {
        const Type& type = types->at(i);
        Representation representation = defaultRepresentationOf(type);
        context.out.println("t"_s, i, ": rep "_s, nameOf(representation), " slot "_s, nameOf(defaultSlotRepresentationOf(type)),
            " cells "_s, yesOrNo(holdsCells(representation)), " legal "_s, LegalRepresentationsText { type });
    }
    for (Representation representation : allMembersOf<Representation>) {
        if (representation == Representation::Unchosen)
            continue;
        Type widest = widestTypeOf(representation);
        context.out.println(nameOf(representation), ": "_s, TypeText { program, widest });
    }
    for (OpaqueKind kind : allMembersOf<OpaqueKind>) {
        Type type = Type::forOpaque(kind);
        context.out.println(TypeText { program, type }, ": "_s, nameOf(representationOf(kind)), " legal "_s, LegalRepresentationsText { type });
    }

    LawChecker laws(context.out);
    checkRepresentationLaws(laws, program, types->span());
    laws.finish();
}

static void runMembers(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;
    PrintStream& out = context.out;

    for (unsigned index = 0; index < program.numberOfLayoutsAsRead(); ++index) {
        LayoutID layout(index);
        out.println(LayoutText { program, layout }, ": members "_s, LayoutSetText { program, program.declaredMembersOf(layout) });
    }
    for (unsigned i = 0; i < types->size(); ++i) {
        const Type& type = types->at(i);
        out.println('t', i, ": members "_s, LayoutSetText { program, type.layouts }, " only-layouts: "_s, yesOrNo(type.hasOnlyLayouts()));
    }
}

static void runFindField(TestCommandContext& context)
{
    PrintStream& out = context.out;
    std::span<const String> words = context.arguments;
    if (words.size() < 2)
        return printMissingArgument(out);
    Program& program = *context.program;
    auto layout = parseLayout(words[0], program);
    if (!layout)
        return printBadArgument(out, words[0]);
    auto key = parseKey(words[1], program);
    if (!key)
        return printBadArgument(out, words[1]);

    std::span<const String> options = words.subspan(2);
    BaseChain chain = BaseChain::Proven;
    if (!options.empty()) {
        if (auto parsedChain = parseBaseChain(options[0])) {
            chain = *parsedChain;
            options = options.subspan(1);
        }
    }
    bool shouldAppendInheritedFields = !options.empty() && options[0] == "append-inherited"_s;
    if (shouldAppendInheritedFields)
        options = options.subspan(1);
    if (!options.empty())
        return printBadArgument(out, options[0]);

    if (shouldAppendInheritedFields)
        appendInheritedFields(program, *layout);
    auto field = findField(program, *layout, *key, chain);
    if (!field)
        return out.println("field: none"_s);
    out.println("field: "_s, FieldText { program, *field }, " declaration: "_s, FieldText { program, declarationOf(program, *field) });
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
        auto list = parseLayoutList(word, program);
        if (!list)
            return printBadArgument(out, word);
        lists.append(WTF::move(*list));
    }

    TypeArena arena(program.types);
    Vector<LayoutSetID> sets = lists.map([&](const Vector<LayoutID>& list) {
        return arena.completeLayoutSet(list.span());
    });
    for (unsigned i = 0; i < sets.size(); ++i)
        out.println(words[i], ": set "_s, sets[i].index(), ' ', LayoutSetText { program, sets[i] });

    LawChecker laws(out);
    for (unsigned i = 0; i < sets.size(); ++i) {
        for (unsigned j = 0; j < sets.size(); ++j) {
            bool hasEqualMembers = SortedSet<LayoutID>(lists[i].span()) == SortedSet<LayoutID>(lists[j].span());
            laws.check((sets[i] == sets[j]) == hasEqualMembers, "one-id"_s, i, j);
        }
    }
    laws.finish();
}

static uint16_t unsplittablePrimitivesOf(const Type& type)
{
    uint16_t primitives = Primitive::WholeNumber | Primitive::AnyString | Primitive::BigInt | Primitive::Absent;
    if (type.integers.minimum < 0 && type.integers.maximum > 0)
        primitives |= Primitive::Integer;
    return primitives;
}

static void checkTruthinessLaws(LawChecker& laws, const Program& program, std::span<const Type> types, std::span<const Type> truthyParts, std::span<const Type> falsyParts, TypeArena& arena)
{
    for (unsigned i = 0; i < types.size(); ++i) {
        const Type& type = types[i];
        const Type& truthy = truthyParts[i];
        const Type& falsy = falsyParts[i];
        laws.check(truthy.isSubsetOf(type, program) && falsy.isSubsetOf(type, program), "inside"_s, i, i);
        laws.check(truthy.unionWith(falsy, arena) == type, "complete"_s, i, i);
        laws.check(isCanonical(truthy) && isCanonical(falsy), "canonical"_s, i, i);
        if (type.isJavaScriptValue())
            laws.check(truthy.intersectWith(falsy, arena).hasOnlyPrimitives(unsplittablePrimitivesOf(type)), "disjoint"_s, i, i);
    }
}

static void runTruthy(TestCommandContext& context)
{
    auto types = typesUnderTest(context);
    if (!types)
        return;
    const Program& program = *context.program;

    TypeArena arena(context.program->types);
    Vector<Type> truthyParts = types->map([&](const Type& type) {
        return truthyPartOf(type, arena);
    });
    Vector<Type> falsyParts = types->map([&](const Type& type) {
        return falsyPartOf(type, arena);
    });
    for (unsigned i = 0; i < types->size(); ++i) {
        context.out.println("t"_s, i, " truthy = "_s, TypeText { program, truthyParts[i] });
        context.out.println("t"_s, i, " falsy = "_s, TypeText { program, falsyParts[i] });
    }

    LawChecker laws(context.out);
    checkTruthinessLaws(laws, program, types->span(), truthyParts.span(), falsyParts.span(), arena);
    laws.finish();
}

static std::optional<NumberSet> parseNumberSet(StringView text)
{
    if (text == "-"_s)
        return NumberSet();
    Vector<uint32_t> numbers;
    for (StringView entry : text.splitAllowingEmptyEntries(',')) {
        auto number = parseTestNumber<uint32_t>(entry);
        if (!number)
            return std::nullopt;
        numbers.append(*number);
    }
    return NumberSet(numbers.span());
}

static void printChange(PrintStream& out, const NumberSet& set, bool changed)
{
    out.println(NumberSetText { set }, ' ', changed ? "changed"_s : "unchanged"_s);
}

template<typename Operation, size_t size>
static const Operation* findOperation(const Operation (&operations)[size], StringView name)
{
    for (const Operation& operation : operations) {
        if (name == operation.name)
            return &operation;
    }
    return nullptr;
}

static constexpr ElementOperation elementOperations[] = {
    { "add"_s, [](PrintStream& out, NumberSet& set, uint32_t number) { printChange(out, set, set.add(number)); } },
    { "remove"_s, [](PrintStream& out, NumberSet& set, uint32_t number) { printChange(out, set, set.remove(number)); } },
    { "contains"_s, [](PrintStream& out, NumberSet& set, uint32_t number) { out.println(yesOrNo(set.contains(number))); } },
};

static constexpr SetOperation setOperations[] = {
    { "unite"_s, [](PrintStream& out, NumberSet& set, const NumberSet& other) { printChange(out, set, set.unite(other)); } },
    { "intersect"_s, [](PrintStream& out, NumberSet& set, const NumberSet& other) {
        set.intersect(other);
        out.println(NumberSetText { set });
    } },
    { "subtract"_s, [](PrintStream& out, NumberSet& set, const NumberSet& other) {
        set.subtract(other);
        out.println(NumberSetText { set });
    } },
    { "subset"_s, [](PrintStream& out, NumberSet& set, const NumberSet& other) { out.println(yesOrNo(set.isSubsetOf(other))); } },
    { "overlaps"_s, [](PrintStream& out, NumberSet& set, const NumberSet& other) { out.println(yesOrNo(set.overlaps(other))); } },
};

static void runSortedSet(TestCommandContext& context)
{
    PrintStream& out = context.out;
    std::span<const String> words = context.arguments;
    if (words.empty())
        return printMissingArgument(out);

    const ElementOperation* elementOperation = findOperation(elementOperations, words[0]);
    const SetOperation* setOperation = findOperation(setOperations, words[0]);
    if (!elementOperation && !setOperation && words[0] != "list"_s)
        return printBadArgument(out, words[0]);
    size_t numberOfWords = elementOperation || setOperation ? 3 : 2;
    if (words.size() < numberOfWords)
        return printMissingArgument(out);
    if (words.size() > numberOfWords)
        return printBadArgument(out, words[numberOfWords]);

    auto set = parseNumberSet(words[1]);
    if (!set)
        return printBadArgument(out, words[1]);
    if (elementOperation) {
        auto number = parseTestNumber<uint32_t>(words[2]);
        if (!number)
            return printBadArgument(out, words[2]);
        return elementOperation->run(out, *set, *number);
    }
    if (!setOperation)
        return out.println(NumberSetText { *set });
    if (words[2] == "self"_s)
        return setOperation->run(out, *set, *set);
    auto other = parseNumberSet(words[2]);
    if (!other)
        return printBadArgument(out, words[2]);
    setOperation->run(out, *set, *other);
}

std::span<const TestCommand> typeTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "subset"_s, IRForm::Prepared, runSubset },
        { "union"_s, IRForm::Prepared, runUnion },
        { "union-widening"_s, IRForm::Prepared, runUnionWidening },
        { "intersect"_s, IRForm::Prepared, runIntersect },
        { "cast"_s, IRForm::Prepared, runCast },
        { "canonical"_s, IRForm::Prepared, runCanonical },
        { "without-proofs"_s, IRForm::Prepared, runWithoutProofs },
        { "rep"_s, IRForm::Prepared, runRepresentation },
        { "members"_s, IRForm::Prepared, runMembers },
        { "find-field"_s, IRForm::Prepared, runFindField },
        { "layout-set"_s, IRForm::Prepared, runLayoutSet },
        { "truthy"_s, IRForm::Prepared, runTruthy },
        { "sorted-set"_s, std::nullopt, runSortedSet },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
