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

// What the algebra of types knows of classes, built-ins, layouts and objects. Whoever has two ids asks TypeArena, which remembers.

// AOTTypeIntersection.cpp
// The members of `set` that pass a test which goes by the member, by the four words and by nothing else but the program. No word is an id of the thread.
// What nobody knows is typed as anything, with every object beside it that has escaped: a hundred thousand in a large program. What is picked out of them lies in the thread and dies with the unit.
// So of a set of the program that is longer than a chunk, the threads of Compile ask each member once for the build: SharedAnswers.
// A long set of the thread is mostly what is left of such a set: IDSetArena::setOfProgramBehind(). What passes of it is what passes of that set and is a member, which takes an intersection by chunks,
// and what passes of the members that it has from elsewhere, which are asked. Anybody else, and about any other set, asks each member each time.
// `askEachMemberOf` is handed `set`, the set of the program behind it, or the members from elsewhere: whether a member passes must not hang on the set that it is asked in.
ObjectSetID filterMembersOncePerBuild(TypeArena&, SharedQuestion, ObjectSetID set, std::array<uint32_t, 4> words, NOESCAPE const ScopedLambda<ObjectSetID(ObjectSetID)>& askEachMemberOf);

enum class ClassRelation : uint8_t { Unknown, Disjoint };

ClassRelation relate(const BuiltinTable&, BuiltinClassID, BuiltinClassID);
bool isFunctionClass(const BuiltinTable&, BuiltinClassID);
// A proxy need not be callable. Every instance of such a class lies in `function`.
bool holdsOnlyCallables(const BuiltinTable&, BuiltinClassID);
bool isCallableBuiltin(const BuiltinTable&, BuiltinID);
bool isTestableBuiltin(const BuiltinTable&, BuiltinID);
bool hasInstanceTest(const BuiltinTable&, BuiltinClassID);

// Where the type written for a slot of a class's contents stands among the contents of its part: Map<K, V> has Key at 0 and Value at 1, Promise<T> has Value at 0,
// and every other class with contents has Element at 0. Decided by the class and the classes above it.
std::optional<uint8_t> positionInTypeOf(const BuiltinTable&, BuiltinClassID, ContentsSlot);
ContentsSlot defaultContentsSlotOf(const BuiltinTable&, BuiltinClassID);

// Any function, a named closure or built-in function, which has no class, or an object of a class of callables. What nobody describes may be called too: overlap() with TypeTable::function.
bool mayContainAnyCallable(TypeReader, const Type&);
// A proxy may be callable. It is the closure of no function.
bool mayContainAnyClosure(TypeReader, const Type&);
bool mayBeInstanceOf(const BuiltinTable&, BuiltinID, BuiltinClassID);
// A member of the library is written in JavaScript: the table's built-in and the closure of the function are one object under two names.
bool mayBeClosureOf(TypeReader, BuiltinID, FunctionID);
bool mayContainBuiltin(TypeReader, const Type&, BuiltinID);
bool overlapsAnotherClass(TypeReader, BuiltinClassID, const Type&);

// The part for the class, or for a class above it that the same test tells from everything else.
const ClassPart* findPartHolding(TypeReader, std::span<const ClassPart>, BuiltinClassID);
// An object of a subclass of a built-in class is an instance of that class, with its own prototype and properties.
// An object of a layout that holdsOrdinaryObjects is an instance of BuiltinClasses::Object. Neither has a shape word, and the types learn that from this function alone.
BuiltinClassID builtinClassWrittenAbove(TypeReader, LayoutID);
bool mayHoldConstructedInstances(const BuiltinTable&, BuiltinClassID holder, BuiltinClassID constructed);
bool isInsideSomeClassPart(TypeReader, LayoutID, std::span<const ClassPart>);
bool mayBeInsideSomeClassPart(TypeReader, LayoutID, std::span<const ClassPart>);

// The layouts that an object with ObjectTrait::HasLayout may have: the one in its record, the members of the tagged family that this one makes,
// and where `new.target` may have selected a layout below, what is written below with the members of the families made there. In no order, and a layout may come twice.
// Type::layouts lists them, describedByShape() puts them in place of the object, and surelyFitsUnknownOf() and mayFitUnknownOf() go over them: isSubset(type, forLayoutSet(type.layouts)) rests on that.
void forEachPossibleLayoutOf(const ObjectTable&, const Program&, const ObjectRecord&, NOESCAPE const ScopedLambda<void(LayoutID)>&);

// Of one named object against the `unknown` of a type: anyObject, anyFunction, unnamedLayouts, unnamedClasses.
bool surelyFitsUnknownOf(TypeReader, ObjectID, const Type&);
bool mayFitUnknownOf(TypeReader, ObjectID, const Type&);
// For whoever asks about many objects and one type. Whether a cell that nobody describes may have one of the layouts that the type lists hangs on the class of the cell alone, and takes a look at each of the layouts.
struct LayoutsFoundForCellsNobodyDescribes {
    struct OfClass {
        // Null: the cells without a class.
        BuiltinClassID builtinClass;
        bool mayHaveListedLayout;
    };
    // Type::unnamedLayouts of the type. A type that lists none may be asked about in between.
    LayoutSetID listed;
    Vector<OfClass, 4> byClass;
};
bool mayFitUnknownOf(TypeReader, ObjectID, const Type&, LayoutsFoundForCellsNobodyDescribes&);
// Two numbers, one object of a run: the same number; a copy for an instance and its shared form; a merged form and anything that merges into it;
// a built-in and the closure that implements it; what a class's constructor makes and a site or a constructed object of the same layout.
bool mayBeSameObject(TypeReader, ObjectID, ObjectID);

// A bit for each ObjectKind.
using ObjectKinds = uint32_t;
constexpr ObjectKinds kindsWith(ObjectKind kind) { return 1u << static_cast<unsigned>(kind); }

// What a set of named objects holds, read once, for whoever asks of each of many objects whether the set names it under another number.
// namesAliasOf() answers what mayBeSameObject() of the object and each member in turn would, and what an answer costs does not follow the size of the set:
//   a shared form and a merged form are looked up
//   the members that are compared by shape are kept by the layouts that they may have, and those without a layout by their classes, each with the kinds of the members
//   what the tables relate is kept by what relates it: built-ins by their entries, host objects by their holders, prototype objects by their declared layouts and constructors,
//   the closures of the library by their functions and by the entries that those implement
// The closures and the sites of the set are not read for an object that is itself a closure or a site.
// Whoever changes mayBeSameObject() changes this. The command `aliases` of testaot compares the two, and so does namesAliasOf() now and then where assertions are on.
class ObjectSetAliasIndex {
    WTF_MAKE_NONCOPYABLE(ObjectSetAliasIndex);
public:
    ObjectSetAliasIndex(TypeReader, const SetView<ObjectID, ObjectSetID>& members);

    // For an object that is no member: it may be the same object as some member that is no copy of it for an instance.
    bool namesAliasOf(ObjectID);

    // How often mayBeSameObject() was asked. For tests.
    size_t numberOfPairsAsked() const { return m_numberOfPairsAsked; }

private:
    // Of members that are copies for instances, of a kind in which two numbers may be one object. What tells such a copy from the other copies of its shared form is its number alone,
    // and it is neither that form nor another copy of it. So it is kept like any member, under its shared form, and whoever asks leaves out what its own shared form has brought.
    class SharedFormsOfCopies {
    public:
        void add(ObjectID sharedForm)
        {
            if (!m_first)
                m_first = sharedForm;
            else if (m_first != sharedForm)
                m_areSeveral = true;
        }

        // Null: any.
        bool holdOtherThan(ObjectID sharedForm) const { return m_areSeveral || (m_first && m_first != sharedForm); }

    private:
        ObjectID m_first;
        bool m_areSeveral { false };
    };

    struct LayoutOfMembers {
        // Of the members that may have the layout.
        ObjectKinds kinds { 0 };
        SharedFormsOfCopies copies;
        // The other three are of objects whose records name the layout, by whether they may have a layout below it.
        // Members that have been spread over the layouts that they may have.
        std::array<ObjectKinds, 2> kindsSpread { };
        // Objects that have asked namesObjectOfShape(), and those that heard yes.
        std::array<ObjectKinds, 2> kindsAsked { };
        std::array<ObjectKinds, 2> kindsWithAlias { };
        // Of a declared layout: some member is the Prototype of it or of a layout derived from it, or the FunctionPrototype of a function that constructs one of those.
        bool isOfPrototype { false };
        bool isOfPrototypeOfConstructor { false };
        SharedFormsOfCopies copiesOfPrototypes;
    };

    struct NoValue { };

    template<typename Value>
    class SlotsByNumber {
    public:
        // It makes the slot where there is none. The reference lasts until the next call.
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

        // Where the number lies, or the free slot where it belongs.
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

        // Empty, or a power of two.
        Vector<Slot, 8> m_slots;
        size_t m_numberInUse { 0 };
    };

    // Members without a layout of which mayBeCellNobodyDescribes(), all of one class or all without a class.
    struct CellsNobodyDescribes {
        const ObjectRecord* example;
        ObjectKinds kinds;
        SharedFormsOfCopies copies { };
    };

    struct ClassOfCells {
        BuiltinClassID builtinClass;
        ObjectKinds kinds;
    };

    void lookAtMembersButClosuresAndSites();
    void lookAtSites();
    void lookAtMember(ObjectID);
    void noteShapeOf(const ObjectRecord&);
    void noteLayout(LayoutID, const ObjectRecord& ofMember);
    void notePrototype(ObjectID, const ObjectRecord&);
    void noteFunctionPrototype(const ObjectRecord&);
    std::span<const ObjectID> closuresOfLibrary();
    bool answerFromWhatWasRead(ObjectID);
    // sharedForm: ObjectTable::sharedFormOf() of the object that asks.
    bool namesObjectOfShape(ObjectID sharedForm, const ObjectRecord&);
    bool namesPrototypeUnderAnotherName(ObjectID sharedForm, const ObjectRecord& prototype);
    bool namesPrototypeOfClassOf(FunctionID constructor);
    bool namesBuiltinThatMayBeClosureOf(FunctionID ofLibrary);
    bool namesClosureThatMayBe(BuiltinID);

    TypeReader m_reader;
    SetView<ObjectID, ObjectSetID> m_members;
    bool m_hasLookedAtMembers { false };
    bool m_hasLookedAtSites { false };
    bool m_hasListedClosuresOfLibrary { false };
    bool m_namesUnknown { false };
    // Of the members that have been looked at.
    ObjectKinds m_kinds { 0 };
    // A Prototype without a declared layout.
    Vector<ObjectID, 4> m_askedOneByOne;
    // As SharedFormsOfCopies has them. A second copy of one brings what the first has brought.
    SlotsByNumber<NoValue> m_sharedFormsOfCopies;
    Vector<ObjectID, 8> m_builtins;
    Vector<ObjectID, 4> m_copiesOfClosures;
    Vector<ObjectID, 4> m_closuresOfLibrary;
    // By the number of the entry: whether a Builtin names it, a ModuleBuiltin, or both.
    SlotsByNumber<ObjectKinds> m_kindsOfBuiltins;
    SlotsByNumber<NoValue> m_holdersOfHostObjects;
    SlotsByNumber<NoValue> m_functionsOfFunctionPrototypes;
    // The constructors of the declared layouts that have isOfPrototype or copiesOfPrototypes.
    SlotsByNumber<NoValue> m_constructorsOfPrototypes;
    // As mayBeClosureOf() asks. Of the built-ins, read when a closure of the library first asks.
    bool m_hasReadBuiltinsForClosures { false };
    bool m_namesBuiltinFilledWithValueOfLibrary { false };
    SlotsByNumber<NoValue> m_functionsThatMayBeBuiltins;
    // Of the closures of the library, read when a built-in first asks. The value: the function's record in the table has been found.
    bool m_hasReadClosuresForBuiltins { false };
    SlotsByNumber<bool> m_functionsOfClosuresOfLibrary;
    SlotsByNumber<NoValue> m_builtinsThatClosuresImplement;
    // What some member other than itself turns into once it has escaped.
    SlotsByNumber<NoValue> m_mergedForms;
    SlotsByNumber<LayoutOfMembers> m_layouts;
    Vector<CellsNobodyDescribes, 4> m_cellsNobodyDescribes;
    // Of the layouts that members may have: the classes of their cells, and the kinds of the members with a layout that a cell without a class may have. As mayBeMadeWithLayout() asks.
    Vector<ClassOfCells, 4> m_classesOfCellsOfLayouts;
    ObjectKinds m_kindsWithLayoutOfCellWithoutClass { 0 };
    size_t m_numberOfPairsAsked { 0 };
};

// The run time copies a record word by word: the copy has the layout of its source, and the source stands for its copies too. Facts and the result rules both go by this.
// What native code has made may be a record where its number names no shape.
bool mayHaveLayoutOfRecord(const Program&, const ObjectRecord&);
// Of the type of what CopyObject, CopyObjectAndDefine or CopyTwoObjects gives: what stands for the ordinary object that the operation makes of any other source. Null: nobody has numbered it.
ObjectID ordinaryObjectMadeByCopy(TypeReader, const Program&, const Type& typeOfCopy);

// Without a memo, for whoever holds the records.
bool isSubset(TypeReader, const Type&, const Type& of);
bool overlap(TypeReader, const Type&, const Type&);
bool mayBeSameObject(TypeReader, const Type&, const Type&);

inline constexpr uint16_t plainPrimitives = Primitive::Defined & ~(Primitive::Integer | Primitive::AnyString | Primitive::AnySymbol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
