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
#include "AOTElementAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTModuleEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include "JSArrayBufferView.h"
#include "JSTypeInfo.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool onlyStringsOccur(const TypeTestPlan& plan)
{
    return plan.objects == ObjectsVerdict::DoNotOccur && !(plan.occurringPrimitives() & cellPrimitives & ~Primitive::AnyString);
}

void TypeTestEmitter::forEachCellCase(CellPointer cell, const TypeTestPlan& plan, const auto& visit)
{
    static constexpr std::pair<uint16_t, JSType> primitives[] = { { Primitive::AnySymbol, SymbolType }, { Primitive::BigInt, HeapBigIntType } };

    const Type from = types().at(plan.from);
    auto visitCellTest = [&](const CellTest& test) {
        MarkedObjects marked = MarkedObjects::Pass;
        if (test.kind == CellTestKind::ShapeIn && m_casesAreBranchedOn && emitter<ShapeTestEmitter>().paysForMark(test.layouts, from)) {
            visit([&] {
                m_nextCaseIsUnlikely = true;
                return passesCellTest(cell, test, from, MarkedObjects::AreAskedAgain);
            });
            marked = MarkedObjects::PassAlone;
        }
        visit([&] {
            return passesCellTest(cell, test, from, marked);
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
    bool testsClosureNumber = functions.size() > 1 && !m_isEmittingPerFunctionBranch;
    if (testsClosureNumber) {
        visit([&] {
            Machine::DeclaredAccessesSuspension suspension(m_out);
            return emitter<ClosureTestEmitter>().isClosureOfAny(cell, functions.span());
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
        if (asksStubForCallable()) {
            visit([&] {
                return isCallableByStub(cell);
            });
            continue;
        }
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
        Condition result;
        for (StringID literal : plan.stringLiterals)
            result = logicalOr(result, callWithoutSite<Condition>(RuntimeOperation::StringEqualsLiteral, { cell, numberOf(m_mentions.add(NumberSpace::String, literal)) }));
        return result;
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
    case CellTestKind::IsBuiltinCell:
        if (!isTestableBuiltin(m_builtins, test.builtin))
            return boolean(false);
        return equal(cell, emitter<ConstantEmitter>().builtinCell(test.builtin));
    case CellTestKind::IsClosureOf:
        return emitter<ClosureTestEmitter>().isClosureOf(cell, test.function);
    case CellTestKind::IsSymbol:
        return equal(cell, emitter<ConstantEmitter>().keyConstant(test.symbol));
    case CellTestKind::SharingModeIs:
        return hasSharingMode(cell, test.sharingMode);
    case CellTestKind::IsNamespaceOf:
        return isNamespaceOf(cell, test.module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
        if (asksStubForCallable())
            return isCallableByStub(cell);
        return selectByBranch<Condition>(emitter<ClosureTestEmitter>().hasFunctionCellType(cell), [&] {
            return boolean(true);
        }, [&] {
            return isCallableByCallData(cell);
        });
    });
}

bool TypeTestEmitter::asksStubForCallable()
{
    const OperationContext* context = generator().contextBeingGenerated();
    return context && context->block() && !generator().runsRepeatedly(context->block());
}

Condition TypeTestEmitter::isCallableByStub(CellPointer cell)
{
    return asBoolean([&](BranchTargets targets) {
        LValue operand = cell.value();
        emitter<RuntimeCallEmitter>().callStubWithoutSiteAndBranch(isCallableCell(), singleElementSpan(operand), { }, targets);
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
    bool asksCellAlone = aspects.contains(PristineAspects::CellAlone);
    aspects.remove(PristineAspects::CellAlone);
    Condition isAsMade = isAsItsClassMadeIt(cell, classID, aspects);
    if (asksCellAlone || !m_program.admitsReplacedBuiltins || !aspects.containsAny({ PristineAspects::Names, PristineAspects::InheritedNamesKept }))
        return isAsMade;
    return logicalAnd(isAsMade, nothingWasWrittenAbove(classID));
}

void TypeTestEmitter::branchOnPristineView(CellPointer cell, BuiltinClassID classID, OptionSet<PristineAspects> aspects, BranchTargets targets)
{
    auto continueIfTrue = [&](Condition condition) {
        LBasicBlock passed = newBlock();
        branch(condition, { normalEdge(passed), targets.notTaken });
        m_out.appendTo(passed);
    };

    bool mayKeepInheritedNames = aspects.contains(PristineAspects::InheritedNamesKept) && !aspects.containsAny(aspectsWithoutTest) && pristineTestOf(m_builtins, classID) == PristineTest::MayPass;
    if (!mayKeepInheritedNames)
        continueIfTrue(isPristine(cell, classID, aspects));
    else {
        bool asksCellAlone = aspects.contains(PristineAspects::CellAlone);
        OptionSet<PristineAspects> aspectsOfEither = aspects - OptionSet<PristineAspects> { PristineAspects::InheritedNamesKept, PristineAspects::Storage, PristineAspects::CellAlone };
        aspectsOfEither.add(PristineAspects::Names);
        BuiltinClassID below = classBelowWithTestedStructure(m_builtins, classID);
        RELEASE_ASSERT(below);
        LBasicBlock hasNames = newBlock();
        LBasicBlock mayBeOfClassBelow = newBlock();
        branch(emitter<ElementAccessEmitter>().isPristineInstance(cell, aspectsOfEither), { normalEdge(hasNames), unlikelyEdge(mayBeOfClassBelow) });
        m_out.appendTo(mayBeOfClassBelow);
        branch(isPristine(cell, below, aspectsOfEither), { normalEdge(hasNames), targets.notTaken });
        m_out.appendTo(hasNames);
        if (!asksCellAlone && m_program.admitsReplacedBuiltins)
            continueIfTrue(nothingWasWrittenAbove(classID));
    }
    branch(hasNoBits(loadByte(cell, m_heaps.JSArrayBufferView_sharingOfMemory), isGrowableSharedMode), targets);
}

Condition TypeTestEmitter::nothingWasWrittenAbove(BuiltinClassID classID)
{
    if (standsAboveClassesOfViews(m_builtins, classID)) {
        Condition nothing = boolean(true);
        for (uint32_t index = 0; index < m_builtins.classCount(); ++index) {
            if (m_builtins.at(BuiltinClassID(index)).parent == classID && isTypedView(m_builtins.typedArrayTypeOf(BuiltinClassID(index))))
                nothing = logicalAnd(nothing, nothingWasWrittenAbove(BuiltinClassID(index)));
        }
        return nothing;
    }
    Pointer classesWrittenAbove = loadImmutablePointer(AOT_INSTANCE_OFFSET(builtins.classesWrittenAbove));
    Word32 wasWrittenAbove(m_out.load8ZeroExt32(m_out.address(m_heaps.entire(AbstractHeap::NamedProperties), classesWrittenAbove.value(), classID.index())));
    return isZero(wasWrittenAbove);
}

Condition TypeTestEmitter::isAsItsClassMadeIt(CellPointer cell, BuiltinClassID classID, OptionSet<PristineAspects> aspects)
{
    if (aspects.containsAny(aspectsWithoutTest))
        return boolean(false);
    if (standsAboveClassesOfViews(m_builtins, classID)) {
        aspects.remove(PristineAspects::Storage);
        return aspects.contains(PristineAspects::InheritedNamesKept) ? boolean(false) : emitter<ElementAccessEmitter>().isPristineInstance(cell, aspects);
    }
    switch (pristineTestOf(m_builtins, classID)) {
    case PristineTest::NeverPasses:
        return boolean(false);
    case PristineTest::MayPass:
        if (classID == BuiltinClasses::Array)
            return emitter<ElementAccessEmitter>().isPristineArray(cell, aspects);
        if (isTypedView(m_builtins.typedArrayTypeOf(classID)))
            aspects.remove(PristineAspects::Storage);
        if (aspects.contains(PristineAspects::InheritedNamesKept)) {
            BuiltinClassID below = classBelowWithTestedStructure(m_builtins, classID);
            RELEASE_ASSERT(below);
            OptionSet<PristineAspects> aspectsOfEither = aspects - PristineAspects::InheritedNamesKept;
            aspectsOfEither.add(PristineAspects::Names);
            return selectByBranch<Condition>(emitter<ElementAccessEmitter>().isPristineInstance(cell, aspectsOfEither), [&] {
                return boolean(true);
            }, [&] {
                return isPristine(cell, below, aspectsOfEither);
            });
        }
        if (aspects.contains(PristineAspects::Names) && pristineFlagOfCellCoversElements(m_builtins.at(classID).type))
            aspects.remove(PristineAspects::Storage);
        return emitter<ElementAccessEmitter>().isPristineInstance(cell, aspects);
    case PristineTest::ClosureAsCreated:
        return emitter<GuardEmitter>().isClosureAsCreated(cell, classID, aspects);
    case PristineTest::InstanceStructure: {
        if (aspects.contains(PristineAspects::Storage))
            return boolean(false);
        std::optional<Receiver> receiver = receiverWithTestedStructure(classID);
        RELEASE_ASSERT(receiver);
        Word32 structureID = loadStructureID(cell);
        return equal(structureID, loadFilledOnce32(Instance::offsetOfReceiverStructureID(*receiver)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
