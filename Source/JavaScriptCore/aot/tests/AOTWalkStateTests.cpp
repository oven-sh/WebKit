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
        OptionSet<FactOfCell> facts;
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

    OptionSet<FactOfCell> factsOf(Reference object) const
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
            if (cell.object == object && cell.facts.contains(FactOfCell::OrderWordIsKnown))
                return cell.orderWord;
        }
        return std::nullopt;
    }

    void noteFacts(Reference object, OptionSet<FactOfCell> facts)
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
            endFactsWhere(FactOfCell::OrderWordIsKnown, [&](Reference other) {
                return other == object;
            });
            return;
        }
        noteFacts(object, { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::OrderWordIsKnown });
        for (Cell& cell : cells) {
            if (cell.object == object)
                cell.orderWord = *orderWord;
        }
    }

    void endFactsWhere(OptionSet<FactOfCell> ended, NOESCAPE const Invocable<bool(Reference)> auto& isMeant)
    {
        for (Cell& cell : cells) {
            if (isMeant(cell.object))
                cell.facts.remove(ended);
        }
        cells.removeAllMatching([](const Cell& cell) {
            return cell.facts.isEmpty();
        });
    }

    static OptionSet<FactOfCell> factsThatRestOn(AbstractHeapSet written)
    {
        OptionSet<FactOfCell> facts;
        if (written.contains(AbstractHeap::ShapeWords))
            facts.add(FactOfCell::NotFrozen);
        if (written.contains(AbstractHeap::NamedProperties))
            facts.add({ FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties });
        if (written.contains(AbstractHeap::PrototypeLinks))
            facts.add(FactOfCell::HasLayoutPrototype);
        if (written.containsAny({ AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::OrderWords }))
            facts.add({ FactOfCell::OrderWordIsKnown, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed });
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

    void noteGuard(Kind kind, uint32_t classLayout, uint32_t key, Reference test)
    {
        note(kind, Reference(), classLayout, key, test);
        noteThatGuardsHaveBegun(classLayout);
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
            if (cell.facts.contains(FactOfCell::OrderWordIsKnown) && other.orderWordOf(cell.object) != cell.orderWord)
                cell.facts.remove(FactOfCell::OrderWordIsKnown);
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
static constexpr uint32_t classesOfWalkStateTests[] { 0, 1, 64, 65, 63, UINT32_MAX >> 1 };

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
        AOT_TEST_CHECK(state.factsOfCell(object) == lists.factsOf(object));
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
    static constexpr FactOfCell facts[] {
        FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::NoExtraProperties, FactOfCell::HasLayoutPrototype, FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed,
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
            pair.state.invalidateAliasedSlots(base, field, scopedLambda<bool(Reference)>(mayBeSameObject));
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
            OptionSet<FactOfCell> noted { facts[random.getUint32(std::size(facts))], facts[random.getUint32(std::size(facts))] };
            pair.state.noteFactsOfCell(base, noted);
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
                pair.lists.noteGuard(Kind::GuardOfStores, classLayout, key, test);
                break;
            case 1:
                pair.state.noteGuardOfReadsHolds(LayoutID(classLayout), PropertyKeyID(key), test);
                pair.lists.noteGuard(Kind::GuardOfReads, classLayout, key, test);
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
            pair.state.invalidate(effectsThatWriteInWalkStateTests(written), scopedLambda<bool(Reference)>(mayBeWritten), scopedLambda<bool(LayoutID)>(mayEndGuard));
            pair.lists.invalidate(written, mayBeWritten, mayEndGuard);
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

std::span<const NativeTest> walkStateTests()
{
    static constexpr NativeTest tests[] {
        { "walk-state/answers-as-plain-lists-do"_s, testAnswersAsPlainListsDo },
        { "walk-state/objects-that-leave-cost-no-more-beside-many-variables"_s, testObjectsThatLeaveCostNoMoreBesideManyVariables },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
