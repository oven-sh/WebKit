/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLiteralStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(literalStringWord(0) == emptyLiteralStringWord);
static_assert(maxLiteralStrings > 0);

// For planProgram(). No header declares it.
LiteralStringTable computeLiteralStrings(const Program&, const Facts&);

namespace {

class CollectorOfLiteralStrings {
    WTF_MAKE_NONCOPYABLE(CollectorOfLiteralStrings);
public:
    explicit CollectorOfLiteralStrings(const Program& program)
        : m_program(program)
        , m_types(program.types)
        , m_emptyString(program.strings.emptyString())
        , m_numberOfStrings(program.strings.size())
        , m_numberOfTypes(program.types.numberOfTypes())
        , m_isNumberedWithItsSet(m_numberOfStrings)
        , m_isLiteral(m_numberOfStrings)
        , m_visitedTypes(m_numberOfTypes)
    {
        RELEASE_ASSERT(m_emptyString && m_emptyString.index() < m_numberOfStrings);
    }

    // `"add" | "mul" | "num"`: the members that no earlier writer lists get numbers side by side, so a test for the set asks whether a word lies in a range.
    // A text may be a member of many sets, and the first takes it. What is written at the positions of the program is what its code tests for, so those sets come in front of what else the wire lists:
    // the writers are the functions, each with its parameters and then its results, the layouts with their fields, and then each type of the wire. That order comes from the program alone.
    void numberSetsAsWritten();
    // Follows ClassPart::writtenContents to any depth.
    void addLiteralsOf(TypeID);
    void addLiteralsOf(const LinkTables&);
    void add(StringID);

    LiteralStringTable createTable();

private:
    void numberMembersAtTopOf(TypeID);
    void visitLater(TypeID);
    void addMembersOf(StringSetID);

    const Program& m_program;
    const TypeTable& m_types;
    const StringID m_emptyString;
    const size_t m_numberOfStrings;
    const size_t m_numberOfTypes;
    // By string.
    BitVector m_isNumberedWithItsSet;
    Vector<StringID> m_stringsNumberedWithTheirSets;
    BitVector m_isLiteral;
    // By position in the TypeTable, where the positions of types are dense. It only skips repeats: nothing is decided by a position.
    BitVector m_visitedTypes;
    // The position of a set is a shard and an index within it, so there is nothing dense to index by.
    HashSet<StringSetID> m_visitedSets;
    Vector<TypeID, 16> m_typesToVisit;
};

void CollectorOfLiteralStrings::add(StringID string)
{
    // A text that a thread of Compile makes has no word: Walker::convertWithoutCode().
    if (string && string.index() < m_numberOfStrings)
        m_isLiteral.quickSet(string.index());
}

void CollectorOfLiteralStrings::addMembersOf(StringSetID set)
{
    if (!set)
        return;
    if (holdsOneMember(set)) {
        add(StringID(positionOf(set)));
        return;
    }
    if (!m_visitedSets.add(set).isNewEntry)
        return;
    m_types.at(set).forEach([&](StringID string) {
        add(string);
    });
}

void CollectorOfLiteralStrings::visitLater(TypeID type)
{
    // A type that is its own value has no lists. One that was made since the table was sized lists what has no number, which claims nothing.
    if (!type || holdsOnlyPrimitives(type) || !liesInProgram(type) || positionOf(type) >= m_numberOfTypes)
        return;
    if (!m_visitedTypes.quickSet(positionOf(type)))
        m_typesToVisit.append(type);
}

void CollectorOfLiteralStrings::addLiteralsOf(TypeID root)
{
    visitLater(root);
    while (!m_typesToVisit.isEmpty()) {
        Type type = m_types.at(m_typesToVisit.takeLast());
        addMembersOf(type.stringLiterals);
        for (const ClassPart& part : classPartsOf(m_types, type)) {
            for (TypeID contents : part.writtenContents)
                visitLater(contents);
        }
    }
}

void CollectorOfLiteralStrings::addLiteralsOf(const LinkTables& tables)
{
    for (const LinkedParameter& parameter : tables.parameters) {
        addLiteralsOf(parameter.proven);
        addLiteralsOf(parameter.expected);
    }
    for (const LinkedFunction& function : tables.functions) {
        addLiteralsOf(function.result);
        addLiteralsOf(function.thrown);
    }
    for (const Vector<LinkedCell>* cells : { &tables.fields, &tables.appendedFields, &tables.variables, &tables.captures }) {
        for (const LinkedCell& cell : *cells)
            addLiteralsOf(cell.proven);
    }
}

void CollectorOfLiteralStrings::numberMembersAtTopOf(TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return;
    m_types.at(m_types.at(type).stringLiterals).forEach([&](StringID string) {
        if (string.index() < m_numberOfStrings && !m_isNumberedWithItsSet.quickSet(string.index()))
            m_stringsNumberedWithTheirSets.append(string);
    });
}

void CollectorOfLiteralStrings::numberSetsAsWritten()
{
    for (const Function& function : m_program.functions) {
        for (const DeclaredType& parameter : function.parameters)
            numberMembersAtTopOf(parameter.written());
        for (const DeclaredType& result : function.results)
            numberMembersAtTopOf(result.written());
    }
    for (const Layout& layout : m_program.layouts) {
        for (const Field& field : layout.fields)
            numberMembersAtTopOf(field.declared.written());
    }
    for (TypeID type : m_types.typesOfWireIndexes())
        numberMembersAtTopOf(type);
}

LiteralStringTable CollectorOfLiteralStrings::createTable()
{
    m_isLiteral.quickSet(m_emptyString.index());
    for (StringID string : m_stringsNumberedWithTheirSets)
        m_isLiteral.quickSet(string.index());
    size_t numberOfMembers = m_isLiteral.bitCount();
    size_t numberOfWords = std::min<size_t>(numberOfMembers, maxLiteralStrings);

    Vector<StringID> strings;
    Vector<StringID> stringsWithoutWord;
    strings.reserveInitialCapacity(numberOfWords);
    stringsWithoutWord.reserveInitialCapacity(numberOfMembers - numberOfWords);

    // After the empty string come the sets in the order of their writers, then the rest by StringID. Both orders come from the wire.
    strings.append(m_emptyString);
    auto append = [&](StringID string) {
        if (string == m_emptyString)
            return;
        if (strings.size() < numberOfWords)
            strings.append(string);
        else
            stringsWithoutWord.append(string);
    };
    for (StringID string : m_stringsNumberedWithTheirSets)
        append(string);
    for (size_t index : m_isLiteral) {
        if (!m_isNumberedWithItsSet.quickGet(index))
            append(StringID(static_cast<uint32_t>(index)));
    }
    RELEASE_ASSERT(strings.size() == numberOfWords && strings.size() + stringsWithoutWord.size() == numberOfMembers);

    Vector<uint32_t> numbersPlusOne;
    numbersPlusOne.fill(0, m_numberOfStrings);
    for (uint32_t number = 0; number < strings.size(); ++number)
        numbersPlusOne[strings[number].index()] = number + 1;

    return LiteralStringTable(WTF::move(strings), WTF::move(stringsWithoutWord), WTF::move(numbersPlusOne));
}

} // anonymous namespace

// Numbered: every string literal of a wire type, or of a type that Link put at a position or in a cell. Unions and intersections of those types add no string.
// The walk also makes types of constants and of the built-ins' table. So the texts that the scan has seen meet others are numbered too, and those that the table writes, and what `typeof` answers.
// A text that meets another only in the walk's sight may still lack a number: formOfTypeInWalk() keeps a value that may be such a text out of the form LiteralString. So any table is right, and a fuller one is faster.
// With TypedOperations withheld: the empty string alone, and every text is a cell.
LiteralStringTable computeLiteralStrings(const Program& program, const Facts& facts)
{
    CollectorOfLiteralStrings collector(program);
    if (!facts.mechanisms().contains(Mechanism::TypedOperations))
        return collector.createTable();

    collector.numberSetsAsWritten();
    collector.addLiteralsOf(facts.linked());
    collector.addLiteralsOf(facts.held());
    for (TypeID type : program.types.typesOfWireIndexes())
        collector.addLiteralsOf(type);
    const ThinSummaries& summaries = facts.summaries();
    for (uint32_t index = 0; index < summaries.size(); ++index) {
        if (!facts.mayBeReached(FunctionID(index)))
            continue;
        for (StringID string : summaries.at(FunctionID(index)).textsThatMeetOthers)
            collector.add(string);
    }
    for (const BuiltinType& type : builtinTableData.types) {
        for (BuiltinName name : type.stringLiterals)
            collector.add(program.stringOf(name));
    }
    for (NamedBuiltinName name : namesAnsweredByTypeOf)
        collector.add(program.stringOf(name));
    return collector.createTable();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
