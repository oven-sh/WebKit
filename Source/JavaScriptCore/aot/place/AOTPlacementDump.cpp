/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacement.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include "AOTPlacementTestCommands.h"
#include "AOTSchemaWalkers.h"
#include "AOTVector.h"
#include <algorithm>
#include <cinttypes>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(FieldAccess::Coverage coverage)
{
    switch (coverage) {
    case FieldAccess::Coverage::NoMember:
        return "NoMember"_s;
    case FieldAccess::Coverage::SomeMembers:
        return "SomeMembers"_s;
    case FieldAccess::Coverage::EveryMemberHasSlot:
        return "EveryMemberHasSlot"_s;
    case FieldAccess::Coverage::EveryMember:
        return "EveryMember"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(StaticsRegion region)
{
    switch (region) {
    case StaticsRegion::FilledAtInstanceCreation:
        return "FilledAtInstanceCreation"_s;
    case StaticsRegion::FilledAtProgramEntry:
        return "FilledAtProgramEntry"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(CallableKind kind)
{
    switch (kind) {
    case CallableKind::Plain:
        return "Plain"_s;
    case CallableKind::Arrow:
        return "Arrow"_s;
    case CallableKind::Method:
        return "Method"_s;
    case CallableKind::Bound:
        return "Bound"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(MemberGuardKind kind)
{
    switch (kind) {
    case MemberGuardKind::Member:
        return "Member"_s;
    case MemberGuardKind::PrototypeLink:
        return "PrototypeLink"_s;
    case MemberGuardKind::EvaluatedOnce:
        return "EvaluatedOnce"_s;
    case MemberGuardKind::InstancePrototype:
        return "InstancePrototype"_s;
    case MemberGuardKind::NotHidden:
        return "NotHidden"_s;
    case MemberGuardKind::PassedByStores:
        return "PassedByStores"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameOf(BuiltinCellKey::Kind kind)
{
    switch (kind) {
    case BuiltinCellKey::Kind::Object:
        return "Object"_s;
    case BuiltinCellKey::Kind::Getter:
        return "Getter"_s;
    case BuiltinCellKey::Kind::Setter:
        return "Setter"_s;
    case BuiltinCellKey::Kind::Value:
        return "Value"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void printShapeTest(PrintStream& out, const ShapeTest& test)
{
    switch (test.form) {
    case ShapeTestForm::NeverPasses:
        out.print("never"_s);
        return;
    case ShapeTestForm::AlwaysPasses:
        out.print("always"_s);
        return;
    case ShapeTestForm::OneShape:
        out.print("one "_s, test.first);
        return;
    case ShapeTestForm::UpTo:
        out.print("upto "_s, test.last());
        return;
    case ShapeTestForm::From:
        out.print("from "_s, test.first);
        return;
    case ShapeTestForm::Range:
        out.print("range "_s, test.first, ".."_s, test.last());
        return;
    case ShapeTestForm::Block:
        out.printf("block 0x%02x", test.blockByte);
        return;
    case ShapeTestForm::BlockOrEarlier:
        out.printf("block>= 0x%02x", test.blockByte);
        return;
    case ShapeTestForm::ImmediateBits:
        out.print("bits "_s, test.first, ".."_s, test.last());
        out.printf(" 0x%" PRIx64, test.bits);
        if (test.bitSetOffset != noBitSet)
            out.print(" @"_s, test.bitSetOffset);
        return;
    case ShapeTestForm::BitSet:
        out.print("bitset "_s, test.first, ".."_s, test.last());
        if (test.bitSetOffset != noBitSet)
            out.print(" @"_s, test.bitSetOffset);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector collectSetsNamedBySites(const Program& program)
{
    BitVector sets(program.types.numberOfLayoutSets());
    auto addSet = [&](LayoutSetID set) {
        sets.set(set.index());
    };
    auto addSetOfType = [&](TypeID type) {
        if (LayoutSetID set = program.at(type).layouts)
            sets.set(set.index());
    };
    for (const Function& function : program.functions) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                forEachLayoutSet(function.at(id), addSet);
                forEachType(function.at(id), addSetOfType);
            }
            forEachLayoutSet(block.terminator, addSet);
            forEachType(block.terminator, addSetOfType);
        }
    }
    return sets;
}

void printRequestRecord(IRPrinter& printer, const RequestRecord& request)
{
    PrintStream& out = printer.out();
    WTF::switchOn(request,
        [&](const NewClassRecord& record) {
            out.print("NewClass "_s);
            printer.print(record.classLayout);
        },
        [&](const CloneAsRecord& record) {
            out.print("CloneAs "_s);
            printer.print(record.layout);
            out.print(" shape="_s, record.shape, " fields="_s);
            printList(out, record.fields, [&](FieldIndex field) {
                out.print(field);
            });
        },
        [&](const TemplateObjectRecord& record) {
            out.print("TemplateObject "_s);
            printList(out, record.parts, [&](const TemplateString& part) {
                printer.print(part.cooked);
                out.print("/"_s);
                printer.print(part.raw);
            });
        },
        [&](const DirectEvalRecord& record) {
            out.print("DirectEval "_s);
            printer.print(record.scope);
            out.print(" "_s);
            printer.print(record.module);
        },
        [&](const DataRecord& record) {
            out.print("Data "_s);
            printer.print(record.data);
            if (record.sharedStorageOffset)
                out.print(" sharedStorage@"_s, *record.sharedStorageOffset);
            if (record.node)
                out.print(" node@"_s, *record.node);
        },
        [&](const CollectionDataRecord& record) {
            out.print("CollectionData "_s);
            printer.print(record.data);
            if (record.hasValues)
                out.print(" hasValues"_s);
        });
}

namespace {

class Dumper {
    WTF_MAKE_NONCOPYABLE(Dumper);
public:
    Dumper(PrintStream& out, const Program& program, const Placement& placement, unsigned numberOfFunctions)
        : m_out(out)
        , m_program(program)
        , m_placement(placement)
        , m_numberOfFunctions(numberOfFunctions)
        , m_printer(out, program)
    {
    }

    void run()
    {
        dumpShapes();
        dumpFieldAccesses();
        dumpShapeTests();
        dumpAllocations();
        dumpKeyOrders();
        dumpVariables();
        dumpGuardBits();
        dumpStatics();
        dumpFunctions();
        dumpConstants();
        dumpRequests();
        dumpPropertyRuns();
        m_placement.globalBindings().dump(m_out, m_program);
    }

private:
    void printFlag(bool isSet, ASCIILiteral name)
    {
        if (isSet)
            m_out.print(" "_s, name);
    }

    void printFields(std::span<const FieldIndex> fields)
    {
        printList(m_out, fields, [&](FieldIndex field) {
            m_out.print(field);
        });
    }

    void printSet(LayoutSetID id)
    {
        if (!id) {
            m_out.print("-"_s);
            return;
        }
        const LayoutSet& set = m_program.at(id);
        CommaPrinter blank(" "_s);
        m_out.print("{"_s);
        for (LayoutID member : set.members) {
            m_out.print(blank);
            m_printer.print(member);
        }
        if (!set.isComplete)
            m_out.print(blank, ".."_s);
        m_out.print("}"_s);
    }

    void dumpShapes()
    {
        const ShapeTable& shapes = m_placement.shapes();
        for (uint32_t number = 1; number <= shapes.numberOfShapes(); ++number) {
            if (!shapes.isAssigned(number))
                continue;
            const PlacedShape& shape = shapes.shape(number);
            m_out.print("shape "_s, number, " "_s);
            m_printer.print(shape.layout);
            printFlag(shape.keysAreNeverEnumerated, "unordered"_s);
            if (!shape.impossibleMutations.isEmpty()) {
                m_out.print(" impossible="_s);
                printList(m_out, shape.impossibleMutations, [&](ObjectMutation mutation) {
                    m_out.print(nameOf(mutation));
                });
            }
            m_out.print("\n"_s);
        }
        if (shapes.numberOfShapes())
            m_out.print("environments from "_s, shapes.firstEnvironmentShape(), "\n"_s);
    }

    void dumpFieldAccesses()
    {
        for (unsigned index = 0; index < m_placement.numberOfFieldAccesses(); ++index) {
            const FieldAccess& access = m_placement.fieldAccess(FieldAccessID(index));
            m_out.print("access "_s, index, " "_s);
            m_printer.print(access.key);
            m_out.print(" "_s);
            printSet(access.receivers);
            m_out.print(access.isStore ? " store "_s : " read "_s, nameOf(access.coverage), " "_s);
            if (access.slot.hasStorage())
                printSlot(m_out, access.slot);
            else
                m_out.print("-"_s);
            if (access.slot.needsExtension()) {
                m_out.print(" extension="_s);
                printSlot(m_out, access.extensionWord);
            }
            if (access.membersWithField && access.membersWithField != access.receivers) {
                m_out.print(" with="_s);
                printSet(access.membersWithField);
            }
            m_out.print(" test="_s);
            printShapeTest(m_out, access.membersWithFieldTest);
            m_out.print(" type="_s);
            m_printer.print(access.contents);
            m_out.print("\n"_s);
        }
    }

    void dumpShapeTests()
    {
        const ShapeTable& shapes = m_placement.shapes();
        for (size_t index : collectSetsNamedBySites(m_program)) {
            LayoutSetID set(index);
            if (!shapes.hasTestOf(set))
                continue;
            m_out.print("set "_s);
            printSet(set);
            m_out.print(" "_s);
            printShapeTest(m_out, shapes.testOf(set));
            m_out.print("\n"_s);
        }
        for (const ShapeTestWithinKnownSet& pair : shapes.testsWithinKnownSets()) {
            m_out.print("set "_s);
            printSet(pair.tested);
            m_out.print(" within "_s);
            printSet(pair.known);
            m_out.print(" "_s);
            printShapeTest(m_out, pair.test);
            m_out.print("\n"_s);
        }
    }

    void dumpAllocations()
    {
        for (unsigned index = 0; index < m_placement.numberOfAllocations(); ++index) {
            const PlacedAllocation& allocation = m_placement.allocation(AllocationID(index));
            m_out.print("allocation "_s, index, " "_s);
            m_printer.print(allocation.layout);
            m_out.print(" shape="_s, allocation.shape, " fields="_s);
            printFields(allocation.initialFields.span());
            m_out.print(" order="_s, orderOf(allocation.orderWord), " count="_s, countOf(allocation.orderWord), " size="_s, allocation.size);
            if (allocation.holdsInnerObjects)
                m_out.print(" holdsInnerObjects"_s);
            if (allocation.numberOfCaptures)
                m_out.print(" captures="_s, allocation.numberOfCaptures);
            if (allocation.constructedBy) {
                m_out.print(" constructedBy="_s);
                m_printer.print(allocation.constructedBy);
            }
            m_out.print("\n"_s);
        }
    }

    void dumpKeyOrders()
    {
        const KeyOrders& keyOrders = m_placement.keyOrders();
        for (unsigned index = 0; index < keyOrders.ofLayout.size(); ++index) {
            LayoutID layout(index);
            std::span<const Vector<FieldIndex>> orders = keyOrders.of(layout);
            for (size_t order = 0; order < orders.size(); ++order) {
                m_out.print("order "_s);
                m_printer.print(layout);
                m_out.print(" "_s, order, ": "_s);
                printFields(orders[order].span());
                m_out.print("\n"_s);
            }
        }
    }

    void dumpVariableInStatics(VariableID id, const PlacedVariable& variable)
    {
        m_printer.print(id);
        m_out.print(" offset="_s, variable.offset, " representation="_s, nameOf(variable.representation), " encoding="_s, nameOf(variable.encoding), " region="_s, nameOf(variable.region));
        if (variable.function) {
            m_out.print(" function="_s);
            m_printer.print(variable.function);
        }
        if (variable.initialWord)
            m_out.printf(" initial=0x%016" PRIx64, variable.initialWord);
        m_out.print("\n"_s);
    }

    void dumpVariables()
    {
        const Vector<PlacedVariable>& variables = m_placement.statics().variables;
        for (unsigned index = 0; index < variables.size(); ++index) {
            const PlacedVariable& variable = variables[index];
            switch (variable.storage) {
            case VariableStorage::None:
                continue;
            case VariableStorage::Statics:
                dumpVariableInStatics(VariableID(index), variable);
                continue;
            case VariableStorage::ClosureTable:
                m_printer.print(VariableID(index));
                m_out.print(" closureTable function="_s);
                m_printer.print(variable.function);
                m_out.print("\n"_s);
                continue;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void dumpGuardBits()
    {
        const GuardBitTable& table = m_placement.guardBits();
        const Vector<PlacedGuardBit>& bits = table.bits;
        for (unsigned index = 0; index < bits.size(); ++index) {
            const PlacedGuardBit& guard = bits[index];
            m_out.print("guard "_s, index, " "_s);
            if (guard.classLayout) {
                m_printer.print(guard.classLayout);
                m_out.print(" "_s, nameOf(guard.side), " "_s, nameOf(guard.kind));
                if (guard.key) {
                    m_out.print(" "_s);
                    m_printer.print(guard.key);
                }
            } else {
                m_out.print("set "_s);
                printList(m_out, table.membersOf(GuardBitID(index)), [&](GuardBitID member) {
                    m_out.print(member.index());
                });
            }
            m_out.print(" bit="_s, guard.bit, " word@"_s, guard.wordOffset, guard.holdsWhileClear ? " holdsWhileClear"_s : ""_s, "\n"_s);
        }
    }

    void dumpStatics()
    {
        const StaticsPlan& statics = m_placement.statics();
        m_out.print("statics size="_s, statics.size, " filledAtEntry="_s, statics.sizeFilledAtProgramEntry, " roots="_s, statics.roots.size(), " cleared="_s);
        printList(m_out, statics.clearedEachCollection, [&](const StaticsRange& range) {
            m_out.print(range.offset, "+"_s, range.size);
        });
        if (statics.distanceFromArrayProfileToLastArray)
            m_out.print(" lastArrayDistance="_s, statics.distanceFromArrayProfileToLastArray);
        m_out.print("\n"_s);

        for (unsigned index = 0; index < statics.moduleValueOffsets.size(); ++index) {
            if (!statics.moduleValueOffsets[index])
                continue;
            m_out.print("moduleValue "_s);
            m_printer.print(ModuleID(index));
            m_out.print(" @"_s, *statics.moduleValueOffsets[index], "\n"_s);
        }
        for (const ModuleBuiltinCell& cell : statics.moduleBuiltinCells) {
            m_out.print("moduleBuiltinCell "_s);
            m_printer.print(cell.builtin);
            m_out.print(" "_s);
            m_printer.print(cell.module);
            m_out.print(" @"_s, cell.offset, "\n"_s);
        }
    }

    void dumpFunctions()
    {
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            FunctionKindData kind = m_placement.functionKind(FunctionID(index));
            m_printer.print(FunctionID(index));
            if (kind.closureStructure)
                m_out.print(" closureStructure="_s, static_cast<unsigned>(kind.closureStructure));
            printFlag(kind.isSlim, "isSlim"_s);
            printFlag(kind.isAsync, "isAsync"_s);
            printFlag(kind.isGenerator, "isGenerator"_s);
            printFlag(kind.takesNewTarget, "takesNewTarget"_s);
            printFlag(kind.isCommonJSModule, "isCommonJSModule"_s);
            printFlag(kind.isInRuntimeModule, "isInRuntimeModule"_s);
            if (kind.accessor() != MethodKind::Method)
                m_out.print(" accessorKind="_s, nameOf(kind.accessor()));
            printFlag(kind.isConstructor, "isConstructor"_s);
            if (kind.callable() != CallableKind::Plain)
                m_out.print(" callableKind="_s, nameOf(kind.callable()));
            printFlag(kind.isDerivedConstructor, "isDerivedConstructor"_s);
            printFlag(kind.hasInferredName, "hasInferredName"_s);
            printFlag(m_placement.closureIdentityOf(FunctionID(index)) == ClosureIdentity::TableCell, "hasOnlyTableCell"_s);
            if (LayoutID constructedClass = m_placement.classConstructedBy(FunctionID(index))) {
                m_out.print(" classConstructedBy="_s);
                m_printer.print(constructedClass);
            }
            m_out.print("\n"_s);
        }

        ClosureFunctionRange closureFunctions = m_placement.closureFunctions();
        if (!closureFunctions.first)
            return;
        m_out.print("closureFunctions first="_s, closureFunctions.first.index(), " end="_s, closureFunctions.end.index(), " omittedWords="_s, closureFunctions.omittedWordCount, "\n"_s);
    }

    void dumpConstants()
    {
        std::span<const BuiltinCellKey> builtinCells = m_placement.builtinCells();
        for (size_t index = 0; index < builtinCells.size(); ++index) {
            m_out.print("builtinCell "_s, index, " "_s, nameOf(builtinCells[index].kind()), " "_s);
            m_printer.print(builtinCells[index].builtin());
            if (std::ranges::binary_search(m_placement.eagerBuiltinCells(), index))
                m_out.print(" eager"_s);
            m_out.print("\n"_s);
        }

        std::span<const StringID> bigIntConstants = m_placement.bigIntConstants();
        for (size_t index = 0; index < bigIntConstants.size(); ++index) {
            m_out.print("bigInt "_s, index, " "_s);
            m_printer.print(bigIntConstants[index]);
            m_out.print("\n"_s);
        }

        std::span<const RequireEdge> requireEdges = m_placement.requireEdges();
        std::span<const StringID> specifiers = m_placement.requireEdgeSpecifiers();
        for (size_t index = 0; index < requireEdges.size(); ++index) {
            m_out.print("requireEdge "_s, index, " "_s);
            m_printer.print(requireEdges[index].referrer);
            m_out.print(" "_s);
            m_printer.print(requireEdges[index].target);
            m_out.print(" "_s);
            m_printer.print(index < specifiers.size() ? specifiers[index] : StringID());
            m_out.print("\n"_s);
        }
    }

    void dumpRequests()
    {
        const RequestTable& requests = m_placement.requests();
        std::span<const RequestRecord> records = requests.records();
        for (unsigned index = 0; index < records.size(); ++index) {
            m_out.print("request @"_s, requests.offsetOf(index), " "_s);
            printRequestRecord(m_printer, records[index]);
            m_out.print("\n"_s);
        }
        if (!records.empty())
            m_out.print("requests size="_s, requests.sizeInBytes(), " shared="_s, requests.numberOfSharedRequests(), "\n"_s);
    }

    void dumpPropertyRuns()
    {
        std::span<const PlacedPropertyRun> runs = m_placement.propertyRuns().runs();
        for (size_t index = 0; index < runs.size(); ++index) {
            const PlacedPropertyRun& run = runs[index];
            m_out.print("propertyRun "_s, index, " "_s);
            printList(m_out, run.keys, [&](PropertyKeyID key) {
                m_printer.print(key);
            });
            m_out.print("\n"_s);
        }
    }

    PrintStream& m_out;
    const Program& m_program;
    const Placement& m_placement;
    unsigned m_numberOfFunctions;
    IRPrinter m_printer;
};

} // anonymous namespace

void Placement::dump(PrintStream& out, const Program& program) const
{
    Dumper(out, program, *this, m_functions.kinds.size()).run();
}

void SiteCaches::dump(PrintStream& out, const Program& program) const
{
    out.print("site caches "_s, m_count, ", without "_s, m_numberOfSitesWithoutCache, "\n"_s);
    IRPrinter printer(out, program);
    for (unsigned functionIndex = 0; functionIndex < program.functions.size(); ++functionIndex) {
        const Function& function = program.functions[functionIndex];
        for (unsigned valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            const Value& value = function.values[valueIndex];
            if (!Value::opcodesWithSiteCache.contains(value.opcode))
                continue;
            SiteCacheID cache = value.siteCache();
            if (!cache)
                continue;
            printer.print(FunctionID(functionIndex));
            out.print(" "_s);
            printer.print(ValueID(valueIndex));
            out.print(" "_s, nameOf(value.opcode), " cache="_s, cache.index(), "\n"_s);
        }
    }
}

void GlobalBindingTable::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (unsigned index = 0; index < bindings.size(); ++index) {
        const PlacedGlobalBinding& binding = bindings[index];
        out.print("globalBinding "_s, index, " "_s);
        printer.print(binding.key.key);
        out.print(binding.key.isStore ? " store"_s : " read"_s);
        if (binding.key.seesScriptBindings)
            out.print(" seesScriptBindings"_s);
        if (binding.key.isForTypeOf)
            out.print(" isForTypeOf"_s);
        if (binding.isStoredByName)
            out.print(" isStoredByName"_s);
        out.print("\n"_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
