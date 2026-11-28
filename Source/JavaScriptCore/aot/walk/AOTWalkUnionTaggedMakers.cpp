/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

struct CaseOfTagInFamily {
    int64_t tag { 0 };
    LayoutID layout;
};

struct LayoutsForTag {
    Vector<CaseOfTagInFamily, 16> integerCases;
    LayoutID defaultLayout;

    bool isOneLayout() const { return integerCases.isEmpty(); }
};

} // anonymous namespace

static const TaggedMaker* findMakerForTaggedMakers(const Walker& walker, FunctionID constructor)
{
    return findTaggedMaker(walker.facts().linked().taggedMakers.span(), constructor);
}

static bool hasObjectsOfItsOwnForTaggedMakers(const Walker& walker, LayoutID member)
{
    return walker.plan().isInUse(member) && walker.plan().slots().holdsCompiledObjects(member);
}

static LayoutID layoutOfTagInFamily(const Walker& walker, const TaggedFamily& family, const LayoutTag& tag)
{
    LayoutID member = layoutMadeWithTag(family, tag);
    return member && hasObjectsOfItsOwnForTaggedMakers(walker, member) ? member : family.maker;
}

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
    case PrimitiveKnownToFolding::Kind::Undefined:
    case PrimitiveKnownToFolding::Kind::Null:
    case PrimitiveKnownToFolding::Kind::Boolean:
        return family.maker;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachTagThatTypeLeaves(const Walker& walker, const TaggedFamily& family, TypeID typeOfTag, NOESCAPE const Invocable<IterationStatus(const LayoutTag&, LayoutID)> auto& functor)
{
    TypingContext typing = walker.typing();
    for (const TaggedLayout& member : family.members) {
        if (!hasObjectsOfItsOwnForTaggedMakers(walker, member.layout))
            continue;
        for (const LayoutTag& tag : member.tags) {
            TypeID narrowed = narrowByIntegerCase(typing, typeOfTag, tag);
            if (!isNeverType(narrowed) && functor(tag, member.layout) == IterationStatus::Done)
                return;
        }
    }
}

static bool tagsCoverType(const Walker& walker, std::span<const LayoutTag> tags, TypeID typeOfTag)
{
    return isNeverType(narrowByOtherwise(walker.types(), typeOfTag, tags));
}

static bool tagSelectsSeveralLayouts(const Walker& walker, const TaggedFamily& family, Reference tag)
{
    TypeID typeOfTag = walker.typeOf(tag);
    LayoutID onlyLayout;
    bool selectsSeveral = false;
    Vector<LayoutTag, 16> tags;
    forEachTagThatTypeLeaves(walker, family, typeOfTag, [&](const LayoutTag& left, LayoutID layout) {
        selectsSeveral = onlyLayout && onlyLayout != layout;
        if (selectsSeveral)
            return IterationStatus::Done;
        onlyLayout = layout;
        tags.append(left);
        return IterationStatus::Continue;
    });
    return selectsSeveral || (onlyLayout && !tagsCoverType(walker, tags.span(), typeOfTag));
}

static LayoutsForTag collectLayoutsForTag(const Walker& walker, const TaggedFamily& family, Reference tag)
{
    LayoutsForTag layouts;
    layouts.defaultLayout = layoutOfKnownTagInFamily(walker, family, tag);
    if (layouts.defaultLayout)
        return layouts;

    TypeID typeOfTag = walker.typeOf(tag);
    Vector<LayoutTag, 16> tags;
    forEachTagThatTypeLeaves(walker, family, typeOfTag, [&](const LayoutTag& left, LayoutID layout) {
        tags.append(left);
        layouts.integerCases.append({ left, layout });
        return IterationStatus::Continue;
    });

    layouts.defaultLayout = family.maker;
    if (tagsCoverType(walker, tags.span(), typeOfTag) && !layouts.integerCases.isEmpty())
        layouts.defaultLayout = layouts.integerCases.takeLast().layout;

    layouts.integerCases.removeAllMatching([&](const CaseOfTagInFamily& caseOfTag) {
        return caseOfTag.layout == layouts.defaultLayout;
    });
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

static Reference buildObjectByTag(Walker& walker, LayoutsForTag& layouts, Reference tag, Reference prototype, Origin origin)
{
    if (layouts.isOneLayout())
        return buildAllocationForTaggedMakers(walker, layouts.defaultLayout, prototype, origin);

    std::ranges::sort(layouts.integerCases, { }, &CaseOfTagInFamily::tag);

    Vector<LayoutID, 32> layoutsOfCases;
    for (const CaseOfTagInFamily& caseOfTag : layouts.integerCases)
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
    Vector<BlockID, 32> targetsOfIntegers;
    for (const CaseOfTagInFamily& caseOfTag : layouts.integerCases) {
        integers.append(caseOfTag.tag);
        targetsOfIntegers.append(blockOf(caseOfTag.layout));
    }

    BlockID otherTags = walker.addBlock();
    BlockID whereObjectsMeet = walker.addBlock();
    Reference object = walker.addParameter(whereObjectsMeet, { }, Representation::Cell);
    walker.switchOn(tag, integers.span(), targetsOfIntegers.span(), otherTags);
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

std::optional<uint32_t> positionOfTagAmongArguments(const Walker& walker, FunctionID constructor)
{
    const TaggedMaker* maker = findMakerForTaggedMakers(walker, constructor);
    if (!maker)
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
    if (const TaggedMaker* maker = findMakerForTaggedMakers(walker, constructor); maker && program.taggedFamilies[maker->family].maker != constructed)
        return { };
    TypeID returned = walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), constructor);
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
    LayoutsForTag layouts = collectLayoutsForTag(walker, family, tag);
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
    LayoutsForTag layouts = collectLayoutsForTag(walker, family, tag);
    return buildObjectByTag(walker, layouts, tag, prototype, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
