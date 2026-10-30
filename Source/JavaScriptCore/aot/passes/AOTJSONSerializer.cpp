/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerBuilder.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBlockFrequencies.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTEffectsDescription.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowResults.h"
#include "AOTKnownValue.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTStringBuilderCursor.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <climits>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesWithJSONText = Primitive::String | Primitive::Number | Primitive::Boolean | Primitive::Null;
static constexpr uint16_t primitivesWithoutJSONText = Primitive::Undefined | Primitive::Absent | Primitive::AnySymbol;
static constexpr uint32_t cursorResultOfJSONAppendValue = 1;
static constexpr ProofSet proofsTestedBySerializers { Proof::NoExtraProperties, Proof::PrototypeLinkUnchanged };
static constexpr OptionSet<PristineAspects> serializedArrayAspects { PristineAspects::Storage, PristineAspects::Names };

namespace {

struct JSONTextWriter final : Emission {
    enum class MemberSupport : uint8_t {
        NotSerialized,
        // The key is among those that are serialized, whatever the slot can hold has no text, and finding that out runs no code.
        WithoutText,
        ByName,
        Slot,
    };

    enum class ValueKind : uint8_t { Generic, Nullish, Scalar, Array, Objects, ObjectParts };

    struct ValuePlan {
        ValueKind kind { ValueKind::Generic };
        bool isExpected { false };
        Opcode row { Opcode::JSONAppendValue };
        TypeID tested;
        TypeID element;
        Vector<LayoutID, 4> layouts;
        // The plans of the kinds of object in a union, each behind its test. The layouts come last: what fails their tests is written by the generic code.
        Vector<ValuePlan> parts;
        bool mayBeNullish { false };
    };

    struct Cursor {
        ValueID value;
        uint32_t reservedBytes { 0 };
    };

    struct MergePoint {
        Expected<BlockFrequency> frequency { BlockFrequency::Normal };
        BlockID block;
        Cursor cursor { { }, UINT_MAX };
        // Set where each arm brings the guard that the code behind the merge relies on.
        ValueID guard;
    };

    struct TypeArm {
        KnownBoolean matches { KnownBoolean::Unknown };
        ValueID narrowed;
        ValueID remaining;
        BlockID otherwise;
    };

    // Where the next member starts, the text of the object is empty, ends in a member, or ends in "{" or ",".
    struct MemberListState {
        std::optional<JSONToken> separator;
        JSONToken closing;
    };
    static constexpr MemberListState notOpened { JSONToken::OpenObject, JSONToken::EmptyObject };
    static constexpr MemberListState afterMember { JSONToken::Comma, JSONToken::CloseObject };
    static constexpr MemberListState afterSeparator { std::nullopt, JSONToken::CloseObjectOverComma };

    struct Position {
        // A member that starts after a separator writes the separator of the next member.
        bool endsWithComma() const { return kind == JSONPosition::Property && !memberList.separator; }

        JSONPosition kind { JSONPosition::Root };
        PropertyKeyID key;
        ValueID index;
        MemberListState memberList { afterSeparator };
    };

    struct Member {
        PropertyKeyID key;
        uint32_t field { 0 };
        uint32_t positionInOrder { 0 };
        ValuePlan plan;
    };

    // The key orders that one arm writes: an order and the expected orders that are prefixes of it. An object with fewer keys has empty slots for the rest.
    struct KeyOrderFamily {
        uint32_t smallestCount() const { return descendingCounts.last(); }

        KeyOrderFields longestOrder;
        Vector<uint32_t, 4> descendingCounts;
    };

    struct TestedKeyOrder {
        ValueID guard;
        ValueID numberOfOwnMembers;
    };

    // Holders that were listed one behind the other. What leaves the outermost of them leaves them all.
    struct ListedHolders {
        uint32_t number { 0 };
        ValueID outermost;
    };

    struct ObjectArm {
        TestedObject object;
        LayoutID layout;
        const KeyOrderFamily& orders;
        // Set where the orders of the arm differ in it.
        ValueID numberOfOwnMembers;
        Vector<Member, 8> members;
        BlockID remainder;
        BlockID done;
        ListedHolders listedHolders;
    };

    // An object or an array that this function is writing. The builder keeps a list of those that are listed, which the host reads to find a cycle.
    struct Holder {
        ValueID value;
        // While the holder is listed, the list that begins with it.
        ValueID listed;
    };

    struct ArrayLoop {
        ValueID array;
        ValueID guard;
        ValueID length;
        TypeID elementType;
        MergePoint& closing;
    };

    // A class object inherits from the class that it extends, which may have a static method of the name.
    bool findsNoToJSON(FunctionID closureFunction) const
    {
        const Function& closure = program().at(closureFunction);
        PropertyKeyID toJSON = program().keyOf(BuiltinNames::toJSON);
        if (closure.constructedClass || !inputs().flow.areClosuresAsCreated(closureFunction, singleElementSpan(toJSON)))
            return false;
        MemberAnswer inherited = findBuiltinObjectMember({ program(), &inputs().flow }, prototypeOfFunctionObject(closure), toJSON, Access::Read);
        return inherited.kind == MemberKind::Absent && inherited.needsNoTest();
    }

    // Every value of the type is left out of an object and written as null in an array, and no code runs to find that out.
    // SerializeJSONProperty answers undefined for undefined, for a symbol, and for a function whose toJSON is not callable.
    bool hasNoJSONText(const Type& type) const
    {
        if (type.isNever() || !type.isJavaScriptValue() || (type.primitives & ~primitivesWithoutJSONText))
            return false;
        if (type.anyObject || type.anyFunction || type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty())
            return false;
        return std::ranges::all_of(type.functions, [&](FunctionID closureFunction) { return findsNoToJSON(closureFunction); });
    }

    MemberSupport memberSupportOf(LayoutID id, uint32_t fieldIndex) const
    {
        const Field& field = program().at(id).fields[fieldIndex];
        if (!isEnumerableStringField(program(), field))
            return MemberSupport::NotSerialized;
        // An accessor's slot holds its functions. The getter's result is what gets written.
        if (field.form.contains(FieldForm::IsAccessor))
            return MemberSupport::ByName;
        if (hasNoJSONText(slotContentsTypeOf(program(), { id, fieldIndex })))
            return MemberSupport::WithoutText;

        PropertyAccessQuery query { .key = field.key, .receivers = ReceiverCandidates::underGuard(singleElementSpan(id)) };
        PropertyAccessPlan plan = planPropertyAccess(PropertyAccessContext { program() }, query);
        bool readsSlot = plan.form == AccessForm::Slot && (plan.operation == Opcode::GetSlot || plan.operation == Opcode::GetSlotOrUndefined);
        return readsSlot ? MemberSupport::Slot : MemberSupport::ByName;
    }

    bool canHaveSerializer(LayoutID id) const
    {
        const Layout& layout = program().at(id);
        if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || layout.flow.allocation == AllocationState::NeverAllocated)
            return false;
        MemberQuery query { MemberStart::Instance, id, program().keyOf(BuiltinNames::toJSON), Access::Read, false };
        MemberAnswer toJSON = findMember(PropertyAccessContext { program(), &inputs().flow }, query);
        return toJSON.kind == MemberKind::Absent && toJSON.conditions.isEmpty() && (toJSON.receiverProofs | proofsTestedBySerializers) == proofsTestedBySerializers;
    }

    // How likely objects of a layout are the data that a program writes as JSON, the likeliest first.
    auto likelihoodOf(LayoutID id) const
    {
        const Layout& layout = program().at(id);
        uint32_t members = 0;
        uint32_t membersWithoutText = 0;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!isEnumerableStringField(program(), layout.fields[field]))
                continue;
            ++members;
            if (hasNoJSONText(slotContentsTypeOf(program(), { id, field })))
                ++membersWithoutText;
        }
        bool isOfLibrary = layout.module && program().at(layout.module).flags.contains(ModuleFlags::IsLibrary);
        uint32_t module = layout.module ? layout.module.index() + 1 : 0;
        return std::tuple { isOfLibrary, layout.kind == LayoutKind::Instance, membersWithoutText, members, module, layout.position, id };
    }

    // toJSON converts the date to a primitive and calls toISOString, each by name. A test run may replace any of them.
    bool findsMembersOfDateAsBuilt() const
    {
        const BuiltinTable& builtins = program().builtins();
        auto isAsBuilt = [&](BuiltinName name) {
            MemberAnswer found = findBuiltinObjectMember({ program(), &inputs().flow }, builtins.at(BuiltinClasses::Date).prototype, program().keyOf(name), Access::Read);
            return found.kind == MemberKind::Builtin && found.needsNoTest();
        };
        return isAsBuilt(BuiltinNames::toJSON) && isAsBuilt(BuiltinNames::toISOString) && std::ranges::all_of(builtins.keysReadBy(Protocol::ToPrimitive), isAsBuilt);
    }

    BuiltinOverloadID findGetTimeOfDate() const
    {
        BuiltinMemberLookup lookup = program().builtins().findForInstance(BuiltinClasses::Date, BuiltinNames::getTime);
        if (lookup.outcome != BuiltinMemberLookup::Outcome::Found)
            return { };

        TypingContext typing { program(), function(), inputs().types };
        BuiltinOperandTypes operands { typing.types.intern(Type::builtinClass(BuiltinClasses::Date)), { } };
        OverloadMatch match = matchOverload({ typing, lookup.member, BuiltinCallMode::Call, operands, operands });
        return match.state == BuiltinMatchState::Proven && match.requirements.isEmpty() ? match.overload : BuiltinOverloadID();
    }

    ValuePlan planValue(ValueID id) const
    {
        const Value& value = function().at(id);
        TypeID expected = value.expected().predicted();
        return planType(program().at(value.type), expected ? &program().at(expected) : nullptr);
    }

    ValuePlan planType(const Type& proven, const Type* expected) const
    {
        Type defined = proven.withoutNullish();
        bool mayBeNullish = proven != defined;
        if (defined.isNever())
            return { .kind = mayBeNullish ? ValueKind::Nullish : ValueKind::Generic, .mayBeNullish = mayBeNullish };

        ValuePlan plan = planDefinedType(defined);
        ValuePlan expectedPlan;
        if (expected && (plan.kind == ValueKind::Generic || plan.kind == ValueKind::Array))
            expectedPlan = planDefinedType(expected->withoutNullish());

        if (plan.kind == ValueKind::Array && expectedPlan.kind == ValueKind::Array) {
            TypeID element = inputs().types.intersectionOf(plan.element, expectedPlan.element);
            if (element != TypeTable::never)
                plan.element = element;
        }
        if (plan.kind == ValueKind::Generic && removePartsThatCannotPass(expectedPlan, defined)) {
            plan = WTF::move(expectedPlan);
            plan.isExpected = true;
        }
        plan.mayBeNullish = mayBeNullish;
        return plan;
    }

    bool removePartsThatCannotPass(ValuePlan& expected, const Type& defined) const
    {
        TypeArena& types = inputs().types;
        auto canNarrowTo = [&](const Type& tested) {
            Type narrowed = defined.intersectWith(tested, types);
            return !narrowed.isNever() && narrowed.isSubsetOf(tested, program());
        };
        if (expected.kind == ValueKind::Objects) {
            expected.layouts.removeAllMatching([&](LayoutID layout) { return !canNarrowTo(Type::forLayout(layout, types)); });
            return !expected.layouts.isEmpty();
        }
        if (expected.kind == ValueKind::ObjectParts) {
            expected.parts.removeAllMatching([&](ValuePlan& part) { return !removePartsThatCannotPass(part, defined); });
            return !expected.parts.isEmpty();
        }
        return expected.tested && canNarrowTo(types.at(expected.tested));
    }

    ValuePlan planDefinedType(const Type& defined) const
    {
        if (defined.isBoolean())
            return { .kind = ValueKind::Scalar, .row = Opcode::JSONAppendBoolean, .tested = TypeTable::boolean };
        if (defined.isInteger() && defined.integers.isSubsetOf(Range::exact()))
            return { .kind = ValueKind::Scalar, .row = Opcode::JSONAppendInteger, .tested = defined.isInt32() ? TypeTable::int32 : inputs().types.intern(Type::integer(Range::exact())) };
        if (defined.isNumber())
            return { .kind = ValueKind::Scalar, .row = Opcode::JSONAppendNumber, .tested = TypeTable::number };
        if (defined.isAnyString())
            return { .kind = ValueKind::Scalar, .row = Opcode::JSONAppendString, .tested = TypeTable::string };

        ValuePlan plan { .kind = ValueKind::ObjectParts };
        ValuePlan layouts;
        for (const UnionPart& part : unionPartsOf(defined.withOnlyObjects())) {
            if (part.kind == UnionPartKind::Layouts)
                layouts = planLayouts(part.type);
            else if (part.kind == UnionPartKind::BuiltinClass) {
                if (ValuePlan instances = planBuiltinClass(part.type); instances.kind != ValueKind::Generic)
                    plan.parts.append(WTF::move(instances));
            }
        }
        if (layouts.kind != ValueKind::Generic)
            plan.parts.append(WTF::move(layouts));
        if (plan.parts.size() == 1)
            return plan.parts.takeLast();
        return plan.parts.isEmpty() ? ValuePlan { } : WTF::move(plan);
    }

    ValuePlan planBuiltinClass(const Type& instances) const
    {
        TypeArena& types = inputs().types;
        const BuiltinTable& builtins = program().builtins();
        const ClassPart* part = instances.singleBuiltinClass();
        RELEASE_ASSERT(part);
        if (pristineTestOf(builtins, part->builtinClass) == PristineTest::NeverPasses)
            return { };
        if (part->builtinClass == BuiltinClasses::Date && findsMembersOfDateAsBuilt() && findGetTimeOfDate())
            return { .kind = ValueKind::Scalar, .row = Opcode::JSONAppendDate, .tested = types.intern(instances.withoutClassProofs()) };
        if (part->builtinClass == BuiltinClasses::Array)
            return { .kind = ValueKind::Array, .tested = types.intern(instances.withoutClassProofs()), .element = part->expectedContentsAt(builtins, ContentsSlot::Element).predicted() };
        return { };
    }

    ValuePlan planLayouts(const Type& objects) const
    {
        ValuePlan plan { .kind = ValueKind::Objects };
        for (LayoutID layout : membersOf(program(), objects)) {
            if (canHaveSerializer(layout))
                plan.layouts.append(layout);
        }
        // The order of the tests, and who gets one where there are too many.
        if (plan.layouts.size() > 1) {
            Vector<decltype(likelihoodOf(LayoutID())), 8> ranked(plan.layouts.size(), [&](size_t index) { return likelihoodOf(plan.layouts[index]); });
            AOT::sort(ranked);
            for (size_t index = 0; index < ranked.size(); ++index)
                plan.layouts[index] = std::get<LayoutID>(ranked[index]);
        }
        BuildContext& build = inputs().build;
        uint64_t limit = build.limit(LimitID::MaxJSONLayoutSetMembers);
        if (plan.layouts.size() > limit) {
            if (m_reportsLimits)
                build.gaveUp(LimitID::MaxJSONLayoutSetMembers, Subject { .function = inputs().functionAsRead, .layout = plan.layouts[limit] });
            plan.layouts.shrink(limit);
        }
        return plan.layouts.isEmpty() ? ValuePlan { } : WTF::move(plan);
    }

    Expected<BlockFrequency> currentFrequency() { return function().at(builder().currentBlock()).frequency; }
    TypeID cursorType() { return inputs().opaqueType(OpaqueKind::BuilderCursor); }

    void ensureCursor(uint32_t bytes)
    {
        if (m_cursor.value && m_cursor.reservedBytes >= bytes)
            return;
        RELEASE_ASSERT(bytes <= maxStringBuilderReservation);
        commit();

        uint64_t size = std::max<uint64_t>(bytes, inputs().build.limit(LimitID::MinRequestedJSONCapacity));
        RELEASE_ASSERT(size <= maxStringBuilderReservation);
        m_cursor.value = builder().append(make<&Value::setSize>(Opcode::StringBuilderReserve, { m_stringBuilder }, static_cast<uint32_t>(size)));
        m_cursor.reservedBytes = static_cast<uint32_t>(size);
    }

    void append(Value&& cursorRow)
    {
        Opcode opcode = cursorRow.opcode;
        uint32_t taken = bytesTakenFromReservation(program(), cursorRow);
        ensureCursor(taken);
        cursorRow.setBuilderOperand(m_stringBuilder);
        cursorRow.setCursorOperand(m_cursor.value);
        ValueID cursor = builder().append(WTF::move(cursorRow));
        if (opcode == Opcode::JSONAppendValue)
            cursor = appendResultAt(cursor, cursorResultOfJSONAppendValue);
        m_cursor = { cursor, renewsReservation(opcode) ? maxStringBuilderReservation : m_cursor.reservedBytes - taken };
    }

    void appendToken(JSONToken token) { append(make<&Value::setJsonToken>(Opcode::JSONAppendToken, { }, token)); }

    void appendText(ValueID string)
    {
        commit();
        builder().append(Opcode::StringBuilderAppend, { m_stringBuilder, string });
    }

    void commit()
    {
        if (!m_cursor.value)
            return;
        builder().append(Opcode::StringBuilderCommit, { m_stringBuilder, m_cursor.value });
        m_cursor = { };
    }

    // An arm that has left for another merge point leaves the builder in no block. From there a jump is nothing, and so is going on at a merge point that no arm has reached.
    bool isInBlock() { return !!builder().currentBlock(); }
    MergePoint createMergePoint() { return { .frequency = currentFrequency() }; }

    void jumpTo(MergePoint& merge, ValueID guard = { })
    {
        if (!isInBlock())
            return;
        ensureCursor(0);
        if (!merge.block) {
            merge.block = builder().addBlock(merge.frequency);
            merge.cursor.value = builder().addParameter(merge.block, cursorType());
            if (guard)
                merge.guard = builder().addParameter(merge.block, TypeTable::guard);
        }
        RELEASE_ASSERT(!guard == !merge.guard);
        merge.cursor.reservedBytes = std::min(merge.cursor.reservedBytes, m_cursor.reservedBytes);
        builder().emitJump(merge.block, guard ? Arguments { m_cursor.value, guard } : Arguments { m_cursor.value });
        m_cursor = { };
    }

    void continueAt(const MergePoint& merge)
    {
        if (!merge.block) {
            RELEASE_ASSERT(!isInBlock());
            return;
        }
        continueAt(merge.block, merge.cursor);
    }

    void continueAt(BlockID block, Cursor cursor)
    {
        builder().moveToEndOf(block);
        m_cursor = cursor;
    }

    void mergeIfAnyArmLeftFor(MergePoint& merge)
    {
        if (!merge.block)
            return;
        jumpTo(merge);
        continueAt(merge);
    }

    void leaveAfterUnknownCode()
    {
        if (!m_afterUnknownCode || !isInBlock())
            return;
        Expected<BlockFrequency> frequency = currentFrequency();
        jumpTo(*m_afterUnknownCode);
        Block& block = builder().function().edit(m_afterUnknownCode->block);
        block.frequency = moreFrequentOf(block.frequency, frequency);
    }

    // The host and a serializer run for a value what JSON.stringify runs for it.
    bool writingRunsCode(const Type& written) const
    {
        EffectDescription description;
        describeOverload(program(), program().builtins().overloadIDsOf(Builtins::JSONStringify).at(0), written, description);
        return description.callsUnknownCode || !description.functions.isEmpty() || !description.slowPathFunctions.isEmpty();
    }

    void leaveIfWritingRunsCode(ValueID written)
    {
        if (writingRunsCode(typeOf(written)))
            leaveAfterUnknownCode();
    }

    bool callsSerializerThatRunsCode(const ValuePlan& plan) const
    {
        SetForScope planOnlyToAsk(m_reportsLimits, false);
        switch (plan.kind) {
        case ValueKind::Generic:
        case ValueKind::Nullish:
        case ValueKind::Scalar:
            return false;
        case ValueKind::Array:
            return callsSerializerThatRunsCode(planType(program().at(plan.element), nullptr));
        case ValueKind::Objects:
            return std::ranges::any_of(plan.layouts, [&](LayoutID layout) { return writingRunsCode(Type::forLayout(layout, inputs().types)); });
        case ValueKind::ObjectParts:
            return std::ranges::any_of(plan.parts, [&](const ValuePlan& part) { return callsSerializerThatRunsCode(part); });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isObjectOrArrayThatRunsCode(const ValuePlan& plan) const
    {
        switch (plan.kind) {
        case ValueKind::Generic:
        case ValueKind::Nullish:
        case ValueKind::Scalar:
            return false;
        case ValueKind::Array:
            return writingRunsCode(program().at(plan.tested));
        case ValueKind::Objects:
            return std::ranges::any_of(plan.layouts, [&](LayoutID layout) { return writingRunsCode(Type::forLayout(layout, inputs().types)); });
        case ValueKind::ObjectParts:
            return std::ranges::any_of(plan.parts, [&](const ValuePlan& part) { return isObjectOrArrayThatRunsCode(part); });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What is listed is around an object or an array whose writing may run code.
    bool mayListHolders(const ValuePlan& plan) const
    {
        SetForScope planOnlyToAsk(m_reportsLimits, false);
        switch (plan.kind) {
        case ValueKind::Generic:
        case ValueKind::Nullish:
        case ValueKind::Scalar:
            return false;
        case ValueKind::Array:
            return isObjectOrArrayThatRunsCode(planType(program().at(plan.element), nullptr));
        case ValueKind::ObjectParts:
            return std::ranges::any_of(plan.parts, [&](const ValuePlan& part) { return mayListHolders(part); });
        case ValueKind::Objects:
            break;
        }
        return std::ranges::any_of(plan.layouts, [&](LayoutID layout) {
            for (uint32_t field = 0; field < program().at(layout).fields.size(); ++field) {
                MemberSupport support = memberSupportOf(layout, field);
                if (support != MemberSupport::NotSerialized && support != MemberSupport::WithoutText && isObjectOrArrayThatRunsCode(planField(layout, field)))
                    return true;
            }
            return false;
        });
    }

    ValuePlan planField(LayoutID layout, uint32_t fieldIndex) const
    {
        TypingContext typing { program(), function(), inputs().types };
        const Field& field = program().at(layout).fields[fieldIndex];
        bool holdsMoreThanDeclared = field.slotType || field.declared.isExpected();
        Type proven = field.slotType ? program().at(slotTypeOf(program(), field)) : typing.typeOf(field.declared);
        return planType(proven, holdsMoreThanDeclared ? &program().at(field.declared.expected().predicted()) : nullptr);
    }

    bool mayHoldItself(const ValuePlan& plan, Vector<LayoutID, 8>& layoutsAround, BitVector& finishedLayouts) const
    {
        SetForScope planOnlyToAsk(m_reportsLimits, false);
        switch (plan.kind) {
        case ValueKind::Generic:
        case ValueKind::Nullish:
        case ValueKind::Scalar:
            return false;
        case ValueKind::Array:
            return mayHoldItself(planType(program().at(plan.element), nullptr), layoutsAround, finishedLayouts);
        case ValueKind::ObjectParts:
            return std::ranges::any_of(plan.parts, [&](const ValuePlan& part) { return mayHoldItself(part, layoutsAround, finishedLayouts); });
        case ValueKind::Objects:
            break;
        }
        for (LayoutID layout : plan.layouts) {
            if (layoutsAround.contains(layout))
                return true;
            if (finishedLayouts.set(layout.index()))
                continue;
            layoutsAround.append(layout);
            for (uint32_t field = 0; field < program().at(layout).fields.size(); ++field) {
                MemberSupport support = memberSupportOf(layout, field);
                if (support != MemberSupport::NotSerialized && support != MemberSupport::WithoutText && mayHoldItself(planField(layout, field), layoutsAround, finishedLayouts))
                    return true;
            }
            layoutsAround.removeLast();
        }
        return false;
    }

    bool mayHoldItself(LayoutID layout) const
    {
        Vector<LayoutID, 8> layoutsAround;
        BitVector finishedLayouts;
        ValuePlan plan { .kind = ValueKind::Objects };
        plan.layouts.append(layout);
        return mayHoldItself(plan, layoutsAround, finishedLayouts);
    }

    // Lists those of the first holders that are not listed yet.
    ListedHolders listHolders(size_t numberOfHolders)
    {
        ListedHolders listed;
        for (Holder& holder : m_holders.mutableSpan().first(numberOfHolders)) {
            if (holder.listed)
                continue;
            holder.listed = builder().append(Opcode::JSONEnterHolder, { m_stringBuilder, holder.value });
            if (!listed.number++)
                listed.outermost = holder.listed;
        }
        return listed;
    }

    void appendLeaveHolders(const ListedHolders& listed)
    {
        if (listed.number)
            builder().append(Opcode::JSONLeaveHolder, { m_stringBuilder, listed.outermost });
    }

    // The holders that were listed last are the innermost ones.
    void markHoldersAsNotListed(const ListedHolders& listed)
    {
        uint32_t numberLeft = listed.number;
        for (Holder& holder : m_holders | std::views::reverse) {
            if (!numberLeft)
                return;
            if (std::exchange(holder.listed, ValueID()))
                --numberLeft;
        }
        RELEASE_ASSERT(!numberLeft);
    }

    void unlistHolders(const ListedHolders& listed)
    {
        appendLeaveHolders(listed);
        markHoldersAsNotListed(listed);
    }

    // The list as it is here. A handler that goes on writing puts it back, because the frames that the exception has ended have taken nothing off it.
    ValueID appendListedHolders()
    {
        for (const Holder& holder : m_holders | std::views::reverse) {
            if (holder.listed)
                return holder.listed;
        }
        return builder().append(Opcode::JSONListedHolders, { m_stringBuilder });
    }

    TypeArm beginTypeArm(ValueID value, TypeID tested, Expected<BlockFrequency> otherwise)
    {
        TypeArm arm { evaluateTypeTest(program(), typeOf(value), program().at(tested)), value, value, { } };
        if (arm.matches != KnownBoolean::Unknown)
            return arm;

        IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(builder().appendTypeTest(value, tested), testedOperandIndex, BlockFrequency::Normal, otherwise);
        return { KnownBoolean::Unknown, branch.provenOnTaken, branch.provenOnNotTaken, branch.notTaken };
    }

    // False: every value has the type of the arm.
    bool endTypeArm(const TypeArm& arm, MergePoint& done, Cursor start)
    {
        jumpTo(done);
        if (!arm.otherwise) {
            continueAt(done);
            return false;
        }
        continueAt(arm.otherwise, start);
        return true;
    }

    void appendRoot(ValueID value, BlockID whenThereIsNoText)
    {
        ValuePlan plan = planValue(value);
        if (plan.kind == ValueKind::Generic) {
            builder().emitJump(whenThereIsNoText);
            builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
            return;
        }

        ValueID start = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
        m_codeMayRunAroundValues = writingRunsCode(typeOf(value));
        appendValue(value, plan, Position { });
        commit();
        ValueID end = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
        emitJumpIf(builder().append(Opcode::StrictEq, { start, end }), whenThereIsNoText);
    }

    void appendValue(ValueID value, const ValuePlan& plan, const Position& position)
    {
        if (!plan.mayBeNullish || plan.kind == ValueKind::Generic) {
            appendPlannedValue(value, plan, position);
            return;
        }

        MergePoint done = createMergePoint();
        Cursor start = m_cursor;
        for (TypeID nullish : { TypeTable::undefined, TypeTable::null }) {
            TypeArm arm = beginTypeArm(value, nullish, BlockFrequency::Normal);
            if (arm.matches == KnownBoolean::False)
                continue;

            if (nullish == TypeTable::null || position.kind == JSONPosition::Element) {
                appendKeyOf(position);
                appendToken(JSONToken::Null);
                appendCommaOf(position);
            }
            if (!endTypeArm(arm, done, start))
                return;
            value = arm.remaining;
        }

        appendPlannedValue(value, plan, position);
        mergeIfAnyArmLeftFor(done);
    }

    void appendPlannedValue(ValueID value, const ValuePlan& plan, const Position& position)
    {
        switch (plan.kind) {
        case ValueKind::Generic:
        case ValueKind::Nullish:
            appendGeneric(value, position);
            return;
        case ValueKind::Objects:
            appendObjects(value, plan, position);
            return;
        case ValueKind::ObjectParts:
            appendObjectParts(value, plan, position);
            return;
        case ValueKind::Scalar:
        case ValueKind::Array:
            break;
        }

        RELEASE_ASSERT(plan.tested);
        KnownBoolean hasTestedType = evaluateTypeTest(program(), typeOf(value), program().at(plan.tested));
        if (hasTestedType == KnownBoolean::False) {
            appendGeneric(value, position);
            return;
        }

        MergePoint done = createMergePoint();
        Cursor start = m_cursor;
        BlockID generic;
        auto ensureGenericBlock = [&] {
            if (!generic)
                generic = builder().addBlock(BlockFrequency::Rare);
            return generic;
        };

        ValueID narrowed = value;
        if (hasTestedType == KnownBoolean::Unknown)
            narrowed = emitTypeTest(value, plan.tested, ensureGenericBlock());
        bool isDate = plan.row == Opcode::JSONAppendDate;
        if (isDate && !pristineAspectsProvenBy(typeOf(narrowed), BuiltinClasses::Date).contains(PristineAspects::Names))
            emitJumpUnless(builder().append(makePristineTest(narrowed, BuiltinClasses::Date, PristineAspects::Names)), ensureGenericBlock());

        if (plan.kind == ValueKind::Array)
            appendArray(narrowed, plan, position);
        else {
            appendKeyOf(position);
            if (isDate) {
                BuiltinOverloadID getTime = findGetTimeOfDate();
                RELEASE_ASSERT(getTime);
                narrowed = builder().append(makeBuiltinCall(getTime, BuiltinCallForm::Whole, { narrowed, { } }, { }));
                appendFirstReadsOfMembersOfDate(narrowed);
            }
            append(make<&Value::setValueOperand>(plan.row, { }, narrowed));
            appendCommaOf(position);
        }
        if (!generic)
            return;
        jumpTo(done);

        continueAt(generic, start);
        appendGeneric(value, position);
        jumpTo(done);
        continueAt(done);
    }

    // The host reads toJSON and valueOf by name and, for a valid date alone, toISOString. Each first read adds its member to Date.prototype.
    void appendFirstReadsOfMembersOfDate(ValueID time)
    {
        auto tryMakeFirstReadOf = [&](BuiltinID member) {
            return tryMakeInitializeBuiltin(program().builtins(), inputs().flow, member);
        };
        for (BuiltinID member : { BuiltinID(Builtins::DatePrototypeToJSON), BuiltinID(Builtins::DatePrototypeValueOf) }) {
            if (std::optional<Value> read = tryMakeFirstReadOf(member))
                builder().append(WTF::move(*read));
        }
        std::optional<Value> readOfValidDate = tryMakeFirstReadOf(Builtins::DatePrototypeToISOString);
        if (!readOfValidDate)
            return;
        MergePoint done = createMergePoint();
        ensureCursor(0);
        Cursor start = m_cursor;
        BlockID isInvalid = builder().emitBranch(builder().append(Opcode::StrictEq, { time, time }), currentFrequency(), BlockFrequency::Rare).notTaken;
        builder().append(WTF::move(*readOfValidDate));
        jumpTo(done);
        continueAt(isInvalid, start);
        jumpTo(done);
        continueAt(done);
    }

    void appendKeyOf(const Position& position)
    {
        if (position.kind != JSONPosition::Property)
            return;

        StringID text = program().at(position.key).text;
        std::optional<JSONToken> token = position.memberList.separator;
        if (canWriteJSONKeyAtCursor(program(), text)) {
            append(make<&Value::setString, &Value::setOpensObject, &Value::setFollowsMember>(Opcode::JSONAppendKey, { }, text, token == JSONToken::OpenObject, token == JSONToken::Comma));
            return;
        }

        if (token)
            appendToken(*token);
        StringBuilder quotedKey;
        quotedKey.appendQuotedJSONString(program().strings.toWTFString(text));
        quotedKey.append(':');
        appendText(appendString(quotedKey.toString()));
    }

    void appendCommaOf(const Position& position)
    {
        if (position.endsWithComma())
            appendToken(JSONToken::Comma);
    }

    void appendObjectParts(ValueID value, const ValuePlan& plan, const Position& position)
    {
        MergePoint done = createMergePoint();
        Cursor start = m_cursor;
        auto appendParts = [&] {
            for (const ValuePlan& part : plan.parts) {
                if (part.kind == ValueKind::Objects) {
                    appendObjects(value, part, position);
                    return;
                }

                TypeArm arm = beginTypeArm(value, part.tested, BlockFrequency::Normal);
                if (arm.matches == KnownBoolean::False)
                    continue;
                appendPlannedValue(arm.narrowed, part, position);
                if (!arm.otherwise)
                    return;
                jumpTo(done);
                continueAt(arm.otherwise, start);
                value = arm.remaining;
            }
            appendGeneric(value, position);
        };
        appendParts();
        mergeIfAnyArmLeftFor(done);
    }

    void appendGeneric(ValueID value, const Position& position)
    {
        if (position.kind == JSONPosition::Root)
            return;

        MergePoint done = createMergePoint();
        Cursor start = m_cursor;
        TypeArm primitiveArm = beginTypeArm(value, inputs().types.intern(Type::fromPrimitives(primitivesWithJSONText)), BlockFrequency::Unlikely);
        if (primitiveArm.matches != KnownBoolean::False) {
            appendKeyOf(position);
            append(make<&Value::setJsonPosition, &Value::setValueOperand>(Opcode::JSONAppendValue, { }, JSONPosition::Element, primitiveArm.narrowed));
            appendCommaOf(position);
            if (!endTypeArm(primitiveArm, done, start))
                return;
        }

        if (hasNoJSONText(typeOf(primitiveArm.remaining))) {
            if (position.kind == JSONPosition::Element)
                appendToken(JSONToken::Null);
        } else {
            TypeArm undefinedArm = beginTypeArm(primitiveArm.remaining, TypeTable::undefined, BlockFrequency::Rare);
            if (undefinedArm.matches != KnownBoolean::False) {
                if (position.kind == JSONPosition::Element)
                    appendToken(JSONToken::Null);
                if (!endTypeArm(undefinedArm, done, start))
                    return;
            }
            appendByHost(undefinedArm.remaining, position);
            leaveIfWritingRunsCode(undefinedArm.remaining);
        }
        mergeIfAnyArmLeftFor(done);
    }

    ValueID appendCallOfSerializer(FunctionID serializer, ValueID object)
    {
        ValueID outcome = appendCallOf(serializer, { object, m_stringBuilder });
        builder().function().edit(outcome).setCalledBuiltin(m_calledBuiltin);
        return outcome;
    }

    void appendByHost(ValueID value, const Position& position)
    {
        RELEASE_ASSERT(position.kind != JSONPosition::Root);
        bool isProperty = position.kind == JSONPosition::Property;
        MergePoint done = createMergePoint();
        ValueID checkpoint;
        ValueID property = position.index;
        if (isProperty) {
            commit();
            checkpoint = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
            appendKeyOf(position);
            property = appendString(program().at(position.key).text);
        }
        commit();
        RELEASE_ASSERT(!m_holders.isEmpty());
        ListedHolders listed = listHolders(m_holders.size() - 1);
        ValueID innermostHolder = m_holders.last().listed ? builder().append(Opcode::Undefined, { }) : m_holders.last().value;
        ValueID hasText = builder().append(make<&Value::setCalledBuiltin>(Opcode::StringBuilderAppendJSON, { m_stringBuilder, value, property, innermostHolder }, m_calledBuiltin));
        unlistHolders(listed);
        BlockID hasNoText = builder().emitBranch(hasText).notTaken;
        appendCommaOf(position);
        jumpTo(done);

        builder().moveToEndOf(hasNoText);
        if (isProperty)
            builder().append(Opcode::StringBuilderRollBack, { m_stringBuilder, checkpoint });
        else
            appendToken(JSONToken::Null);
        jumpTo(done);
        continueAt(done);
    }

    void appendObjects(ValueID value, const ValuePlan& plan, const Position& position)
    {
        RELEASE_ASSERT(!plan.layouts.isEmpty());
        commit();

        bool isProperty = position.kind == JSONPosition::Property;
        Expected<BlockFrequency> frequency = currentFrequency();
        MergePoint done = createMergePoint();
        BlockID generic = builder().addBlock(BlockFrequency::Rare);
        // A serializer that gives up has been called all the same, so what comes behind it has run unknown code.
        BlockID afterSerializerGaveUp = position.kind == JSONPosition::Root ? generic : builder().addBlock(BlockFrequency::Rare);
        ValueID object = emitTypeTest(value, TypeTable::object, generic);

        for (LayoutID layout : plan.layouts) {
            BlockID next = layout == plan.layouts.last() ? generic : builder().addBlock(frequency);
            TestedObject tested = emitShapeTest(object, singleElementSpan(layout), next);

            // Around the object code of the program may have run, so the call as written cannot start over. While the object is written none runs, so the host can write it from the beginning.
            bool runsCode = writingRunsCode(typeOf(tested.object));
            bool isWrittenByHostAfterThrow = !runsCode && m_codeMayRunAroundValues && position.kind != JSONPosition::Root && mayHoldItself(layout);

            ValueID checkpoint;
            if (isProperty || isWrittenByHostAfterThrow)
                checkpoint = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
            if (isProperty) {
                appendKeyOf(position);
                commit();
            }

            RELEASE_ASSERT(!m_cursor.value);
            BlockID unknown = builder().addBlock(BlockFrequency::Rare);
            FunctionID serializer = inputs().functions.ensure(derivedFunctionKey(FunctionRole::Serializer, layout));
            BlockID written = builder().addBlock(currentFrequency());
            ListedHolders listed = runsCode ? listHolders(m_holders.size()) : ListedHolders { };
            ValueID outcome;
            if (isWrittenByHostAfterThrow) {
                ValueID holdersAroundCall = appendListedHolders();
                std::optional<Target> outerHandler = builder().handler();
                BlockID afterCall = builder().addBlock(currentFrequency());
                BlockID threw = builder().addHandlerBlock(BlockRole::CatchHandler, ValueID());
                builder().setHandler(Target { threw, { }, { } });
                BlockID covered = builder().addBlock(currentFrequency());
                builder().setHandler(WTF::move(outerHandler));
                builder().emitJump(covered);

                builder().moveToEndOf(covered);
                outcome = appendCallOfSerializer(serializer, tested.object);
                builder().emitJump(afterCall);

                builder().moveToEndOf(threw);
                builder().append(Opcode::CatchValue, { });
                builder().append(Opcode::JSONRestoreHolders, { m_stringBuilder, holdersAroundCall });
                builder().emitJump(unknown);
                builder().moveToEndOf(afterCall);
            } else
                outcome = appendCallOfSerializer(serializer, tested.object);
            unlistHolders(listed);
            emitBranchOnOutcome(outcome, written, unknown, unknown);

            builder().moveToEndOf(written);
            appendCommaOf(position);
            leaveIfWritingRunsCode(tested.object);
            jumpTo(done);

            builder().moveToEndOf(unknown);
            if (checkpoint)
                builder().append(Opcode::StringBuilderRollBack, { m_stringBuilder, checkpoint });
            builder().emitJump(afterSerializerGaveUp);

            builder().moveToEndOf(next);
        }

        appendGeneric(value, position);
        jumpTo(done);

        if (afterSerializerGaveUp != generic) {
            builder().moveToEndOf(afterSerializerGaveUp);
            appendByHost(object, position);
            leaveIfWritingRunsCode(object);
            jumpTo(done);
        }
        continueAt(done);
    }

    void appendObjectOfLayout(ValueID object, LayoutID layout, BlockID whenShapeDiffers)
    {
        TestedObject tested = emitShapeTest(object, singleElementSpan(layout), whenShapeDiffers);
        if (!receiverStateOf(program(), layout).cannot(ObjectMutation::ChangePrototype))
            emitJumpUnless(builder().append(Opcode::HasLayoutPrototype, { tested.object, tested.guard }), whenShapeDiffers);
        BlockID done = builder().addBlock();

        // An order that is a prefix of another order shares its arm and costs one test, and an order of n fields has n + 1 prefixes.
        BuildContext& build = inputs().build;
        uint64_t maxArms = build.limit(LimitID::MaxExpandedKeyOrders);
        uint64_t maxFields = build.limit(LimitID::MaxExpandedKeyFields);
        ExpectedKeyOrders expectedOrders = expectedKeyOrdersOf(program(), layout, KeyOrderLimits { maxArms * (maxFields + 1), maxFields });
        Vector<KeyOrderFamily, 4> families = groupKeyOrders(layout, expectedOrders.orders.span(), maxArms);
        // Stores after an object's creation give it keys in an unlisted order.
        bool writesAnyOrder = !expectedOrders.coversEveryObject && !families.isEmpty() && tested.guard && hasObservedKeyAdditions(layout);
        Expected<BlockFrequency> frequencyOfOtherOrders = writesAnyOrder ? BlockFrequency::Normal : BlockFrequency::Rare;
        BlockID noOrder = builder().addBlock(frequencyOfOtherOrders);
        // A dedicated arm is the fastest code for an order. Among the creation orders, the longest is the most likely to stay unchanged.
        if (writesAnyOrder) {
            auto longest = std::ranges::max_element(families, { }, [](const KeyOrderFamily& family) { return family.longestOrder.size(); });
            std::swap(families.first(), *longest);
            maxArms = 1;
        }

        uint64_t numberOfArmsLeftOut = expectedOrders.numberOfOrdersLeftOut;
        if (families.size() > maxArms) {
            numberOfArmsLeftOut += families.size() - maxArms;
            families.shrink(static_cast<size_t>(maxArms));
        }
        if (numberOfArmsLeftOut && !writesAnyOrder)
            build.exceeds(LimitID::MaxExpandedKeyOrders, maxArms + numberOfArmsLeftOut, Subject { .function = inputs().functionAsRead, .layout = layout });

        for (const KeyOrderFamily& family : families) {
            BlockID next = builder().addBlock(&family == &families.last() ? frequencyOfOtherOrders : BlockFrequency::Normal);

            ObjectArm arm { { tested.object, { } }, layout, family, { }, { }, { }, done };
            for (uint32_t position = 0; position < family.longestOrder.size(); ++position) {
                uint32_t fieldIndex = family.longestOrder[position];
                if (memberSupportOf(layout, fieldIndex) == MemberSupport::NotSerialized)
                    continue;
                arm.members.append(Member { program().at(layout).fields[fieldIndex].key, fieldIndex, position, planField(layout, fieldIndex) });
            }

            TestedKeyOrder order = emitKeyOrderTests(tested, arm, next);
            arm.object.guard = order.guard;
            arm.numberOfOwnMembers = order.numberOfOwnMembers;

            m_holders.append(Holder { tested.object });
            if (std::ranges::any_of(arm.members, [&](const Member& member) { return callsSerializerThatRunsCode(member.plan); }))
                arm.listedHolders = listHolders(m_holders.size());

            MemberListState memberList = notOpened;
            for (size_t index = 0; index < arm.members.size(); ++index)
                appendMember(arm, index, memberList);
            appendToken(memberList.closing);
            commit();
            appendLeaveHolders(arm.listedHolders);
            builder().emitJump(done);

            appendRemainder(arm);
            markHoldersAsNotListed(arm.listedHolders);
            m_holders.removeLast();
            builder().moveToEndOf(next);
        }
        builder().emitJump(noOrder);

        builder().moveToEndOf(noOrder);
        if (writesAnyOrder)
            appendObjectInAnyOrder(tested, layout, whenShapeDiffers, done);
        else
            builder().emitJump(whenShapeDiffers);
        builder().moveToEndOf(done);
    }

    // Every store to some member is visible, and not all of them are part of a creation: the program itself gives objects of the layout a key later.
    bool hasObservedKeyAdditions(LayoutID layout) const
    {
        // Whether a key computed at class evaluation is a string or a symbol is unknown here.
        if (std::ranges::any_of(program().at(layout).fields, [](const Field& field) { return !!field.computedKey; }))
            return false;
        // A serializer runs no program code itself: the host does, after checking whether a holder is being written twice.
        for (uint32_t field = 0; field < program().at(layout).fields.size(); ++field) {
            if (memberSupportOf(layout, field) == MemberSupport::ByName)
                return false;
        }
        for (uint32_t field = 0; field < program().at(layout).fields.size(); ++field) {
            if (!isEnumerableStringField(program(), program().at(layout).fields[field]))
                continue;
            const FieldFlow& flow = flowOf(program(), { layout, field });
            if (flow.location.writers == WriterState::AllKnown && flow.location.stores != StoreCount::Never && flow.location.initialization != InitializationState::SetAtCreation && flow.occupancy != Occupancy::AlwaysFilled)
                return true;
        }
        return false;
    }

    ValueID appendFieldInKeyOrder(ValueID order, ValueID position, LayoutID layout)
    {
        return builder().append(make<&Value::setLayout>(Opcode::GetFieldInKeyOrder, { order, position }, layout));
    }

    // A loop over the object's keys, in order. Each key dispatches to the code that writes its field's member.
    void appendObjectInAnyOrder(const TestedObject& tested, LayoutID layout, BlockID ifOrderIsNotListed, BlockID done)
    {
        static constexpr int64_t orderIsNotListed = -2;
        Expected<BlockFrequency> frequency = currentFrequency();
        ValueID object = tested.object;

        ValueID order = builder().append(make<&Value::setLayout>(Opcode::GetOrderWord, { object, tested.guard }, layout));
        ValueID firstField = appendFieldInKeyOrder(order, builder().appendInteger(0), layout);
        emitJumpIf(builder().append(Opcode::StrictEq, { firstField, builder().appendInteger(orderIsNotListed) }), ifOrderIsNotListed);
        ValueID count = builder().append(Opcode::BitAnd, { builder().append(Opcode::URShift, { order, builder().appendInteger(orderWordCountShift) }), builder().appendInteger(maxFieldsInOrder) });

        Vector<Member, 8> members;
        for (uint32_t field = 0; field < program().at(layout).fields.size(); ++field) {
            if (memberSupportOf(layout, field) == MemberSupport::NotSerialized)
                continue;
            members.append(Member { program().at(layout).fields[field].key, field, 0, planField(layout, field) });
            // The read does not rely on the key being listed.
            members.last().plan.mayBeNullish = true;
        }

        m_holders.append(Holder { object });
        ListedHolders listed;
        if (std::ranges::any_of(members, [&](const Member& member) { return callsSerializerThatRunsCode(member.plan); }))
            listed = listHolders(m_holders.size());
        appendToken(JSONToken::OpenObject);

        BlockID head = builder().addBlock(frequency);
        ValueID position = builder().addParameter(head, inputs().types.intern(Type::integer({ 0, typeOf(count).integers.maximum })));
        ValueID cursor = builder().addParameter(head, cursorType());
        ValueID guard = builder().addParameter(head, TypeTable::guard);
        builder().emitJump(head, { builder().appendInteger(0), m_cursor.value, tested.guard });

        // One reservation covers whichever member comes next.
        continueAt(head, { cursor, 0 });
        builder().append(Opcode::LoopHint, { });
        ensureCursor(1);
        Cursor atKey = m_cursor;
        IRBuilder::ProvenBranch hasKey = builder().emitBranchWithProof(builder().append(Opcode::Less, { position, count }));
        ValueID field = appendFieldInKeyOrder(order, hasKey.provenOnTaken, layout);
        Vector<std::pair<int64_t, Target>> cases;
        for (const Member& member : members)
            cases.append({ static_cast<int64_t>(member.field), Target { builder().addBlock(frequency), { }, { } } });
        Vector<BlockID, 8> blocksOfMembers = cases.map<Vector<BlockID, 8>>([](auto& entry) { return entry.second.block; });
        BlockID isNotWritten = builder().addBlock(frequency);
        builder().emitSwitch(field, WTF::move(cases), Target { isNotWritten, { }, { } });

        MergePoint next { .frequency = frequency };
        BlockID remainder;
        continueAt(isNotWritten, atKey);
        jumpTo(next, guard);

        for (size_t index = 0; index < members.size(); ++index) {
            const Member& member = members[index];
            continueAt(blocksOfMembers[index], atKey);
            ValueID value;
            switch (memberSupportOf(layout, member.field)) {
            case MemberSupport::WithoutText:
                jumpTo(next, guard);
                continue;
            case MemberSupport::Slot:
                value = appendFieldRead({ object, guard }, layout, member.field, Opcode::GetSlotOrUndefined);
                break;
            case MemberSupport::ByName:
            case MemberSupport::NotSerialized:
                RELEASE_ASSERT_NOT_REACHED();
            }

            MergePoint afterUnknownCode { .frequency = BlockFrequency::Rare };
            MergePoint* outer = std::exchange(m_afterUnknownCode, &afterUnknownCode);
            appendValue(value, member.plan, Position { JSONPosition::Property, member.key, { }, afterSeparator });
            m_afterUnknownCode = outer;
            jumpTo(next, guard);
            if (!afterUnknownCode.block)
                continue;

            // The keys written are those the object had at the start.
            continueAt(afterUnknownCode);
            commit();
            BlockID stale = builder().addBlock(BlockFrequency::Rare);
            TestedObject renewed = emitShapeTest(object, singleElementSpan(layout), stale);
            emitJumpUnless(builder().append(Opcode::StrictEq, { builder().append(make<&Value::setLayout>(Opcode::GetOrderWord, { object, renewed.guard }, layout)), order }), stale);
            jumpTo(next, renewed.guard ? renewed.guard : guard);

            builder().moveToEndOf(stale);
            if (!remainder)
                remainder = builder().addBlock(BlockFrequency::Rare);
            builder().emitJump(remainder);
        }

        continueAt(next);
        builder().emitJump(head, { builder().append(Opcode::Add, { hasKey.provenOnTaken, builder().appendInteger(1) }), m_cursor.value, next.guard });

        continueAt(hasKey.notTaken, atKey);
        appendToken(JSONToken::CloseObjectOverComma);
        commit();
        appendLeaveHolders(listed);
        builder().emitJump(done);

        if (remainder) {
            builder().moveToEndOf(remainder);
            ValueID names = builder().append(make<&Value::setLayout>(Opcode::GetKeyNamesInOrder, { order, builder().append(Opcode::Add, { hasKey.provenOnTaken, builder().appendInteger(1) }) }, layout));
            ListedHolders listedForHost = listHolders(m_holders.size() - 1);
            ValueID innermostHolder = m_holders.last().listed ? builder().append(Opcode::Undefined, { }) : object;
            ValueID hasText = builder().append(make<&Value::setCalledBuiltin>(Opcode::StringBuilderAppendJSONMembers, { m_stringBuilder, object, names, builder().appendInteger(0), innermostHolder }, m_calledBuiltin));
            unlistHolders(listedForHost);
            BlockID hasNoText = builder().emitBranch(hasText).notTaken;
            appendText(appendString("}"_s));
            appendLeaveHolders(listed);
            builder().emitJump(done);

            builder().moveToEndOf(hasNoText);
            appendToken(JSONToken::CloseObjectOverComma);
            commit();
            appendLeaveHolders(listed);
            builder().emitJump(done);
        }
        markHoldersAsNotListed(listed);
        m_holders.removeLast();
    }

    // An arm of its own is the fastest code for an order, so orders share an arm only where there are more of them than arms, and those first that differ least.
    Vector<KeyOrderFamily, 4> groupKeyOrders(LayoutID layout, std::span<const KeyOrderFields> orders, uint64_t maxArms) const
    {
        const Vector<Field>& fields = program().at(layout).fields;
        auto canBeMissingFrom = [&](std::span<const uint32_t> order, size_t count) {
            return std::ranges::all_of(order.subspan(count), [&](uint32_t field) {
                MemberSupport support = memberSupportOf(layout, field);
                if (support != MemberSupport::Slot)
                    return support != MemberSupport::ByName;
                return !!(program().at(slotTypeOf(program(), fields[field])).primitives & Primitive::Undefined);
            });
        };

        Vector<KeyOrderFamily, 4> families;
        for (const KeyOrderFields& order : orders)
            families.append(KeyOrderFamily { order, { static_cast<uint32_t>(order.size()) } });

        while (families.size() > maxArms) {
            struct { size_t shorter { notFound }; size_t longer { notFound }; size_t numberOfMissingFields { 0 }; } best;
            for (size_t shorter = 0; shorter < families.size(); ++shorter) {
                for (size_t longer = 0; longer < families.size(); ++longer) {
                    std::span<const uint32_t> longestOrder = families[longer].longestOrder.span();
                    if (longestOrder.size() <= families[shorter].longestOrder.size() || !spanHasPrefix(longestOrder, families[shorter].longestOrder.span()))
                        continue;
                    size_t smallestCount = families[shorter].smallestCount();
                    size_t numberOfMissingFields = longestOrder.size() - smallestCount;
                    bool isBetter = best.shorter == notFound || numberOfMissingFields < best.numberOfMissingFields
                        || (numberOfMissingFields == best.numberOfMissingFields && longestOrder.size() > families[best.longer].longestOrder.size());
                    if (isBetter && canBeMissingFrom(longestOrder, smallestCount))
                        best = { shorter, longer, numberOfMissingFields };
                }
            }
            if (best.shorter == notFound)
                break;

            KeyOrderFamily merged = WTF::move(families[best.longer]);
            merged.descendingCounts.appendVector(families[best.shorter].descendingCounts);
            AOT::sort(merged.descendingCounts, std::ranges::greater { });
            // Flow lists the most common order first. The arms are tested in list order, and the limit cuts off the end of the list.
            families[std::min(best.shorter, best.longer)] = WTF::move(merged);
            families.removeAt(std::max(best.shorter, best.longer));
        }
        return families;
    }

    TestedKeyOrder emitKeyOrderTests(const TestedObject& tested, const ObjectArm& arm, BlockID notTaken)
    {
        std::span<const uint32_t> longestOrder = arm.orders.longestOrder.span();
        std::span<const uint32_t> counts = arm.orders.descendingCounts.span();
        if (counts.size() == 1 && tested.guard && inputs().build.isSabotaged(Sabotage::WrittenRecordsHaveTheirKeysInOneOrder))
            return { tested.guard, { } };
        if (counts.size() == 1)
            return { emitKeyOrderTest(tested, arm.layout, longestOrder, notTaken), { } };

        Expected<BlockFrequency> frequency = currentFrequency();
        BlockID matched = builder().addBlock(frequency);
        ValueID guard = builder().addParameter(matched, TypeTable::guard);
        ValueID numberOfOwnMembers = builder().addParameter(matched, TypeTable::int32);
        for (uint32_t count : counts) {
            bool isLastCount = count == counts.back();
            BlockID next = isLastCount ? notTaken : builder().addBlock(frequency);
            ValueID guardOfOrder = emitKeyOrderTest(tested, arm.layout, longestOrder.first(count), next);
            auto numberOfMembers = std::ranges::count_if(arm.members, [&](const Member& member) { return member.positionInOrder < count; });
            builder().emitJump(matched, { guardOfOrder, builder().appendInteger(numberOfMembers) });
            if (!isLastCount)
                builder().moveToEndOf(next);
        }
        builder().moveToEndOf(matched);
        return { guard, numberOfOwnMembers };
    }

    void appendMember(ObjectArm& arm, size_t index, MemberListState& memberList)
    {
        const Member& member = arm.members[index];
        const ValuePlan& plan = member.plan;
        Expected<BlockFrequency> frequency = currentFrequency();

        ValueID value;
        bool isReadByName = false;
        switch (memberSupportOf(arm.layout, member.field)) {
        case MemberSupport::WithoutText:
            // The member stays in the list: where the guards go stale, the remaining keys are read by name.
            return;
        case MemberSupport::Slot:
            value = appendFieldRead(arm.object, arm.layout, member.field, member.positionInOrder < arm.orders.smallestCount() ? Opcode::GetSlot : Opcode::GetSlotOrUndefined);
            break;
        case MemberSupport::ByName:
            commit();
            value = builder().append(make<&Value::setKey>(Opcode::PropGet, { arm.object.object }, member.key));
            isReadByName = true;
            break;
        case MemberSupport::NotSerialized:
            RELEASE_ASSERT_NOT_REACHED();
        }

        const Type& type = typeOf(value);
        bool mayBeOmitted = plan.kind == ValueKind::Generic || plan.isExpected || (type.primitives & primitivesWithoutJSONText) || type.hasObjects();
        if (mayBeOmitted && memberList.separator) {
            appendToken(*memberList.separator);
            memberList = afterSeparator;
        }

        MergePoint afterUnknownCode { .frequency = BlockFrequency::Rare };
        MergePoint* outer = std::exchange(m_afterUnknownCode, &afterUnknownCode);
        appendValue(value, plan, Position { JSONPosition::Property, member.key, { }, memberList });
        if (isReadByName)
            leaveAfterUnknownCode();
        m_afterUnknownCode = outer;
        if (memberList.separator == JSONToken::OpenObject)
            memberList = afterMember;

        if (!afterUnknownCode.block)
            return;
        // Behind the last member nothing relies on the guards.
        bool isLast = index + 1 == arm.members.size();
        MergePoint next { .frequency = frequency };
        jumpTo(next, isLast ? ValueID() : arm.object.guard);
        continueAt(afterUnknownCode);
        if (!isLast)
            renewGuards(arm, index + 1, memberList);
        jumpTo(next, isLast ? ValueID() : arm.object.guard);
        continueAt(next);
        if (!isLast)
            arm.object.guard = next.guard;
    }

    void renewGuards(ObjectArm& arm, size_t nextMember, MemberListState memberList)
    {
        if (inputs().build.isSabotaged(Sabotage::WrittenRecordsStayAsTheyWere))
            return;
        commit();
        BlockID stale = builder().addBlock(BlockFrequency::Rare);
        TestedObject tested = emitShapeTest(arm.object.object, singleElementSpan(arm.layout), stale);
        TestedKeyOrder order = emitKeyOrderTests(tested, arm, stale);
        // The keys that are written are those the object had at the beginning.
        if (order.numberOfOwnMembers)
            emitJumpUnless(builder().append(Opcode::StrictEq, { order.numberOfOwnMembers, arm.numberOfOwnMembers }), stale);
        arm.object.guard = order.guard;
        BlockID renewed = builder().currentBlock();

        builder().moveToEndOf(stale);
        if (memberList.separator)
            appendToken(*memberList.separator);
        commit();
        if (!arm.remainder) {
            arm.remainder = builder().addBlock(BlockFrequency::Rare);
            builder().addParameter(arm.remainder, TypeTable::int32);
        }
        builder().emitJump(arm.remainder, { builder().appendInteger(static_cast<int64_t>(nextMember)) });

        builder().moveToEndOf(renewed);
    }

    void appendRemainder(const ObjectArm& arm)
    {
        if (!arm.remainder)
            return;

        RELEASE_ASSERT(!m_cursor.value);
        builder().moveToEndOf(arm.remainder);
        ValueID firstMember = function().at(arm.remainder).parameters[0];

        Vector<ValueID, 8> keyTexts;
        for (const Member& member : arm.members)
            keyTexts.append(appendString(program().at(member.key).text));
        ValueID keys = builder().append(make<&Value::setContents, &Value::setElementsOperands>(Opcode::NewArray, { }, TypeTable::string, keyTexts.span()));
        if (arm.numberOfOwnMembers)
            builder().append(make<&Value::setKey, &Value::setMode>(Opcode::PropSet, { keys, arm.numberOfOwnMembers }, program().keyOf(BuiltinNames::length), ECMAMode::Strict));

        RELEASE_ASSERT(m_holders.last().value == arm.object.object);
        ListedHolders listed = listHolders(m_holders.size() - 1);
        ValueID innermostHolder = m_holders.last().listed ? builder().append(Opcode::Undefined, { }) : arm.object.object;
        ValueID hasText = builder().append(make<&Value::setCalledBuiltin>(Opcode::StringBuilderAppendJSONMembers, { m_stringBuilder, arm.object.object, keys, firstMember, innermostHolder }, m_calledBuiltin));
        unlistHolders(listed);
        BlockID hasNoText = builder().emitBranch(hasText).notTaken;
        appendText(appendString("}"_s));
        appendLeaveHolders(arm.listedHolders);
        builder().emitJump(arm.done);

        builder().moveToEndOf(hasNoText);
        appendToken(JSONToken::CloseObjectOverComma);
        commit();
        appendLeaveHolders(arm.listedHolders);
        builder().emitJump(arm.done);
    }

    IRBuilder::ProvenBranch emitBranchOnNextIndex(ValueID index, ValueID length)
    {
        ValueID next = builder().append(Opcode::Add, { index, builder().appendInteger(1) });
        return builder().emitBranchWithProof(builder().append(Opcode::Less, { next, length }));
    }

    bool objectsAmongElementsGoToHost(ArrayStorage storage, TypeID elementType) const
    {
        SetForScope planOnlyToAsk(m_reportsLimits, false);
        Type read = typeOfElementRead(storage);
        return read.hasObjects() && planType(read, &program().at(elementType)).kind == ValueKind::Generic;
    }

    void appendArray(ValueID array, const ValuePlan& plan, const Position& position)
    {
        MergePoint done = createMergePoint();
        MergePoint written = createMergePoint();
        MergePoint closing = createMergePoint();
        BlockID generic = builder().addBlock(BlockFrequency::Rare);
        Cursor start = m_cursor;

        ValueID pristine = emitProvenBranch(builder().append(makePristineTest(array, BuiltinClasses::Array, serializedArrayAspects)), generic);
        ValueID length = builder().append(Opcode::GetArrayLength, { array, pristine });
        ValueID isEmpty = builder().append(Opcode::StrictEq, { length, builder().appendInteger(0) });
        BlockID hasElements = builder().emitBranch(isEmpty).notTaken;
        appendKeyOf(position);
        appendToken(JSONToken::EmptyArray);
        jumpTo(written);

        builder().moveToEndOf(hasElements);
        bool elementsMayRunUnknownCode = false;
        m_holders.append(Holder { array });
        ListedHolders listed = callsSerializerThatRunsCode(plan) ? listHolders(m_holders.size()) : ListedHolders { };
        for (const ArrayStorageForm& form : arrayStorageForms) {
            if (!typeOfElementRead(form.kind).withoutNullish().overlaps(program().at(plan.element), program()))
                continue;
            IRBuilder::ProvenBranch hasStorage = builder().emitBranchWithProof(builder().append(makeArrayStorageTest(array, form.kind)));
            ValueID guards[] { pristine, hasStorage.provenOnTaken };
            // Each object among elements of unknown type is a host call. A list that starts with one is passed to the host whole, in one call.
            if (!listed.number && objectsAmongElementsGoToHost(form.kind, plan.element)) {
                Value first = make<&Value::setArrayStorage, &Value::setKeyIsIntegral>(Opcode::GetArrayElement, { array, builder().appendInteger(0), builder().appendAllGuards(guards) }, form.kind, true);
                emitJumpIf(builder().appendTypeTest(builder().append(WTF::move(first)), TypeTable::object), generic);
            }
            m_cursor = start;
            appendKeyOf(position);
            if (!form.holdsCells) {
                // Nothing but numbers and holes: the run time writes the list in one call, in fewer instructions a number than a loop here takes.
                append(make<&Value::setJsonPosition, &Value::setValueOperand>(Opcode::JSONAppendValue, { }, JSONPosition::Element, array));
                appendLeaveHolders(listed);
                jumpTo(written);
                builder().moveToEndOf(hasStorage.notTaken);
                continue;
            }
            appendToken(JSONToken::OpenArray);
            elementsMayRunUnknownCode |= appendArrayLoop({ array, builder().appendAllGuards(guards), length, plan.element, closing }, form.kind) == ElementCode::MayRunUnknownCode;
            builder().moveToEndOf(hasStorage.notTaken);
        }
        unlistHolders(listed);
        m_holders.removeLast();
        builder().emitJump(generic);

        if (closing.block) {
            continueAt(closing);
            appendToken(JSONToken::CloseArray);
            appendLeaveHolders(listed);
            if (elementsMayRunUnknownCode && m_afterUnknownCode) {
                appendCommaOf(position);
                leaveAfterUnknownCode();
            }
            jumpTo(written);
        }
        continueAt(written);
        appendCommaOf(position);
        jumpTo(done);

        continueAt(generic, start);
        appendGeneric(array, position);
        jumpTo(done);
        continueAt(done);
    }

    enum class ElementCode : uint8_t { RunsNoUnknownCode, MayRunUnknownCode };
    ElementCode appendArrayLoop(const ArrayLoop& loop, ArrayStorage storage)
    {
        BlockID body = builder().addBlock(currentFrequency());
        ValueID index = builder().addParameter(body, inputs().types.intern(Type::integer({ 0, typeOf(loop.length).integers.maximum - 1 })));
        ValueID cursor = builder().addParameter(body, cursorType());
        ValueID guard = builder().addParameter(body, TypeTable::guard);
        ValueID firstIndex = builder().appendInteger(0);
        ensureCursor(0);
        builder().emitJump(body, { firstIndex, m_cursor.value, loop.guard });

        continueAt(body, { cursor, 0 });
        builder().append(Opcode::LoopHint, { });
        Value read = make<&Value::setArrayStorage, &Value::setKeyIsIntegral>(Opcode::GetArrayElement, { loop.array, index, guard }, storage, true);

        MergePoint afterUnknownCode { .frequency = BlockFrequency::Rare };
        MergePoint* outer = std::exchange(m_afterUnknownCode, &afterUnknownCode);
        appendArrayElement(loop, builder().append(WTF::move(read)), index);
        m_afterUnknownCode = outer;

        auto appendStepToNextElement = [&](bool renewsGuards) {
            IRBuilder::ProvenBranch hasNext = emitBranchOnNextIndex(index, loop.length);
            Cursor afterElement = m_cursor;
            BlockID stale;
            ValueID guardOfNext = guard;
            if (renewsGuards) {
                stale = builder().addBlock(BlockFrequency::Rare);
                ValueID renewed[] {
                    emitProvenBranch(builder().append(makePristineTest(loop.array, BuiltinClasses::Array, serializedArrayAspects)), stale),
                    emitProvenBranch(builder().append(makeArrayStorageTest(loop.array, storage)), stale),
                };
                guardOfNext = builder().appendAllGuards(renewed);
                ValueID currentLength = builder().append(Opcode::GetArrayLength, { loop.array, guardOfNext });
                emitJumpUnless(builder().append(Opcode::Less, { hasNext.provenOnTaken, currentLength }), stale);
            }
            appendToken(JSONToken::Comma);
            builder().emitJump(body, { hasNext.provenOnTaken, m_cursor.value, guardOfNext });

            continueAt(hasNext.notTaken, afterElement);
            jumpTo(loop.closing);
            if (renewsGuards) {
                continueAt(stale, afterElement);
                appendRemainingElements(loop, hasNext.provenOnTaken);
            }
        };
        if (isInBlock())
            appendStepToNextElement(false);
        if (!afterUnknownCode.block)
            return ElementCode::RunsNoUnknownCode;
        continueAt(afterUnknownCode);
        appendStepToNextElement(true);
        return ElementCode::MayRunUnknownCode;
    }

    void appendArrayElement(const ArrayLoop& loop, ValueID element, ValueID index)
    {
        MergePoint done = createMergePoint();
        if (typeOf(element).primitives & Primitive::Absent) {
            Cursor start = m_cursor;
            IRBuilder::ProvenBranch isHole = builder().emitBranchWithProof(builder().appendTypeTest(element, TypeTable::absent), testedOperandIndex, BlockFrequency::Unlikely, BlockFrequency::Normal);
            appendToken(JSONToken::Null);
            jumpTo(done);

            continueAt(isHole.notTaken, start);
            element = isHole.provenOnNotTaken;
        }

        appendValue(element, planType(typeOf(element), &program().at(loop.elementType)), { .kind = JSONPosition::Element, .index = index });
        mergeIfAnyArmLeftFor(done);
    }

    void appendRemainingElements(const ArrayLoop& loop, ValueID firstIndex)
    {
        BlockID body = builder().addBlock(BlockFrequency::Rare);
        ValueID index = builder().addParameter(body, function().at(firstIndex).type);
        commit();
        builder().emitJump(body, { firstIndex });

        builder().moveToEndOf(body);
        builder().append(Opcode::LoopHint, { });
        ValueID element = builder().append(make<&Value::setKeyIsIntegral>(Opcode::ElemGet, { loop.array, index }, true));
        appendToken(JSONToken::Comma);
        appendByHost(element, { .kind = JSONPosition::Element, .index = index });
        commit();

        IRBuilder::ProvenBranch hasNext = emitBranchOnNextIndex(index, loop.length);
        builder().emitJump(body, { hasNext.provenOnTaken });

        builder().moveToEndOf(hasNext.notTaken);
        jumpTo(loop.closing);
    }

    ValueID m_stringBuilder;
    BuiltinID m_calledBuiltin { };
    Cursor m_cursor { };
    // The outermost first.
    Vector<Holder, 4> m_holders;
    bool m_codeMayRunAroundValues { false };
    // False while a plan is made only to ask something about it: the plan that is written reports what it leaves out.
    mutable bool m_reportsLimits { true };
    // Where an arm goes on that has run unknown code: the object or the array around the value renews its guards there, and the other arms keep theirs.
    MergePoint* m_afterUnknownCode { nullptr };
};

} // anonymous namespace

bool JSONSerializerBuilder::hasPlanFor(ValueID value) const
{
    JSONTextWriter writer { { builder, inputs }, stringBuilder };
    return writer.planValue(value).kind != JSONTextWriter::ValueKind::Generic && !writer.typeOf(value).hasOnlyPrimitives(Primitive::Undefined);
}

bool JSONSerializerBuilder::writesOnePrimitive(ValueID value) const
{
    JSONTextWriter writer { { builder, inputs }, stringBuilder };
    writer.m_reportsLimits = false;
    JSONTextWriter::ValuePlan plan = writer.planValue(value);
    return plan.kind == JSONTextWriter::ValueKind::Nullish || (plan.kind == JSONTextWriter::ValueKind::Scalar && !inputs.types.at(plan.tested).hasObjects());
}

bool JSONSerializerBuilder::writingRunsCode(ValueID value) const
{
    JSONTextWriter writer { { builder, inputs }, stringBuilder };
    return writer.writingRunsCode(writer.typeOf(value));
}

bool JSONSerializerBuilder::mayListHolders(ValueID value) const
{
    JSONTextWriter writer { { builder, inputs }, stringBuilder };
    writer.m_reportsLimits = false;
    return writer.mayListHolders(writer.planValue(value));
}

bool JSONSerializerBuilder::mayHoldItself(ValueID value) const
{
    JSONTextWriter writer { { builder, inputs }, stringBuilder };
    writer.m_reportsLimits = false;
    Vector<LayoutID, 8> layoutsAround;
    BitVector finishedLayouts;
    return writer.mayHoldItself(writer.planValue(value), layoutsAround, finishedLayouts);
}

void JSONSerializerBuilder::appendRoot(ValueID value, BlockID whenThereIsNoText)
{
    JSONTextWriter { { builder, inputs }, stringBuilder, calledBuiltin }.appendRoot(value, whenThereIsNoText);
}

DerivedSignature signatureOfSerializer(const EmissionInputs& inputs)
{
    return { { TypeTable::object, inputs.opaqueType(OpaqueKind::StringBuilder) }, { inputs.outcomeType() } };
}

Function buildSerializer(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    LayoutID layout(key.parameter);
    EmissionInputs inputs(derived, FunctionID());
    DerivedFunctionHead head = createDerivedFunction(inputs, FunctionRole::Serializer, CreatorRecord::Serializer { layout }, signatureOfSerializer(inputs));

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    JSONTextWriter writer { { builder, inputs }, head.parameters[1] };

    BlockID differs = builder.addBlock(BlockFrequency::Rare);
    builder.emitFailureOfPredictions(Target { differs, { }, { } });
    writer.m_codeMayRunAroundValues = writer.writingRunsCode(Type::forLayout(layout, inputs.types));
    // Where code may run, the holders are listed. Where none runs, nothing is kept about them until the text has grown long: then the run time asks for those that follow.
    if (!writer.m_codeMayRunAroundValues && writer.mayHoldItself(layout)) {
        IRBuilder::BranchBlocks atEntry = builder.emitBranch(builder.append(Emission::make<&Value::setFailsAtWatchedLevels>(Opcode::JSONHasStackSpace, { }, true)), BlockFrequency::Normal, BlockFrequency::Rare);
        builder.moveToEndOf(atEntry.notTaken);
        IRBuilder::BranchBlocks whenAsked = builder.emitBranch(builder.append(Opcode::JSONEnterWatchedLevel, { head.parameters[1], head.parameters[0] }), BlockFrequency::Rare, BlockFrequency::Rare);
        builder.moveToEndOf(whenAsked.notTaken);
        builder.emitJump(differs);
        builder.moveToEndOf(whenAsked.taken);
        builder.emitJump(atEntry.taken);
        builder.moveToEndOf(atEntry.taken);
    } else
        writer.emitJumpUnless(builder.append(Opcode::JSONHasStackSpace, { }), differs);
    writer.appendObjectOfLayout(head.parameters[0], layout, differs);
    builder.emitReturn(writer.appendOutcome(JSONOutcome::Parsed));

    builder.moveToEndOf(differs);
    builder.emitReturn(writer.appendOutcome(JSONOutcome::Unknown));
    return WTF::move(head.function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
