/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpMatchPlan.h"

#if ENABLE(AOT)

#include "YarrPattern.h"
#include <algorithm>
#include <wtf/ASCIICType.h>

namespace JSC { namespace AOT {

using Node = RegExpMatchNode;
using Sequence = RegExpMatchNode::Sequence;

static constexpr unsigned maxNodesOfPlan = 24;
static constexpr unsigned maxNodesOfStickyPlan = 64;
static constexpr unsigned maxCopiesOfGroup = 6;
static constexpr unsigned maxFixedCharacters = 64;
static constexpr unsigned minAlternativesChosen = 4;
static constexpr unsigned maxAlternativesChosen = 64;

static unsigned addLengths(unsigned a, unsigned b)
{
    return a > Node::unbounded - b ? Node::unbounded : a + b;
}

static unsigned multiplyLengths(unsigned a, unsigned b)
{
    if (!a || !b)
        return 0;
    return a > Node::unbounded / b ? Node::unbounded : a * b;
}

static unsigned minimumLengthOf(const Node& node)
{
    switch (node.kind) {
    case Node::Kind::Characters:
        return node.minimum;
    case Node::Kind::Group: {
        if (!node.minimum)
            return 0;
        unsigned length = Node::unbounded;
        for (const Sequence& alternative : node.alternatives)
            length = std::min(length, minimumLengthOf(alternative.span()));
        return length;
    }
    case Node::Kind::StartOfSubject:
    case Node::Kind::StartOfLine:
    case Node::Kind::EndOfSubject:
    case Node::Kind::EndOfLine:
    case Node::Kind::WordBoundary:
    case Node::Kind::NoWordBoundary:
    case Node::Kind::Lookahead:
    case Node::Kind::NegativeLookahead:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned minimumLengthOf(std::span<const RegExpMatchNode> nodes)
{
    unsigned length = 0;
    for (const Node& node : nodes)
        length = addLengths(length, minimumLengthOf(node));
    return length;
}

static unsigned maximumLengthOf(const Sequence& sequence)
{
    unsigned length = 0;
    for (const Node& node : sequence) {
        switch (node.kind) {
        case Node::Kind::Characters:
            length = addLengths(length, node.maximum);
            break;
        case Node::Kind::Group: {
            unsigned longest = 0;
            for (const Sequence& alternative : node.alternatives)
                longest = std::max(longest, maximumLengthOf(alternative));
            length = addLengths(length, multiplyLengths(longest, node.maximum));
            break;
        }
        case Node::Kind::StartOfSubject:
        case Node::Kind::StartOfLine:
        case Node::Kind::EndOfSubject:
        case Node::Kind::EndOfLine:
        case Node::Kind::WordBoundary:
        case Node::Kind::NoWordBoundary:
        case Node::Kind::Lookahead:
        case Node::Kind::NegativeLookahead:
            break;
        }
    }
    return length;
}

namespace {

struct Beginning {
    Latin1Set characters { };
    bool takesAnyCharacter { false };
    bool cannotFail { false };
};

static constexpr Beginning endOfPattern() { return { .characters = { }, .takesAnyCharacter = true, .cannotFail = true }; }

class Planner {
public:
    Planner(const Yarr::YarrPattern& pattern, bool isSticky)
        : m_pattern(pattern)
        , m_maxNodes(isSticky ? maxNodesOfStickyPlan : maxNodesOfPlan)
    {
    }

    std::unique_ptr<const RegExpMatchPlan> plan();
    std::unique_ptr<const RegExpMatchPlan> planOfPrefix() const;

private:
    bool addAlternatives(const Yarr::PatternDisjunction&, Vector<Sequence>&);
    bool addTerms(const Yarr::PatternAlternative&, Sequence&);
    bool addTerm(const Yarr::PatternTerm&, Sequence&);
    bool addCharacters(const Yarr::PatternTerm&, const Latin1Set&, Sequence&);
    bool addGroup(const Yarr::PatternTerm&, Sequence&);
    static std::optional<Latin1Set> charactersOf(const Yarr::PatternTerm&);

    static std::optional<bool> addFirstCharacters(const Yarr::PatternAlternative&, Latin1Set&);
    static std::optional<bool> addFirstCharacters(const Yarr::PatternDisjunction&, Latin1Set&);

    Beginning beginningOf(const Node&, const Beginning& rest) const;
    Beginning beginningOf(std::span<const Node>, const Beginning& rest) const;
    bool areExclusive(const Vector<Sequence>&, const Beginning& rest) const;
    Vector<Latin1Set> beginningsOf(const Vector<Sequence>&) const;

    bool analyze(Sequence&, const Beginning& rest);
    bool analyze(Node&, const Beginning& rest);
    bool analyzeGroup(Node&, const Beginning& rest);

    unsigned costOf(std::span<const Node>, unsigned costOfRest) const;

    const Yarr::YarrPattern& m_pattern;
    const unsigned m_maxNodes;
    unsigned m_numberOfNodes { 0 };
    bool m_isRefused { false };
};

} // anonymous namespace

std::optional<Latin1Set> Planner::charactersOf(const Yarr::PatternTerm& term)
{
    Latin1Set characters;
    if (term.type == Yarr::PatternTerm::Type::PatternCharacter) {
        char32_t character = term.patternCharacter;
        if (character > 0xff)
            return characters;
        characters.set(character);
        if (term.ignoreCase() && isASCIIAlpha(character)) {
            characters.set(toASCIILower(character));
            characters.set(toASCIIUpper(character));
        }
        return characters;
    }

    const Yarr::CharacterClass& characterClass = *term.characterClass;
    if (characterClass.hasStrings())
        return std::nullopt;
    auto add = [&](char32_t first, char32_t last) {
        for (char32_t character = first; character <= std::min<char32_t>(last, 0xff); ++character)
            characters.set(character);
    };
    if (characterClass.m_anyCharacter)
        characters.setAll();
    for (char32_t character : characterClass.m_matches8)
        add(character, character);
    for (char32_t character : characterClass.m_matches32)
        add(character, character);
    for (const Yarr::CharacterRange& range : characterClass.m_ranges8)
        add(range.begin, range.end);
    for (const Yarr::CharacterRange& range : characterClass.m_ranges32)
        add(range.begin, range.end);
    if (term.invert())
        characters.invert();
    return characters;
}

bool Planner::addCharacters(const Yarr::PatternTerm& term, const Latin1Set& characters, Sequence& sequence)
{
    unsigned minimum = term.quantityMinCount;
    unsigned maximum = term.quantityMaxCount == Yarr::quantifyInfinite ? Node::unbounded : term.quantityMaxCount.value();
    if (minimum == maximum && maximum > maxFixedCharacters)
        return false;
    if (minimum != maximum && minimum)
        return false;
    sequence.append(Node { .kind = Node::Kind::Characters, .isGreedy = term.quantityType != Yarr::QuantifierType::NonGreedy, .minimum = minimum, .maximum = maximum, .characters = characters });
    return true;
}

bool Planner::addGroup(const Yarr::PatternTerm& term, Sequence& sequence)
{
    Node group { .kind = Node::Kind::Group, .isGreedy = term.quantityType != Yarr::QuantifierType::NonGreedy };
    unsigned minimum = term.quantityMinCount;
    group.maximum = term.quantityMaxCount == Yarr::quantifyInfinite ? Node::unbounded : term.quantityMaxCount.value();
    if (term.capture())
        group.group = term.parentheses.subpatternId;
    if (term.containsAnyCaptures()) {
        group.firstInnerGroup = term.parentheses.subpatternId;
        group.lastInnerGroup = term.parentheses.lastSubpatternId;
    }
    if (!addAlternatives(*term.parentheses.disjunction, group.alternatives))
        return false;

    bool isLoop = group.maximum == Node::unbounded && minimum <= 1;
    bool isOptional = !minimum && group.maximum == 1;
    if (isLoop || isOptional) {
        group.minimum = minimum;
        sequence.append(WTF::move(group));
        return true;
    }
    if (minimum != group.maximum || minimum > maxCopiesOfGroup)
        return false;
    group.maximum = 1;
    for (unsigned copy = 0; copy < minimum; ++copy)
        sequence.append(group);
    return true;
}

bool Planner::addTerm(const Yarr::PatternTerm& term, Sequence& sequence)
{
    if (++m_numberOfNodes > m_maxNodes || term.matchDirection() != Yarr::Forward)
        return false;
    switch (term.type) {
    case Yarr::PatternTerm::Type::AssertionBOL:
        sequence.append(Node { .kind = term.multiline() ? Node::Kind::StartOfLine : Node::Kind::StartOfSubject });
        return true;
    case Yarr::PatternTerm::Type::AssertionEOL:
        sequence.append(Node { .kind = term.multiline() ? Node::Kind::EndOfLine : Node::Kind::EndOfSubject });
        return true;
    case Yarr::PatternTerm::Type::AssertionWordBoundary:
        sequence.append(Node { .kind = term.invert() ? Node::Kind::NoWordBoundary : Node::Kind::WordBoundary });
        return true;
    case Yarr::PatternTerm::Type::PatternCharacter:
    case Yarr::PatternTerm::Type::CharacterClass: {
        std::optional<Latin1Set> characters = charactersOf(term);
        return characters && addCharacters(term, *characters, sequence);
    }
    case Yarr::PatternTerm::Type::ParenthesesSubpattern:
        return addGroup(term, sequence);
    case Yarr::PatternTerm::Type::ParentheticalAssertion: {
        if (term.containsAnyCaptures())
            return false;
        Node assertion { .kind = term.invert() ? Node::Kind::NegativeLookahead : Node::Kind::Lookahead };
        if (!addAlternatives(*term.parentheses.disjunction, assertion.alternatives))
            return false;
        sequence.append(WTF::move(assertion));
        return true;
    }
    case Yarr::PatternTerm::Type::NumberedBackReference:
    case Yarr::PatternTerm::Type::NamedBackReference:
    case Yarr::PatternTerm::Type::NumberedForwardReference:
    case Yarr::PatternTerm::Type::NamedForwardReference:
    case Yarr::PatternTerm::Type::DotStarEnclosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Planner::addTerms(const Yarr::PatternAlternative& alternative, Sequence& sequence)
{
    if (alternative.matchDirection() != Yarr::Forward)
        return false;
    for (const Yarr::PatternTerm& term : alternative.m_terms) {
        if (!addTerm(term, sequence))
            return false;
    }
    return true;
}

bool Planner::addAlternatives(const Yarr::PatternDisjunction& disjunction, Vector<Sequence>& alternatives)
{
    for (auto& alternative : disjunction.m_alternatives) {
        Sequence sequence;
        if (!addTerms(*alternative, sequence))
            return false;
        alternatives.append(WTF::move(sequence));
    }
    return true;
}

Beginning Planner::beginningOf(const Node& node, const Beginning& rest) const
{
    switch (node.kind) {
    case Node::Kind::Characters: {
        if (node.minimum)
            return { .characters = node.characters };
        Beginning beginning = rest;
        beginning.characters.merge(node.characters);
        return beginning;
    }
    case Node::Kind::Group: {
        Beginning beginning;
        if (!node.minimum)
            beginning = rest;
        for (const Sequence& alternative : node.alternatives) {
            Beginning inner = beginningOf(alternative.span(), rest);
            beginning.characters.merge(inner.characters);
            beginning.takesAnyCharacter |= inner.takesAnyCharacter;
            beginning.cannotFail |= inner.cannotFail;
        }
        return beginning;
    }
    case Node::Kind::EndOfSubject:
        return { };
    case Node::Kind::EndOfLine: {
        Beginning beginning;
        beginning.characters.set('\n');
        beginning.characters.set('\r');
        return beginning;
    }
    case Node::Kind::StartOfSubject:
    case Node::Kind::StartOfLine:
    case Node::Kind::WordBoundary:
    case Node::Kind::NoWordBoundary:
    case Node::Kind::Lookahead:
    case Node::Kind::NegativeLookahead:
        return { .characters = rest.characters, .takesAnyCharacter = rest.takesAnyCharacter };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Beginning Planner::beginningOf(std::span<const Node> nodes, const Beginning& rest) const
{
    Beginning beginning = rest;
    for (size_t index = nodes.size(); index--;)
        beginning = beginningOf(nodes[index], beginning);
    return beginning;
}

static bool areDisjoint(const Latin1Set& a, const Latin1Set& b)
{
    Latin1Set both = a;
    both.filter(b);
    return both.isEmpty();
}

static bool rejects(const Beginning& beginning, const Latin1Set& characters)
{
    return !beginning.takesAnyCharacter && areDisjoint(beginning.characters, characters);
}

bool Planner::areExclusive(const Vector<Sequence>& alternatives, const Beginning& rest) const
{
    Vector<Beginning, 8> beginnings;
    for (const Sequence& alternative : alternatives)
        beginnings.append(beginningOf(alternative.span(), rest));
    for (size_t later = 1; later < alternatives.size(); ++later) {
        for (size_t earlier = 0; earlier < later; ++earlier) {
            if (!minimumLengthOf(alternatives[earlier].span()) || !rejects(beginnings[later], beginnings[earlier].characters))
                return false;
        }
    }
    return true;
}

Vector<Latin1Set> Planner::beginningsOf(const Vector<Sequence>& alternatives) const
{
    Vector<Latin1Set> beginnings;
    if (alternatives.size() < minAlternativesChosen || alternatives.size() > maxAlternativesChosen)
        return beginnings;
    for (const Sequence& alternative : alternatives) {
        if (!minimumLengthOf(alternative.span()))
            return { };
        beginnings.append(beginningOf(alternative.span(), { }).characters);
    }
    return beginnings;
}

bool Planner::analyzeGroup(Node& group, const Beginning& rest)
{
    const bool isLoop = group.maximum == Node::unbounded;
    const bool isOptional = !group.minimum && !isLoop;
    if ((isLoop || isOptional) && !group.isGreedy) {
        m_isRefused = true;
        return false;
    }

    Beginning all;
    bool mayBeEmpty = false;
    for (const Sequence& alternative : group.alternatives) {
        all.characters.merge(beginningOf(alternative.span(), { }).characters);
        mayBeEmpty |= !minimumLengthOf(alternative.span());
    }

    if (isOptional && mayBeEmpty) {
        m_isRefused = true;
        return false;
    }

    Beginning restOfAlternatives = rest;
    if (isLoop) {
        if (mayBeEmpty || (!rest.cannotFail && !rejects(rest, all.characters))) {
            m_isRefused = true;
            return false;
        }
        restOfAlternatives.characters.merge(all.characters);
    }

    bool isDeterministic = true;
    for (Sequence& alternative : group.alternatives)
        isDeterministic &= analyze(alternative, restOfAlternatives);

    if (group.alternatives.size() == 1 && !isLoop && !isOptional) {
        group.commits = true;
        return isDeterministic;
    }
    bool isExclusive = restOfAlternatives.cannotFail || areExclusive(group.alternatives, restOfAlternatives);
    if (isOptional && isExclusive)
        isExclusive = rest.cannotFail || rejects(rest, all.characters);
    group.commits = isDeterministic && isExclusive;
    if (isLoop && !group.commits)
        m_isRefused = true;
    if (group.commits)
        group.beginnings = beginningsOf(group.alternatives);
    return group.commits;
}

bool Planner::analyze(Node& node, const Beginning& rest)
{
    switch (node.kind) {
    case Node::Kind::Characters:
        if (!node.isRun())
            return true;
        node.givesBack = node.isGreedy ? !rest.cannotFail && !rejects(rest, node.characters) : !rest.cannotFail;
        return !node.givesBack;
    case Node::Kind::Group:
        return analyzeGroup(node, rest);
    case Node::Kind::Lookahead:
    case Node::Kind::NegativeLookahead:
        for (Sequence& alternative : node.alternatives)
            analyze(alternative, endOfPattern());
        node.beginnings = beginningsOf(node.alternatives);
        return true;
    case Node::Kind::StartOfSubject:
    case Node::Kind::StartOfLine:
    case Node::Kind::EndOfSubject:
    case Node::Kind::EndOfLine:
    case Node::Kind::WordBoundary:
    case Node::Kind::NoWordBoundary:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Planner::analyze(Sequence& sequence, const Beginning& rest)
{
    bool isDeterministic = true;
    Beginning restOfNode = rest;
    for (size_t index = sequence.size(); index--;) {
        isDeterministic &= analyze(sequence[index], restOfNode);
        restOfNode = beginningOf(sequence[index], restOfNode);
    }
    return isDeterministic;
}

unsigned Planner::costOf(std::span<const Node> nodes, unsigned costOfRest) const
{
    unsigned cost = costOfRest;
    for (size_t index = nodes.size(); index--;) {
        const Node& node = nodes[index];
        unsigned own = node.isRun() || node.kind != Node::Kind::Characters ? 2 : 1;
        unsigned inner = 0;
        for (const Sequence& alternative : node.alternatives)
            inner = addLengths(inner, costOf(alternative.span(), node.commits ? 0 : cost));
        if (!node.commits && !node.minimum)
            inner = addLengths(inner, cost);
        cost = node.commits ? addLengths(addLengths(cost, own), inner) : addLengths(own, inner);
    }
    return cost;
}

std::unique_ptr<const RegExpMatchPlan> Planner::plan()
{
    if (m_pattern.m_containsBackreferences || m_pattern.m_containsLookbehinds || m_pattern.m_numDuplicateNamedCaptureGroups || m_pattern.containsUnsignedLengthPattern())
        return nullptr;

    auto plan = makeUnique<RegExpMatchPlan>();
    plan->numberOfGroups = m_pattern.m_numSubpatterns;
    auto& alternatives = m_pattern.m_body->m_alternatives;
    const bool hasAlternativesOnce = std::ranges::any_of(alternatives, [](auto& alternative) { return alternative->onceThrough(); });
    for (auto& alternative : alternatives) {
        if (hasAlternativesOnce && !alternative->onceThrough()) {
            plan->isSearched = true;
            continue;
        }
        Sequence sequence;
        if (!addTerms(*alternative, sequence))
            return nullptr;
        plan->alternatives.append(WTF::move(sequence));
    }
    plan->isSearched |= !hasAlternativesOnce;

    unsigned maximumLengthToEnd = 0;
    Beginning beginning;
    plan->minimumLength = Node::unbounded;
    for (Sequence& alternative : plan->alternatives) {
        analyze(alternative, endOfPattern());
        plan->cost = addLengths(plan->cost, costOf(alternative.span(), 1));
        plan->minimumLength = std::min(plan->minimumLength, minimumLengthOf(alternative.span()));
        maximumLengthToEnd = std::max(maximumLengthToEnd, alternative.isEmpty() || alternative.last().kind != Node::Kind::EndOfSubject ? Node::unbounded : maximumLengthOf(alternative));
        beginning.characters.merge(beginningOf(alternative.span(), endOfPattern()).characters);
    }
    if (m_isRefused || plan->cost > m_maxNodes || plan->minimumLength > maxFixedCharacters)
        return nullptr;
    plan->beginnings = beginningsOf(plan->alternatives);
    if (maximumLengthToEnd != Node::unbounded)
        plan->maximumLengthToEnd = maximumLengthToEnd;
    if (plan->minimumLength)
        plan->firstCharacters = beginning.characters;
    return plan;
}

std::optional<bool> Planner::addFirstCharacters(const Yarr::PatternAlternative& alternative, Latin1Set& characters)
{
    for (const Yarr::PatternTerm& term : alternative.m_terms) {
        switch (term.type) {
        case Yarr::PatternTerm::Type::AssertionBOL:
        case Yarr::PatternTerm::Type::AssertionEOL:
        case Yarr::PatternTerm::Type::AssertionWordBoundary:
        case Yarr::PatternTerm::Type::ParentheticalAssertion:
            continue;
        case Yarr::PatternTerm::Type::PatternCharacter:
        case Yarr::PatternTerm::Type::CharacterClass: {
            std::optional<Latin1Set> own = charactersOf(term);
            if (!own)
                return std::nullopt;
            characters.merge(*own);
            if (term.quantityMinCount)
                return false;
            continue;
        }
        case Yarr::PatternTerm::Type::ParenthesesSubpattern: {
            std::optional<bool> mayBeEmpty = addFirstCharacters(*term.parentheses.disjunction, characters);
            if (!mayBeEmpty)
                return std::nullopt;
            if (term.quantityMinCount && !*mayBeEmpty)
                return false;
            continue;
        }
        case Yarr::PatternTerm::Type::NumberedBackReference:
        case Yarr::PatternTerm::Type::NamedBackReference:
        case Yarr::PatternTerm::Type::NumberedForwardReference:
        case Yarr::PatternTerm::Type::NamedForwardReference:
        case Yarr::PatternTerm::Type::DotStarEnclosure:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return true;
}

std::optional<bool> Planner::addFirstCharacters(const Yarr::PatternDisjunction& disjunction, Latin1Set& characters)
{
    bool mayBeEmpty = false;
    for (auto& alternative : disjunction.m_alternatives) {
        std::optional<bool> ofAlternative = addFirstCharacters(*alternative, characters);
        if (!ofAlternative)
            return std::nullopt;
        mayBeEmpty |= *ofAlternative;
    }
    return mayBeEmpty;
}

std::unique_ptr<const RegExpMatchPlan> Planner::planOfPrefix() const
{
    Latin1Set characters;
    std::optional<bool> mayBeEmpty = addFirstCharacters(*m_pattern.m_body, characters);
    if (!mayBeEmpty || *mayBeEmpty || characters.count() == characters.size())
        return nullptr;
    auto plan = makeUnique<RegExpMatchPlan>();
    plan->alternatives.append(Sequence { Node { .characters = characters } });
    plan->cost = 1;
    plan->minimumLength = 1;
    plan->firstCharacters = characters;
    plan->isPrefix = true;
    return plan;
}

std::unique_ptr<const RegExpMatchPlan> planRegExpMatch(const Yarr::YarrPattern& pattern, bool isSticky)
{
    Planner planner(pattern, isSticky);
    std::unique_ptr<const RegExpMatchPlan> plan = planner.plan();
    return plan || !isSticky ? WTF::move(plan) : planner.planOfPrefix();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
