/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <array>
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

ObjectSetID filterMembersOncePerBuild(TypeArena&, SharedQuestion, ObjectSetID set, std::array<uint32_t, 4> words, NOESCAPE const ScopedLambda<ObjectSetID(ObjectSetID)>& askEachMemberOf);

enum class ClassRelation : uint8_t { Unknown, Disjoint };

ClassRelation relate(const BuiltinTable&, BuiltinClassID, BuiltinClassID);
bool isFunctionClass(const BuiltinTable&, BuiltinClassID);
bool holdsOnlyCallables(const BuiltinTable&, BuiltinClassID);
bool isCallableBuiltin(const BuiltinTable&, BuiltinID);
bool isTestableBuiltin(const BuiltinTable&, BuiltinID);
bool hasInstanceTest(const BuiltinTable&, BuiltinClassID);
bool testTellsNoClassOfObjectsIn(TypeReader, const Type&);

std::optional<uint8_t> positionInTypeOf(const BuiltinTable&, BuiltinClassID, ContentsSlot);
ContentsSlot defaultContentsSlotOf(const BuiltinTable&, BuiltinClassID);

bool mayContainAnyCallable(TypeReader, const Type&);
bool mayContainAnyClosure(TypeReader, const Type&);
bool mayBeInstanceOf(const BuiltinTable&, BuiltinID, BuiltinClassID);
bool mayBeClosureOf(TypeReader, BuiltinID, FunctionID);
bool hostUsesPutValueInto(const BuiltinTable&, const BuiltinHostUses&, BuiltinID entry);
bool mayContainBuiltin(TypeReader, const Type&, BuiltinID);
bool overlapsAnotherClass(TypeReader, BuiltinClassID, const Type&);

const ClassPart* findPartHolding(TypeReader, std::span<const ClassPart>, BuiltinClassID);
BuiltinClassID builtinClassWrittenAbove(TypeReader, LayoutID);
bool mayHoldConstructedInstances(const BuiltinTable&, BuiltinClassID holder, BuiltinClassID constructed);
bool isInsideSomeClassPart(TypeReader, LayoutID, std::span<const ClassPart>);
bool mayBeInsideSomeClassPart(TypeReader, LayoutID, std::span<const ClassPart>);

bool surelyFitsUnknownOf(TypeReader, ObjectID, const Type&);
bool mayFitUnknownOf(TypeReader, ObjectID, const Type&);
struct LayoutsFoundForCellsNobodyDescribes {
    struct OfClass {
        BuiltinClassID builtinClass;
        bool mayHaveListedLayout;
    };
    LayoutSetID listed;
    Vector<OfClass, 4> byClass;
};
bool mayFitUnknownOf(TypeReader, ObjectID, const Type&, LayoutsFoundForCellsNobodyDescribes&);
bool mayBeSameObject(TypeReader, ObjectID, ObjectID);

using ObjectKinds = uint32_t;
constexpr ObjectKinds kindsWith(ObjectKind kind) { return 1u << static_cast<unsigned>(kind); }

class ObjectSetAliasIndex {
    WTF_MAKE_NONCOPYABLE(ObjectSetAliasIndex);
public:
    ObjectSetAliasIndex(TypeReader, const SetView<ObjectID, ObjectSetID>& members);

    bool namesAliasOf(ObjectID);

    size_t numberOfPairsAsked() const { return m_numberOfPairsAsked; }

private:
    struct LayoutOfMembers {
        ObjectKinds kinds { 0 };
        std::array<ObjectKinds, 2> kindsSpread { };
        std::array<ObjectKinds, 2> kindsAsked { };
        std::array<ObjectKinds, 2> kindsWithAlias { };
        bool isOfPrototype { false };
        bool isOfPrototypeOfConstructor { false };
    };

    struct NoValue { };

    template<typename Value>
    class SlotsByNumber {
    public:
        Value& at(uint32_t number)
        {
            if ((m_numberInUse + 1) * 2 > m_slots.size())
                grow();
            Slot& slot = m_slots[placeOf(number)];
            if (!slot.numberPlusOne) {
                slot.numberPlusOne = number + 1;
                ++m_numberInUse;
            }
            return slot.value;
        }

        const Value* find(uint32_t number) const
        {
            if (m_slots.isEmpty())
                return nullptr;
            const Slot& slot = m_slots[placeOf(number)];
            return slot.numberPlusOne ? &slot.value : nullptr;
        }

    private:
        struct Slot {
            uint32_t numberPlusOne { 0 };
            Value value { };
        };

        size_t placeOf(uint32_t number) const
        {
            size_t mask = m_slots.size() - 1;
            uint32_t hash = number * 0x9e3779b1u;
            size_t place = (hash ^ hash >> 15) & mask;
            while (m_slots[place].numberPlusOne && m_slots[place].numberPlusOne != number + 1)
                place = (place + 1) & mask;
            return place;
        }

        void grow()
        {
            Vector<Slot, 8> former = std::exchange(m_slots, { });
            m_slots.fill(Slot { }, std::max<size_t>(8, former.size() * 2));
            for (const Slot& slot : former) {
                if (slot.numberPlusOne)
                    m_slots[placeOf(slot.numberPlusOne - 1)] = slot;
            }
        }

        Vector<Slot, 8> m_slots;
        size_t m_numberInUse { 0 };
    };

    struct CellsNobodyDescribes {
        const ObjectRecord* example;
        ObjectKinds kinds;
    };

    struct ClassOfCells {
        BuiltinClassID builtinClass;
        ObjectKinds kinds;
    };

    void lookAtMembersButClosures();
    void lookAtMember(ObjectID);
    void noteShapeOf(const ObjectRecord&);
    void noteLayout(LayoutID, const ObjectRecord& ofMember);
    void notePrototype(ObjectID, const ObjectRecord&);
    void noteFunctionPrototype(const ObjectRecord&);
    std::span<const ObjectID> closuresOfLibrary();
    bool answerFromWhatWasRead(ObjectID);
    bool namesObjectOfShape(ObjectID, const ObjectRecord&);
    bool namesPrototypeUnderAnotherName(const ObjectRecord& prototype);
    bool namesPrototypeOfClassOf(FunctionID constructor);
    bool namesBuiltinThatMayBeClosureOf(FunctionID ofLibrary);
    bool namesClosureThatMayBe(BuiltinID);

    TypeReader m_reader;
    SetView<ObjectID, ObjectSetID> m_members;
    bool m_hasLookedAtMembers { false };
    bool m_hasListedClosuresOfLibrary { false };
    bool m_namesUnknown { false };
    ObjectKinds m_kinds { 0 };
    Vector<ObjectID, 4> m_askedOneByOne;
    Vector<ObjectID, 8> m_builtins;
    Vector<ObjectID, 4> m_closuresOfLibrary;
    SlotsByNumber<ObjectKinds> m_kindsOfBuiltins;
    SlotsByNumber<NoValue> m_holdersOfHostObjects;
    SlotsByNumber<NoValue> m_functionsOfFunctionPrototypes;
    SlotsByNumber<NoValue> m_constructorsOfPrototypes;
    bool m_hasReadBuiltinsForClosures { false };
    bool m_namesBuiltinFilledWithValueOfLibrary { false };
    SlotsByNumber<NoValue> m_functionsThatMayBeBuiltins;
    bool m_hasReadClosuresForBuiltins { false };
    SlotsByNumber<bool> m_functionsOfClosuresOfLibrary;
    SlotsByNumber<NoValue> m_builtinsThatClosuresImplement;
    SlotsByNumber<NoValue> m_mergedForms;
    SlotsByNumber<LayoutOfMembers> m_layouts;
    Vector<CellsNobodyDescribes, 4> m_cellsNobodyDescribes;
    Vector<ClassOfCells, 4> m_classesOfCellsOfLayouts;
    ObjectKinds m_kindsWithLayoutOfCellWithoutClass { 0 };
    size_t m_numberOfPairsAsked { 0 };
};

bool mayHaveLayoutOfRecord(const Program&, const ObjectRecord&);
ObjectID ordinaryObjectMadeByCopy(TypeReader, const Program&, const Type& typeOfCopy);

bool isSubset(TypeReader, const Type&, const Type& of);
bool overlap(TypeReader, const Type&, const Type&);
bool mayBeSameObject(TypeReader, const Type&, const Type&);

inline constexpr uint16_t plainPrimitives = Primitive::Defined & ~(Primitive::Integer | Primitive::AnyString | Primitive::AnySymbol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
