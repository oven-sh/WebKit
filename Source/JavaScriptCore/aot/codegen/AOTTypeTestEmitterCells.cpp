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
#include "AOTIdentifierNumbering.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPlacement.h"
#include "AOTPureOperations.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubsValues.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
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

    const Type& from = m_program.at(plan.from);
    auto visitCellTest = [&](const CellTest& test) {
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
    bool testsFunctionIndex = functions.size() > 1;
    if (testsFunctionIndex) {
        visit([&] {
            return emitter<ClosureTestEmitter>().isClosureOfAny(cell, functions.span());
        });
    }

    for (const CellTest& test : plan.cellTests) {
        if (test.kind == CellTestKind::IsSymbol || (testsFunctionIndex && test.kind == CellTestKind::IsClosureOf))
            continue;
        if (test.kind != CellTestKind::IsCallable) {
            visitCellTest(test);
            continue;
        }
        if (asksStubWhetherCallable()) {
            visit([&] {
                return isCallableByStub(cell.value());
            });
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

void TypeTestEmitter::testCell(CellPointer cell, const TypeTestPlan& plan, const Failure& failure)
{
    if (plan.everyCellPasses())
        return;
    testCases(failure, [&](const auto& visit) {
        forEachCellCase(cell, plan, visit);
    });
}

Condition TypeTestEmitter::cellPasses(CellPointer cell, const TypeTestPlan& plan)
{
    Condition result;
    forEachCellCase(cell, plan, [&](const auto& emitTest) {
        result = logicalOr(result, emitTest());
    });
    return result ? result : boolean(false);
}

Condition TypeTestEmitter::stringPasses(CellPointer cell, const TypeTestPlan& plan)
{
    if (plan.stringLiterals.isEmpty())
        return hasCellType(cell, StringType);
    if (onlyStringsOccur(plan))
        return isOneOfStringLiterals(cell, plan);
    return either<Condition>(hasCellType(cell, StringType), [&] {
        return isOneOfStringLiterals(cell, plan);
    }, [&] {
        return boolean(false);
    });
}

Condition TypeTestEmitter::passesCellTest(CellPointer cell, const CellTest& test, const Type& from)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        return isObjectCell(cell);
    case CellTestKind::IsCallable:
        return isCallable(cell);
    case CellTestKind::CellTypeIs:
        return hasCellTypeIn(cell, test.cellTypes);
    case CellTestKind::ShapeIn:
        return emitter<ShapeTestEmitter>().hasLayoutIn(cell, test.layouts, from);
    case CellTestKind::InheritsClassInfo:
        return inheritsClassInfo(cell, test.builtinClass);
    case CellTestKind::IsPristine: {
        Condition isInstance = cellPasses(cell, computeTypeTestPlanFromAnything(m_program, Type::builtinClass(test.builtinClass)));
        return logicalAnd(isInstance, isPristine(cell, test.builtinClass, { PristineAspects::Storage, PristineAspects::Names }));
    }
    // The not-taken edge removes the tested cell from the type, so the test creates a cell that does not exist yet.
    case CellTestKind::IsBuiltinCell:
        if (!isTestableBuiltin(m_builtins, test.builtin))
            return boolean(false);
        return equal(cell, emitter<ConstantEmitter>().builtinCell(test.builtin));
    case CellTestKind::IsClosureOf:
        return emitter<ClosureTestEmitter>().isClosureOf(cell, test.function, from, m_shape);
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
        return isCallableByStub(cell.value());
    });
}

// The stub refuses what is no cell, so a test that only a callable cell passes needs no other.
bool TypeTestEmitter::isDecidedByCallableStub(const TypeTestPlan& plan) const
{
    return asksStubWhetherCallable() && plan.objects == ObjectsVerdict::NeedTest && !(plan.passingPrimitives & cellPrimitives)
        && plan.cellTests.size() == 1 && plan.cellTests[0].kind == CellTestKind::IsCallable;
}

std::optional<FunctionID> TypeTestEmitter::functionDecidedByClosureStub(const TypeTestPlan& plan)
{
    if (plan.objects != ObjectsVerdict::NeedTest || plan.passingPrimitives & cellPrimitives || plan.cellTests.size() != 1 || plan.cellTests[0].kind != CellTestKind::IsClosureOf)
        return std::nullopt;
    FunctionID function = plan.cellTests[0].function;
    if (!emitter<ClosureTestEmitter>().asksStubWhetherClosureOf(function, m_shape))
        return std::nullopt;
    return function;
}

Condition TypeTestEmitter::isCallableByStub(LValue cellOrOtherValue)
{
    return asBoolean([&](BranchTargets targets) {
        LValue operands[] = { cellOrOtherValue };
        emitter<RuntimeCallEmitter>().callStubWithoutSiteAndBranch(valueIsCallable(), operands, targets);
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
    LValue classInfo = m_out.phi(B3::pointerType(), firstClassInfo);
    ValueFromBlock found = m_out.anchor(boolean(true).value());
    m_out.branch(m_out.equal(classInfo, expected), normalEdge(continuation), normalEdge(parentClass));

    m_out.appendTo(parentClass);
    LValue parent = m_out.loadPtr(classInfo, m_heaps.ClassInfo_parentClass);
    m_out.addIncomingToPhi(classInfo, m_out.anchor(parent));
    ValueFromBlock notFound = m_out.anchor(boolean(false).value());
    m_out.branch(m_out.isNull(parent), normalEdge(continuation), normalEdge(loop));

    m_out.appendTo(continuation);
    return Condition(m_out.phi(B3::Int32, found, notFound));
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
        if (aspects.contains(PristineAspects::NamesKeptBelow)) {
            BuiltinClassID below = classBelowWithTestedStructure(m_builtins, classID);
            RELEASE_ASSERT(below);
            OptionSet<PristineAspects> aspectsOfEither = aspects - PristineAspects::NamesKeptBelow;
            aspectsOfEither.add(PristineAspects::Names);
            return logicalOr(emitter<ElementAccessEmitter>().isPristineInstance(cell, aspectsOfEither), isPristine(cell, below, aspectsOfEither));
        }
        return emitter<ElementAccessEmitter>().isPristineInstance(cell, aspects);
    case PristineTest::ClosureAsCreated:
        return emitter<GuardEmitter>().isClosureAsCreated(cell);
    case PristineTest::InstanceStructure: {
        if (aspects.contains(PristineAspects::Storage))
            return boolean(false);
        std::optional<Receiver> receiver = receiverWithTestedStructure(classID);
        RELEASE_ASSERT(receiver);
        return equal(loadStructureID(cell), loadMutable32(Instance::offsetOfReceiverStructureID(*receiver)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition TypeTestEmitter::isOneOfStringLiterals(CellPointer string, const TypeTestPlan& plan)
{
    return isOneOfLiteralStringWords(emitter<ConversionEmitter>().toLiteralString(string), plan);
}

Condition TypeTestEmitter::isOneOfLiteralStringWords(LiteralStringWord word, const TypeTestPlan& plan)
{
    // Two literals or more are the cases of a switch, which compares and branches, or tests a bit where four or more numbers lie within 64.
    AOT_THRESHOLD(size_t, minLiteralsForSwitch, 2);
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Vector<uint32_t, 8> numbers;
    for (StringID literal : plan.stringLiterals) {
        if (inputs().literalStrings.indexOf(literal))
            numbers.append(inputs().literalStrings.numberOf(literal));
    }
    bool usesSwitch = numbers.size() >= minLiteralsForSwitch;
    Condition result;
    if (usesSwitch) {
        result = asBoolean([&](BranchTargets targets) {
            bool switchesOnNumber = numbers.size() >= B3::Limits::minCasesForBitTests;
            LValue subject = word.value();
            if (switchesOnNumber)
                subject = m_out.lShr(m_out.sub(subject, m_out.constInt64(emptyLiteralStringWord)), m_out.constInt32(literalStringShift));
            Vector<SwitchTarget, 8> cases;
            for (uint32_t number : numbers)
                cases.append({ static_cast<int64_t>(switchesOnNumber ? number : literalStringWord(number)), targets.taken });
            m_out.switchOn(subject, cases.span(), targets.notTaken);
        });
    }
    for (StringID literal : plan.stringLiterals) {
        if (inputs().literalStrings.indexOf(literal)) {
            if (!usesSwitch)
                result = logicalOr(result, equal(word, conversions.literalStringConstant(literal)));
            continue;
        }
        if (inputs().literalStrings.isLeftOut(literal))
            continue;
        result = logicalOr(result, either<Condition>(conversions.isStringCell(word), [&] {
            return callWithoutSite<Condition>(RuntimeOperation::StringEqualsLiteral, { CellPointer(word.value()), word32(literal.index()) });
        }, [&] {
            return boolean(false);
        }));
    }
    return result ? result : boolean(false);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
