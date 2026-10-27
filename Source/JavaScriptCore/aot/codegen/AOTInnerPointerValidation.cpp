/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInnerPointerValidation.h"

#if ENABLE(AOT)

#include "AOTHeaps.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRegisters.h"
#include "AirCode.h"
#include "AirCustom.h"
#include "AirFrameEmission.h"
#include "AirFrameStates.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include <algorithm>
#include <limits>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT {

namespace {

using B3::Air::Arg;
using B3::Air::BasicBlock;
using B3::Air::Code;
using B3::Air::Inst;
using B3::Air::Tmp;

// A location is a general purpose register or four bytes of the frame. A value is what one instruction leaves in a location, or what a block finds in one that it reads before it writes: a head.
// A move between locations copies the value. A head exists only where the location is read later, and is one class with what the predecessors of its block leave there.
class InnerPointerValidation {
public:
    InnerPointerValidation(Code& code, const InnerPointerRules& rules, InnerPointerFindings& findings)
        : m_code(code)
        , m_rules(rules)
        , m_findings(findings)
        , m_hasFrameRecord(code.frameLayout().hasFrameRecord)
        , m_bytesBelowFrameRecord(code.bytesBelowFrameRecord())
        , m_numberOfCalleeSaves(code.calleeSaveRegisterAtOffsetList().registerCount())
    {
    }

    void run()
    {
        findSpilledWords();
        numberValues();
        if (m_definitions.isEmpty() || m_safepoints.isEmpty())
            return;
        findPredecessors();
        // As where paths meet: what is chosen is one class with what it is chosen from.
        for (auto [result, source] : m_choices)
            unite(result, source);
        // What such a load gives is owned by the cell if it turns out to be a pointer, and says nothing otherwise.
        for (const Definition& definition : m_loadsOfUnknownFields)
            addOwner(definition);
        for (const Definition& definition : m_definitions) {
            addOwner(definition);
            add(definition.pointer, IsPointer);
            if (contains(m_rules.fieldsOutsideCells.span(), definition.load->range()))
                add(definition.owner, IsPointer);
        }
        solve();
        findWhatGivesCellsLater();
        report();
    }

    uint32_t overwriteDeadValues()
    {
        m_recordsSites = true;
        if (m_code.policy().frame.emitsFrame)
            m_frameStates.emplace(m_code);
        findSpilledWords();
        numberValues();
        if (m_sites.isEmpty())
            return 0;
        findPredecessors();
        for (unsigned unit = 0; unit < m_isFollowed.size(); ++unit)
            follow(unit);
        solve();
        return overwriteAtSites();
    }

private:
    static constexpr unsigned none = std::numeric_limits<unsigned>::max();
    static constexpr unsigned clobbered = 0;
    static constexpr unsigned numberOfRegisters = MacroAssembler::numberOfRegisters();

    enum ClassFlag : uint8_t {
        IsPointer = 1 << 0,
        IsOwnerOfSuspect = 1 << 1,
        // One of its values is read behind a safepoint that it was made in front of.
        HasSuspect = 1 << 2,
        HasOrdinaryDefinition = 1 << 3,
    };

    enum MemberFlag : uint8_t {
        IsHead = 1 << 0,
        IsLiveOut = 1 << 1,
        HasFollowedCopies = 1 << 2,
        HasFollowedSums = 1 << 3,
        GivesCellsBehindBlock = 1 << 4,
    };

    struct Facts {
        unsigned parent { none };
        unsigned nextInClass { none };
        unsigned block { none };
        unsigned location { none };
        unsigned created { 0 };
        unsigned lastRead { 0 };
        // The last place in its block where something is loaded through it that may name a cell, or where it is handed on.
        unsigned lastReadThatMayGiveCells { 0 };
        unsigned firstCopy { none };
        unsigned firstSum { none };
        // What a sum is computed from, and what a choice is made from.
        unsigned firstSource { none };
        unsigned nextHeadInBlock { none };
        unsigned nextHeadOfUnit { none };
        unsigned firstOwner { none };
        unsigned lastOwner { none };
        uint8_t classFlags { 0 };
        uint8_t memberFlags { 0 };
    };

    struct Link {
        unsigned target;
        unsigned next;
    };

    struct Definition {
        unsigned pointer;
        unsigned owner;
        const B3::MemoryValue* load;
    };

    struct Owner {
        unsigned value;
        unsigned next;
        const B3::MemoryValue* load;
    };

    struct Safepoint {
        unsigned position;
        Inst* inst;
    };

    // What each location holds in front of a safepoint.
    struct Site {
        unsigned block;
        unsigned index;
        unsigned firstValue;
        unsigned numberOfLocations;
        bool frameIsSetUp;
    };

    using KeyTraits = WTF::UnsignedWithZeroKeyHashTraits<uint64_t>;
    using ValueByBlockAndUnit = UncheckedKeyHashMap<uint64_t, unsigned, DefaultHash<uint64_t>, KeyTraits>;
    using ReportedPlaces = UncheckedKeyHashSet<std::pair<const B3::MemoryValue*, Inst*>>;

    static uint64_t keyOf(unsigned block, unsigned unit) { return static_cast<uint64_t>(block) << 32 | unit; }
    static bool isRegister(unsigned location) { return location < numberOfRegisters; }
    static unsigned unitOf(unsigned location) { return location - numberOfRegisters; }

    bool isFrameBase(Tmp tmp) const
    {
        return tmp == Tmp(MacroAssembler::stackPointerRegister) || (m_hasFrameRecord && tmp == Tmp(GPRInfo::callFrameRegister));
    }

    std::optional<unsigned> locationOfRegister(Tmp tmp) const
    {
        if (!tmp.isGP() || !tmp.isReg() || isFrameBase(tmp))
            return std::nullopt;
        return tmp.reg().index();
    }

    std::optional<int32_t> offsetFromFramePointer(const Arg& arg) const
    {
        if (arg.isExtendedOffsetAddr())
            return arg.offset();
        if (!arg.isAddr() || !isFrameBase(arg.base()))
            return std::nullopt;
        if (arg.base() == Tmp(MacroAssembler::stackPointerRegister))
            return arg.offset() - static_cast<int32_t>(m_bytesBelowFrameRecord);
        return arg.offset();
    }

    unsigned locationOfFrameOffset(int32_t offset)
    {
        auto result = m_unitOfOffset.add(offset >> 2, m_firstHeadOfUnit.size());
        if (result.isNewEntry) {
            m_firstHeadOfUnit.append(none);
            m_isFollowed.append(false);
            m_current.append(none);
        }
        return numberOfRegisters + result.iterator->value;
    }

    std::optional<unsigned> locationOf(const Arg& arg)
    {
        if (arg.isTmp())
            return locationOfRegister(arg.tmp());
        if (std::optional<int32_t> offset = offsetFromFramePointer(arg))
            return locationOfFrameOffset(*offset);
        return std::nullopt;
    }

    bool isCellCageBase(const Arg& arg) const
    {
        return m_rules.cellCageBase && arg.isTmp() && arg.tmp() == Tmp(*m_rules.cellCageBase);
    }

    bool isReferenceShift(const Arg& arg) const
    {
        return m_rules.cellCageBase && arg.isImm() && arg.value() == static_cast<int64_t>(m_rules.referenceShift);
    }

    static bool contains(std::span<const B3::HeapRange> ranges, const B3::HeapRange& range)
    {
        if (!range)
            return false;
        return std::ranges::any_of(ranges, [&](const B3::HeapRange& outer) {
            return outer.begin() <= range.begin() && range.end() <= outer.end();
        });
    }

    bool mayCollect(const Inst& inst) const
    {
        if (inst.kind.opcode != B3::Air::Patch || !inst.origin)
            return false;
        B3::Effects effects = inst.origin->effects();
        return effects.writes.overlaps(m_rules.collectorState) && effects.writes.overlaps(m_rules.weakTableStorage);
    }

    static const B3::MemoryValue* findLoadFoldedInto(B3::Value* value, int64_t offset, unsigned depth)
    {
        if (auto* memory = value->as<B3::MemoryValue>())
            return memory->isLoad() && memory->offset() == offset ? memory : nullptr;
        if (!depth)
            return nullptr;
        for (B3::Value* child : value->children()) {
            if (const B3::MemoryValue* load = findLoadFoldedInto(child, offset, depth - 1))
                return load;
        }
        return nullptr;
    }

    // An access that has taken over the trap of a check of its cell has the origin of the check too (foldTrapsIntoAccesses).
    static const B3::MemoryValue* trapFoldedInto(const Inst& inst, const Arg& address)
    {
        if (!inst.kind.effects || !inst.origin || !inst.origin->traps())
            return nullptr;
        auto* memory = inst.origin->as<B3::MemoryValue>();
        return memory && memory->offset() != address.offset() ? memory : nullptr;
    }

    const B3::MemoryValue* loadOf(const Inst& inst, const Arg& address) const
    {
        B3::Value* origin = trapFoldedInto(inst, address) ? m_code.originOfAccessThatTookTrap(inst.origin) : inst.origin;
        if (!origin)
            return nullptr;
        if (auto* memory = origin->as<B3::MemoryValue>())
            return memory->isLoad() ? memory : nullptr;
        return findLoadFoldedInto(origin, address.offset(), 3);
    }

    static bool computesAddress(B3::Air::Opcode opcode)
    {
        switch (opcode) {
        case B3::Air::Add64:
        case B3::Air::Sub64:
        case B3::Air::Lea64:
        case B3::Air::AddLeftShift64:
        case B3::Air::AddZeroExtend64:
        case B3::Air::AddSignExtend64:
            return true;
        default:
            return false;
        }
    }

    static bool choosesBetweenLastOperands(B3::Air::Opcode opcode)
    {
        switch (opcode) {
        case B3::Air::MoveConditionally32:
        case B3::Air::MoveConditionally64:
        case B3::Air::MoveConditionallyTest32:
        case B3::Air::MoveConditionallyTest64:
        case B3::Air::MoveConditionallyDouble:
        case B3::Air::MoveConditionallyFloat:
            return true;
        default:
            return false;
        }
    }

    unsigned create(unsigned block, unsigned position)
    {
        unsigned value = m_facts.size();
        m_facts.append(Facts { .parent = value, .nextInClass = value, .block = block, .created = position });
        return value;
    }

    unsigned headOf(unsigned block, unsigned location)
    {
        unsigned value;
        if (isRegister(location)) {
            size_t index = static_cast<size_t>(block) * numberOfRegisters + location;
            if (m_headOfRegister[index] != none)
                return m_headOfRegister[index];
            value = create(block, 0);
            m_headOfRegister[index] = value;
            m_headsToResolve.append(value);
        } else {
            auto result = m_headOfUnit.add(keyOf(block, unitOf(location)), none);
            if (!result.isNewEntry)
                return result.iterator->value;
            value = create(block, 0);
            result.iterator->value = value;
            unsigned unit = unitOf(location);
            m_facts[value].nextHeadOfUnit = std::exchange(m_firstHeadOfUnit[unit], value);
            if (m_isFollowed[unit])
                m_headsToResolve.append(value);
        }
        Facts& facts = m_facts[value];
        facts.location = location;
        facts.memberFlags |= IsHead;
        facts.nextHeadInBlock = std::exchange(m_firstHeadOfBlock[block], value);
        return value;
    }

    unsigned tailOf(unsigned block, unsigned location)
    {
        if (isRegister(location)) {
            size_t index = static_cast<size_t>(block) * numberOfRegisters + location;
            if (m_tailOfRegister[index] == none)
                m_tailOfRegister[index] = headOf(block, location);
            return m_tailOfRegister[index];
        }
        if (auto found = m_tailOfUnit.find(keyOf(block, unitOf(location))); found != m_tailOfUnit.end())
            return found->value;
        return headOf(block, location);
    }

    unsigned valueIn(unsigned location)
    {
        if (m_current[location] == none) {
            if (!isRegister(location))
                m_unitsTouched.append(location);
            m_current[location] = headOf(m_block, location);
        }
        return m_current[location];
    }

    unsigned read(unsigned location, unsigned position)
    {
        unsigned value = valueIn(location);
        m_facts[value].lastRead = std::max(m_facts[value].lastRead, position);
        return value;
    }

    void noteReadThatMayGiveCells(unsigned value, unsigned position)
    {
        if (value == clobbered)
            return;
        Facts& facts = m_facts[value];
        if (!facts.lastReadThatMayGiveCells && !(facts.memberFlags & GivesCellsBehindBlock))
            m_valuesThatGiveCells.append(value);
        facts.lastReadThatMayGiveCells = std::max(facts.lastReadThatMayGiveCells, position);
    }

    void set(unsigned location, unsigned value)
    {
        if (!isRegister(location)) {
            if (m_current[location] == none)
                m_unitsTouched.append(location);
            if (value != clobbered)
                link(m_facts[value].firstCopy, unitOf(location));
        }
        m_current[location] = value;
    }

    void link(unsigned& first, unsigned target)
    {
        m_links.append(Link { target, first });
        first = m_links.size() - 1;
    }

    void numberValues()
    {
        m_facts.append(Facts { .parent = clobbered, .nextInClass = clobbered });
        m_current.fill(none, numberOfRegisters);
        m_headOfRegister.fill(none, static_cast<size_t>(m_code.size()) * numberOfRegisters);
        m_tailOfRegister.fill(none, static_cast<size_t>(m_code.size()) * numberOfRegisters);
        m_firstHeadOfBlock.fill(none, m_code.size());
        m_firstChoiceOfBlock.fill(none, m_code.size());
        m_firstSafepointOfBlock.fill(0, m_code.size() + 1);
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            m_firstSafepointOfBlock[blockIndex] = m_safepoints.size();
            if (BasicBlock* block = m_code[blockIndex])
                numberValuesIn(*block);
        }
        m_firstSafepointOfBlock[m_code.size()] = m_safepoints.size();
    }

    void numberValuesIn(BasicBlock& block)
    {
        m_block = block.index();
        m_frameIsSetUp = !m_frameStates || m_frameStates->atHead(&block) == B3::Air::FrameState::SetUp;
        for (unsigned index = 0; index < block.size(); ++index) {
            Inst& inst = block.at(index);
            m_early = 2 * index + 1;
            m_late = m_early + 1;
            if (m_recordsSites && mayCollect(inst)) {
                m_sites.append(Site { m_block, index, static_cast<unsigned>(m_valuesAtSites.size()), static_cast<unsigned>(m_current.size()), m_frameIsSetUp });
                m_valuesAtSites.appendVector(m_current);
            }
            if (!copy(inst))
                compute(inst);
        }
        for (unsigned location = 0; location < numberOfRegisters; ++location)
            m_tailOfRegister[static_cast<size_t>(m_block) * numberOfRegisters + location] = std::exchange(m_current[location], none);
        for (unsigned location : m_unitsTouched)
            m_tailOfUnit.add(keyOf(m_block, unitOf(location)), std::exchange(m_current[location], none));
        m_unitsTouched.shrink(0);
    }

    bool copy(const Arg& source, const Arg& destination)
    {
        std::optional<unsigned> from = locationOf(source);
        std::optional<unsigned> to = locationOf(destination);
        if (!from || !to)
            return false;
        // What goes into any other word of the frame may be read by anybody.
        bool isHandedOn = !isRegister(*to) && !isSpilled(destination);
        set(*to, isHandedOn ? read(*from, m_early) : valueIn(*from));
        return true;
    }

    bool copyPair(const Arg& memory, const Arg& first, const Arg& second, bool isLoad)
    {
        std::optional<int32_t> offset = offsetFromFramePointer(memory);
        std::optional<unsigned> firstRegister = locationOf(first);
        std::optional<unsigned> secondRegister = locationOf(second);
        if (!offset || !firstRegister || !secondRegister)
            return false;
        unsigned firstWord = locationOfFrameOffset(*offset);
        unsigned secondWord = locationOfFrameOffset(*offset + static_cast<int32_t>(sizeof(CPURegister)));
        if (isLoad) {
            set(*firstRegister, valueIn(firstWord));
            set(*secondRegister, valueIn(secondWord));
        } else {
            bool isHandedOn = !isSpilled(memory);
            set(firstWord, isHandedOn ? read(*firstRegister, m_early) : valueIn(*firstRegister));
            set(secondWord, isHandedOn ? read(*secondRegister, m_early) : valueIn(*secondRegister));
        }
        return true;
    }

    unsigned locationOfWordAboveStackPointer(unsigned word)
    {
        return locationOfFrameOffset(static_cast<int32_t>(word * sizeof(CPURegister)) - static_cast<int32_t>(m_bytesBelowFrameRecord));
    }

    void enterFrame(Inst& inst)
    {
        std::span<Arg> args = inst.args();
        unsigned endOfScratchRegisters = 1 + B3::Air::EnterFrameCustom::numberOfScratchRegisters(inst);
        for (Arg& scratch : args.subspan(1, endOfScratchRegisters - 1))
            set(*locationOf(scratch), clobbered);
        unsigned numberOfWords = B3::Air::numberOfWordsAllocatedAtFrameSetup(m_code);
        for (size_t i = endOfScratchRegisters + m_numberOfCalleeSaves; i < args.size(); i += 2)
            set(locationOfWordAboveStackPointer(numberOfWords - 1 - static_cast<unsigned>(args[i + 1].value())), valueIn(*locationOf(args[i])));
    }

    void leaveFrame(Inst& inst)
    {
        std::span<Arg> args = inst.args();
        for (Arg& restored : args.first(m_numberOfCalleeSaves)) {
            if (std::optional<unsigned> location = locationOf(restored))
                set(*location, clobbered);
        }
        for (size_t i = m_numberOfCalleeSaves; i < args.size(); i += 2)
            set(*locationOf(args[i]), valueIn(locationOfWordAboveStackPointer(static_cast<unsigned>(args[i + 1].value()))));
    }

    // True if the instruction only moves values, or gives another form of the reference that it reads.
    bool copy(Inst& inst)
    {
        std::span<Arg> args = inst.args();
        switch (inst.kind.opcode) {
        case B3::Air::EnterFrame:
            enterFrame(inst);
            m_frameIsSetUp = true;
            return true;
        case B3::Air::LeaveFrame:
            leaveFrame(inst);
            m_frameIsSetUp = false;
            return true;
        case B3::Air::Move:
        case B3::Air::Move32:
            if (!copy(args[0], args[1]))
                return false;
            if (args.size() == 3)
                set(*locationOf(args[2]), clobbered);
            return true;
        case B3::Air::Swap64: {
            std::optional<unsigned> first = locationOf(args[0]);
            std::optional<unsigned> second = locationOf(args[1]);
            if (!first || !second)
                return false;
            unsigned value = valueIn(*first);
            set(*first, valueIn(*second));
            set(*second, value);
            return true;
        }
        case B3::Air::LoadPair64:
            return copyPair(args[0], args[1], args[2], true);
        case B3::Air::StorePair64:
            return copyPair(args[2], args[0], args[1], false);
        case B3::Air::Lshift64:
        case B3::Air::Urshift64:
            if (args.size() == 2)
                return isReferenceShift(args[0]) && copy(args[1], args[1]);
            return isReferenceShift(args[1]) && copy(args[0], args[2]);
        case B3::Air::Add64:
            if (isCellCageBase(args[0]))
                return copy(args[1], args.back());
            return args.size() == 3 && isCellCageBase(args[1]) && copy(args[0], args[2]);
        case B3::Air::AddLeftShift64:
            return isCellCageBase(args[0]) && isReferenceShift(args[2]) && copy(args[1], args[3]);
        case B3::Air::Lea64:
            return args[0].isIndex() && isCellCageBase(Arg(args[0].base())) && copy(Arg(args[0].index()), args[1]);
        default:
            return false;
        }
    }

    void compute(Inst& inst)
    {
        m_operands.shrink(0);
        const Arg* addressOfLoad = nullptr;
        unsigned baseOfLoad = none;
        bool handsOperandsOn = inst.kind.opcode == B3::Air::Patch;
        inst.forEachArg([&](Arg& arg, Arg::Role role, B3::Bank, Width) {
            handsOperandsOn |= Arg::isAnyDef(role) && arg.isMemory() && !offsetFromFramePointer(arg);
        });
        inst.forEachArg([&](Arg& arg, Arg::Role role, B3::Bank bank, Width width) {
            if (std::optional<unsigned> location = locationOf(arg)) {
                if (Arg::isAnyUse(role) && role != Arg::Scratch) {
                    unsigned position = Arg::isLateUse(role) ? m_late : m_early;
                    m_operands.append(read(*location, position));
                    if (handsOperandsOn)
                        noteReadThatMayGiveCells(m_operands.last(), position);
                }
                return;
            }
            if (!arg.isMemory())
                return;
            const B3::MemoryValue* loadThroughAddress = Arg::isAnyUse(role) ? loadOf(inst, arg) : nullptr;
            // A reference has 32 or 64 bits and is no floating point number.
            bool mayGiveCells = Arg::isAnyUse(role) && bank == B3::GP && width >= Width32 && !(loadThroughAddress && contains(m_rules.contentsWithoutCells.span(), loadThroughAddress->range()));
            arg.forEachTmpFast([&](Tmp& tmp) {
                if (std::optional<unsigned> location = locationOfRegister(tmp)) {
                    m_operands.append(read(*location, m_early));
                    if (mayGiveCells)
                        noteReadThatMayGiveCells(m_operands.last(), m_early);
                }
            });
            if (!Arg::isAnyUse(role) || !(arg.isAddr() || arg.isIndex() || arg.isSimpleAddr()))
                return;
            // An address may decode a reference by itself.
            Tmp cell = arg.isIndex() && isCellCageBase(Arg(arg.base())) ? arg.index() : arg.base();
            if (std::optional<unsigned> base = locationOfRegister(cell)) {
                addressOfLoad = &arg;
                baseOfLoad = valueIn(*base);
            }
        });

        const B3::MemoryValue* load = addressOfLoad ? loadOf(inst, *addressOfLoad) : nullptr;
        bool isPermanent = inst.kind.opcode == B3::Air::Move && (inst.args()[0].isSomeImm() || (load && contains(m_rules.permanentCells.span(), load->range())));

        if (inst.kind.opcode == B3::Air::Patch) {
            if (mayCollect(inst))
                m_safepoints.append(Safepoint { m_early, &inst });
            auto clobber = [&](Reg reg) {
                if (std::optional<unsigned> location = locationOfRegister(Tmp(reg)))
                    set(*location, clobbered);
            };
            inst.extraEarlyClobberedRegs().forEach(clobber);
            inst.extraClobberedRegs().forEach(clobber);
        }

        unsigned result = none;
        unsigned numberOfResults = 0;
        inst.forEachArg([&](Arg& arg, Arg::Role role, B3::Bank, Width) {
            if (!Arg::isAnyDef(role))
                return;
            std::optional<unsigned> location = locationOf(arg);
            if (!location)
                return;
            if (role == Arg::Scratch) {
                set(*location, clobbered);
                return;
            }
            result = create(m_block, Arg::isEarlyDef(role) ? m_early : m_late);
            if (!isPermanent)
                m_facts[result].classFlags |= HasOrdinaryDefinition;
            set(*location, result);
            ++numberOfResults;
        });
        if (numberOfResults != 1)
            return;

        if (load && baseOfLoad != clobbered && contains(m_rules.fieldsThatHoldPointers.span(), load->range()))
            m_definitions.append(Definition { result, baseOfLoad, load });
        if (const B3::MemoryValue* trap = !load && addressOfLoad && baseOfLoad != clobbered ? trapFoldedInto(inst, *addressOfLoad) : nullptr)
            m_loadsOfUnknownFields.append(Definition { result, baseOfLoad, trap });
        if (choosesBetweenLastOperands(inst.kind.opcode)) {
            m_facts[result].nextHeadInBlock = std::exchange(m_firstChoiceOfBlock[m_block], result);
            for (unsigned source : m_operands.span().last(std::min<size_t>(m_operands.size(), 2))) {
                if (source == clobbered)
                    continue;
                m_choices.append({ result, source });
                link(m_facts[result].firstSource, source);
            }
            return;
        }
        if (!computesAddress(inst.kind.opcode))
            return;
        for (unsigned operand : m_operands) {
            if (operand == clobbered)
                continue;
            link(m_facts[operand].firstSum, result);
            link(m_facts[result].firstSource, operand);
        }
    }

    void findPredecessors()
    {
        m_firstPredecessorOfBlock.fill(0, m_code.size() + 1);
        for (BasicBlock* block : m_code) {
            for (BasicBlock* successor : block->successorBlocks())
                ++m_firstPredecessorOfBlock[successor->index() + 1];
        }
        for (unsigned index = 0; index < m_code.size(); ++index)
            m_firstPredecessorOfBlock[index + 1] += m_firstPredecessorOfBlock[index];
        m_predecessors.fill(none, m_firstPredecessorOfBlock.last());
        Vector<unsigned> next = m_firstPredecessorOfBlock;
        for (BasicBlock* block : m_code) {
            for (BasicBlock* successor : block->successorBlocks())
                m_predecessors[next[successor->index()]++] = block->index();
        }
    }

    std::span<const unsigned> predecessorsOf(unsigned block) const
    {
        return m_predecessors.span().subspan(m_firstPredecessorOfBlock[block], m_firstPredecessorOfBlock[block + 1] - m_firstPredecessorOfBlock[block]);
    }

    unsigned find(unsigned value)
    {
        while (m_facts[value].parent != value) {
            m_facts[value].parent = m_facts[m_facts[value].parent].parent;
            value = m_facts[value].parent;
        }
        return value;
    }

    static uint8_t withImpliedFlags(uint8_t flags)
    {
        if ((flags & IsPointer) && (flags & IsOwnerOfSuspect))
            flags |= HasSuspect;
        return flags;
    }

    void raise(unsigned root, uint8_t flags)
    {
        uint8_t added = flags & ~m_facts[root].classFlags;
        if (!added)
            return;
        m_facts[root].classFlags = flags;
        if (added & (IsPointer | IsOwnerOfSuspect)) {
            unsigned member = root;
            do {
                m_valuesToVisit.append(member);
                member = m_facts[member].nextInClass;
            } while (member != root);
        }
        if (added & HasSuspect) {
            for (unsigned owner = m_facts[root].firstOwner; owner != none; owner = m_owners[owner].next)
                m_ownersOfSuspects.append(m_owners[owner].value);
        }
    }

    void add(unsigned value, uint8_t flags)
    {
        unsigned root = find(value);
        raise(root, withImpliedFlags(m_facts[root].classFlags | flags));
    }

    void unite(unsigned first, unsigned second)
    {
        first = find(first);
        second = find(second);
        if (first == second)
            return;
        uint8_t flags = withImpliedFlags(m_facts[first].classFlags | m_facts[second].classFlags);
        raise(first, flags);
        raise(second, flags);
        m_facts[second].parent = first;
        std::swap(m_facts[first].nextInClass, m_facts[second].nextInClass);
        if (m_facts[second].firstOwner == none)
            return;
        if (m_facts[first].firstOwner == none)
            m_facts[first].firstOwner = m_facts[second].firstOwner;
        else
            m_owners[m_facts[first].lastOwner].next = m_facts[second].firstOwner;
        m_facts[first].lastOwner = m_facts[second].lastOwner;
    }

    void addOwner(const Definition& definition)
    {
        Facts& facts = m_facts[find(definition.pointer)];
        m_owners.append(Owner { definition.owner, facts.firstOwner, definition.load });
        facts.firstOwner = m_owners.size() - 1;
        if (facts.lastOwner == none)
            facts.lastOwner = facts.firstOwner;
    }

    void follow(unsigned unit)
    {
        if (std::exchange(m_isFollowed[unit], true))
            return;
        for (unsigned head = m_firstHeadOfUnit[unit]; head != none; head = m_facts[head].nextHeadOfUnit)
            m_headsToResolve.append(head);
    }

    // The last safepoint behind the making of the value and not behind the end.
    const Safepoint* lastSafepointUpTo(unsigned value, unsigned end) const
    {
        const Facts& facts = m_facts[value];
        std::span<const Safepoint> safepoints = m_safepoints.span().subspan(m_firstSafepointOfBlock[facts.block], m_firstSafepointOfBlock[facts.block + 1] - m_firstSafepointOfBlock[facts.block]);
        auto firstBehind = std::ranges::upper_bound(safepoints, end, { }, &Safepoint::position);
        if (firstBehind == safepoints.begin())
            return nullptr;
        const Safepoint& last = *std::prev(firstBehind);
        return last.position > facts.created ? &last : nullptr;
    }

    // The last safepoint that the value is an operand of, or is read behind.
    const Safepoint* lastSafepointReachedBy(unsigned value) const
    {
        return lastSafepointUpTo(value, endOf(m_facts[value]));
    }

    void judge(unsigned value)
    {
        if (lastSafepointReachedBy(value))
            add(value, HasSuspect);
    }

    void resolve(unsigned head)
    {
        unsigned block = m_facts[head].block;
        unsigned location = m_facts[head].location;
        std::span<const unsigned> predecessors = predecessorsOf(block);
        if (predecessors.empty())
            add(head, HasOrdinaryDefinition);
        for (unsigned predecessor : predecessors) {
            unsigned incoming = tailOf(predecessor, location);
            if (incoming == clobbered)
                continue;
            if (!(m_facts[incoming].memberFlags & IsLiveOut)) {
                m_facts[incoming].memberFlags |= IsLiveOut;
                if (m_facts[find(incoming)].classFlags & IsPointer)
                    judge(incoming);
            }
            unite(head, incoming);
        }
    }

    void visit(unsigned value)
    {
        if (!(m_facts[value].memberFlags & HasFollowedCopies)) {
            m_facts[value].memberFlags |= HasFollowedCopies;
            for (unsigned copy = m_facts[value].firstCopy; copy != none; copy = m_links[copy].next)
                follow(m_links[copy].target);
            if ((m_facts[value].memberFlags & IsHead) && !isRegister(m_facts[value].location))
                follow(unitOf(m_facts[value].location));
        }
        if (!(m_facts[find(value)].classFlags & IsPointer) || (m_facts[value].memberFlags & HasFollowedSums))
            return;
        m_facts[value].memberFlags |= HasFollowedSums;
        m_pointers.append(value);
        for (unsigned sum = m_facts[value].firstSum; sum != none; sum = m_links[sum].next)
            unite(value, m_links[sum].target);
        judge(value);
    }

    void solve()
    {
        for (;;) {
            if (!m_headsToResolve.isEmpty())
                resolve(m_headsToResolve.takeLast());
            else if (!m_valuesToVisit.isEmpty())
                visit(m_valuesToVisit.takeLast());
            else if (!m_ownersOfSuspects.isEmpty())
                add(m_ownersOfSuspects.takeLast(), IsOwnerOfSuspect);
            else
                return;
        }
    }

    static unsigned endOf(const Facts& facts)
    {
        return facts.memberFlags & IsLiveOut ? none : facts.lastRead;
    }

    static unsigned endOfReadsThatMayGiveCells(const Facts& facts)
    {
        return facts.memberFlags & GivesCellsBehindBlock ? none : facts.lastReadThatMayGiveCells;
    }

    // A pointer gives cells where a sum or a choice made from it does, and at the end of each block in front of one that finds it in a location and gives cells through it.
    void findWhatGivesCellsLater()
    {
        while (!m_valuesThatGiveCells.isEmpty()) {
            unsigned value = m_valuesThatGiveCells.takeLast();
            if (!(m_facts[find(value)].classFlags & IsPointer))
                continue;
            for (unsigned source = m_facts[value].firstSource; source != none; source = m_links[source].next)
                noteReadThatMayGiveCells(m_links[source].target, m_facts[value].created);
            if (!(m_facts[value].memberFlags & IsHead))
                continue;
            for (unsigned predecessor : predecessorsOf(m_facts[value].block)) {
                unsigned incoming = tailOf(predecessor, m_facts[value].location);
                if (incoming == clobbered || (m_facts[incoming].memberFlags & GivesCellsBehindBlock))
                    continue;
                if (!m_facts[incoming].lastReadThatMayGiveCells)
                    m_valuesThatGiveCells.append(incoming);
                m_facts[incoming].memberFlags |= GivesCellsBehindBlock;
            }
        }
    }

    void noteWhatBlockHolds(unsigned block)
    {
        for (unsigned head = m_firstHeadOfBlock[block]; head != none; head = m_facts[head].nextHeadInBlock) {
            unsigned root = find(head);
            if (std::exchange(m_blockOfEnd[root], block) != block)
                m_end[root] = 0;
            m_end[root] = std::max(m_end[root], endOf(m_facts[head]));
        }
        m_choicesOfNotedBlock.shrink(0);
        for (unsigned choice = m_firstChoiceOfBlock[block]; choice != none; choice = m_facts[choice].nextHeadInBlock)
            m_choicesOfNotedBlock.append({ find(choice), choice });
        std::ranges::sort(m_choicesOfNotedBlock);
    }

    // The load that made a pointer whose owner is last read in front of the position, if there is one.
    const B3::MemoryValue* findLoadWithoutOwner(unsigned pointer, unsigned block, unsigned position, bool givesCells)
    {
        ++m_query;
        m_classesToAsk.shrink(0);
        m_classesToAsk.append(find(pointer));
        m_queryOfClass[m_classesToAsk[0]] = m_query;
        while (!m_classesToAsk.isEmpty()) {
            for (unsigned index = m_facts[m_classesToAsk.takeLast()].firstOwner; index != none; index = m_owners[index].next) {
                const Owner& owner = m_owners[index];
                unsigned root = find(owner.value);
                if (std::exchange(m_queryOfClass[root], m_query) == m_query)
                    continue;
                if (m_facts[root].classFlags & IsPointer) {
                    m_classesToAsk.append(root);
                    continue;
                }
                if (!givesCells && contains(m_rules.fieldsThatHoldPointersTheScanKnows.span(), owner.load->range()))
                    continue;
                if (!(m_facts[root].classFlags & HasOrdinaryDefinition))
                    continue;
                if (m_blockOfEnd[root] == block && m_end[root] >= position)
                    continue;
                auto spans = [&](unsigned value) {
                    const Facts& facts = m_facts[value];
                    return facts.block == block && facts.created < position && endOf(facts) >= position;
                };
                if (spans(owner.value))
                    continue;
                auto choices = std::ranges::equal_range(m_choicesOfNotedBlock, root, { }, &std::pair<unsigned, unsigned>::first);
                if (std::ranges::any_of(choices, [&](const auto& choice) { return spans(choice.second); }))
                    continue;
                return owner.load;
            }
        }
        return nullptr;
    }

    unsigned headIfAny(unsigned block, unsigned location) const
    {
        if (isRegister(location))
            return m_headOfRegister[static_cast<size_t>(block) * numberOfRegisters + location];
        auto found = m_headOfUnit.find(keyOf(block, unitOf(location)));
        return found == m_headOfUnit.end() ? none : found->value;
    }

    bool isDeadAt(const Site& site, unsigned location) const
    {
        unsigned value = location < site.numberOfLocations ? m_valuesAtSites[site.firstValue + location] : none;
        if (value == none)
            value = headIfAny(site.block, location);
        return value == none || value == clobbered || endOf(m_facts[value]) < 2 * site.index + 1;
    }

    bool isDeadAt(const Site& site, int32_t offsetFromFramePointer) const
    {
        auto unit = m_unitOfOffset.find(offsetFromFramePointer >> 2);
        return unit == m_unitOfOffset.end() || isDeadAt(site, numberOfRegisters + unit->value);
    }

    // The offsets of the words that consist of spill slots alone. Only the instructions of the function read those.
    void findSpilledWords()
    {
        constexpr int32_t wordSize = sizeof(CPURegister);
        UncheckedKeyHashMap<int32_t, unsigned, DefaultHash<int32_t>, WTF::UnsignedWithZeroKeyHashTraits<int32_t>> spilledBytesOfWord;
        UncheckedKeyHashSet<int32_t, DefaultHash<int32_t>, WTF::UnsignedWithZeroKeyHashTraits<int32_t>> otherWords;
        for (B3::Air::StackSlot* slot : m_code.stackSlots()) {
            int32_t begin = static_cast<int32_t>(slot->offsetFromFP());
            int32_t end = begin + static_cast<int32_t>(slot->byteSize());
            for (int32_t word = begin & -wordSize; word < end; word += wordSize) {
                if (!slot->isSpill() || slot->byteSize() > sizeof(CPURegister))
                    otherWords.add(word);
                else
                    spilledBytesOfWord.add(word, 0).iterator->value |= ((1u << slot->byteSize()) - 1) << (begin - word);
            }
        }
        for (auto& entry : spilledBytesOfWord) {
            if (entry.value == 0xff && !otherWords.contains(entry.key))
                m_spilledWords.add(entry.key);
        }
    }

    bool isSpilled(const Arg& address) const
    {
        return m_spilledWords.contains(*offsetFromFramePointer(address) & -static_cast<int32_t>(sizeof(CPURegister)));
    }

    Arg addressInFrame(int32_t offsetFromFramePointer) const
    {
        if (m_hasFrameRecord)
            return Arg::addr(Tmp(GPRInfo::callFrameRegister), offsetFromFramePointer);
        return Arg::addr(Tmp(MacroAssembler::stackPointerRegister), offsetFromFramePointer + static_cast<int32_t>(m_bytesBelowFrameRecord));
    }

    uint32_t overwriteAtSites()
    {
        constexpr int64_t wordThatNamesNothing = 1;
        // A slot that has no place in the frame says offset 0, so only words that some instruction names count.
        Vector<int32_t> spilledWords;
        for (int32_t word : m_spilledWords) {
            if (m_unitOfOffset.contains(word >> 2) || m_unitOfOffset.contains((word + 4) >> 2))
                spilledWords.append(word);
        }
        std::ranges::sort(spilledWords);
        // A register that callees save may be written where this function has saved it, and any other everywhere.
        uint64_t savedRegisters = 0;
        for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList()) {
            if (entry.reg().isGPR())
                savedRegisters |= 1ull << entry.reg().index();
        }
        RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
        calleeSaves.exclude(m_code.unsavedCalleeSaves());
        calleeSaves.merge(m_code.policy().registers.savedByCallee);
        uint64_t allocatableRegisters = 0;
        for (Reg reg : m_code.regsInPriorityOrder(B3::GP))
            allocatableRegisters |= 1ull << reg.index();
        allocatableRegisters &= ~(1ull << Reg(GPRInfo::callFrameRegister).index() | 1ull << Reg(MacroAssembler::stackPointerRegister).index());
        uint64_t registersOfCaller = 0;
        calleeSaves.forEach([&](Reg reg) {
            if (reg.isGPR())
                registersOfCaller |= 1ull << reg.index();
        });
        uint32_t numberOverwritten = 0;
        B3::Air::InsertionSet insertionSet(m_code);
        for (size_t next = 0; next < m_sites.size();) {
            BasicBlock* block = m_code[m_sites[next].block];
            for (; next < m_sites.size() && m_sites[next].block == block->index(); ++next) {
                const Site& site = m_sites[next];
                B3::Value* origin = block->at(site.index).origin;
                std::optional<Tmp> overwritten;
                for (unsigned location = 0; location < numberOfRegisters; ++location) {
                    Reg reg = Reg::fromIndex(location);
                    uint64_t bit = 1ull << location;
                    if (!(allocatableRegisters & bit) || ((registersOfCaller & bit) && !(site.frameIsSetUp && (savedRegisters & bit))) || !isDeadAt(site, location))
                        continue;
                    overwritten = Tmp(reg);
                    insertionSet.insert(site.index, B3::Air::Move, origin, Arg::bigImm(wordThatNamesNothing), *overwritten);
                    ++numberOverwritten;
                }
                if (!overwritten || !site.frameIsSetUp)
                    continue;
                for (int32_t offset : spilledWords) {
                    Arg address = addressInFrame(offset);
                    if (!isDeadAt(site, offset) || !isDeadAt(site, offset + 4) || !address.isValidForm(B3::Air::Move, Width64))
                        continue;
                    insertionSet.insert(site.index, B3::Air::Move, origin, *overwritten, address);
                    ++numberOverwritten;
                }
            }
            insertionSet.execute(block);
        }
        return numberOverwritten;
    }

    void report()
    {
        std::ranges::sort(m_pointers, { }, [&](unsigned value) { return m_facts[value].block; });
        m_blockOfEnd.fill(none, m_facts.size());
        m_end.fill(0, m_facts.size());
        m_queryOfClass.fill(0, m_facts.size());
        unsigned notedBlock = none;
        ReportedPlaces reported;
        for (unsigned pointer : m_pointers) {
            const Safepoint* safepoint = lastSafepointReachedBy(pointer);
            if (!safepoint)
                continue;
            ++m_findings.numberOfPointersAcrossSafepoints;
            const Facts& facts = m_facts[pointer];
            if (std::exchange(notedBlock, facts.block) != facts.block)
                noteWhatBlockHolds(facts.block);
            // What the scan keeps by the pointer alone needs its owner only up to the last read that may give a cell.
            unsigned endOfCells = endOfReadsThatMayGiveCells(facts);
            if (const Safepoint* safepointOfCells = lastSafepointUpTo(pointer, endOfCells))
                reportUnlessOwnerIsHeld(reported, pointer, *safepointOfCells, endOfCells, facts.lastReadThatMayGiveCells, true);
            if (endOfCells != endOf(facts))
                reportUnlessOwnerIsHeld(reported, pointer, *safepoint, endOf(facts), facts.lastRead, false);
        }
    }

    void reportUnlessOwnerIsHeld(ReportedPlaces& reported, unsigned pointer, const Safepoint& safepoint, unsigned end, unsigned lastReadInBlock, bool givesCells)
    {
        const Facts& facts = m_facts[pointer];
        // Whoever is handed a pointer and its owner holds the owner for as long as it needs the pointer.
        bool isOnlyOperand = end == safepoint.position;
        const B3::MemoryValue* load = findLoadWithoutOwner(pointer, facts.block, safepoint.position + !isOnlyOperand, givesCells);
        if (!load || !reported.add({ load, safepoint.inst }).isNewEntry)
            return;
        StringPrintStream detail;
        detail.print(": ", deepDump(m_code.proc(), load), isOnlyOperand ? " is an operand of " : " is read behind ", *safepoint.inst, " in block #", facts.block);
        if (lastReadInBlock > safepoint.position)
            detail.print(" by ", m_code[facts.block]->at((lastReadInBlock - 1) / 2));
        else if (!isOnlyOperand)
            detail.print(" by a later block");
        m_findings.violations.append(InnerPointerViolation { load->origin(), safepoint.inst->origin->origin(), detail.toString() });
    }

    Code& m_code;
    const InnerPointerRules& m_rules;
    InnerPointerFindings& m_findings;
    const bool m_hasFrameRecord;
    const unsigned m_bytesBelowFrameRecord;
    const unsigned m_numberOfCalleeSaves;

    Vector<Facts> m_facts;
    Vector<Link> m_links;
    Vector<Owner> m_owners;
    Vector<Definition> m_definitions;
    Vector<Definition> m_loadsOfUnknownFields;
    Vector<std::pair<unsigned, unsigned>> m_choices;
    Vector<Safepoint> m_safepoints;
    Vector<unsigned> m_firstSafepointOfBlock;
    Vector<unsigned> m_firstPredecessorOfBlock;
    Vector<unsigned> m_predecessors;

    UncheckedKeyHashMap<int32_t, unsigned, DefaultHash<int32_t>, WTF::UnsignedWithZeroKeyHashTraits<int32_t>> m_unitOfOffset;
    UncheckedKeyHashSet<int32_t, DefaultHash<int32_t>, WTF::UnsignedWithZeroKeyHashTraits<int32_t>> m_spilledWords;
    Vector<unsigned> m_firstHeadOfUnit;
    Vector<bool> m_isFollowed;
    Vector<unsigned> m_firstHeadOfBlock;
    Vector<unsigned> m_firstChoiceOfBlock;
    Vector<std::pair<unsigned, unsigned>> m_choicesOfNotedBlock;
    Vector<unsigned> m_headOfRegister;
    Vector<unsigned> m_tailOfRegister;
    ValueByBlockAndUnit m_headOfUnit;
    ValueByBlockAndUnit m_tailOfUnit;

    bool m_recordsSites { false };
    bool m_frameIsSetUp { true };
    std::optional<B3::Air::FrameStates> m_frameStates;
    Vector<Site> m_sites;
    Vector<unsigned> m_valuesAtSites;

    unsigned m_block { 0 };
    unsigned m_early { 0 };
    unsigned m_late { 0 };
    Vector<unsigned> m_current;
    Vector<unsigned> m_unitsTouched;
    Vector<unsigned, 8> m_operands;

    Vector<unsigned> m_headsToResolve;
    Vector<unsigned> m_valuesToVisit;
    Vector<unsigned> m_ownersOfSuspects;
    Vector<unsigned> m_valuesThatGiveCells;
    Vector<unsigned> m_pointers;

    Vector<unsigned> m_blockOfEnd;
    Vector<unsigned> m_end;
    Vector<unsigned> m_queryOfClass;
    Vector<unsigned> m_classesToAsk;
    unsigned m_query { 0 };
};

} // anonymous namespace

void findInnerPointerViolations(B3::Air::Code& code, const InnerPointerRules& rules, InnerPointerFindings& findings)
{
    InnerPointerValidation(code, rules, findings).run();
}

uint32_t overwriteDeadValuesAtSafepoints(B3::Air::Code& code, const InnerPointerRules& rules)
{
    InnerPointerFindings findings;
    return InnerPointerValidation(code, rules, findings).overwriteDeadValues();
}

InnerPointerRules innerPointerRulesOf(B3::Procedure& proc, bool usesNarrowReferences)
{
    const AOTHeaps& heaps = AOTHeaps::of(proc);
    InnerPointerRules rules;
    for (const B3::AbstractHeap* heap : std::initializer_list<const B3::AbstractHeap*> { &heaps.JSArrayBufferView_vector, &heaps.JSObject_butterfly, &heaps.JSString_value, &heaps.StringImpl_data, &heaps.JSArrayBuffer_impl, &heaps.ArrayBuffer_data, &heaps.WeakMapImpl_buffer, &heaps.JSPropertyNameEnumerator_cachedPropertyNamesVector })
        rules.fieldsThatHoldPointers.append(heaps.rangeOf(*heap));
    for (const B3::AbstractHeap* heap : std::initializer_list<const B3::AbstractHeap*> { &heaps.StringImpl_data, &heaps.ArrayBuffer_data })
        rules.fieldsOutsideCells.append(heaps.rangeOf(*heap));
    rules.fieldsThatHoldPointersTheScanKnows.append(heaps.rangeOf(heaps.JSObject_butterfly));
    for (const B3::AbstractHeap* heap : std::initializer_list<const B3::AbstractHeap*> { &heaps.indexedInt32Properties.atAnyIndex(), &heaps.indexedDoubleProperties.atAnyIndex(), &heaps.Butterfly_publicLength, &heaps.Butterfly_vectorLength })
        rules.contentsWithoutCells.append(heaps.rangeOf(*heap));
    for (const B3::AbstractHeap* heap : std::initializer_list<const B3::AbstractHeap*> { &heaps.AOTInstance_stringCells, &heaps.AOTInstance_fixedFields, &heaps.AOTImage_tables })
        rules.permanentCells.append(heaps.rangeOf(*heap));
    rules.collectorState = heaps.rangeOf(heaps.collectorState);
    rules.weakTableStorage = heaps.rangeOf(heaps.weakTableStorage);
    if (usesNarrowReferences) {
        rules.cellCageBase = cellCageBaseGPR;
        rules.referenceShift = cell32Shift;
    }
    return rules;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
