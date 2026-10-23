/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDerivedFunctions.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCalls.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OwnAllocation {
    Vector<ValueID, 2> prototypeWithCasts;
    ValueID allocation;
};

enum class ReturnedValues : uint8_t { Unknown, OnlyUndefined };

} // anonymous namespace

static ValueID newTargetParameterOf(const Function& function)
{
    std::optional<unsigned> index = function.newTargetParameterIndex();
    return index ? function.at(entryBlock).parameters[*index] : ValueID();
}

static bool belongsToOwnConstruction(const Function& function, const Value& value)
{
    ValueID newTarget = newTargetParameterOf(function);
    return newTarget
        && !value.inlineFrame
        && value.layout() == function.constructedClass
        && sameObjectThroughCasts(function, value.newTargetOperand()) == newTarget;
}

static bool isGuardUse(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operandKind(use.operandIndex) == OperandKind::Guard;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet castsWithoutTest { Opcode::Upcast, Opcode::AssumeType };

static bool canBeDefinedByName(const Field& field)
{
    return field.key && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

static ValueID findOwnPrototypeRead(const Function& function, const CFG& cfg)
{
    ValueID prototype;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::GetConstructPrototype || !belongsToOwnConstruction(function, value))
                continue;
            if (prototype || block != entryBlock)
                return { };
            prototype = id;
        }
    }
    return prototype;
}

static std::optional<OwnAllocation> findOwnAllocation(const Program& program, const Function& function)
{
    CFG cfg(function);
    ValueID prototype = findOwnPrototypeRead(function, cfg);
    if (!prototype)
        return std::nullopt;

    UseLists useLists(function, cfg);
    auto isOperandInEntryBlock = [](const std::optional<Use>& use) {
        return use && use->kind == Use::Kind::Operand && use->block == entryBlock;
    };
    Vector<ValueID, 2> prototypeWithCasts { prototype };
    std::optional<Use> prototypeUse = useLists.singleUse(prototype);
    while (isOperandInEntryBlock(prototypeUse) && castsWithoutTest.contains(function.at(prototypeUse->user).opcode)) {
        prototypeWithCasts.append(prototypeUse->user);
        prototypeUse = useLists.singleUse(prototypeUse->user);
    }
    if (!isOperandInEntryBlock(prototypeUse))
        return std::nullopt;

    ValueID id = prototypeUse->user;
    const Value& allocation = function.at(id);
    if (allocation.opcode != Opcode::NewObject || prototypeUse->operandIndex != OperandIndex::NewObject::prototype || declaredLayoutOf(program, allocation.layout()) != function.constructedClass)
        return std::nullopt;

    const Vector<Field>& fields = program.at(allocation.layout()).fields;
    bool canDefineInitialFields = std::ranges::all_of(allocation.fields(), [&](uint32_t field) {
        return field < fields.size() && canBeDefinedByName(fields[field]);
    });
    bool isGuard = std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
        return isGuardUse(function, use);
    });
    if (!canDefineInitialFields || isGuard)
        return std::nullopt;
    return OwnAllocation { WTF::move(prototypeWithCasts), id };
}

static Vector<ValueSite, 2> collectOwnSuperCalls(const Function& function)
{
    Vector<ValueSite, 2> calls;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (OperandPosition::parent(value.opcode) && belongsToOwnConstruction(function, value))
                calls.append(ValueSite { block, index, values[index] });
        }
    }
    return calls;
}

// A second super() constructs another object before binding this throws, so one object serves one call.
static bool runsAtMostOneCall(const Function& function, std::span<const ValueSite> calls)
{
    CFG cfg(function);
    BlockCycles cycles(cfg);
    for (const ValueSite& call : calls) {
        if (cycles.isInCycle(call.block))
            return false;
        BitVector reachableBlocks = cfg.blocksReachableFrom(call.block, { });
        for (const ValueSite& other : calls) {
            if (other.value != call.value && reachableBlocks.get(other.block.index()))
                return false;
        }
    }
    return true;
}

// The callees of a call name the version it enters: the function as read where every argument is proven, else its generic version.
static FunctionID findEnteredVersion(const Program& program, const CalleeSet& callees, FunctionID function)
{
    for (FunctionID callee : callees.functions) {
        const Function& version = program.at(callee);
        if (version.role == FunctionRole::GenericVersion && version.derivation.source == function)
            return callee;
    }
    return std::ranges::binary_search(callees.functions, function) ? function : FunctionID();
}

// Where it is not, the class object is expected to inherit from the class that the source names, and the run time clears the guard of the link where the class is defined with another parent.
static bool isBaseProven(const Layout& holder)
{
    return holder.flow.baseState != BaseState::Unknown;
}

static FunctionID findParentTakingThis(const Program& program, const Value& superCall)
{
    if (superCall.opcode != Opcode::SuperConstruct)
        return { };

    const Layout& holder = program.at(superCall.layout());
    LayoutID base = isBaseProven(holder) ? holder.flow.base : predictedHeritageOf(holder);
    FunctionID parent = base ? program.at(base).constructor : FunctionID();
    if (!parent)
        return { };

    // Flow stamps the super call of a class whose base it has proven. Where it has proven none there is no stamp, and the parent is entered as read.
    CalleeSetID callees = superCall.callees();
    if (!callees && isBaseProven(holder))
        return { };
    FunctionID entered = callees ? findEnteredVersion(program, program.at(callees), parent) : parent;
    if (!entered)
        return { };
    if (!program.at(parent).captures.isEmpty() || !canConstructDirectly(program, parent) || !canTakeAllocatedThis(program, entered))
        return { };
    return entered;
}

static bool canPassThisToSuper(const Program& program, const Function& function)
{
    Vector<ValueSite, 2> calls = collectOwnSuperCalls(function);
    if (calls.isEmpty() || !runsAtMostOneCall(function, calls.span()))
        return false;
    return std::ranges::all_of(calls, [&](const ValueSite& call) {
        return !!findParentTakingThis(program, function.at(call.value));
    });
}

bool canTakeAllocatedThis(const Program& program, FunctionID constructor)
{
    const Function& function = program.at(constructor);
    if (!function.hasBody() || !function.mayBeCloned() || !function.constructedClass || function.has(FunctionFlags::HasThis) || function.results.size() != 1)
        return false;
    switch (function.constructorForm()) {
    case ConstructorForm::NotConstructor:
        return false;
    case ConstructorForm::Base:
        return !!findOwnAllocation(program, function);
    case ConstructorForm::Derived:
        return canPassThisToSuper(program, function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LayoutID layoutAllocatedBy(const Program& program, FunctionID constructor)
{
    const Function& function = program.at(constructor);
    if (function.constructorForm() == ConstructorForm::Base) {
        if (std::optional<OwnAllocation> own = findOwnAllocation(program, function))
            return function.at(own->allocation).layout();
    }
    return function.constructedClass;
}

// Every caller hands over an object that it allocated with the layout of the class or of a class below it.
static DeclaredType computeThisType(const Program& program, const Function& original, TypeArena& types)
{
    TypeID replacedType = TypeTable::never;
    switch (original.constructorForm()) {
    case ConstructorForm::NotConstructor:
        RELEASE_ASSERT_NOT_REACHED();
    case ConstructorForm::Base: {
        std::optional<OwnAllocation> own = findOwnAllocation(program, original);
        RELEASE_ASSERT(own);
        replacedType = original.at(own->allocation).type;
        break;
    }
    case ConstructorForm::Derived:
        for (const ValueSite& call : collectOwnSuperCalls(original))
            replacedType = types.unionOf(replacedType, original.at(call.value).type);
        break;
    }

    if (program.at(replacedType).layouts)
        return { .type = replacedType };
    // The source need not say what a class extends: the base may be a value, and only flow finds that it is this class.
    Type declared = Type::forLayoutSet(program.declaredMembersOf(original.constructedClass));
    return { .type = types.intern(declared.unionWith(Type::forLayouts(program.layoutsBelow(original.constructedClass), types), types)) };
}

static ValueID addThisParameter(IRBuilder& builder)
{
    Function& version = builder.function();
    ValueID thisParameter = builder.addParameter(entryBlock, version.thisType.type);

    auto& parameters = version.edit(entryBlock, BlockEdit::Parameters).parameters;
    RELEASE_ASSERT(parameters.last() == thisParameter);
    parameters.removeLast();
    parameters.insert(*version.thisParameterIndex(), thisParameter);
    return thisParameter;
}

static void replaceAllocation(IRBuilder& builder, ValueID thisParameter)
{
    const Program& program = builder.context().program;
    Function& version = builder.function();
    std::optional<OwnAllocation> own = findOwnAllocation(program, version);
    RELEASE_ASSERT(own);

    const Value allocation = version.at(own->allocation);
    const Vector<Field>& fields = program.at(allocation.layout()).fields;
    ProgramPoint point { entryBlock, static_cast<uint32_t>(version.at(entryBlock).values.find(own->allocation)) };
    builder.setOrigin(allocation.origin());
    for (size_t index = 0; index < allocation.fields().size(); ++index) {
        Value define = IRBuilder::make(Opcode::PropDefine, { thisParameter, allocation.variadicOperands()[index] });
        define.setKey(fields[allocation.fields()[index]].key);
        builder.insertBefore(point, WTF::move(define));
    }
    builder.applyInsertions();

    ValueSubstitution substitution(version);
    substitution.replace(own->allocation, thisParameter);
    substitution.apply(version, nullptr);

    const BlockValues& values = version.at(entryBlock).values;
    BitVector erasedIndexes(values.size());
    for (size_t index = 0; index < values.size(); ++index) {
        if (own->prototypeWithCasts.contains(values[index]) || values[index] == own->allocation)
            erasedIndexes.set(index);
    }
    builder.eraseValues(entryBlock, erasedIndexes);
}

static ValueID appendParentVersionCall(IRBuilder& builder, DerivedFunctionInputs& inputs, const Value& superCall, FunctionID parent, ValueID thisParameter)
{
    FunctionID parentVersion = inputs.derivedFunctions.ensure(derivedFunctionKey(FunctionRole::AllocatedThisVersion, parent));
    ValueID calleeValue;
    if (inputs.program.at(parentVersion).has(FunctionFlags::TakesCallee))
        calleeValue = convertTo(builder, superCall.parentOperand(), DeclaredType { .type = TypeTable::function });

    return appendDirectCall(builder, {
        .callee = parentVersion,
        .thisValue = thisParameter,
        .calleeValue = calleeValue,
        .newTarget = superCall.newTargetOperand(),
        .arguments = superCall.variadicOperands(),
        .accessSite = superCall.accessSite(),
        .isConstruction = true,
    });
}

// Up to this call the constructor has done nothing that running it once more would show.
static bool canStartOverAt(const Program& program, const Function& version, const ValueSite& site)
{
    const Block& entry = version.at(entryBlock);
    if (site.block != entryBlock || entry.handler)
        return false;

    EffectSummaries noSummaries;
    EffectContext context { program, noSummaries };
    return std::ranges::all_of(entry.values.span().first(site.index), [&](ValueID id) {
        return effectsOf(context, version, version.at(id)).onAnyEdge().isUnobservable();
    });
}

// The object that was handed in goes unused, and the caller takes the one that is returned.
static void returnWhatSourceConstructs(IRBuilder& builder, ValueID thisParameter)
{
    const Function& version = builder.function();
    FunctionID source = version.derivation.source;
    Vector<ValueID, 8> arguments;
    for (ValueID parameter : version.at(entryBlock).parameters) {
        if (parameter != thisParameter)
            arguments.append(parameter);
    }

    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(source);
    call.setArgumentsOperands(arguments.span());
    call.setIsConstruction(true);
    builder.emitReturn(builder.append(WTF::move(call)));
}

static void passThisToSuperCall(IRBuilder& builder, DerivedFunctionInputs& inputs, const ValueSite& site, ValueID thisParameter)
{
    const Program& program = inputs.program;
    Function& version = builder.function();
    const Value original = version.at(site.value);
    FunctionID parent = findParentTakingThis(program, original);
    if (!parent)
        return;

    bool canStartOver = canStartOverAt(program, version, site);
    IRBuilder::Expansion expansion = builder.beginExpansion(site.point());
    Vector<BlockID, 3> continuationPredecessors;
    auto finishArm = [&](ValueID result) {
        ValueID converted = convertTo(builder, result, DeclaredType { .type = original.type });
        if (version.at(converted).type != TypeTable::never)
            continuationPredecessors.append(builder.currentBlock());
        builder.endExpansion(expansion, converted);
    };

    if (program.at(original.layout()).classFlow.staticLink != MemberMutability::Fixed || !isBaseProven(program.at(original.layout()))) {
        MemberCondition condition { .kind = MemberCondition::Kind::PrototypeLinkUnchanged, .layout = original.layout(), .side = ClassSide::Static };
        ValueID isUnchanged = builder.append(makeConditionsTest(inputs.types, singleElementSpan(condition)));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isUnchanged, 0, BlockFrequency::Normal, BlockFrequency::Rare);
        builder.moveToEndOf(branch.notTaken);
        // An arm that came back would make of `this` one of two objects for all the code that follows.
        if (canStartOver)
            returnWhatSourceConstructs(builder, thisParameter);
        else
            finishArm(builder.appendWithType(Value(original), original.type));
        builder.moveToEndOf(branch.taken);
    }

    ValueID returned = appendParentVersionCall(builder, inputs, original, parent, thisParameter);
    if (version.at(returned).type == TypeTable::never)
        builder.emitUnreachable();
    else {
        if (program.typeOf(version, returned).hasObjects()) {
            ValueID isObject = builder.appendTypeTest(returned, TypeTable::object);
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isObject, OperandIndex::TypeTest::value, BlockFrequency::Unlikely, BlockFrequency::Normal);
            finishArm(branch.provenOnTaken);
            builder.moveToEndOf(branch.notTaken);
        }
        finishArm(thisParameter);
    }

    if (continuationPredecessors.size() == 1)
        builder.mergeContinuation(expansion, continuationPredecessors[0]);
}

static void passThisToSuper(IRBuilder& builder, DerivedFunctionInputs& inputs, ValueID thisParameter)
{
    Vector<ValueSite, 2> calls = collectOwnSuperCalls(builder.function());
    for (size_t index = calls.size(); index--;)
        passThisToSuperCall(builder, inputs, calls[index], thisParameter);
    builder.setHandler(std::nullopt);
}

static ReturnedValues returnUndefinedForThis(IRBuilder& builder, ValueID thisParameter)
{
    Function& version = builder.function();
    CFG cfg(version);
    ValueIdentities identities(version, cfg);

    ReturnedValues returnedValues = ReturnedValues::OnlyUndefined;
    for (uint32_t blockIndex = 0; blockIndex < version.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        switch (version.at(block).terminator.kind) {
        case TerminatorKind::Return:
            break;
        case TerminatorKind::ReturnCall:
            returnedValues = ReturnedValues::Unknown;
            continue;
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            continue;
        }

        ValueID returned = version.at(block).terminator.valueOperand();
        if (!returned)
            continue;
        if (identities.identityOf(returned) != thisParameter) {
            returnedValues = ReturnedValues::Unknown;
            continue;
        }

        builder.setOrigin(version.at(block).terminator.origin());
        ValueID undefined = builder.insertBefore(pointBeforeTerminatorOf(version, block), IRBuilder::make(Opcode::Undefined));
        version.edit(block, BlockEdit::Terminator).terminator.setValueOperand(undefined);
    }
    builder.applyInsertions();
    return returnedValues;
}

Function buildAllocatedThisVersion(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Program& program = inputs.program;
    const Function& original = program.at(key.target);

    Function version = original;
    version.role = FunctionRole::AllocatedThisVersion;
    version.creator = CreatorRecord::Version { .provenVersion = key.target };
    version.flags.add(FunctionFlags::HasThis);
    version.capabilities = initialCapabilitiesOf(version);
    version.thisType = computeThisType(program, original, inputs.types);

    Vector<ParameterOrigin> parameterMap(original.numberOfEntryParameters(), [](size_t index) {
        return ParameterOrigin { static_cast<uint32_t>(index) };
    });
    parameterMap.insert(*version.thisParameterIndex(), ParameterOrigin { });
    version.derivation = Derivation { .source = key.target, .parameterMap = WTF::move(parameterMap) };

    IRBuilder builder(AnalysisContext { inputs.build, program, &inputs.flow, nullptr, key.target }, version, inputs.types);
    builder.setOrigin({ original.position, 0 });
    ValueID thisParameter = addThisParameter(builder);

    switch (original.constructorForm()) {
    case ConstructorForm::NotConstructor:
        RELEASE_ASSERT_NOT_REACHED();
    case ConstructorForm::Base:
        replaceAllocation(builder, thisParameter);
        break;
    case ConstructorForm::Derived:
        passThisToSuper(builder, inputs, thisParameter);
        break;
    }

    DeclaredType& result = version.results[0];
    switch (returnUndefinedForThis(builder, thisParameter)) {
    case ReturnedValues::Unknown:
        result.type = inputs.types.unionOf(result.type, TypeTable::undefined);
        break;
    case ReturnedValues::OnlyUndefined:
        result = DeclaredType { .type = TypeTable::undefined };
        break;
    }
    return version;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
