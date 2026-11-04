/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringParsers.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <limits>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- The nodes of a type

namespace {

struct ParsedTypeAlternative {
    ParserShape shape { ParserShape::Unknown };
    ParserLiteral literal;
    TypeID contents;
    LayoutID layout;
};

struct FamilyOfParsedMembers {
    LayoutSetID family;
    LayoutID owner;
};

} // anonymous namespace

// Two layouts may fit each other's types: `{ port, host, tags? }` and a literal `{ port, host }`. A parser builds the declared one, which has a field for every member.
static bool isBetterHeadForParser(const Program& program, LayoutID candidate, LayoutID other)
{
    auto rankOf = [&](LayoutID id) {
        const Layout& layout = program.at(id);
        return std::tuple { !!layout.name, layout.fields.size(), std::numeric_limits<uint32_t>::max() - id.index() };
    };
    return rankOf(candidate) > rankOf(other);
}

static std::span<const LayoutID> familyDeclaredByHeadForParser(const Program& program, LayoutID declared)
{
    if (!program.hasDeclaredMembers(declared))
        return { };
    return membersOf(program, program.declaredMembersOf(declared));
}

static Vector<FamilyOfParsedMembers> familiesOfParsedMembers(const Program& program, std::span<const LayoutID> members)
{
    Vector<FamilyOfParsedMembers> families;
    for (LayoutID member : members) {
        if (program.hasDeclaredMembers(member))
            families.append({ program.declaredMembersOf(member), member });
    }
    // Sets are told apart by their members' owner with the lowest number, never by the number of the set, which follows thread arrival.
    AOT::sort(families, { }, [](const FamilyOfParsedMembers& family) {
        return family.owner;
    });
    Vector<FamilyOfParsedMembers> distinct;
    // By LayoutSetID::bits(), widened so that no id reads as an empty place of the table. A type may have thousands of members.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> placesOfFamilies;
    for (const FamilyOfParsedMembers& family : families) {
        auto place = placesOfFamilies.add(family.family.bits(), static_cast<uint32_t>(distinct.size()));
        if (place.isNewEntry)
            distinct.append(family);
        else if (isBetterHeadForParser(program, family.owner, distinct[place.iterator->value].owner))
            distinct[place.iterator->value].owner = family.owner;
    }
    AOT::stableSort(distinct, std::ranges::greater { }, [&](const FamilyOfParsedMembers& family) {
        return membersOf(program, family.family).size();
    });
    return distinct;
}

// The record layouts of the type that fit under no other member's declaration.
static Vector<LayoutID, 4> headLayoutsOfParsedType(const Program& program, const Type& type)
{
    std::span<const LayoutID> members = membersOf(program, type);
    Vector<LayoutID, 4> candidates;
    for (LayoutID member : members) {
        if (program.at(member).kind == LayoutKind::Record)
            candidates.append(member);
    }
    for (const FamilyOfParsedMembers& family : familiesOfParsedMembers(program, members)) {
        if (candidates.size() <= 1)
            break;
        std::span<const LayoutID> subtypes = membersOf(program, family.family);
        candidates.removeAllMatching([&](LayoutID candidate) {
            if (candidate == family.owner || !std::ranges::binary_search(subtypes, candidate))
                return false;
            bool staysBesideOwner = isBetterHeadForParser(program, candidate, family.owner) && std::ranges::binary_search(familyDeclaredByHeadForParser(program, candidate), family.owner);
            return !staysBesideOwner;
        });
    }
    return candidates;
}

static Vector<ParsedTypeAlternative, 4> alternativesOfParsedType(const Program& program, TypeID id)
{
    Type type = program.at(id);
    Vector<ParsedTypeAlternative, 4> alternatives;
    if (type.primitives & Primitive::Null)
        alternatives.append({ .shape = ParserShape::Null });
    if (uint16_t booleans = type.primitives & Primitive::Boolean; booleans == Primitive::Boolean)
        alternatives.append({ .shape = ParserShape::Boolean });
    else if (booleans)
        alternatives.append({ .shape = ParserShape::BooleanLiteral, .literal = { .kind = ParserLiteral::Kind::Boolean, .boolean = booleans == Primitive::True } });
    if (type.primitives & Primitive::Number)
        alternatives.append({ .shape = ParserShape::Number });
    if (type.primitives & Primitive::String)
        alternatives.append({ .shape = ParserShape::String });
    else if (type.hasLiteralStrings()) {
        TypeReader(program).at(type.stringLiterals).forEach([&](StringID string) {
            alternatives.append({ .shape = ParserShape::StringLiteral, .literal = { .kind = ParserLiteral::Kind::String, .string = string } });
        });
    }
    if (const ClassPart* array = findClassPart(program, type, BuiltinClasses::Array))
        alternatives.append({ .shape = ParserShape::Array, .contents = array->writtenContentsAt(program.builtins(), ContentsSlot::Element) });
    for (LayoutID layout : headLayoutsOfParsedType(program, type))
        alternatives.append({ .shape = ParserShape::Object, .layout = layout });
    return alternatives;
}

static const ParsedTypeAlternative* onlyContainerAmongParsedAlternatives(const Vector<ParsedTypeAlternative, 4>& alternatives)
{
    const ParsedTypeAlternative* found = nullptr;
    for (const ParsedTypeAlternative& alternative : alternatives) {
        if (alternative.shape < ParserShape::Array)
            continue;
        if (found)
            return nullptr;
        found = &alternative;
    }
    return found;
}

LayoutID layoutOfObjectsInOnlyArrayOf(const Program& program, TypeID type)
{
    if (program.at(type).hasUnboundedUnknown())
        return { };
    Vector<ParsedTypeAlternative, 4> alternatives = alternativesOfParsedType(program, type);
    const ParsedTypeAlternative* array = onlyContainerAmongParsedAlternatives(alternatives);
    if (!array || array->shape != ParserShape::Array || !array->contents || program.at(array->contents).hasUnboundedUnknown())
        return { };
    Vector<ParsedTypeAlternative, 4> elements = alternativesOfParsedType(program, array->contents);
    const ParsedTypeAlternative* object = onlyContainerAmongParsedAlternatives(elements);
    return object && object->shape == ParserShape::Object ? object->layout : LayoutID();
}

// An own, enumerable property under a text: what JSON.stringify writes and JSON.parse makes.
bool isMemberThatJSONSpells(const Program& program, const Field& field)
{
    constexpr OptionSet<FieldFlags> flagsOfOtherFields { FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsBrandOfMethods };
    return field.key && program.at(field.key).kind == PropertyKeyKind::Text && !field.form.contains(FieldForm::IsNotEnumerable) && !field.flags.containsAny(flagsOfOtherFields);
}

TypeID typeOfParsedMember(const Field& field)
{
    TypeID written = field.declared.expected().predicted();
    return written ? written : TypeTable::anything;
}

bool isRecordThatParsersCanMake(const Program& program, LayoutID id)
{
    constexpr OptionSet<LayoutFlags> flagsOfOtherLayouts { LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction, LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsNeverAllocated, LayoutFlags::IsWrittenUnderDictionaryType, LayoutFlags::IsDerived, LayoutFlags::IsOfCreatedObject };
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.constructor || layout.base || layout.flags.containsAny(flagsOfOtherLayouts) || !layout.fields.size())
        return false;
    return std::ranges::all_of(layout.fields, [&](const Field& field) {
        if (!field.key)
            return false;
        TypeID written = field.declared.written();
        if (!written)
            return true;
        Type type = program.at(written);
        return type.isAnything() || !type.mayBeFunction();
    });
}

static ParserNode describeParserAlternative(const ParserContext& context, TypeID type, const ParsedTypeAlternative& alternative)
{
    const Program& program = context.program;
    ParserNode node { .shape = alternative.shape, .type = type, .literal = alternative.literal, .layout = alternative.layout };
    if (node.shape == ParserShape::Array)
        node.children.append(ParserNodeRef { alternative.contents ? alternative.contents : TypeTable::anything });
    if (node.shape != ParserShape::Object)
        return node;

    const auto& fields = program.at(node.layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isMemberThatJSONSpells(program, field))
            continue;
        TypeID written = typeOfParsedMember(field);
        // An object without a value in a slot that is always filled, or filled at birth, is one that no other code makes.
        const PlacedSlot& slot = context.plan.slot(node.layout, index);
        const LinkedCell* proven = context.facts.linked().findCellOfField({ node.layout, index });
        bool isAlwaysFilled = proven && proven->isFilled;
        bool mayBeLeftOut = !field.declared.written() || program.at(written).primitives & Primitive::Undefined;
        node.members.append({ field.key, ParserNodeRef { written }, index, mayBeLeftOut && !isAlwaysFilled && !slot.isFilledAtBirth });
    }
    return node;
}

ParserNode describeParserNode(const ParserContext& context, ParserNodeRef ref)
{
    auto [type, part] = ref;
    Vector<ParsedTypeAlternative, 4> alternatives = alternativesOfParsedType(context.program, type);
    if (part) {
        RELEASE_ASSERT(part <= alternatives.size());
        return describeParserAlternative(context, type, alternatives[part - 1]);
    }

    ParserNode node { .type = type };
    if (context.program.at(type).hasUnboundedUnknown() || alternatives.isEmpty())
        return node;
    if (alternatives.size() == 1)
        return describeParserAlternative(context, type, alternatives[0]);

    node.shape = ParserShape::Union;
    for (uint32_t index = 0; index < alternatives.size(); ++index)
        node.children.append(ParserNodeRef { type, index + 1 });
    return node;
}

static OptionSet<JSONValueClass> classesAcceptedByParserShape(ParserShape shape)
{
    switch (shape) {
    case ParserShape::Unknown:
        return { JSONValueClass::Null, JSONValueClass::Boolean, JSONValueClass::Number, JSONValueClass::String, JSONValueClass::Array, JSONValueClass::Object };
    case ParserShape::Null:
        return JSONValueClass::Null;
    case ParserShape::Boolean:
    case ParserShape::BooleanLiteral:
        return JSONValueClass::Boolean;
    case ParserShape::Number:
        return JSONValueClass::Number;
    case ParserShape::String:
    case ParserShape::StringLiteral:
        return JSONValueClass::String;
    case ParserShape::Array:
        return JSONValueClass::Array;
    case ParserShape::Object:
        return JSONValueClass::Object;
    case ParserShape::Union:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionSet<JSONValueClass> valueClassesOf(const ParserContext& context, ParserNodeRef ref)
{
    ParserNode node = describeParserNode(context, ref);
    OptionSet<JSONValueClass> classes = classesAcceptedByParserShape(node.shape);
    // The arms of a union are alternatives of its own type, and none of them is a union.
    for (ParserNodeRef arm : node.shape == ParserShape::Union ? node.children.span() : std::span<const ParserNodeRef>())
        classes.add(classesAcceptedByParserShape(describeParserNode(context, arm).shape));
    return classes;
}

// ---- Discriminants

static std::optional<ParserLiteral> findTagOfParserArm(const ParserContext& context, const ParserNode& arm, PropertyKeyID key)
{
    if (arm.shape != ParserShape::Object)
        return std::nullopt;
    const ParserMember* member = std::ranges::find(arm.members, key, &ParserMember::key);
    if (member == arm.members.end())
        return std::nullopt;

    ParserNode tag = describeParserNode(context, member->node);
    if (tag.shape != ParserShape::Null && tag.shape != ParserShape::BooleanLiteral && tag.shape != ParserShape::StringLiteral)
        return std::nullopt;
    return tag.literal;
}

static std::optional<TaggedParserArms> partitionParserArmsByTag(const ParserContext& context, std::span<const ParserNodeRef> arms, std::span<const ParserNode> nodes, PropertyKeyID key)
{
    TaggedParserArms result { .discriminant = { .key = key } };
    for (size_t index = 0; index < arms.size(); ++index) {
        std::optional<ParserLiteral> tag = findTagOfParserArm(context, nodes[index], key);
        if (!tag) {
            result.untagged.append(arms[index]);
            continue;
        }
        if (result.discriminant.tagOfArm.contains(*tag))
            return std::nullopt;
        result.discriminant.tagOfArm.append(*tag);
        result.tagged.append(arms[index]);
    }
    if (result.tagged.size() < 2)
        return std::nullopt;
    return result;
}

std::optional<TaggedParserArms> findTaggedParserArms(const ParserContext& context, std::span<const ParserNodeRef> arms)
{
    Vector<ParserNode, 4> nodes(arms.size(), [&](size_t index) {
        return describeParserNode(context, arms[index]);
    });
    size_t numberOfObjects = std::ranges::count(nodes, ParserShape::Object, &ParserNode::shape);

    std::optional<TaggedParserArms> firstPartition;
    Vector<PropertyKeyID, 8> triedKeys;
    for (const ParserNode& node : nodes) {
        for (const ParserMember& member : node.members) {
            if (triedKeys.contains(member.key) || !findTagOfParserArm(context, node, member.key))
                continue;
            triedKeys.append(member.key);
            std::optional<TaggedParserArms> partition = partitionParserArmsByTag(context, arms, nodes.span(), member.key);
            if (!partition)
                continue;
            if (partition->tagged.size() == numberOfObjects)
                return partition;
            if (!firstPartition)
                firstPartition = WTF::move(partition);
        }
    }
    return firstPartition;
}

// ---- Which nodes are compiled

using ParserNodeKey = std::pair<TypeID, uint32_t>;

static bool canReachParserNode(const ParserContext& context, ParserNodeRef start, ParserNodeRef target)
{
    HashSet<ParserNodeKey> visited;
    visited.add({ start.type, start.part });
    Vector<ParserNodeRef, 16> worklist { start };
    bool isReached = false;
    while (!isReached && !worklist.isEmpty()) {
        describeParserNode(context, worklist.takeLast()).forEachChild([&](ParserNodeRef child) {
            if (child == target)
                isReached = true;
            else if (visited.add({ child.type, child.part }).isNewEntry)
                worklist.append(child);
        });
    }
    return isReached;
}

static Vector<ParserNodeRef, 4> parserArmsOfClass(const ParserContext& context, std::span<const ParserNodeRef> arms, JSONValueClass valueClass)
{
    Vector<ParserNodeRef, 4> candidates;
    for (ParserNodeRef arm : arms) {
        if (valueClassesOf(context, arm).contains(valueClass))
            candidates.append(arm);
    }
    return candidates;
}

// Each arm that is tried looks anew at all that it holds. Two arms that both lead back to the union look at a text of depth n 2^n times.
static bool triesSeveralRecursiveParserArms(const ParserContext& context, std::span<const ParserNodeRef> candidates, ParserNodeRef unionRef)
{
    auto isRecursive = [&](ParserNodeRef arm) {
        return canReachParserNode(context, arm, unionRef);
    };
    std::optional<TaggedParserArms> arms = findTaggedParserArms(context, candidates);
    unsigned numberOfRecursiveTrials = arms && std::ranges::any_of(arms->tagged, isRecursive) ? 1 : 0;
    for (ParserNodeRef arm : arms ? arms->untagged.span() : candidates) {
        if (isRecursive(arm) && ++numberOfRecursiveTrials > 1)
            return true;
    }
    return false;
}

// How often one part of a value is looked at below a node, at most: unions below unions multiply. A parser that looks more often than this is slower than the parser of untyped values.
static constexpr unsigned mostLooksAtOnePartOfParsedText = 64;
static constexpr unsigned mostDepthOfCountedLooksOfParser = 128;

static unsigned countLooksAtOnePartOfParsedText(const ParserContext& context, ParserNodeRef ref, HashMap<ParserNodeKey, unsigned>& known, unsigned depth)
{
    if (depth > mostDepthOfCountedLooksOfParser)
        return mostLooksAtOnePartOfParsedText + 1;
    ParserNodeKey key { ref.type, ref.part };
    if (auto added = known.add(key, 1); !added.isNewEntry)
        return added.iterator->value;

    auto looksBelow = [&](ParserNodeRef child) {
        return countLooksAtOnePartOfParsedText(context, child, known, depth + 1);
    };
    ParserNode node = describeParserNode(context, ref);
    unsigned looks = 1;
    if (node.shape != ParserShape::Union) {
        node.forEachChild([&](ParserNodeRef child) {
            looks = std::max(looks, looksBelow(child));
        });
    } else {
        for (JSONValueClass valueClass : { JSONValueClass::Array, JSONValueClass::Object }) {
            Vector<ParserNodeRef, 4> candidates = parserArmsOfClass(context, node.children.span(), valueClass);
            std::optional<TaggedParserArms> tagged = findTaggedParserArms(context, candidates.span());
            unsigned looksOfClass = 0;
            if (tagged) {
                for (ParserNodeRef arm : tagged->tagged)
                    looksOfClass = std::max(looksOfClass, looksBelow(arm));
            }
            for (ParserNodeRef arm : tagged ? tagged->untagged.span() : candidates.span())
                looksOfClass += looksBelow(arm);
            looks = std::max(looks, looksOfClass);
        }
    }
    looks = std::min(looks, mostLooksAtOnePartOfParsedText + 1);
    known.set(key, looks);
    return looks;
}

static bool looksAgainAndAgainAtParsedText(const ParserContext& context, ParserNodeRef unionRef, const ParserNode& unionNode)
{
    HashMap<ParserNodeKey, unsigned> known;
    if (countLooksAtOnePartOfParsedText(context, unionRef, known, 0) > mostLooksAtOnePartOfParsedText)
        return true;
    for (JSONValueClass valueClass : { JSONValueClass::Array, JSONValueClass::Object }) {
        Vector<ParserNodeRef, 4> candidates = parserArmsOfClass(context, unionNode.children.span(), valueClass);
        if (candidates.size() > 1 && triesSeveralRecursiveParserArms(context, candidates.span(), unionRef))
            return true;
    }
    return false;
}

bool parsersMayAllocate(const ParserContext& context, LayoutID layout)
{
    if (!layout || !isRecordThatParsersCanMake(context.program, layout))
        return false;
    if (!context.plan.isInUse(layout) || !context.plan.layout(layout).holdsCompiledObjects() || !context.plan.shapes().generalShapeOf(layout))
        return false;
    // A parser makes objects of the layout where no allocation of the program stands. Of a layout that Facts lists, linked() has given every proof up.
    return context.facts.parsersMayMake(layout);
}

bool isParserNodeCompiled(const ParserContext& context, ParserNodeRef ref)
{
    ParserNode node = describeParserNode(context, ref);
    if (node.shape == ParserShape::Union)
        return !looksAgainAndAgainAtParsedText(context, ref, node);
    return node.shape != ParserShape::Object || parsersMayAllocate(context, node.layout);
}

bool hasTypedPartForParser(const ParserContext& context, ParserNodeRef ref)
{
    ParserNode node = describeParserNode(context, ref);
    if (!node.isContainer() || !isParserNodeCompiled(context, ref))
        return false;
    if (node.shape == ParserShape::Array)
        return !describeParserNode(context, node.children[0]).isContainer() || hasTypedPartForParser(context, node.children[0]);
    return node.shape != ParserShape::Union || node.children.containsIf([&](ParserNodeRef arm) {
        return hasTypedPartForParser(context, arm);
    });
}

// ---- The functions

FunctionID parserFunctionOf(const Program& program, TypeID type)
{
    auto found = std::ranges::lower_bound(program.parsersByType, type.bits(), { }, &std::pair<uint32_t, FunctionID>::first);
    return found != program.parsersByType.end() && found->first == type.bits() ? found->second : FunctionID();
}

// TypeID::bits() of the types that have been looked at, with a parser or without.
using ParsedTypesLookedAt = HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static bool hasParserOfItsOwn(const Vector<ParsedTypeAlternative, 4>& alternatives)
{
    return alternatives.size() > 1 || (alternatives.size() == 1 && alternatives[0].shape >= ParserShape::Array);
}

template<typename Functor>
static void forEachTypeBelowParsedAlternatives(const Program& program, const Vector<ParsedTypeAlternative, 4>& alternatives, NOESCAPE const Functor& functor)
{
    for (const ParsedTypeAlternative& alternative : alternatives) {
        if (alternative.shape == ParserShape::Array)
            functor(alternative.contents);
        if (alternative.shape != ParserShape::Object)
            continue;
        for (const Field& field : program.at(alternative.layout).fields) {
            if (isMemberThatJSONSpells(program, field))
                functor(typeOfParsedMember(field));
        }
    }
}

// Every type that a parser of `root` may call a parser for and that has not been looked at, `root` first, each once, in an order that follows from the tables alone: functor(TypeID, const Vector<ParsedTypeAlternative, 4>&).
template<typename Functor>
static void forEachTypeWithParser(const Program& program, TypeID root, ParsedTypesLookedAt& lookedAt, NOESCAPE const Functor& functor)
{
    Vector<TypeID, 8> worklist { root };
    while (!worklist.isEmpty()) {
        TypeID type = worklist.takeLast();
        if (!type || !lookedAt.add(type.bits()).isNewEntry || program.at(type).hasUnboundedUnknown())
            continue;
        Vector<ParsedTypeAlternative, 4> alternatives = alternativesOfParsedType(program, type);
        if (!hasParserOfItsOwn(alternatives))
            continue;
        functor(type, alternatives);
        // Taken off the end, so what is listed last here is looked at first: the order is a function of the tables either way.
        forEachTypeBelowParsedAlternatives(program, alternatives, [&](TypeID below) {
            worklist.append(below);
        });
    }
}

void forEachTypeParsedBelow(const Program& program, TypeID type, NOESCAPE const ScopedLambda<void(TypeID)>& functor)
{
    if (!type || program.at(type).hasUnboundedUnknown())
        return;
    forEachTypeBelowParsedAlternatives(program, alternativesOfParsedType(program, type), [&](TypeID below) {
        if (below)
            functor(below);
    });
}

void forEachLayoutParsedAt(const Program& program, TypeID type, NOESCAPE const ScopedLambda<void(LayoutID)>& functor)
{
    if (!type || program.at(type).hasUnboundedUnknown())
        return;
    for (const ParsedTypeAlternative& alternative : alternativesOfParsedType(program, type)) {
        if (alternative.shape == ParserShape::Object)
            functor(alternative.layout);
    }
}

void orderOfFieldsInAllocationOfParser(const Program& program, LayoutID layout, Vector<uint32_t, 8>& fields)
{
    std::span<const uint32_t> declared = program.at(layout).declaredOrder.span();
    AOT::sort(fields, { }, [&](uint32_t field) {
        return std::pair { static_cast<size_t>(std::ranges::find(declared, field) - declared.begin()), field };
    });
}

Vector<uint32_t, 8> fieldsThatTextMayNotLeaveOut(const Program& program, LayoutID layout)
{
    Vector<uint32_t, 8> required;
    const auto& fields = program.at(layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isMemberThatJSONSpells(program, field))
            continue;
        bool mayBeLeftOut = !field.declared.written() || program.at(typeOfParsedMember(field)).primitives & Primitive::Undefined;
        if (!mayBeLeftOut)
            required.append(index);
    }
    orderOfFieldsInAllocationOfParser(program, layout, required);
    return required;
}

ObjectsMadeByParsers objectsMadeByParsersOf(const Program& program, TypeID root)
{
    ObjectsMadeByParsers made;
    ParsedTypesLookedAt lookedAt;
    forEachTypeWithParser(program, root, lookedAt, [&](TypeID type, const Vector<ParsedTypeAlternative, 4>& alternatives) {
        for (const ParsedTypeAlternative& alternative : alternatives) {
            if (alternative.shape == ParserShape::Array)
                made.typesWithArrays.append(type);
            if (alternative.shape == ParserShape::Object)
                made.layouts.append(alternative.layout);
        }
    });
    AOT::sort(made.layouts);
    made.layouts.shrink(std::ranges::unique(made.layouts).begin() - made.layouts.begin());
    return made;
}

void Program::appendParsers()
{
    if (!parsersByType.isEmpty())
        return;

    Vector<TypeID> roots;
    for (const ExpectedResult& expected : expectedResults) {
        if (!expected.flags.contains(ExpectedResultFlags::IsRowsOfQuery))
            roots.append(expected.type);
    }
    appendParsersFor(roots.span());
}

void Program::appendParsersFor(std::span<const TypeID> roots)
{
    ParsedTypesLookedAt lookedAt;
    for (auto [bitsOfType, parser] : parsersByType)
        lookedAt.add(bitsOfType);
    Vector<TypeID> parsedTypes;
    for (TypeID root : roots) {
        forEachTypeWithParser(*this, root, lookedAt, [&](TypeID type, const Vector<ParsedTypeAlternative, 4>&) {
            parsedTypes.append(type);
        });
    }
    if (parsedTypes.isEmpty())
        return;

    // A parser takes and returns what JSONParseNode takes and gives.
    TypeArena arena(types, ArenaKeeps::InProgram);
    constexpr uint16_t primitivesThatJSONSpells = Primitive::Null | Primitive::Boolean | Primitive::Number | Primitive::String;
    DeclaredType cursor(Proven::byCompiler(arena.forOpaque(OpaqueKind::Cursor)));
    DeclaredType outcome(Proven::byCompiler(arena.integer({ 0, numberOfEnumerators<JSONOutcome> - 1 })));
    DeclaredType value(Proven::byCompiler(arena.unionOf(TypeArena::fromPrimitives(primitivesThatJSONSpells), TypeTable::object)));

    // Reader::readFunctions() has left room for a quarter more, so the functions move only where there are more parsers than that. The parsers that the scan finds go into the same room.
    for (TypeID type : parsedTypes) {
        Function parser;
        parser.position = BodyValue::noPosition;
        parser.kind = FunctionKind::Arrow;
        parser.role = FunctionRole::Parser;
        parser.creator = CreatorRecord::Parser { type, LayoutID(), TypedResultForm::ParsedText };
        // It is written in no module, and what it does is a part of the call of the built-in function.
        parser.flags = FunctionFlags::IsHiddenInStackTraces;
        parser.length = 1;
        // It takes no `this`: walkParserUnit() reads here which type it parses.
        parser.thisType = DeclaredType(ExpectedType(type));
        parser.parameters.append(cursor);
        parser.results.appendList({ outcome, value, cursor });
        parser.capabilities = initialCapabilitiesOf(parser);
        parsersByType.append({ type.bits(), FunctionID(static_cast<uint32_t>(functions.size())) });
        functions.append(WTF::move(parser));
    }
    AOT::sort(parsersByType, { }, &std::pair<uint32_t, FunctionID>::first);
    types.objects().addFunctionsWithoutSites(static_cast<uint32_t>(functions.size()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
