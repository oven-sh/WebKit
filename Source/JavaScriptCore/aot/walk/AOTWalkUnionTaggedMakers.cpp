/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function Node(kind, pos, end) { this.kind = kind; .. }   new Node(Kind.Name)`: the object gets the layout of the member of the tagged family that its tag names (LinkTables::taggedMakers).
// The maker's own layout is right for every object and so is any member's, so nothing here is a proof, and a tag may always be left out. At a suspension: nothing. At a throw: an allocation throws as the one that it stands for.
// Withheld with TypedSlots: Link lists no maker, and every object has its maker's layout.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTTaggedLayouts.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTUnionTaggedMakers.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/IterationStatus.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename Tag>
struct CaseOfTagInFamily {
    Tag tag { };
    LayoutID layout;
};

struct TagsOfFamily {
    Vector<int64_t, 16> integers;
    Vector<StringID, 16> strings;

    void append(const LayoutTag& tag)
    {
        WTF::switchOn(tag,
            [&](int64_t integer) {
                integers.append(integer);
            },
            [&](StringID string) {
                strings.append(string);
            });
    }
};

struct LayoutsForTag {
    Vector<CaseOfTagInFamily<int64_t>, 16> integerCases;
    Vector<CaseOfTagInFamily<StringID>, 16> stringCases;
    // Of an object whose tag is none of the cases.
    LayoutID defaultLayout;

    bool isOneLayout() const { return integerCases.isEmpty() && stringCases.isEmpty(); }
};

// A SwitchString resolves a rope, which an allocation as written does not: the looks at loops and at handlers have not counted on it.
enum class SwitchOnTextsOfTags : bool { IsNotForeseen, IsForeseen };

} // anonymous namespace

static const TaggedMaker* findMakerForTaggedMakers(const Walker& walker, FunctionID constructor)
{
    return findTaggedMaker(walker.facts().linked().taggedMakers.span(), constructor);
}

// The objects of any other member have the maker's own layout.
static bool hasObjectsOfItsOwnForTaggedMakers(const Walker& walker, LayoutID member)
{
    return walker.plan().isInUse(member) && walker.plan().slots().holdsCompiledObjects(member);
}

static LayoutID layoutOfTagInFamily(const Walker& walker, const TaggedFamily& family, const LayoutTag& tag)
{
    LayoutID member = layoutMadeWithTag(family, tag);
    return member && hasObjectsOfItsOwnForTaggedMakers(walker, member) ? member : family.maker;
}

// A text that the walk has made may be spelled like a tag, which is a text of the program.
static LayoutID layoutOfTextInFamily(const Walker& walker, const TaggedFamily& family, StringID text)
{
    const TypeArena& types = walker.types();
    for (const TaggedLayout& member : family.members) {
        for (const LayoutTag& tag : member.tags) {
            const StringID* string = std::get_if<StringID>(&tag);
            if (string && types.spellSameText(*string, text))
                return layoutOfTagInFamily(walker, family, tag);
        }
    }
    return family.maker;
}

// No type tells two doubles apart, and none names a made text: those two are read off the constant.
static PrimitiveKnownToFolding primitiveHeldForTaggedMakers(const Walker& walker, Reference reference)
{
    if (reference.isRecord()) {
        const Operation& record = walker.code().at(reference.id());
        if (record.opcode == Opcode::Double)
            return PrimitiveKnownToFolding::number(record.number());
        if (record.opcode == Opcode::String)
            return PrimitiveKnownToFolding::text(record.string());
    }
    Type type = walker.types().at(walker.typeOf(reference));
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

// The maker's own layout for a value that is the tag of no member. Null: the value is not known here.
static LayoutID layoutOfKnownTagInFamily(const Walker& walker, const TaggedFamily& family, Reference tag)
{
    PrimitiveKnownToFolding known = primitiveHeldForTaggedMakers(walker, tag);
    switch (known.kind()) {
    case PrimitiveKnownToFolding::Kind::Unknown: {
        Type type = walker.types().at(walker.typeOf(tag));
        return type.isJavaScriptValue() && type.isObject() ? family.maker : LayoutID();
    }
    case PrimitiveKnownToFolding::Kind::Number:
        return isTrackedInteger(known.asNumber()) ? layoutOfTagInFamily(walker, family, LayoutTag(static_cast<int64_t>(known.asNumber()))) : family.maker;
    case PrimitiveKnownToFolding::Kind::Text:
        return layoutOfTextInFamily(walker, family, known.asText());
    case PrimitiveKnownToFolding::Kind::Undefined:
    case PrimitiveKnownToFolding::Kind::Null:
    case PrimitiveKnownToFolding::Kind::Boolean:
        return family.maker;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Only the tags that the proven type of the value leaves.
static void forEachTagThatTypeLeaves(const Walker& walker, const TaggedFamily& family, TypeID typeOfTag, NOESCAPE const Invocable<IterationStatus(const LayoutTag&, LayoutID)> auto& functor)
{
    TypingContext typing = walker.typing();
    for (const TaggedLayout& member : family.members) {
        if (!hasObjectsOfItsOwnForTaggedMakers(walker, member.layout))
            continue;
        for (const LayoutTag& tag : member.tags) {
            TypeID narrowed = WTF::switchOn(tag,
                [&](int64_t integer) {
                    return narrowByIntegerCase(typing, typeOfTag, integer);
                },
                [&](StringID string) {
                    return narrowByStringCase(typing, typeOfTag, string);
                });
            if (!isNeverType(narrowed) && functor(tag, member.layout) == IterationStatus::Done)
                return;
        }
    }
}

static bool tagsCoverType(const Walker& walker, const TagsOfFamily& tags, TypeID typeOfTag)
{
    TypeArena& types = walker.types();
    return isNeverType(narrowByOtherwise(types, narrowByOtherwise(types, typeOfTag, tags.integers.span()), tags.strings.span()));
}

// The switch over the tags is as long as the family, so it stands once, in a unit of the maker, and not at each `new`. It builds nothing.
static bool tagSelectsSeveralLayouts(const Walker& walker, const TaggedFamily& family, Reference tag)
{
    TypeID typeOfTag = walker.typeOf(tag);
    LayoutID onlyLayout;
    bool selectsSeveral = false;
    TagsOfFamily tags;
    forEachTagThatTypeLeaves(walker, family, typeOfTag, [&](const LayoutTag& left, LayoutID layout) {
        selectsSeveral = onlyLayout && onlyLayout != layout;
        if (selectsSeveral)
            return IterationStatus::Done;
        onlyLayout = layout;
        tags.append(left);
        return IterationStatus::Continue;
    });
    return selectsSeveral || (onlyLayout && !tagsCoverType(walker, tags, typeOfTag));
}

// It builds nothing.
static LayoutsForTag collectLayoutsForTag(const Walker& walker, const TaggedFamily& family, Reference tag, SwitchOnTextsOfTags switchOnTexts)
{
    LayoutsForTag layouts;
    layouts.defaultLayout = layoutOfKnownTagInFamily(walker, family, tag);
    if (layouts.defaultLayout)
        return layouts;

    TypeID typeOfTag = walker.typeOf(tag);
    TagsOfFamily tags;
    forEachTagThatTypeLeaves(walker, family, typeOfTag, [&](const LayoutTag& left, LayoutID layout) {
        bool isText = std::holds_alternative<StringID>(left);
        if (isText && switchOnTexts == SwitchOnTextsOfTags::IsNotForeseen)
            return IterationStatus::Continue;
        tags.append(left);
        if (isText)
            layouts.stringCases.append({ std::get<StringID>(left), layout });
        else
            layouts.integerCases.append({ std::get<int64_t>(left), layout });
        return IterationStatus::Continue;
    });

    // Where the cases cover what the value can be, the last of them needs no test and nothing gets the maker's own layout.
    layouts.defaultLayout = family.maker;
    if (tagsCoverType(walker, tags, typeOfTag)) {
        if (!layouts.stringCases.isEmpty())
            layouts.defaultLayout = layouts.stringCases.takeLast().layout;
        else if (!layouts.integerCases.isEmpty())
            layouts.defaultLayout = layouts.integerCases.takeLast().layout;
    }

    auto hasDefaultLayout = [&](const auto& caseOfTag) {
        return caseOfTag.layout == layouts.defaultLayout;
    };
    layouts.integerCases.removeAllMatching(hasDefaultLayout);
    layouts.stringCases.removeAllMatching(hasDefaultLayout);
    return layouts;
}

static Reference buildAllocationForTaggedMakers(Walker& walker, LayoutID layout, Reference prototype, Origin origin)
{
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(layout);
    if (prototype)
        allocation.setPrototypeOperand(prototype);
    walker.chooseConservativeFields(allocation);
    return walker.build(WTF::move(allocation));
}

// No integer is a text, so the texts are asked for on the edge that no integer takes. The tags of one layout share one block, and all blocks meet.
static Reference buildObjectByTag(Walker& walker, LayoutsForTag& layouts, Reference tag, Reference prototype, Origin origin)
{
    if (layouts.isOneLayout())
        return buildAllocationForTaggedMakers(walker, layouts.defaultLayout, prototype, origin);

    std::ranges::sort(layouts.integerCases, { }, &CaseOfTagInFamily<int64_t>::tag);
    std::ranges::sort(layouts.stringCases, { }, &CaseOfTagInFamily<StringID>::tag);

    Vector<LayoutID, 32> layoutsOfCases;
    for (const CaseOfTagInFamily<int64_t>& caseOfTag : layouts.integerCases)
        layoutsOfCases.append(caseOfTag.layout);
    for (const CaseOfTagInFamily<StringID>& caseOfTag : layouts.stringCases)
        layoutsOfCases.append(caseOfTag.layout);
    std::ranges::sort(layoutsOfCases);
    layoutsOfCases.shrink(layoutsOfCases.size() - std::ranges::unique(layoutsOfCases).size());
    Vector<BlockID, 32> blocksOfLayouts(layoutsOfCases.size(), [&](size_t) {
        return walker.addBlock();
    });
    auto blockOf = [&](LayoutID layout) {
        return blocksOfLayouts[std::ranges::lower_bound(layoutsOfCases, layout) - layoutsOfCases.begin()];
    };

    Vector<int64_t, 32> integers;
    Vector<StringID, 32> texts;
    Vector<BlockID, 32> targetsOfIntegers;
    Vector<BlockID, 32> targetsOfTexts;
    for (const CaseOfTagInFamily<int64_t>& caseOfTag : layouts.integerCases) {
        integers.append(caseOfTag.tag);
        targetsOfIntegers.append(blockOf(caseOfTag.layout));
    }
    for (const CaseOfTagInFamily<StringID>& caseOfTag : layouts.stringCases) {
        texts.append(caseOfTag.tag);
        targetsOfTexts.append(blockOf(caseOfTag.layout));
    }

    BlockID otherTags = walker.addBlock();
    BlockID whereObjectsMeet = walker.addBlock();
    Reference object = walker.addParameter(whereObjectsMeet, { }, Representation::Cell);
    if (integers.isEmpty())
        walker.switchOnTexts(tag, texts.span(), targetsOfTexts.span(), otherTags);
    else if (texts.isEmpty())
        walker.switchOn(tag, integers.span(), targetsOfIntegers.span(), otherTags);
    else {
        BlockID noInteger = walker.addBlock();
        walker.switchOn(tag, integers.span(), targetsOfIntegers.span(), noInteger);
        walker.continueIn(noInteger);
        walker.switchOnTexts(tag, texts.span(), targetsOfTexts.span(), otherTags);
    }
    if (walker.hasFailed())
        return { };

    auto allocateIn = [&](BlockID block, LayoutID layout) {
        walker.continueIn(block);
        if (Reference made = buildAllocationForTaggedMakers(walker, layout, prototype, origin))
            walker.jumpTo(whereObjectsMeet, singleElementSpan(made));
    };
    for (size_t index = 0; index < layoutsOfCases.size(); ++index)
        allocateIn(blocksOfLayouts[index], layoutsOfCases[index]);
    allocateIn(otherTags, layouts.defaultLayout);
    walker.continueIn(whereObjectsMeet);
    return walker.isUnreachable() ? Reference() : object;
}

// Nothing: whoever makes `this` for the constructor has no layout to choose.
std::optional<uint32_t> positionOfTagAmongArguments(const Walker& walker, FunctionID constructor)
{
    const TaggedMaker* maker = findMakerForTaggedMakers(walker, constructor);
    if (!maker || maker->allocation)
        return std::nullopt;
    return maker->tagParameter - walker.program().at(constructor).firstParameterIndex();
}

TypeID typeOfObjectsMadeBy(const Walker& walker, FunctionID constructor)
{
    const Program& program = walker.program();
    LayoutID constructed = program.at(constructor).constructedClass;
    if (!constructed)
        return { };
    Vector<LayoutID, 16> layouts;
    if (walker.plan().isInUse(constructed))
        layouts.append(constructed);
    if (const TaggedMaker* maker = findMakerForTaggedMakers(walker, constructor); maker && program.taggedFamilies[maker->family].maker == constructed) {
        for (const TaggedLayout& member : program.taggedFamilies[maker->family].members) {
            if (hasObjectsOfItsOwnForTaggedMakers(walker, member.layout))
                layouts.append(member.layout);
        }
    }
    if (layouts.size() == 1)
        return walker.types().forLayout(layouts[0]);
    return layouts.isEmpty() ? TypeID() : walker.types().forLayouts(layouts.span());
}

TypeID typeOfObjectsMadeThroughEntryOf(const Walker& walker, FunctionID constructor)
{
    const Program& program = walker.program();
    LayoutID constructed = program.at(constructor).constructedClass;
    if (!constructed || !walker.plan().isInUse(constructed) || !walker.plan().conventions().exists(CodeUnit { constructor, CodeRole::AllocatingConstructEntry }))
        return { };
    // BuilderOfEntries::allocateThis() asks no more of a maker than that it is listed.
    if (const TaggedMaker* maker = findMakerForTaggedMakers(walker, constructor); maker && program.taggedFamilies[maker->family].maker != constructed)
        return { };
    TypeID returned = walker.facts().linked().function(constructor).result;
    if (!returned || walker.types().overlap(returned, TypeTable::object))
        return { };
    return typeOfObjectsMadeBy(walker, constructor);
}

bool chooseLayoutByTagAtConstruction(Walker& walker, const BodyValue& construction, FunctionID constructor, LayoutID& constructed)
{
    std::optional<uint32_t> position = positionOfTagAmongArguments(walker, constructor);
    if (!position || construction.opcode() != Opcode::Construct)
        return true;
    const TaggedFamily& family = walker.program().taggedFamilies[findMakerForTaggedMakers(walker, constructor)->family];
    if (family.maker != constructed)
        return true;
    // What is left out is undefined, which is the tag of no member.
    std::span<const ValueID> arguments = construction.as<Opcode::Construct>().argumentsOperands();
    Reference tag = *position < arguments.size() ? walker.referenceOf(arguments[*position]) : Reference();
    if (!tag)
        return true;

    if (LayoutID known = layoutOfKnownTagInFamily(walker, family, tag)) {
        constructed = known;
        return true;
    }
    if (tagSelectsSeveralLayouts(walker, family, tag))
        return false;
    LayoutsForTag layouts = collectLayoutsForTag(walker, family, tag, SwitchOnTextsOfTags::IsForeseen);
    if (!layouts.isOneLayout())
        return false;
    constructed = layouts.defaultLayout;
    return true;
}

Reference buildThisByTag(Walker& walker, FunctionID constructor, Reference prototype, Reference tag, Origin origin)
{
    if (!positionOfTagAmongArguments(walker, constructor) || !tag || !tag.isRecord())
        return { };
    const TaggedFamily& family = walker.program().taggedFamilies[findMakerForTaggedMakers(walker, constructor)->family];
    LayoutsForTag layouts = collectLayoutsForTag(walker, family, tag, SwitchOnTextsOfTags::IsForeseen);
    return buildObjectByTag(walker, layouts, tag, prototype, origin);
}

// `class Node { constructor(kind) { this.kind = kind } }`: the constructor allocates its own object. Under the prototype object of a class below the maker the run time answers with an instance of that class, whatever the allocation names,
// so the tag is asked only where new.target is the class itself. A class that is evaluated once has one prototype object, which an allocation without the operand names.
bool buildObjectOfTaggedClassConstructor(Walker& walker, const BodyValue& allocation)
{
    if (allocation.opcode() != Opcode::NewObject)
        return false;
    const TaggedMaker* maker = findMakerForTaggedMakers(walker, walker.currentFunction());
    if (!maker || maker->allocation != allocation.id())
        return false;
    const Program& program = walker.program();
    const TaggedFamily& family = program.taggedFamilies[maker->family];
    if (!walker.plan().hasOneInstance() || !walker.facts().isEvaluatedAtMostOnce(family.maker) || !walker.plan().hasClassRecord(family.maker))
        return false;

    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    Reference newTarget = walker.referenceOf(entry.parameter(*program.at(maker->constructor).newTargetParameterIndex()));
    Reference tag = walker.referenceOf(entry.parameter(maker->tagParameter));
    if (!newTarget || !tag || !tag.isRecord())
        return false;
    LayoutsForTag layouts = collectLayoutsForTag(walker, family, tag, SwitchOnTextsOfTags::IsNotForeseen);
    if (layouts.isOneLayout() && layouts.defaultLayout == family.maker)
        return false;
    // The switch stands once, in the unit of the maker.
    if (!layouts.isOneLayout() && walker.depthOfCopies())
        return false;

    Origin origin = walker.originOf(allocation);
    Type typeOfNewTarget = walker.types().at(walker.typeOf(newTarget));
    if (typeOfNewTarget.isObject() && typeOfNewTarget.isJavaScriptValue() && singleFunctionOf(walker.types(), typeOfNewTarget) == maker->constructor) {
        if (Reference object = buildObjectByTag(walker, layouts, tag, { }, origin))
            walker.bind(allocation.id(), object);
        return true;
    }

    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !newTarget.isValue() || !walker.facts().linked().mayBeMentionedBy(walker.unit().function, maker->constructor))
        return false;
    Draft written = walker.draftOf(allocation);
    if (walker.hasFailed())
        return true;
    Draft test(Opcode::IsClosureOf, origin);
    walker.chooseConservativeFields(test);
    test.setFunction(maker->constructor);
    test.setValueOperand(newTarget);
    Reference isOwnNewTarget = walker.build(WTF::move(test));
    if (!isOwnNewTarget)
        return true;

    BlockID underOwnNewTarget = walker.addBlock();
    BlockID underOtherNewTarget = walker.addBlock(BlockFrequency::Unlikely);
    BlockID behind = walker.addBlock();
    Reference object = walker.addParameter(behind, { }, Representation::Cell);
    walker.branch(isOwnNewTarget, underOwnNewTarget, underOtherNewTarget);
    walker.noteGuessOpened();

    walker.continueIn(underOtherNewTarget);
    walker.chooseConservativeFields(written);
    if (Reference asWritten = walker.build(WTF::move(written)))
        walker.jumpTo(behind, singleElementSpan(asWritten));

    walker.continueIn(underOwnNewTarget);
    if (Reference byTag = buildObjectByTag(walker, layouts, tag, { }, origin))
        walker.jumpTo(behind, singleElementSpan(byTag));

    walker.continueIn(behind);
    if (!walker.isUnreachable())
        walker.bind(allocation.id(), object);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
