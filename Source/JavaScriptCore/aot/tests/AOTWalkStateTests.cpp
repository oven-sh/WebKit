/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTTypeTable.h"
#include "AOTWalkState.h"
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> walkStateTests();

using namespace Typed;

namespace {

class PlainListsOfWalkStateTests {
public:
    enum class Kind : uint8_t { Slot, Variable, EnvironmentField, Element, GuardOfStores, GuardOfReads };
    struct Shape {
        Reference object;
        TypeID proven;
    };
    struct Cell {
        Reference object;
        OptionSet<CellFact> facts;
        uint32_t orderWord;
    };
    struct Place {
        Kind kind;
        Reference base;
        uint32_t first;
        uint32_t second;
        Reference value;
    };

    TypeID shapeOf(Reference object) const
    {
        for (const Shape& shape : shapes) {
            if (shape.object == object)
                return shape.proven;
        }
        return { };
    }

    void noteShape(Reference object, TypeID proven)
    {
        shapes.removeAllMatching([&](const Shape& shape) {
            return shape.object == object;
        });
        if (proven)
            shapes.append({ object, proven });
    }

    OptionSet<CellFact> factsOf(Reference object) const
    {
        for (const Cell& cell : cells) {
            if (cell.object == object)
                return cell.facts;
        }
        return { };
    }

    std::optional<uint32_t> orderWordOf(Reference object) const
    {
        for (const Cell& cell : cells) {
            if (cell.object == object && cell.facts.contains(CellFact::OrderWordIsKnown))
                return cell.orderWord;
        }
        return std::nullopt;
    }

    void noteFacts(Reference object, OptionSet<CellFact> facts)
    {
        for (Cell& cell : cells) {
            if (cell.object == object) {
                cell.facts.add(facts);
                return;
            }
        }
        if (!facts.isEmpty())
            cells.append({ object, facts, 0 });
    }

    void noteOrderWord(Reference object, std::optional<uint32_t> orderWord)
    {
        if (!orderWord) {
            endFactsWhere(CellFact::OrderWordIsKnown, [&](Reference other) {
                return other == object;
            });
            return;
        }
        noteFacts(object, { CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties, CellFact::OrderWordIsKnown });
        for (Cell& cell : cells) {
            if (cell.object == object)
                cell.orderWord = *orderWord;
        }
    }

    void endFactsWhere(OptionSet<CellFact> ended, NOESCAPE const Invocable<bool(Reference)> auto& isMeant)
    {
        for (Cell& cell : cells) {
            if (isMeant(cell.object))
                cell.facts.remove(ended);
        }
        cells.removeAllMatching([](const Cell& cell) {
            return cell.facts.isEmpty();
        });
    }

    static OptionSet<CellFact> factsThatRestOn(AbstractHeapSet written)
    {
        OptionSet<CellFact> facts;
        if (written.contains(AbstractHeap::ShapeWords))
            facts.add({ CellFact::NotFrozen, CellFact::IsExtensible });
        if (written.contains(AbstractHeap::NamedProperties))
            facts.add({ CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties });
        if (written.contains(AbstractHeap::PrototypeLinks))
            facts.add(CellFact::HasLayoutPrototype);
        if (written.containsAny({ AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::OrderWords }))
            facts.add({ CellFact::OrderWordIsKnown, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent });
        return facts;
    }

    bool isFresh(Reference object) const { return fresh.contains(object); }

    void noteFresh(Reference object)
    {
        if (!isFresh(object))
            fresh.append(object);
    }

    void markEscaped(Reference object) { fresh.removeFirst(object); }

    Reference valueOf(Kind kind, Reference base, uint32_t first, uint32_t second) const
    {
        for (const Place& place : places) {
            if (place.kind == kind && place.base == base && place.first == first && place.second == second)
                return place.value;
        }
        return { };
    }

    void note(Kind kind, Reference base, uint32_t first, uint32_t second, Reference value)
    {
        bool maySharePlaceWithOthers = kind == Kind::Slot || kind == Kind::Element;
        if (maySharePlaceWithOthers && isFresh(base) && (!value || valueOf(kind, base, first, second) != value)) {
            places.removeAllMatching([&](const Place& place) {
                return place.kind == kind && place.base == base && place.first != first;
            });
        }
        places.removeAllMatching([&](const Place& place) {
            return place.kind == kind && place.base == base && place.first == first && place.second == second;
        });
        if (value)
            places.append({ kind, base, first, second, value });
    }

    void noteThatGuardsHaveBegun(uint32_t classLayout)
    {
        if (!begun.contains(classLayout))
            begun.append(classLayout);
    }

    bool someGuardHoldsOf(uint32_t classLayout) const
    {
        return places.containsIf([&](const Place& place) {
            return (place.kind == Kind::GuardOfStores || place.kind == Kind::GuardOfReads) && place.first == classLayout;
        });
    }

    void invalidateAliasedSlots(Reference object, FieldReference field, NOESCAPE const Invocable<bool(Reference)> auto& mayBeSameObject)
    {
        uint32_t layout = field.layout.index();
        places.removeAllMatching([&](const Place& place) {
            if (place.kind != Kind::Slot)
                return false;
            if (place.base == object)
                return place.first != layout;
            if (place.first == layout && place.second != field.field)
                return false;
            return !isFresh(object) && !isFresh(place.base) && mayBeSameObject(place.base);
        });
    }

    void invalidate(AbstractHeapSet written)
    {
        invalidate(written, [](Reference) {
            return true;
        }, [](LayoutID) {
            return true;
        });
    }

    void invalidate(AbstractHeapSet written, NOESCAPE const Invocable<bool(Reference)> auto& mayBeWritten, NOESCAPE const Invocable<bool(LayoutID)> auto& mayEndGuard)
    {
        constexpr AbstractHeapSet underElements { AbstractHeap::Elements, AbstractHeap::ArrayStorage, AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage };
        constexpr AbstractHeapSet underElementsOfObjects { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
        auto isWritten = [&](Reference object) {
            return !isFresh(object) && mayBeWritten(object);
        };
        if (written.contains(AbstractHeap::ShapeWords)) {
            shapes.removeAllMatching([&](const Shape& shape) {
                return isWritten(shape.object);
            });
        }
        endFactsWhere(factsThatRestOn(written), isWritten);
        places.removeAllMatching([&](const Place& place) {
            switch (place.kind) {
            case Kind::Slot:
                return written.contains(AbstractHeap::ObjectFields) && isWritten(place.base);
            case Kind::Variable:
                return written.contains(AbstractHeap::ModuleVariables);
            case Kind::EnvironmentField:
                return written.contains(AbstractHeap::EnvironmentFields) && isWritten(place.base);
            case Kind::Element:
                if (written.containsAny(underElements - underElementsOfObjects))
                    return written.containsAny(underElements - perObjectHeaps) || !isFresh(place.base);
                return written.containsAny(underElementsOfObjects) && isWritten(place.base);
            case Kind::GuardOfStores:
            case Kind::GuardOfReads:
                return written.contains(AbstractHeap::MemberGuards) && mayEndGuard(LayoutID(place.first));
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    void invalidateHeap()
    {
        shapes.removeAllMatching([&](const Shape& shape) {
            return !isFresh(shape.object);
        });
        places.removeAllMatching([&](const Place& place) {
            return !isFresh(place.base);
        });
        cells.removeAllMatching([&](const Cell& cell) {
            return !isFresh(cell.object);
        });
    }

    void meetWith(const PlainListsOfWalkStateTests& other)
    {
        for (Cell& cell : cells) {
            if (cell.facts.contains(CellFact::OrderWordIsKnown) && other.orderWordOf(cell.object) != cell.orderWord)
                cell.facts.remove(CellFact::OrderWordIsKnown);
            cell.facts = cell.facts & other.factsOf(cell.object);
        }
        cells.removeAllMatching([](const Cell& cell) {
            return cell.facts.isEmpty();
        });
        shapes.removeAllMatching([&](const Shape& shape) {
            return other.shapeOf(shape.object) != shape.proven;
        });
        places.removeAllMatching([&](const Place& place) {
            return other.valueOf(place.kind, place.base, place.first, place.second) != place.value;
        });
        fresh.removeAllMatching([&](Reference object) {
            return !other.isFresh(object);
        });
        begun.removeAllMatching([&](uint32_t classLayout) {
            return !other.begun.contains(classLayout);
        });
    }

    Vector<Shape> shapes;
    Vector<Cell> cells;
    Vector<Place> places;
    Vector<Reference> fresh;
    Vector<uint32_t> begun;
};

struct PairOfWalkStateTests {
    WalkState state;
    PlainListsOfWalkStateTests lists;
};

} // anonymous namespace

static Reference valueOfWalkStateTests(uint32_t number)
{
    return Reference::value(ValueID(number));
}

static FieldReference fieldOfWalkStateTests(uint32_t layout, uint32_t field)
{
    return { LayoutID(layout), field };
}

static Effects effectsThatWriteInWalkStateTests(AbstractHeapSet written)
{
    Effects effects = Effects::none();
    effects.writes = LocationMasks(written);
    return effects;
}

static constexpr uint32_t layoutsOfWalkStateTests[] { 0, 1, 64, 65, 63, UINT32_MAX };
static constexpr uint32_t classesOfWalkStateTests[] { 0, 1, 64, 65, 63, UINT32_MAX >> 3 };

static void checkSameAnswersInWalkStateTests(const PairOfWalkStateTests& pair, uint32_t numberOfObjects, uint32_t numberOfFields)
{
    using Kind = PlainListsOfWalkStateTests::Kind;
    const WalkState& state = pair.state;
    const PlainListsOfWalkStateTests& lists = pair.lists;
    AOT_TEST_CHECK(state.isEmpty() == (lists.shapes.isEmpty() && lists.cells.isEmpty() && lists.places.isEmpty() && lists.fresh.isEmpty() && lists.begun.isEmpty()));
    for (uint32_t layout : classesOfWalkStateTests) {
        AOT_TEST_CHECK(state.guardsHaveBegun(LayoutID(layout)) == lists.begun.contains(layout));
        AOT_TEST_CHECK(state.someGuardHoldsOf(LayoutID(layout)) == lists.someGuardHoldsOf(layout));
        for (uint32_t key = 0; key < numberOfFields; ++key) {
            AOT_TEST_CHECK(state.storesPassAbove(LayoutID(layout), PropertyKeyID(key)) == !!lists.valueOf(Kind::GuardOfStores, Reference(), layout, key));
            AOT_TEST_CHECK(state.guardOfReadsHolds(LayoutID(layout), PropertyKeyID(key)) == !!lists.valueOf(Kind::GuardOfReads, Reference(), layout, key));
        }
    }
    for (uint32_t number = 0; number < numberOfObjects; ++number) {
        Reference object = valueOfWalkStateTests(number);
        AOT_TEST_CHECK(state.shapeOf(object) == lists.shapeOf(object));
        AOT_TEST_CHECK(state.cellFacts(object) == lists.factsOf(object));
        AOT_TEST_CHECK(state.orderWordOf(object) == lists.orderWordOf(object));
        AOT_TEST_CHECK(state.isFresh(object) == lists.isFresh(object));
        for (uint32_t layout : layoutsOfWalkStateTests) {
            for (uint32_t field = 0; field < numberOfFields; ++field) {
                AOT_TEST_CHECK(state.slot(object, fieldOfWalkStateTests(layout, field)) == lists.valueOf(Kind::Slot, object, layout, field));
                AOT_TEST_CHECK(state.environmentField(object, fieldOfWalkStateTests(layout, field)) == lists.valueOf(Kind::EnvironmentField, object, layout, field));
            }
        }
        for (uint32_t index = 0; index < numberOfFields; ++index)
            AOT_TEST_CHECK(state.element(object, valueOfWalkStateTests(index)) == lists.valueOf(Kind::Element, object, valueOfWalkStateTests(index).bits(), 0));
        AOT_TEST_CHECK(state.variable(VariableID(number)) == lists.valueOf(Kind::Variable, Reference(), number, 0));
    }
}

static void runRandomStepsInWalkStateTests(unsigned seed, uint32_t numberOfObjects, uint32_t numberOfSteps, uint32_t stepsBetweenChecks)
{
    using Kind = PlainListsOfWalkStateTests::Kind;
    constexpr uint32_t numberOfLayouts = std::size(layoutsOfWalkStateTests);
    constexpr uint32_t numberOfFields = 4;
    constexpr size_t numberOfStates = 4;
    static constexpr TypeID shapes[] { TypeID(), TypeTable::object, TypeTable::number, TypeTable::string };
    static constexpr AbstractHeap heaps[] {
        AbstractHeap::ShapeWords, AbstractHeap::ObjectFields, AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields,
        AbstractHeap::Elements, AbstractHeap::ArrayStorage, AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage,
        AbstractHeap::NamedProperties, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards,
    };
    static constexpr CellFact facts[] {
        CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::NoExtraProperties, CellFact::HasLayoutPrototype, CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed, CellFact::EnumeratorIsCurrent,
    };

    WeakRandom random(seed);
    NumbersOfWalkStates numbers;
    Vector<PairOfWalkStateTests> pairs(numberOfStates);
    for (PairOfWalkStateTests& pair : pairs)
        pair.state.setNumbers(numbers);
    auto object = [&] {
        return valueOfWalkStateTests(random.getUint32(numberOfObjects));
    };
    auto value = [&] {
        return random.getUint32(4) ? valueOfWalkStateTests(random.getUint32(3)) : Reference();
    };
    auto layout = [&] {
        return layoutsOfWalkStateTests[random.getUint32(numberOfLayouts)];
    };

    constexpr uint32_t holdersInRow = 12;
    constexpr uint32_t timesThroughRow = 400;
    constexpr uint32_t stepsBetweenRows = 64;
    auto goThroughRowOfHolders = [&](PairOfWalkStateTests& pair) {
        for (uint32_t number = 0; number < holdersInRow; ++number) {
            Reference base = valueOfWalkStateTests(number);
            pair.state.markEscaped(base);
            pair.lists.markEscaped(base);
            pair.state.noteShape(base, TypeTable::object);
            pair.lists.noteShape(base, TypeTable::object);
        }
        for (uint32_t time = 0; time < timesThroughRow; ++time) {
            pair.state.noteVariable(VariableID(0u), valueOfWalkStateTests(time % 3));
            pair.state.invalidate(effectsThatWriteInWalkStateTests(AbstractHeap::ModuleVariables));
        }
        pair.lists.invalidate(AbstractHeap::ModuleVariables);
    };

    for (uint32_t step = 0; step < numberOfSteps; ++step) {
        size_t place = random.getUint32(numberOfStates);
        PairOfWalkStateTests& pair = pairs[place];
        if (numberOfObjects >= holdersInRow && !(step % stepsBetweenRows))
            goThroughRowOfHolders(pair);
        switch (random.getUint32(31)) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4: {
            Reference base = object();
            FieldReference field = fieldOfWalkStateTests(layout(), random.getUint32(numberOfFields));
            Reference noted = value();
            pair.state.noteSlot(base, field, noted);
            pair.lists.note(Kind::Slot, base, field.layout.index(), field.field, noted);
            break;
        }
        case 5:
        case 6: {
            Reference base = object();
            FieldReference field = fieldOfWalkStateTests(layout(), random.getUint32(numberOfFields));
            Reference noted = value();
            pair.state.noteEnvironmentField(base, field, noted);
            pair.lists.note(Kind::EnvironmentField, base, field.layout.index(), field.field, noted);
            break;
        }
        case 7:
        case 8: {
            Reference base = object();
            Reference index = valueOfWalkStateTests(random.getUint32(numberOfFields));
            Reference noted = value();
            pair.state.noteElement(base, index, noted);
            pair.lists.note(Kind::Element, base, index.bits(), 0, noted);
            break;
        }
        case 9:
        case 10: {
            uint32_t variable = random.getUint32(numberOfObjects);
            Reference noted = value();
            pair.state.noteVariable(VariableID(variable), noted);
            pair.lists.note(Kind::Variable, Reference(), variable, 0, noted);
            break;
        }
        case 11:
        case 12: {
            Reference base = object();
            TypeID shape = shapes[random.getUint32(std::size(shapes))];
            pair.state.noteShape(base, shape);
            pair.lists.noteShape(base, shape);
            break;
        }
        case 13:
        case 14:
        case 15: {
            Reference base = object();
            pair.state.noteFresh(base);
            pair.lists.noteFresh(base);
            break;
        }
        case 16:
        case 17: {
            Reference base = object();
            pair.state.markEscaped(base);
            pair.lists.markEscaped(base);
            break;
        }
        case 18: {
            Reference base = object();
            FieldReference field = fieldOfWalkStateTests(layout(), random.getUint32(numberOfFields));
            auto mayBeSameObject = [](Reference other) {
                return !(other.id().index() % 3);
            };
            auto isOfLayoutAlone = [](Reference named, LayoutID asked) {
                return named.id().index() % 3 && !((named.id().index() + asked.index()) % 2);
            };
            pair.state.invalidateAliasedSlots(base, field, scopedLambda<bool(Reference)>(mayBeSameObject), scopedLambda<bool(Reference, LayoutID)>(isOfLayoutAlone));
            pair.lists.invalidateAliasedSlots(base, field, mayBeSameObject);
            break;
        }
        case 19:
        case 20: {
            AbstractHeapSet written { heaps[random.getUint32(std::size(heaps))], heaps[random.getUint32(std::size(heaps))] };
            pair.state.invalidate(effectsThatWriteInWalkStateTests(written));
            pair.lists.invalidate(written);
            break;
        }
        case 21:
            if (random.getUint32(4))
                break;
            pair.state.invalidateHeap();
            pair.lists.invalidateHeap();
            break;
        case 22: {
            size_t from = random.getUint32(numberOfStates);
            if (from == place)
                break;
            if (random.getUint32(2)) {
                pair.state = pairs[from].state;
                pair.lists = pairs[from].lists;
            } else {
                pair.state = WTF::move(pairs[from].state);
                pair.lists = std::exchange(pairs[from].lists, { });
                pairs[from].state.clear();
            }
            break;
        }
        case 23: {
            size_t other = random.getUint32(numberOfStates);
            pair.state.meetWith(pairs[other].state);
            if (other != place)
                pair.lists.meetWith(pairs[other].lists);
            break;
        }
        case 24:
        case 25: {
            Reference base = object();
            OptionSet<CellFact> noted { facts[random.getUint32(std::size(facts))], facts[random.getUint32(std::size(facts))] };
            pair.state.recordCellFacts(base, noted);
            pair.lists.noteFacts(base, noted);
            break;
        }
        case 26: {
            Reference base = object();
            std::optional<uint32_t> orderWord = random.getUint32(4) ? std::optional<uint32_t>(random.getUint32(3)) : std::nullopt;
            pair.state.noteOrderWord(base, orderWord);
            pair.lists.noteOrderWord(base, orderWord);
            break;
        }
        case 27: {
            uint32_t classLayout = classesOfWalkStateTests[random.getUint32(std::size(classesOfWalkStateTests))];
            uint32_t key = random.getUint32(numberOfFields);
            Reference test = valueOfWalkStateTests(random.getUint32(3));
            switch (random.getUint32(3)) {
            case 0:
                pair.state.noteStoresPassAbove(LayoutID(classLayout), PropertyKeyID(key), test);
                pair.lists.note(Kind::GuardOfStores, Reference(), classLayout, key, test);
                break;
            case 1:
                pair.state.noteGuardOfReadsHolds(LayoutID(classLayout), PropertyKeyID(key), test);
                pair.lists.note(Kind::GuardOfReads, Reference(), classLayout, key, test);
                break;
            case 2:
                pair.state.noteThatGuardsHaveBegun(LayoutID(classLayout));
                pair.lists.noteThatGuardsHaveBegun(classLayout);
                break;
            }
            break;
        }
        case 28:
        case 29:
        case 30: {
            AbstractHeapSet written { heaps[random.getUint32(std::size(heaps))], heaps[random.getUint32(std::size(heaps))] };
            uint32_t rest = random.getUint32(3);
            auto mayBeWritten = [&](Reference holder) {
                return holder.id().index() % 3 != rest;
            };
            auto mayEndGuard = [&](LayoutID classOfGuard) {
                return classOfGuard.index() % 3 != rest;
            };
            Reference rootWritten = object();
            auto findClassOf = [&](Reference named) -> WalkState::ClassOfHolder {
                uint32_t number = named.id().index();
                if (!(number % 2))
                    return { };
                return { LayoutID(layoutsOfWalkStateTests[number % numberOfLayouts]), number % 4 == 1 };
            };
            WalkState::ClassOfHolder classOfRoot = random.getUint32(2) ? findClassOf(rootWritten) : WalkState::ClassOfHolder();
            LayoutID layoutOfObjectWritten = classOfRoot.layoutAlone;
            auto isOfLayoutAlone = [&](Reference named, LayoutID asked) {
                return named.id().index() % 2 && asked == LayoutID(layoutsOfWalkStateTests[named.id().index() % numberOfLayouts]);
            };
            auto askedOfHolders = scopedLambda<bool(Reference, LayoutID)>(isOfLayoutAlone);
            auto mayBeWrittenByWriter = [&](Reference holder) {
                if (layoutOfObjectWritten && holder.id().index() % 2 && layoutOfObjectWritten != LayoutID(layoutsOfWalkStateTests[holder.id().index() % numberOfLayouts]))
                    return false;
                if (layoutOfObjectWritten && classOfRoot.isNewCellItself && findClassOf(holder).isNewCellItself && holder != rootWritten)
                    return false;
                return mayBeWritten(holder);
            };
            auto askedForClass = scopedLambda<WalkState::ClassOfHolder(Reference)>(findClassOf);
            WalkState::WriterOfOneObject writer { rootWritten, classOfRoot, askedOfHolders, askedForClass };
            pair.state.invalidate(effectsThatWriteInWalkStateTests(written), scopedLambda<bool(Reference)>(mayBeWrittenByWriter), scopedLambda<bool(LayoutID, PropertyKeyID)>([&](LayoutID classOfGuard, PropertyKeyID) { return mayEndGuard(classOfGuard); }), nullptr, layoutOfObjectWritten ? &writer : nullptr);
            pair.lists.invalidate(written, mayBeWrittenByWriter, mayEndGuard);
            break;
        }
        }
        if (!(step % stepsBetweenChecks))
            checkSameAnswersInWalkStateTests(pair, numberOfObjects, numberOfFields);
    }
    for (const PairOfWalkStateTests& pair : pairs)
        checkSameAnswersInWalkStateTests(pair, numberOfObjects, numberOfFields);
}

static void testAnswersAsPlainListsDo()
{
    for (unsigned seed = 1; seed <= 8; ++seed) {
        runRandomStepsInWalkStateTests(seed, 4, 2000, 1);
        runRandomStepsInWalkStateTests(seed, 12, 3000, 10);
        runRandomStepsInWalkStateTests(seed, 100, 10000, 500);
        runRandomStepsInWalkStateTests(seed, 400, 40000, 200);
    }
}

static void testObjectsThatLeaveCostNoMoreBesideManyVariables()
{
    constexpr uint32_t numberOfStatements = 600000;
    WalkState state;
    for (uint32_t statement = 0; statement < numberOfStatements; ++statement) {
        state.noteVariable(VariableID(statement), valueOfWalkStateTests(statement));
        Reference object = valueOfWalkStateTests(numberOfStatements + statement);
        state.noteShape(object, TypeTable::object);
        state.noteSlot(object, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(1));
        state.noteFresh(object);
        state.markEscaped(object);
        state.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::ObjectFields, AbstractHeap::ShapeWords }));
        AOT_TEST_CHECK(!state.shapeOf(object) && !state.slot(object, fieldOfWalkStateTests(0, 0)));
        AOT_TEST_CHECK(state.variable(VariableID(statement / 2)) == valueOfWalkStateTests(statement / 2));
    }
    for (uint32_t statement = 0; statement < numberOfStatements; ++statement)
        AOT_TEST_CHECK(state.variable(VariableID(statement)) == valueOfWalkStateTests(statement));
}

static void testWhatIsBelievedOfFreshHoldersIsHeldAgainstWhatArrives()
{
    Reference kept = valueOfWalkStateTests(1);
    Reference scope = valueOfWalkStateTests(2);
    Reference known = valueOfWalkStateTests(3);
    WalkState believed;
    for (Reference object : { kept, scope }) {
        believed.noteShape(object, TypeTable::object);
        believed.noteFresh(object);
    }
    believed.noteSlot(kept, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(10));
    believed.noteElement(kept, valueOfWalkStateTests(20), valueOfWalkStateTests(11));
    believed.noteEnvironmentField(scope, fieldOfWalkStateTests(1, 2), valueOfWalkStateTests(12));
    believed.noteSlot(known, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(13));

    struct Lacking {
        Reference holder;
        std::optional<FieldReference> fieldOfScope;
        Reference believed;
    };
    auto lackingIn = [&](const WalkState& arriving) {
        Vector<Lacking> lacking;
        believed.forEachBeliefOfFreshHolderLackingIn(arriving, [&](Reference holder, std::optional<FieldReference> fieldOfScope, Reference value) {
            lacking.append(Lacking { holder, fieldOfScope, value });
        });
        return lacking;
    };

    WalkState same = believed;
    AOT_TEST_CHECK(lackingIn(same).isEmpty());
    same.noteSlot(known, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(14));
    same.noteSlot(kept, fieldOfWalkStateTests(0, 1), valueOfWalkStateTests(15));
    AOT_TEST_CHECK(lackingIn(same).isEmpty());

    WalkState stored = believed;
    stored.noteSlot(kept, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(16));
    Vector<Lacking> slot = lackingIn(stored);
    AOT_TEST_CHECK(slot.size() == 1 && slot[0].holder == kept && !slot[0].fieldOfScope && slot[0].believed == valueOfWalkStateTests(10));

    WalkState laid = believed;
    laid.noteElement(kept, valueOfWalkStateTests(20), valueOfWalkStateTests(17));
    Vector<Lacking> element = lackingIn(laid);
    AOT_TEST_CHECK(element.size() == 1 && element[0].holder == kept && element[0].believed == valueOfWalkStateTests(11));

    WalkState assigned = believed;
    assigned.noteEnvironmentField(scope, fieldOfWalkStateTests(1, 2), valueOfWalkStateTests(18));
    Vector<Lacking> field = lackingIn(assigned);
    AOT_TEST_CHECK(field.size() == 1 && field[0].holder == scope && field[0].fieldOfScope && field[0].fieldOfScope->field == 2 && field[0].believed == valueOfWalkStateTests(12));

    WalkState left = believed;
    left.markEscaped(kept);
    Vector<Lacking> cell = lackingIn(left);
    AOT_TEST_CHECK(cell.size() == 1 && cell[0].holder == kept && !cell[0].believed);

    AOT_TEST_CHECK(lackingIn(WalkState()).size() == 2);
}

static void testOneChangeAmongManyFreshHoldersIsFoundInWhatIsNotShared()
{
    constexpr uint32_t numberOfHolders = 400;
    constexpr uint32_t changed = 211;
    WalkState believed;
    for (uint32_t number = 0; number < numberOfHolders; ++number) {
        Reference object = valueOfWalkStateTests(number);
        believed.noteShape(object, TypeTable::object);
        believed.noteFresh(object);
        believed.recordCellFacts(object, { CellFact::NotFrozen });
        believed.noteSlot(object, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(numberOfHolders + number));
    }
    Reference object = valueOfWalkStateTests(changed);
    auto lackingIn = [&](const WalkState& arriving) {
        Vector<std::pair<Reference, Reference>> lacking;
        believed.forEachBeliefOfFreshHolderLackingIn(arriving, [&](Reference holder, std::optional<FieldReference>, Reference value) {
            lacking.append({ holder, value });
        });
        return lacking;
    };
    auto onlyChangedHolderIsLacking = [&](const WalkState& arriving, Reference value) {
        auto lacking = lackingIn(arriving);
        return lacking.size() == 1 && lacking[0].first == object && lacking[0].second == value;
    };

    WalkState same = believed;
    same.noteSlot(valueOfWalkStateTests(2 * numberOfHolders + 1), fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(1));
    AOT_TEST_CHECK(lackingIn(same).isEmpty());

    WalkState stored = believed;
    stored.noteSlot(object, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(7));
    AOT_TEST_CHECK(onlyChangedHolderIsLacking(stored, valueOfWalkStateTests(numberOfHolders + changed)));

    WalkState left = believed;
    left.markEscaped(object);
    AOT_TEST_CHECK(onlyChangedHolderIsLacking(left, Reference()));

    WalkState shaped = believed;
    shaped.noteShape(object, TypeTable::function);
    AOT_TEST_CHECK(onlyChangedHolderIsLacking(shaped, Reference()));

    WalkState frozen = believed;
    frozen.invalidateCellFacts(object, { CellFact::NotFrozen });
    AOT_TEST_CHECK(onlyChangedHolderIsLacking(frozen, Reference()));
}

static void testListTakesRoomOfFormerListAndNothingThatItHeld()
{
    Vector<uint32_t> former;
    for (uint32_t number = 0; number < 1000; ++number)
        former.append(number);
    const uint32_t* room = former.span().data();
    size_t capacity = former.capacity();
    Vector<uint32_t> list;
    takeRoom(list, former);
    AOT_TEST_CHECK(list.isEmpty() && list.capacity() == capacity && former.isEmpty() && !former.capacity());
    list.append(7);
    AOT_TEST_CHECK(list.span().data() == room);

    Vector<uint32_t, 4> formerInItsOwnRoom { 1, 2, 3 };
    Vector<uint32_t, 4> listInItsOwnRoom;
    takeRoom(listInItsOwnRoom, formerInItsOwnRoom);
    AOT_TEST_CHECK(listInItsOwnRoom.isEmpty() && listInItsOwnRoom.capacity() == 4);
}

static void testRecordsKeepTheirSegmentsWhereUnitIsForgotten()
{
    StableArray<uint32_t, 4> records;
    for (uint32_t number = 0; number < 40; ++number)
        records.append(number);
    const uint32_t* first = &records[0];
    records.forgetUnit();
    AOT_TEST_CHECK(records.isEmpty());
#if ASSERT_ENABLED
    AOT_TEST_CHECK(*reinterpret_cast<const uint8_t*>(first) == byteOfRoomThatIsTaken);
#endif
    records.append(7);
    AOT_TEST_CHECK(&records[0] == first && records[0] == 7);
    for (uint32_t number = 1; number < 40; ++number)
        records.append(number);
    for (uint32_t number = 1; number < 40; ++number)
        AOT_TEST_CHECK(records[number] == number);
}

static void testRecordsTakeFewerAddressesThanProcessHas()
{
    StableArray<uint64_t, 44> records;
    StableArray<uint64_t, 44> others;
    for (uint64_t number = 0; number < 100000; ++number) {
        records.append(number);
        others.append(~number);
    }
    const uint64_t* first = &records[0];
    for (uint64_t number = 100000; number < 200000; ++number)
        records.append(number);
    AOT_TEST_CHECK(&records[0] == first);
    for (uint64_t number = 0; number < 100000; ++number)
        AOT_TEST_CHECK(records[number] == number && records[number + 100000] == number + 100000 && others[number] == ~number);
}

static void testSideWordsKeepSegmentsOfUsualSizeAlone()
{
    SideWords words;
    uint32_t* inLargeSegment = words.allocate(1u << 15);
    uint32_t* inUsualSegment = words.allocate(8);
    AOT_TEST_CHECK(inLargeSegment != inUsualSegment);
    words.forgetUnit();
    AOT_TEST_CHECK(words.allocate(8) == inUsualSegment);
}

static void testElementsOutsideViewsOutliveWritesIntoViews()
{
    Reference list = valueOfWalkStateTests(1);
    Reference other = valueOfWalkStateTests(2);
    Reference key = valueOfWalkStateTests(20);
    auto stateBehind = [&](AbstractHeapSet written) {
        WalkState state;
        state.noteElement(list, key, valueOfWalkStateTests(10));
        state.noteElementsLieOutsideViews(list);
        state.noteElement(other, key, valueOfWalkStateTests(11));
        WalkState shared = state;
        state.invalidate(effectsThatWriteInWalkStateTests(written));
        AOT_TEST_CHECK(shared.element(list, key) == valueOfWalkStateTests(10) && shared.element(other, key) == valueOfWalkStateTests(11));
        return state;
    };
    for (AbstractHeapSet written : { AbstractHeapSet { AbstractHeap::TypedArrayElements }, AbstractHeapSet { AbstractHeap::TypedArrayStorage }, AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::TypedArrayStorage, AbstractHeap::ObjectFields } }) {
        WalkState state = stateBehind(written);
        AOT_TEST_CHECK(state.element(list, key) == valueOfWalkStateTests(10));
        AOT_TEST_CHECK(!state.element(other, key));
    }
    for (AbstractHeapSet written : { AbstractHeapSet { AbstractHeap::Elements }, AbstractHeapSet { AbstractHeap::ArrayStorage }, AbstractHeapSet { AbstractHeap::TypedArrayElements, AbstractHeap::Elements } }) {
        WalkState state = stateBehind(written);
        AOT_TEST_CHECK(!state.element(list, key));
        AOT_TEST_CHECK(!state.element(other, key));
    }

    for (bool otherBearsMark : { false, true }) {
        WalkState one;
        one.noteElement(list, key, valueOfWalkStateTests(10));
        one.noteElementsLieOutsideViews(list);
        WalkState arriving;
        arriving.noteElement(list, key, valueOfWalkStateTests(10));
        if (otherBearsMark)
            arriving.noteElementsLieOutsideViews(list);
        one.meetWith(arriving);
        AOT_TEST_CHECK(one.element(list, key) == valueOfWalkStateTests(10));
        one.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::TypedArrayElements }));
        AOT_TEST_CHECK(!!one.element(list, key) == otherBearsMark);
    }

    {
        WalkState revived;
        revived.noteSlot(other, fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(1));
        revived.noteElement(list, key, valueOfWalkStateTests(10));
        revived.noteElementsLieOutsideViews(list);
        revived.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::Elements }));
        AOT_TEST_CHECK(!revived.element(list, key));
        revived.noteElement(list, key, valueOfWalkStateTests(12));
        revived.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::TypedArrayElements }));
        AOT_TEST_CHECK(!revived.element(list, key));
    }

    constexpr uint32_t numberOfHolders = 96;
    WalkState large;
    auto holderAt = [](uint32_t place) { return valueOfWalkStateTests(1000 + place); };
    auto noteAll = [&] {
        for (uint32_t place = 0; place < numberOfHolders; ++place) {
            large.noteSlot(holderAt(place), fieldOfWalkStateTests(0, 0), valueOfWalkStateTests(1));
            large.noteElement(holderAt(place), key, valueOfWalkStateTests(2000 + place));
            if (place & 1)
                large.noteElementsLieOutsideViews(holderAt(place));
        }
    };
    for (uint32_t round = 0; round < 4000; ++round) {
        noteAll();
        large.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::TypedArrayElements }));
        for (uint32_t place = 0; place < numberOfHolders; ++place)
            AOT_TEST_CHECK((large.element(holderAt(place), key) == valueOfWalkStateTests(2000 + place)) == !!(place & 1) && (!!large.element(holderAt(place), key)) == !!(place & 1));
        if (round & 1) {
            large.invalidate(effectsThatWriteInWalkStateTests({ AbstractHeap::Elements }));
            for (uint32_t place = 0; place < numberOfHolders; ++place)
                AOT_TEST_CHECK(!large.element(holderAt(place), key));
        }
    }
}

std::span<const NativeTest> walkStateTests()
{
    static constexpr NativeTest tests[] {
        { "walk-state/answers-as-plain-lists-do"_s, testAnswersAsPlainListsDo },
        { "walk-state/objects-that-leave-cost-no-more-beside-many-variables"_s, testObjectsThatLeaveCostNoMoreBesideManyVariables },
        { "walk-state/what-is-believed-of-fresh-holders-is-held-against-what-arrives"_s, testWhatIsBelievedOfFreshHoldersIsHeldAgainstWhatArrives },
        { "walk-state/one-change-among-many-fresh-holders-is-found-in-what-is-not-shared"_s, testOneChangeAmongManyFreshHoldersIsFoundInWhatIsNotShared },
        { "walk-state/a-list-takes-the-room-of-a-former-list-and-nothing-that-it-held"_s, testListTakesRoomOfFormerListAndNothingThatItHeld },
        { "walk-state/records-keep-their-segments-where-a-unit-is-forgotten"_s, testRecordsKeepTheirSegmentsWhereUnitIsForgotten },
        { "walk-state/records-take-fewer-addresses-than-a-process-has"_s, testRecordsTakeFewerAddressesThanProcessHas },
        { "walk-state/side-words-keep-segments-of-the-usual-size-alone"_s, testSideWordsKeepSegmentsOfUsualSizeAlone },
        { "walk-state/elements-outside-views-outlive-writes-into-views"_s, testElementsOutsideViewsOutliveWritesIntoViews },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
