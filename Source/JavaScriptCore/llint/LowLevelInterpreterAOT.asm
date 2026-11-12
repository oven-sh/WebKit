# Copyright (C) 2026 Oven, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

# Handlers of bytecode in a process that runs ahead-of-time compiled code, where no JIT takes over from the interpreter.
# LLInt::initialize() puts them into the opcode maps when Options::useAOT() is set, in place of the handlers of the same opcodes, so no
# other process runs them.
#
# The Structure of a compiled object prohibits caching by offset. What its properties are is in the hashed table of
# AOT::VMPropertyCaches, under (StructureID, name, access). The slow paths fill the table. The cache of a get_by_id holds a property
# that lies in the cell of a compiled object or among its extra properties: AOT::Interpreter::fieldOffsetForSiteCache().
#
# Nobody reads a value profile here, so none is written.

const CompiledObjectType = constexpr CompiledObjectType
const ClosureVarResolve = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::None))
const FusedRead = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::Read))
const FusedReadIntoScopeRegister = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadIntoScopeRegister))
const FusedReadAndCheck = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadAndCheck))
const FusedReadIntoScopeRegisterAndCheck = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadIntoScopeRegisterAndCheck))
const FusedReadOuter = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadOuter))
const FusedReadOuterIntoScopeRegister = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadOuterIntoScopeRegister))
const FusedReadOuterAndCheck = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadOuterAndCheck))
const FusedReadOuterIntoScopeRegisterAndCheck = constexpr (AOT::Interpreter::FusedClosureVariableRead::typeAndKind(AOT::Interpreter::FusedClosureVariableRead::ReadOuterIntoScopeRegisterAndCheck))
const FirstFusedRead = constexpr (AOT::Interpreter::FusedClosureVariableRead::kindOnly(AOT::Interpreter::FusedClosureVariableRead::Read))
const FirstFusedGlobalRead = constexpr (AOT::Interpreter::FusedClosureVariableRead::kindOnly(AOT::Interpreter::FusedClosureVariableRead::ReadGlobal))
const FusedGlobalReadIntoScopeRegister = constexpr (AOT::Interpreter::FusedClosureVariableRead::kindOnly(AOT::Interpreter::FusedClosureVariableRead::ReadGlobalIntoScopeRegister))
const FusedGlobalShift = constexpr AOT::Interpreter::FusedClosureVariableRead::offsetShift
const FusedVariable = constexpr AOT::Interpreter::FusedClosureVariableRead::offsetOfVariable
const NarrowReferenceInSiteCache = constexpr AOT::Interpreter::narrowReferenceInSiteCache
const ExtraPropertyInSiteCache = constexpr AOT::Interpreter::extraPropertyInSiteCache
const FieldInSiteCache = constexpr AOT::Interpreter::fieldInSiteCache

const HashedSlotKey = constexpr (AOT::HashedSlot::offsetOfKey())
const SlotStructureID = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfStructureID())
const SlotKind = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfKind())
const SlotFlags = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfFlags())
const SlotLocation = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfLocation())
const SlotPayload = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfPayload())
const SlotSecondPayloadWord = constexpr (AOT::HashedSlot::offsetOfSlot() + AOT::Slot::offsetOfSecondPayloadWord())
const HashedSlotSize = constexpr (sizeof(AOT::HashedSlot))

const InstanceHashedSlots = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots))
const InstanceHashedSlotOffsetMask = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hashedSlotOffsetMask))
const InstanceCellCageBase = constexpr (AOT_INSTANCE_OFFSET(cellCageBase))
const InstanceTables = constexpr (AOT_INSTANCE_OFFSET(image.tables))
const InstanceGlobalObject = constexpr (AOT_INSTANCE_OFFSET(globalObject))
const InstanceAuxiliarySpace = constexpr (AOT_INSTANCE_OFFSET(allocation.auxiliarySpace))
const InstanceCellSpace = constexpr (AOT_INSTANCE_OFFSET(allocation.cellSpace))
const InstanceCustomAccessors = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.customAccessors))
const InstanceEnumerators = constexpr (AOT_INSTANCE_OFFSET(objects.enumerators))
const InstanceHasInstanceUID = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hasInstanceUID))
const InstancePrototypeUID = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID))
const InstanceDefaultHasInstanceFunction = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.defaultHasInstanceFunction))
const InstanceFunctionAllocations = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.functionAllocationEntries))
const InstanceClonedArgumentsAllocator = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.clonedArgumentsAllocator))
const MaxInlineClonedArguments = 16
const InstanceSwitchTargets = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.switchTargets))
const InstancePromiseAllocator = constexpr AOT::Interpreter::offsetOfPromiseAllocator
const InstancePromiseHeader = constexpr AOT::Interpreter::offsetOfPromiseHeader
const InstanceArrayAllocator = constexpr (AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator))
const InstanceNewArrayWithContiguousHeader = constexpr (AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader))
const InstanceNewArrayWithInt32Header = constexpr (AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header))
const InstanceEmptyArrayAllocator = constexpr (AOT_INSTANCE_OFFSET(shortArrays.allocators) + AOT::ShortArrayWords::indexOfAllocator(BASE_CONTIGUOUS_VECTOR_LEN_EMPTY) * sizeof(void*))
const InstanceRopeStringAllocator = constexpr (AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator))
const InstanceStringHeader = constexpr (AOT_INSTANCE_PREFIX_OFFSET(stringHeader))
const InstanceClosureEntrypoint = constexpr (AOT_INSTANCE_OFFSET(stubs.closureEntrypoint))
const InstanceSlimClosureEntrypoint = constexpr (AOT_INSTANCE_OFFSET(stubs.slimClosureEntrypoint))

const ObjectLiteralPlanTag = constexpr AOT::Interpreter::ObjectLiteralPlan::allocatorTag
const ObjectLiteralNotPlannedYet = constexpr AOT::Interpreter::ObjectLiteralPlan::notPlannedYet
const ObjectLiteralPlanAllocator = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfAllocator() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanCellHeader = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfCellHeader() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanSecondWord = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfSecondWord() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanNumberOfValues = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfNumberOfValues() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanNumberOfClearedWords = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfNumberOfClearedWords() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanOffsetOfClearedWords = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfOffsetOfClearedWords() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanValueOffsets = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfValueOffsets() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)

const RuntimeTypePrimitives = constexpr (AOT::RuntimeType::offsetOfPrimitives())
const RuntimeTypeFlags = constexpr (AOT::RuntimeType::offsetOfFlags())
const RuntimeTypeNumberOfCellTests = constexpr (AOT::RuntimeType::offsetOfNumberOfCellTests())
const RuntimeTypeNumberOfLiteralStrings = constexpr (AOT::RuntimeType::offsetOfNumberOfLiteralStrings())
const RuntimeTypeKindOfFirstCellTest = constexpr (AOT::RuntimeType::offsetOfCellTests() + AOT::RuntimeCellTest::offsetOfKind())
const RuntimeTypeSize = constexpr (sizeof(AOT::RuntimeType))

# The header and the receiver, a multiple of 16 bytes.
const FrameSizeForGetter = 48
const FrameSizeForSetter = 64

# LazyUniqueRef: an address with one of these bits set is not the object's.
const HasOwnPropertyCacheIsLazy = 3

const RopeStringLength = constexpr (JSRopeString::offsetOfLength())
const MaxLengthOfStringsComparedInline = 32

const LocalAllocatorIntervalStart = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart())
const LocalAllocatorIntervalEnd = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd())
const LocalAllocatorNextInterval = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval())
const LocalAllocatorSecret = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret())
const LocalAllocatorCellSize = constexpr (LocalAllocator::offsetOfCellSize())
const FreeCellScrambledBits = constexpr (FreeCell::offsetOfScrambledBits())
const StructureTypeInfoBlob = constexpr (Structure::indexingModeIncludingHistoryOffset())

const FunctionAllocationExecutable = constexpr (OBJECT_OFFSETOF(AOT::FunctionAllocationEntry, executable))
const FunctionAllocationHeader = constexpr (OBJECT_OFFSETOF(AOT::FunctionAllocationEntry, header))
const FunctionAllocationAllocator = constexpr (OBJECT_OFFSETOF(AOT::FunctionAllocationEntry, allocator))
const FunctionAllocationSize = constexpr (sizeof(AOT::FunctionAllocationEntry))
const NumberOfFunctionAllocations = constexpr AOT::FunctionAllocationEntry::numberOfBirths
const FunctionAllocationExecutableShift = constexpr AOT::FunctionAllocationEntry::executableShift

const SwitchTargetString = constexpr (OBJECT_OFFSETOF(AOT::StringSwitchTarget, string))
const SwitchTargetInstruction = constexpr (OBJECT_OFFSETOF(AOT::StringSwitchTarget, instruction))
const SwitchTargetOffset = constexpr (OBJECT_OFFSETOF(AOT::StringSwitchTarget, offset))
const SwitchTargetSize = constexpr (sizeof(AOT::StringSwitchTarget))
const NumberOfSwitchTargets = constexpr AOT::StringSwitchTarget::numberOfTargets
const SwitchTargetStringShift = constexpr AOT::StringSwitchTarget::stringShift
const SwitchTargetInstructionShift = constexpr AOT::StringSwitchTarget::instructionShift

macro aotOp(opcodeName, opcodeStruct, fn)
    commonOp(llint_aot_%opcodeName%, traceExecution, macro (size)
        macro getImpl(fieldName, dst)
            get(size, opcodeStruct, fieldName, dst)
        end

        macro dispatchImpl()
            dispatchOp(size, opcodeName)
        end

        # metadata() for tables as they are under useAOT: metadataTablesAlwaysHave32BitOffsets(), offsetOfFirstMetadata.
        macro metadataImpl(dst, scratch)
            if X86_64
                getu(size, opcodeStruct, m_metadataID, dst)
                muli sizeof %opcodeStruct%::Metadata, dst
                addi (constexpr %opcodeStruct%::opcodeID * 4 + MetadataOffsetTable32Offset)[metadataTable], dst
            else
                loadi (constexpr %opcodeStruct%::opcodeID * 4 + MetadataOffsetTable32Offset)[metadataTable], dst
                getu(size, opcodeStruct, m_metadataID, scratch)
                muli sizeof %opcodeStruct%::Metadata, scratch
                addi scratch, dst
            end
            addp metadataTable, dst
        end

        # No handler returns t5, nor does a macro of the ordinary handlers that is handed this one: enumeratorGetByVal returns t2.
        macro returnImpl(value)
            getImpl(m_dst, t5)
            storeq value, [cfr, t5, 8]
            dispatchImpl()
        end

        fn(size, getImpl, dispatchImpl, metadataImpl, returnImpl)
    end)
end

# The metadata of a site: its offset in the table on x64, its address elsewhere. makeSiteAddress() turns it into the address.
macro locateSite(size, opcodeStruct, site, scratch)
    if X86_64
        getu(size, opcodeStruct, m_metadataID, site)
        muli sizeof %opcodeStruct%::Metadata, site
        addi (constexpr %opcodeStruct%::opcodeID * 4 + MetadataOffsetTable32Offset)[metadataTable], site
    else
        loadi (constexpr %opcodeStruct%::opcodeID * 4 + MetadataOffsetTable32Offset)[metadataTable], site
        getu(size, opcodeStruct, m_metadataID, scratch)
        muli sizeof %opcodeStruct%::Metadata, scratch
        addi scratch, site
        addp metadataTable, site
    end
end

macro makeSiteAddress(site)
    if X86_64
        addp metadataTable, site
    end
end

macro loadSiteInt(opcodeStruct, fieldName, site, value)
    if X86_64
        loadi %opcodeStruct%::Metadata::%fieldName%[metadataTable, site, 1], value
    else
        loadi %opcodeStruct%::Metadata::%fieldName%[site], value
    end
end

macro loadSitePointer(opcodeStruct, fieldName, site, value)
    if X86_64
        loadp %opcodeStruct%::Metadata::%fieldName%[metadataTable, site, 1], value
    else
        loadp %opcodeStruct%::Metadata::%fieldName%[site], value
    end
end

# loadConstantOrVariable() in two halves, so that a register, which is what most operands are, is loaded without a jump over the other half. The second half stands behind the handler's own code.
macro loadOperandInRegister(size, index, value, isConstant)
    size(macro ()
        bpgteq index, FirstConstantRegisterIndexNarrow, isConstant
    end, macro ()
        bpgteq index, FirstConstantRegisterIndexWide16, isConstant
    end, macro ()
        bpgteq index, FirstConstantRegisterIndexWide32, isConstant
    end, macro (branch) branch() end)
    loadq [cfr, index, 8], value
end

macro loadConstantOperand(size, index, value, isLoaded)
    loadConstant(size, index, value)
    jmp isLoaded
end

# AOT::instanceOfTables(), whose tables answer for every cell, compiled object or not. What belongs to a realm is asked of the instance that loadRealmInstance() leaves.
macro loadTableInstance(instance, hasNone)
    loadp CodeBlock[cfr], instance
    loadp CodeBlock::m_globalObject[instance], instance
    loadp JSGlobalObject::m_aotTableInstance[instance], instance
    btpz instance, hasNone
end

macro loadRealmInstance(instance, hasNone)
    loadp CodeBlock[cfr], instance
    loadp CodeBlock::m_globalObject[instance], instance
    loadp JSGlobalObject::m_aotInstance[instance], instance
    btpz instance, hasNone
end

macro loadIdentifier(size, opcodeStruct, fieldName, uid, scratch)
    getu(size, opcodeStruct, fieldName, scratch)
    loadp CodeBlock[cfr], uid
    loadp CodeBlock::m_unlinkedCode[uid], uid
    loadp UnlinkedCodeBlock::m_identifiers + IdentifierFixedVector::m_storage[uid], uid
    loadp (constexpr (IdentifierFixedVector::Storage::offsetOfData()))[uid, scratch, PtrSize], uid
end

# AOT::HashedSlotTable::findEntry(). The sets of the table are aligned to their size, so the ways of a set differ in the same bits of
# the address as of the offset. The key is AOT::HashedSlot::keyFor(), which for a read is the uid.
macro findHashedSlot(instance, structureID, uid, makeKey, entry, scratch, miss)
    move structureID, entry
    muli constexpr AOT::HashedSlot::structureMultiplier, entry
    urshifti constexpr AOT::HashedSlot::structureHashShift, entry
    move uid, scratch
    lshifti constexpr AOT::HashedSlot::uidHashShift, scratch
    addi scratch, entry
    andq InstanceHashedSlotOffsetMask[instance], entry
    addp InstanceHashedSlots[instance], entry
    makeKey(uid)
    bpneq uid, HashedSlotKey[entry], .secondWay
    bieq structureID, SlotStructureID[entry], .found
.secondWay:
    xorp HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], .thirdWay
    bieq structureID, SlotStructureID[entry], .found
.thirdWay:
    xorp 3 * HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], .fourthWay
    bieq structureID, SlotStructureID[entry], .found
.fourthWay:
    xorp HashedSlotSize, entry
    bpneq uid, HashedSlotKey[entry], miss
    bineq structureID, SlotStructureID[entry], miss
.found:
end

macro makeKeyOfRead(uid)
end

macro makeKeyOfStore(uid)
    orp constexpr AOT::SlotAccess::Store, uid
end

macro jumpToOrdinaryHandler(size, opcodeName)
    size(macro () jmp _llint_%opcodeName% end, macro () jmp _llint_%opcodeName%_wide16 end, macro () jmp _llint_%opcodeName%_wide32 end, macro (jump) jump() end)
end

# Leaves the address of the field in address and its AOT::SlotRepresentation in representation.
macro countBits(value, scratch, mask)
    # The stubs of compiled code count with the same instruction.
    if X86_64
        popcntq value, value
    else
        move value, scratch
        urshiftq 1, scratch
        move 0x5555555555555555, mask
        andq mask, scratch
        subq scratch, value
        move value, scratch
        urshiftq 2, scratch
        move 0x3333333333333333, mask
        andq mask, scratch
        andq mask, value
        addq scratch, value
        move value, scratch
        urshiftq 4, scratch
        addq scratch, value
        move 0x0f0f0f0f0f0f0f0f, mask
        andq mask, value
        move 0x0101010101010101, mask
        mulq mask, value
        urshiftq 56, value
    end
end

# computeFieldAddress() in the stubs. A field outside the cell lies in a granule of the Extension, which holds only the granules whose bits are set.
macro locateField(instance, entry, object, address, representation, scratch, secondScratch, cannot)
    btbnz SlotFlags[entry], constexpr AOT::Slot::isOutOfLine, .isOutOfLine
    loadi SlotPayload[entry], address
    move address, representation
    urshifti constexpr AOT::SlotData::offsetBits, representation
    andi constexpr AOT::SlotData::maxOffset, address
    addp object, address
    jmp .located

.isOutOfLine:
    loadi JSCell::m_structureID[object], address
    structureIDToStructureWithScratch(address, representation)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtensionSlot()))[address], address
    move address, representation
    urshifti constexpr AOT::SlotData::offsetBits, representation
    bineq representation, constexpr AOT::SlotRepresentation::Cell32, cannot
    andi constexpr AOT::SlotData::maxOffset, address
    loadi [object, address], address
    lshiftp constexpr AOT::cell32Shift, address
    addp InstanceCellCageBase[instance], address
    # The word of an object without an Extension names one that holds no granule.
    addp constexpr AOT::offsetOfEmptyExtensionInCage, address
    # The bit of the granule goes to the top and the bits of later granules drop out. What is left counts the granule and those before it,
    # which is also the number of words from the start of the Extension to the granule. A shift takes its amount modulo 64.
    loadi SlotPayload[entry], representation
    andi constexpr AOT::SlotData::maxOffset, representation
    urshifti 3, representation
    negi representation
    loadq (constexpr AOT::offsetOfExtensionGranuleBits)[address], scratch
    lshiftq representation, scratch
    bqgteq scratch, 0, cannot
    countBits(scratch, representation, secondScratch)
    leap [address, scratch, 8], address
    loadi SlotPayload[entry], representation
    move representation, scratch
    andi (constexpr AOT::extensionGranuleSize) - 1, scratch
    addp scratch, address
    urshifti constexpr AOT::SlotData::offsetBits, representation
.located:
end

# AOT::readSlot(), for the representations that take no more than the bits of the slot. A word below AOT::minCell32 is a marker, which is
# the JSValue it stands for. The markers of a double are NaNs.
macro readField(instance, address, representation, result, isAbsent, cannot, done)
    bieq representation, constexpr AOT::SlotRepresentation::Cell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::JSValue, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalInt32, .optionalInt32
    bieq representation, constexpr AOT::SlotRepresentation::OptionalCell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::OptionalDouble, .double
    bieq representation, constexpr AOT::SlotRepresentation::Double, .double
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, .boolean
    bieq representation, constexpr AOT::SlotRepresentation::Int32, .int32
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalBoolean, .optionalBoolean
    bieq representation, constexpr AOT::SlotRepresentation::DeferredClosure, .deferredClosure
    bieq representation, constexpr AOT::SlotRepresentation::OptionalInt64, .integer
    bieq representation, constexpr AOT::SlotRepresentation::Int64, .integer
    jmp cannot

    # Only the object makes the closure that its slot still defers.
.deferredClosure:
    loadi [address], result
    bib result, constexpr AOT::minCell32, cannot
    lshiftp constexpr AOT::cell32Shift, result
    addp InstanceCellCageBase[instance], result
    jmp done

.narrowReference:
    loadi [address], result
    bib result, constexpr AOT::minCell32, .marker
    lshiftp constexpr AOT::cell32Shift, result
    addp InstanceCellCageBase[instance], result
    jmp done

.boxed:
    loadq [address], result
    bqaeq result, constexpr AOT::minCell32, done
.marker:
    btqz result, isAbsent
    bqa result, ValueUndefined, cannot
    jmp done

.optionalInt32:
    loadi [address], result
    bilt result, constexpr AOT::minInlineInt32, .int32Marker
    orq numberTag, result
    jmp done
.int32Marker:
    bieq result, constexpr AOT::absentInt32, isAbsent
    bieq result, constexpr AOT::undefinedInt32, .undefined
    bieq result, constexpr AOT::nullInt32, .null
    jmp cannot

.int32:
    loadi [address], result
    orq numberTag, result
    jmp done

    # The markers of an integer are above every integer that a slot holds.
.integer:
    loadq [address], result
    sxi2q result, representation
    bqneq result, representation, .exceedsInt32
    zxi2q result, result
    orq numberTag, result
    jmp done
.exceedsInt32:
    move constexpr AOT::undefinedInteger, representation
    bqeq result, representation, .undefined
    move constexpr AOT::absentInteger, representation
    bqeq result, representation, isAbsent
    move constexpr AOT::nullInteger, representation
    bqeq result, representation, .null
    cq2ds result, ft0
    fd2q ft0, result
    subq numberTag, result
    jmp done

.double:
    loadd [address], ft0
    bdnequn ft0, ft0, .isNotANumber
    bcd2i ft0, result, .isNotInt32
    orq numberTag, result
    jmp done
.isNotInt32:
    fd2q ft0, result
    subq numberTag, result
    jmp done
.isNotANumber:
    bineq representation, constexpr AOT::SlotRepresentation::OptionalDouble, cannot
    loadq [address], result
    move constexpr AOT::absentDouble, representation
    bqeq result, representation, isAbsent
    move constexpr AOT::undefinedDouble, representation
    bqeq result, representation, .undefined
    move constexpr AOT::nullDouble, representation
    bqneq result, representation, cannot
.null:
    move ValueNull, result
    jmp done
.undefined:
    move ValueUndefined, result
    jmp done

.boolean:
    loadb [address], result
    orq ValueFalse, result
    jmp done

.optionalBoolean:
    loadb [address], result
    btqz result, isAbsent
    jmp done
end

# AOT::slotHoldsAbsent().
macro branchOnFieldPresence(address, representation, scratch, isPresent, isAbsent, cannot)
    bieq representation, constexpr AOT::SlotRepresentation::Cell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::JSValue, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalInt32, .optionalInt32
    bieq representation, constexpr AOT::SlotRepresentation::OptionalCell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::OptionalDouble, .optionalDouble
    bieq representation, constexpr AOT::SlotRepresentation::Double, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Int32, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalBoolean, .optionalBoolean
    bieq representation, constexpr AOT::SlotRepresentation::Int64, isPresent
    bineq representation, constexpr AOT::SlotRepresentation::OptionalInt64, cannot
    move constexpr AOT::absentInteger, scratch
    bqeq [address], scratch, isAbsent
    jmp isPresent

.optionalBoolean:
    btbz [address], isAbsent
    jmp isPresent

.narrowReference:
    btiz [address], isAbsent
    jmp isPresent

.boxed:
    btqz [address], isAbsent
    jmp isPresent

.optionalInt32:
    bieq [address], constexpr AOT::absentInt32, isAbsent
    jmp isPresent

.optionalDouble:
    move constexpr AOT::absentDouble, scratch
    bqeq [address], scratch, isAbsent
    jmp isPresent
end

# The word index of a property counts from the start of the cell, or down from the butterfly.
macro loadPropertyAtWordIndex(holder, wordIndex, result)
    bigteq wordIndex, 0, .isInline
    loadp JSObjectWithButterfly::m_butterfly[holder], holder
.isInline:
    loadq [holder, wordIndex, 8], result
end

macro loadExtraProperties(instance, object, structureID, result, scratch, cannot)
    structureIDToStructureWithScratch(structureID, scratch)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesSlot()))[structureID], result
    move result, scratch
    urshifti constexpr AOT::SlotData::offsetBits, scratch
    bineq scratch, constexpr AOT::SlotRepresentation::Cell32, cannot
    andi constexpr AOT::SlotData::maxOffset, result
    loadi [object, result], result
    btiz result, cannot
    lshiftp constexpr AOT::cell32Shift, result
    addp InstanceCellCageBase[instance], result
end

# AOT::uidOfKey().
macro loadNameOfKey(key, uid, isNoName)
    bbeq JSCell::m_type[key], StringType, .string
    bbneq JSCell::m_type[key], SymbolType, isNoName
    loadp (constexpr (Symbol::offsetOfSymbolImpl()))[key], uid
    jmp .done
.string:
    loadp JSString::m_fiber[key], uid
    btpnz uid, isRopeInPointer, isNoName
    btiz StringImpl::m_hashAndFlags[uid], constexpr StringImpl::s_hashFlagStringKindIsAtom, isNoName
.done:
end

# PropertyTable::findImpl() for an atom that is no symbol. t0: the object, t1: the name, which stay. Leaves the offset and the attributes
# as a compact entry has them in t2.
macro findInPropertyTable(isAbsent, cannot)
    loadi JSCell::m_structureID[t0], t2
    structureIDToStructureWithScratch(t2, t3)
    loadp (constexpr (Structure::propertyTableUnsafeOffset()))[t2], t2
    btpz t2, cannot
    loadp (constexpr (PropertyTable::offsetOfIndexVector()))[t2], t5
    loadi (constexpr (PropertyTable::offsetOfIndexSize()))[t2], t6
    loadi StringImpl::m_hashAndFlags[t1], t7
    urshifti constexpr (StringImpl::s_flagCount), t7
    move 0, t3
    btpz t5, (constexpr PropertyTable::flagOfCompactIndexVector), .nextWide

.next:
    subi t6, 1, t2
    andi t2, t7
    loadb -1[t5, t7, 1], t2
    btiz t2, isAbsent
    leap (-1 - constexpr (sizeof(CompactPropertyTableEntry)))[t5, t2, 8], t2
    loadq [t2, t6, 1], t2
    xorq t1, t2
    rrotateq 48, t2
    bqbeq t2, 0xffff, .isFound
    addi 1, t3
    addi t3, t7
    jmp .next

.nextWide:
    subi t6, 1, t2
    andi t2, t7
    loadi [t5, t7, 4], t2
    btiz t2, isAbsent
    lshiftq 4, t2
    leap (- constexpr (sizeof(PropertyTableEntry)))[t2, t6, 4], t2
    addp t5, t2
    bpeq [t2], t1, .isFoundInWide
    addi 1, t3
    addi t3, t7
    jmp .nextWide

.isFoundInWide:
    loadi 8[t2], t3
    bia t3, 0x7fffff, cannot
    lshifti 8, t3
    loadb 12[t2], t2
    ori t3, t2
.isFound:
end

# The kinds that any object may have. t3: the object, t5: the instance, t0: the kind of the slot in entry. Leaves the value in t0.
# Goes to callsCustomGetter with the AOT::CustomAccessorRecord in t0.
macro getFromHashedSlot(entry, slow, callsCustomGetter, done)
    bbeq t0, constexpr AOT::Slot::Kind::Value, .value
    bbeq t0, constexpr AOT::Slot::Kind::Absent, .undefined
    bbeq t0, constexpr AOT::Slot::Kind::CustomGetter, .customGetter
    bbneq t0, constexpr AOT::Slot::Kind::Constant, slow
    loadq SlotPayload[entry], t0
    jmp done

    # AOT::callCustomGetterAndReturn(). The object may hold what its getter answered before.
.customGetter:
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    bpneq JSGlobalObject::m_aotInstance[t0], t5, slow
    loadi SlotPayload[entry], t0
    lshiftp constexpr AOT::Interpreter::customAccessorRecordShift, t0
    addp InstanceCustomAccessors[t5], t0
    loadi (constexpr (OBJECT_OFFSETOF(AOT::CustomAccessorRecord, offsetOfCachedValue)))[t0], t1
    btiz t1, callsCustomGetter
    loadq [t3, t1], t1
    btqz t1, callsCustomGetter
    move t1, t0
    jmp done

.undefined:
    move ValueUndefined, t0
    jmp done

.value:
    loadhsq SlotLocation[entry], t1
    btbnz SlotFlags[entry], constexpr AOT::Slot::isOwn, .hasHolder
    loadp SlotPayload[entry], t3
.hasHolder:
    loadPropertyAtWordIndex(t3, t1, t0)
    jmp done
end

# t3: the compiled object, t1: the uid. Leaves the value in t0.
macro getFromCompiledObject(slow, done)
    loadTableInstance(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::ExtraProperty, .extraProperty
    bbeq t0, constexpr AOT::Slot::Kind::InheritedField, .inheritedField
    bbeq t0, constexpr AOT::Slot::Kind::FieldOrInherited, .field
    bbeq t0, constexpr AOT::Slot::Kind::FieldOrInheritedField, .field
    bbeq t0, constexpr AOT::Slot::fieldOfGetter, .field
    getFromHashedSlot(t6, slow, slow, done)

    # The value of a data property does not depend on the receiver, so the holder is asked for it as for a field of its own.
.inheritedField:
    loadp SlotPayload[t6], t3
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    bbneq SlotKind[t6], constexpr AOT::Slot::Kind::Field, slow

.field:
    locateField(t5, t6, t3, t1, t2, t0, t7, slow)
    readField(t5, t1, t2, t0, .absentField, slow, done)

.absentField:
    bbeq SlotKind[t6], constexpr AOT::Slot::Kind::FieldOrInherited, .inherits
    bbneq SlotKind[t6], constexpr AOT::Slot::Kind::Field, slow
    btbz SlotFlags[t6], constexpr AOT::Slot::absentIsUndefined, slow
    move ValueUndefined, t0
    jmp done

    # loadInheritedValueBySlot() in the stubs.
.inherits:
    btbnz SlotFlags[t6], constexpr AOT::Interpreter::inheritedValueSlowPathFlags, slow
    loadi SlotSecondPayloadWord[t6], t0
    lshiftp constexpr AOT::cell32Shift, t0
    addp InstanceCellCageBase[t5], t0
    loadhsq SlotLocation[t6], t1
    loadPropertyAtWordIndex(t0, t1, t0)
    jmp done

.extraProperty:
    loadExtraProperties(t5, t3, t2, t0, t1, slow)
    loadis SlotSecondPayloadWord[t6], t1
    loadPropertyAtWordIndex(t0, t1, t0)
    jmp done
end

# t3: the object, t1: the uid.
macro testPresenceInObject(slow, isPresent, isAbsent)
    loadTableInstance(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Value, isPresent
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::Absent, isAbsent
    move 1, t1
    lshifti t0, t1
    btinz t1, constexpr AOT::presentPropertyKinds, isPresent
    jmp slow

.field:
    locateField(t5, t6, t3, t1, t2, t0, t7, slow)
    branchOnFieldPresence(t1, t2, t0, isPresent, .absentField, slow)

.absentField:
    btbnz SlotFlags[t6], constexpr AOT::Slot::absentIsUndefined, isAbsent
    jmp slow
end

aotOp(op_get_by_id, OpGetById, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadOperandInRegister(size, t0, t3, .baseIsConstant)
.hasBase:
    btqnz t3, notCellMask, .slow
    # The offset of a site in another mode than the default one is negative, if its Structure matches: setupGetByIdPrototypeCache().
    locateSite(size, OpGetById, t2, t1)
    loadi JSCell::m_structureID[t3], t1
    if X86_64
        bineq t1, OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[metadataTable, t2, 1], .otherStructure
        loadis OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset[metadataTable, t2, 1], t1
    else
        bineq t1, OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], .otherStructure
        loadis OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset[t2], t1
    end
    biaeq t1, firstOutOfLineOffset, .outOfLine
    loadq sizeof JSObjectWithButterfly[t3, t1, 8], t0
    return(t0)

.outOfLine:
    makeSiteAddress(t2)
    biaeq t1, FieldInSiteCache, .isNotInButterfly
    loadp JSObjectWithButterfly::m_butterfly[t3], t0
    negi t1
    sxi2q t1, t1
    loadq (firstOutOfLineOffset - 2) * 8[t0, t1, 8], t0
    return(t0)

.isNotInButterfly:
    bilt t1, 0, .otherStructureMode
    bigteq t1, NarrowReferenceInSiteCache, .narrowReference
    bigteq t1, ExtraPropertyInSiteCache, .extraProperty
    subi FieldInSiteCache, t1
    loadq sizeof JSObjectWithButterfly[t3, t1, 8], t0
    btqz t0, .compiledObject
    return(t0)

.extraProperty:
    loadh OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset + 2[t2], t0
    andi constexpr AOT::Interpreter::maxHiddenWordOffsetInSiteCache, t0
    loadi [t3, t0], t0
    btiz t0, .compiledObject
    loadTableInstance(t5, .slow)
    lshiftp constexpr AOT::cell32Shift, t0
    addp InstanceCellCageBase[t5], t0
    loadhsq OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset[t2], t1
    loadPropertyAtWordIndex(t0, t1, t0)
    return(t0)

    # A word below AOT::minCell32 is an empty slot, a marker or a closure that the object has yet to make.
.narrowReference:
    subi NarrowReferenceInSiteCache, t1
    loadi [t3, t1], t0
    bib t0, constexpr AOT::minCell32, .compiledObject
    loadTableInstance(t5, .slow)
    lshiftp constexpr AOT::cell32Shift, t0
    addp InstanceCellCageBase[t5], t0
    return(t0)

.otherStructureMode:
    bbneq OpGetById::Metadata::m_modeMetadata.mode[t2], constexpr GetByIdMode::ProtoLoad, .isAbsent
    loadp OpGetById::Metadata::m_modeMetadata.protoLoadMode.cachedSlot[t2], t0
    andi constexpr AOT::Interpreter::otherModeOffsetMask, t1
    bigteq t1, firstOutOfLineOffset, .methodIsOutOfLine
    loadq sizeof JSObjectWithButterfly[t0, t1, 8], t0
    return(t0)

.methodIsOutOfLine:
    loadp JSObjectWithButterfly::m_butterfly[t0], t0
    negi t1
    sxi2q t1, t1
    loadq (firstOutOfLineOffset - 2) * 8[t0, t1, 8], t0
    return(t0)

.isAbsent:
    return(ValueUndefined)

.otherMode:
    bbeq JSCell::m_type[t3], CompiledObjectType, .compiledObject
    makeSiteAddress(t2)
    bbneq OpGetById::Metadata::m_modeMetadata.mode[t2], constexpr GetByIdMode::ArrayLength, .probe
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    btiz t0, IsArray, .probe
    btiz t0, IndexingShapeMask, .probe
    loadp JSObjectWithButterfly::m_butterfly[t3], t0
    loadi -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t0], t0
    bilt t0, 0, .probe
    orq numberTag, t0
    return(t0)

.otherStructure:
    if X86_64
        bbneq OpGetById::Metadata::m_modeMetadata.mode[metadataTable, t2, 1], constexpr GetByIdMode::Default, .otherMode
    else
        bbneq OpGetById::Metadata::m_modeMetadata.mode[t2], constexpr GetByIdMode::Default, .otherMode
    end
    makeSiteAddress(t2)
    btiz OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], .holdsNothing
    # The first object that came by need not be like those that follow: JSON.parse() makes an ordinary object of a shape that it sees for the first time and records from then on.
    # So the objects of other Structures are counted up to zero, where the slow path lays what has come now in the cache and sets a wait twice as long: startWaitForOtherStructure().
    baddiz 1, OpGetById::Metadata::m_modeMetadata.defaultMode.padding1[t2], .slow
    bbneq JSCell::m_type[t3], CompiledObjectType, .probe

.compiledObject:
    loadIdentifier(size, OpGetById, m_property, t1, t0)
    getFromCompiledObject(.slow, .done)

    # The slow path sees whether the cache of a site that holds nothing yet can hold the property, and takes a site that cannot out of the default mode.
.holdsNothing:
    bbeq JSCell::m_type[t3], CompiledObjectType, .slow
    bbeq JSCell::m_type[t3], constexpr ProxyObjectType, .slow

    # What the cache of the site does not hold.
.probe:
    loadi OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], t7
    bbb JSCell::m_type[t3], ObjectType, .baseIsNotObject
.mayBeFiled:
    loadTableInstance(t5, .slow)
    loadIdentifier(size, OpGetById, m_property, t1, t0)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, .isAbsentFromTable)
    loadb SlotKind[t6], t0
    # Compiled code may have filed the property before the site ran for the first time. Only the slow path fills the cache of the site.
    btinz t7, .readsHashedSlot
    bbneq t0, constexpr AOT::Slot::Kind::Value, .readsHashedSlot
    btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .slow
.readsHashedSlot:
    getFromHashedSlot(t6, .otherKind, .callsCustomGetter, .done)

.otherKind:
    if X86_64 or ARM64
        bbneq t0, constexpr AOT::Slot::Kind::Getter, .isNoGetter
        move t3, t2
        loadhsq SlotLocation[t6], t1
        btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .getterHasHolder
        loadp SlotPayload[t6], t3
    .getterHasHolder:
        loadPropertyAtWordIndex(t3, t1, t0)
        btqz t0, .slow
        btqnz t0, notCellMask, .slow
        bbneq JSCell::m_type[t0], constexpr GetterSetterType, .slow
        loadp (constexpr (GetterSetter::offsetOfGetter()))[t0], t0
        bbneq JSCell::m_type[t0], JSFunctionType, .slow
        loadp JSFunction::m_executableOrRareData[t0], t5
        btpz t5, (constexpr JSFunction::rareDataTag), .hasExecutable
        loadp (FunctionRareData::m_executable - (constexpr JSFunction::rareDataTag))[t5], t5
    .hasExecutable:
        loadp ExecutableBase::m_jitCodeForCallWithArityCheck[t5], t1
        btpz t1, .slow
        move 0, t3
        bbneq JSCell::m_type[t5], FunctionExecutableType, .hasCodeBlock
        loadp FunctionExecutable::m_codeBlockForCall[t5], t3
    .hasCodeBlock:
        # A frame for a callee without arguments lies behind this one.
        storePC()
        subp FrameSizeForGetter - CallerFrameAndPCSize, sp
        storep t3, CodeBlock - CallerFrameAndPCSize[sp]
        storep t0, Callee - CallerFrameAndPCSize[sp]
        storei 1, ArgumentCountIncludingThis + LowWordOffset - CallerFrameAndPCSize[sp]
        storeq t2, ThisArgumentOffset - CallerFrameAndPCSize[sp]
        call t1, JSEntryPtrTag
        restoreStackPointerAfterCall()
        loadPC()
        get(m_dst, t1)
        storeq r0, [cfr, t1, 8]
        dispatch()
    .isNoGetter:
    end

    # As in getFromCompiledObject(): the holder is asked as for a field of its own.
    bbneq SlotKind[t6], constexpr AOT::Slot::Kind::InheritedField, .slow
    loadp SlotPayload[t6], t3
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, .slow)
    bbneq SlotKind[t6], constexpr AOT::Slot::Kind::Field, .slow
    locateField(t5, t6, t3, t1, t2, t0, t7, .slow)
    readField(t5, t1, t2, t0, .fieldAboveIsAbsent, .slow, .done)

.fieldAboveIsAbsent:
    btbz SlotFlags[t6], constexpr AOT::Slot::absentIsUndefined, .slow
    move ValueUndefined, t0

.done:
    return(t0)

    # t0: the AOT::CustomAccessorRecord, t3: the object.
.callsCustomGetter:
    move t0, a2
    move t3, a3
    prepareStateForCCall()
    move cfr, a0
    move PC, a1
    cCall4(_llint_slow_path_aot_call_custom_getter)
    restoreStateAfterCCall()
    dispatch()

.isAbsentFromTable:
    callSlowPath(_llint_slow_path_aot_get_uncached)
    dispatch()

.baseIsNotObject:
    # Its Structure is the VM's, its prototype the realm's.
    loadRealmInstance(t5, .slow)
    bbeq JSCell::m_type[t3], StringType, .mayBeFiled
.slow:
    callSlowPath(_llint_slow_path_get_by_id)
    dispatch()

.baseIsConstant:
    loadConstantOperand(size, t0, t3, .hasBase)
end)

# A data property that the table holds does not depend on the receiver.
aotOp(op_get_by_id_with_this, OpGetByIdWithThis, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadConstantOrVariableCell(size, t0, t3, .slow)
    bbb JSCell::m_type[t3], ObjectType, .slow
    loadTableInstance(t5, .slow)
    loadIdentifier(size, OpGetByIdWithThis, m_property, t1, t0)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, .slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::CustomGetter, .slow
    getFromHashedSlot(t6, .slow, .slow, .done)

.done:
    return(t0)

.slow:
    callSlowPath(_llint_slow_path_aot_get_by_id_with_this)
    dispatch()
end)

# The index is in bounds. A character that takes more than one byte has no string of its own in SmallStrings.
macro loadStringCharacter(impl, index, character, isWide)
    btiz StringImpl::m_hashAndFlags[impl], HashFlags8BitBuffer, .has16BitCharacters
    loadp StringImpl::m_data8[impl], impl
    loadb [impl, index], character
    jmp .hasCharacter
.has16BitCharacters:
    loadp StringImpl::m_data8[impl], impl
    loadh [impl, index, 2], character
    bia character, constexpr maxSingleCharacterString, isWide
.hasCharacter:
end

aotOp(op_get_by_val, OpGetByVal, macro (size, get, dispatch, metadata, return)
    # An element that an array of values holds. Neither profile of the ordinary handler has a reader here.
    get(m_base, t1)
    loadOperandInRegister(size, t1, t0, .baseIsConstant)
.hasBase:
    btqnz t0, notCellMask, .otherAccess
    get(m_property, t1)
    loadOperandInRegister(size, t1, t3, .indexIsConstant)
.hasIndex:
    bqb t3, numberTag, .isNoIndex
    loadb JSCell::m_indexingTypeAndMisc[t0], t2
    andi IndexingShapeMask, t2
    bieq t2, ContiguousShape, .holdsValues
    bieq t2, DoubleShape, .holdsDoubles
    bineq t2, Int32Shape, .holdsNoValues
.holdsValues:
    loadp JSObjectWithButterfly::m_butterfly[t0], t1
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t1], .otherAccess
    zxi2q t3, t3
    loadq [t1, t3, 8], t0
    btqz t0, .otherAccess
    return(t0)

.baseIsConstant:
    loadConstantOperand(size, t1, t0, .hasBase)

.indexIsConstant:
    loadConstantOperand(size, t1, t3, .hasIndex)

.holdsDoubles:
    loadp JSObjectWithButterfly::m_butterfly[t0], t1
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t1], .otherAccess
    zxi2q t3, t3
    loadd [t1, t3, 8], ft0
    bdnequn ft0, ft0, .otherAccess
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

    # A character of a whole string, as a string.
.holdsNoValues:
    bbneq JSCell::m_type[t0], StringType, .otherAccess
    loadp JSString::m_fiber[t0], t1
    btpnz t1, isRopeInPointer, .otherAccess
    biaeq t3, StringImpl::m_length[t1], .otherAccess
    zxi2q t3, t3
    loadStringCharacter(t1, t3, t2, .otherAccess)
    loadp CodeBlock[cfr], t1
    loadp CodeBlock::m_vm[t1], t1
    loadp VM::smallStrings + SmallStrings::m_singleCharacterStrings[t1, t2, PtrSize], t0
    return(t0)

    # An own value of a plain object.
.isNoIndex:
    btqnz t3, notCellMask, .otherAccess
    bbneq JSCell::m_type[t0], FinalObjectType, .otherAccess
    bbneq JSCell::m_type[t3], StringType, .otherAccess
    loadNameOfKey(t3, t1, .otherAccess)
    findInPropertyTable(.otherAccess, .otherAccess)
    btinz t2, constexpr PropertyAttribute::AccessorOrCustomAccessorOrValue, .otherAccess
    urshifti 8, t2
    loadPropertyAtVariableOffset(t2, t0, t1)
    return(t1)

.otherAccess:
    getByVal(size, get, dispatch, metadata, return, macro ()
        get(m_base, t0)
        loadConstantOrVariableCell(size, t0, t3, .slow)
        bbneq JSCell::m_type[t3], CompiledObjectType, .slow
        get(m_property, t0)
        loadConstantOrVariableCell(size, t0, t2, .slow)
        loadNameOfKey(t2, t1, .slow)
        getFromCompiledObject(.slow, .done)

    .done:
        return(t0)

    .slow:
        callSlowPath(_llint_slow_path_get_by_val)
        dispatch()
    end)
end)

aotOp(op_in_by_id, OpInById, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadOperandInRegister(size, t0, t3, .baseIsConstant)
.hasBase:
    btqnz t3, notCellMask, .slow
    bbb JSCell::m_type[t3], ObjectType, .slow
    loadIdentifier(size, OpInById, m_property, t1, t0)
    testPresenceInObject(.slow, .true, .false)

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_llint_slow_path_aot_in_by_id)
    dispatch()

.baseIsConstant:
    loadConstantOperand(size, t0, t3, .hasBase)
end)

aotOp(op_in_by_val, OpInByVal, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadConstantOrVariableCell(size, t0, t3, .slow)
    bbb JSCell::m_type[t3], ObjectType, .slow
    get(m_property, t0)
    loadConstantOrVariable(size, t0, t2)
    bqaeq t2, numberTag, .isIndex
    btqnz t2, notCellMask, .slow
    loadNameOfKey(t2, t1, .slow)
    testPresenceInObject(.slow, .true, .false)

    # An element that an array has.
.isIndex:
    loadb JSCell::m_indexingTypeAndMisc[t3], t1
    andi IndexingShapeMask, t1
    bieq t1, ContiguousShape, .mayHoldValue
    bieq t1, Int32Shape, .mayHoldValue
    bineq t1, DoubleShape, .slow
    loadp JSObjectWithButterfly::m_butterfly[t3], t1
    biaeq t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t1], .slow
    zxi2q t2, t2
    loadd [t1, t2, 8], ft0
    bdnequn ft0, ft0, .slow
    return(ValueTrue)

.mayHoldValue:
    loadp JSObjectWithButterfly::m_butterfly[t3], t1
    biaeq t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t1], .slow
    zxi2q t2, t2
    btqz [t1, t2, 8], .slow

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_llint_slow_path_aot_in_by_val)
    dispatch()
end)

# t5: the instance, t2: the StructureID of holder, t1: the uid. A data property that the hashed table holds, own or inherited. Clobbers holder. Leaves the value in t0.
macro getValueFromHashedSlot(holder, slow)
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    bbneq SlotKind[t6], constexpr AOT::Slot::Kind::Value, slow
    loadhsq SlotLocation[t6], t1
    btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .hasHolder
    loadp SlotPayload[t6], holder
.hasHolder:
    loadPropertyAtWordIndex(holder, t1, t0)
end

# AOT::generateInstanceOf(): the constructor's Symbol.hasInstance and "prototype" out of the hashed table, then JSObject::defaultHasInstance().
aotOp(op_instanceof, OpInstanceof, macro (size, get, dispatch, metadata, return)
    get(m_constructor, t0)
    loadOperandInRegister(size, t0, t3, .slow)
    btqnz t3, notCellMask, .slow
    bbb JSCell::m_type[t3], ObjectType, .slow
    btbz JSCell::m_flags[t3], ImplementsDefaultHasInstance, .slow
    loadTableInstance(t5, .slow)
    loadi JSCell::m_structureID[t3], t2
    loadp InstanceHasInstanceUID[t5], t1
    move t3, t7
    getValueFromHashedSlot(t7, .slow)
    bpneq t0, InstanceDefaultHasInstanceFunction[t5], .slow

    get(m_value, t0)
    loadOperandInRegister(size, t0, t7, .slow)
    btqnz t7, notCellMask, .false
    bbb JSCell::m_type[t7], ObjectType, .false

    loadp InstancePrototypeUID[t5], t1
    getValueFromHashedSlot(t3, .slow)
    btqnz t0, notCellMask, .slow
    bbb JSCell::m_type[t0], ObjectType, .slow

.nextLink:
    btbnz JSCell::m_flags[t7], OverridesGetPrototype, .slow
    loadStructureWithScratch(t7, t1, t2)
    loadq Structure::m_prototype[t1], t1
    btqz t1, .hasPolyProto
.hasPrototype:
    bqeq t1, t0, .true
    btqnz t1, notCellMask, .false
    move t1, t7
    jmp .nextLink

.hasPolyProto:
    move knownPolyProtoOffset, t1
    loadInlineOffset(t1, t7, t2)
    move t2, t1
    jmp .hasPrototype

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_llint_slow_path_aot_instanceof)
    dispatch()
end)

# The enumerator that a loop over a compiled object with this Structure and order word got last. KeyOrderTable::publishEnumerator() lays it there, and compiled code reads it as this does: ForInEmitter.
# Of any other object, as the ordinary handler has it.
aotOp(op_get_property_enumerator, OpGetPropertyEnumerator, macro (size, get, dispatch, metadata, return)
    get(m_base, t1)
    loadConstantOrVariableCell(size, t1, t0, .slow)
    bbeq JSCell::m_type[t0], CompiledObjectType, .compiledObject

    loadb JSCell::m_indexingTypeAndMisc[t0], t1
    andi IndexingTypeMask, t1
    bia t1, ArrayWithUndecided, .slow
    loadStructureWithScratch(t0, t1, t2)
    loadp Structure::m_previousOrRareData[t1], t1
    btpz t1, .slow
    bbeq JSCell::m_type[t1], StructureType, .slow
    loadp StructureRareData::m_cachedPropertyNameEnumeratorAndFlag[t1], t1
    btpz t1, .slow
    btpnz t1, (constexpr StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag), .slow
    return(t1)

.compiledObject:
    loadTableInstance(t5, .slow)
    loadi JSCell::m_structureID[t0], t1
    loadi (constexpr AOT::offsetOfOrderWord)[t0], t2
    move t1, t3
    urshifti constexpr AOT::enumeratorHashShift, t3
    addi t2, t3
    andi (constexpr AOT::numberOfCachedEnumerators) - 1, t3
    lshiftp 4, t3
    addp InstanceEnumerators[t5], t3
    bineq t1, (constexpr (OBJECT_OFFSETOF(AOT::EnumeratorCacheEntry, structureID)))[t3], .slow
    bineq t2, (constexpr (OBJECT_OFFSETOF(AOT::EnumeratorCacheEntry, orderWord)))[t3], .slow
    loadp (constexpr (OBJECT_OFFSETOF(AOT::EnumeratorCacheEntry, enumerator)))[t3], t0
    return(t0)

.slow:
    callSlowPath(_slow_path_get_property_enumerator)
    dispatch()
end)

# JSPropertyNameEnumerator::computeNext() for an enumerator that is current for a compiled object, whose mode is always GenericMode, and at both ends of a loop, which the ordinary handler leaves to the slow path.
aotOp(op_enumerator_next, OpEnumeratorNext, macro (size, get, dispatch, metadata, return)
    enumeratorNext(size, get, dispatch, macro ()
        loadVariable(get, m_mode, t0)
        bbeq t0, constexpr JSPropertyNameEnumerator::InitMode, .first
        bbeq t0, constexpr JSPropertyNameEnumerator::OwnStructureMode, .last
        bbneq t0, constexpr JSPropertyNameEnumerator::GenericMode, .slow
        get(m_base, t1)
        loadConstantOrVariableCell(size, t1, t0, .slow)
        bbneq JSCell::m_type[t0], CompiledObjectType, .slow
        loadVariable(get, m_enumerator, t1)
        loadi JSPropertyNameEnumerator::m_cachedStructureID[t1], t2
        bineq t2, JSCell::m_structureID[t0], .slow
        loadi (constexpr AOT::offsetOfOrderWord)[t0], t2
        andi JSPropertyNameEnumerator::m_cachedOrderWordMask[t1], t2
        bineq t2, JSPropertyNameEnumerator::m_cachedOrderWord[t1], .slow

        loadVariable(get, m_index, t2)
        addq 1, t2
        loadi JSPropertyNameEnumerator::m_endOwnPropertyIndex[t1], t3
        biaeq t2, t3, .last

        storeVariable(get, m_index, t2, t3)
        loadp JSPropertyNameEnumerator::m_propertyNames[t1], t3
        zxi2q t2, t2
        loadp [t3, t2, PtrSize], t3
        storeVariable(get, m_propertyName, t3, t2)
        dispatch()

        # The start of a loop over an ordinary object without indexed properties that still has the Structure of the enumerator.
    .first:
        loadVariable(get, m_enumerator, t1)
        btinz JSPropertyNameEnumerator::m_indexedLength[t1], .slow
        get(m_base, t2)
        loadConstantOrVariableCell(size, t2, t0, .slow)
        loadi JSPropertyNameEnumerator::m_cachedStructureID[t1], t2
        bineq t2, JSCell::m_structureID[t0], .slow
        bbeq JSCell::m_type[t0], CompiledObjectType, .firstOfCompiledObject
        btiz JSPropertyNameEnumerator::m_endStructurePropertyIndex[t1], .slow
        loadp JSPropertyNameEnumerator::m_propertyNames[t1], t3
        loadp [t3], t3
        btpz t3, .slow
        storeVariable(get, m_propertyName, t3, t2)
        move constexpr JSPropertyNameEnumerator::OwnStructureMode, t3
        orq numberTag, t3
        storeVariable(get, m_mode, t3, t2)
        storeVariable(get, m_index, numberTag, t2)
        dispatch()

        # The same over a compiled object that still has the order word of the enumerator. Its names go by GenericMode: an index is no offset there.
    .firstOfCompiledObject:
        loadi (constexpr AOT::offsetOfOrderWord)[t0], t2
        andi JSPropertyNameEnumerator::m_cachedOrderWordMask[t1], t2
        bineq t2, JSPropertyNameEnumerator::m_cachedOrderWord[t1], .slow
        btiz JSPropertyNameEnumerator::m_endOwnPropertyIndex[t1], .slow
        loadp JSPropertyNameEnumerator::m_propertyNames[t1], t3
        loadp [t3], t3
        btpz t3, .slow
        storeVariable(get, m_propertyName, t3, t2)
        move constexpr JSPropertyNameEnumerator::GenericMode, t3
        orq numberTag, t3
        storeVariable(get, m_mode, t3, t2)
        storeVariable(get, m_index, numberTag, t2)
        dispatch()

        # Behind the last name, whatever the object has become.
    .last:
        loadVariable(get, m_enumerator, t1)
        loadVariable(get, m_index, t2)
        addq 1, t2
        bib t2, JSPropertyNameEnumerator::m_endGenericPropertyIndex[t1], .slow
        storeVariable(get, m_index, t2, t3)
        loadp CodeBlock[cfr], t3
        loadp CodeBlock::m_vm[t3], t3
        loadp VM::smallStrings + SmallStrings::m_sentinelString[t3], t3
        storeVariable(get, m_propertyName, t3, t2)
        dispatch()

    .slow:
        callSlowPath(_slow_path_enumerator_next)
        dispatch()
    end)
end)

aotOp(op_enumerator_get_by_val, OpEnumeratorGetByVal, macro (size, get, dispatch, metadata, return)
    enumeratorGetByVal(size, get, dispatch, metadata, return, macro ()
        get(m_base, t0)
        loadConstantOrVariableCell(size, t0, t3, .slow)
        bbneq JSCell::m_type[t3], CompiledObjectType, .slow
        loadVariable(get, m_propertyName, t2)
        loadNameOfKey(t2, t1, .miss)
        getFromCompiledObject(.miss, .done)

    .done:
        valueProfile(size, OpEnumeratorGetByVal, m_valueProfile, t0, t2)
        return(t0)

    .miss:
        callSlowPath(_llint_slow_path_aot_enumerator_get_by_val)
        dispatch()

    .slow:
        callSlowPath(_slow_path_enumerator_get_by_val)
        dispatch()
    end)
end)

# Whoever gives a property of a prototype another value tells those who have looked it up from below: the slow path does, and the store by offset of the ordinary handler tells nobody.
aotOp(op_enumerator_put_by_val, OpEnumeratorPutByVal, macro (size, get, dispatch, metadata, return)
    get(m_base, t1)
    loadConstantOrVariableCell(size, t1, t0, .ordinary)
    loadi JSCell::m_structureID[t0], t2
    loadVariable(get, m_enumerator, t1)
    bineq t2, JSPropertyNameEnumerator::m_cachedStructureID[t1], .otherStructure
    structureIDToStructureWithScratch(t2, t3)
    btinz Structure::m_bitField[t2], (constexpr Structure::s_mayBePrototypeBits), .slow
.ordinary:
    jumpToOrdinaryHandler(size, op_enumerator_put_by_val)

.otherStructure:
    bbneq JSCell::m_type[t0], FinalObjectType, .ordinary
    loadVariable(get, m_mode, t1)
    bbeq t1, constexpr JSPropertyNameEnumerator::IndexedMode, .slow
    loadVariable(get, m_propertyName, t2)
    loadNameOfKey(t2, t1, .slow)
    loadTableInstance(t5, .slow)
    loadi JSCell::m_structureID[t0], t2
    findHashedSlot(t5, t2, t1, makeKeyOfStore, t6, t3, .slow)
    loadb SlotKind[t6], t1
    bbeq t1, constexpr AOT::Slot::Kind::Value, .storesValue
    bbneq t1, constexpr AOT::Slot::Kind::Transition, .slow
    btbnz SlotFlags[t6], constexpr AOT::Slot::growsStorage, .slow
    loadi SlotPayload[t6], t2
    storei t2, JSCell::m_structureID[t0]
.storesValue:
    loadhsq SlotLocation[t6], t1
    get(m_value, t2)
    loadConstantOrVariable(size, t2, t3)
    move t0, t6
    bigteq t1, 0, .storesInCell
    loadp JSObjectWithButterfly::m_butterfly[t0], t6
.storesInCell:
    storeq t3, [t6, t1, 8]
    writeBarrierOnCell(t0, t1)
    dispatch()

.slow:
    callSlowPath(_llint_slow_path_aot_enumerator_put_by_val)
    dispatch()
end)

# Heap::writeBarrier(). While the collector marks beside this thread, the threshold sends every cell to the slow path, which has the fence.
macro writeBarrierOnCell(cell, scratch)
    loadp CodeBlock[cfr], scratch
    loadp CodeBlock::m_vm[scratch], scratch
    loadi (constexpr (VM::offsetOfHeapBarrierThreshold()))[scratch], scratch
    bba JSCell::m_cellState[cell], scratch, .isRememberedOrInEden
    push PB, PC
    move cell, a1
    move cfr, a0
    cCall2Void(_llint_write_barrier_slow)
    pop PC, PB
.isRememberedOrInEden:
end

macro branchIfBarrierIsNeeded(cell, scratch, isNeeded)
    loadp CodeBlock[cfr], scratch
    loadp CodeBlock::m_vm[scratch], scratch
    loadi (constexpr (VM::offsetOfHeapBarrierThreshold()))[scratch], scratch
    bbbeq JSCell::m_cellState[cell], scratch, isNeeded
end

# Stores to a field that is in its slot. AOT::RuntimeLayoutTable::fieldAccepts() and AOT::tryWriteSlot(), for the types and representations
# that a test of the value's tag decides, and of an int32 the range of the type.
aotOp(op_put_by_id, OpPutById, macro (size, get, dispatch, metadata, return)
    macro loadSiteOffset(offset)
        if X86_64
            loadb OpPutById::Metadata::m_offset[metadataTable, t5, 1], offset
        else
            loadb OpPutById::Metadata::m_offset[t5], offset
        end
    end

    get(m_base, t1)
    loadOperandInRegister(size, t1, t0, .baseIsConstant)
.hasBase:
    btqnz t0, notCellMask, .ordinary

    # AOT::Interpreter::StoreSiteKind. The cache never holds the Structure of a compiled object. One write barrier stands behind both stores of a transition.
    locateSite(size, OpPutById, t5, t2)
    loadSiteInt(OpPutById, m_oldStructureID, t5, t2)
    bineq t2, JSCell::m_structureID[t0], .otherStructure
    if X86_64
        loadb OpPutById::Metadata::m_offset + 3[metadataTable, t5, 1], t7
    else
        loadb OpPutById::Metadata::m_offset + 3[t5], t7
    end
    bineq t7, constexpr AOT::Interpreter::StoreSiteKind::Adds, .isNoAddition
.adds:
    get(m_value, t1)
    loadOperandInRegister(size, t1, t3, .addedValueIsConstant)
.hasAddedValue:
    loadSiteInt(OpPutById, m_newStructureID, t5, t6)
    loadSiteOffset(t1)
    storei t6, JSCell::m_structureID[t0]
    storeq t3, sizeof JSObjectWithButterfly[t0, t1, 8]
    writeBarrierOnCell(t0, t1)
    dispatch()

.isNoAddition:
    bineq t7, constexpr AOT::Interpreter::StoreSiteKind::Replaces, .isNoReplacement
    get(m_value, t1)
    loadOperandInRegister(size, t1, t3, .newValueIsConstant)
.hasNewValue:
    loadSiteOffset(t1)
    storeq t3, sizeof JSObjectWithButterfly[t0, t1, 8]
    btqnz t3, notCellMask, .stored
    writeBarrierOnCell(t0, t1)
.stored:
    dispatch()

.isNoReplacement:
    loadSitePointer(OpPutById, m_structureChain, t5, t3)
    btpz t3, .chainIsChecked
    structureIDToStructureWithScratch(t2, t1)
    loadp StructureChain::m_vector[t3], t3
    loadq Structure::m_prototype[t2], t2
    bqeq t2, ValueNull, .chainIsChecked
.nextPrototype:
    loadi JSCell::m_structureID[t2], t2
    bineq t2, [t3], .ordinary
    addp 4, t3
    # An empty entry ends the chain, and an object that still has its Structure still has its prototype.
    btiz [t3], .chainIsChecked
    structureIDToStructureWithScratch(t2, t1)
    loadq Structure::m_prototype[t2], t2
    jmp .nextPrototype

.chainIsChecked:
    bieq t7, constexpr AOT::Interpreter::StoreSiteKind::AddsWithChain, .adds

    # The property is beside the cell.
    get(m_value, t1)
    loadConstantOrVariable(size, t1, t3)
    loadSiteInt(OpPutById, m_offset, t5, t1)
    loadp JSObjectWithButterfly::m_butterfly[t0], t2
    negi t1
    sxi2q t1, t1
    loadSiteInt(OpPutById, m_newStructureID, t5, t6)
    btiz t6, .replacesOutOfLine
    storei t6, JSCell::m_structureID[t0]
.replacesOutOfLine:
    storeq t3, (firstOutOfLineOffset - 2) * 8[t2, t1, 8]
    writeBarrierOnCell(t0, t1)
    dispatch()

.otherStructure:
    bbeq JSCell::m_type[t0], CompiledObjectType, .compiledObject
    bbneq JSCell::m_type[t0], FinalObjectType, .ordinary
    getu(size, OpPutById, m_flags, t1)
    btinz t1, constexpr PutByIdFlagsFits::s_isDirectBit, .ordinary
    loadTableInstance(t5, .ordinary)
    loadIdentifier(size, OpPutById, m_property, t1, t2)
    loadi JSCell::m_structureID[t0], t2
    findHashedSlot(t5, t2, t1, makeKeyOfStore, t6, t3, .ordinary)
    loadb SlotKind[t6], t1
    bbeq t1, constexpr AOT::Slot::Kind::Value, .storesValue
    bbeq t1, constexpr AOT::Slot::Kind::Transition, .addsByTransition
    if X86_64 or ARM64
        bbeq t1, constexpr AOT::Slot::Kind::Setter, .setter
    end
.ordinary:
    jumpToOrdinaryHandler(size, op_put_by_id)

.addsByTransition:
    btbnz SlotFlags[t6], constexpr AOT::Slot::growsStorage, .growsStorage
    loadi SlotPayload[t6], t2
    storei t2, JSCell::m_structureID[t0]
.storesValue:
    loadhsq SlotLocation[t6], t1
    get(m_value, t2)
    loadConstantOrVariable(size, t2, t3)
    move t0, t6
    bigteq t1, 0, .storesInCell
    loadp JSObjectWithButterfly::m_butterfly[t0], t6
.storesInCell:
    storeq t3, [t6, t1, 8]
    writeBarrierOnCell(t0, t1)
    dispatch()

    # JSObject::nukeStructureAndSetButterfly(), for an object that has no butterfly yet: a collector that marks beside this thread sees a nuked Structure or the Structure and the butterfly that belong together.
.growsStorage:
    btbnz SlotFlags[t6], constexpr AOT::Slot::storageHasIndexingHeader, .ordinary
    btpnz JSObjectWithButterfly::m_butterfly[t0], .ordinary
    bineq SlotSecondPayloadWord[t6], constexpr initialOutOfLineCapacity, .ordinary
    loadp InstanceAuxiliarySpace[t5], t1
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(initialOutOfLineCapacity * sizeof(EncodedJSValue)) * sizeof(void*)))[t1], t1
    btpz t1, .ordinary
    allocateCell(t1, t2, t3, .ordinary)
    storeq ValueEmpty, [t2]
    storeq ValueEmpty, 8[t2]
    storeq ValueEmpty, 16[t2]
    storeq ValueEmpty, 24[t2]
    addp constexpr (initialOutOfLineCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader)), t2
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t3)
    loadhsq SlotLocation[t6], t1
    storeq t3, [t2, t1, 8]
    fenceAfterAllocation(t5)
    loadi JSCell::m_structureID[t0], t1
    ori constexpr StructureID::nukedStructureIDBit, t1
    storei t1, JSCell::m_structureID[t0]
    fenceAfterAllocation(t5)
    storep t2, JSObjectWithButterfly::m_butterfly[t0]
    fenceAfterAllocation(t5)
    loadi SlotPayload[t6], t1
    storei t1, JSCell::m_structureID[t0]
    writeBarrierOnCell(t0, t1)
    dispatch()

    if X86_64 or ARM64
    .setter:
        move t0, t2
        loadhsq SlotLocation[t6], t1
        btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .setterHasHolder
        loadp SlotPayload[t6], t0
    .setterHasHolder:
        loadPropertyAtWordIndex(t0, t1, t3)
        btqz t3, .ordinary
        btqnz t3, notCellMask, .ordinary
        bbneq JSCell::m_type[t3], constexpr GetterSetterType, .ordinary
        loadp (constexpr (GetterSetter::offsetOfSetter()))[t3], t0
        bbneq JSCell::m_type[t0], JSFunctionType, .ordinary
        loadp JSFunction::m_executableOrRareData[t0], t5
        btpz t5, (constexpr JSFunction::rareDataTag), .setterHasExecutable
        loadp (FunctionRareData::m_executable - (constexpr JSFunction::rareDataTag))[t5], t5
    .setterHasExecutable:
        loadp ExecutableBase::m_jitCodeForCallWithArityCheck[t5], t1
        btpz t1, .ordinary
        move 0, t6
        bbneq JSCell::m_type[t5], FunctionExecutableType, .setterHasCodeBlock
        loadp FunctionExecutable::m_codeBlockForCall[t5], t6
    .setterHasCodeBlock:
        get(m_value, t5)
        loadConstantOrVariable(size, t5, t3)
        # A frame for a callee with one argument lies behind this one.
        storePC()
        subp FrameSizeForSetter - CallerFrameAndPCSize, sp
        storep t6, CodeBlock - CallerFrameAndPCSize[sp]
        storep t0, Callee - CallerFrameAndPCSize[sp]
        storei 2, ArgumentCountIncludingThis + LowWordOffset - CallerFrameAndPCSize[sp]
        storeq t2, ThisArgumentOffset - CallerFrameAndPCSize[sp]
        storeq t3, FirstArgumentOffset - CallerFrameAndPCSize[sp]
        call t1, JSEntryPtrTag
        restoreStackPointerAfterCall()
        loadPC()
        dispatch()
    end

.baseIsConstant:
    loadConstantOperand(size, t1, t0, .hasBase)

.addedValueIsConstant:
    loadConstantOperand(size, t1, t3, .hasAddedValue)

.newValueIsConstant:
    loadConstantOperand(size, t1, t3, .hasNewValue)

.compiledObject:
    getu(size, OpPutById, m_flags, t1)
    btinz t1, constexpr PutByIdFlagsFits::s_isDirectBit, .slow
    loadTableInstance(t5, .slow)
    loadIdentifier(size, OpPutById, m_property, t1, t2)
    loadi JSCell::m_structureID[t0], t2
    findHashedSlot(t5, t2, t1, makeKeyOfStore, t6, t3, .slow)
    loadb SlotKind[t6], t1
    bbeq t1, constexpr AOT::Slot::Kind::Field, .field
    bbeq t1, constexpr AOT::Slot::Kind::ExtraPropertyAddition, .addsExtraProperty
    bbneq t1, constexpr AOT::Slot::Kind::FieldCreation, .slow
    # In place of the type such a slot holds an order word.
    btbnz SlotFlags[t6], constexpr AOT::Slot::remembersOrder, .slow
.field:
    btinz (constexpr AOT::offsetOfOrderWord)[t0], constexpr AOT::orderWordIsFrozen, .slow
    locateField(t5, t6, t0, t1, t2, t3, t7, .slow)
    loadi SlotSecondPayloadWord[t6], t6
    addp InstanceTables[t5], t6
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t3)
    loadh RuntimeTypePrimitives[t6], t5

    # t0: object, t1: address of the field, t2: representation, t3: value, t5: primitives of the type, t6: AOT::RuntimeType
    bqaeq t3, numberTag, .int32
    btqnz t3, numberTag, .double
    btqnz t3, notCellMask, .otherImmediate
    btqz t3, .slow
    loadb JSCell::m_type[t3], t7
    bbaeq t7, ObjectType, .object
    bbneq t7, StringType, .slow
    btiz t5, constexpr AOT::Typed::Primitive::String, .slow
    loadh RuntimeTypeNumberOfLiteralStrings[t6], t7
    btinz t7, .slow
    jmp .storeCell

.object:
    loadh RuntimeTypeNumberOfCellTests[t6], t7
    btiz t7, .slow
    bbneq RuntimeTypeKindOfFirstCellTest[t6], constexpr AOT::CellTestKind::IsObject, .slow
.storeCell:
    bieq t2, constexpr AOT::SlotRepresentation::Cell32, .storeNarrowReference
    bieq t2, constexpr AOT::SlotRepresentation::OptionalCell32, .storeNarrowReference
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedCell
    bineq t2, constexpr AOT::SlotRepresentation::Cell, .slow
.storeBoxedCell:
    btqz [t1], .slow
    storeq t3, [t1]
    jmp .barrier
.storeNarrowReference:
    btiz [t1], .slow
    urshiftq constexpr AOT::cell32Shift, t3
    storei t3, [t1]
.barrier:
    writeBarrierOnCell(t0, t1)
    dispatch()

.int32:
    btiz t5, constexpr AOT::Typed::Primitive::Integer, .slow
    btbz RuntimeTypeFlags[t6], constexpr AOT::RuntimeType::hasIntegerRange, .int32IsInRange
    # AOT::RuntimeType::integerRange(): two integers behind the cell tests, of two words each, and the literal strings, of one.
    loadh RuntimeTypeNumberOfCellTests[t6], t7
    loadh RuntimeTypeNumberOfLiteralStrings[t6], t5
    lshifti 1, t7
    addi t5, t7
    leap RuntimeTypeSize[t6, t7, 4], t7
    sxi2q t3, t5
    bqlt t5, [t7], .slow
    bqgt t5, 8[t7], .slow
.int32IsInRange:
    bieq t2, constexpr AOT::SlotRepresentation::OptionalInt32, .storeOptionalInt32
    bieq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .storeInt32AsOptionalDouble
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::Double, .storeInt32AsDouble
    bineq t2, constexpr AOT::SlotRepresentation::Int32, .slow
    storei t3, [t1]
    dispatch()
.storeOptionalInt32:
    bieq [t1], constexpr AOT::absentInt32, .slow
    bilt t3, constexpr AOT::minInlineInt32, .slow
    storei t3, [t1]
    dispatch()
.storeInt32AsOptionalDouble:
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
.storeInt32AsDouble:
    ci2ds t3, ft0
    stored ft0, [t1]
    dispatch()

.double:
    andi constexpr AOT::Typed::Primitive::Number, t5
    bineq t5, constexpr AOT::Typed::Primitive::Number, .slow
    btbnz RuntimeTypeFlags[t6], constexpr AOT::RuntimeType::hasIntegerRange, .slow
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::Double, .storeDouble
    bineq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .slow
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
.storeDouble:
    addq numberTag, t3
    storeq t3, [t1]
    dispatch()

.otherImmediate:
    bqeq t3, ValueUndefined, .undefined
    bqeq t3, ValueNull, .null
    bqeq t3, ValueTrue, .true
    bqneq t3, ValueFalse, .slow
    btiz t5, constexpr AOT::Typed::Primitive::False, .slow
    jmp .storeBoolean
.true:
    btiz t5, constexpr AOT::Typed::Primitive::True, .slow
.storeBoolean:
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::OptionalBoolean, .storeByteOfImmediate
    bineq t2, constexpr AOT::SlotRepresentation::Boolean, .slow
    andi 1, t3
    storeb t3, [t1]
    dispatch()
# A marker that the type of the field does not list is the run time's: AOT::FieldData::slotHoldsUndefinedAndNull says whether the slot takes it, and AOT::Object::store() sets the mark
# of the cell, which may answer for the raw value of the field. The type of such a field lists neither: Typed::PlacedSlot::typeInLayoutRecord().
.undefined:
    btiz t5, constexpr AOT::Typed::Primitive::Undefined, .slow
    jmp .storeUndefinedOrNull
.null:
    btiz t5, constexpr AOT::Typed::Primitive::Null, .slow
.storeUndefinedOrNull:
    bieq t2, constexpr AOT::SlotRepresentation::JSValue, .storeBoxedPrimitive
    bieq t2, constexpr AOT::SlotRepresentation::OptionalInt32, .storeMarkerInOptionalInt32
    bieq t2, constexpr AOT::SlotRepresentation::OptionalBoolean, .storeByteOfImmediate
    bieq t2, constexpr AOT::SlotRepresentation::OptionalDouble, .storeMarkerInOptionalDouble
    bineq t2, constexpr AOT::SlotRepresentation::OptionalCell32, .slow
    btiz [t1], .slow
    storei t3, [t1]
    dispatch()
.storeMarkerInOptionalDouble:
    move constexpr AOT::absentDouble, t7
    bqeq [t1], t7, .slow
    move constexpr AOT::undefinedDouble, t7
    bqeq t3, ValueUndefined, .storeDoubleMarker
    move constexpr AOT::nullDouble, t7
.storeDoubleMarker:
    storeq t7, [t1]
    dispatch()
.storeMarkerInOptionalInt32:
    bieq [t1], constexpr AOT::absentInt32, .slow
    move constexpr AOT::undefinedInt32, t7
    bqeq t3, ValueUndefined, .storeInt32Marker
    move constexpr AOT::nullInt32, t7
.storeInt32Marker:
    storei t7, [t1]
    dispatch()
.storeByteOfImmediate:
    btbz [t1], .slow
    storeb t3, [t1]
    dispatch()
.storeBoxedPrimitive:
    btqz [t1], .slow
    storeq t3, [t1]
    dispatch()

    # addExtraProperty() in the stubs, for a property that lies in the cell of the extra properties. t2: the StructureID of the object.
.addsExtraProperty:
    btbnz SlotFlags[t6], constexpr AOT::Slot::isOutOfLine, .slow
    loadi (constexpr AOT::offsetOfOrderWord)[t0], t1
    btinz t1, constexpr AOT::orderWordIsNotExtensible, .slow
    branchIfBarrierIsNeeded(t0, t3, .slow)
    structureIDToStructureWithScratch(t2, t3)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesSlot()))[t2], t7
    move t7, t3
    urshifti constexpr AOT::SlotData::offsetBits, t3
    bineq t3, constexpr AOT::SlotRepresentation::Cell32, .slow
    andi constexpr AOT::SlotData::maxOffset, t7
    loadi SlotPayload[t6], t2
    structureIDToStructureWithScratch(t2, t3)

    # t0: object, t1: its order word, t2: the Structure it gets, t5: instance, t6: slot, t7: where the object names its extra properties
    btiz t1, constexpr AOT::orderWordHasExtraProperties, .createsExtraProperties
    loadi [t0, t7], t1
    btiz t1, .slow
    lshiftp constexpr AOT::cell32Shift, t1
    addp InstanceCellCageBase[t5], t1
    branchIfBarrierIsNeeded(t1, t3, .slow)
    get(m_value, t3)
    loadConstantOrVariable(size, t3, t5)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesStructureID()))[t2], t3
    storei t3, JSCell::m_structureID[t1]
    loadi SlotPayload[t6], t3
    storei t3, JSCell::m_structureID[t0]
    loadis SlotSecondPayloadWord[t6], t3
    storeq t5, [t1, t3, 8]
    dispatch()

.createsExtraProperties:
    loadh (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesCellSize()))[t2], t3
    urshifti 1, t3
    loadp InstanceCellSpace[t5], t1
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t1, t3, 1], t3
    btpz t3, .slow
    allocateCell(t3, t1, t5, .slow)
    loadi (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesStructureID()))[t2], t3
    storei t3, JSCell::m_structureID[t1]
    storei (constexpr (JSFinalObject::defaultTypeInfoBlob())), JSCell::m_indexingTypeAndMisc[t1]
    storep 0, JSObjectWithButterfly::m_butterfly[t1]
    loadh (constexpr (AOT::CompiledStructure::offsetOfExtraPropertiesCellSize()))[t2], t3
.clearExtraProperty:
    subi 8, t3
    storeq ValueEmpty, [t1, t3]
    bia t3, sizeof JSObjectWithButterfly + 8, .clearExtraProperty
    get(m_value, t3)
    loadConstantOrVariable(size, t3, t5)
    storeq t5, sizeof JSObjectWithButterfly[t1]
    fenceAfterAllocation(t3)
    urshiftq constexpr AOT::cell32Shift, t1
    storei t1, [t0, t7]
    loadi (constexpr AOT::offsetOfOrderWord)[t0], t3
    ori constexpr AOT::orderWordHasExtraProperties, t3
    storei t3, (constexpr AOT::offsetOfOrderWord)[t0]
    loadi SlotPayload[t6], t3
    storei t3, JSCell::m_structureID[t0]
    dispatch()

.slow:
    callSlowPath(_llint_slow_path_put_by_id)
    dispatch()
end)

# FreeList::allocateWithCellSize(). The low half of the bits of a free cell is the distance to the next interval, the high half the length
# of this one.
# An element of an array that has one there already, and an own value of a plain object. No array profile has a reader.
aotOp(op_put_by_val, OpPutByVal, macro (size, get, dispatch, metadata, return)
    get(m_base, t1)
    loadOperandInRegister(size, t1, t0, .ordinary)
    btqnz t0, notCellMask, .ordinary
    get(m_property, t1)
    loadOperandInRegister(size, t1, t3, .propertyIsConstant)
.hasProperty:
    bqb t3, numberTag, .isNoIndex
    loadb JSCell::m_indexingTypeAndMisc[t0], t2
    btinz t2, CopyOnWrite, .ordinary
    andi IndexingShapeMask, t2
    get(m_value, t5)
    bineq t2, ContiguousShape, .isNotContiguous
    loadp JSObjectWithButterfly::m_butterfly[t0], t2
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t2], .isAppended
.hasRoom:
    zxi2q t3, t3
    loadConstantOrVariable(size, t5, t1)
    storeq t1, [t2, t3, 8]
    btqnz t1, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
.isDone:
    dispatch()

.isAppended:
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.vectorLength[t2], .ordinary
    addi 1, t3, t1
    storei t1, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t2]
    jmp .hasRoom

.isNotContiguous:
    bineq t2, Int32Shape, .isNotInt32
    loadp JSObjectWithButterfly::m_butterfly[t0], t2
    loadConstantOrVariable(size, t5, t1)
    bqb t1, numberTag, .ordinary
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t2], .isInt32Appended
.hasRoomForInt32:
    zxi2q t3, t3
    storeq t1, [t2, t3, 8]
    dispatch()

.isInt32Appended:
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.vectorLength[t2], .ordinary
    addi 1, t3, t5
    storei t5, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t2]
    jmp .hasRoomForInt32

.isNotInt32:
    bineq t2, DoubleShape, .ordinary
    loadp JSObjectWithButterfly::m_butterfly[t0], t2
    biaeq t3, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t2], .ordinary
    loadConstantOrVariable(size, t5, t1)
    btqz t1, numberTag, .ordinary
    numberToDouble(t1, ft0)
    bdnequn ft0, ft0, .ordinary
    zxi2q t3, t3
    stored ft0, [t2, t3, 8]
    dispatch()

.isNoIndex:
    btqnz t3, notCellMask, .ordinary
    bbneq JSCell::m_type[t0], FinalObjectType, .ordinary
    bbneq JSCell::m_type[t3], StringType, .ordinary
    loadNameOfKey(t3, t1, .ordinary)
    findInPropertyTable(.ordinary, .ordinary)
    move constexpr (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue), t3
    btinz t2, t3, .ordinary
    loadi JSCell::m_structureID[t0], t3
    structureIDToStructureWithScratch(t3, t5)
    loadi (constexpr (Structure::bitFieldOffset()))[t3], t3
    move constexpr (Structure::s_isWatchingReplacementBits | Structure::s_mayBePrototypeBits | Structure::s_anyImmutablePropertiesBits), t5
    btinz t3, t5, .ordinary
    urshifti 8, t2
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t1)
    move t0, t3
    storePropertyAtVariableOffset(t2, t3, t1)
    btqnz t1, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
    dispatch()

.propertyIsConstant:
    loadConstantOperand(size, t1, t3, .hasProperty)

.ordinary:
    jumpToOrdinaryHandler(size, op_put_by_val)
end)

macro allocateCell(allocator, result, scratch, slow)
    loadp LocalAllocatorIntervalStart[allocator], result
    bpb result, LocalAllocatorIntervalEnd[allocator], .hasInterval
    loadp LocalAllocatorNextInterval[allocator], result
    btpnz result, 1, slow
    loadi LocalAllocatorSecret[allocator], scratch
    xori FreeCellScrambledBits[result], scratch
    sxi2q scratch, scratch
    addp result, scratch
    storep scratch, LocalAllocatorNextInterval[allocator]
    loadi LocalAllocatorSecret + 4[allocator], scratch
    xori FreeCellScrambledBits + 4[result], scratch
    addp result, scratch
    storep scratch, LocalAllocatorIntervalEnd[allocator]
.hasInterval:
    loadi LocalAllocatorCellSize[allocator], scratch
    addp result, scratch
    storep scratch, LocalAllocatorIntervalStart[allocator]
end

# The collector may mark while this thread runs, so it has to see the cell's contents no later than a reference to the cell.
macro fenceAfterAllocation(scratch)
    if ARM64 or ARM64E
        loadp CodeBlock[cfr], scratch
        loadp CodeBlock::m_vm[scratch], scratch
        btbz (constexpr (VM::offsetOfHeapMutatorShouldBeFenced()))[scratch], .isNotFenced
        memfence
    .isNotFenced:
    end
end

# JSLexicalEnvironment::create(), once SymbolTable::notifyCreation() has nothing left to say.
aotOp(op_create_lexical_environment, OpCreateLexicalEnvironment, macro (size, get, dispatch, metadata, return)
    loadTableInstance(t5, .slow)
    get(m_symbolTable, t0)
    loadConstantOrVariable(size, t0, t1)
    bpneq (constexpr AOT::Interpreter::SymbolTableOffsets::offsetOfSingleton)[t1], constexpr AOT::Interpreter::SymbolTableOffsets::invalidatedSingleton, .slow
    loadi (constexpr AOT::Interpreter::SymbolTableOffsets::offsetOfMaxScopeOffset)[t1], t2
    addi 1, t2
    # MarkedSpace::sizeClassToIndex(): an allocator of eight bytes for each step of sixteen.
    move t2, t3
    lshifti 3, t3
    addi JSLexicalEnvironment_variables + 15, t3
    andi ~15, t3
    bia t3, constexpr MarkedSpace::largeCutoff, .slow
    urshifti 1, t3
    loadp InstanceCellSpace[t5], t0
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t0, t3, 1], t0
    btpz t0, .slow
    allocateCell(t0, t6, t7, .slow)
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    loadi JSGlobalObject::m_lexicalEnvironmentStructure[t0], t0
    storei t0, JSCell::m_structureID[t6]
    structureIDToStructureWithScratch(t0, t7)
    loadi StructureTypeInfoBlob[t0], t0
    storei t0, JSCell::m_indexingTypeAndMisc[t6]
    storep 0, JSObjectWithButterfly::m_butterfly[t6]
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    storep t0, JSScope::m_next[t6]
    storep t1, (constexpr (JSSymbolTableObject::offsetOfSymbolTable()))[t6]
    btiz t2, .isFilled
    get(m_initialValue, t0)
    loadConstantOrVariable(size, t0, t1)
.fillVariable:
    subi 1, t2
    storeq t1, JSLexicalEnvironment_variables[t6, t2, 8]
    btinz t2, .fillVariable
.isFilled:
    fenceAfterAllocation(t0)
    return(t6)

.slow:
    jumpToOrdinaryHandler(size, op_create_lexical_environment)
end)

aotOp(op_mov, OpMov, macro (size, get, dispatch, metadata, return)
    get(m_src, t1)
    loadOperandInRegister(size, t1, t0, .sourceIsConstant)
    return(t0)

.sourceIsConstant:
    loadConstant(size, t1, t0)
    return(t0)
end)

# No JIT waits for the count of returns.
aotOp(op_ret, OpRet, macro (size, get, dispatch, metadata, return)
    get(m_value, t2)
    loadOperandInRegister(size, t2, r0, .valueIsConstant)
.hasValue:
    doReturn()

.valueIsConstant:
    loadConstantOperand(size, t2, r0, .hasValue)
end)

# AOT::generateNewPromise().
aotOp(op_new_promise, OpNewPromise, macro (size, get, dispatch, metadata, return)
    loadRealmInstance(t5, .slow)
    loadp InstancePromiseAllocator[t5], t0
    btpz t0, .slow
    allocateCell(t0, t1, t2, .slow)
    loadq InstancePromiseHeader[t5], t0
    storeq t0, [t1]
    storep 0, JSObjectWithButterfly::m_butterfly[t1]
    storeq 0, (constexpr (JSPromise::offsetOfPacked()))[t1]
    storeq ValueEmpty, (constexpr (JSPromise::offsetOfSlot()))[t1]
    fenceAfterAllocation(t0)
    return(t1)

.slow:
    jumpToOrdinaryHandler(size, op_new_promise)
end)

# JSValue::toThisSlowCase(): strict code takes a primitive as it is, sloppy code the global `this` for undefined and null.
aotOp(op_to_this, OpToThis, macro (size, get, dispatch, metadata, return)
    get(m_srcDst, t1)
    loadq [cfr, t1, 8], t0
    btqz t0, notCellMask, .ordinary
    getu(size, OpToThis, m_ecmaMode, t2)
    bineq t2, constexpr ECMAMode::StrictMode, .sloppy
    dispatch()

    # Sloppy code called without a receiver.
.sloppy:
    andq ~TagUndefined, t0
    bqneq t0, ValueNull, .ordinary
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    loadp JSGlobalObject::m_globalThis[t0], t0
    storeq t0, [cfr, t1, 8]
    dispatch()

.ordinary:
    jumpToOrdinaryHandler(size, op_to_this)
end)

# AOT::Interpreter::FusedClosureVariableRead. An empty variable is left to the read and to its check.
aotOp(op_resolve_scope, OpResolveScope, macro (size, get, dispatch, metadata, return)
    macro loadScope(scope, scratch)
        loadSiteInt(OpResolveScope, m_localScopeDepth, t5, scratch)
        get(m_scope, scope)
        loadq [cfr, scope, 8], scope
        btiz scratch, .hasScope
    .nextScope:
        loadp JSScope::m_next[scope], scope
        bsubinz 1, scratch, .nextScope
    .hasScope:
    end

    # Leaves the variable in t1.
    macro loadVariable()
        if X86_64
            loadh OpResolveScope::Metadata::m_resolveType + FusedVariable[metadataTable, t5, 1], t1
        else
            loadh OpResolveScope::Metadata::m_resolveType + FusedVariable[t5], t1
        end
        loadq JSLexicalEnvironment_variables[t0, t1, 8], t1
    end

    macro loadScopeOfRegister(scope, scratch)
        get(m_scope, scope)
        loadq [cfr, scope, 8], scope
    end

    macro storeIntoReadDestination(value, scratch)
        loadbsq (constexpr (op_resolve_scope_length + OpGetFromScope_m_dst_index)) + 2 * OpcodeIDNarrowSize[PB, PC, 1], scratch
        storeq value, [cfr, scratch, 8]
    end

    macro storeIntoScopeRegister(value)
        get(m_dst, t2)
        storeq value, [cfr, t2, 8]
    end

    locateSite(size, OpResolveScope, t5, t0)
    if X86_64
        loadh OpResolveScope::Metadata::m_resolveType[metadataTable, t5, 1], t3
    else
        loadh OpResolveScope::Metadata::m_resolveType[t5], t3
    end

    # Both instructions are narrow.
    macro readVariable(isEmpty)
        bineq t3, FusedRead, .isNoRead
        loadScopeOfRegister(t0, t2)
        loadVariable()
        storeIntoScopeRegister(t0)
        storeIntoReadDestination(t1, t2)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNoRead:
        bineq t3, FusedReadIntoScopeRegister, .isNotReadIntoScopeRegister
        loadScopeOfRegister(t0, t2)
        loadVariable()
        storeIntoScopeRegister(t1)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadIntoScopeRegister:
        bineq t3, FusedReadAndCheck, .isNotReadAndCheck
        loadScopeOfRegister(t0, t2)
        loadVariable()
        btqz t1, isEmpty
        storeIntoScopeRegister(t0)
        storeIntoReadDestination(t1, t2)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length + op_check_tdz_length)) + 3 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadAndCheck:
        bineq t3, FusedReadIntoScopeRegisterAndCheck, .isNotReadIntoScopeRegisterAndCheck
        loadScopeOfRegister(t0, t2)
        loadVariable()
        btqz t1, isEmpty
        storeIntoScopeRegister(t1)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length + op_check_tdz_length)) + 3 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadIntoScopeRegisterAndCheck:
        bib t3, FirstFusedRead, .isNotFused
        biaeq t3, FirstFusedGlobalRead, .readsGlobal
        bineq t3, FusedReadOuter, .isNotReadOuter
        loadScope(t0, t2)
        loadVariable()
        storeIntoScopeRegister(t0)
        storeIntoReadDestination(t1, t2)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadOuter:
        bineq t3, FusedReadOuterIntoScopeRegister, .isNotReadOuterIntoScopeRegister
        loadScope(t0, t2)
        loadVariable()
        storeIntoScopeRegister(t1)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadOuterIntoScopeRegister:
        bineq t3, FusedReadOuterAndCheck, .isNotReadOuterAndCheck
        loadScope(t0, t2)
        loadVariable()
        btqz t1, isEmpty
        storeIntoScopeRegister(t0)
        storeIntoReadDestination(t1, t2)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length + op_check_tdz_length)) + 3 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotReadOuterAndCheck:
        bineq t3, FusedReadOuterIntoScopeRegisterAndCheck, .isNotFused
        loadScope(t0, t2)
        loadVariable()
        btqz t1, isEmpty
        storeIntoScopeRegister(t1)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length + op_check_tdz_length)) + 3 * OpcodeIDNarrowSize, PC
        nextInstruction()

        # The read throws when it is empty.
    .readsGlobal:
        if X86_64
            loadq OpResolveScope::Metadata::m_resolveType[metadataTable, t5, 1], t1
        else
            loadq OpResolveScope::Metadata::m_resolveType[t5], t1
        end
        urshiftq FusedGlobalShift, t1
        loadq [t1], t1
        loadSitePointer(OpResolveScope, m_constantScope, t5, t0)
        btqz t1, isEmpty
        biaeq t3, FusedGlobalReadIntoScopeRegister, .readsGlobalIntoScopeRegister
        storeIntoScopeRegister(t0)
        storeIntoReadDestination(t1, t2)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .readsGlobalIntoScopeRegister:
        storeIntoScopeRegister(t1)
        addp (constexpr (op_resolve_scope_length + op_get_from_scope_length)) + 2 * OpcodeIDNarrowSize, PC
        nextInstruction()

    .isNotFused:
    end

    size(readVariable, macro (isEmpty) end, macro (isEmpty) end, macro (readVariable) readVariable(.isEmpty) end)

    bineq t3, ClosureVarResolve, .isNotClosureVariable
    loadScope(t0, t2)
.isEmpty:
    storeIntoScopeRegister(t0)
    dispatch()

.isNotClosureVariable:
    bineq t3, GlobalProperty, .ordinary
    loadSitePointer(OpResolveScope, m_constantScope, t5, t0)
    loadSiteInt(OpResolveScope, m_globalLexicalBindingEpoch, t5, t1)
    bineq JSGlobalObject::m_globalLexicalBindingEpoch[t0], t1, .ordinary
    storeIntoScopeRegister(t0)
    dispatch()

.ordinary:
    jumpToOrdinaryHandler(size, op_resolve_scope)
end)

# Reads of variables that the handler above does not do.
aotOp(op_get_from_scope, OpGetFromScope, macro (size, get, dispatch, metadata, return)
    locateSite(size, OpGetFromScope, t5, t0)
    if X86_64
        loadb OpGetFromScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[metadataTable, t5, 1], t3
    else
        loadb OpGetFromScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[t5], t3
    end
    bineq t3, GlobalProperty, .isNotGlobalProperty
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    loadSiteInt(OpGetFromScope, m_structureID, t5, t1)
    bineq JSCell::m_structureID[t0], t1, .isOtherProperty
    loadSitePointer(OpGetFromScope, m_operand, t5, t1)
    loadPropertyAtVariableOffset(t1, t0, t2)
    return(t2)

.isNotGlobalProperty:
    loadSitePointer(OpGetFromScope, m_operand, t5, t1)
    bineq t3, ClosureVar, .isNotClosureVariable
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    loadq JSLexicalEnvironment_variables[t0, t1, 8], t2
    return(t2)

.isNotClosureVariable:
    bia t3, GlobalLexicalVar, .isNoVariable
    loadq [t1], t2
    btqz t2, .ordinary
    return(t2)

    # AOT::Interpreter::tryGetFromContext(), by what the table holds.
.isNoVariable:
    bineq t3, constexpr UnresolvedProperty, .ordinary
.isOtherProperty:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    bbneq JSCell::m_type[t0], constexpr GlobalObjectType, .ordinary
    loadp JSGlobalObject::m_contextifiedObject[t0], t3
    btpz t3, .ordinary
    loadTableInstance(t5, .ordinary)
    loadIdentifier(size, OpGetFromScope, m_var, t1, t0)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, .ordinary)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Absent, .isBehindSandbox
    bbeq JSCell::m_type[t3], CompiledObjectType, .isInCompiledSandbox
    bbneq JSCell::m_type[t3], FinalObjectType, .ordinary
    bbneq t0, constexpr AOT::Slot::Kind::Value, .ordinary
    getFromHashedSlot(t6, .ordinary, .ordinary, .isOfSandbox)

.isInCompiledSandbox:
    getFromCompiledObject(.ordinary, .isOfSandbox)

    # The sandbox itself reads as the global object. A field that is absent reads as undefined too.
.isOfSandbox:
    bqeq t0, ValueUndefined, .ordinary
    btqz t0, .ordinary
    get(m_scope, t1)
    loadq [cfr, t1, 8], t1
    bpeq JSGlobalObject::m_contextifiedObject[t1], t0, .ordinary
    return(t0)

.isBehindSandbox:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    findInPropertyTable(.ordinary, .ordinary)
    btinz t2, constexpr PropertyAttribute::AccessorOrCustomAccessorOrValue, .ordinary
    urshifti 8, t2
    loadPropertyAtVariableOffset(t2, t0, t1)
    return(t1)

.ordinary:
    jumpToOrdinaryHandler(size, op_get_from_scope)
end)

aotOp(op_put_to_scope, OpPutToScope, macro (size, get, dispatch, metadata, return)
    locateSite(size, OpPutToScope, t5, t0)
    if X86_64
        loadi OpPutToScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[metadataTable, t5, 1], t0
    else
        loadi OpPutToScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[t5], t0
    end
    andi ResolveTypeMask, t0
    bineq t0, ClosureVar, .isNotClosureVariable
    get(m_value, t1)
    loadOperandInRegister(size, t1, t2, .valueIsConstant)
.hasValue:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    loadSitePointer(OpPutToScope, m_operand, t5, t1)
    storeq t2, JSLexicalEnvironment_variables[t0, t1, 8]
    btqnz t2, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
.isDone:
    dispatch()

.valueIsConstant:
    loadConstantOperand(size, t1, t2, .hasValue)

.isNotClosureVariable:
    bieq t0, GlobalLexicalVar, .storesGlobal
    bia t0, GlobalVar, .maybeContextVariable
    bieq t0, GlobalProperty, .storesGlobalProperty
    varReadOnlyCheck(.ordinary, t2)
.storesGlobal:
    # The ordinary handler initializes, and checks what is stored to before it is initialized.
    loadSitePointer(OpPutToScope, m_watchpointSet, t5, t2)
    btpz t2, .isNotWatched
    notifyWrite(t2, t0, .ordinary)
.isNotWatched:
    loadSitePointer(OpPutToScope, m_operand, t5, t3)
    loadq [t3], t0
    btqz t0, .ordinary
    get(m_value, t1)
    loadConstantOrVariable(size, t1, t2)
    storeq t2, [t3]
    btqnz t2, notCellMask, .isDone
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    writeBarrierOnCell(t0, t1)
    dispatch()

.storesGlobalProperty:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    loadSiteInt(OpPutToScope, m_structureID, t5, t1)
    bineq JSCell::m_structureID[t0], t1, .isOtherProperty
    get(m_value, t1)
    loadConstantOrVariable(size, t1, t2)
    loadSitePointer(OpPutToScope, m_operand, t5, t1)
    move t0, t3
    storePropertyAtVariableOffset(t1, t3, t2)
    btqnz t2, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
    dispatch()

    # A variable of a context, whose address the site has, and of which the sandbox has a property: AOT::Interpreter::tryPutToContext().
.maybeContextVariable:
    bineq t0, constexpr Dynamic, .isNotContextVariable
    loadSitePointer(OpPutToScope, m_operand, t5, t1)
    btpz t1, .ordinary
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    bbneq JSCell::m_type[t0], constexpr GlobalObjectType, .ordinary
    loadp JSGlobalObject::m_contextifiedObject[t0], t0
    btpz t0, .ordinary
    varReadOnlyCheck(.ordinary, t2)
.hasSandbox:
    bbeq JSCell::m_type[t0], FinalObjectType, .hasHolder
    bbneq JSCell::m_type[t0], CompiledObjectType, .ordinary
    loadTableInstance(t5, .ordinary)
    loadi JSCell::m_structureID[t0], t2
    move t2, t3
    structureIDToStructureWithScratch(t3, t1)
    move constexpr Structure::s_mayBePrototypeBits, t1
    loadi (constexpr (Structure::bitFieldOffset()))[t3], t3
    btinz t3, t1, .ordinary
    loadExtraProperties(t5, t0, t2, t1, t3, .ordinary)
    move t1, t0
    bbneq JSCell::m_type[t0], FinalObjectType, .ordinary
.hasHolder:
    loadIdentifier(size, OpPutToScope, m_var, t1, t2)
    findInPropertyTable(.ordinary, .ordinary)
    move constexpr (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue), t3
    btinz t2, t3, .ordinary
    loadi JSCell::m_structureID[t0], t3
    structureIDToStructureWithScratch(t3, t5)
    loadi (constexpr (Structure::bitFieldOffset()))[t3], t3
    move constexpr (Structure::s_isWatchingReplacementBits | Structure::s_mayBePrototypeBits | Structure::s_anyImmutablePropertiesBits), t5
    btinz t3, t5, .ordinary
    urshifti 8, t2
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t1)
    move t0, t3
    storePropertyAtVariableOffset(t2, t3, t1)
    locateSite(size, OpPutToScope, t5, t2)
    if X86_64
        loadb OpPutToScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[metadataTable, t5, 1], t2
    else
        loadb OpPutToScope::Metadata::m_getPutInfo + GetPutInfo::m_operand[t5], t2
    end
    bineq t2, constexpr Dynamic, .storesGlobalObjectProperty
    loadSitePointer(OpPutToScope, m_operand, t5, t2)
    storeq t1, [t2]
    btqnz t1, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    writeBarrierOnCell(t0, t1)
    dispatch()

    # A property of the global object is looked for twice: what forbids the store is known before the sandbox is written to.
.isNotContextVariable:
    bineq t0, constexpr UnresolvedProperty, .ordinary
.isOtherProperty:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    bbneq JSCell::m_type[t0], constexpr GlobalObjectType, .ordinary
    btpz JSGlobalObject::m_contextifiedObject[t0], .ordinary
    loadIdentifier(size, OpPutToScope, m_var, t1, t2)
    findInPropertyTable(.ordinary, .ordinary)
    move constexpr (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue), t3
    btinz t2, t3, .ordinary
    loadi JSCell::m_structureID[t0], t3
    structureIDToStructureWithScratch(t3, t5)
    loadi (constexpr (Structure::bitFieldOffset()))[t3], t3
    move constexpr (Structure::s_isWatchingReplacementBits | Structure::s_mayBePrototypeBits | Structure::s_anyImmutablePropertiesBits), t5
    btinz t3, t5, .ordinary
    loadp JSGlobalObject::m_contextifiedObject[t0], t0
    jmp .hasSandbox

    # t0: what holds the property of the sandbox.
.storesGlobalObjectProperty:
    btqnz t1, notCellMask, .holderIsDone
    writeBarrierOnCell(t0, t1)
.holderIsDone:
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    loadIdentifier(size, OpPutToScope, m_var, t1, t2)
    findInPropertyTable(.isDone, .isDone)
    urshifti 8, t2
    get(m_value, t5)
    loadConstantOrVariable(size, t5, t1)
    move t0, t3
    storePropertyAtVariableOffset(t2, t3, t1)
    btqnz t1, notCellMask, .isDone
    writeBarrierOnCell(t0, t1)
    dispatch()

.ordinary:
    jumpToOrdinaryHandler(size, op_put_to_scope)
end)

# There is no JIT to count the iterations of a loop. This also executes op_check_traps, which follows.
aotOp(op_loop_hint, OpLoopHint, macro (size, get, dispatch, metadata, return)
    loadp CodeBlock[cfr], t1
    loadp CodeBlock::m_vm[t1], t1
    btinz VM::m_threadContext + VMThreadContext::m_traps + VMTraps::m_trapBits[t1], VMTrapsAsyncEvents, .checksTraps
    loadb (constexpr op_loop_hint_length) + OpcodeIDNarrowSize[PB, PC, 1], t0
    bineq t0, constexpr op_check_traps, .checksTraps
    addp (constexpr op_loop_hint_length) + (constexpr op_check_traps_length) + 2 * OpcodeIDNarrowSize, PC
    nextInstruction()

.checksTraps:
    dispatch()
end)

aotOp(op_switch_string, OpSwitchString, macro (size, get, dispatch, metadata, return)
    get(m_scrutinee, t1)
    loadConstantOrVariable(size, t1, t0)
    btqnz t0, notCellMask, .slow
    bbneq JSCell::m_type[t0], StringType, .slow
    loadp JSString::m_fiber[t0], t0
    btpnz t0, isRopeInPointer, .slow
    loadTableInstance(t5, .slow)
    loadp InstanceSwitchTargets[t5], t5
    btpz t5, .slow
    move PB, t2
    addp PC, t2
    move t0, t1
    urshiftp SwitchTargetStringShift, t1
    move t2, t3
    urshiftp SwitchTargetInstructionShift, t3
    addp t2, t1
    addp t3, t1
    andp NumberOfSwitchTargets - 1, t1
    muli SwitchTargetSize, t1
    addp t1, t5
    bpneq t0, SwitchTargetString[t5], .slow
    bpneq t2, SwitchTargetInstruction[t5], .slow
    addp SwitchTargetOffset[t5], PC
    nextInstruction()

.slow:
    callSlowPath(_llint_slow_path_aot_switch_string)
    nextInstruction()
end)

# The profiles of arithmetic operations are not read.
aotOp(op_sub, OpSub, macro (size, get, dispatch, metadata, return)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadOperandInRegister(size, t2, t0, .leftIsConstant)
.hasLeftOperand:
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .isNoInt32
    bqb t1, numberTag, .isNoInt32
    bsubio t1, t0, .overflows
    orq numberTag, t0
    return(t0)

.overflows:
    addi t1, t0
    ci2ds t0, ft0
    ci2ds t1, ft1
    subd ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.isNoInt32:
    btqz t0, numberTag, .ordinary
    btqz t1, numberTag, .ordinary
    numberToDouble(t0, ft0)
    numberToDouble(t1, ft1)
    subd ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.leftIsConstant:
    loadConstantOperand(size, t2, t0, .hasLeftOperand)

.ordinary:
    jumpToOrdinaryHandler(size, op_sub)
end)

aotOp(op_inc, OpInc, macro (size, get, dispatch, metadata, return)
    get(m_srcDst, t0)
    loadq [cfr, t0, 8], t1
    bqb t1, numberTag, .ordinary
    baddio 1, t1, .ordinary
    orq numberTag, t1
    storeq t1, [cfr, t0, 8]
    dispatch()

.ordinary:
    jumpToOrdinaryHandler(size, op_inc)
end)

# JSC::toInt32() of a double whose integer part takes less than 63 bits, which the low half of value holds then. What is larger, infinite or not a number is left to the ordinary handler.
macro truncateDouble(value, scratch, cannot)
    btqz value, numberTag, cannot
    addq numberTag, value
    fq2d value, ft0
    truncated2qs ft0, value
    move 0x4000000000000000, scratch
    addq value, scratch
    bqlt scratch, 0, cannot
end

macro aotBitOp(opcodeName, opcodeStruct, operation)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        get(m_rhs, t0)
        get(m_lhs, t2)
        loadConstantOrVariable(size, t0, t1)
        loadOperandInRegister(size, t2, t0, .leftIsConstant)
    .hasLeftOperand:
        bqb t0, numberTag, .leftIsNoInt32
    .hasLeft:
        bqb t1, numberTag, .rightIsNoInt32
    .hasRight:
        operation(t0, t1)
        orq numberTag, t0
        return(t0)

    .leftIsNoInt32:
        truncateDouble(t0, t3, .ordinary)
        jmp .hasLeft

    .rightIsNoInt32:
        truncateDouble(t1, t3, .ordinary)
        jmp .hasRight

    .leftIsConstant:
        loadConstantOperand(size, t2, t0, .hasLeftOperand)

    .ordinary:
        jumpToOrdinaryHandler(size, op_%opcodeName%)
    end)
end

aotBitOp(lshift, OpLshift,
    macro (lhs, rhs) lshifti rhs, lhs end)

aotBitOp(rshift, OpRshift,
    macro (lhs, rhs) rshifti rhs, lhs end)

aotBitOp(urshift, OpUrshift,
    macro (lhs, rhs) urshifti rhs, lhs end)

aotBitOp(bitand, OpBitand,
    macro (lhs, rhs) andi rhs, lhs end)

aotBitOp(bitor, OpBitor,
    macro (lhs, rhs) ori rhs, lhs end)

aotBitOp(bitxor, OpBitxor,
    macro (lhs, rhs) xori rhs, lhs end)

# The ordinary handler leaves a product that is no int32 to the slow path.
aotOp(op_mul, OpMul, macro (size, get, dispatch, metadata, return)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadOperandInRegister(size, t2, t0, .leftIsConstant)
.hasLeftOperand:
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .isNoInt32
    bqb t1, numberTag, .isNoInt32
    move t0, t3
    bmulio t1, t3, .isNoInt32
    btiz t3, .isZero
.hasProduct:
    orq numberTag, t3
    return(t3)

.isZero:
    bilt t0, 0, .isNoInt32
    bigteq t1, 0, .hasProduct

.isNoInt32:
    btqz t0, numberTag, .ordinary
    btqz t1, numberTag, .ordinary
    numberToDouble(t0, ft0)
    numberToDouble(t1, ft1)
    muld ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.leftIsConstant:
    loadConstantOperand(size, t2, t0, .hasLeftOperand)

.ordinary:
    jumpToOrdinaryHandler(size, op_mul)
end)

# Integers alone: fmod() of two integers is exact, and a zero has the sign of the dividend.
if X86_64 or ARM64
aotOp(op_mod, OpMod, macro (size, get, dispatch, metadata, return)
    macro remainder32()
        if X86_64
            # Leaves it in edx, which is t2.
            cdqi
            idivi t1
        else
            divis t1, t0, t2
            muli t1, t2
            subi t0, t2, t2
        end
    end

    macro remainder64()
        if X86_64
            cqoq
            idivq t1
        else
            divqs t1, t0, t2
            mulq t1, t2
            subq t0, t2, t2
        end
    end

    get(m_lhs, t2)
    get(m_rhs, t3)
    loadOperandInRegister(size, t2, t0, .leftIsConstant)
.hasLeftOperand:
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .isNoInt32
    bqb t1, numberTag, .isNoInt32
    bilteq t1, 0, .ordinary
    bilt t0, 0, .isNegative
    remainder32()
    orq numberTag, t2
    return(t2)

.isNegative:
    remainder32()
    btiz t2, .isNegativeZero
    orq numberTag, t2
    return(t2)

.isNoInt32:
    btqz t0, numberTag, .ordinary
    btqz t1, numberTag, .ordinary
    move t0, t3
    move t1, t5
    numberToDouble(t3, ft0)
    numberToDouble(t5, ft1)
    truncated2qs ft0, t0
    cq2ds t0, ft2
    bdnequn ft0, ft2, .ordinary
    truncated2qs ft1, t1
    cq2ds t1, ft2
    bdnequn ft1, ft2, .ordinary
    move 0x4000000000000000, t3
    move t3, t5
    addq t0, t3
    bqlt t3, 0, .ordinary
    addq t1, t5
    bqlt t5, 0, .ordinary
    # Either zero may be negative.
    btqz t0, .ordinary
    bqlteq t1, 0, .ordinary
    move t0, t3
    remainder64()
    btqz t2, .isZero
.hasRemainder:
    sxi2q t2, t3
    bqneq t2, t3, .isDouble
    zxi2q t2, t2
    orq numberTag, t2
    return(t2)

.isZero:
    bqgteq t3, 0, .hasRemainder
.isNegativeZero:
    move 0x8000000000000000, t0
    subq numberTag, t0
    return(t0)

.isDouble:
    cq2ds t2, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.leftIsConstant:
    loadConstantOperand(size, t2, t0, .hasLeftOperand)

.ordinary:
    jumpToOrdinaryHandler(size, op_mod)
end)
end

# The ordinary handler leaves a quotient that is no int32 to the slow path.
aotOp(op_div, OpDiv, macro (size, get, dispatch, metadata, return)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadConstantOrVariable(size, t2, t0)
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .ordinary
    bqb t1, numberTag, .ordinary
    ci2ds t0, ft0
    ci2ds t1, ft1
    divd ft1, ft0
    bcd2i ft0, t0, .isNoInt32
    orq numberTag, t0
    return(t0)

.isNoInt32:
    btiz t1, .ordinary
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.ordinary:
    jumpToOrdinaryHandler(size, op_div)
end)

aotOp(op_unsigned, OpUnsigned, macro (size, get, dispatch, metadata, return)
    get(m_operand, t1)
    loadConstantOrVariable(size, t1, t2)
    bqb t2, numberTag, .ordinary
    bilt t2, 0, .exceedsInt32
    return(t2)

.exceedsInt32:
    zxi2q t2, t0
    cq2ds t0, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.ordinary:
    jumpToOrdinaryHandler(size, op_unsigned)
end)

# JSFunction::create() of an executable that has made functions before: AOT::FunctionAllocationEntry.
macro aotNewFunction(opcodeName, opcodeStruct, executables)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        loadTableInstance(t5, .slow)
        loadp InstanceFunctionAllocations[t5], t5
        btpz t5, .slow
        getu(size, opcodeStruct, m_functionDecl, t1)
        loadp CodeBlock[cfr], t0
        loadp executables[t0], t0
        loadp (constexpr AOT::Interpreter::offsetOfExecutablesInStorage)[t0, t1, PtrSize], t0
        btpz t0, .slow
        move t0, t1
        urshiftp FunctionAllocationExecutableShift, t1
        andp NumberOfFunctionAllocations - 1, t1
        muli FunctionAllocationSize, t1
        addp t1, t5
        bpneq t0, FunctionAllocationExecutable[t5], .slow
        loadp FunctionAllocationAllocator[t5], t1
        allocateCell(t1, t2, t3, .slow)
        loadq FunctionAllocationHeader[t5], t3
        storeq t3, [t2]
        storep 0, JSObjectWithButterfly::m_butterfly[t2]
        get(m_scope, t1)
        loadq [cfr, t1, 8], t1
        storep t1, JSCallee::m_scope[t2]
        storep t0, JSFunction::m_executableOrRareData[t2]
        fenceAfterAllocation(t3)
        return(t2)

    .slow:
        callSlowPath(_llint_slow_path_%opcodeName%)
        dispatch()
    end)
end

aotNewFunction(new_func, OpNewFunc, CodeBlock::m_functionDecls)

aotNewFunction(new_func_exp, OpNewFuncExp, CodeBlock::m_functionExprs)

# Reads the allocation profile of the constructor as Baseline code does. No JIT reads the profile of callees.
aotOp(op_create_this, OpCreateThis, macro (size, get, dispatch, metadata, return)
    get(m_callee, t1)
    loadq [cfr, t1, 8], t0
    bbneq JSCell::m_type[t0], JSFunctionType, .slow
    loadp JSFunction::m_executableOrRareData[t0], t5
    btpz t5, (constexpr JSFunction::rareDataTag), .slow
    loadp (FunctionRareData::m_objectAllocationProfile.m_allocator - (constexpr JSFunction::rareDataTag))[t5], t1
    btpz t1, .slow
    loadp (FunctionRareData::m_objectAllocationProfile.m_structure - (constexpr JSFunction::rareDataTag))[t5], t2
    allocateCell(t1, t0, t3, .slow)
    storei t2, JSCell::m_structureID[t0]
    loadi StructureTypeInfoBlob[t2], t3
    storei t3, JSCell::m_indexingTypeAndMisc[t0]
    storep 0, JSObjectWithButterfly::m_butterfly[t0]
    loadb (constexpr (Structure::inlineCapacityOffset()))[t2], t3
    btiz t3, .isInitialized
.clearProperty:
    subi 1, t3
    storeq ValueEmpty, sizeof JSObjectWithButterfly[t0, t3, 8]
    btinz t3, .clearProperty
.isInitialized:
    btqnz Structure::m_prototype[t2], .hasMonoProto
    loadp (constexpr (FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfPrototype() - JSFunction::rareDataTag))[t5], t3
    storeq t3, (constexpr (sizeof(JSObjectWithButterfly) + knownPolyProtoOffset * sizeof(EncodedJSValue)))[t0]
.hasMonoProto:
    fenceAfterAllocation(t3)
    return(t0)

.slow:
    callSlowPath(_slow_path_create_this)
    dispatch()
end)

# ClonedArguments::createWithMachineFrame(), for as many arguments as a small butterfly holds. The butterfly is allocated first: nobody sees it when the cell cannot be.
aotOp(op_create_cloned_arguments, OpCreateClonedArguments, macro (size, get, dispatch, metadata, return)
    loadTableInstance(t5, .slow)
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    loadi JSGlobalObject::m_clonedArgumentsStructure[t0], t1
    structureIDToStructureWithScratch(t1, t2)
    loadi StructureTypeInfoBlob[t1], t1
    move t1, t2
    andi IndexingShapeMask, t2
    bineq t2, ContiguousShape, .slow
    loadi ArgumentCountIncludingThis + LowWordOffset[cfr], t2
    subi 1, t2
    bia t2, MaxInlineClonedArguments, .slow
    # MarkedSpace::sizeClassToIndex(): an allocator of eight bytes for each step of sixteen.
    move t2, t3
    lshifti 3, t3
    addi (constexpr (initialOutOfLineCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader))) + 15, t3
    andi ~15, t3
    urshifti 1, t3
    loadp InstanceAuxiliarySpace[t5], t0
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t0, t3, 1], t0
    btpz t0, .slow
    loadp InstanceClonedArgumentsAllocator[t5], t5
    btpz t5, .slow
    allocateCell(t0, t6, t7, .slow)
    allocateCell(t5, t0, t7, .slow)
    storei t1, JSCell::m_indexingTypeAndMisc[t0]
    loadp CodeBlock[cfr], t1
    loadp CodeBlock::m_globalObject[t1], t1
    loadi JSGlobalObject::m_clonedArgumentsStructure[t1], t1
    storei t1, JSCell::m_structureID[t0]
    # The first property beyond the cell is the length.
    storeq ValueEmpty, [t6]
    storeq ValueEmpty, 8[t6]
    storeq ValueEmpty, 16[t6]
    move t2, t1
    orq numberTag, t1
    storeq t1, 24[t6]
    addp constexpr (initialOutOfLineCapacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader)), t6
    storei t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t6]
    storei t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.vectorLength[t6]
    storep t6, JSObjectWithButterfly::m_butterfly[t0]
    loadp Callee[cfr], t1
    storep t1, (constexpr (ClonedArguments::offsetOfCallee()))[t0]
    btiz t2, .isFilled
.copyArgument:
    subi 1, t2
    loadq FirstArgumentOffset[cfr, t2, 8], t1
    storeq t1, [t6, t2, 8]
    btinz t2, .copyArgument
.isFilled:
    fenceAfterAllocation(t1)
    return(t0)

.slow:
    callSlowPath(_llint_slow_path_create_cloned_arguments)
    dispatch()
end)

# JSFinalObject::create().
aotOp(op_new_object, OpNewObject, macro (size, get, dispatch, metadata, return)
    metadata(t5, t0)
    loadp OpNewObject::Metadata::m_objectAllocationProfile.m_allocator[t5], t1
    btpz t1, .slow
    btpnz t1, ObjectLiteralPlanTag, .hasPlan
    loadp OpNewObject::Metadata::m_objectAllocationProfile.m_structure[t5], t2
    allocateCell(t1, t0, t3, .slow)
    storei t2, JSCell::m_structureID[t0]
    loadi StructureTypeInfoBlob[t2], t3
    storei t3, JSCell::m_indexingTypeAndMisc[t0]
    storep 0, JSObjectWithButterfly::m_butterfly[t0]
    loadb (constexpr (Structure::inlineCapacityOffset()))[t2], t3
    btiz t3, .isInitialized
.clearProperty:
    subi 1, t3
    storeq ValueEmpty, sizeof JSObjectWithButterfly[t0, t3, 8]
    btinz t3, .clearProperty
.isInitialized:
    fenceAfterAllocation(t3)
    return(t0)

.hasPlan:
    bpeq t1, ObjectLiteralNotPlannedYet, .slow
    loadp ObjectLiteralPlanAllocator[t1], t2
    allocateCell(t2, t0, t3, .slow)
    loadq ObjectLiteralPlanCellHeader[t1], t3
    storeq t3, [t0]
    loadq ObjectLiteralPlanSecondWord[t1], t3
    storeq t3, JSObjectWithButterfly::m_butterfly[t0]
    loadi ObjectLiteralPlanNumberOfClearedWords[t1], t3
    btiz t3, .isCleared
    loadi ObjectLiteralPlanOffsetOfClearedWords[t1], t2
    addp t0, t2
.clearWord:
    subi 1, t3
    storeq ValueEmpty, [t2, t3, 8]
    btinz t3, .clearWord
.isCleared:
    loadi ObjectLiteralPlanNumberOfValues[t1], t2
    leap ObjectLiteralPlanValueOffsets[t1], t3
    get(m_dst, t5)
    size(macro ()
        addp (constexpr op_new_object_length) + OpcodeIDNarrowSize, PC
    end, macro ()
        addp (constexpr op_new_object_length) * 2 + OpcodeIDWide16SizeJS, PC
    end, macro ()
        addp (constexpr op_new_object_length) * 4 + OpcodeIDWide32SizeJS, PC
    end, macro (advance) advance() end)
.storeValue:
    loadb [PB, PC, 1], t6
    bieq t6, constexpr op_wide16, .storeIsWide16
    getOperandNarrow(OpPutById, m_value, t6)
    addp (constexpr op_put_by_id_length) + OpcodeIDNarrowSize, PC
    loadOperandInRegister(narrow, t6, t7, .valueIsConstant)
.hasValue:
    loadh [t3], t6
    storeq t7, [t0, t6, 1]
    addp 2, t3
    subi 1, t2
    btinz t2, .storeValue
    fenceAfterAllocation(t3)
    storeq t0, [cfr, t5, 8]
    nextInstruction()

.slow:
    callSlowPath(_llint_slow_path_aot_new_object)
    nextInstruction()

.valueIsConstant:
    loadConstantOperand(narrow, t6, t7, .hasValue)

.storeIsWide16:
    getOperandWide16JS(OpPutById, m_value, t6)
    loadConstantOrVariable(wide16, t6, t7)
    addp (constexpr op_put_by_id_length) * 2 + OpcodeIDWide16SizeJS, PC
    jmp .hasValue
end)

# constructArrayNegativeIndexed(), where storing the elements leaves the indexing type of the profile as it is.
aotOp(op_new_array, OpNewArray, macro (size, get, dispatch, metadata, return)
    loadRealmInstance(t5, .ofOtherRealm)
    # AOT::branchIfHavingABadTime().
    btiz InstanceNewArrayWithContiguousHeader[t5], .slow
.hasInstance:
    metadata(t6, t0)

    # ArrayAllocationProfile::selectIndexingType().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t3
    move t3, t2
    urshiftq constexpr AOT::arrayAllocationProfileIndexingTypeShift, t2
    lshiftq constexpr AOT::arrayAllocationProfileTypeBits, t3
    urshiftq constexpr AOT::arrayAllocationProfileTypeBits, t3
    btqz t3, .typeIsCurrent
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    andi constexpr AllWritableArrayTypes, t0
    bineq t0, t2, .slow
.typeIsCurrent:

    # Butterfly::optimalContiguousVectorLength(): the storage has a size class of its own up to MarkedSpace::preciseCutoff.
    getu(size, OpNewArray, m_argc, t1)
    btinz t1, .hasElements
    bia t2, constexpr ArrayWithContiguous, .slow
    bieq t2, constexpr ArrayWithContiguous, .emptyInlineButterfly
    bieq t2, constexpr ArrayWithInt32, .emptyInlineButterfly
    bineq t2, constexpr ArrayWithUndecided, .emptySeparateButterfly

    # Like the NewEmptyArray stub: the butterfly is in the cell of the array, and an undecided array gets Int32 storage.
.emptyInlineButterfly:
    loadp InstanceEmptyArrayAllocator[t5], t0
    allocateCell(t0, t1, t3, .hasNoCell)
    bineq t2, constexpr ArrayWithContiguous, .holdsInt32
    loadq InstanceNewArrayWithContiguousHeader[t5], t2
    jmp .hasHeader
.holdsInt32:
    loadq InstanceNewArrayWithInt32Header[t5], t2
.hasHeader:
    storeq t2, [t1]
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN_EMPTY, t0
    lshiftq 32, t0
    storeq t0, (constexpr (sizeof(JSArray)))[t1]
    move ValueEmpty, t0
    # BASE_CONTIGUOUS_VECTOR_LEN_EMPTY words.
    storeq t0, (constexpr (sizeof(JSArray) + sizeof(IndexingHeader)))[t1]
    storeq t0, (constexpr (sizeof(JSArray) + sizeof(IndexingHeader) + 8))[t1]
    storeq t0, (constexpr (sizeof(JSArray) + sizeof(IndexingHeader) + 16))[t1]
    storeq t0, (constexpr (sizeof(JSArray) + sizeof(IndexingHeader) + 24))[t1]
    storeq t0, (constexpr (sizeof(JSArray) + sizeof(IndexingHeader) + 32))[t1]
    leap (constexpr (sizeof(JSArray) + sizeof(IndexingHeader)))[t1], t0
    storep t0, JSObjectWithButterfly::m_butterfly[t1]
    fenceAfterAllocation(t0)
    jmp .updateProfile

.hasNoCell:
    callSlowPath(_llint_slow_path_aot_new_empty_array)
    dispatch()

.emptySeparateButterfly:
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN_EMPTY, t3
    jmp .elementsFit
.hasElements:
    bia t1, constexpr AOT::maxFastPathArrayLiteralLength, .slow
    move t1, t3
    ori 1, t3
    biaeq t3, constexpr BASE_CONTIGUOUS_VECTOR_LEN, .hasVectorLength
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN, t3
.hasVectorLength:
    bieq t2, constexpr ArrayWithContiguous, .elementsFit
    bineq t2, constexpr ArrayWithInt32, .slow
    get(m_argv, t0)
    leap [cfr, t0, 8], t0
    move t1, t7
.checkElement:
    bqb [t0], numberTag, .slow
    subp 8, t0
    subi 1, t7
    btinz t7, .checkElement
.elementsFit:

    # t2: indexing type, t3: vector length. The indexing header is one more word.
    addi 1, t3
    loadp InstanceAuxiliarySpace[t5], t0
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t0, t3, 4], t0
    btpz t0, .slow
    allocateCell(t0, t7, t1, .slow)
    subi 1, t3
    getu(size, OpNewArray, m_argc, t1)
    move t3, t0
    lshiftq 32, t0
    orq t1, t0
    storeq t0, [t7]

    move ValueEmpty, t0
    bineq t2, constexpr ArrayWithDouble, .hasHole
    move constexpr AOT::holeInDoubleArray, t0
.hasHole:
    btinz t1, .clearsBehindElements
    # BASE_CONTIGUOUS_VECTOR_LEN_EMPTY words.
    storeq t0, 8[t7]
    storeq t0, 16[t7]
    storeq t0, 24[t7]
    storeq t0, 32[t7]
    storeq t0, 40[t7]
    jmp .isFilled
.clearsBehindElements:
    bieq t3, t1, .isCleared
.clearElement:
    storeq t0, [t7, t3, 8]
    subi 1, t3
    bineq t3, t1, .clearElement
.isCleared:
    # The elements lie in registers of descending number. This goes from the last to the first.
    get(m_argv, t0)
    subp t1, t0
    leap 8[cfr, t0, 8], t0
.copyElement:
    loadq [t0], t3
    storeq t3, [t7, t1, 8]
    addp 8, t0
    subi 1, t1
    btinz t1, .copyElement
.isFilled:
    addp constexpr (sizeof(IndexingHeader)), t7

    loadp InstanceArrayAllocator[t5], t0
    allocateCell(t0, t1, t3, .slow)

    # JSGlobalObject::arrayStructureForIndexingTypeDuringAllocation().
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    urshifti constexpr IndexingShapeShift, t2
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t2, 4], t2
    storei t2, JSCell::m_structureID[t1]
    structureIDToStructureWithScratch(t2, t0)
    loadi StructureTypeInfoBlob[t2], t0
    storei t0, JSCell::m_indexingTypeAndMisc[t1]
    storep t7, JSObjectWithButterfly::m_butterfly[t1]
    fenceAfterAllocation(t0)

.updateProfile:
    # ArrayAllocationProfile::updateLastAllocation().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t0
    urshiftq constexpr AOT::arrayAllocationProfilePointerBits, t0
    lshiftq constexpr AOT::arrayAllocationProfilePointerBits, t0
    orq t1, t0
    storeq t0, OpNewArray::Metadata::m_arrayAllocationProfile[t6]
    return(t1)

    # A realm without an instance of its own.
.ofOtherRealm:
    getu(size, OpNewArray, m_argc, t0)
    btiz t0, .slow
    loadTableInstance(t5, .slow)
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    move constexpr ArrayWithContiguous, t1
    urshifti constexpr IndexingShapeShift, t1
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t1, 4], t0
    structureIDToStructureWithScratch(t0, t1)
    loadi StructureTypeInfoBlob[t0], t0
    andi IndexingShapeMask, t0
    bieq t0, ContiguousShape, .hasInstance
.slow:
    callSlowPath(_llint_slow_path_new_array)
    dispatch()
end)

# constructArray() of the arguments behind the named parameters. The butterfly is allocated first: nobody sees it when the cell cannot be.
aotOp(op_create_rest, OpCreateRest, macro (size, get, dispatch, metadata, return)
    loadRealmInstance(t5, .ofOtherRealm)
    # AOT::branchIfHavingABadTime().
    btiz InstanceNewArrayWithContiguousHeader[t5], .slow
.hasInstance:
    loadi ArgumentCountIncludingThis + LowWordOffset[cfr], t1
    getu(size, OpCreateRest, m_numParametersToSkip, t2)
    subi 1, t1
    subi t2, t1
    bigteq t1, 0, .hasCount
    move 0, t1
.hasCount:
    bia t1, MaxInlineClonedArguments, .slow
    move t1, t3
    ori 1, t3
    biaeq t3, constexpr BASE_CONTIGUOUS_VECTOR_LEN, .hasVectorLength
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN, t3
.hasVectorLength:
    addi 1, t3
    loadp InstanceAuxiliarySpace[t5], t0
    loadp (constexpr (CompleteSubspace::offsetOfAllocatorForSizeStep()))[t0, t3, 4], t0
    btpz t0, .slow
    allocateCell(t0, t7, t6, .slow)
    subi 1, t3
    move t3, t0
    lshiftq 32, t0
    orq t1, t0
    storeq t0, [t7]
.clearElement:
    bieq t3, t1, .isCleared
    storeq ValueEmpty, [t7, t3, 8]
    subi 1, t3
    jmp .clearElement
.isCleared:
    btiz t1, .isFilled
    leap FirstArgumentOffset[cfr, t2, 8], t2
.copyArgument:
    loadq -8[t2, t1, 8], t0
    storeq t0, [t7, t1, 8]
    subi 1, t1
    btinz t1, .copyArgument
.isFilled:
    addp constexpr (sizeof(IndexingHeader)), t7
    loadp InstanceArrayAllocator[t5], t0
    allocateCell(t0, t1, t3, .slow)
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    move constexpr ArrayWithContiguous, t2
    urshifti constexpr IndexingShapeShift, t2
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t2, 4], t2
    storei t2, JSCell::m_structureID[t1]
    structureIDToStructureWithScratch(t2, t0)
    loadi StructureTypeInfoBlob[t2], t0
    storei t0, JSCell::m_indexingTypeAndMisc[t1]
    storep t7, JSObjectWithButterfly::m_butterfly[t1]
    fenceAfterAllocation(t0)
    return(t1)

    # A realm without an instance of its own.
.ofOtherRealm:
    loadTableInstance(t5, .slow)
    loadp CodeBlock[cfr], t0
    loadp CodeBlock::m_globalObject[t0], t0
    move constexpr ArrayWithContiguous, t1
    urshifti constexpr IndexingShapeShift, t1
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t1, 4], t0
    structureIDToStructureWithScratch(t0, t1)
    loadi StructureTypeInfoBlob[t0], t0
    andi IndexingShapeMask, t0
    bieq t0, ContiguousShape, .hasInstance
.slow:
    callSlowPath(_slow_path_create_rest)
    dispatch()
end)

# JSValue::toBoolean() of what needs no more than the bits of the value and, of a string, its length.
macro aotJumpOnTruthiness(opcodeName, opcodeStruct, ifTruthy, ifFalsy)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        macro jump()
            get(m_targetLabel, t0)
            jumpImpl(dispatchIndirect, t0)
        end

        get(m_condition, t1)
        loadOperandInRegister(size, t1, t0, .conditionIsConstant)
    .hasCondition:
        btqnz t0, ~0xf, .isNoBooleanUndefinedOrNull
        btinz t0, 1, .truthy
    .falsy:
        ifFalsy(jump, dispatch)

    .isNoBooleanUndefinedOrNull:
        bqb t0, numberTag, .isNoInt32
        btiz t0, .falsy
    .truthy:
        ifTruthy(jump, dispatch)

    .isNoInt32:
        btqz t0, numberTag, .isNoNumber
        addq numberTag, t0
        fq2d t0, ft0
        move 0, t1
        fq2d t1, ft1
        bdneq ft0, ft1, .truthy
        jmp .falsy

    .isNoNumber:
        btqnz t0, notCellMask, .slow
        loadb JSCell::m_type[t0], t1
        bbeq t1, StringType, .string
        bbbeq t1, constexpr LastValueCompareCellType, .slow
        btbz JSCell::m_flags[t0], constexpr MasqueradesAsUndefined, .truthy
    .slow:
        callSlowPath(_llint_slow_path_%opcodeName%)
        nextInstruction()

    .conditionIsConstant:
        loadConstantOperand(size, t1, t0, .hasCondition)

    .string:
        loadp JSString::m_fiber[t0], t1
        btpnz t1, isRopeInPointer, .rope
        btinz StringImpl::m_length[t1], .truthy
        jmp .falsy

    .rope:
        btinz RopeStringLength[t0], .truthy
        jmp .falsy
    end)
end

aotJumpOnTruthiness(jtrue, OpJtrue,
    macro (jump, dispatch) jump() end,
    macro (jump, dispatch) dispatch() end)

aotJumpOnTruthiness(jfalse, OpJfalse,
    macro (jump, dispatch) dispatch() end,
    macro (jump, dispatch) jump() end)

macro aotCompareJump(opcodeName, opcodeStruct, compareInt32s)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        get(m_lhs, t2)
        get(m_rhs, t3)
        loadOperandInRegister(size, t2, t0, .leftIsConstant)
    .hasLeftOperand:
        loadOperandInRegister(size, t3, t1, .rightIsConstant)
    .hasRightOperand:
        bqb t0, numberTag, .ordinary
        bqb t1, numberTag, .ordinary
        compareInt32s(t0, t1, .jumps)
        dispatch()

    .jumps:
        get(m_targetLabel, t0)
        jumpImpl(dispatchIndirect, t0)

    .leftIsConstant:
        loadConstantOperand(size, t2, t0, .hasLeftOperand)

    .rightIsConstant:
        loadConstantOperand(size, t3, t1, .hasRightOperand)

    .ordinary:
        jumpToOrdinaryHandler(size, op_%opcodeName%)
    end)
end

aotCompareJump(jless, OpJless,
    macro (left, right, target) bilt left, right, target end)

aotCompareJump(jlesseq, OpJlesseq,
    macro (left, right, target) bilteq left, right, target end)

aotCompareJump(jgreater, OpJgreater,
    macro (left, right, target) bigt left, right, target end)

aotCompareJump(jgreatereq, OpJgreatereq,
    macro (left, right, target) bigteq left, right, target end)

aotCompareJump(jnless, OpJnless,
    macro (left, right, target) bigteq left, right, target end)

aotCompareJump(jnlesseq, OpJnlesseq,
    macro (left, right, target) bigt left, right, target end)

aotCompareJump(jngreater, OpJngreater,
    macro (left, right, target) bilteq left, right, target end)

aotCompareJump(jngreatereq, OpJngreatereq,
    macro (left, right, target) bilt left, right, target end)

macro numberToDouble(number, result)
    bqaeq number, numberTag, .isInt32
    addq numberTag, number
    fq2d number, result
    jmp .isConverted
.isInt32:
    ci2ds number, result
.isConverted:
end

# Both are cells whose types are at most LastValueCompareCellType, and they are not the same cell. Clobbers both.
macro compareStringsStrictly(left, right, scratch1, scratch2, scratch3, equal, notEqual, cannot)
    bbneq JSCell::m_type[left], StringType, cannot
    bbneq JSCell::m_type[right], StringType, cannot
    loadp JSString::m_fiber[left], scratch1
    loadp JSString::m_fiber[right], scratch2
    btpnz scratch1, isRopeInPointer, cannot
    btpnz scratch2, isRopeInPointer, cannot
    bpeq scratch1, scratch2, equal
    loadi StringImpl::m_length[scratch1], scratch3
    bineq scratch3, StringImpl::m_length[scratch2], notEqual
    loadi StringImpl::m_hashAndFlags[scratch1], left
    andi StringImpl::m_hashAndFlags[scratch2], left
    btinz left, constexpr StringImpl::s_hashFlagStringKindIsAtom, notEqual
    btiz left, HashFlags8BitBuffer, cannot
    bia scratch3, MaxLengthOfStringsComparedInline, cannot
    btiz scratch3, equal
    loadp StringImpl::m_data8[scratch1], scratch1
    loadp StringImpl::m_data8[scratch2], scratch2
.compareCharacter:
    subi 1, scratch3
    loadb [scratch1, scratch3], left
    loadb [scratch2, scratch3], right
    bbneq left, right, notEqual
    btinz scratch3, .compareCharacter
    jmp equal
end

# JSValue::strictEqual(). What is left to the ordinary handler, and there to its slow path: ropes, strings of the same length that are long
# or have characters of two bytes, and BigInts.
macro compareStrictly(size, get, opcodeName, equal, notEqual)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadConstantOrVariable(size, t2, t0)
    loadOperandInRegister(size, t3, t1, .rightIsConstant)
.hasRight:
    bqeq t0, t1, .hasSameBits
    btqnz t0, notCellMask, .leftIsNoCell
    btqnz t1, notCellMask, notEqual
    bba JSCell::m_type[t0], constexpr LastValueCompareCellType, notEqual
    bba JSCell::m_type[t1], constexpr LastValueCompareCellType, notEqual
    compareStringsStrictly(t0, t1, t2, t3, t5, equal, notEqual, .ordinary)

    # Only NaN differs from itself.
.hasSameBits:
    btqz t0, numberTag, equal
    bqaeq t0, numberTag, equal
    addq numberTag, t0
    fq2d t0, ft0
    bdeq ft0, ft0, equal
    jmp notEqual

    # Of values with different bits of which one is no cell, only two numbers may be equal, and of those not two int32s.
.leftIsNoCell:
    btqz t0, numberTag, notEqual
    btqz t1, numberTag, notEqual
    move t0, t2
    andq t1, t2
    bqaeq t2, numberTag, notEqual
    numberToDouble(t0, ft0)
    numberToDouble(t1, ft1)
    bdeq ft0, ft1, equal
    jmp notEqual

.ordinary:
    jumpToOrdinaryHandler(size, opcodeName)

.rightIsConstant:
    loadConstantOperand(size, t3, t1, .hasRight)
end

macro aotStrictEquality(opcodeName, opcodeStruct, valueIfEqual, valueIfNotEqual)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        compareStrictly(size, get, op_%opcodeName%, .equal, .notEqual)

    .equal:
        return(valueIfEqual)

    .notEqual:
        return(valueIfNotEqual)
    end)
end

aotStrictEquality(stricteq, OpStricteq, ValueTrue, ValueFalse)
aotStrictEquality(nstricteq, OpNstricteq, ValueFalse, ValueTrue)

macro aotJumpOnStrictEquality(opcodeName, opcodeStruct, ifEqual, ifNotEqual)
    aotOp(op_%opcodeName%, opcodeStruct, macro (size, get, dispatch, metadata, return)
        macro jump()
            get(m_targetLabel, t0)
            jumpImpl(dispatchIndirect, t0)
        end

        compareStrictly(size, get, op_%opcodeName%, .equal, .notEqual)

    .equal:
        ifEqual(jump, dispatch)

    .notEqual:
        ifNotEqual(jump, dispatch)
    end)
end

aotJumpOnStrictEquality(jstricteq, OpJstricteq,
    macro (jump, dispatch) jump() end,
    macro (jump, dispatch) dispatch() end)

aotJumpOnStrictEquality(jnstricteq, OpJnstricteq,
    macro (jump, dispatch) dispatch() end,
    macro (jump, dispatch) jump() end)

# Leaves in flags a word whose bit HashFlags8BitBuffer says whether a character takes one byte.
macro loadFlagsAndLengthOfString(string, flags, length)
    loadp JSString::m_fiber[string], flags
    btpnz flags, isRopeInPointer, .isRope
    loadi StringImpl::m_length[flags], length
    loadi StringImpl::m_hashAndFlags[flags], flags
    jmp .isLoaded
.isRope:
    loadi RopeStringLength[string], length
.isLoaded:
end

# The string that VM::numericStrings keeps of a small integer. The strings are its first member: stringFromInteger().
macro loadStringOfSmallInteger(value, scratch, cannot)
    bqb value, numberTag, cannot
    biaeq value, constexpr NumericStrings::cacheSize, cannot
    zxi2q value, value
    muli sizeof NumericStrings::StringWithJSString, value
    loadp CodeBlock[cfr], scratch
    loadp CodeBlock::m_vm[scratch], scratch
    addp scratch, value
    loadp VM::numericStrings + (constexpr (NumericStrings::StringWithJSString::offsetOfJSString()))[value], value
    btpz value, cannot
end

# jsString(JSGlobalObject*, JSString*, JSString*) of two strings of which neither is empty: JSRopeString::create(). Two int32s are added
# here as well, so that they do not pay for the test. Nothing reads the arithmetic profile where there is no JIT.
aotOp(op_add, OpAdd, macro (size, get, dispatch, metadata, return)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadOperandInRegister(size, t2, t0, .leftIsConstant)
.hasLeftOperand:
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .leftIsNoInt32
    bqb t1, numberTag, .rightIsNoInt32
    baddio t1, t0, .overflows
    orq numberTag, t0
    return(t0)

.overflows:
    subi t1, t0
    ci2ds t0, ft0
    ci2ds t1, ft1
    addd ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.rightIsNoInt32:
    btqnz t1, notCellMask, .addsBoolean
    bbneq JSCell::m_type[t1], StringType, .ordinary
    loadStringOfSmallInteger(t0, t2, .addsNumberToString)
    jmp .hasStrings

.rightIsNotCell:
    loadStringOfSmallInteger(t1, t2, .addsNumberToString)
    jmp .hasStrings

.leftIsNoInt32:
    btqnz t0, notCellMask, .leftIsNoCell
    bbneq JSCell::m_type[t0], StringType, .ordinary
    btqnz t1, notCellMask, .rightIsNotCell
    bbneq JSCell::m_type[t1], StringType, .ordinary
.hasStrings:
    loadFlagsAndLengthOfString(t0, t2, t3)
    btiz t3, .ordinary
    loadFlagsAndLengthOfString(t1, t5, t6)
    btiz t6, .ordinary
    baddio t6, t3, .ordinary
    andi t5, t2
    andi HashFlags8BitBuffer, t2
    orp t0, t2
    orp isRopeInPointer, t2

    # t1: the right string, t2: the left string and the flags, t3: the length
    loadTableInstance(t5, .ordinary)
    loadp InstanceRopeStringAllocator[t5], t6
    allocateCell(t6, t7, t0, .ordinary)
    loadq InstanceStringHeader[t5], t0
    storeq t0, [t7]
    storep t2, JSString::m_fiber[t7]
    move t1, t2
    lshiftq 32, t2
    orq t3, t2
    storeq t2, RopeStringLength[t7]
    urshiftq 32, t1
    storeq t1, RopeStringLength + 8[t7]
    fenceAfterAllocation(t0)
    return(t7)

    # A double and a number.
.leftIsNoCell:
    btqz t0, numberTag, .ordinary
    btqz t1, numberTag, .ordinary
    numberToDouble(t0, ft0)
    numberToDouble(t1, ft1)
    addd ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.addsBoolean:
    btqnz t1, numberTag, .addsDouble
    xorq ValueFalse, t1
    bqa t1, 1, .ordinary
    baddio t1, t0, .ordinary
    orq numberTag, t0
    return(t0)

.addsDouble:
    ci2ds t0, ft0
    numberToDouble(t1, ft1)
    addd ft1, ft0
    fd2q ft0, t0
    subq numberTag, t0
    return(t0)

.addsNumberToString:
    callSlowPath(_llint_slow_path_aot_add_number_to_string)
    dispatch()

.leftIsConstant:
    loadConstantOperand(size, t2, t0, .hasLeftOperand)

.ordinary:
    jumpToOrdinaryHandler(size, op_add)
end)

aotOp(op_to_string, OpToString, macro (size, get, dispatch, metadata, return)
    get(m_operand, t1)
    loadConstantOrVariable(size, t1, t0)
    btqnz t0, notCellMask, .isNoCell
    bbneq JSCell::m_type[t0], StringType, .slow
    return(t0)

.isNoCell:
    loadStringOfSmallInteger(t0, t2, .slow)
    return(t0)

.slow:
    callSlowPath(_llint_slow_path_aot_to_string)
    dispatch()
end)

# jsString(JSGlobalObject*, JSString*, JSString*, JSString*) of three strings of which none is empty, which is what most templates join.
aotOp(op_strcat, OpStrcat, macro (size, get, dispatch, metadata, return)
    getu(size, OpStrcat, m_count, t1)
    bineq t1, 3, .slow
    get(m_src, t1)
    loadq [cfr, t1, 8], t0
    loadq -8[cfr, t1, 8], t2
    loadq -16[cfr, t1, 8], t3
    move t0, t5
    orq t2, t5
    orq t3, t5
    btqnz t5, notCellMask, .slow
    bbneq JSCell::m_type[t0], StringType, .slow
    bbneq JSCell::m_type[t2], StringType, .slow
    bbneq JSCell::m_type[t3], StringType, .slow
    loadFlagsAndLengthOfString(t0, t5, t6)
    btiz t6, .slow
    loadFlagsAndLengthOfString(t2, t7, t1)
    btiz t1, .slow
    baddio t1, t6, .slow
    andi t7, t5
    loadFlagsAndLengthOfString(t3, t7, t1)
    btiz t1, .slow
    baddio t1, t6, .slow
    andi t7, t5
    andi HashFlags8BitBuffer, t5
    orp t0, t5
    orp isRopeInPointer, t5

    # t5: the first string and the flags, t6: the length and the low half of the second string, t2: the rest
    move t2, t1
    lshiftq 32, t1
    orq t1, t6
    urshiftq 32, t2
    lshiftq 16, t3
    orq t3, t2
    loadTableInstance(t1, .slow)
    loadp InstanceRopeStringAllocator[t1], t7
    allocateCell(t7, t0, t3, .slow)
    loadq InstanceStringHeader[t1], t3
    storeq t3, [t0]
    storep t5, JSString::m_fiber[t0]
    storeq t6, RopeStringLength[t0]
    storeq t2, RopeStringLength + 8[t0]
    fenceAfterAllocation(t3)
    return(t0)

.slow:
    callSlowPath(_slow_path_strcat)
    dispatch()
end)

# JSCell::isCallable() of an object that does not masquerade as undefined. Clobbers type.
macro branchOnCallability(object, type, isCallable, isNotCallable, cannot)
    btbnz JSCell::m_flags[object], constexpr MasqueradesAsUndefined, cannot
    subi constexpr JSFunctionType, type
    bibeq type, constexpr (NullSetterFunctionType - JSFunctionType), isCallable
    subi constexpr (SlimClosureType - JSFunctionType), type
    bibeq type, constexpr (SlimClosureWithPropertiesType - SlimClosureType), isCallable
    btbnz JSCell::m_flags[object], constexpr OverridesGetCallData, cannot
    jmp isNotCallable
end

# jsTypeofIsFunction().
aotOp(op_typeof_is_function, OpTypeofIsFunction, macro (size, get, dispatch, metadata, return)
    get(m_operand, t1)
    loadConstantOrVariable(size, t1, t0)
    btqnz t0, notCellMask, .false
    loadb JSCell::m_type[t0], t1
    bbb t1, ObjectType, .false
    branchOnCallability(t0, t1, .true, .false, .slow)

.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_slow_path_typeof_is_function)
    dispatch()
end)

# jsTypeofIsObject().
aotOp(op_typeof_is_object, OpTypeofIsObject, macro (size, get, dispatch, metadata, return)
    get(m_operand, t1)
    loadConstantOrVariable(size, t1, t0)
    btqnz t0, notCellMask, .isNoCell
    loadb JSCell::m_type[t0], t1
    bbb t1, ObjectType, .false
    branchOnCallability(t0, t1, .false, .true, .slow)

.isNoCell:
    bqneq t0, ValueNull, .false
.true:
    return(ValueTrue)

.false:
    return(ValueFalse)

.slow:
    callSlowPath(_slow_path_typeof_is_object)
    dispatch()
end)

# jsTypeStringForValue().
aotOp(op_typeof, OpTypeof, macro (size, get, dispatch, metadata, return)
    get(m_value, t1)
    loadConstantOrVariable(size, t1, t0)
    loadp CodeBlock[cfr], t2
    loadp CodeBlock::m_vm[t2], t2
    btqnz t0, notCellMask, .isNoCell
    loadb JSCell::m_type[t0], t1
    bbeq t1, StringType, .string
    bbb t1, ObjectType, .slow
    branchOnCallability(t0, t1, .function, .object, .slow)

.isNoCell:
    btqnz t0, numberTag, .number
    bqeq t0, ValueUndefined, .undefined
    bqeq t0, ValueNull, .object
    bqeq t0, ValueTrue, .boolean
    bqneq t0, ValueFalse, .slow
.boolean:
    loadp VM::smallStrings + SmallStrings::m_boolean[t2], t0
    return(t0)

.undefined:
    loadp VM::smallStrings + SmallStrings::m_undefined[t2], t0
    return(t0)

.number:
    loadp VM::smallStrings + SmallStrings::m_number[t2], t0
    return(t0)

.string:
    loadp VM::smallStrings + SmallStrings::m_string[t2], t0
    return(t0)

.function:
    loadp VM::smallStrings + SmallStrings::m_function[t2], t0
    return(t0)

.object:
    loadp VM::smallStrings + SmallStrings::m_object[t2], t0
    return(t0)

.slow:
    callSlowPath(_slow_path_typeof)
    dispatch()
end)

# JSString::length(). The length of an array, which the ordinary handler reads under GetByIdMode::ArrayLength, is read here as well, so that
# it does not pay for the test.
aotOp(op_get_length, OpGetLength, macro (size, get, dispatch, metadata, return)
    get(m_base, t0)
    loadOperandInRegister(size, t0, t3, .ordinary)
    btqnz t3, notCellMask, .ordinary
    bbeq JSCell::m_type[t3], StringType, .string
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    btiz t0, IsArray, .ordinary
    btiz t0, IndexingShapeMask, .ordinary
    loadCagedJSValue(JSObjectWithButterfly::m_butterfly[t3], t0, t1)
    loadi -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t0], t0
    bilt t0, 0, .ordinary
    jmp .hasLength

.string:
    loadp JSString::m_fiber[t3], t0
    btpnz t0, isRopeInPointer, .rope
    loadi StringImpl::m_length[t0], t0
    jmp .hasLength
.rope:
    loadi RopeStringLength[t3], t0
.hasLength:
    orq numberTag, t0
    return(t0)

.ordinary:
    jumpToOrdinaryHandler(size, op_get_length)
end)

# The caller need not be bytecode, so its frame does not say which realm this is.
# What the ordinary handler writes for a JIT has no reader here: the StructureID of the receiver for an array profile, the result for a value profile.
macro restoreStackPointer()
    if X86_64
        loadp CodeBlock[cfr], t2
        loadi CodeBlock::m_numCalleeLocals[t2], t2
        negp t2
        leap -maxFrameExtentForSlowPathCall[cfr, t2, 8], sp
    else
        restoreStackPointerAfterCall()
    end
end

macro dispatchAfterCallWithoutProfile(size, opcodeStruct, valueProfileName, dstVirtualRegister, dispatch)
    loadPC()
    get(size, opcodeStruct, dstVirtualRegister, t1)
    storeq r0, [cfr, t1, 8]
    dispatch()
end

macro invokeCallWithoutProfile(opcodeName, size, opcodeStruct, valueProfileName, dstVirtualRegister, dispatch, callee, maybeOldCFR, callPtrTag)
    callTargetFunction(opcodeName, size, opcodeStruct, dispatchAfterCallWithoutProfile, valueProfileName, dstVirtualRegister, dispatch, callee, callPtrTag)
end

if not ARM64E and not C_LOOP
# callHelper(). The word of a site that has seen several callees is not the address of a cell.
macro aotRegularCall(opcodeName, opcodeStruct, size, get, finish)
    locateSite(size, opcodeStruct, t5, t0)
    if X86_64
        loadp %opcodeStruct%::Metadata::m_callLinkInfo + LazyCallLinkInfo::m_data[metadataTable, t5, 1], t5
    else
        loadp %opcodeStruct%::Metadata::m_callLinkInfo + LazyCallLinkInfo::m_data[t5], t5
    end
    get(m_callee, t1)
    loadOperandInRegister(size, t1, t0, .calleeIsConstant)
.hasCallee:
    getu(size, opcodeStruct, m_argv, t3)
    if X86_64
        negp t3
        leap [cfr, t3, 8], t3
    else
        lshifti 3, t3
        subp cfr, t3, t3
    end
    getu(size, opcodeStruct, m_argc, t2)
    storei t2, ArgumentCountIncludingThis + LowWordOffset[t3]
    storePC()
    storeq t0, Callee[t3]
    if X86_64
        leap CallerFrameAndPCSize[t3], sp
    else
        move t3, sp
        addp CallerFrameAndPCSize, sp
    end
    loadp CallLinkInfo::m_callee[t5], t1
    bqneq t0, t1, .otherCallee
    loadp CallLinkInfo::m_codeBlock[t5], t2
    storep t2, CodeBlock[t3]
.hasTarget:
    loadp CallLinkInfo::m_monomorphicCallDestination[t5], t5
.enters:
    call t5, JSEntryPtrTag
    defineReturnLabel(opcodeName, size)
    restoreStackPointer()
    loadPC()
    finish()

    # t0 is the callee and t2 the CallLinkInfo.
.otherCallee:
    move t5, t2
    btpz t1, (constexpr CallLinkInfo::polymorphicCalleeMask), .isUnlinked
    loadp CallLinkInfo::m_codeBlock[t5], t1
    storep t1, CodeBlock[t3]
    jmp .hasTarget

.isUnlinked:
    storep 0, CodeBlock[t3]
    leap _g_config, t5
    loadp JSCConfigOffset + constexpr JSC::offsetOfJSCConfigDefaultCallThunk[t5], t5
    jmp .enters

.calleeIsConstant:
    loadConstantOperand(size, t1, t0, .hasCallee)
end

aotOp(op_call, OpCall, macro (size, get, dispatch, metadata, return)
    aotRegularCall(aot_op_call, OpCall, size, get, macro ()
        get(m_dst, t1)
        storeq r0, [cfr, t1, 8]
        dispatch()
    end)
end)

aotOp(op_call_ignore_result, OpCallIgnoreResult, macro (size, get, dispatch, metadata, return)
    aotRegularCall(aot_op_call_ignore_result, OpCallIgnoreResult, size, get, macro ()
        dispatch()
    end)
end)

macro prepareTailCallSite()
    btpnz CallLinkInfo::m_owner[t5], .hasCallLinkInfo
    prepareStateForCCall()
    move cfr, a0
    move PC, a1
    cCall2(_llint_slow_path_ensure_call_link_info)
    restoreStateAfterCCall()
    move r1, t5
    loadq Callee - CallerFrameAndPCSize[sp], t0
    move sp, t3
    subp CallerFrameAndPCSize, t3
.hasCallLinkInfo:
end

aotOp(op_tail_call, OpTailCall, macro (size, get, dispatch, metadata, return)
    metadata(t5, t0)

    macro getCallee(dst)
        get(m_callee, dst)
    end

    macro getArgumentStart(dst)
        getu(size, OpTailCall, m_argv, dst)
    end

    macro getArgumentCount(dst)
        getu(size, OpTailCall, m_argc, dst)
    end

    callHelper(aot_op_tail_call, OpTailCall, dispatchAfterTailCall, m_valueProfile, m_dst, prepareForTailCall, invokeForTailCall, prepareForSlowTailCall, prepareTailCallSite, size, dispatch, metadata, getCallee, getArgumentStart, getArgumentCount)
end)

aotOp(op_construct, OpConstruct, macro (size, get, dispatch, metadata, return)
    # `new f()` of a function of the program that only returns. What AOT::ClassTable::createThis() remembers of it is the whole construction: nothing is entered.
    get(m_callee, t1)
    loadConstantOrVariable(size, t1, t0)
    btqnz t0, notCellMask, .enters
    bbneq JSCell::m_type[t0], constexpr CompiledClosureType, .enters
    loadp JSFunction::m_executableOrRareData[t0], t1
    btpz t1, (constexpr JSFunction::rareDataTag), .enters
    bbneq (FunctionRareData::m_compiledAllocationConstructorHighBits - (constexpr JSFunction::rareDataTag))[t1], constexpr AOT::Interpreter::trivialConstructorHighBits, .enters
    loadi (FunctionRareData::m_compiledAllocationStructureID - (constexpr JSFunction::rareDataTag))[t1], t2
    btiz t2, .enters
    # new.target lies where the callee would find `this`.
    getu(size, OpConstruct, m_argv, t3)
    negp t3
    bqneq ThisArgumentOffset[cfr, t3, 8], t0, .enters
    loadp CodeBlock[cfr], t1
    loadp CodeBlock::m_vm[t1], t1
    loadp (constexpr AOT::Interpreter::offsetOfDefaultFinalObjectAllocator)[t1], t1
    btpz t1, .enters
    allocateCell(t1, t0, t3, .enters)
    storei t2, JSCell::m_structureID[t0]
    structureIDToStructureWithScratch(t2, t3)
    loadi StructureTypeInfoBlob[t2], t3
    storei t3, JSCell::m_indexingTypeAndMisc[t0]
    storep 0, JSObjectWithButterfly::m_butterfly[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly + 8[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly + 16[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly + 24[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly + 32[t0]
    storeq ValueEmpty, sizeof JSObjectWithButterfly + 40[t0]
    fenceAfterAllocation(t3)
    return(t0)

.enters:
    metadata(t5, t0)

    macro getCallee(dst)
        get(m_callee, dst)
    end

    macro getArgumentStart(dst)
        getu(size, OpConstruct, m_argv, dst)
    end

    macro getArgumentCount(dst)
        getu(size, OpConstruct, m_argc, dst)
    end

    callHelper(aot_op_construct, OpConstruct, dispatchAfterCallWithoutProfile, m_valueProfile, m_dst, prepareForRegularCall, invokeCallWithoutProfile, prepareForSlowRegularCall, prepareCallSiteForConstruct, size, dispatch, metadata, getCallee, getArgumentStart, getArgumentCount)
end)
end

macro loadInstanceOfCompiledCallee(callee, instance, scratch)
    loadStructureWithScratch(callee, instance, scratch)
    loadp Structure::m_realm[instance], instance
    loadp JSGlobalObject::m_aotInstance[instance], instance
end

if X86_64 or ARM64
# The prologue and op_enter in one. Every test stands in front of the first store into the frame, so a rare case gives the frame back and enters the ordinary prologue.
macro aotFunctionPrologue(ordinaryPrologue, checkArity)
    preserveCallerPCAndCFR()
    loadp CodeBlock[cfr], t1
    loadp CodeBlock::m_vm[t1], t2
    if X86_64
        loadi CodeBlock::m_numCalleeLocals[t1], t0
        negp t0
        leap -maxFrameExtentForSlowPathCall[cfr, t0, 8], t0
    else
        getFrameRegisterSizeForCodeBlock(t1, t0)
        subp cfr, t0, t0
    end
    bpa VMTrapAwareSoftStackLimitOffset[t2], t0, .ordinary
    checkArity(.ordinary)
    # Each makes a word negative: a trap, a CodeBlock whose state is at or below the threshold of the write barrier, a tainted CodeBlock.
    loadi VM::m_threadContext + VMThreadContext::m_traps + VMTraps::m_trapBits[t2], t5
    andi VMTrapsAsyncEvents, t5
    negi t5
    loadb JSCell::m_cellState[t1], t3
    subi (constexpr (VM::offsetOfHeapBarrierThreshold()))[t2], t3
    subi 1, t3
    ori t5, t3
    ori CodeBlock::m_numberOfArgumentsToSkipAndCouldBeTainted[t1], t3
    bilt t3, 0, .ordinary

    preserveCalleeSavesUsedByLLInt()
    move sp, t2
    move t0, sp

    # The variables, which begin the frame, are undefined. What follows them is written before it is read.
    # Up to three words past them are written, which are the frame's or lie below it.
    loadi CodeBlock::m_numVars[t1], t3
    if X86_64
        negp t3
        leap [cfr, t3, 8], t3
    else
        lshiftp 3, t3
        subp cfr, t3, t3
    end
    bpbeq t2, t3, .frameIsFilled
    if ARM64
        move ValueUndefined, t5
    .fillFrame:
        storepairq t5, t5, -2 * PtrSize[t2]
        storepairq t5, t5, -4 * PtrSize[t2]
        subp 4 * PtrSize, t2
        bpa t2, t3, .fillFrame
    else
        leap _g_undefinedValues, t5
        loadv [t5], v0
    .fillFrame:
        storev v0, -2 * PtrSize[t2]
        storev v0, -4 * PtrSize[t2]
        subp 4 * PtrSize, t2
        bpa t2, t3, .fillFrame
    end
.frameIsFilled:

    loadis CodeBlock::m_scopeRegister[t1], t2
    loadp Callee[cfr], t0
    loadp JSCallee::m_scope[t0], t0
    storeq t0, [cfr, t2, 8]

    loadp CodeBlock::m_instructionsRawPointer[t1], PB
    loadp CodeBlock::m_metadata[t1], metadataTable
    move TagNumber, numberTag
    addq TagOther, numberTag, notCellMask
    assert(macro (ok) bbeq [PB], constexpr op_enter, ok end)
    move (constexpr op_enter_length) + OpcodeIDNarrowSize, PC
    nextInstruction()

.ordinary:
    restoreCallerPCAndCFR()
    jmp ordinaryPrologue
end

macro aotArityIsNotChecked(ordinary)
end

macro aotArityIsChecked(ordinary)
    loadi LowWordOffset + ArgumentCountIncludingThis[cfr], t3
    bib t3, CodeBlock::m_numParameters[t1], ordinary
end

op(llint_aot_function_for_call_prologue, macro ()
    aotFunctionPrologue(_llint_function_for_call_prologue, aotArityIsNotChecked)
end)

op(llint_aot_function_for_construct_prologue, macro ()
    aotFunctionPrologue(_llint_function_for_construct_prologue, aotArityIsNotChecked)
end)

op(llint_aot_function_for_call_arity_check, macro ()
    aotFunctionPrologue(_llint_function_for_call_arity_check, aotArityIsChecked)
end)

op(llint_aot_function_for_construct_arity_check, macro ()
    aotFunctionPrologue(_llint_function_for_construct_arity_check, aotArityIsChecked)
end)
end

# A call site that has seen too many callees for a stub. t0: the callee, t2: the CallLinkInfo. The ordinary trampoline needs both, and t1, as they are.
# AOT::Closure::getCallData() and AOT::SlimClosure::getCallData(). The sign of the word of a closure says that it is a class.
op(llint_aot_virtual_call_trampoline, macro ()
    btqnz t0, NotCellMask, .ordinary
    loadb JSCell::m_type[t0], t3
    bbeq t3, constexpr CompiledClosureType, .closure
    subi constexpr SlimClosureType, t3
    bia t3, constexpr (SlimClosureWithPropertiesType - SlimClosureType), .ordinary
    loadInstanceOfCompiledCallee(t0, t5, t3)
    loadp InstanceSlimClosureEntrypoint[t5], t5
    jmp t5, JSEntryPtrTag

.closure:
    loadq (constexpr AOT::offsetOfClosureEntry)[t0], t3
    bqlt t3, 0, .ordinary
    loadInstanceOfCompiledCallee(t0, t5, t3)
    loadp InstanceClosureEntrypoint[t5], t5
    jmp t5, JSEntryPtrTag

.ordinary:
    jmp _llint_virtual_call_trampoline
end)

if not C_LOOP
# What a call enters in place of llint_native_call_trampoline where the callee is String.prototype.charCodeAt: VM::getHostFunction(). A string that is whole, or a piece of one that is, at an int32
# inside it: text that is made at run time to compare strings is made of these. It makes no frame, so the arguments lie where the caller has put them. Everything else is the host function's.
if X86_64
    const CalleeFrameAtEntry = PtrSize - CallerFrameAndPCSize
else
    const CalleeFrameAtEntry = -CallerFrameAndPCSize
end

global _llint_aot_char_code_at_thunk
_llint_aot_char_code_at_thunk:
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t0
    bib t0, 2, .hostFunction
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t2
    move constexpr JSValue::NotCellMask, t1
    btqnz t3, t1, .hostFunction
    bbneq JSCell::m_type[t3], StringType, .hostFunction
    move constexpr JSValue::NumberTag, t5
    bqb t2, t5, .hostFunction
    loadp JSString::m_fiber[t3], t1
    btpnz t1, isRopeInPointer, .rope
    biaeq t2, StringImpl::m_length[t1], .hostFunction
    zxi2q t2, t2
.hasCharacters:
    loadi StringImpl::m_hashAndFlags[t1], t3
    loadp StringImpl::m_data8[t1], t1
    btiz t3, HashFlags8BitBuffer, .hasWideCharacters
    loadb [t1, t2], r0
    orq t5, r0
    ret

.hasWideCharacters:
    loadh [t1, t2, 2], r0
    orq t5, r0
    ret

    # A piece of another string stays a rope: the host function reads it through its base as well.
.rope:
    btpz t1, (constexpr JSRopeString::isSubstringInPointer), .hostFunction
    biaeq t2, RopeStringLength[t3], .hostFunction
    loadq (constexpr (JSRopeString::offsetOfFiber2()))[t3], t1
    urshiftq (constexpr JSRopeString::CompactFibers::shiftOfFiber2InUpperWord), t1
    zxi2q t2, t2
    addq t1, t2
    loadi (constexpr (JSRopeString::offsetOfFiber1Lower()))[t3], t1
    loadh (constexpr (JSRopeString::offsetOfFiber2()))[t3], t0
    lshiftq 32, t0
    orq t0, t1
    loadp JSString::m_fiber[t1], t1
    btpnz t1, isRopeInPointer, .hostFunction
    jmp .hasCharacters

.hostFunction:
    jmp _llint_native_call_trampoline

# The same for String.prototype.indexOf: one character of one byte looked for in a whole string of such characters, from the start or from an int32 that is not negative: text that is made at run time to
# take a URL apart asks for this. Everything else is the host function's.
global _llint_aot_string_index_of_thunk
_llint_aot_string_index_of_thunk:
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t0
    bib t0, 2, .indexOfIsHostFunctions
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t2
    move constexpr JSValue::NotCellMask, t1
    btqnz t3, t1, .indexOfIsHostFunctions
    btqnz t2, t1, .indexOfIsHostFunctions
    bbneq JSCell::m_type[t3], StringType, .indexOfIsHostFunctions
    bbneq JSCell::m_type[t2], StringType, .indexOfIsHostFunctions
    loadp JSString::m_fiber[t3], t3
    btpnz t3, isRopeInPointer, .indexOfIsHostFunctions
    loadp JSString::m_fiber[t2], t2
    btpnz t2, isRopeInPointer, .indexOfIsHostFunctions
    bineq StringImpl::m_length[t2], 1, .indexOfIsHostFunctions
    btiz StringImpl::m_hashAndFlags[t2], HashFlags8BitBuffer, .indexOfIsHostFunctions
    btiz StringImpl::m_hashAndFlags[t3], HashFlags8BitBuffer, .indexOfIsHostFunctions
    move 0, t1
    bib t0, 3, .indexOfHasStart
    loadq CalleeFrameAtEntry + FirstArgumentOffset + 8[sp], t1
    move constexpr JSValue::NumberTag, t5
    bqb t1, t5, .indexOfIsHostFunctions
    bilt t1, 0, .indexOfIsHostFunctions
    zxi2q t1, t1
.indexOfHasStart:
    loadp StringImpl::m_data8[t2], t2
    loadb [t2], t2
    loadi StringImpl::m_length[t3], t0
    loadp StringImpl::m_data8[t3], t3
.indexOfNextCharacter:
    biaeq t1, t0, .indexOfFindsNothing
    loadb [t3, t1], t5
    bieq t5, t2, .indexOfFindsCharacter
    addi 1, t1
    jmp .indexOfNextCharacter

.indexOfFindsNothing:
    move 0xffffffff, t1
.indexOfFindsCharacter:
    move constexpr JSValue::NumberTag, t5
    move t1, r0
    orq t5, r0
    ret

.indexOfIsHostFunctions:
    jmp _llint_native_call_trampoline

# What a call enters in place of llint_native_call_trampoline where the callee is a bound function: VM::getBoundFunction(). No stack trace lists the frame of a bound function, so the target of one
# that binds a receiver alone is entered in its place, in the frame that the caller has made. Everything else is the host function's.
global _llint_aot_bound_function_call_thunk
_llint_aot_bound_function_call_thunk:
    loadp CalleeFrameAtEntry + Callee[sp], t3
    btinz (constexpr (JSBoundFunction::offsetOfBoundArgsLength()))[t3], .boundHostFunction
    loadp (constexpr (JSBoundFunction::offsetOfTargetFunction()))[t3], t0
    loadb JSCell::m_type[t0], t1
    bbeq t1, constexpr CompiledClosureType, .boundClosure
    bbeq t1, JSFunctionType, .boundFunction
    subi constexpr SlimClosureType, t1
    bia t1, constexpr (SlimClosureWithPropertiesType - SlimClosureType), .boundHostFunction
    loadInstanceOfCompiledCallee(t0, t5, t1)
    loadp InstanceSlimClosureEntrypoint[t5], t5
.enterBoundTarget:
    loadq (constexpr (JSBoundFunction::offsetOfBoundThis()))[t3], t1
    storeq t1, CalleeFrameAtEntry + ThisArgumentOffset[sp]
    storep t0, CalleeFrameAtEntry + Callee[sp]
    jmp t5, JSEntryPtrTag

    # The sign of the word of a closure says that it is a class.
.boundClosure:
    loadq (constexpr AOT::offsetOfClosureEntry)[t0], t1
    bqlt t1, 0, .boundHostFunction
    loadInstanceOfCompiledCallee(t0, t5, t1)
    loadp InstanceClosureEntrypoint[t5], t5
    jmp .enterBoundTarget

    # virtualThunkFor().
.boundFunction:
    loadp JSFunction::m_executableOrRareData[t0], t1
    btpz t1, (constexpr JSFunction::rareDataTag), .boundTargetHasExecutable
    loadp (FunctionRareData::m_executable - (constexpr JSFunction::rareDataTag))[t1], t1
.boundTargetHasExecutable:
    loadp ExecutableBase::m_jitCodeForCallWithArityCheck[t1], t5
    btpz t5, .boundHostFunction
    move 0, t2
    bbneq JSCell::m_type[t1], FunctionExecutableType, .boundTargetHasCodeBlock
    loadp FunctionExecutable::m_codeBlockForCall[t1], t2
.boundTargetHasCodeBlock:
    storep t2, CalleeFrameAtEntry + CodeBlock[sp]
    jmp .enterBoundTarget

.boundHostFunction:
    jmp _llint_native_call_trampoline

# The same for predicates that text made at run time to validate values is full of, where the bits of the first argument answer. Everything else is the host function's.
macro loadFirstArgumentOfThunk(value, hostFunction)
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], value
    bib value, 2, hostFunction
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], value
end

# isNaN() and Number.isNaN() of a number.
global _llint_aot_is_nan_thunk
_llint_aot_is_nan_thunk:
    loadFirstArgumentOfThunk(t0, .isNaNHostFunction)
    move constexpr JSValue::NumberTag, t1
    bqaeq t0, t1, .isNoNaN
    btqz t0, t1, .isNaNHostFunction
    addq t1, t0
    fq2d t0, ft0
    bdnequn ft0, ft0, .isNaN
.isNoNaN:
    move ValueFalse, r0
    ret
.isNaN:
    move ValueTrue, r0
    ret
.isNaNHostFunction:
    jmp _llint_native_call_trampoline

# isFinite() and Number.isFinite() of a number: every bit of the exponent is set in an infinity and in NaN.
global _llint_aot_is_finite_thunk
_llint_aot_is_finite_thunk:
    loadFirstArgumentOfThunk(t0, .isFiniteHostFunction)
    move constexpr JSValue::NumberTag, t1
    bqaeq t0, t1, .isFinite
    btqz t0, t1, .isFiniteHostFunction
    addq t1, t0
    move 0x7ff0000000000000, t1
    andq t1, t0
    bqeq t0, t1, .isNotFinite
.isFinite:
    move ValueTrue, r0
    ret
.isNotFinite:
    move ValueFalse, r0
    ret
.isFiniteHostFunction:
    jmp _llint_native_call_trampoline

# Number.isInteger() and Number.isSafeInteger() of an int32.
global _llint_aot_is_integer_thunk
_llint_aot_is_integer_thunk:
    loadFirstArgumentOfThunk(t0, .isIntegerHostFunction)
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .isIntegerHostFunction
    move ValueTrue, r0
    ret
.isIntegerHostFunction:
    jmp _llint_native_call_trampoline

# Array.isArray() of everything but a proxy.
global _llint_aot_is_array_thunk
_llint_aot_is_array_thunk:
    loadFirstArgumentOfThunk(t0, .isArrayHostFunction)
    move constexpr JSValue::NotCellMask, t1
    btqnz t0, t1, .isNoArray
    loadb JSCell::m_type[t0], t1
    bbeq t1, ArrayType, .isArray
    bbeq t1, constexpr DerivedArrayType, .isArray
    bbeq t1, constexpr ProxyObjectType, .isArrayHostFunction
.isNoArray:
    move ValueFalse, r0
    ret
.isArray:
    move ValueTrue, r0
    ret
.isArrayHostFunction:
    jmp _llint_native_call_trampoline

macro loadThunkVM(vm, scratch1, scratch2)
    loadp CalleeFrameAtEntry + Callee[sp], scratch1
    loadStructureWithScratch(scratch1, vm, scratch2)
    loadp Structure::m_realm[vm], vm
    loadp JSGlobalObject::m_vm[vm], vm
end

# String.prototype.charAt() of a whole string at an int32 inside it.
global _llint_aot_char_at_thunk
_llint_aot_char_at_thunk:
    loadFirstArgumentOfThunk(t2, .charAtHostFunction)
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    move constexpr JSValue::NotCellMask, t1
    btqnz t3, t1, .charAtHostFunction
    bbneq JSCell::m_type[t3], StringType, .charAtHostFunction
    move constexpr JSValue::NumberTag, t1
    bqb t2, t1, .charAtHostFunction
    loadp JSString::m_fiber[t3], t1
    btpnz t1, isRopeInPointer, .charAtHostFunction
    biaeq t2, StringImpl::m_length[t1], .charAtHostFunction
    zxi2q t2, t2
    loadStringCharacter(t1, t2, t0, .charAtHostFunction)
    loadThunkVM(t1, t2, t3)
    loadp VM::smallStrings + SmallStrings::m_singleCharacterStrings[t1, t0, PtrSize], r0
    ret
.charAtHostFunction:
    jmp _llint_native_call_trampoline

# String.fromCharCode() of one character that takes one byte.
global _llint_aot_from_char_code_thunk
_llint_aot_from_char_code_thunk:
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t0
    bineq t0, 2, .fromCharCodeHostFunction
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t0
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .fromCharCodeHostFunction
    bia t0, constexpr maxSingleCharacterString, .fromCharCodeHostFunction
    zxi2q t0, t0
    loadThunkVM(t1, t2, t3)
    loadp VM::smallStrings + SmallStrings::m_singleCharacterStrings[t1, t0, PtrSize], r0
    ret
.fromCharCodeHostFunction:
    jmp _llint_native_call_trampoline

# Object.prototype.hasOwnProperty() of an object and an atom, by the HasOwnPropertyCache that the host function fills.
global _llint_aot_has_own_property_thunk
_llint_aot_has_own_property_thunk:
    loadFirstArgumentOfThunk(t0, .hasOwnPropertyHostFunction)
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    move constexpr JSValue::NotCellMask, t1
    btqnz t0, t1, .hasOwnPropertyHostFunction
    btqnz t3, t1, .hasOwnPropertyHostFunction
    bbneq JSCell::m_type[t0], StringType, .hasOwnPropertyHostFunction
    bbb JSCell::m_type[t3], ObjectType, .hasOwnPropertyHostFunction
    loadp JSString::m_fiber[t0], t2
    btpnz t2, isRopeInPointer, .hasOwnPropertyHostFunction
    loadi StringImpl::m_hashAndFlags[t2], t5
    btiz t5, constexpr StringImpl::s_hashFlagStringKindIsAtom, .hasOwnPropertyHostFunction
    loadThunkVM(t1, t0, t6)
    loadp VM::m_hasOwnPropertyCache[t1], t1
    btpnz t1, HasOwnPropertyCacheIsLazy, .hasOwnPropertyHostFunction
    loadi JSCell::m_structureID[t3], t0
    urshifti constexpr StringImpl::s_flagCount, t5
    addi t0, t5
    andi constexpr HasOwnPropertyCache::mask, t5
    muli constexpr (sizeof(HasOwnPropertyCache::Entry)), t5
    addp t1, t5
    bpneq t2, (constexpr (HasOwnPropertyCache::Entry::offsetOfImpl()))[t5], .hasOwnPropertyHostFunction
    bineq t0, (constexpr (HasOwnPropertyCache::Entry::offsetOfStructureID()))[t5], .hasOwnPropertyHostFunction
    loadb (constexpr (HasOwnPropertyCache::Entry::offsetOfResult()))[t5], t1
    orq ValueFalse, t1
    move t1, r0
    ret
.hasOwnPropertyHostFunction:
    jmp _llint_native_call_trampoline

# Math.floor() of an int32, and of a double whose result is one.
global _llint_aot_math_floor_thunk
_llint_aot_math_floor_thunk:
    loadFirstArgumentOfThunk(t0, .floorHostFunction)
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .floorOfNonInt32
    move t0, r0
    ret
.floorOfNonInt32:
    btqz t0, t1, .floorHostFunction
    addq t1, t0
    fq2d t0, ft0
    bdnequn ft0, ft0, .floorHostFunction
    truncated2qs ft0, t2
    move 0x4000000000000000, t3
    addq t2, t3
    bqlt t3, 0, .floorHostFunction
    cq2ds t2, ft1
    bdlteq ft1, ft0, .isFloor
    subq 1, t2
.isFloor:
    btqnz t2, .isNotNegativeZero
    bqlt t0, 0, .floorHostFunction
.isNotNegativeZero:
    sxi2q t2, t3
    bqneq t2, t3, .floorHostFunction
    zxi2q t2, t2
    orq t1, t2
    move t2, r0
    ret
.floorHostFunction:
    jmp _llint_native_call_trampoline

macro loadThunkInt32Arguments(first, second, tag, hostFunction)
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], first
    bineq first, 3, hostFunction
    move constexpr JSValue::NumberTag, tag
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], first
    bqb first, tag, hostFunction
    loadq CalleeFrameAtEntry + FirstArgumentOffset + 8[sp], second
    bqb second, tag, hostFunction
end

global _llint_aot_math_imul_thunk
_llint_aot_math_imul_thunk:
    loadThunkInt32Arguments(t0, t2, t1, .imulHostFunction)
    muli t2, t0
    orq t1, t0
    move t0, r0
    ret
.imulHostFunction:
    jmp _llint_native_call_trampoline

# Leaves a double, and the number in result as it is.
macro loadThunkDouble(value, tag, scratch, result, hostFunction)
    bqaeq value, tag, .isInt32
    btqz value, tag, hostFunction
    move value, scratch
    addq tag, scratch
    fq2d scratch, result
    jmp .isLoaded
.isInt32:
    ci2ds value, result
.isLoaded:
end

# Math.max() and Math.min() of int32s, and of two numbers of which one is greater.
macro minMaxThunk(keepsFirstInt32, keepsFirstDouble, hostFunction)
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t2
    bib t2, 2, hostFunction
    subi 2, t2
    move constexpr JSValue::NumberTag, t1
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t0
    bqb t0, t1, .isNoInt32
.next:
    btiz t2, .isLast
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp, t2, 8], t3
    bqb t3, t1, .isNoInt32
    subi 1, t2
    keepsFirstInt32(t0, t3, .next)
    move t3, t0
    jmp .next
.isLast:
    move t0, r0
    ret

.isNoInt32:
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t2
    bineq t2, 3, hostFunction
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t0
    loadThunkDouble(t0, t1, t2, ft0, hostFunction)
    loadq CalleeFrameAtEntry + FirstArgumentOffset + 8[sp], t3
    loadThunkDouble(t3, t1, t2, ft1, hostFunction)
    keepsFirstDouble(ft0, ft1, .isLast)
    move t3, t0
    keepsFirstDouble(ft1, ft0, .isLast)
end

global _llint_aot_math_max_thunk
_llint_aot_math_max_thunk:
    minMaxThunk(macro (first, second, keepsFirst) bigteq first, second, keepsFirst end, macro (first, second, keepsFirst) bdgt first, second, keepsFirst end, .maxHostFunction)
.maxHostFunction:
    jmp _llint_native_call_trampoline

global _llint_aot_math_min_thunk
_llint_aot_math_min_thunk:
    minMaxThunk(macro (first, second, keepsFirst) bilteq first, second, keepsFirst end, macro (first, second, keepsFirst) bdlt first, second, keepsFirst end, .minHostFunction)
.minHostFunction:
    jmp _llint_native_call_trampoline

global _llint_aot_math_abs_thunk
_llint_aot_math_abs_thunk:
    loadFirstArgumentOfThunk(t0, .absHostFunction)
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .absOfNonInt32
    bigteq t0, 0, .isAbsolute
    bieq t0, -2147483648, .absHostFunction
    negi t0
    orq t1, t0
.isAbsolute:
    move t0, r0
    ret
.absOfNonInt32:
    btqz t0, t1, .absHostFunction
    addq t1, t0
    lshiftq 1, t0
    urshiftq 1, t0
    subq t1, t0
    move t0, r0
    ret
.absHostFunction:
    jmp _llint_native_call_trampoline

# An int32 where the root is one.
global _llint_aot_math_sqrt_thunk
_llint_aot_math_sqrt_thunk:
    loadFirstArgumentOfThunk(t0, .sqrtHostFunction)
    move constexpr JSValue::NumberTag, t1
    loadThunkDouble(t0, t1, t2, ft0, .sqrtHostFunction)
    sqrtd ft0, ft0
    bcd2i ft0, t0, .rootIsNotInt32
    orq t1, t0
    move t0, r0
    ret
.rootIsNotInt32:
    bdnequn ft0, ft0, .sqrtHostFunction
    fd2q ft0, t0
    subq t1, t0
    move t0, r0
    ret
.sqrtHostFunction:
    jmp _llint_native_call_trampoline

# Math.ceil(), Math.trunc() and Math.round() of an int32, and of a double whose result is an int32 other than zero.
macro roundingThunk(correct, hostFunction)
    loadFirstArgumentOfThunk(t0, hostFunction)
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .isNoInt32
    move t0, r0
    ret
.isNoInt32:
    btqz t0, t1, hostFunction
    addq t1, t0
    fq2d t0, ft0
    bdnequn ft0, ft0, hostFunction
    truncated2qs ft0, t2
    move 0x4000000000000000, t3
    addq t2, t3
    bqlt t3, 0, hostFunction
    cq2ds t2, ft1
    correct(ft0, ft1, t2, t0)
    btqz t2, hostFunction
    sxi2q t2, t3
    bqneq t2, t3, hostFunction
    zxi2q t2, t2
    orq t1, t2
    move t2, r0
    ret
end

global _llint_aot_math_ceil_thunk
_llint_aot_math_ceil_thunk:
    roundingThunk(macro (value, truncatedValue, result, scratch)
        bdgteq truncatedValue, value, .isCeiling
        addq 1, result
    .isCeiling:
    end, .ceilHostFunction)
.ceilHostFunction:
    jmp _llint_native_call_trampoline

global _llint_aot_math_trunc_thunk
_llint_aot_math_trunc_thunk:
    roundingThunk(macro (value, truncatedValue, result, scratch) end, .truncHostFunction)
.truncHostFunction:
    jmp _llint_native_call_trampoline

# The difference to the truncated value is exact.
global _llint_aot_math_round_thunk
_llint_aot_math_round_thunk:
    roundingThunk(macro (value, truncatedValue, result, scratch)
        subd truncatedValue, value
        move 0x3fe0000000000000, scratch
        fq2d scratch, truncatedValue
        bdlt value, truncatedValue, .isNotRoundedUp
        addq 1, result
    .isNotRoundedUp:
        addd truncatedValue, value
        move 0, scratch
        fq2d scratch, truncatedValue
        bdgteq value, truncatedValue, .isNotRoundedDown
        subq 1, result
    .isNotRoundedDown:
    end, .roundHostFunction)
.roundHostFunction:
    jmp _llint_native_call_trampoline

global _llint_aot_math_clz32_thunk
_llint_aot_math_clz32_thunk:
    loadFirstArgumentOfThunk(t0, .clz32HostFunction)
    move constexpr JSValue::NumberTag, t1
    bqb t0, t1, .clz32HostFunction
    if X86_64
        # Processors without LZCNT execute BSR in its place, so the exponent of the number as a double is used: 1023 + 31 less the exponent.
        move 32, t2
        btiz t0, .hasLeadingZeros
        zxi2q t0, t0
        cq2ds t0, ft0
        fd2q ft0, t0
        urshiftq 52, t0
        move 1054, t2
        subi t0, t2
    .hasLeadingZeros:
    else
        lzcnti t0, t2
    end
    orq t1, t2
    move t2, r0
    ret
.clz32HostFunction:
    jmp _llint_native_call_trampoline

# Array.prototype.push() of one value that fits behind the elements of an array of int32s or of values: JSArray::pushInline().
global _llint_aot_array_push_thunk
_llint_aot_array_push_thunk:
    loadi CalleeFrameAtEntry + ArgumentCountIncludingThis + LowWordOffset[sp], t0
    bineq t0, 2, .pushesSeveral
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    loadq CalleeFrameAtEntry + FirstArgumentOffset[sp], t2
    move constexpr JSValue::NotCellMask, t1
    btqnz t3, t1, .pushHostFunction
    bbneq JSCell::m_type[t3], ArrayType, .pushHostFunction
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    andi constexpr IndexingModeMask, t0
    bieq t0, constexpr ArrayWithContiguous, .pushesValue
    bineq t0, constexpr ArrayWithInt32, .pushHostFunction
    move constexpr JSValue::NumberTag, t5
    bqb t2, t5, .pushHostFunction
    jmp .pushesNoCell

.pushesValue:
    btqnz t2, t1, .pushesNoCell
    loadp CalleeFrameAtEntry + Callee[sp], t0
    loadStructureWithScratch(t0, t1, t5)
    loadp Structure::m_realm[t1], t1
    loadp JSGlobalObject::m_vm[t1], t1
    loadi (constexpr (VM::offsetOfHeapBarrierThreshold()))[t1], t1
    bbbeq JSCell::m_cellState[t3], t1, .pushHostFunction
.pushesNoCell:
    loadp JSObjectWithButterfly::m_butterfly[t3], t5
    loadi -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t5], t1
    biaeq t1, -sizeof IndexingHeader + IndexingHeader::u.lengths.vectorLength[t5], .pushHostFunction
    storeq t2, [t5, t1, 8]
    addi 1, t1
    storei t1, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t5]
    move constexpr JSValue::NumberTag, r0
    orq t1, r0
    ret

.pushesSeveral:
    bib t0, 2, .pushHostFunction
    bia t0, 9, .pushHostFunction
    loadq CalleeFrameAtEntry + ThisArgumentOffset[sp], t3
    move constexpr JSValue::NotCellMask, t1
    btqnz t3, t1, .pushHostFunction
    bbneq JSCell::m_type[t3], ArrayType, .pushHostFunction
    loadb JSCell::m_indexingTypeAndMisc[t3], t1
    andi constexpr IndexingModeMask, t1
    bineq t1, constexpr ArrayWithContiguous, .pushHostFunction
    loadp CalleeFrameAtEntry + Callee[sp], t2
    loadStructureWithScratch(t2, t1, t5)
    loadp Structure::m_realm[t1], t1
    loadp JSGlobalObject::m_vm[t1], t1
    loadi (constexpr (VM::offsetOfHeapBarrierThreshold()))[t1], t1
    bbbeq JSCell::m_cellState[t3], t1, .pushHostFunction
    loadp JSObjectWithButterfly::m_butterfly[t3], t5
    loadi -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t5], t1
    subi 1, t0
    move t1, t2
    addi t0, t2
    bia t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.vectorLength[t5], .pushHostFunction
    leap [t5, t1, 8], t1
    leap CalleeFrameAtEntry + FirstArgumentOffset[sp], t3
.pushLoop:
    loadd [t3], ft0
    stored ft0, [t1]
    addp 8, t3
    addp 8, t1
    bsubinz 1, t0, .pushLoop
    storei t2, -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t5]
    move constexpr JSValue::NumberTag, r0
    orq t2, r0
    ret

.pushHostFunction:
    jmp _llint_native_call_trampoline
end

if not C_LOOP
entry(llint_aot, macro ()
    macro setEntryAddresses(opcodeStruct, opcodeName)
        setEntryAddress(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%)
        setEntryAddressWide16(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%_wide16)
        setEntryAddressWide32(constexpr %opcodeStruct%::opcodeID, _llint_aot_%opcodeName%_wide32)
    end

    setEntryAddresses(OpAdd, op_add)
    if not ARM64E
        setEntryAddresses(OpCall, op_call)
        setEntryAddresses(OpCallIgnoreResult, op_call_ignore_result)
        setEntryAddresses(OpConstruct, op_construct)
        setEntryAddresses(OpTailCall, op_tail_call)
    end
    setEntryAddresses(OpCreateLexicalEnvironment, op_create_lexical_environment)
    setEntryAddresses(OpCreateThis, op_create_this)
    setEntryAddresses(OpCreateRest, op_create_rest)
    setEntryAddresses(OpGetByIdWithThis, op_get_by_id_with_this)
    setEntryAddresses(OpCreateClonedArguments, op_create_cloned_arguments)
    setEntryAddresses(OpEnumeratorGetByVal, op_enumerator_get_by_val)
    setEntryAddresses(OpEnumeratorNext, op_enumerator_next)
    setEntryAddresses(OpEnumeratorPutByVal, op_enumerator_put_by_val)
    setEntryAddresses(OpBitand, op_bitand)
    setEntryAddresses(OpDiv, op_div)
    if X86_64 or ARM64
        setEntryAddresses(OpMod, op_mod)
    end
    setEntryAddresses(OpMul, op_mul)
    setEntryAddresses(OpBitor, op_bitor)
    setEntryAddresses(OpBitxor, op_bitxor)
    setEntryAddresses(OpLshift, op_lshift)
    setEntryAddresses(OpRshift, op_rshift)
    setEntryAddresses(OpUnsigned, op_unsigned)
    setEntryAddresses(OpUrshift, op_urshift)
    setEntryAddresses(OpGetById, op_get_by_id)
    setEntryAddresses(OpGetByVal, op_get_by_val)
    setEntryAddresses(OpGetFromScope, op_get_from_scope)
    setEntryAddresses(OpGetLength, op_get_length)
    setEntryAddresses(OpGetPropertyEnumerator, op_get_property_enumerator)
    setEntryAddresses(OpInById, op_in_by_id)
    setEntryAddresses(OpInByVal, op_in_by_val)
    setEntryAddresses(OpInc, op_inc)
    setEntryAddresses(OpInstanceof, op_instanceof)
    setEntryAddresses(OpJfalse, op_jfalse)
    setEntryAddresses(OpJgreater, op_jgreater)
    setEntryAddresses(OpJgreatereq, op_jgreatereq)
    setEntryAddresses(OpJless, op_jless)
    setEntryAddresses(OpJlesseq, op_jlesseq)
    setEntryAddresses(OpJngreater, op_jngreater)
    setEntryAddresses(OpJngreatereq, op_jngreatereq)
    setEntryAddresses(OpJnless, op_jnless)
    setEntryAddresses(OpJnlesseq, op_jnlesseq)
    setEntryAddresses(OpJnstricteq, op_jnstricteq)
    setEntryAddresses(OpJstricteq, op_jstricteq)
    setEntryAddresses(OpJtrue, op_jtrue)
    setEntryAddresses(OpLoopHint, op_loop_hint)
    setEntryAddresses(OpMov, op_mov)
    setEntryAddresses(OpNewArray, op_new_array)
    setEntryAddresses(OpNewFunc, op_new_func)
    setEntryAddresses(OpNewFuncExp, op_new_func_exp)
    setEntryAddresses(OpNewObject, op_new_object)
    setEntryAddresses(OpNewPromise, op_new_promise)
    setEntryAddresses(OpNstricteq, op_nstricteq)
    setEntryAddresses(OpPutById, op_put_by_id)
    setEntryAddresses(OpPutByVal, op_put_by_val)
    setEntryAddresses(OpPutToScope, op_put_to_scope)
    setEntryAddresses(OpResolveScope, op_resolve_scope)
    setEntryAddresses(OpRet, op_ret)
    setEntryAddresses(OpStricteq, op_stricteq)
    setEntryAddresses(OpSub, op_sub)
    setEntryAddresses(OpSwitchString, op_switch_string)
    setEntryAddresses(OpStrcat, op_strcat)
    setEntryAddresses(OpToString, op_to_string)
    setEntryAddresses(OpToThis, op_to_this)
    setEntryAddresses(OpTypeof, op_typeof)
    setEntryAddresses(OpTypeofIsFunction, op_typeof_is_function)
    setEntryAddresses(OpTypeofIsObject, op_typeof_is_object)

    # VM::getCTIVirtualCall() reads the entry of the ordinary trampoline.
    setEntryAddress(constexpr llint_virtual_call_trampoline, _llint_aot_virtual_call_trampoline)

    if X86_64 or ARM64
        setEntryAddress(constexpr llint_function_for_call_prologue, _llint_aot_function_for_call_prologue)
        setEntryAddress(constexpr llint_function_for_construct_prologue, _llint_aot_function_for_construct_prologue)
        setEntryAddress(constexpr llint_function_for_call_arity_check, _llint_aot_function_for_call_arity_check)
        setEntryAddress(constexpr llint_function_for_construct_arity_check, _llint_aot_function_for_construct_arity_check)
    end
end)
end
