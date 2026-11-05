/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTLiteralStrings.h"
#include "AOTModuleEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include "JSTypeInfo.h"
#include <bit>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// The numbers of literals that lie less than a window's width above the first of them. Bit n stands for the number `first + n`.
struct LiteralStringWindow {
    uint32_t first;
    uint32_t last;
    uint64_t bits;
};

static constexpr uint32_t numbersPerLiteralStringWindow = 64;
// A word below that of the first number wraps to a distance past the window, in a narrow word too.
static_assert(literalStringWord(maxLiteralStrings) <= UINT32_MAX - (numbersPerLiteralStringWindow << literalStringShift));

static bool onlyStringsOccur(const TypeTestPlan& plan)
{
    return plan.objects == ObjectsVerdict::DoNotOccur && !(plan.occurringPrimitives() & cellPrimitives & ~Primitive::AnyString);
}

void TypeTestEmitter::forEachCellCase(CellPointer cell, const TypeTestPlan& plan, const auto& visit)
{
    static constexpr std::pair<uint16_t, JSType> primitives[] = { { Primitive::AnySymbol, SymbolType }, { Primitive::BigInt, HeapBigIntType } };

    const Type from = types().at(plan.from);
    auto visitCellTest = [&](const CellTest& test) {
        // An object without the mark pays nothing for the mark: one case asks the word as it lies, the next lets marked objects pass.
        if (test.kind == CellTestKind::ShapeIn && m_casesAreBranchedOn && emitter<ShapeTestEmitter>().paysForMark(test.layouts, from)) {
            visit([&] {
                m_nextCaseIsUnlikely = true;
                return passesCellTest(cell, test, from, MarkedObjects::AreAskedAgain);
            });
        }
        visit([&] {
            return passesCellTest(cell, test, from);
        });
    };

    if (plan.passingPrimitives & Primitive::AnyString) {
        visit([&] {
            return stringPasses(cell, plan);
        });
    }
    for (const auto& primitive : primitives) {
        if (plan.passingPrimitives & primitive.first) {
            visit([&] {
                return hasCellType(cell, primitive.second);
            });
        }
    }
    for (const CellTest& test : plan.cellTests) {
        if (test.kind == CellTestKind::IsSymbol)
            visitCellTest(test);
    }

    switch (plan.objects) {
    case ObjectsVerdict::DoNotOccur:
    case ObjectsVerdict::AllFail:
        return;
    case ObjectsVerdict::AllPass:
        visit([&] {
            return plan.occurringPrimitives() & cellPrimitives ? isObjectCell(cell) : boolean(true);
        });
        return;
    case ObjectsVerdict::NeedTest:
        break;
    }

    Vector<FunctionID, 8> functions;
    for (const CellTest& test : plan.cellTests) {
        if (test.kind == CellTestKind::IsClosureOf)
            functions.append(test.function);
    }
    bool testsClosureNumber = functions.size() > 1;
    if (testsClosureNumber) {
        visit([&] {
            Machine::DeclaredAccessesSuspension suspension(m_out);
            return emitter<ClosureTestEmitter>().isClosureOfAny(cell, functions.span(), from);
        });
    }

    for (const CellTest& test : plan.cellTests) {
        if (test.kind == CellTestKind::IsSymbol || (testsClosureNumber && test.kind == CellTestKind::IsClosureOf))
            continue;
        if (test.kind != CellTestKind::IsCallable) {
            visitCellTest(test);
            continue;
        }
        visit([&] {
            return emitter<ClosureTestEmitter>().isSlimClosureAsCreated(cell);
        });
        visit([&] {
            return emitter<ClosureTestEmitter>().hasFunctionCellType(cell);
        });
        visit([&] {
            return isCallableByCallData(cell);
        });
    }
}

std::optional<Condition> TypeTestEmitter::tryGetSingleCondition(CellPointer cell, const TypeTestPlan& plan)
{
    if (plan.everyCellPasses())
        return boolean(true);

    unsigned numberOfCases = 0;
    forEachCellCase(cell, plan, [&](const auto&) {
        ++numberOfCases;
    });
    if (numberOfCases > 1)
        return std::nullopt;
    return cellPasses(cell, plan);
}

void TypeTestEmitter::testCell(CellPointer cell, const TypeTestPlan& plan, BlockEdge failure)
{
    if (plan.everyCellPasses())
        return;
    testCases(failure, [&](const auto& visit) {
        forEachCellCase(cell, plan, visit);
    });
}

Condition TypeTestEmitter::cellPasses(CellPointer cell, const TypeTestPlan& plan)
{
    SetForScope casesAreBranchedOn(m_casesAreBranchedOn, false);
    Condition result;
    forEachCellCase(cell, plan, [&](const auto& emitTest) {
        result = logicalOr(result, emitTest());
    });
    return result ? result : boolean(false);
}

Condition TypeTestEmitter::stringPasses(CellPointer cell, const TypeTestPlan& plan)
{
    auto isOneOfLiterals = [&] {
        return isOneOfLiteralStringWords(emitter<ConversionEmitter>().toLiteralString(cell), plan);
    };
    if (plan.stringLiterals.isEmpty())
        return hasCellType(cell, StringType);
    if (onlyStringsOccur(plan))
        return isOneOfLiterals();
    return either<Condition>(hasCellType(cell, StringType), isOneOfLiterals, [&] {
        return boolean(false);
    });
}

Condition TypeTestEmitter::passesCellTest(CellPointer cell, const CellTest& test, const Type& from, MarkedObjects marked)
{
    // What a cell is never changes, so a type test declares no read. The words that say what it is do change.
    Machine::DeclaredAccessesSuspension suspension(m_out);
    switch (test.kind) {
    case CellTestKind::IsObject:
        return isObjectCell(cell);
    case CellTestKind::IsCallable:
        return isCallable(cell);
    case CellTestKind::CellTypeIs:
        return hasCellTypeIn(cell, test.cellTypes);
    case CellTestKind::ShapeIn: {
        Condition hasShape = emitter<ShapeTestEmitter>().hasShapeIn(cell, test.layouts, from, marked);
        if (!test.asksForInstance)
            return hasShape;
        return logicalAnd(hasShape, emitter<GuardEmitter>().belongsToInstance(cell));
    }
    case CellTestKind::InheritsClassInfo:
        return inheritsClassInfo(cell, test.builtinClass);
    case CellTestKind::IsPristine: {
        Condition isInstance = emitBoolean(cell, planFor(TypeTable::cell, Representation::Cell, types().builtinClass(test.builtinClass)));
        return logicalAnd(isInstance, isPristine(cell, test.builtinClass, { PristineAspects::Storage, PristineAspects::Names }));
    }
    // The not-taken edge removes the tested cell from the type, so the test creates a cell that does not exist yet.
    case CellTestKind::IsBuiltinCell:
        if (!isTestableBuiltin(m_builtins, test.builtin))
            return boolean(false);
        return equal(cell, emitter<ConstantEmitter>().builtinCell(test.builtin));
    case CellTestKind::IsClosureOf:
        return emitter<ClosureTestEmitter>().isClosureOf(cell, test.function, from);
    case CellTestKind::IsSymbol:
        return equal(cell, emitter<ConstantEmitter>().keyConstant(test.symbol));
    case CellTestKind::SharingModeIs:
        return hasSharingMode(cell, test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return isNamespaceOf(cell, test.module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A buffer is made shared before it gets its wrapper, and a shared buffer is never detached.
Condition TypeTestEmitter::hasSharingMode(CellPointer cell, ArrayBufferSharingMode sharingMode)
{
    return selectByBranch<Condition>(hasCellType(cell, ArrayBufferType), [&] {
        Pointer buffer = loadField<Pointer>(cell, m_heaps.JSArrayBuffer_impl);
        Pointer sharedContents = loadField<Pointer>(buffer, m_heaps.ArrayBuffer_shared);
        ensureStillAliveHere(cell);
        switch (sharingMode) {
        case ArrayBufferSharingMode::Default:
            return equal(sharedContents, pointerConstant(0));
        case ArrayBufferSharingMode::Shared:
            return notEqual(sharedContents, pointerConstant(0));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }, [&] {
        return boolean(false);
    });
}

Condition TypeTestEmitter::isNamespaceOf(CellPointer cell, ModuleID module)
{
    ModuleEmitter& modules = emitter<ModuleEmitter>();
    if (modules.hasSingleNamespaceInValueWord(module))
        return equal(boxCell(cell), modules.loadModuleValue(module));

    // The cell type of a namespace object never changes. A cell that leaves ObjectType is refused by the call, whichever type was read.
    Condition mayBeNamespace = hasOrHadCellType(cell, ObjectType);
    return selectByBranch<Condition>(mayBeNamespace, [&] {
        return callLeaf<Condition>(RuntimeOperation::IsNamespaceOfModule, { cell, word32(static_cast<int32_t>(module.index())) });
    }, [&] {
        return boolean(false);
    });
}

Condition TypeTestEmitter::isCallable(CellPointer cell)
{
    return selectByBranch<Condition>(emitter<ClosureTestEmitter>().isSlimClosureAsCreated(cell), [&] {
        return boolean(true);
    }, [&] {
        return selectByBranch<Condition>(emitter<ClosureTestEmitter>().hasFunctionCellType(cell), [&] {
            return boolean(true);
        }, [&] {
            return isCallableByCallData(cell);
        });
    });
}

Condition TypeTestEmitter::isCallableByCallData(CellPointer cell)
{
    return either<Condition>(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), OverridesGetCallData), [&] {
        return boolean(false);
    }, [&] {
        return callWithoutSite<Condition>(RuntimeOperation::IsCallable, { cell }, { .isOnRarePath = true });
    });
}

Condition TypeTestEmitter::inheritsClassInfo(CellPointer cell, BuiltinClassID classID)
{
    LValue classInfos = loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.instanceClassInfos)).value();
    LValue expected = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, classInfos, static_cast<ptrdiff_t>(classID.index()) * sizeof(const ClassInfo*)));

    LBasicBlock loop = newBlock();
    LBasicBlock parentClass = newBlock();
    LBasicBlock continuation = newBlock();

    ValueFromBlock firstClassInfo = m_out.anchor(m_out.loadPtr(loadStructure(cell).value(), m_heaps.Structure_classInfo));
    m_out.jump(loop);

    m_out.appendTo(loop);
    LValue classInfo = m_out.phi(Machine::pointerType(), firstClassInfo);
    ValueFromBlock found = m_out.anchor(boolean(true).value());
    m_out.branch(m_out.equal(classInfo, expected), normalEdge(continuation), normalEdge(parentClass));

    m_out.appendTo(parentClass);
    LValue parent = m_out.loadPtr(classInfo, m_heaps.ClassInfo_parentClass);
    m_out.addIncomingToPhi(classInfo, m_out.anchor(parent));
    ValueFromBlock notFound = m_out.anchor(boolean(false).value());
    m_out.branch(m_out.isNull(parent), normalEdge(continuation), normalEdge(loop));

    m_out.appendTo(continuation);
    return Condition(m_out.phi(Machine::Int32, found, notFound));
}

Condition TypeTestEmitter::isPristine(CellPointer cell, BuiltinClassID classID, OptionSet<PristineAspects> aspects)
{
    if (aspects.containsAny(aspectsWithoutTest))
        return boolean(false);
    switch (pristineTestOf(m_builtins, classID)) {
    case PristineTest::NeverPasses:
        return boolean(false);
    case PristineTest::MayPass:
        if (classID == BuiltinClasses::Array)
            return emitter<ElementAccessEmitter>().isPristineArray(cell, aspects);
        // A view answers for every number by itself: no accessor lies among its elements, and what stands above it is never asked. Structure::tryMarkPristine() gives it the bit of the names alone.
        if (isTypedView(m_builtins.typedArrayTypeOf(classID)))
            aspects.remove(PristineAspects::Storage);
        if (aspects.contains(PristineAspects::NamesKeptBelow)) {
            BuiltinClassID below = classBelowWithTestedStructure(m_builtins, classID);
            RELEASE_ASSERT(below);
            OptionSet<PristineAspects> aspectsOfEither = aspects - PristineAspects::NamesKeptBelow;
            aspectsOfEither.add(PristineAspects::Names);
            Condition isPristineArray = emitter<ElementAccessEmitter>().isPristineInstance(cell, aspectsOfEither);
            return logicalOr(isPristineArray, isPristine(cell, below, aspectsOfEither));
        }
        return emitter<ElementAccessEmitter>().isPristineInstance(cell, aspects);
    case PristineTest::ClosureAsCreated:
        return emitter<GuardEmitter>().isClosureAsCreated(cell, classID, aspects);
    case PristineTest::InstanceStructure: {
        if (aspects.contains(PristineAspects::Storage))
            return boolean(false);
        std::optional<Receiver> receiver = receiverWithTestedStructure(classID);
        RELEASE_ASSERT(receiver);
        Word32 structureID = loadStructureID(cell);
        // The word is zero until the realm makes the class, and keeps the number that it then gets. No cell has the number zero, so a word that is read too early fails the test.
        return equal(structureID, loadFilledOnce32(Instance::offsetOfReceiverStructureID(*receiver)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A marker lies below the word of every number and a cell above it, and no word lies between those of two neighboring numbers.
Condition TypeTestEmitter::isInLiteralStringWindows(LValue word, std::span<const LiteralStringWindow> ascendingWindows)
{
    if (ascendingWindows.size() > 1) {
        size_t middle = ascendingWindows.size() / 2;
        return selectByBranch<Condition>(Condition(m_out.below(word, literalStringWord(ascendingWindows[middle].first))), [&] {
            return isInLiteralStringWindows(word, ascendingWindows.first(middle));
        }, [&] {
            return isInLiteralStringWindows(word, ascendingWindows.subspan(middle));
        });
    }

    const LiteralStringWindow& window = ascendingWindows.front();
    if (window.first == window.last)
        return Condition(m_out.equal(word, literalStringWord(window.first)));

    uint32_t count = window.last - window.first + 1;
    LValue distance = m_out.sub(word, literalStringWord(window.first));
    if (window.bits == UINT64_MAX >> (numbersPerLiteralStringWindow - count))
        return Condition(m_out.below(distance, count << literalStringShift));

    // The machine reads the low bits of an amount alone, so a number outside the window finds the bit of another.
    LValue index = m_out.lShr(distance, literalStringShift);
    LValue isInWindow = m_out.below(index, count);
    LValue amount = m_out.castToInt32(index);
    LValue shiftedBits = count <= 32
        ? m_out.lShr(m_out.constInt32(static_cast<int32_t>(window.bits)), amount)
        : m_out.castToInt32(m_out.lShr(m_out.constInt64(static_cast<int64_t>(window.bits)), amount));
    return Condition(m_out.bitAnd(shiftedBits, isInWindow));
}

Condition TypeTestEmitter::isOneOfLiteralStringWords(LiteralStringWord word, const TypeTestPlan& plan)
{
    // Two literals or more are the cases of a switch, unless four or more of their numbers lie within one window: then each number is a bit of a constant.
    static constexpr size_t minLiteralsForSwitch = 2;
    static constexpr int minLiteralsForBitTest = 4;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Vector<uint32_t, 8> numbers;
    for (StringID literal : plan.stringLiterals) {
        if (std::optional<uint32_t> number = literalStrings().indexOf(literal))
            numbers.append(*number);
    }
    AOT::sort(numbers);
    Vector<LiteralStringWindow, 4> windows;
    bool testsBits = false;
    for (uint32_t number : numbers) {
        if (windows.isEmpty() || number - windows.last().first >= numbersPerLiteralStringWindow)
            windows.append({ number, number, 0 });
        LiteralStringWindow& window = windows.last();
        window.last = number;
        window.bits |= 1ull << (number - window.first);
        testsBits |= std::popcount(window.bits) >= minLiteralsForBitTest;
    }
    bool usesSwitch = !testsBits && numbers.size() >= minLiteralsForSwitch;
    Condition result;
    if (testsBits) {
        std::optional<NarrowReference> narrow = narrowWordBehind(word);
        result = isInLiteralStringWindows(narrow ? narrow->value() : word.value(), windows.span());
    }
    if (usesSwitch) {
        result = asBoolean([&](BranchTargets targets) {
            Vector<SwitchTarget, 8> cases;
            for (uint32_t number : numbers)
                cases.append({ static_cast<int64_t>(literalStringWord(number)), targets.taken });
            m_out.switchOn(word.value(), cases.span(), targets.notTaken);
        });
    }
    for (StringID literal : plan.stringLiterals) {
        if (std::optional<uint32_t> number = literalStrings().indexOf(literal)) {
            if (!testsBits && !usesSwitch) {
                std::optional<NarrowReference> narrow = narrowWordBehind(word);
                result = logicalOr(result, narrow ? equal(*narrow, NarrowReference(m_out.constInt32(static_cast<int32_t>(literalStringWord(*number))))) : equal(word, conversions.literalStringConstant(literal)));
            }
            continue;
        }
        result = logicalOr(result, either<Condition>(conversions.isStringCell(word), [&] {
            return callWithoutSite<Condition>(RuntimeOperation::StringEqualsLiteral, { CellPointer(word.value()), numberOf(m_mentions.add(NumberSpace::String, literal)) });
        }, [&] {
            return boolean(false);
        }));
    }
    return result ? result : boolean(false);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
