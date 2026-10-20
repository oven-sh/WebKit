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
#include "AOTIdentifierNumbering.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPlacement.h"
#include "AOTPureOperations.h"
#include "AOTShapeTestEmitter.h"
#include "AOTTypeRelations.h"
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

    for (const CellTest& test : plan.cellTests) {
        if (test.kind == CellTestKind::IsSymbol)
            continue;
        if (test.kind != CellTestKind::IsCallable) {
            visitCellTest(test);
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
        return emitter<ShapeTestEmitter>().hasShapeIn(cell, test.layouts, from);
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
    Condition mayBeNamespace = equal(loadCellType(cell), word32(ObjectType));
    return selectByBranch<Condition>(mayBeNamespace, [&] {
        return callLeaf<Condition>(RuntimeOperation::IsNamespaceOfModule, { cell, word32(static_cast<int32_t>(module.index())) });
    }, [&] {
        return boolean(false);
    });
}

Condition TypeTestEmitter::isCallable(CellPointer cell)
{
    return selectByBranch<Condition>(emitter<ClosureTestEmitter>().hasFunctionCellType(cell), [&] {
        return boolean(true);
    }, [&] {
        return isCallableByCallData(cell);
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
        return emitter<ElementAccessEmitter>().isPristineArray(cell, aspects);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition TypeTestEmitter::isOneOfStringLiterals(CellPointer string, const TypeTestPlan& plan)
{
    return isOneOfLiteralStringWords(emitter<ConversionEmitter>().toLiteralString(string), plan);
}

Condition TypeTestEmitter::isOneOfLiteralStringWords(LiteralStringWord word, const TypeTestPlan& plan)
{
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Condition result;
    for (StringID literal : plan.stringLiterals) {
        if (inputs().literalStrings.indexOf(literal)) {
            result = logicalOr(result, equal(word, conversions.literalStringConstant(literal)));
            continue;
        }
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
