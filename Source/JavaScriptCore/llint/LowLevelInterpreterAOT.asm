# Copyright (C) 2026 Oven, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

# Handlers of bytecode in a process that runs ahead-of-time compiled code, where no JIT takes over from the interpreter.
# LLInt::initialize() puts them into the opcode maps when Options::useAOT() is set, in place of the handlers of the same opcodes, so no
# other process runs them.
#
# The Structure of a compiled object prohibits caching by offset. What its properties are is in the hashed table of
# AOT::VMPropertyCaches, under (StructureID, name, access). The slow paths fill the table. The cache of a get_by_id holds a field that
# lies in the cell of a compiled object: a boxed value under the offset that a property of an ordinary object at its place would have,
# a narrow reference under AOT::Interpreter::narrowReferenceInSiteCache and its distance from the start of the cell.
#
# Nobody reads a value profile here, so none is written.

const CompiledObjectType = constexpr CompiledObjectType
const NarrowReferenceInSiteCache = constexpr AOT::Interpreter::narrowReferenceInSiteCache
const ExtraPropertyInSiteCache = constexpr AOT::Interpreter::extraPropertyInSiteCache

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
const InstanceHasInstanceUID = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.hasInstanceUID))
const InstancePrototypeUID = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.prototypeUID))
const InstanceDefaultHasInstanceFunction = constexpr (AOT_INSTANCE_OFFSET(propertyCaches.defaultHasInstanceFunction))
const InstancePromiseAllocator = constexpr AOT::Interpreter::offsetOfPromiseAllocator
const InstancePromiseHeader = constexpr AOT::Interpreter::offsetOfPromiseHeader
const InstanceArrayAllocator = constexpr (AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator))
const InstanceNewArrayWithContiguousHeader = constexpr (AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader))
const InstanceRopeStringAllocator = constexpr (AOT_INSTANCE_PREFIX_OFFSET(ropeStringAllocator))
const InstanceStringHeader = constexpr (AOT_INSTANCE_PREFIX_OFFSET(stringHeader))
const InstanceClosureEntrypoint = constexpr (AOT_INSTANCE_OFFSET(stubs.closureEntrypoint))
const InstanceSlimClosureEntrypoint = constexpr (AOT_INSTANCE_OFFSET(stubs.slimClosureEntrypoint))

const ObjectLiteralPlanTag = constexpr AOT::Interpreter::ObjectLiteralPlan::allocatorTag
const ObjectLiteralPlanSlowPathTag = constexpr AOT::Interpreter::ObjectLiteralPlan::slowPathTag
const ObjectLiteralPlanAcceptsInt32 = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsInt32
const ObjectLiteralPlanAcceptsDouble = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsDouble
const ObjectLiteralPlanAcceptsString = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsString
const ObjectLiteralPlanAcceptsObject = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsObject
const ObjectLiteralPlanAcceptsBoolean = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsBoolean
const ObjectLiteralPlanAcceptsNull = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsNull
const ObjectLiteralPlanAcceptsUndefined = constexpr AOT::Interpreter::ObjectLiteralPlan::acceptsUndefined
const ObjectLiteralNotPlannedYet = constexpr AOT::Interpreter::ObjectLiteralPlan::notPlannedYet
const ObjectLiteralPlanAcceptedValues = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfAcceptedValues() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanAcceptedStructureIDs = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfAcceptedStructureIDs() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanFirstAcceptedCellTypes = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfFirstAcceptedCellTypes() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanLastAcceptedCellTypes = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfLastAcceptedCellTypes() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanInitialWords = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfInitialWords() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
const ObjectLiteralPlanNumberOfInitialWords = constexpr (AOT::Interpreter::ObjectLiteralPlan::offsetOfNumberOfInitialWords() - AOT::Interpreter::ObjectLiteralPlan::allocatorTag)
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

const RopeStringLength = constexpr (JSRopeString::offsetOfLength())
const MaxLengthOfStringsComparedInline = 32

const LocalAllocatorIntervalStart = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart())
const LocalAllocatorIntervalEnd = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd())
const LocalAllocatorNextInterval = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval())
const LocalAllocatorSecret = constexpr (LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret())
const LocalAllocatorCellSize = constexpr (LocalAllocator::offsetOfCellSize())
const FreeCellScrambledBits = constexpr (FreeCell::offsetOfScrambledBits())
const StructureTypeInfoBlob = constexpr (Structure::indexingModeIncludingHistoryOffset())

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
            loadi (constexpr %opcodeStruct%::opcodeID * 4 + MetadataOffsetTable32Offset)[metadataTable], dst
            getu(size, opcodeStruct, m_metadataID, scratch)
            muli sizeof %opcodeStruct%::Metadata, scratch
            addi scratch, dst
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

macro loadInstanceOfRealm(instance, hasNone)
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
# the JSValue it stands for, or names a literal string. The markers of a double are NaNs.
macro readField(instance, address, representation, result, isAbsent, cannot, done)
    bieq representation, constexpr AOT::SlotRepresentation::Cell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::JSValue, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::OptionalInt32, .optionalInt32
    bieq representation, constexpr AOT::SlotRepresentation::OptionalCell32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::OptionalDouble, .double
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Double, .double
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, .boolean
    bieq representation, constexpr AOT::SlotRepresentation::Int32, .int32
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString, .boxed
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
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString32, .narrowReference
    bieq representation, constexpr AOT::SlotRepresentation::Double, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Boolean, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Int32, isPresent
    bieq representation, constexpr AOT::SlotRepresentation::Cell, .boxed
    bieq representation, constexpr AOT::SlotRepresentation::LiteralString, .boxed
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
    loadInstanceOfRealm(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::ExtraProperty, .extraProperty
    bbeq t0, constexpr AOT::Slot::Kind::InheritedField, .inheritedField
    bbeq t0, constexpr AOT::Slot::Kind::FieldOrInherited, .field
    bbeq t0, constexpr AOT::Slot::Kind::FieldOrInheritedField, .field
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
    btbnz SlotFlags[t6], constexpr AOT::Interpreter::flagsOfInheritedValuesLeftToSlowPath, slow
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
    loadInstanceOfRealm(t5, slow)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, slow)
    loadb SlotKind[t6], t0
    bbeq t0, constexpr AOT::Slot::Kind::Value, isPresent
    bbeq t0, constexpr AOT::Slot::Kind::Field, .field
    bbeq t0, constexpr AOT::Slot::Kind::Absent, isAbsent
    move 1, t1
    lshifti t0, t1
    btinz t1, constexpr AOT::kindsOfPresentProperties, isPresent
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
    metadata(t2, t1)
    bbneq OpGetById::Metadata::m_modeMetadata.mode[t2], constexpr GetByIdMode::Default, .otherMode
    loadi JSCell::m_structureID[t3], t1
    bineq t1, OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], .otherStructure
    loadis OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset[t2], t1
    bigteq t1, firstOutOfLineOffset, .outOfLine
    loadq sizeof JSObjectWithButterfly[t3, t1, 8], t0
    # Only the slot of a field is ever empty.
    btqz t0, .compiledObject
    return(t0)

.outOfLine:
    bigteq t1, NarrowReferenceInSiteCache, .narrowReference
    bigteq t1, ExtraPropertyInSiteCache, .extraProperty
    loadp JSObjectWithButterfly::m_butterfly[t3], t0
    negi t1
    sxi2q t1, t1
    loadq (firstOutOfLineOffset - 2) * 8[t0, t1, 8], t0
    return(t0)

.extraProperty:
    loadh OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset + 2[t2], t0
    andi constexpr AOT::Interpreter::maxHiddenWordOffsetInSiteCache, t0
    loadi [t3, t0], t0
    btiz t0, .compiledObject
    loadInstanceOfRealm(t5, .slow)
    lshiftp constexpr AOT::cell32Shift, t0
    addp InstanceCellCageBase[t5], t0
    loadhsq OpGetById::Metadata::m_modeMetadata.defaultMode.cachedOffset[t2], t1
    loadPropertyAtWordIndex(t0, t1, t0)
    return(t0)

    # A word below AOT::minCell32 is an empty slot, a marker, a literal string or a closure that the object has yet to make.
.narrowReference:
    subi NarrowReferenceInSiteCache, t1
    loadi [t3, t1], t0
    bib t0, constexpr AOT::minCell32, .compiledObject
    loadInstanceOfRealm(t5, .slow)
    lshiftp constexpr AOT::cell32Shift, t0
    addp InstanceCellCageBase[t5], t0
    return(t0)

.otherMode:
    bbeq JSCell::m_type[t3], CompiledObjectType, .compiledObject
    loadb OpGetById::Metadata::m_modeMetadata.mode[t2], t1
    bbneq t1, constexpr GetByIdMode::ProtoLoad, .isNoMethod
    loadi JSCell::m_structureID[t3], t1
    bineq t1, OpGetById::Metadata::m_modeMetadata.protoLoadMode.structureID[t2], .probe
    loadis OpGetById::Metadata::m_modeMetadata.protoLoadMode.cachedOffset[t2], t1
    loadp OpGetById::Metadata::m_modeMetadata.protoLoadMode.cachedSlot[t2], t0
    bigteq t1, firstOutOfLineOffset, .methodIsOutOfLine
    loadq sizeof JSObjectWithButterfly[t0, t1, 8], t0
    return(t0)

.methodIsOutOfLine:
    loadp JSObjectWithButterfly::m_butterfly[t0], t0
    negi t1
    sxi2q t1, t1
    loadq (firstOutOfLineOffset - 2) * 8[t0, t1, 8], t0
    return(t0)

.isNoMethod:
    bbneq t1, constexpr GetByIdMode::ArrayLength, .isAbsent
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    btiz t0, IsArray, .probe
    btiz t0, IndexingShapeMask, .probe
    loadp JSObjectWithButterfly::m_butterfly[t3], t0
    loadi -sizeof IndexingHeader + IndexingHeader::u.lengths.publicLength[t0], t0
    bilt t0, 0, .probe
    orq numberTag, t0
    return(t0)

.isAbsent:
    loadi JSCell::m_structureID[t3], t1
    bineq t1, OpGetById::Metadata::m_modeMetadata.unsetMode.structureID[t2], .probe
    return(ValueUndefined)

.otherStructure:
    bbneq JSCell::m_type[t3], CompiledObjectType, .probe
    # The slow path sees whether the cache of a site that holds nothing yet can hold the field, and leaves the mode of a site that cannot.
    btiz OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], .slow

.compiledObject:
    loadIdentifier(size, OpGetById, m_property, t1, t0)
    getFromCompiledObject(.slow, .done)

    # What the cache of the site does not hold.
.probe:
    loadi OpGetById::Metadata::m_modeMetadata.defaultMode.structureID[t2], t7
    bbb JSCell::m_type[t3], ObjectType, .slow
    loadInstanceOfRealm(t5, .slow)
    loadIdentifier(size, OpGetById, m_property, t1, t0)
    loadi JSCell::m_structureID[t3], t2
    findHashedSlot(t5, t2, t1, makeKeyOfRead, t6, t0, .slow)
    loadb SlotKind[t6], t0
    # Compiled code may have filed the property before the site ran for the first time. Only the slow path fills the cache of the site.
    btinz t7, .readsHashedSlot
    bbneq t0, constexpr AOT::Slot::Kind::Value, .readsHashedSlot
    btbnz SlotFlags[t6], constexpr AOT::Slot::isOwn, .slow
.readsHashedSlot:
    getFromHashedSlot(t6, .otherKind, .callsCustomGetter, .done)

    # As in getFromCompiledObject(): the holder is asked as for a field of its own.
.otherKind:
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

.slow:
    callSlowPath(_llint_slow_path_get_by_id)
    dispatch()

.baseIsConstant:
    loadConstantOperand(size, t0, t3, .hasBase)
end)

aotOp(op_get_by_val, OpGetByVal, macro (size, get, dispatch, metadata, return)
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
    loadConstantOrVariableCell(size, t0, t2, .slow)
    loadNameOfKey(t2, t1, .slow)
    testPresenceInObject(.slow, .true, .false)

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
    loadInstanceOfRealm(t5, .slow)
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
        btiz JSPropertyNameEnumerator::m_endStructurePropertyIndex[t1], .slow
        get(m_base, t2)
        loadConstantOrVariableCell(size, t2, t0, .slow)
        bbeq JSCell::m_type[t0], CompiledObjectType, .slow
        loadi JSPropertyNameEnumerator::m_cachedStructureID[t1], t2
        bineq t2, JSCell::m_structureID[t0], .slow
        loadp JSPropertyNameEnumerator::m_propertyNames[t1], t3
        loadp [t3], t3
        btpz t3, .slow
        storeVariable(get, m_propertyName, t3, t2)
        move constexpr JSPropertyNameEnumerator::OwnStructureMode, t3
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

# Stores to a field that is in its slot. AOT::RuntimeLayoutTable::slotAdmits() and AOT::tryWriteSlot(), for the types and representations
# that a test of the value's tag decides, and of an int32 the range of the type.
aotOp(op_put_by_id, OpPutById, macro (size, get, dispatch, metadata, return)
    get(m_base, t1)
    loadOperandInRegister(size, t1, t0, .ordinary)
    btqnz t0, notCellMask, .ordinary

    # The cache of the site, as the ordinary handler reads it: it never holds the Structure of a compiled object. One write barrier stands behind both stores of a transition.
    metadata(t5, t2)
    loadi OpPutById::Metadata::m_oldStructureID[t5], t2
    bineq t2, JSCell::m_structureID[t0], .otherStructure
    loadi OpPutById::Metadata::m_newStructureID[t5], t1
    btiz t1, .replacesValue
    loadp OpPutById::Metadata::m_structureChain[t5], t3
    btpz t3, .addsProperty
    structureIDToStructureWithScratch(t2, t1)
    loadp StructureChain::m_vector[t3], t3
    loadq Structure::m_prototype[t2], t2
    bqeq t2, ValueNull, .addsProperty
.nextPrototype:
    loadi JSCell::m_structureID[t2], t2
    bineq t2, [t3], .ordinary
    addp 4, t3
    # An empty entry ends the chain, and an object that still has its Structure still has its prototype.
    btiz [t3], .addsProperty
    structureIDToStructureWithScratch(t2, t1)
    loadq Structure::m_prototype[t2], t2
    jmp .nextPrototype

.addsProperty:
    get(m_value, t1)
    loadOperandInRegister(size, t1, t3, .addedValueIsConstant)
.hasAddedValue:
    loadi OpPutById::Metadata::m_newStructureID[t5], t2
    loadi OpPutById::Metadata::m_offset[t5], t1
    storei t2, JSCell::m_structureID[t0]
    move t0, t6
    storePropertyAtVariableOffset(t1, t6, t3)
    writeBarrierOnCell(t0, t1)
    dispatch()

.replacesValue:
    get(m_value, t1)
    loadOperandInRegister(size, t1, t3, .newValueIsConstant)
.hasNewValue:
    loadi OpPutById::Metadata::m_offset[t5], t1
    move t0, t6
    storePropertyAtVariableOffset(t1, t6, t3)
    btqnz t3, notCellMask, .stored
    btqz t3, .stored
    writeBarrierOnCell(t0, t1)
.stored:
    dispatch()

.otherStructure:
    bbeq JSCell::m_type[t0], CompiledObjectType, .compiledObject
.ordinary:
    jumpToOrdinaryHandler(size, op_put_by_id)

.addedValueIsConstant:
    loadConstantOperand(size, t1, t3, .hasAddedValue)

.newValueIsConstant:
    loadConstantOperand(size, t1, t3, .hasNewValue)

.compiledObject:
    getu(size, OpPutById, m_flags, t1)
    btinz t1, constexpr PutByIdFlagsFits::s_isDirectBit, .slow
    loadInstanceOfRealm(t5, .slow)
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
# Each of these representations has a marker for either, and holds it whatever the type of the field says. The mark of a cell may answer for the raw value of a field
# whose type lists neither, so such a field takes a marker here only in a cell that has the mark. AOT::Object::store() sets it.
.undefined:
    btinz t5, constexpr AOT::Typed::Primitive::Undefined, .storeUndefinedOrNull
    jmp .markerOutsideType
.null:
    btinz t5, constexpr AOT::Typed::Primitive::Null, .storeUndefinedOrNull
.markerOutsideType:
    btbz JSCell::m_flags[t0], constexpr TypeInfoPerCellBit, .slow
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
    loadInstanceOfRealm(t5, .slow)
    get(m_symbolTable, t0)
    loadConstantOrVariable(size, t0, t1)
    bpneq (constexpr AOT::Interpreter::PlacesInSymbolTable::offsetOfSingleton)[t1], constexpr AOT::Interpreter::PlacesInSymbolTable::invalidatedSingleton, .slow
    loadi (constexpr AOT::Interpreter::PlacesInSymbolTable::offsetOfMaxScopeOffset)[t1], t2
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
    loadp InstanceGlobalObject[t5], t0
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
    loadConstantOrVariable(size, t2, r0)
    doReturn()
end)

# AOT::generateNewPromise().
aotOp(op_new_promise, OpNewPromise, macro (size, get, dispatch, metadata, return)
    loadInstanceOfRealm(t5, .slow)
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
    metadata(t5, t0)
    loadi OpResolveScope::Metadata::m_resolveType[t5], t1
    move t1, t0
    andi constexpr AOT::Interpreter::FusedClosureVariableRead::typeMask, t0
    bineq t0, ClosureVar, .ordinary
    loadi OpResolveScope::Metadata::m_localScopeDepth[t5], t2
    get(m_scope, t0)
    loadq [cfr, t0, 8], t0
    btiz t2, .hasScope
.nextScope:
    loadp JSScope::m_next[t0], t0
    subi 1, t2
    btinz t2, .nextScope
.hasScope:
    get(m_dst, t2)
    storeq t0, [cfr, t2, 8]
    urshifti constexpr AOT::Interpreter::FusedClosureVariableRead::typeBits, t1
    move t1, t3
    andi constexpr AOT::Interpreter::FusedClosureVariableRead::bytesToSkipMask, t3
    btiz t3, .isDone
    urshifti constexpr AOT::Interpreter::FusedClosureVariableRead::bytesToSkipBits, t1
    loadq JSLexicalEnvironment_variables[t0, t1, 8], t0
    btqz t0, .isDone
    addp (constexpr op_resolve_scope_length) + OpcodeIDNarrowSize, PC
    getOperandNarrow(OpGetFromScope, m_dst, t2)
    storeq t0, [cfr, t2, 8]
    addp t3, PC
    nextInstruction()

.isDone:
    dispatch()

.ordinary:
    jumpToOrdinaryHandler(size, op_resolve_scope)
end)

# JSFinalObject::create().
aotOp(op_new_object, OpNewObject, macro (size, get, dispatch, metadata, return)
    metadata(t5, t0)
    loadp OpNewObject::Metadata::m_objectAllocationProfile.m_allocator[t5], t1
    btpnz t1, ObjectLiteralPlanTag, .hasPlan
    btpz t1, .slow
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
    btpnz t1, ObjectLiteralPlanSlowPathTag, .testsValues
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

# ObjectBuilder::tryBuild(), where every slot holds its value as it is.
.testsValues:
    bpeq t1, ObjectLiteralNotPlannedYet, .slow
    subp ObjectLiteralPlanSlowPathTag, t1
    loadp ObjectLiteralPlanAllocator[t1], t2
    allocateCell(t2, t0, t5, .slow)
    loadq ObjectLiteralPlanCellHeader[t1], t2
    storeq t2, [t0]
    loadp ObjectLiteralPlanInitialWords[t1], t3
    loadi ObjectLiteralPlanNumberOfInitialWords[t1], t2
.copyInitialWord:
    subi 1, t2
    loadq [t3, t2, 8], t6
    storeq t6, JSObjectWithButterfly::m_butterfly[t0, t2, 8]
    btinz t2, .copyInitialWord
    loadp ObjectLiteralPlanAcceptedStructureIDs[t1], t3
    storei PC, CallSiteIndex[cfr]
    get(m_dst, t5)
    size(macro ()
        addp (constexpr op_new_object_length) + OpcodeIDNarrowSize, PC
    end, macro ()
        addp (constexpr op_new_object_length) * 2 + OpcodeIDWide16SizeJS, PC
    end, macro ()
        addp (constexpr op_new_object_length) * 4 + OpcodeIDWide32SizeJS, PC
    end, macro (advance) advance() end)
.testValue:
    loadb [PB, PC, 1], t6
    bieq t6, constexpr op_wide16, .testedStoreIsWide16
    getOperandNarrow(OpPutById, m_value, t6)
    addp (constexpr op_put_by_id_length) + OpcodeIDNarrowSize, PC
    loadOperandInRegister(narrow, t6, t7, .testedValueIsConstant)
.hasTestedValue:
    bqaeq t7, numberTag, .valueIsInt32
    btqz t7, notCellMask, .valueIsCell
    btqnz t7, numberTag, .valueIsDouble
    move t7, t6
    andq ~1, t6
    bqneq t6, ValueFalse, .valueIsUndefinedOrNull
    move ObjectLiteralPlanAcceptsBoolean, t6
.testKind:
    btbz ObjectLiteralPlanAcceptedValues[t1, t2, 1], t6, .givesUp
.isAccepted:
    loadh ObjectLiteralPlanValueOffsets[t1, t2, 2], t6
    storeq t7, [t0, t6, 1]
    addi 1, t2
    bib t2, ObjectLiteralPlanNumberOfValues[t1], .testValue
    fenceAfterAllocation(t3)
    storeq t0, [cfr, t5, 8]
    nextInstruction()

.valueIsInt32:
    move ObjectLiteralPlanAcceptsInt32, t6
    jmp .testKind

.valueIsDouble:
    move ObjectLiteralPlanAcceptsDouble, t6
    jmp .testKind

.valueIsUndefinedOrNull:
    move ObjectLiteralPlanAcceptsUndefined, t6
    bqeq t7, ValueUndefined, .testKind
    bqneq t7, ValueNull, .givesUp
    move ObjectLiteralPlanAcceptsNull, t6
    jmp .testKind

.valueIsCell:
    move ObjectLiteralPlanAcceptsString, t6
    bbeq JSCell::m_type[t7], StringType, .testKindOfCell
    bbb JSCell::m_type[t7], ObjectType, .givesUp
    move ObjectLiteralPlanAcceptsObject, t6
.testKindOfCell:
    btbnz ObjectLiteralPlanAcceptedValues[t1, t2, 1], t6, .isAccepted
    loadi JSCell::m_structureID[t7], t6
    bieq t6, [t3, t2, 4], .isAccepted
    loadb JSCell::m_type[t7], t6
    bbb t6, ObjectLiteralPlanFirstAcceptedCellTypes[t1, t2, 1], .givesUp
    bbbeq t6, ObjectLiteralPlanLastAcceptedCellTypes[t1, t2, 1], .isAccepted

.givesUp:
    loadi CallSiteIndex[cfr], PC
    jmp .slow

.testedValueIsConstant:
    loadConstantOperand(narrow, t6, t7, .hasTestedValue)

.testedStoreIsWide16:
    getOperandWide16JS(OpPutById, m_value, t6)
    loadConstantOrVariable(wide16, t6, t7)
    addp (constexpr op_put_by_id_length) * 2 + OpcodeIDWide16SizeJS, PC
    jmp .hasTestedValue
end)

# constructArrayNegativeIndexed(), where storing the elements leaves the indexing type of the profile as it is.
aotOp(op_new_array, OpNewArray, macro (size, get, dispatch, metadata, return)
    loadInstanceOfRealm(t5, .slow)
    # AOT::branchIfHavingABadTime().
    btiz InstanceNewArrayWithContiguousHeader[t5], .slow
    metadata(t6, t0)

    # ArrayAllocationProfile::selectIndexingType().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t3
    move t3, t2
    urshiftq constexpr AOT::shiftOfIndexingTypeInArrayAllocationProfile, t2
    lshiftq constexpr AOT::bitsOfTypeInArrayAllocationProfile, t3
    urshiftq constexpr AOT::bitsOfTypeInArrayAllocationProfile, t3
    btqz t3, .typeIsCurrent
    loadb JSCell::m_indexingTypeAndMisc[t3], t0
    andi constexpr AllWritableArrayTypes, t0
    bineq t0, t2, .slow
.typeIsCurrent:

    # Butterfly::optimalContiguousVectorLength(): the storage has a size class of its own up to MarkedSpace::preciseCutoff.
    getu(size, OpNewArray, m_argc, t1)
    btinz t1, .hasElements
    bia t2, constexpr ArrayWithContiguous, .slow
    move constexpr BASE_CONTIGUOUS_VECTOR_LEN_EMPTY, t3
    jmp .elementsFit
.hasElements:
    bia t1, constexpr AOT::maxLengthOfArrayLiteralAllocatedInline, .slow
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
    loadp InstanceGlobalObject[t5], t0
    urshifti constexpr IndexingShapeShift, t2
    loadi JSGlobalObject::m_arrayStructureForIndexingShapeDuringAllocation[t0, t2, 4], t2
    storei t2, JSCell::m_structureID[t1]
    structureIDToStructureWithScratch(t2, t0)
    loadi StructureTypeInfoBlob[t2], t0
    storei t0, JSCell::m_indexingTypeAndMisc[t1]
    storep t7, JSObjectWithButterfly::m_butterfly[t1]
    fenceAfterAllocation(t0)

    # ArrayAllocationProfile::updateLastAllocation().
    loadq OpNewArray::Metadata::m_arrayAllocationProfile[t6], t0
    urshiftq constexpr AOT::bitsOfPointerInArrayAllocationProfile, t0
    lshiftq constexpr AOT::bitsOfPointerInArrayAllocationProfile, t0
    orq t1, t0
    storeq t0, OpNewArray::Metadata::m_arrayAllocationProfile[t6]
    return(t1)

.slow:
    callSlowPath(_llint_slow_path_aot_new_array)
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
        callSlowPath(_llint_slow_path_aot_%opcodeName%)
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
    loadConstantOrVariable(size, t3, t1)
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

# jsString(JSGlobalObject*, JSString*, JSString*) of two strings of which neither is empty: JSRopeString::create(). Two int32s are added
# here as well, so that they do not pay for the test. Nothing reads the arithmetic profile where there is no JIT.
aotOp(op_add, OpAdd, macro (size, get, dispatch, metadata, return)
    get(m_lhs, t2)
    get(m_rhs, t3)
    loadConstantOrVariable(size, t2, t0)
    loadConstantOrVariable(size, t3, t1)
    bqb t0, numberTag, .leftIsNoInt32
    bqb t1, numberTag, .ordinary
    baddio t1, t0, .ordinary
    orq numberTag, t0
    return(t0)

.leftIsNoInt32:
    btqnz t0, notCellMask, .ordinary
    btqnz t1, notCellMask, .ordinary
    bbneq JSCell::m_type[t0], StringType, .ordinary
    bbneq JSCell::m_type[t1], StringType, .ordinary

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
    loadInstanceOfRealm(t5, .ordinary)
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

.ordinary:
    jumpToOrdinaryHandler(size, op_add)
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
aotOp(op_call, OpCall, macro (size, get, dispatch, metadata, return)
    metadata(t5, t0)

    macro getCallee(dst)
        get(m_callee, dst)
    end

    macro getArgumentStart(dst)
        getu(size, OpCall, m_argv, dst)
    end

    macro getArgumentCount(dst)
        getu(size, OpCall, m_argc, dst)
    end

    callHelper(aot_op_call, OpCall, dispatchAfterCallWithoutProfile, m_valueProfile, m_dst, prepareForRegularCall, invokeCallWithoutProfile, prepareForSlowRegularCall, prepareCallSiteForConstruct, size, dispatch, metadata, getCallee, getArgumentStart, getArgumentCount)
end)
end

macro loadInstanceOfCompiledCallee(callee, instance, scratch)
    loadStructureWithScratch(callee, instance, scratch)
    loadp Structure::m_realm[instance], instance
    loadp JSGlobalObject::m_aotInstance[instance], instance
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
    end
    setEntryAddresses(OpCreateLexicalEnvironment, op_create_lexical_environment)
    setEntryAddresses(OpEnumeratorGetByVal, op_enumerator_get_by_val)
    setEntryAddresses(OpEnumeratorNext, op_enumerator_next)
    setEntryAddresses(OpGetById, op_get_by_id)
    setEntryAddresses(OpGetByVal, op_get_by_val)
    setEntryAddresses(OpGetLength, op_get_length)
    setEntryAddresses(OpInById, op_in_by_id)
    setEntryAddresses(OpInByVal, op_in_by_val)
    setEntryAddresses(OpInstanceof, op_instanceof)
    setEntryAddresses(OpJfalse, op_jfalse)
    setEntryAddresses(OpJnstricteq, op_jnstricteq)
    setEntryAddresses(OpJstricteq, op_jstricteq)
    setEntryAddresses(OpJtrue, op_jtrue)
    setEntryAddresses(OpMov, op_mov)
    setEntryAddresses(OpNewArray, op_new_array)
    setEntryAddresses(OpNewObject, op_new_object)
    setEntryAddresses(OpNewPromise, op_new_promise)
    setEntryAddresses(OpNstricteq, op_nstricteq)
    setEntryAddresses(OpPutById, op_put_by_id)
    setEntryAddresses(OpResolveScope, op_resolve_scope)
    setEntryAddresses(OpRet, op_ret)
    setEntryAddresses(OpStricteq, op_stricteq)
    setEntryAddresses(OpToThis, op_to_this)
    setEntryAddresses(OpTypeofIsFunction, op_typeof_is_function)
    setEntryAddresses(OpTypeofIsObject, op_typeof_is_object)

    # VM::getCTIVirtualCall() reads the entry of the ordinary trampoline.
    setEntryAddress(constexpr llint_virtual_call_trampoline, _llint_aot_virtual_call_trampoline)
end)
end
