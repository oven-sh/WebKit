/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTSlotRepresentation.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class FieldInOwnKeyPlan : uint8_t {
    Unknown,
    LeftOut,
    Listed,
    Accessor,
};

} // anonymous namespace

bool orderWordTellsOwnKeysOf(const Walker& walker, LayoutID layout)
{
    const Plan& plan = walker.plan();
    return plan.layout(layout).holdsCompiledObjects() && plan.shapes().shapesOf(layout).count == 1;
}

static FieldInOwnKeyPlan classifyFieldInCopiesOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const Program& program = walker.program();
    const Layout& record = program.at(layout);
    FieldFacts fieldFacts = walker.facts().field({ layout, field });
    PropertyKeyID key;
    OptionSet<FieldForm> form = fieldFacts.formOfValue;
    if (field < record.fields.size()) {
        const Field& declared = record.fields[field];
        if (declared.computedKey)
            return FieldInOwnKeyPlan::Unknown;
        if (declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden }))
            return FieldInOwnKeyPlan::LeftOut;
        key = declared.key;
        form.add(declared.form);
    } else
        key = walker.plan().slots().keyOf(program, { layout, field });
    if (!key)
        return FieldInOwnKeyPlan::Unknown;

    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        break;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return FieldInOwnKeyPlan::LeftOut;
    }
    if (form.contains(FieldForm::IsNotEnumerable))
        return FieldInOwnKeyPlan::LeftOut;
    if (form.contains(FieldForm::IsAccessor) || fieldFacts.facts.contains(FieldFact::IsAccessor) || walker.plan().slot(layout, field).holdsAccessor)
        return isAccessorAmongOwnKeys(walker, layout, field) ? FieldInOwnKeyPlan::Accessor : FieldInOwnKeyPlan::Unknown;
    return FieldInOwnKeyPlan::Listed;
}

bool isAccessorAmongOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const PlacedSlot& slot = walker.plan().slot(layout, field);
    if (!slot.holdsAccessor || !slot.hasStorage() || slot.isOutOfLine() || !walker.has(Mechanism::DirectCalls))
        return false;
    const LinkedAccessorsOfField* expected = walker.facts().linked().findAccessorsExpectedIn({ layout, field });
    return expected && expected->getter;
}

bool staysOwnKeyAfterCalls(const Walker& walker, LayoutID layout, FieldIndex field)
{
    OptionSet<FieldStability> stability = walker.facts().field({ layout, field }).stability;
    if (!stability.contains(FieldStability::IsNeverDeleted))
        return false;
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn({ layout, field });
    return stability.contains(FieldStability::IsNeverRedefined) || (laid && laid->getterIsProven && !laid->getterMayNotBeLaidYet);
}

namespace {

struct KeptByCopiesOwnKeys final : Walker::KeptForBuild {
    struct ArmsOfLayout {
        Vector<OwnKeyOrder, 2> arms;
        bool everyExpectedOrderHasArm { false };
    };
    HashMap<uint64_t, ArmsOfLayout> armsOfLayouts;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whetherOwnKeysCanBePlanned;
};

} // anonymous namespace

static KeptByCopiesOwnKeys& keptByCopiesOwnKeys(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByCopiesOwnKeys();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByCopiesOwnKeys>();
    return static_cast<KeptByCopiesOwnKeys&>(*kept);
}

static bool canPlanOwnKeysOfLayoutInCopiesOwnKeys(Walker& walker, LayoutID layout)
{
    return keptByCopiesOwnKeys(walker).whetherOwnKeysCanBePlanned.ensure(layout.index(), [&] {
        const Program& program = walker.program();
        const Plan& plan = walker.plan();
        if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !orderWordTellsOwnKeysOf(walker, layout))
            return false;

        const PlannedLayout& planned = plan.layout(layout);
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            if (planned.slots[field].hasStorage() && !mayBeCreatedByStore(planned.slots[field].representation))
                return false;
            PropertyKeyID key = plan.slots().keyOf(program, { layout, field });
            if (key && program.at(key).kind == PropertyKeyKind::Text && (program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto))
                return false;
        }
        return true;
    }).iterator->value;
}

static std::optional<OwnKeyOrder> ownKeysAmongCreatedFieldsInCopiesOwnKeys(const Walker& walker, LayoutID layout, std::span<const FieldIndex> created, AccessorsAmongOwnKeys accessors, size_t mostFields = std::numeric_limits<size_t>::max())
{
    OwnKeyOrder order { layout, { }, { } };
    order.created.append(created);
    for (FieldIndex field : created) {
        if (order.listsAccessors && !staysOwnKeyAfterCalls(walker, layout, field))
            return std::nullopt;
        switch (classifyFieldInCopiesOwnKeys(walker, layout, field)) {
        case FieldInOwnKeyPlan::Unknown:
            return std::nullopt;
        case FieldInOwnKeyPlan::LeftOut:
            break;
        case FieldInOwnKeyPlan::Accessor:
            if (accessors == AccessorsAmongOwnKeys::AreRefused)
                return std::nullopt;
            order.listsAccessors = true;
            order.fields.append(field);
            if (order.fields.size() > mostFields)
                return std::nullopt;
            break;
        case FieldInOwnKeyPlan::Listed: {
            const PlacedSlot& slot = walker.plan().slot(layout, field);
            if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage))
                return std::nullopt;
            if (slot.representation == SlotRepresentation::DeferredClosure)
                return std::nullopt;
            order.fields.append(field);
            if (order.fields.size() > mostFields)
                return std::nullopt;
            break;
        }
        }
    }
    return order;
}

static LayoutID onlyLayoutProvenInCopiesOwnKeys(Walker& walker, TypeID proven)
{
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

static Reference nameWhoseOrderWordIsKnownInCopiesOwnKeys(Walker& walker, Reference object)
{
    if (walker.state().orderWordOf(object))
        return object;
    ValueID cell = underlyingCellOf(walker.code(), object.id());
    if (walker.code().isConstant(cell))
        return { };
    Reference root = Reference::value(cell);
    return walker.state().orderWordOf(root) ? root : Reference();
}

std::optional<OwnKeyOrder> ownKeysInSight(Walker& walker, Reference object, AccessorsAmongOwnKeys accessors)
{
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue())
        return std::nullopt;
    Reference known = nameWhoseOrderWordIsKnownInCopiesOwnKeys(walker, object);
    std::optional<Vector<FieldIndex, 8>> remaining = known ? std::nullopt : remainingFieldsOfFreshRecord(walker, object);
    LayoutID layout = known || remaining ? onlyLayoutProvenInCopiesOwnKeys(walker, walker.typeOf(object)) : LayoutID();
    if (!layout || !canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout))
        return std::nullopt;
    if (remaining) {
        walker.count(StatisticID::OwnKeysListedAfterDeletions);
        return ownKeysAmongCreatedFieldsInCopiesOwnKeys(walker, layout, remaining->span(), accessors);
    }

    uint32_t orderWord = *walker.state().orderWordOf(known);
    if (orderWord & ~orderAndCountMask || isOrderStoredOutOfLine(orderWord))
        return std::nullopt;
    const KeyOrders& orders = walker.plan().keyOrders();
    std::span<const FieldIndex> created;
    if (uint32_t count = countOf(orderWord)) {
        if (orderOf(orderWord) >= orders.numberOfOrdersOf(layout) || count > orders.order(layout, orderOf(orderWord)).size())
            return std::nullopt;
        created = orders.order(layout, orderOf(orderWord)).first(count);
    }
    return ownKeysAmongCreatedFieldsInCopiesOwnKeys(walker, layout, created, accessors);
}

static bool isRequiredInCopiesOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const Layout& record = walker.program().at(layout);
    if (field < record.fields.size())
        return record.fields[field].flags.contains(FieldFlags::IsRequired);
    return walker.facts().linked().isAppendedFieldRequiredBySomeWrittenType({ layout, field });
}

static Vector<uint32_t, 2> countsAtWhichMakersStopInCopiesOwnKeys(const Walker& walker, LayoutID layout, std::span<const FieldIndex> order)
{
    uint32_t behindLastRequiredField = 0;
    for (uint32_t place = 0; place < order.size(); ++place) {
        if (isRequiredInCopiesOwnKeys(walker, layout, order[place]))
            behindLastRequiredField = place + 1;
    }

    Vector<uint32_t, 2> counts;
    uint32_t whole = static_cast<uint32_t>(order.size());
    if (!walker.isInLoop()) {
        const PlannedLayout& planned = walker.plan().layout(layout);
        uint32_t numberOfFieldsThatMakersCreate = 0;
        bool everyFieldIsRequired = true;
        uint32_t numberOfDeclaredFields = static_cast<uint32_t>(walker.program().at(layout).fields.size());
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            if (!planned.slots[field].hasStorage())
                continue;
            bool isRequired = isRequiredInCopiesOwnKeys(walker, layout, field);
            if (!isRequired && field >= numberOfDeclaredFields)
                continue;
            ++numberOfFieldsThatMakersCreate;
            everyFieldIsRequired &= isRequired;
        }
        if (everyFieldIsRequired && behindLastRequiredField == numberOfFieldsThatMakersCreate)
            counts.append(behindLastRequiredField);
        return counts;
    }
    counts.append(whole);
    if (behindLastRequiredField && behindLastRequiredField != whole)
        counts.append(behindLastRequiredField);
    return counts;
}

static bool isMadeWithFields(Walker& walker, LayoutID layout)
{
    const KeyOrders& keyOrders = walker.plan().keyOrders();
    for (uint32_t number = 0; number < keyOrders.numberOfOrdersOf(layout); ++number) {
        if (std::ranges::any_of(keyOrders.whereMakersStop(layout, number), [](uint8_t stops) { return !!stops; }))
            return true;
    }
    return false;
}

bool isMadeWithKeys(Walker& walker, LayoutID layout, std::span<const FieldIndex> created)
{
    const KeyOrders& keyOrders = walker.plan().keyOrders();
    for (uint32_t number = 0; number < keyOrders.numberOfOrdersOf(layout); ++number) {
        std::span<const uint8_t> whereMakersStop = keyOrders.whereMakersStop(layout, number);
        if (!created.empty() && created.size() <= whereMakersStop.size() && whereMakersStop[created.size() - 1] && std::ranges::equal(keyOrders.order(layout, number).first(created.size()), created))
            return true;
    }
    return false;
}

bool listsAllRequiredFields(const Walker& walker, LayoutID layout, std::span<const FieldIndex> created)
{
    const PlannedLayout& planned = walker.plan().layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].hasStorage() && isRequiredInCopiesOwnKeys(walker, layout, field) && !std::ranges::contains(created, field))
            return false;
    }
    return true;
}

bool objectsOfLayoutComeInSeveralOrders(Walker& walker, LayoutID layout)
{
    const KeyOrders& keyOrders = walker.plan().keyOrders();
    uint32_t numberOfOrders = keyOrders.numberOfOrdersOf(layout);
    if (numberOfOrders < 2)
        return false;
    uint32_t numberOfCompleteOrders = 0;
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const uint8_t> whereMakersStop = keyOrders.whereMakersStop(layout, number);
        uint32_t count = static_cast<uint32_t>(whereMakersStop.size());
        while (count && !whereMakersStop[count - 1])
            --count;
        numberOfCompleteOrders += count && listsAllRequiredFields(walker, layout, keyOrders.order(layout, number).first(count));
    }
    return numberOfCompleteOrders > 1;
}

static bool findArmsOfLayoutInCopiesOwnKeys(Walker& walker, LayoutID layout, unsigned mostArms, size_t mostFields, AccessorsAmongOwnKeys accessors, OwnKeyPlan& plan)
{
    if (!canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout))
        return false;

    const KeyOrders& keyOrders = walker.plan().keyOrders();
    size_t firstArmOfLayout = plan.orders.size();
    bool hasList = false;
    bool everyListHasArm = true;
    auto addArmWhereThereIsRoom = [&](std::span<const FieldIndex> created) {
        bool hasArmAlready = std::ranges::any_of(plan.orders.span().subspan(firstArmOfLayout), [&](const OwnKeyOrder& earlier) {
            return std::ranges::equal(earlier.created, created);
        });
        if (hasArmAlready)
            return true;
        if (plan.orders.size() == mostArms)
            return false;
        hasList = true;
        std::optional<OwnKeyOrder> arm = keyOrders.findOrderWord(layout, created) ? ownKeysAmongCreatedFieldsInCopiesOwnKeys(walker, layout, created, accessors, mostFields) : std::nullopt;
        if (arm)
            plan.orders.append(WTF::move(*arm));
        else
            everyListHasArm = false;
        return true;
    };

    uint32_t numberOfOrders = keyOrders.numberOfOrdersOf(layout);
    if (walker.isInLoop()) {
        Vector<std::span<const FieldIndex>, 8> beginnings;
        Vector<uint32_t, 16> lastStopTakenOfOrders;
        for (bool someOrderHasStop = true; someOrderHasStop;) {
            someOrderHasStop = false;
            for (uint32_t number = 0; number < numberOfOrders; ++number) {
                std::span<const uint8_t> whereMakersStop = keyOrders.whereMakersStop(layout, number);
                if (lastStopTakenOfOrders.size() == number)
                    lastStopTakenOfOrders.append(static_cast<uint32_t>(whereMakersStop.size()) + 1);
                uint32_t count = lastStopTakenOfOrders[number] ? lastStopTakenOfOrders[number] - 1 : 0;
                while (count && !whereMakersStop[count - 1])
                    --count;
                lastStopTakenOfOrders[number] = count;
                someOrderHasStop |= !!count;
                if (count)
                    beginnings.append(keyOrders.order(layout, number).first(count));
            }
        }
        std::ranges::stable_partition(beginnings, [&](std::span<const FieldIndex> created) {
            return listsAllRequiredFields(walker, layout, created);
        });
        for (std::span<const FieldIndex> created : beginnings) {
            if (!addArmWhereThereIsRoom(created))
                return false;
        }
    }
    std::optional<bool> everyStopOfMakersHasArm = hasList ? std::optional<bool>(everyListHasArm) : std::nullopt;
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> order = keyOrders.order(layout, number);
        for (uint32_t count : countsAtWhichMakersStopInCopiesOwnKeys(walker, layout, order)) {
            if (!addArmWhereThereIsRoom(order.first(count)))
                return everyStopOfMakersHasArm.value_or(false);
        }
    }
    return everyStopOfMakersHasArm.value_or(hasList && everyListHasArm);
}

static bool appendOrdersOfLayoutInCopiesOwnKeys(Walker& walker, LayoutID layout, unsigned mostArms, size_t mostFields, AccessorsAmongOwnKeys accessors, OwnKeyPlan& plan)
{
    static_assert(std::max(mostArmsOfOwnKeyWalk, maxArmsOfCopiedSource) < 1 << 7);
    RELEASE_ASSERT(plan.orders.size() <= mostArms && mostArms <= std::max(mostArmsOfOwnKeyWalk, maxArmsOfCopiedSource));
    unsigned room = mostArms - static_cast<unsigned>(plan.orders.size());
    mostFields = std::min<size_t>(mostFields, walker.plan().layout(layout).numberOfFields());
    if (mostFields > std::numeric_limits<uint16_t>::max())
        return findArmsOfLayoutInCopiesOwnKeys(walker, layout, mostArms, mostFields, accessors, plan);
    uint64_t key = (static_cast<uint64_t>(layout.index()) + 1) << 32 | static_cast<uint64_t>(accessors) << 24 | static_cast<uint64_t>(mostFields) << 8 | room << 1 | walker.isInLoop();
    const KeptByCopiesOwnKeys::ArmsOfLayout& found = keptByCopiesOwnKeys(walker).armsOfLayouts.ensure(key, [&] {
        OwnKeyPlan ofLayoutAlone;
        bool everyExpectedOrderHasArm = findArmsOfLayoutInCopiesOwnKeys(walker, layout, room, mostFields, accessors, ofLayoutAlone);
        return KeptByCopiesOwnKeys::ArmsOfLayout { WTF::move(ofLayoutAlone.orders), everyExpectedOrderHasArm };
    }).iterator->value;
    plan.orders.appendVector(found.arms);
    return found.everyExpectedOrderHasArm;
}

static LayoutID layoutWrittenOfSourceInCopiesOwnKeys(Walker& walker, ValueID source)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    TypeID written;
    for (ValueID definition = source; definition && !written;) {
        if (index.isParameter(definition)) {
            const Function& function = program.at(walker.currentFunction());
            if (const DeclaredType* parameter = index.blockOf(definition) == entryBlock && definition.index() < function.numberOfEntryParameters() ? function.declaredEntryParameter(definition.index()) : nullptr)
                written = parameter->written();
            break;
        }
        BodyValue operation = index.value(definition);
        switch (operation.opcode()) {
        case Opcode::Convert:
            written = operation.as<Opcode::Convert>().declared().written();
            definition = operation.operand(0);
            break;
        case Opcode::CheckInitialized:
            definition = operation.operand(0);
            break;
        case Opcode::GetModuleVariable:
            written = program.at(operation.as<Opcode::GetModuleVariable>().variable()).declared.written();
            definition = { };
            break;
        default:
            definition = { };
            break;
        }
    }
    return layoutThatNamesFamily(walker, written);
}

static bool listLayoutsInCopiesOwnKeys(Walker& walker, TypeID listed, ValueID source, unsigned mostArms, size_t mostFields, AccessorsAmongOwnKeys accessors, Vector<LayoutID, mostArmsOfOwnKeyWalk>& layouts, bool& hasMoreLayouts)
{
    if (!listed)
        return false;
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return false;
    LayoutID written = holdsOneMember(type.layouts) ? LayoutID() : layoutWrittenOfSourceInCopiesOwnKeys(walker, source);
    if (written && (!walker.plan().isInUse(written) || !walker.types().layoutSets().contains(type.layouts, written)))
        written = { };
    if (OwnKeyPlan ofWritten; written) {
        appendOrdersOfLayoutInCopiesOwnKeys(walker, written, mostArms, mostFields, accessors, ofWritten);
        if (ofWritten.orders.isEmpty())
            written = { };
    }
    if (written)
        layouts.append(written);
    for (LayoutID member : membersOf(walker.types(), type.layouts)) {
        if (member == written || !walker.plan().isInUse(member))
            continue;
        if (layouts.size() == mostArms) {
            hasMoreLayouts = true;
            break;
        }
        layouts.append(member);
    }
    return !layouts.isEmpty();
}

OwnKeyPlan planOwnKeyWalk(Walker& walker, ValueID source, Reference built, size_t mostFields, unsigned maxArmsInLoop, AccessorsAmongOwnKeys accessors)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !built || !built.isValue())
        return { };

    TypeArena& types = walker.types();
    unsigned mostArms = walker.isInLoop() ? maxArmsInLoop : 1;
    TypeID proven = walker.typeOf(built);
    Vector<LayoutID, mostArmsOfOwnKeyWalk> layouts;
    bool hasMoreLayouts = false;

    OwnKeyPlan plan;
    plan.isDeclared = true;
    bool layoutsAreListedByProof = listLayoutsInCopiesOwnKeys(walker, proven, source, mostArms, mostFields, accessors, layouts, hasMoreLayouts);
    if (!layoutsAreListedByProof) {
        ExpectationOfValue expectation = findExpectation(walker, source);
        TypeID expected = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
        if (!expected || !proven || !types.overlap(proven, expected) || !listLayoutsInCopiesOwnKeys(walker, expected, source, mostArms, mostFields, accessors, layouts, hasMoreLayouts))
            return { };
        plan.isDeclared = expectation.isDeclared;
    }
    plan.layoutsAreProven = layoutsAreListedByProof && layouts.size() == 1 && !hasMoreLayouts;

    plan.hasArmForEveryExpectedOrder = !hasMoreLayouts;
    for (LayoutID layout : layouts) {
        bool hasEveryArm = appendOrdersOfLayoutInCopiesOwnKeys(walker, layout, mostArms, mostFields, accessors, plan);
        plan.hasArmForEveryExpectedOrder &= hasEveryArm;
        bool requiresNoField = walker.isInLoop() && canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout) && listsAllRequiredFields(walker, layout, { });
        plan.coversMakers &= hasEveryArm || (requiresNoField && !isMadeWithFields(walker, layout));
        if (requiresNoField)
            plan.layoutsWithoutRequiredFields.append(layout);
    }
    plan.coversMakers &= !hasMoreLayouts;
    if (plan.orders.isEmpty())
        return { };
    return plan;
}

static bool continueIfKeysMatch(Walker& walker, Reference test, BlockID whereTestFails)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    walker.branch(test, passed, whereTestFails);
    walker.continueIn(passed);
    return !walker.isUnreachable() && !walker.hasFailed();
}

bool continueIfOwnKeys(Walker& walker, Reference& object, const OwnKeyOrder& order, bool layoutIsProven, Origin origin, const WaysOutOfOwnKeyTests& waysOut)
{
    origin.isTailCall = false;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(walker.types().at(walker.types().forLayout(order.layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        if (!continueIfKeysMatch(walker, hasShape, waysOut.whereLayoutDiffers))
            return false;
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(object);
        narrowed.setTestOperand(hasShape);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(true);
        object = walker.build(WTF::move(narrowed));
        if (!object)
            return false;
    }

    Draft hasOrder(Opcode::HasKeyOrder, origin);
    hasOrder.setObjectOperand(object);
    hasOrder.setLayout(order.layout);
    hasOrder.setFields(order.created.span());
    if (!continueIfKeysMatch(walker, walker.build(WTF::move(hasOrder)), waysOut.whereOrderDiffers))
        return false;

    BlockID whereGuessFails = waysOut.whereTestFails;
    walker.letGuessFailUnasked(whereGuessFails, origin);
    Draft hasNothingElse(Opcode::HasNoExtraProperties, origin);
    hasNothingElse.setObjectOperand(object);
    hasNothingElse.setHasNoFieldOutsideSlot(true);
    if (!continueIfKeysMatch(walker, walker.build(WTF::move(hasNothingElse)), waysOut.whereTestFails))
        return false;
    walker.state().recordCellFacts(object, CellFact::NoFieldOutsideSlot);
    walker.checkFactsRememberedOfCell(object, origin);
    return !walker.isUnreachable();
}

bool continueIfOwnKeys(Walker& walker, Reference& object, const OwnKeyOrder& order, bool layoutIsProven, Origin origin, BlockID whereTestFails)
{
    return continueIfOwnKeys(walker, object, order, layoutIsProven, origin, WaysOutOfOwnKeyTests { whereTestFails, whereTestFails, whereTestFails });
}

Reference valueOfCopiedKey(Walker& walker, const BodyValue& copy, Reference object, LayoutID layout, FieldIndex field, Origin origin)
{
    if (!walker.plan().slot(layout, field).holdsAccessor)
        return valueOfOwnKey(walker, object, layout, field, origin);
    TypeArena& types = walker.types();
    const LinkTables& linked = walker.facts().linked();
    const LinkedAccessorsOfField* laid = linked.findAccessorsExpectedIn({ layout, field });
    RELEASE_ASSERT(laid && laid->getter);
    origin.isTailCall = false;
    Draft load(Opcode::GetSlotGetter, origin);
    load.setObjectOperand(object);
    load.setKey(walker.plan().slots().keyOf(walker.program(), { layout, field }));
    load.setReceivers(types.at(types.forLayout(layout)).layouts);
    load.setLevel(KnowledgeLevel::Proven);
    if (!laid->getterIsProven)
        load.representation = Representation::JSValue;
    Reference getter = walker.build(WTF::move(load));
    if (!getter || walker.isUnreachable() || walker.hasFailed())
        return { };
    bool isKnownFunction = linked.mayBeMentionedBy(walker.unit(), laid->getter) && types.isSubset(walker.typeOf(getter), types.forFunction(laid->getter));
    bool isReturnedAtOnce = copy.opcode() == Opcode::Call && walker.draftOf(copy).resultIsReturnedAtOnce;
    Reference boundBefore = walker.referenceBoundTo(copy.id());
    if (isKnownFunction && !isReturnedAtOnce && callMethodOfProtocol(walker, copy, laid->getter, object, getter)) {
        if (walker.isUnreachable() || walker.hasFailed())
            return { };
        Reference value = walker.referenceBoundTo(copy.id());
        walker.bind(copy.id(), boundBefore);
        return value;
    }
    Draft call(Opcode::Call, origin);
    call.setCalleeOperand(getter);
    call.setThisValueOperand(object);
    return walker.build(WTF::move(call));
}

Reference valueOfOwnKey(Walker& walker, Reference object, LayoutID layout, FieldIndex field, Origin origin)
{
    const Program& program = walker.program();
    PropertyKeyID key = walker.plan().slots().keyOf(program, { layout, field });
    FieldReference place = findField(program, layout, key).value_or(FieldReference { layout, field });

    if (walker.has(Mechanism::Folding)) {
        Reference names[] { object, object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : Reference() };
        for (Reference name : names) {
            for (FieldReference known : { place, FieldReference { layout, field } }) {
                Reference held = name ? walker.state().slot(name, known) : Reference();
                TypeID type = held ? walker.typeOf(held) : TypeID();
                if (type && !(walker.types().at(type).primitives & Primitive::Absent) && valueFitsSlot(walker, { layout, field }, held))
                    return held;
            }
        }
    }

    origin.isTailCall = false;
    Draft read(Opcode::GetSlot, origin);
    read.setObjectOperand(object);
    read.setKey(key);
    read.setReceivers(walker.types().at(walker.types().forLayout(layout)).layouts);
    read.setLevel(KnowledgeLevel::Proven);
    Reference value = walker.build(WTF::move(read));
    if (value)
        walker.state().noteSlot(object, place, value);
    return value;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
